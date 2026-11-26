#ifndef VK_FOVEATION_H
#define VK_FOVEATION_H
#include "../renderercommon/vulkan/vulkan.h"
#include "vk_foveation_math.h"
#define VK_FOV_MAX_RATES 32
#define VK_FOV_MAX_SLOTS 4
#define VK_FOV_EXT_FDM_OFFSET_EXTENSION_NAME "VK_EXT_fragment_density_map_offset"
typedef enum { VK_FOV_BACKEND_NONE, VK_FOV_BACKEND_SHADING_RATE, VK_FOV_BACKEND_FDM } vkFovBackend_t;
typedef struct {
	int supported;
	vkFovBackend_t backend;
	uint32_t texelWidth, texelHeight; /* Shading-rate or density-map texel, per backend. */
	const char *reason; /* Static diagnostic; valid even when unsupported. */
	const char *fdmReason; /* Why the density map backend was not chosen; NULL when not probed. */
	uint32_t availableExtensions;
	vkFovRate_t rates[VK_FOV_MAX_RATES];
	uint32_t rateCount;
	const char *extensions[8];
	uint32_t extensionCount;
	VkPhysicalDeviceFragmentShadingRateFeaturesKHR feature;
	/* Fragment density map backend. */
	int fdm2, fdmOffset, tileProperties;
	uint32_t fdmTexelMaxWidth, fdmTexelMaxHeight;
	const char *fdmOffsetExtension;
	VkExtent2D fdmOffsetGranularity;
	VkFormat fdmLinearFormat; /* VK_FORMAT_UNDEFINED when the host cannot write a linear map. */
	VkExtent2D fdmLinearExtent;
	VkPhysicalDeviceFragmentDensityMapFeaturesEXT fdmFeature;
	VkPhysicalDeviceFragmentDensityMapOffsetFeaturesQCOM offsetFeature;
	VkPhysicalDeviceTilePropertiesFeaturesQCOM tileFeature;
} vkFovCaps_t;
/* Before VkDevice creation; unsupported hardware returns VK_SUCCESS with supported = 0. */
VkResult VK_FovQuery( vkFovCaps_t *, VkInstance, VkPhysicalDevice, PFN_vkGetInstanceProcAddr,
					  uint32_t apiVersion );
/* The selected backend's feature structs, chained ahead of next. */
void *VK_FovFeatures( vkFovCaps_t *, void *next );
#define VK_FOV_PROCS(X) \
	X( CreateImage ) \
	X( DestroyImage ) X( GetImageMemoryRequirements ) X( AllocateMemory ) X( FreeMemory ) X( BindImageMemory ) \
	X( CreateImageView ) X( DestroyImageView ) X( CreateBuffer ) X( DestroyBuffer ) \
	X( GetBufferMemoryRequirements ) X( BindBufferMemory ) X( MapMemory ) X( UnmapMemory ) \
	X( CmdPipelineBarrier ) X( CmdCopyBufferToImage ) X( GetImageSubresourceLayout )
typedef struct {
#define FOV_PROC(n) PFN_vk##n n;
	VK_FOV_PROCS( FOV_PROC )
#undef FOV_PROC
} vkFovDispatch_t;
typedef struct {
	VkDevice device;
	vkFovDispatch_t vk;
	vkFovCaps_t caps;
	VkImage image;
	VkDeviceMemory memory;
	VkImageView view;
	VkPipeline debugPipeline; /* Renderer-owned, retired with this target set. */
	VkBuffer staging[VK_FOV_MAX_SLOTS];
	VkDeviceMemory stagingMemory[VK_FOV_MAX_SLOTS];
	void *mapped[VK_FOV_MAX_SLOTS];
	uint32_t width, height, eyeWidth, eyeHeight, layers, slots;
	size_t bytes;
	uint64_t uploadedSerial;
	int uploaded;
	/* Effective map last recorded into the attachment. Cleared with resources. */
	vkFovMap_t uploadedMap;
	/* Density maps: the next map is written to scratch; current is what the image holds. */
	uint8_t *scratch, *current;
	/* A host-written map is linear and mapped, and stays in GENERAL. */
	int hostWritten;
	VkSubresourceLayout layerLayout[2];
	void *imageMapped;
	uint16_t half[256];
} vkFovResources_t;
/* Extents are per eye; the image always has two layers. Failure fully rolls back. */
VkResult VK_FovCreate( vkFovResources_t *, VkDevice, PFN_vkGetDeviceProcAddr,
					   const VkPhysicalDeviceMemoryProperties *, const vkFovCaps_t *, uint32_t eyeWidth,
					   uint32_t eyeHeight, uint32_t slots, VkImageCreateFlags flags );
/* The memory a host-written map lives in: host visible and coherent, cached and device local where offered. */
int VK_FovHostMemoryType( const VkPhysicalDeviceMemoryProperties *, uint32_t typeBits, uint32_t *index );
/* Writes a byte map's eye layers into a linear image through each layer's layout; a half table makes it R16G16. */
void VK_FdmWriteImage( uint8_t *image, const VkSubresourceLayout layers[2], const uint8_t *map, uint32_t width,
					   uint32_t height, const uint16_t *half );
/* Density maps: copies current through the staging buffer, leaving the image ready for a pass. */
void VK_FovCopyDensity( vkFovResources_t *, VkCommandBuffer );
/* Density maps: writes current into a host-written map. */
void VK_FovWriteDensity( vkFovResources_t * );
/* Density maps, once after creation: fills the image with current and leaves it in its pass layout. */
void VK_FovInitDensity( vkFovResources_t *, VkCommandBuffer );
/* Appends the density map as the pass's last attachment; attachments must have room for it. */
void VK_FovDensityAttachment( const vkFovResources_t *, VkRenderPassCreateInfo *, VkAttachmentDescription *attachments,
							  VkRenderPassFragmentDensityMapCreateInfoEXT * );
/* After the slot fence, before any scene pass; one monotonically increasing serial across map loads. */
VkResult VK_FovUpload( vkFovResources_t *, VkCommandBuffer, uint32_t slot, uint64_t serial,
					   const vkFovMap_t * );
/* A recorded upload is not durable if its command buffer is discarded. */
void VK_FovDiscardUpload( vkFovResources_t * );
/* The caller keeps the attachment, reference and subpass chain alive through vkCreateRenderPass2. */
void VK_FovAttachment( const vkFovResources_t *, uint32_t index, VkAttachmentDescription2 *,
					   VkAttachmentReference2 *, VkFragmentShadingRateAttachmentInfoKHR * );
/* One-subpass bridge: up to five attachments, three deps; unknown pNext or input attachments are rejected, not dropped. */
VkResult VK_FovCreateRenderPass( const vkFovResources_t *, const VkRenderPassCreateInfo *,
								 PFN_vkCreateRenderPass2, VkRenderPass * );
/* GPU idle; destroy framebuffer references to view before this call. */
void VK_FovDestroy( vkFovResources_t * );
#endif
