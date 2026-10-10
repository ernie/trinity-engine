/* KTX2 (Basis Universal) textures to RGBA, for the runtime's controller models: Meta's carry UASTC with Zstd. */
#ifndef VR_KTX2_H
#define VR_KTX2_H
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns 1 with the top mip as width x height RGBA from alloc, which the caller releases; 0 with nothing allocated. */
int VR_KTX2Decode( const void *data, int size, unsigned char **rgba, int *width, int *height,
				   void *( *alloc )( size_t ), void ( *release )( void * ) );

#ifdef __cplusplus
}
#endif
#endif
