#include "vk_foveation.h"
#include <string.h>
#include <stdlib.h>
VkResult VK_FovQuery( vkFovCaps_t *caps, VkInstance instance, VkPhysicalDevice physical,
					  PFN_vkGetInstanceProcAddr proc, uint32_t api ) {
	PFN_vkEnumerateDeviceExtensionProperties extensions;
	PFN_vkGetPhysicalDeviceFeatures2 features;
	PFN_vkGetPhysicalDeviceProperties2 properties;
	PFN_vkGetPhysicalDeviceFormatProperties formats;
	PFN_vkGetPhysicalDeviceFragmentShadingRatesKHR rates;
	VkExtensionProperties *ext;
	VkPhysicalDeviceFeatures2 f;
	VkPhysicalDeviceProperties2 p;
	VkPhysicalDeviceFragmentShadingRatePropertiesKHR fp;
	VkFormatProperties format;
	VkPhysicalDeviceFragmentShadingRateKHR list[VK_FOV_MAX_RATES];
	uint32_t count = 0, i;
	int hasRate = 0, hasPass = api >= VK_API_VERSION_1_2;
	VkResult result;
	if ( !caps )
		return VK_ERROR_INITIALIZATION_FAILED;
	memset( caps, 0, sizeof( *caps ) );
#define UNAVAILABLE(why) \
	do { \
		caps->reason = why; \
		return VK_SUCCESS; \
	} while ( 0 )
	if ( !instance || !physical || !proc )
		UNAVAILABLE( "missing Vulkan instance, physical device, or loader dispatch" );
	if ( api < VK_API_VERSION_1_1 )
		UNAVAILABLE( "negotiated Vulkan API below 1.1" );
#define LOAD(var, name) \
	var = (PFN_##name)proc( instance, #name ); \
	if ( !var ) \
	UNAVAILABLE( "missing " #name )
	LOAD( extensions, vkEnumerateDeviceExtensionProperties );
	LOAD( features, vkGetPhysicalDeviceFeatures2 );
	LOAD( properties, vkGetPhysicalDeviceProperties2 );
	LOAD( formats, vkGetPhysicalDeviceFormatProperties );
	LOAD( rates, vkGetPhysicalDeviceFragmentShadingRatesKHR );
#undef LOAD
	result = extensions( physical, NULL, &count, NULL );
	if ( result != VK_SUCCESS ) {
		caps->reason = "device extension count query failed";
		return result;
	}
	caps->availableExtensions = count;
	if ( !count )
		UNAVAILABLE( "device reports no extensions" );
	if ( count > 4096 )
		UNAVAILABLE( "device extension count exceeds defensive limit 4096" );
	ext = malloc( (size_t)count * sizeof( *ext ) );
	if ( !ext ) {
		caps->reason = "out of host memory enumerating device extensions";
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}
	result = extensions( physical, NULL, &count, ext );
	if ( result != VK_SUCCESS ) {
		free( ext );
		caps->reason = "device extension enumeration incomplete or failed";
		return result < 0 ? result : VK_SUCCESS;
	}
	for ( i = 0; i < count; i++ ) {
		ext[i].extensionName[VK_MAX_EXTENSION_NAME_SIZE - 1] = 0;
		if ( !strcmp( ext[i].extensionName, VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME ) )
			hasRate = 1;
		if ( !strcmp( ext[i].extensionName, VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME ) )
			hasPass = 1;
	}
	free( ext );
	if ( !hasRate )
		UNAVAILABLE( "VK_KHR_fragment_shading_rate is not advertised" );
	if ( !hasPass )
		UNAVAILABLE( "VK_KHR_create_renderpass2 is required below Vulkan 1.2" );
	memset( &f, 0, sizeof( f ) );
	f.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	caps->feature.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR;
	f.pNext = &caps->feature;
	features( physical, &f );
	if ( !caps->feature.attachmentFragmentShadingRate )
		UNAVAILABLE( "attachmentFragmentShadingRate feature is false" );
	memset( &fp, 0, sizeof( fp ) );
	fp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR;
	memset( &p, 0, sizeof( p ) );
	p.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
	p.pNext = &fp;
	properties( physical, &p );
	if ( !fp.layeredShadingRateAttachments )
		UNAVAILABLE( "layeredShadingRateAttachments is false; using full rate" );
	caps->texelWidth = fp.minFragmentShadingRateAttachmentTexelSize.width;
	caps->texelHeight = fp.minFragmentShadingRateAttachmentTexelSize.height;
	if ( !caps->texelWidth || !caps->texelHeight )
		UNAVAILABLE( "invalid shading rate attachment texel dimensions" );
	formats( physical, VK_FORMAT_R8_UINT, &format );
	if ( !(format.optimalTilingFeatures & VK_FORMAT_FEATURE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR) )
		UNAVAILABLE( "R8_UINT cannot be a shading rate attachment" );
	memset( list, 0, sizeof( list ) );
	for ( i = 0; i < VK_FOV_MAX_RATES; i++ )
		list[i].sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_KHR;
	count = VK_FOV_MAX_RATES;
	result = rates( physical, &count, list );
	if ( result < 0 ) {
		caps->reason = "fragment shading rate enumeration failed";
		return result;
	}
	if ( count > VK_FOV_MAX_RATES )
		count = VK_FOV_MAX_RATES;
	for ( i = 0; i < count; i++ ) {
		caps->rates[i].width = list[i].fragmentSize.width;
		caps->rates[i].height = list[i].fragmentSize.height;
		caps->rates[i].samples = list[i].sampleCounts;
	}
	caps->rateCount = count;
	if ( !count )
		UNAVAILABLE( "device reports no fragment shading rates" );
	/* Only the attachment feature is requested; do not enable every queried bit. */
	caps->feature.pipelineFragmentShadingRate = caps->feature.primitiveFragmentShadingRate = VK_FALSE;
	caps->extensions[caps->extensionCount++] = VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME;
	if ( api < VK_API_VERSION_1_2 )
		caps->extensions[caps->extensionCount++] = VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME;
	caps->supported = 1;
	caps->reason = "attachment shading rate supported";
	return VK_SUCCESS;
#undef UNAVAILABLE
}
static int memoryType( const VkPhysicalDeviceMemoryProperties *p, uint32_t mask, VkMemoryPropertyFlags flags,
					   uint32_t *out ) {
	uint32_t i;
	for ( i = 0; i < p->memoryTypeCount; i++ )
		if ( (mask & (1u << i)) && (p->memoryTypes[i].propertyFlags & flags) == flags ) {
			*out = i;
			return 1;
		}
	return 0;
}
void VK_FovDestroy( vkFovResources_t *r ) {
	uint32_t i;
	if ( !r )
		return;
	for ( i = 0; i < VK_FOV_MAX_SLOTS; i++ ) {
		if ( r->mapped[i] )
			r->vk.UnmapMemory( r->device, r->stagingMemory[i] );
		if ( r->staging[i] )
			r->vk.DestroyBuffer( r->device, r->staging[i], NULL );
		if ( r->stagingMemory[i] )
			r->vk.FreeMemory( r->device, r->stagingMemory[i], NULL );
	}
	if ( r->view )
		r->vk.DestroyImageView( r->device, r->view, NULL );
	if ( r->image )
		r->vk.DestroyImage( r->device, r->image, NULL );
	if ( r->memory )
		r->vk.FreeMemory( r->device, r->memory, NULL );
	memset( r, 0, sizeof( *r ) );
}
VkResult VK_FovCreate( vkFovResources_t *r, VkDevice device, PFN_vkGetDeviceProcAddr proc,
					   const VkPhysicalDeviceMemoryProperties *memory, const vkFovCaps_t *caps,
					   uint32_t width, uint32_t height, uint32_t slots ) {
	VkImageCreateInfo image;
	VkImageViewCreateInfo view;
	VkMemoryRequirements req;
	VkMemoryAllocateInfo alloc;
	VkBufferCreateInfo buffer;
	VkResult result;
	uint32_t i;
	if ( !r )
		return VK_ERROR_INITIALIZATION_FAILED;
	memset( r, 0, sizeof( *r ) );
	if ( !device || !proc || !memory || !caps || !caps->supported || !width || !height || !slots ||
		slots > VK_FOV_MAX_SLOTS || !caps->texelWidth || !caps->texelHeight )
		return VK_ERROR_INITIALIZATION_FAILED;
	r->device = device;
	r->caps = *caps;
	r->caps.feature.pNext = NULL;
	r->eyeWidth = width;
	r->eyeHeight = height;
	r->slots = slots;
	r->layers = 2;
#define LOAD(n) \
	r->vk.n = (PFN_vk##n)proc( device, "vk" #n ); \
	if ( !r->vk.n ) { \
		memset( r, 0, sizeof( *r ) ); \
		return VK_ERROR_EXTENSION_NOT_PRESENT; \
	}
	VK_FOV_PROCS( LOAD )
#undef LOAD
	r->width = width / caps->texelWidth + (width % caps->texelWidth != 0);
	r->height = height / caps->texelHeight + (height % caps->texelHeight != 0);
	if ( r->height > SIZE_MAX / r->width / r->layers ) {
		result = VK_ERROR_OUT_OF_HOST_MEMORY;
		goto fail;
	}
	r->bytes = (size_t)r->width * r->height * r->layers;
	memset( &image, 0, sizeof( image ) );
	image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image.imageType = VK_IMAGE_TYPE_2D;
	image.format = VK_FORMAT_R8_UINT;
	image.extent.width = r->width;
	image.extent.height = r->height;
	image.extent.depth = 1;
	image.mipLevels = 1;
	image.arrayLayers = r->layers;
	image.samples = VK_SAMPLE_COUNT_1_BIT;
	image.tiling = VK_IMAGE_TILING_OPTIMAL;
	image.usage = VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
#define CHECK(call) \
	do { \
		result = (call); \
		if ( result != VK_SUCCESS ) \
			goto fail; \
	} while ( 0 )
	CHECK( r->vk.CreateImage( device, &image, NULL, &r->image ) );
	r->vk.GetImageMemoryRequirements( device, r->image, &req );
	memset( &alloc, 0, sizeof( alloc ) );
	alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc.allocationSize = req.size;
	if ( !memoryType( memory, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
					 &alloc.memoryTypeIndex ) ) {
		result = VK_ERROR_FEATURE_NOT_PRESENT;
		goto fail;
	}
	CHECK( r->vk.AllocateMemory( device, &alloc, NULL, &r->memory ) );
	CHECK( r->vk.BindImageMemory( device, r->image, r->memory, 0 ) );
	memset( &view, 0, sizeof( view ) );
	view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view.image = r->image;
	view.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
	view.format = VK_FORMAT_R8_UINT;
	view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view.subresourceRange.levelCount = 1;
	view.subresourceRange.layerCount = r->layers;
	CHECK( r->vk.CreateImageView( device, &view, NULL, &r->view ) );
	memset( &buffer, 0, sizeof( buffer ) );
	buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer.size = r->bytes;
	buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	for ( i = 0; i < slots; i++ ) {
		CHECK( r->vk.CreateBuffer( device, &buffer, NULL, &r->staging[i] ) );
		r->vk.GetBufferMemoryRequirements( device, r->staging[i], &req );
		alloc.allocationSize = req.size;
		if ( !memoryType( memory, req.memoryTypeBits,
						 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
						 &alloc.memoryTypeIndex ) ) {
			result = VK_ERROR_FEATURE_NOT_PRESENT;
			goto fail;
		}
		CHECK( r->vk.AllocateMemory( device, &alloc, NULL, &r->stagingMemory[i] ) );
		CHECK( r->vk.BindBufferMemory( device, r->staging[i], r->stagingMemory[i], 0 ) );
		CHECK( r->vk.MapMemory( device, r->stagingMemory[i], 0, VK_WHOLE_SIZE, 0, &r->mapped[i] ) );
	}
	return VK_SUCCESS;
fail:
	VK_FovDestroy( r );
	return result;
#undef CHECK
}
void VK_FovDiscardUpload( vkFovResources_t *r ) {
	if ( r ) {
		r->uploaded = 0;
		r->uploadedSerial = 0;
	}
}
VkResult VK_FovUpload( vkFovResources_t *r, VkCommandBuffer cmd, uint32_t slot, uint64_t serial,
					   const vkFovMap_t *map ) {
	VkImageMemoryBarrier barrier;
	VkBufferImageCopy copy;
	if ( !r || !r->image || !cmd || slot >= r->slots || !map )
		return VK_ERROR_INITIALIZATION_FAILED;
	if ( r->uploaded && r->uploadedSerial == serial )
		return VK_SUCCESS;
	if ( map->width != r->eyeWidth || map->height != r->eyeHeight || map->texelWidth != r->caps.texelWidth ||
		map->texelHeight != r->caps.texelHeight )
		return VK_ERROR_INITIALIZATION_FAILED;
	/* Disabled/menu maps are uniformly full-rate regardless of tracked centers.
	 * Capabilities and attachment geometry are immutable for this resource owner. */
	if ( r->uploaded && ((r->uploadedMap.strength <= 0 && map->strength <= 0) ||
						!memcmp( &r->uploadedMap, map, sizeof( *map ) )) ) {
		r->uploadedSerial = serial;
		return VK_SUCCESS;
	}
	if ( !VK_FovWrite( r->mapped[slot], r->bytes, map, r->caps.rates, r->caps.rateCount ) )
		return VK_ERROR_INITIALIZATION_FAILED;
	memset( &barrier, 0, sizeof( barrier ) );
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = r->image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.layerCount = r->layers;
	barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	r->vk.CmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR,
							  VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier );
	memset( &copy, 0, sizeof( copy ) );
	copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	copy.imageSubresource.layerCount = r->layers;
	copy.imageExtent.width = r->width;
	copy.imageExtent.height = r->height;
	copy.imageExtent.depth = 1;
	r->vk.CmdCopyBufferToImage( cmd, r->staging[slot], r->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
								&copy );
	barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR;
	r->vk.CmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
							  VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR, 0, 0, NULL, 0, NULL,
							  1, &barrier );
	r->uploaded = 1;
	r->uploadedSerial = serial;
	r->uploadedMap = *map;
	return VK_SUCCESS;
}

void VK_FovAttachment( const vkFovResources_t *r, uint32_t index, VkAttachmentDescription2 *description,
					   VkAttachmentReference2 *reference, VkFragmentShadingRateAttachmentInfoKHR *info ) {
	memset( description, 0, sizeof( *description ) );
	description->sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
	description->format = VK_FORMAT_R8_UINT;
	description->samples = VK_SAMPLE_COUNT_1_BIT;
	description->loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	description->storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	description->stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	description->stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	description->initialLayout = description->finalLayout =
		VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
	memset( reference, 0, sizeof( *reference ) );
	reference->sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
	reference->attachment = index;
	reference->layout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
	reference->aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	memset( info, 0, sizeof( *info ) );
	info->sType = VK_STRUCTURE_TYPE_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR;
	info->pFragmentShadingRateAttachment = reference;
	info->shadingRateAttachmentTexelSize.width = r->caps.texelWidth;
	info->shadingRateAttachmentTexelSize.height = r->caps.texelHeight;
}

static void convertReference( const VkAttachmentReference *src, VkAttachmentReference2 *dst ) {
	memset( dst, 0, sizeof( *dst ) );
	dst->sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
	dst->attachment = src->attachment;
	dst->layout = src->layout;
}
VkResult VK_FovCreateRenderPass( const vkFovResources_t *r, const VkRenderPassCreateInfo *legacy,
								 PFN_vkCreateRenderPass2 create, VkRenderPass *out ) {
	VkRenderPassCreateInfo2 info;
	VkAttachmentDescription2 attachments[6];
	VkSubpassDescription2 sub;
	VkAttachmentReference2 colors[5], resolves[5], depth, rateRef;
	VkSubpassDependency2 deps[3];
	VkFragmentShadingRateAttachmentInfoKHR rate;
	const VkSubpassDescription *original;
	uint32_t i;
	const VkRenderPassMultiviewCreateInfo *multiview = NULL;
	if ( !r || !r->device || !legacy || !create || !out || legacy->subpassCount != 1 || !legacy->pSubpasses ||
		legacy->attachmentCount > 5 || legacy->dependencyCount > 3 ||
		(legacy->attachmentCount && !legacy->pAttachments) ||
		(legacy->dependencyCount && !legacy->pDependencies) )
		return VK_ERROR_INITIALIZATION_FAILED;
	if ( legacy->pNext ) {
		multiview = (const VkRenderPassMultiviewCreateInfo *)legacy->pNext;
		if ( multiview->sType != VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO || multiview->pNext ||
			(multiview->subpassCount &&
			 (multiview->subpassCount != legacy->subpassCount || !multiview->pViewMasks)) ||
			(multiview->dependencyCount &&
			 (multiview->dependencyCount != legacy->dependencyCount || !multiview->pViewOffsets)) ||
			(multiview->correlationMaskCount && !multiview->pCorrelationMasks) )
			return VK_ERROR_INITIALIZATION_FAILED;
	}
	original = legacy->pSubpasses;
	if ( original->pipelineBindPoint != VK_PIPELINE_BIND_POINT_GRAPHICS || original->inputAttachmentCount ||
		original->colorAttachmentCount > 5 ||
		(original->colorAttachmentCount && !original->pColorAttachments) )
		return VK_ERROR_INITIALIZATION_FAILED;
	memset( &info, 0, sizeof( info ) );
	info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2;
	info.flags = legacy->flags;
	info.attachmentCount = legacy->attachmentCount + 1;
	info.pAttachments = attachments;
	info.subpassCount = 1;
	info.pSubpasses = &sub;
	info.dependencyCount = legacy->dependencyCount;
	info.pDependencies = deps;
	if ( multiview ) {
		info.correlatedViewMaskCount = multiview->correlationMaskCount;
		info.pCorrelatedViewMasks = multiview->pCorrelationMasks;
	}
	memset( attachments, 0, sizeof( attachments ) );
	for ( i = 0; i < legacy->attachmentCount; i++ ) {
		const VkAttachmentDescription *a = &legacy->pAttachments[i];
		VkAttachmentDescription2 *b = &attachments[i];
		b->sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
		b->flags = a->flags;
		b->format = a->format;
		b->samples = a->samples;
		b->loadOp = a->loadOp;
		b->storeOp = a->storeOp;
		b->stencilLoadOp = a->stencilLoadOp;
		b->stencilStoreOp = a->stencilStoreOp;
		b->initialLayout = a->initialLayout;
		b->finalLayout = a->finalLayout;
	}
	VK_FovAttachment( r, legacy->attachmentCount, &attachments[legacy->attachmentCount], &rateRef, &rate );
	memset( &sub, 0, sizeof( sub ) );
	sub.sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2;
	sub.pNext = &rate;
	sub.viewMask = multiview && multiview->subpassCount ? multiview->pViewMasks[0] : 0;
	sub.flags = original->flags;
	sub.pipelineBindPoint = original->pipelineBindPoint;
	sub.colorAttachmentCount = original->colorAttachmentCount;
	sub.pColorAttachments = colors;
	sub.preserveAttachmentCount = original->preserveAttachmentCount;
	sub.pPreserveAttachments = original->pPreserveAttachments;
	for ( i = 0; i < original->colorAttachmentCount; i++ ) {
		convertReference( &original->pColorAttachments[i], &colors[i] );
		if ( original->pResolveAttachments )
			convertReference( &original->pResolveAttachments[i], &resolves[i] );
	}
	if ( original->pResolveAttachments )
		sub.pResolveAttachments = resolves;
	if ( original->pDepthStencilAttachment ) {
		convertReference( original->pDepthStencilAttachment, &depth );
		sub.pDepthStencilAttachment = &depth;
	}
	memset( deps, 0, sizeof( deps ) );
	for ( i = 0; i < legacy->dependencyCount; i++ ) {
		const VkSubpassDependency *a = &legacy->pDependencies[i];
		VkSubpassDependency2 *b = &deps[i];
		b->sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2;
		b->srcSubpass = a->srcSubpass;
		b->dstSubpass = a->dstSubpass;
		b->srcStageMask = a->srcStageMask;
		b->dstStageMask = a->dstStageMask;
		b->srcAccessMask = a->srcAccessMask;
		b->dstAccessMask = a->dstAccessMask;
		b->dependencyFlags = a->dependencyFlags;
		b->viewOffset = multiview && multiview->dependencyCount ? multiview->pViewOffsets[i] : 0;
	}
	return create( r->device, &info, NULL, out );
}
