/* Optional OpenXR discovery: no loader import library or renderer dependency. */
#include "xr_loader.h"
#define XR_NO_PROTOTYPES
#include "../thirdparty/openxr/openxr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__EMSCRIPTEN__)
void *XRLoader_LoadLibrary( void ) {
	return NULL;
}
xrLoaderProc_t XRLoader_LibrarySymbol( void *lib, const char *name ) {
	(void)lib;
	(void)name;
	return NULL;
}
void XRLoader_UnloadLibrary( void *lib ) {
	(void)lib;
}
#elif defined(_WIN32)
#include <windows.h>
void *XRLoader_LoadLibrary( void ) {
	return (void *)LoadLibraryA( "openxr_loader.dll" );
}
xrLoaderProc_t XRLoader_LibrarySymbol( void *lib, const char *name ) {
	return (xrLoaderProc_t)GetProcAddress( (HMODULE)lib, name );
}
void XRLoader_UnloadLibrary( void *lib ) {
	FreeLibrary( (HMODULE)lib );
}
#else
#include <dlfcn.h>
#include <string.h>
#include <limits.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

/* A loader beside the executable comes first; neither lookup depends on the working directory. */
static void *XRLoader_OpenUnix( void ) {
#ifdef __APPLE__
	const char *name = "libopenxr_loader.dylib";
#else
	const char *name = "libopenxr_loader.so.1";
#endif
#if defined(__APPLE__) || defined(__linux__)
	char path[PATH_MAX];
	char *slash;
	void *library;
	int valid = 0;
#ifdef __APPLE__
	uint32_t size = sizeof( path );
	valid = _NSGetExecutablePath( path, &size ) == 0;
#else
	ssize_t size = readlink( "/proc/self/exe", path, sizeof( path ) - 1 );
	if ( size > 0 && size < (ssize_t)sizeof( path ) - 1 ) {
		path[size] = 0;
		valid = 1;
	}
#endif
	if ( valid && (slash = strrchr( path, '/' )) != NULL &&
		(size_t)(slash + 1 - path) + strlen( name ) < sizeof( path ) ) {
		strcpy( slash + 1, name );
		library = dlopen( path, RTLD_NOW | RTLD_LOCAL );
		if ( library )
			return library;
	}
#endif
	return dlopen( name, RTLD_NOW | RTLD_LOCAL );
}
void *XRLoader_LoadLibrary( void ) {
	return XRLoader_OpenUnix();
}
xrLoaderProc_t XRLoader_LibrarySymbol( void *lib, const char *name ) {
	return (xrLoaderProc_t)dlsym( lib, name );
}
void XRLoader_UnloadLibrary( void *lib ) {
	dlclose( lib );
}
#endif

static xrLoaderStatus_t XRLoader_Failure( xrLoaderInfo_t *info, xrLoaderStatus_t status,
									XrResult result, const char *message ) {
	info->status = status;
	info->result = result;
	snprintf( info->message, sizeof( info->message ), "%s (OpenXR result %d)", message, (int)result );
	return status;
}

static xrLoaderStatus_t XRLoader_RuntimeFailure( xrLoaderInfo_t *info, XrResult result ) {
	return XRLoader_Failure( info, result == XR_ERROR_RUNTIME_UNAVAILABLE ? XRLOADER_NO_RUNTIME : XRLOADER_PROBE_FAILED,
							result, "OpenXR runtime discovery failed" );
}

typedef struct {
	void *library;
	XrInstance instance;
	PFN_xrDestroyInstance destroy;
	PFN_xrGetSystem getSystem;
	PFN_xrGetSystemProperties systemProperties;
} xrLoaderState_t;

static xrLoaderState_t watch;

static void XRLoader_ReleaseInstance( xrLoaderState_t *st ) {
	if ( st->instance != XR_NULL_HANDLE && st->destroy )
		st->destroy( st->instance );
	st->instance = XR_NULL_HANDLE;
	st->destroy = NULL;
	st->getSystem = NULL;
	st->systemProperties = NULL;
}

static void XRLoader_Release( xrLoaderState_t *st ) {
	XRLoader_ReleaseInstance( st );
	if ( st->library )
		XRLoader_UnloadLibrary( st->library );
	memset( st, 0, sizeof( *st ) );
}

