#include "vr_model.h"
#include <math.h>
#include <string.h>

#define CGLTF_IMPLEMENTATION
#include "../thirdparty/cgltf/cgltf.h"

#define VR_MODEL_AMBIENT 0.35f
#define VR_MODEL_MAX_BLOCK ( (size_t)256 << 20 )

typedef struct {
	vrModelAlloc_t alloc;
	vrModelFree_t release;
} vrModelMemory_t;

static void *VRM_Alloc( void *user, cgltf_size size ) {
	return ( (vrModelMemory_t *)user )->alloc( size );
}
static void VRM_Release( void *user, void *memory ) {
	if ( memory )
		( (vrModelMemory_t *)user )->release( memory );
}
/* Every buffer and image has to be inside the file itself. */
static cgltf_result VRM_NoFile( const struct cgltf_memory_options *memory, const struct cgltf_file_options *file,
								const char *path, cgltf_size *size, void **data ) {
	(void)memory;
	(void)file;
	(void)path;
	(void)size;
	(void)data;
	return cgltf_result_file_not_found;
}

/* One pass sizes the block, a second fills it: base is NULL while counting. */
typedef struct {
	unsigned char *base;
	size_t used;
	int overflow;
} vrModelArena_t;

static void *VRM_Take( vrModelArena_t *arena, size_t size ) {
	void *memory = arena->base ? arena->base + arena->used : NULL;
	if ( size > VR_MODEL_MAX_BLOCK - arena->used ) {
		arena->overflow = 1;
		return NULL;
	}
	arena->used += ( size + 7 ) & ~(size_t)7;
	return memory;
}

static const cgltf_accessor *VRM_Attribute( const cgltf_primitive *primitive, cgltf_attribute_type type, int index ) {
	cgltf_size i;
	for ( i = 0; i < primitive->attributes_count; i++ )
		if ( primitive->attributes[i].type == type && primitive->attributes[i].index == index )
			return primitive->attributes[i].data;
	return NULL;
}

static int VRM_Drawable( const cgltf_primitive *primitive ) {
	const cgltf_accessor *position = VRM_Attribute( primitive, cgltf_attribute_type_position, 0 );
	return primitive->type == cgltf_primitive_type_triangles && !primitive->has_draco_mesh_compression && position &&
		   position->type == cgltf_type_vec3 && position->count >= 3;
}

static int VRM_PrimitiveTriangles( const cgltf_primitive *primitive ) {
	const cgltf_size count = primitive->indices ? primitive->indices->count
												: VRM_Attribute( primitive, cgltf_attribute_type_position, 0 )->count;
	return count > 0x1000000 ? 0x1000000 / 3 : (int)( count / 3 );
}

static const cgltf_image *VRM_BaseImage( const cgltf_material *material ) {
	const cgltf_texture *texture =
		material && material->has_pbr_metallic_roughness ? material->pbr_metallic_roughness.base_color_texture.texture : NULL;
	if ( !texture )
		return NULL;
	/* a KHR_texture_basisu texture carries its image in the extension, not in source */
	return texture->image ? texture->image : texture->basisu_image;
}

/* The encoded bytes of an image held in a buffer view or a base64 data URI; 0 when it is neither PNG nor JPEG. */
#ifndef VR_MODEL_KTX2
#define VR_MODEL_KTX2 0
#endif

static int VRM_ImageFormat( const unsigned char *source, size_t size ) {
	static const unsigned char png[4] = {0x89, 'P', 'N', 'G'};
	static const unsigned char ktx2[12] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n'};
	if ( size >= 4 && !memcmp( source, png, 4 ) )
		return VR_MODEL_IMAGE_PNG;
	if ( size >= 2 && source[0] == 0xFF && source[1] == 0xD8 )
		return VR_MODEL_IMAGE_JPEG;
	if ( VR_MODEL_KTX2 && size >= 48 && !memcmp( source, ktx2, 12 ) )
		return VR_MODEL_IMAGE_KTX2;
	return -1;
}

static size_t VRM_ImageBytes( const cgltf_image *image, const cgltf_options *options, unsigned char *out, int *format ) {
	const unsigned char *source = NULL;
	void *decoded = NULL;
	size_t size = 0;
	if ( image->buffer_view && image->buffer_view->buffer && image->buffer_view->buffer->data ) {
		source = (const unsigned char *)image->buffer_view->buffer->data + image->buffer_view->offset;
		size = image->buffer_view->size;
	} else if ( image->uri && !strncmp( image->uri, "data:", 5 ) ) {
		const char *comma = strchr( image->uri, ',' );
		size_t length = comma ? strlen( comma + 1 ) : 0;
		if ( !comma || comma - image->uri < 7 || strncmp( comma - 7, ";base64", 7 ) )
			return 0;
		while ( length && comma[length] == '=' )
			length--;
		size = length * 3 / 4;
		if ( size < 4 || cgltf_load_buffer_base64( options, size, comma + 1, &decoded ) != cgltf_result_success )
			return 0;
		source = decoded;
	}
	if ( size < 4 || size > 0x7fffffff || VRM_ImageFormat( source, size ) < 0 )
		size = 0;
	else {
		*format = VRM_ImageFormat( source, size );
		if ( out )
			memcpy( out, source, size );
	}
	if ( decoded )
		options->memory.free_func( options->memory.user_data, decoded );
	return size;
}

