/* Meta's controller models (XR_FB_render_model), for a runtime without the EXT pair. No engine or Vulkan dependencies. */
#ifndef VK_XR_META_H
#define VK_XR_META_H
#include "vk_xr_models.h"
#include "../vrcommon/vr_input_types.h"

/* Resolves the extension's functions; loading waits for the first frame that shows the models. log may be NULL. */
void VK_XRMeta_Init( XrInstance, XrSession, PFN_xrGetInstanceProcAddr, vrModelAlloc_t, vrModelFree_t,
					 void ( *log )( const char *line ) );
/* Returns 1 on the frame that fetched the models. */
int VK_XRMeta_Update( XrSpace base, XrTime time, int shown, const XrSpace grips[2], const clXRHandInput_t *hands );
const vkXRModel_t *VK_XRMeta_Model( int hand );
/* Before the session is destroyed. */
void VK_XRMeta_Shutdown( void );
#endif
