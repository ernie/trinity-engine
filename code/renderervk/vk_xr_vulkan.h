/* Host-owned Vulkan graphics and optional OpenXR stereo presentation.
 * No engine, Vulkan loader, or OpenXR loader link dependencies. */
#ifndef VK_XR_VULKAN_H
#define VK_XR_VULKAN_H
#include "../renderercommon/vulkan/vulkan.h"
#ifndef XR_NO_PROTOTYPES
#define XR_NO_PROTOTYPES
#endif
#ifndef XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_GRAPHICS_API_VULKAN
#endif
#include "../thirdparty/openxr/openxr_platform.h"

#define VK_XRVK_MAX_IMAGES 32
/* MUTABLE_FORMAT lets direct mode render through a UNORM view of the sRGB image. */
#define VK_XRVK_TARGET_USAGE \
	(XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | \
	 XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT)
#define VK_XRVK_FUNCTIONS(X) \
	X( GetVulkanGraphicsRequirements2KHR ) \
	X( CreateVulkanInstanceKHR ) X( GetVulkanGraphicsDevice2KHR ) X( CreateVulkanDeviceKHR ) X( CreateSession ) \
	X( DestroySession ) X( BeginSession ) X( EndSession ) X( RequestExitSession ) X( PollEvent ) \
	X( CreateReferenceSpace ) X( DestroySpace ) X( EnumerateViewConfigurationViews ) \
	X( EnumerateEnvironmentBlendModes ) X( EnumerateSwapchainFormats ) X( CreateSwapchain ) \
	X( DestroySwapchain ) X( EnumerateSwapchainImages ) X( AcquireSwapchainImage ) \
	X( WaitSwapchainImage ) X( ReleaseSwapchainImage ) X( WaitFrame ) X( BeginFrame ) \
	X( EndFrame ) X( LocateViews )

typedef struct {
#define VK_XRVK_PROC(name) PFN_xr##name name;
	VK_XRVK_FUNCTIONS( VK_XRVK_PROC )
#undef VK_XRVK_PROC
} vkXRVkDispatch_t;

typedef struct {
	XrSwapchain handle;
	uint32_t width, height, count, index;
	int acquired, waited;
	VkImageCreateFlags createFlags; /* extra flags the runtime accepted */
	XrSwapchainImageVulkan2KHR images[VK_XRVK_MAX_IMAGES];
} vkXRVkTarget_t;

typedef struct {
	vkXRVkDispatch_t xr;
	XrInstance instance; /* Borrowed; owner must outlive this context. */
	XrSystemId system;
	XrSession session;
	XrSpace space;
	XrSpace viewSpace;
	XrReferenceSpaceType spaceType;
	int referenceSpaceChanged; /* Host consumes after the next valid view pose. */
	XrTime referenceSpaceChangeTime;
	int scope; /* Submit the mono scope texture as a head-locked quad. */
	XrGraphicsRequirementsVulkan2KHR requirements;
	XrSessionState state;
	XrEnvironmentBlendMode blend;
	int running, lost, frameBegun, renderable;
	int profileChanged;
	int modelsChanged; /* the set of controller models changed during the last action sync */
	int formatList;
	int createInfoMeta;					   /* XR_META_vulkan_swapchain_create_info enabled */
	VkImageCreateFlags targetCreateFlags; /* extra flags to request; set before Bind */
	XrTime displayTime;
	XrDuration displayPeriod; /* the runtime's predicted frame interval, i.e. the display rate it runs the app at */
	VkFormat format;
	float renderScale;					/* zero defaults to 1; set before Bind */
	uint32_t maxEyeWidth, maxEyeHeight; /* optional graphics limits */
	vkXRVkTarget_t target; /* layer 0 left eye, layer 1 right eye */
	XrView views[2];
} vkXRVk_t;

void VK_XRVK_Sleep( unsigned msec );
/* Instance must enable XR_KHR_vulkan_enable2; the host destroys both Vulkan objects. */
XrResult VK_XRVK_Init( vkXRVk_t *ctx, XrInstance instance, XrSystemId system,
					   PFN_xrGetInstanceProcAddr getproc, int formatList );
XrResult VK_XRVK_CreateInstance( vkXRVk_t *ctx, PFN_vkGetInstanceProcAddr proc,
								 const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *allocator,
								 VkInstance *instance, VkResult *result );
XrResult VK_XRVK_GetPhysicalDevice( vkXRVk_t *ctx, VkInstance instance, VkPhysicalDevice *device );
XrResult VK_XRVK_CreateDevice( vkXRVk_t *ctx, PFN_vkGetInstanceProcAddr proc, VkPhysicalDevice physical,
							   const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *allocator,
							   VkDevice *device, VkResult *result );
/* Preferred formats are checked in caller order. Only stereo + opaque blend.
 * One two-layer color image, sampleCount=1; renderer owns depth/MSAA resources. */
XrResult VK_XRVK_Bind( vkXRVk_t *ctx, const XrGraphicsBindingVulkan2KHR *binding, const VkFormat *formats,
					   uint32_t formatCount );
/* A candidate target must be zeroed; the caller waits for GPU idle and drops framebuffer references before destroy. */
int VK_XRVK_TargetsBusy( const vkXRVk_t *ctx );
XrResult VK_XRVK_CreateTarget( vkXRVk_t *ctx, vkXRVkTarget_t *target );
XrResult VK_XRVK_DestroyTarget( vkXRVk_t *ctx, vkXRVkTarget_t *target );
/* Poll only outside a frame. It consumes at most 64 events. READY starts; STOPPING ends;
 * EXITING/LOSS_PENDING and instance loss mark lost for host flat recovery. */
XrResult VK_XRVK_Poll( vkXRVk_t *ctx );
int VK_XRVK_ConsumeSpaceChange( vkXRVk_t *ctx );
/* Begin returns XR_SESSION_NOT_FOCUSED when not running; a wait failure requires teardown, since an image
 * cannot be released until its wait succeeds. */
XrResult VK_XRVK_Begin( vkXRVk_t *ctx );
/* Images go back to COLOR_ATTACHMENT_OPTIMAL before End; a failed Begin with frameBegun set still owes End( ctx, 0 ) or Shutdown. */
XrResult VK_XRVK_End( vkXRVk_t *ctx, int rendered );
/* Polls up to 100 times so the session reaches STOPPING before it is destroyed. */
void VK_XRVK_Shutdown( vkXRVk_t *ctx );
#endif