static int VRM_SameImage( const cgltf_image *a, const cgltf_image *b ) {
	const cgltf_buffer_view *u = a->buffer_view, *v = b->buffer_view;
	if ( u && v )
		return u->buffer && v->buffer && u->buffer->data && v->buffer->data && u->size == v->size &&
			   !memcmp( (const char *)u->buffer->data + u->offset, (const char *)v->buffer->data + v->offset, u->size );
	return !u && !v && a->uri && b->uri && !strcmp( a->uri, b->uri );
}

/* Area-weighted vertex normals for a primitive that carries none. */
static void VRM_DeriveNormals( vrModelPrimitive_t *p ) {
	int i, k;
	memset( p->normals, 0, (size_t)p->vertexCount * 3 * sizeof( float ) );
	for ( i = 0; i + 2 < p->indexCount; i += 3 ) {
		const float *a = p->positions + p->indices[i] * 3, *b = p->positions + p->indices[i + 1] * 3,
					*c = p->positions + p->indices[i + 2] * 3;
		float u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, v[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
		float n[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
		for ( k = 0; k < 3; k++ ) {
			float *out = p->normals + p->indices[i + k] * 3;
			out[0] += n[0];
			out[1] += n[1];
			out[2] += n[2];
		}
	}
}

/* The node's transform as a pose: its TRS without the scale, or the same taken back out of its matrix. */
static void VRM_RestPose( const cgltf_node *node, vrModelPose_t *pose ) {
	float m[16], len[3], t;
	int i, k;
	if ( !node->has_matrix ) {
		memcpy( pose->position, node->translation, sizeof( pose->position ) );
		memcpy( pose->orientation, node->rotation, sizeof( pose->orientation ) );
		return;
	}
	memcpy( m, node->matrix, sizeof( m ) );
	for ( k = 0; k < 3; k++ ) {
		pose->position[k] = m[12 + k];
		len[k] = sqrtf( m[k * 4] * m[k * 4] + m[k * 4 + 1] * m[k * 4 + 1] + m[k * 4 + 2] * m[k * 4 + 2] );
		for ( i = 0; i < 3 && len[k] > 0; i++ )
			m[k * 4 + i] /= len[k];
	}
	/* a mirrored basis flips one axis back so the quaternion below is a rotation */
	if ( m[0] * ( m[5] * m[10] - m[6] * m[9] ) - m[4] * ( m[1] * m[10] - m[2] * m[9] ) + m[8] * ( m[1] * m[6] - m[2] * m[5] ) < 0 )
		for ( i = 0; i < 3; i++ )
			m[8 + i] = -m[8 + i];
	t = m[0] + m[5] + m[10];
	if ( t > 0 ) {
		const float r = sqrtf( 1 + t ), w = 0.5f / r;
		pose->orientation[0] = ( m[6] - m[9] ) * w;
		pose->orientation[1] = ( m[8] - m[2] ) * w;
		pose->orientation[2] = ( m[1] - m[4] ) * w;
		pose->orientation[3] = 0.5f * r;
	} else {
		const int a = m[0] >= m[5] && m[0] >= m[10] ? 0 : m[5] >= m[10] ? 1 : 2, b = ( a + 1 ) % 3, c = ( a + 2 ) % 3;
		const float r = sqrtf( 1 + m[a * 5] - m[b * 5] - m[c * 5] ), w = 0.5f / r;
		pose->orientation[a] = 0.5f * r;
		pose->orientation[b] = ( m[a * 4 + b] + m[b * 4 + a] ) * w;
		pose->orientation[c] = ( m[a * 4 + c] + m[c * 4 + a] ) * w;
		pose->orientation[3] = ( m[b * 4 + c] - m[c * 4 + b] ) * w;
	}
}

/* The first animation's channel of one path for a node, when it holds the keys the path needs; NULL otherwise. */
static const cgltf_accessor *VRM_Channel( const cgltf_data *data, const cgltf_node *node, cgltf_animation_path_type path ) {
	const cgltf_animation *animation = data->animations_count ? &data->animations[0] : NULL;
	const cgltf_type type = path == cgltf_animation_path_type_rotation ? cgltf_type_vec4 : cgltf_type_vec3;
	cgltf_size i;
	for ( i = 0; animation && i < animation->channels_count; i++ ) {
		const cgltf_animation_channel *channel = &animation->channels[i];
		const cgltf_accessor *output = channel->sampler ? channel->sampler->output : NULL;
		if ( channel->target_node == node && channel->target_path == path && output && output->type == type &&
			 output->count > 0 && output->count <= 0x10000 )
			return output;
	}
	return NULL;
}

/* Builds the model into the arena. Returns 0 when the asset is over a limit or, while filling, inconsistent. */
static int VRM_Build( const cgltf_data *data, const cgltf_options *options, int *nodeIndex, const cgltf_node **queue,
					  int *imageIndex, size_t *imageSize, vrModelArena_t *arena, vrModel_t *model ) {
	const cgltf_scene *scene = data->scene ? data->scene : data->scenes_count ? &data->scenes[0] : NULL;
	const int fill = arena->base != NULL;
	int nodes = 0, head, primitives = 0, triangles = 0;
	cgltf_size i, j;
	if ( !scene )
		return 0;
	/* Breadth-first from the scene's roots, so a parent always precedes its children. */
	for ( i = 0; i < data->nodes_count; i++ )
		nodeIndex[i] = -1;
	for ( i = 0; i < scene->nodes_count; i++ ) {
		const cgltf_size n = (cgltf_size)( scene->nodes[i] - data->nodes );
		if ( nodeIndex[n] < 0 ) {
			nodeIndex[n] = nodes;
			queue[nodes++] = scene->nodes[i];
		}
	}
	for ( head = 0; head < nodes; head++ )
		for ( i = 0; i < queue[head]->children_count; i++ ) {
			const cgltf_size n = (cgltf_size)( queue[head]->children[i] - data->nodes );
			if ( nodeIndex[n] < 0 ) {
				nodeIndex[n] = nodes;
				queue[nodes++] = queue[head]->children[i];
			}
		}
	if ( !nodes || nodes > VR_MODEL_MAX_NODES )
		return 0;
	/* a mesh draws once for every node that carries it, so that is what the budget counts */
	for ( head = 0; head < nodes; head++ )
		for ( j = 0; queue[head]->mesh && j < queue[head]->mesh->primitives_count; j++ )
			if ( VRM_Drawable( &queue[head]->mesh->primitives[j] ) ) {
				triangles += VRM_PrimitiveTriangles( &queue[head]->mesh->primitives[j] );
				if ( triangles > VR_MODEL_MAX_TRIANGLES )
					return 0;
			}
	model->nodeCount = nodes;
	model->nodes = VRM_Take( arena, (size_t)nodes * sizeof( vrModelNode_t ) );
	model->meshCount = (int)data->meshes_count;
	model->meshes = VRM_Take( arena, data->meshes_count * sizeof( vrModelMesh_t ) );
	for ( i = 0; i < data->meshes_count; i++ )
		for ( j = 0; j < data->meshes[i].primitives_count; j++ )
			primitives += VRM_Drawable( &data->meshes[i].primitives[j] );
	model->primitiveCount = primitives;
	model->primitives = VRM_Take( arena, (size_t)primitives * sizeof( vrModelPrimitive_t ) );
	model->materialCount = (int)data->materials_count + 1;
	model->materials = VRM_Take( arena, ( data->materials_count + 1 ) * sizeof( vrModelMaterial_t ) );
	model->images = VRM_Take( arena, data->images_count * sizeof( vrModelImage_t ) );
	model->imageCount = 0;
	model->skinCount = (int)data->skins_count;
	model->skins = VRM_Take( arena, data->skins_count * sizeof( vrModelSkin_t ) );

	for ( head = 0; head < nodes; head++ ) {
		const cgltf_node *source = queue[head];
		const cgltf_accessor *positions = VRM_Channel( data, source, cgltf_animation_path_type_translation );
		const cgltf_accessor *orientations = VRM_Channel( data, source, cgltf_animation_path_type_rotation );
		vrModelNode_t local, *node = fill ? &model->nodes[head] : &local;
		memset( node, 0, sizeof( *node ) );
		node->positionKeys = positions ? (int)positions->count : 0;
		node->orientationKeys = orientations ? (int)orientations->count : 0;
		node->positions = VRM_Take( arena, (size_t)node->positionKeys * 3 * sizeof( float ) );
		node->orientations = VRM_Take( arena, (size_t)node->orientationKeys * 4 * sizeof( float ) );
		if ( !fill )
			continue;
		node->parent = source->parent ? nodeIndex[source->parent - data->nodes] : -1;
		node->mesh = source->mesh ? (int)( source->mesh - data->meshes ) : -1;
		node->skin = source->skin ? (int)( source->skin - data->skins ) : -1;
		cgltf_node_transform_local( source, node->local );
		VRM_RestPose( source, &node->rest );
		if ( source->name )
			strncpy( node->name, source->name, VR_MODEL_NAME_SIZE - 1 );
		if ( ( positions && cgltf_accessor_unpack_floats( positions, node->positions, (cgltf_size)node->positionKeys * 3 ) !=
								(cgltf_size)node->positionKeys * 3 ) ||
			 ( orientations && cgltf_accessor_unpack_floats( orientations, node->orientations,
															  (cgltf_size)node->orientationKeys * 4 ) != (cgltf_size)node->orientationKeys * 4 ) )
			return 0;
	}

	/* Every joint has to be a node of the scene; a skin without bind matrices binds at the identity. */
	for ( i = 0; i < data->skins_count; i++ ) {
		const cgltf_skin *source = &data->skins[i];
		vrModelSkin_t local, *skin = fill ? &model->skins[i] : &local;
		int k;
		if ( !source->joints_count || source->joints_count > VR_MODEL_MAX_JOINTS )
			return 0;
		if ( source->inverse_bind_matrices && ( source->inverse_bind_matrices->type != cgltf_type_mat4 ||
												source->inverse_bind_matrices->count < source->joints_count ) )
			return 0;
		skin->jointCount = (int)source->joints_count;
		skin->joints = VRM_Take( arena, source->joints_count * sizeof( int ) );
		skin->inverseBind = VRM_Take( arena, source->joints_count * 16 * sizeof( float ) );
		if ( !fill )
			continue;
		for ( k = 0; k < skin->jointCount; k++ ) {
			skin->joints[k] = nodeIndex[source->joints[k] - data->nodes];
			if ( skin->joints[k] < 0 )
				return 0;
			if ( !source->inverse_bind_matrices ) {
				memset( skin->inverseBind + k * 16, 0, 16 * sizeof( float ) );
				skin->inverseBind[k * 16] = skin->inverseBind[k * 16 + 5] = skin->inverseBind[k * 16 + 10] = skin->inverseBind[k * 16 + 15] = 1;
			} else if ( !cgltf_accessor_read_float( source->inverse_bind_matrices, (cgltf_size)k, skin->inverseBind + k * 16, 16 ) )
				return 0;
		}
	}

	primitives = 0;
	for ( i = 0; i < data->meshes_count; i++ ) {
		if ( fill ) {
			model->meshes[i].firstPrimitive = primitives;
			model->meshes[i].primitiveCount = 0;
		}
		for ( j = 0; j < data->meshes[i].primitives_count; j++ ) {
			const cgltf_primitive *source = &data->meshes[i].primitives[j];
			const cgltf_accessor *position, *normal, *texCoord, *joints, *weights;
			vrModelPrimitive_t local, *p = fill ? &model->primitives[primitives] : &local;
			int k;
			if ( !VRM_Drawable( source ) )
				continue;
			position = VRM_Attribute( source, cgltf_attribute_type_position, 0 );
			normal = VRM_Attribute( source, cgltf_attribute_type_normal, 0 );
			joints = VRM_Attribute( source, cgltf_attribute_type_joints, 0 );
			weights = VRM_Attribute( source, cgltf_attribute_type_weights, 0 );
			/* skinning takes both, four a vertex */
			if ( !joints || !weights || joints->type != cgltf_type_vec4 || weights->type != cgltf_type_vec4 ||
				 joints->count != position->count || weights->count != position->count ||
				 ( joints->component_type != cgltf_component_type_r_8u && joints->component_type != cgltf_component_type_r_16u ) )
				joints = weights = NULL;
			texCoord = VRM_Attribute( source, cgltf_attribute_type_texcoord,
									  source->material && source->material->has_pbr_metallic_roughness
										  ? source->material->pbr_metallic_roughness.base_color_texture.texcoord
										  : 0 );
			if ( normal && ( normal->type != cgltf_type_vec3 || normal->count != position->count ) )
				normal = NULL;
			if ( texCoord && ( texCoord->type != cgltf_type_vec2 || texCoord->count != position->count ) )
				texCoord = NULL;
			if ( position->count > 0x1000000 || ( source->indices && source->indices->count > 0x1000000 ) )
				return 0;
			p->vertexCount = (int)position->count;
			p->indexCount = (int)( source->indices ? source->indices->count : position->count );
			p->indexCount -= p->indexCount % 3;
			p->material = source->material ? (int)( source->material - data->materials ) : (int)data->materials_count;
			p->positions = VRM_Take( arena, (size_t)p->vertexCount * 3 * sizeof( float ) );
			p->normals = VRM_Take( arena, (size_t)p->vertexCount * 3 * sizeof( float ) );
			p->texCoords = VRM_Take( arena, (size_t)p->vertexCount * 2 * sizeof( float ) );
			p->indices = VRM_Take( arena, (size_t)p->indexCount * sizeof( unsigned ) );
			p->joints = joints ? VRM_Take( arena, (size_t)p->vertexCount * 4 * sizeof( unsigned short ) ) : NULL;
			p->weights = weights ? VRM_Take( arena, (size_t)p->vertexCount * 4 * sizeof( float ) ) : NULL;
			primitives++;
			if ( !fill )
				continue;
			for ( k = 0; joints && k < p->vertexCount; k++ ) {
				cgltf_uint j[4];
				int c;
				if ( !cgltf_accessor_read_uint( joints, (cgltf_size)k, j, 4 ) )
					return 0;
				for ( c = 0; c < 4; c++ )
					p->joints[k * 4 + c] = (unsigned short)( j[c] > 0xffff ? 0xffff : j[c] );
			}
			if ( weights && cgltf_accessor_unpack_floats( weights, p->weights, (cgltf_size)p->vertexCount * 4 ) !=
								(cgltf_size)p->vertexCount * 4 )
				return 0;
			model->meshes[i].primitiveCount++;
			if ( cgltf_accessor_unpack_floats( position, p->positions, (cgltf_size)p->vertexCount * 3 ) !=
				 (cgltf_size)p->vertexCount * 3 )
				return 0;
			if ( !texCoord || cgltf_accessor_unpack_floats( texCoord, p->texCoords, (cgltf_size)p->vertexCount * 2 ) !=
								  (cgltf_size)p->vertexCount * 2 )
				memset( p->texCoords, 0, (size_t)p->vertexCount * 2 * sizeof( float ) );
			if ( source->indices && cgltf_accessor_unpack_indices( source->indices, p->indices, sizeof( unsigned ),
																   (cgltf_size)p->indexCount ) != (cgltf_size)p->indexCount )
				return 0;
			for ( k = 0; k < p->indexCount; k++ ) {
				if ( !source->indices )
					p->indices[k] = (unsigned)k;
				else if ( p->indices[k] >= (unsigned)p->vertexCount )
					return 0;
			}
			if ( !normal || cgltf_accessor_unpack_floats( normal, p->normals, (cgltf_size)p->vertexCount * 3 ) !=
								(cgltf_size)p->vertexCount * 3 )
				VRM_DeriveNormals( p );
		}
	}
	model->triangleCount = triangles;

	/* a skinned part's joints have to be in the skin of the node that draws it */
	for ( head = 0; head < nodes && fill; head++ ) {
		const vrModelNode_t *node = &model->nodes[head];
		const vrModelMesh_t *mesh = node->mesh >= 0 ? &model->meshes[node->mesh] : NULL;
		int p, k;
		if ( !mesh || node->skin < 0 )
			continue;
		for ( p = mesh->firstPrimitive; p < mesh->firstPrimitive + mesh->primitiveCount; p++ )
			for ( k = 0; model->primitives[p].joints && k < model->primitives[p].vertexCount * 4; k++ )
				if ( model->primitives[p].joints[k] >= model->skins[node->skin].jointCount )
					return 0;
	}

	/* only an image some base color uses is kept, once however many materials share it */
	for ( i = 0; i < data->images_count; i++ )
		imageIndex[i] = -1;

	for ( i = 0; i <= data->materials_count; i++ ) {
		const cgltf_material *source = i < data->materials_count ? &data->materials[i] : NULL;
		const cgltf_image *image = VRM_BaseImage( source );
		vrModelMaterial_t local, *m = fill ? &model->materials[i] : &local;
		int k, format = 0;
		for ( k = 0; k < 4; k++ )
			m->color[k] = source && source->has_pbr_metallic_roughness ? source->pbr_metallic_roughness.base_color_factor[k] : 1;
		m->doubleSided = source && source->double_sided;
		m->image = -1;
		if ( image ) {
			const cgltf_size g = (cgltf_size)( image - data->images );
			cgltf_size h;
			/* a runtime may give every part its own copy of one texture */
			for ( h = 0; h < data->images_count && imageIndex[g] == -1; h++ )
				if ( imageIndex[h] >= 0 && VRM_SameImage( image, &data->images[h] ) )
					imageIndex[g] = imageIndex[h];
			if ( imageIndex[g] == -1 ) {
				unsigned char *bytes;
				/* the sizing pass measures; the fill pass has to find the same bytes */
				if ( !fill )
					imageSize[g] = VRM_ImageBytes( image, options, NULL, &format );
				bytes = VRM_Take( arena, imageSize[g] );
				imageIndex[g] = imageSize[g] ? model->imageCount++ : -2;
				if ( fill && imageSize[g] ) {
					vrModelImage_t *out = &model->images[imageIndex[g]];
					if ( VRM_ImageBytes( image, options, bytes, &format ) != imageSize[g] )
						return 0;
					out->data = bytes;
					out->size = (int)imageSize[g];
					out->format = format;
					out->pixels = NULL;
					out->width = out->height = 0;
				}
			}
			if ( imageIndex[g] >= 0 )
				m->image = imageIndex[g];
			else
				/* the color lives in a texture of a kind this parser does not decode (Meta's are Basis Universal), so
				 * a matte mid-gray stands in for it rather than the material's white base */
				for ( k = 0; k < 3; k++ )
					m->color[k] *= 0.62f;
		}
	}
	return !arena->overflow;
}

int VR_ModelParse( const void *glb, size_t size, vrModelAlloc_t alloc, vrModelFree_t release, vrModel_t *model ) {
	vrModelMemory_t memory;
	cgltf_options options;
	cgltf_data *data = NULL;
	vrModelArena_t arena;
	int *nodeIndex = NULL, *imageIndex = NULL;
	size_t *imageSize = NULL;
	const cgltf_node **queue = NULL;
	cgltf_size i;
	int ok = 0;
	if ( !model )
		return 0;
	memset( model, 0, sizeof( *model ) );
	if ( !glb || size < 12 || !alloc || !release )
		return 0;
	memory.alloc = alloc;
	memory.release = release;
	memset( &options, 0, sizeof( options ) );
	options.type = cgltf_file_type_glb;
	options.memory.alloc_func = VRM_Alloc;
	options.memory.free_func = VRM_Release;
	options.memory.user_data = &memory;
	options.file.read = VRM_NoFile;
	if ( cgltf_parse( &options, glb, size, &data ) != cgltf_result_success )
		return 0;
	for ( i = 0; i < data->buffers_count; i++ )
		if ( data->buffers[i].uri && strncmp( data->buffers[i].uri, "data:", 5 ) )
			goto done;
	if ( cgltf_load_buffers( &options, data, "" ) != cgltf_result_success || cgltf_validate( data ) != cgltf_result_success )
		goto done;
	if ( !data->nodes_count || data->nodes_count > 0x100000 || data->images_count > 0x10000 )
		goto done;
	nodeIndex = alloc( data->nodes_count * sizeof( int ) );
	queue = alloc( data->nodes_count * sizeof( *queue ) );
	imageIndex = alloc( ( data->images_count + 1 ) * sizeof( int ) );
	imageSize = alloc( ( data->images_count + 1 ) * sizeof( size_t ) );
	if ( !nodeIndex || !queue || !imageIndex || !imageSize )
		goto done;
	memset( &arena, 0, sizeof( arena ) );
	if ( !VRM_Build( data, &options, nodeIndex, queue, imageIndex, imageSize, &arena, model ) || !arena.used )
		goto done;
	arena.base = alloc( arena.used );
	if ( !arena.base )
		goto done;
	arena.used = 0;
	memset( model, 0, sizeof( *model ) );
	model->block = arena.base;
	ok = VRM_Build( data, &options, nodeIndex, queue, imageIndex, imageSize, &arena, model );
done:
	if ( nodeIndex )
		release( nodeIndex );
	if ( queue )
		release( (void *)queue );
	if ( imageIndex )
		release( imageIndex );
	if ( imageSize )
		release( imageSize );
	cgltf_free( data );
	if ( !ok ) {
		if ( model->block )
			release( model->block );
		memset( model, 0, sizeof( *model ) );
	}
	return ok;
}

void VR_ModelFree( vrModel_t *model, vrModelFree_t release ) {
	int i;
	if ( !model )
		return;
	for ( i = 0; i < model->imageCount && release; i++ )
		if ( model->images[i].pixels )
			release( model->images[i].pixels );
	if ( model->block && release )
		release( model->block );
	memset( model, 0, sizeof( *model ) );
}

int VR_ModelImageSize( const vrModelImage_t *image, int *width, int *height ) {
	const unsigned char *d = image->data;
	int at = 2;
	if ( !d )
		return 0;
	if ( image->format == VR_MODEL_IMAGE_KTX2 ) {
		/* the identifier, then vkFormat, typeSize, pixelWidth, pixelHeight, little-endian */
		if ( image->size < 48 )
			return 0;
		*width = (int)( (unsigned)d[20] | d[21] << 8 | d[22] << 16 | (unsigned)d[23] << 24 );
		*height = (int)( (unsigned)d[24] | d[25] << 8 | d[26] << 16 | (unsigned)d[27] << 24 );
		return 1;
	}
	if ( image->format == VR_MODEL_IMAGE_PNG ) {
		/* the signature, then the header chunk: length, "IHDR", width, height */
		if ( image->size < 24 || memcmp( d, "\x89PNG\r\n\x1a\n", 8 ) || memcmp( d + 12, "IHDR", 4 ) )
			return 0;
		*width = (int)( (unsigned)d[16] << 24 | d[17] << 16 | d[18] << 8 | d[19] );
		*height = (int)( (unsigned)d[20] << 24 | d[21] << 16 | d[22] << 8 | d[23] );
		return 1;
	}
	if ( image->size < 4 || d[0] != 0xff || d[1] != 0xd8 )
		return 0;
	/* segments up to the first frame header, which carries the size; the scan follows it */
	while ( at + 4 <= image->size && d[at] == 0xff ) {
		const int marker = d[at + 1];
		if ( marker == 0xff ) {
			at++;
			continue;
		}
		if ( marker == 0xda || marker == 0xd9 )
			return 0;
		if ( marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc ) {
			if ( at + 9 > image->size )
				return 0;
			*height = d[at + 5] << 8 | d[at + 6];
			*width = d[at + 7] << 8 | d[at + 8];
			return 1;
		}
		at += 2 + ( d[at + 2] << 8 | d[at + 3] );
	}
	return 0;
}

int VR_ModelKeyframe( const vrModel_t *model, int node, int index, vrModelPose_t *pose ) {
	const vrModelNode_t *n;
	if ( node < 0 || node >= model->nodeCount )
		return 0;
	n = &model->nodes[node];
	if ( index < 0 || ( index >= n->positionKeys && index >= n->orientationKeys ) )
		return 0;
	*pose = n->rest;
	if ( index < n->positionKeys )
		memcpy( pose->position, n->positions + index * 3, sizeof( pose->position ) );
	if ( index < n->orientationKeys )
		memcpy( pose->orientation, n->orientations + index * 4, sizeof( pose->orientation ) );
	return 1;
}

void VR_ModelBindNodes( const vrModel_t *model, const char ( *names )[VR_MODEL_NAME_SIZE], int count, int *map ) {
	int i, n;
	for ( i = 0; i < count; i++ ) {
		map[i] = -1;
		for ( n = 0; n < model->nodeCount; n++ )
			if ( !strncmp( model->nodes[n].name, names[i], VR_MODEL_NAME_SIZE ) ) {
				map[i] = n;
				break;
			}
	}
}

static void VRM_PoseMatrix( const vrModelPose_t *pose, float m[16] ) {
	float x = pose->orientation[0], y = pose->orientation[1], z = pose->orientation[2], w = pose->orientation[3];
	float n = x * x + y * y + z * z + w * w;
	if ( !( n > 0.000001f ) ) {
		x = y = z = 0;
		w = n = 1;
	}
	n = 2 / n;
	m[0] = 1 - n * ( y * y + z * z );
	m[1] = n * ( x * y + w * z );
	m[2] = n * ( x * z - w * y );
	m[3] = 0;
	m[4] = n * ( x * y - w * z );
	m[5] = 1 - n * ( x * x + z * z );
	m[6] = n * ( y * z + w * x );
	m[7] = 0;
	m[8] = n * ( x * z + w * y );
	m[9] = n * ( y * z - w * x );
	m[10] = 1 - n * ( x * x + y * y );
	m[11] = 0;
	m[12] = pose->position[0];
	m[13] = pose->position[1];
	m[14] = pose->position[2];
	m[15] = 1;
}

/* out = a * b; out may not alias either. */
static void VRM_Multiply( const float *a, const float *b, float *out ) {
	int column, row;
	for ( column = 0; column < 4; column++ )
		for ( row = 0; row < 4; row++ )
			out[column * 4 + row] = a[row] * b[column * 4] + a[4 + row] * b[column * 4 + 1] +
									a[8 + row] * b[column * 4 + 2] + a[12 + row] * b[column * 4 + 3];
}

void VR_ModelPose( const vrModel_t *model, const vrModelPose_t *root, const vrModelNodeState_t *states,
				   const int *map, int count, float *world, unsigned char *visible ) {
	float base[16], local[16];
	int i;
	VRM_PoseMatrix( root, base );
	/* bit 0: visible; bit 1: the node's slot in world holds a replacement transform */
	memset( visible, 1, (size_t)model->nodeCount );
	for ( i = 0; states && i < count; i++ )
		if ( map[i] >= 0 && map[i] < model->nodeCount ) {
			VRM_PoseMatrix( &states[i].pose, world + map[i] * 16 );
			visible[map[i]] = states[i].visible ? 3 : 2;
		}
	for ( i = 0; i < model->nodeCount; i++ ) {
		const vrModelNode_t *node = &model->nodes[i];
		const float *parent = node->parent >= 0 ? world + node->parent * 16 : base;
		memcpy( local, visible[i] & 2 ? world + i * 16 : node->local, sizeof( local ) );
		VRM_Multiply( parent, local, world + i * 16 );
		visible[i] = ( visible[i] & 1 ) && ( node->parent < 0 || visible[node->parent] );
	}
}

size_t VR_ModelPack( const vrModel_t *model, void *out, unsigned *offsets ) {
	size_t size = 0;
	int p, i;
	for ( p = 0; p < model->primitiveCount; p++ ) {
		const vrModelPrimitive_t *primitive = &model->primitives[p];
		if ( offsets )
			offsets[p] = (unsigned)size;
		if ( out ) {
			float *xyz = (float *)( (char *)out + size );
			for ( i = 0; i < primitive->vertexCount; i++ ) {
				memcpy( xyz + i * 4, primitive->positions + i * 3, 3 * sizeof( float ) );
				xyz[i * 4 + 3] = 1;
			}
			memcpy( (char *)out + size + VR_MODEL_PACK_ST( primitive->vertexCount ), primitive->texCoords,
					(size_t)primitive->vertexCount * 2 * sizeof( float ) );
			memcpy( (char *)out + size + VR_MODEL_PACK_INDEX( primitive->vertexCount ), primitive->indices,
					(size_t)primitive->indexCount * sizeof( unsigned ) );
		}
		size += VR_MODEL_PACK_INDEX( primitive->vertexCount ) + (size_t)primitive->indexCount * sizeof( unsigned );
	}
	return size;
}

/* One vertex's headlight color from its normal and the vector to the eye, both in one space; lit is the
 * material's color scaled to bytes. toEye NULL means no eye to light from, so the ambient floor. */
static void VRM_Light( const vrModelMaterial_t *material, const float lit[3], const float *n, const float *toEye,
					   unsigned char *rgba ) {
	const float nn = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
	const float vv = toEye ? toEye[0] * toEye[0] + toEye[1] * toEye[1] + toEye[2] * toEye[2] : 0;
	float facing = nn > 0 && vv > 0 ? ( n[0] * toEye[0] + n[1] * toEye[1] + n[2] * toEye[2] ) / sqrtf( nn * vv ) : 0, light;
	int k;
	if ( facing < 0 )
		facing = material->doubleSided ? -facing : 0;
	light = VR_MODEL_AMBIENT + ( 1 - VR_MODEL_AMBIENT ) * facing;
	for ( k = 0; k < 3; k++ ) {
		const float c = lit[k] * light + 0.5f;
		rgba[k] = (unsigned char)( c > 255 ? 255 : c );
	}
	rgba[3] = 255;
}

void VR_ModelShade( const vrModel_t *model, int primitive, const float world[16], const float eye[3],
					unsigned char *rgba ) {
	const vrModelPrimitive_t *p = &model->primitives[primitive];
	const vrModelMaterial_t *material = &model->materials[p->material];
	const float *m = world;
	/* the eye in the node's own space, so nothing per vertex needs the matrix */
	const float c0 = m[5] * m[10] - m[6] * m[9], c1 = m[6] * m[8] - m[4] * m[10], c2 = m[4] * m[9] - m[5] * m[8];
	const float det = m[0] * c0 + m[1] * c1 + m[2] * c2;
	const float d[3] = {eye[0] - m[12], eye[1] - m[13], eye[2] - m[14]};
	const int invertible = det > 1e-12f || det < -1e-12f;
	float local[3] = {0, 0, 0}, lit[3];
	int i, k;
	if ( invertible ) {
		const float r = 1 / det;
		local[0] = r * ( c0 * d[0] + c1 * d[1] + c2 * d[2] );
		local[1] = r * ( ( m[2] * m[9] - m[1] * m[10] ) * d[0] + ( m[0] * m[10] - m[2] * m[8] ) * d[1] +
						 ( m[1] * m[8] - m[0] * m[9] ) * d[2] );
		local[2] = r * ( ( m[1] * m[6] - m[2] * m[5] ) * d[0] + ( m[2] * m[4] - m[0] * m[6] ) * d[1] +
						 ( m[0] * m[5] - m[1] * m[4] ) * d[2] );
	}
	for ( k = 0; k < 3; k++ )
		lit[k] = material->color[k] * 255;
	for ( i = 0; i < p->vertexCount; i++, rgba += 4 ) {
		const float *q = p->positions + i * 3;
		const float v[3] = {local[0] - q[0], local[1] - q[1], local[2] - q[2]};
		VRM_Light( material, lit, p->normals + i * 3, invertible ? v : NULL, rgba );
	}
}

/* The matrix each joint of a skin carries its bound vertices by: its world matrix after its inverse bind.
 * Returns the joint count, 0 when there is no such skin. */
static int VRM_SkinMatrices( const vrModel_t *model, int skin, const float *world, float *matrices ) {
	const vrModelSkin_t *s;
	int j;
	if ( skin < 0 || skin >= model->skinCount )
		return 0;
	s = &model->skins[skin];
	for ( j = 0; j < s->jointCount; j++ )
		VRM_Multiply( world + s->joints[j] * 16, s->inverseBind + j * 16, matrices + j * 16 );
	return s->jointCount;
}

/* One vertex under its weighted joint matrices: position with w 1, and its normal made unit again. */
static void VRM_SkinVertex( const vrModelPrimitive_t *p, const float *matrices, int jointCount, int i, float position[4],
							float normal[3] ) {
	const float *v = p->positions + i * 3, *n = p->normals + i * 3;
	float m[16], len;
	int k, c;
	memset( m, 0, sizeof( m ) );
	for ( k = 0; k < 4; k++ ) {
		const float w = p->weights[i * 4 + k];
		const int j = p->joints[i * 4 + k];
		if ( w != 0 && j < jointCount )
			for ( c = 0; c < 16; c++ )
				m[c] += w * matrices[j * 16 + c];
	}
	for ( c = 0; c < 3; c++ ) {
		position[c] = m[c] * v[0] + m[4 + c] * v[1] + m[8 + c] * v[2] + m[12 + c];
		normal[c] = m[c] * n[0] + m[4 + c] * n[1] + m[8 + c] * n[2];
	}
	position[3] = 1;
	len = sqrtf( normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2] );
	for ( c = 0; c < 3 && len > 0; c++ )
		normal[c] /= len;
}

void VR_ModelSkin( const vrModel_t *model, int primitive, int skin, const float *world, float *positions, float *normals ) {
	const vrModelPrimitive_t *p = &model->primitives[primitive];
	float matrices[VR_MODEL_MAX_JOINTS * 16], position[4], normal[3];
	const int joints = p->joints ? VRM_SkinMatrices( model, skin, world, matrices ) : 0;
	int i;
	for ( i = 0; i < p->vertexCount; i++ ) {
		if ( joints )
			VRM_SkinVertex( p, matrices, joints, i, position, normal );
		else {
			memcpy( position, p->positions + i * 3, 3 * sizeof( float ) );
			position[3] = 1;
			memcpy( normal, p->normals + i * 3, sizeof( normal ) );
		}
		if ( positions )
			memcpy( positions + i * 4, position, sizeof( position ) );
		if ( normals )
			memcpy( normals + i * 3, normal, sizeof( normal ) );
	}
}

void VR_ModelSkinShade( const vrModel_t *model, int primitive, int skin, const float *world, const float eye[3],
						float *positions, unsigned char *rgba ) {
	const vrModelPrimitive_t *p = &model->primitives[primitive];
	const vrModelMaterial_t *material = &model->materials[p->material];
	float matrices[VR_MODEL_MAX_JOINTS * 16], normal[3], lit[3];
	const int joints = p->joints ? VRM_SkinMatrices( model, skin, world, matrices ) : 0;
	int i, k;
	for ( k = 0; k < 3; k++ )
		lit[k] = material->color[k] * 255;
	for ( i = 0; i < p->vertexCount; i++, positions += 4, rgba += 4 ) {
		float toEye[3];
		if ( joints )
			VRM_SkinVertex( p, matrices, joints, i, positions, normal );
		else {
			memcpy( positions, p->positions + i * 3, 3 * sizeof( float ) );
			positions[3] = 1;
			memcpy( normal, p->normals + i * 3, sizeof( normal ) );
		}
		for ( k = 0; k < 3; k++ )
			toEye[k] = eye[k] - positions[k];
		VRM_Light( material, lit, normal, toEye, rgba );
	}
}
