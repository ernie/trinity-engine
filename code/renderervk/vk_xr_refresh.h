/* Optional display refresh control; owns no runtime handles. */
#ifndef VK_XR_REFRESH_H
#define VK_XR_REFRESH_H
#include <stddef.h>
#ifndef XR_NO_PROTOTYPES
#define XR_NO_PROTOTYPES
#endif
#include "../thirdparty/openxr/openxr.h"
typedef struct {
	XrSession session;
	PFN_xrEnumerateDisplayRefreshRatesFB enumerate;
	PFN_xrGetDisplayRefreshRateFB get;
	PFN_xrRequestDisplayRefreshRateFB request;
	float rates[32], current;
	uint32_t count;
	int available;
} vkXRRefresh_t;
/* enabled is the live owner's enabled extension flag. Failures are optional:
 * caller logs them and continues using the runtime's current refresh rate. */
XrResult VK_XRRefresh_Init( vkXRRefresh_t *, XrInstance, XrSession, PFN_xrGetInstanceProcAddr, int enabled );
XrResult VK_XRRefresh_Request( vkXRRefresh_t *, float hz );
void VK_XRRefresh_FormatRates( const vkXRRefresh_t *, char *, size_t );
#endif
