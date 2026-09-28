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
	const char *extensions[6];
	uint32_t extensionCount;
	VkPhysicalDeviceFragmentShadingRateFeaturesKHR feature;
	/* Fragment density map backend. */
	int fdm2, fdmOffset, tileProperties;
	uint32_t fdmTexelMaxWidth, fdmTexelMaxHeight;
	const char *fdmOffsetExtension;
	VkExtent2D fdmOffsetGranularity;
	VkPhysicalDeviceFragmentDensityMapFeaturesEXT fdmFeature;
	VkPhysicalDeviceFragmentDensityMapOffsetFeaturesQCOM offsetFeature;
	VkPhysicalDeviceTilePropertiesFeaturesQCOM tileFeature;
} vkFovCaps_t;
/* Before VkDevice creation, runtime-selected physical device only. Unsupported
 * hardware returns SUCCESS with supported=0. Full shading rate wins, otherwise a
 * non-subsampled density map. Caller appends extensions and chains
 * VK_FovFeatures into device creation only when supported. */
VkResult VK_FovQuery( vkFovCaps_t *, VkInstance, VkPhysicalDevice, PFN_vkGetInstanceProcAddr,
					  uint32_t apiVersion );
/* The selected backend's feature structs, chained ahead of next. */
void *VK_FovFeatures( vkFovCaps_t *, void *next );
#define VK_FOV_PROCS(X) \
	X( CreateImage ) \
	X( DestroyImage ) X( GetImageMemoryRequirements ) X( AllocateMemory ) X( FreeMemory ) X( BindImageMemory ) \
	X( CreateImageView ) X( DestroyImageView ) X( CreateBuffer ) X( DestroyBuffer ) \
	X( GetBufferMemoryRequirements ) X( BindBufferMemory ) X( MapMemory ) X( UnmapMemory ) \
	X( CmdPipelineBarrier ) X( CmdCopyBufferToImage )
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
} vkFovResources_t;
/* No global Vulkan dispatch or engine allocations. Caller must enable queried
 * feature/extensions before creation. Extents are per-eye; the image always has
 * two layers. Failure fully rolls back resources. */
VkResult VK_FovCreate( vkFovResources_t *, VkDevice, PFN_vkGetDeviceProcAddr,
					   const VkPhysicalDeviceMemoryProperties *, const vkFovCaps_t *, uint32_t eyeWidth,
					   uint32_t eyeHeight, uint32_t slots, VkImageCreateFlags flags );
/* Density maps: copies current through the slot's staging buffer, leaving the image ready for a pass. */
void VK_FovCopyDensity( vkFovResources_t *, VkCommandBuffer, uint32_t slot );
/* Appends the density map as the pass's last attachment; attachments must have room for it. */
void VK_FovDensityAttachment( VkRenderPassCreateInfo *, VkAttachmentDescription *attachments,
							  VkRenderPassFragmentDensityMapCreateInfoEXT * );
/* After the slot fence, before any scene pass. An identical effective map is
 * reused across frames; repeated serials never rewrite in-flight staging.
 * Use one monotonically increasing serial across map loads. */
VkResult VK_FovUpload( vkFovResources_t *, VkCommandBuffer, uint32_t slot, uint64_t serial,
					   const vkFovMap_t * );
/* A recorded upload is not durable if its command buffer is discarded. */
void VK_FovDiscardUpload( vkFovResources_t * );
/* Build the extra LOAD/STORE attachment, reference and subpass chain.
 * Caller must keep all three structs alive through vkCreateRenderPass2. */
void VK_FovAttachment( const vkFovResources_t *, uint32_t index, VkAttachmentDescription2 *,
					   VkAttachmentReference2 *, VkFragmentShadingRateAttachmentInfoKHR * );
/* Bounded one-subpass bridge from VkRenderPassCreateInfo for the scene and
 * post-bloom passes. Up to five original attachments/color outputs, three deps;
 * preserves multiview masks, correlations and dependency offsets;
 * rejects unknown pNext/input attachments instead of dropping semantics. */
VkResult VK_FovCreateRenderPass( const vkFovResources_t *, const VkRenderPassCreateInfo *,
								 PFN_vkCreateRenderPass2, VkRenderPass * );
/* GPU idle; destroy framebuffer references to view before this call. */
void VK_FovDestroy( vkFovResources_t * );
#endif