/* A runtime that may still start keeps the loader resident for cheap polls. */
static void XRLoader_Settle( xrLoaderState_t *st, xrLoaderStatus_t status ) {
	if ( status == XRLOADER_NO_RUNTIME || status == XRLOADER_PROBE_FAILED )
		XRLoader_ReleaseInstance( st );
	else
		XRLoader_Release( st );
}

static xrLoaderStatus_t XRLoader_Open( xrLoaderState_t *st, xrLoaderInfo_t *info ) {
	XrResult result = XR_SUCCESS;
	PFN_xrGetInstanceProcAddr getproc;
	PFN_xrCreateInstance create;
	PFN_xrEnumerateInstanceExtensionProperties enumerate;
	PFN_xrGetInstanceProperties properties;
	XrExtensionProperties *extensions = NULL;
	XrInstanceCreateInfo ci;
	XrInstanceProperties ip;
	uint32_t count = 0, capacity, i;
	if ( !info )
		return XRLOADER_PROBE_FAILED;
	memset( info, 0, sizeof( *info ) );
	XRLoader_Failure( info, XRLOADER_PROBE_FAILED, XR_SUCCESS, "OpenXR discovery failed" );
	if ( !st->library )
		st->library = XRLoader_LoadLibrary();
	if ( !st->library )
		return XRLoader_Failure( info, XRLOADER_NO_LOADER, XR_SUCCESS,
							"OpenXR loader could not be loaded (missing loader or dependency)" );
	getproc = (PFN_xrGetInstanceProcAddr)XRLoader_LibrarySymbol( st->library, "xrGetInstanceProcAddr" );
	/* Resolved before create, so every instance created here can be destroyed. */
	st->destroy = (PFN_xrDestroyInstance)XRLoader_LibrarySymbol( st->library, "xrDestroyInstance" );
	if ( !getproc || !st->destroy ) {
		XRLoader_Failure( info, XRLOADER_PROBE_FAILED, XR_ERROR_FUNCTION_UNSUPPORTED,
					"Missing xrGetInstanceProcAddr or xrDestroyInstance" );
		goto done;
	}
#define RESOLVE(handle, name, dest) \
	do { \
		PFN_xrVoidFunction fn = NULL; \
		result = getproc( handle, #name, &fn ); \
		if ( XR_FAILED( result ) || !fn ) { \
			XRLoader_Failure( info, XRLOADER_PROBE_FAILED, result, "Missing " #name ); \
			goto done; \
		} \
		dest = (PFN_##name)fn; \
	} while ( 0 )
	RESOLVE( XR_NULL_HANDLE, xrCreateInstance, create );
	RESOLVE( XR_NULL_HANDLE, xrEnumerateInstanceExtensionProperties, enumerate );
	result = enumerate( NULL, 0, &count, NULL );
	if ( XR_FAILED( result ) )
		goto runtime_failure;
	/* Bound allocation and do not loop if a runtime changes its extension list. */
	if ( count > 1024 ) {
		XRLoader_Failure( info, XRLOADER_PROBE_FAILED, XR_ERROR_LIMIT_REACHED, "Too many OpenXR extensions" );
		goto done;
	}
	capacity = count;
	if ( capacity ) {
		extensions = (XrExtensionProperties *)calloc( capacity, sizeof( *extensions ) );
		if ( !extensions ) {
			XRLoader_Failure( info, XRLOADER_PROBE_FAILED, XR_ERROR_OUT_OF_MEMORY,
						"OpenXR extension allocation failed" );
			goto done;
		}
		for ( i = 0; i < capacity; i++ )
			extensions[i].type = XR_TYPE_EXTENSION_PROPERTIES;
		result = enumerate( NULL, capacity, &count, extensions );
		if ( XR_FAILED( result ) )
			goto runtime_failure;
		if ( count > capacity ) {
			XRLoader_Failure( info, XRLOADER_PROBE_FAILED, XR_ERROR_SIZE_INSUFFICIENT,
						"OpenXR extension list changed" );
			goto done;
		}
		for ( i = 0; i < count; i++ ) {
			extensions[i].extensionName[XR_MAX_EXTENSION_NAME_SIZE - 1] = '\0';
			if ( !strcmp( extensions[i].extensionName, "XR_KHR_vulkan_enable" ) )
				info->vulkanEnable = 1;
			if ( !strcmp( extensions[i].extensionName, "XR_KHR_vulkan_enable2" ) )
				info->vulkanEnable2 = 1;
		}
	}
	if ( !info->vulkanEnable && !info->vulkanEnable2 ) {
		XRLoader_Failure( info, XRLOADER_NO_VULKAN, XR_ERROR_EXTENSION_NOT_PRESENT,
					"OpenXR runtime has no Vulkan binding" );
		goto done;
	}
	memset( &ci, 0, sizeof( ci ) );
	ci.type = XR_TYPE_INSTANCE_CREATE_INFO;
	strcpy( ci.applicationInfo.applicationName, "Trinity" );
	strcpy( ci.applicationInfo.engineName, "Trinity Engine" );
	/* 1.1 first; retry 1.0 unless the failure was one 1.0 would share */
	ci.applicationInfo.apiVersion = XR_MAKE_VERSION( 1, 1, 0 );
	result = create( &ci, &st->instance );
	if ( XR_FAILED( result ) && result != XR_ERROR_RUNTIME_UNAVAILABLE && result != XR_ERROR_LIMIT_REACHED &&
		 result != XR_ERROR_OUT_OF_MEMORY && result != XR_ERROR_INSTANCE_LOST ) {
		ci.applicationInfo.apiVersion = XR_MAKE_VERSION( 1, 0, 0 );
		result = create( &ci, &st->instance );
	}
	if ( XR_FAILED( result ) )
		goto runtime_failure;
	RESOLVE( st->instance, xrGetInstanceProperties, properties );
	RESOLVE( st->instance, xrGetSystem, st->getSystem );
	RESOLVE( st->instance, xrGetSystemProperties, st->systemProperties );
	memset( &ip, 0, sizeof( ip ) );
	ip.type = XR_TYPE_INSTANCE_PROPERTIES;
	result = properties( st->instance, &ip );
	if ( XR_FAILED( result ) )
		goto runtime_failure;
	memcpy( info->runtimeName, ip.runtimeName, sizeof( info->runtimeName ) );
	info->runtimeName[sizeof( info->runtimeName ) - 1] = '\0';
	info->runtimeVersion = ip.runtimeVersion;
	info->status = XRLOADER_AVAILABLE;
	goto done;
runtime_failure:
	XRLoader_RuntimeFailure( info, result );
done:
	free( extensions );
	if ( info->status != XRLOADER_AVAILABLE )
		XRLoader_Settle( st, info->status );
	return info->status;
#undef RESOLVE
}

static xrLoaderStatus_t XRLoader_QuerySystem( xrLoaderState_t *st, xrLoaderInfo_t *info ) {
	XrSystemId system = XR_NULL_SYSTEM_ID;
	XrResult result;
	XrSystemGetInfo si;
	XrSystemProperties sp;
	memset( &si, 0, sizeof( si ) );
	si.type = XR_TYPE_SYSTEM_GET_INFO;
	si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	result = st->getSystem( st->instance, &si, &system );
	if ( result == XR_ERROR_FORM_FACTOR_UNAVAILABLE || result == XR_ERROR_FORM_FACTOR_UNSUPPORTED )
		return XRLoader_Failure( info, XRLOADER_NO_HEADSET, result, "No OpenXR headset is available" );
	if ( XR_FAILED( result ) )
		return XRLoader_RuntimeFailure( info, result );
	memset( &sp, 0, sizeof( sp ) );
	sp.type = XR_TYPE_SYSTEM_PROPERTIES;
	result = st->systemProperties( st->instance, system, &sp );
	if ( XR_FAILED( result ) )
		return XRLoader_RuntimeFailure( info, result );
	memcpy( info->systemName, sp.systemName, sizeof( info->systemName ) );
	info->systemName[sizeof( info->systemName ) - 1] = '\0';
	info->status = XRLOADER_AVAILABLE;
	info->result = XR_SUCCESS;
	snprintf( info->message, sizeof( info->message ), "OpenXR headset available" );
	return info->status;
}

xrLoaderStatus_t XRLoader_WatchBegin( xrLoaderInfo_t *info ) {
	XRLoader_WatchEnd();
	return XRLoader_WatchPoll( info );
}

xrLoaderStatus_t XRLoader_WatchPoll( xrLoaderInfo_t *info ) {
	if ( !watch.instance && XRLoader_Open( &watch, info ) != XRLOADER_AVAILABLE )
		return info->status;
	XRLoader_QuerySystem( &watch, info );
	if ( info->status != XRLOADER_NO_HEADSET )
		XRLoader_Settle( &watch, info->status );
	return info->status;
}

void XRLoader_WatchEnd( void ) {
	XRLoader_Release( &watch );
}
