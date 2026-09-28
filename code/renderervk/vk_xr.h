#ifndef VK_XR_H
#define VK_XR_H
#include "vk_xr_vulkan.h"
#include "vk_foveation_math.h"

void VK_DesktopTrackingStatus( qboolean visible, qhandle_t font, qhandle_t icon );
qboolean VK_XR_PrepareInit( qboolean enabled );
qboolean VK_XR_SetActive( qboolean active );
qboolean VK_XR_ResolutionChanged( void );
int VK_XR_BeginFrame( refXRFrame_t *frame );
int VK_XR_EndFrame( void );
void VK_XR_Submitted( void );
int VK_XR_Status( void );
qboolean VK_XR_Haptic( int hand, float amplitude, int durationMs );
const char *VK_XR_LastError( void );
void VK_XR_Info( void );
qboolean VK_XR_Enabled( void );
uint32_t VK_XR_ApiVersion( uint32_t version );
VkResult VK_XR_CreateInstance( PFN_vkCreateInstance normal, const VkInstanceCreateInfo *info,
							   VkInstance *instance );
VkResult VK_XR_CreateDevice( PFN_vkCreateDevice normal, VkPhysicalDevice physical,
							 const VkDeviceCreateInfo *info, VkDevice *device );
VkPhysicalDevice VK_XR_PhysicalDevice( VkInstance instance );
/* targetFlags asks the runtime for extra swapchain image create flags; it may refuse them. */
void VK_XR_Bind( VkInstance instance, VkPhysicalDevice physical, VkDevice device, uint32_t queueFamily,
				 VkImageCreateFlags targetFlags );
void VK_XR_ShutdownSession( void );
void VK_XR_ShutdownInstance( void );
void VK_XR_CopyEyes( VkCommandBuffer command, VkImage source, VkFormat format, int width, int height,
					 VkImageLayout layout );
/* Valid after VK_XR_Bind. The images belong to the runtime. */
qboolean VK_XR_SwapchainImages( VkFormat *format, uint32_t *count, VkImage images[VK_XRVK_MAX_IMAGES] );
/* Index acquired this frame; valid while VK_XR_Drawing(). */
uint32_t VK_XR_AcquiredIndex( void );
/* Direct mode wrote the acquired image inside the frame's command buffer. */
void VK_XR_Rendered( void );
/* eye: 0 left, 1 right. */
qboolean VK_XR_EyeView( int eye, refdef_t *view, float fov[4] );
void VK_XR_SetupView( refdef_t *view, viewParms_t *parms );
qboolean VK_XR_Drawing( void );
void VK_XR_SetVirtualScreen( qboolean enabled, qboolean menuYawLocked, refXRFrame_t *frame );
const vrScreenGeometry_t *VK_XR_Screen( void );
void VK_XR_FloorOrigin( vec3_t origin );
void VK_XR_ScreenCaptureRect( int eyeWidth, int eyeHeight, int rect[4] );
void VK_XR_EyeMatrix( int eye, float matrix[16] );
void VK_XR_HudMatrix( int eye, float matrix[16] );
void VK_XR_ScreenMatrix( int eye, float matrix[16] );
void VK_XR_TargetSize( uint32_t *width, uint32_t *height );
/* The create flags the runtime accepted for the eye swapchain. */
VkImageCreateFlags VK_XR_TargetCreateFlags( void );
void VK_XR_SetZoom( qboolean zoomed, float *level );
float VK_XR_ScopeScaleY( void );
qboolean VK_XR_ScopeNeedsBands( void );
void VK_XR_FoveationCaps( qboolean supported );
void VK_XR_FoveationMap( vkFovMap_t *map );
#endif
