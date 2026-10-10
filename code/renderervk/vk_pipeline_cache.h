// The saved pipeline cache's file format and checks, free of engine dependencies
#ifndef VK_PIPELINE_CACHE_H
#define VK_PIPELINE_CACHE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define VK_PIPELINE_CACHE_MAGIC 0x43505254u		// "TRPC"
#define VK_PIPELINE_CACHE_MAX ( 16 * 1024 * 1024 )	// bounds growth within one build
#define VK_PIPELINE_CACHE_VULKAN_HEADER 32			// VkPipelineCacheHeaderVersionOne

typedef struct {
	uint32_t magic;
	uint32_t dataSize;
	uint32_t checksum;		// over the data, so a write cut short reads as damaged
	char version[64];		// the engine build; another build starts empty
} vkPipelineCacheFile_t;

static inline uint32_t VK_PipelineCacheChecksum( const uint8_t *data, size_t size ) {
	uint32_t hash = 2166136261u;	// FNV-1a
	size_t i;
	for ( i = 0; i < size; i++ ) {
		hash ^= data[i];
		hash *= 16777619u;
	}
	return hash;
}

static inline void VK_PipelineCacheVersion( char field[64], const char *version ) {
	memset( field, 0, 64 );
	strncpy( field, version, 63 );
}

static inline void VK_PipelineCacheWrap( vkPipelineCacheFile_t *header, const char *version, const uint8_t *data, uint32_t size ) {
	header->magic = VK_PIPELINE_CACHE_MAGIC;
	header->dataSize = size;
	header->checksum = VK_PipelineCacheChecksum( data, size );
	VK_PipelineCacheVersion( header->version, version );
}

// The driver's data, or NULL for a damaged file or one from another build, device or driver.
// Drivers should reject foreign data themselves, but some mobile drivers mishandle it.
static inline const uint8_t *VK_PipelineCacheUnwrap( const uint8_t *file, size_t fileSize, const char *version, uint32_t vendorID,
													  uint32_t deviceID, const uint8_t uuid[16], size_t *dataSize ) {
	vkPipelineCacheFile_t header;
	char expected[64];
	const uint8_t *data;
	uint32_t vulkan[4];

	*dataSize = 0;
	if ( fileSize < sizeof( header ) )
		return NULL;
	memcpy( &header, file, sizeof( header ) );
	VK_PipelineCacheVersion( expected, version );
	if ( header.magic != VK_PIPELINE_CACHE_MAGIC || memcmp( header.version, expected, sizeof( expected ) ) ||
		 header.dataSize < VK_PIPELINE_CACHE_VULKAN_HEADER || header.dataSize > VK_PIPELINE_CACHE_MAX ||
		 fileSize != sizeof( header ) + header.dataSize )
		return NULL;
	data = file + sizeof( header );
	if ( VK_PipelineCacheChecksum( data, header.dataSize ) != header.checksum )
		return NULL;
	// headerSize, headerVersion, vendorID, deviceID, then the 16-byte pipelineCacheUUID
	memcpy( vulkan, data, sizeof( vulkan ) );
	if ( vulkan[0] < VK_PIPELINE_CACHE_VULKAN_HEADER || vulkan[0] > header.dataSize || vulkan[1] != 1 ||
		 vulkan[2] != vendorID || vulkan[3] != deviceID || memcmp( data + 16, uuid, 16 ) )
		return NULL;
	*dataSize = header.dataSize;
	return data;
}

static inline int VK_PipelineCacheWorthSaving( size_t size, size_t saved ) {
	return size > 0 && size <= VK_PIPELINE_CACHE_MAX && size != saved;
}

#endif
