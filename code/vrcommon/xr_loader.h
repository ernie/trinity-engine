/* Optional, graphics-independent OpenXR discovery. */
#ifndef VRCOMMON_XR_LOADER_H
#define VRCOMMON_XR_LOADER_H

#include <stdint.h>

typedef enum {
	XRLOADER_AVAILABLE,
	XRLOADER_NO_LOADER,
	XRLOADER_NO_RUNTIME,
	XRLOADER_NO_HEADSET,
	XRLOADER_NO_VULKAN,
	XRLOADER_PROBE_FAILED
} xrLoaderStatus_t;

typedef struct {
	xrLoaderStatus_t status;
	int result; /* Raw XrResult when an OpenXR call failed. */
	uint64_t runtimeVersion;
	int vulkanEnable, vulkanEnable2;
	char runtimeName[128];
	char systemName[256];
	char message[256];
} xrLoaderInfo_t;

typedef void (*xrLoaderProc_t)(void);
#ifdef __ANDROID__
/* The JavaVM and activity that the Android loader and every instance need; set before any OpenXR call. */
void XRLoader_SetAndroidContext( void *vm, void *activity );
#endif
/* What every XrInstanceCreateInfo carries for this platform: a next chain and one required extension, or NULL. */
const void *XRLoader_InstanceNext( void );
const char *XRLoader_PlatformExtension( void );
void *XRLoader_LoadLibrary( void );
xrLoaderProc_t XRLoader_LibrarySymbol( void *library, const char *name );
void XRLoader_UnloadLibrary( void *library );

/* Every exit from the headset wait must call XRLoader_WatchEnd before any renderer XR init. */
xrLoaderStatus_t XRLoader_WatchBegin( xrLoaderInfo_t *info );
xrLoaderStatus_t XRLoader_WatchPoll( xrLoaderInfo_t *info );
void XRLoader_WatchEnd( void );

#endif
