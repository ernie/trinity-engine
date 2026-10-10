/* Headsets give no desktop Surface (PICO gives a VR activity none), so the mirror swapchain renders to an
 * offscreen image reader it never presents. */
#include <dlfcn.h>
#include <android/hardware_buffer.h>
#include <media/NdkImageReader.h>

#include <SDL.h>

#include "../client/client.h"
#include "../client/cl_vr.h"
#include "../client/cl_renderer_recovery.h"
#include "../vrcommon/vr_cvars.h"
#include "../sdl/sdl_glw.h"

glwstate_t glw_state;
SDL_Window *SDL_window = NULL;
cvar_t *in_nograb;

static void *vulkanLibrary;
static PFN_vkGetInstanceProcAddr getInstanceProcAddr;
/* kept for the process: the renderer's surface may outlive a VKimp_Shutdown */
static AImageReader *mirrorReader;

/* vulkan_android.h is not among the engine's Vulkan headers */
typedef struct {
	VkStructureType sType;
	const void *pNext;
	VkFlags flags;
	ANativeWindow *window;
} androidSurfaceCreateInfo_t;
typedef VkResult ( VKAPI_PTR *createAndroidSurface_t )( VkInstance instance, const androidSurfaceCreateInfo_t *info,
	const VkAllocationCallbacks *allocator, VkSurfaceKHR *surface );
/* API 26, while the APK installs from API 25 */
typedef media_status_t ( *imageReaderNewWithUsage_t )( int32_t width, int32_t height, int32_t format, uint64_t usage,
	int32_t maxImages, AImageReader **reader );

void GLimp_InitGamma( glconfig_t *config ) {
	config->deviceSupportsGamma = qfalse;
}

void GLimp_SetGamma( unsigned char red[256], unsigned char green[256], unsigned char blue[256] ) {
}

void GLW_RestoreGamma( void ) {
}

void GLW_HideFullscreenWindow( void ) {
}

void VKimp_Init( glconfig_t *config ) {
	Com_DPrintf( "VKimp_Init()\n" );

	in_nograb = Cvar_Get( "in_nograb", "0", CVAR_ARCHIVE );
	r_swapInterval = Cvar_Get( "r_swapInterval", "0", CVAR_ARCHIVE | CVAR_LATCH );

	glw_state.config = config;
	VR_InitMirrorCvars();

	if ( !SDL_WasInit( SDL_INIT_VIDEO ) && SDL_Init( SDL_INIT_VIDEO ) != 0 ) {
		CL_RendererError( ERR_FATAL, "VKimp_Init() - SDL_Init( SDL_INIT_VIDEO ) failed: %s", SDL_GetError() );
		return;
	}

	if ( !vulkanLibrary ) {
		vulkanLibrary = dlopen( "libvulkan.so", RTLD_NOW | RTLD_LOCAL );
		if ( vulkanLibrary )
			getInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym( vulkanLibrary, "vkGetInstanceProcAddr" );
	}
	if ( !getInstanceProcAddr ) {
		CL_RendererError( ERR_FATAL, "VKimp_Init: vkGetInstanceProcAddr is unavailable" );
		return;
	}

	config->vidWidth = vr_mirrorWidth->integer;
	config->vidHeight = vr_mirrorHeight->integer;
	config->windowAspect = (float)config->vidWidth / config->vidHeight;
	config->isFullscreen = qfalse;
	config->colorBits = 24;
	config->depthBits = 24;
	config->stencilBits = cl_stencilbits->integer;
	config->driverType = GLDRV_ICD;
	config->hardwareType = GLHW_GENERIC;
	glw_state.isFullscreen = qfalse;
	glw_state.desktop_width = glw_state.window_width = config->vidWidth;
	glw_state.desktop_height = glw_state.window_height = config->vidHeight;
	glw_state.monitorCount = 1;
	/* the state a hidden mirror window reports */
	gw_active = qfalse;
	gw_minimized = qtrue;

	IN_Init();
	HandleEvents();
	Key_ClearStates();
}

void *VK_GetInstanceProcAddr( VkInstance instance, const char *name ) {
	return getInstanceProcAddr( instance, name );
}

qboolean VK_CreateSurface( VkInstance instance, VkSurfaceKHR *surface ) {
	createAndroidSurface_t create = (createAndroidSurface_t)getInstanceProcAddr( instance, "vkCreateAndroidSurfaceKHR" );
	androidSurfaceCreateInfo_t info;
	ANativeWindow *window = NULL;

	if ( !create )
		return qfalse;
	if ( !mirrorReader ) {
		imageReaderNewWithUsage_t newWithUsage =
			(imageReaderNewWithUsage_t)dlsym( RTLD_DEFAULT, "AImageReader_newWithUsage" );
		if ( !newWithUsage || newWithUsage( glw_state.window_width, glw_state.window_height, AIMAGE_FORMAT_RGBA_8888,
				AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, 3, &mirrorReader ) != AMEDIA_OK ) {
			mirrorReader = NULL;
			return qfalse;
		}
	}
	if ( AImageReader_getWindow( mirrorReader, &window ) != AMEDIA_OK || !window )
		return qfalse;

	Com_Memset( &info, 0, sizeof( info ) );
	info.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
	info.window = window;
	return create( instance, &info, NULL, surface ) == VK_SUCCESS ? qtrue : qfalse;
}

float VK_ConfigureHDR( qboolean enable ) {
	(void)enable;
	return 1.0f;
}

void VKimp_Shutdown( qboolean unloadDLL ) {
	IN_Shutdown();
	if ( unloadDLL )
		SDL_QuitSubSystem( SDL_INIT_VIDEO );
}

char *Sys_GetClipboardData( void ) {
	char *data = NULL;
	char *cliptext;

	if ( ( cliptext = SDL_GetClipboardText() ) != NULL ) {
		if ( cliptext[0] != '\0' ) {
			size_t bufsize = strlen( cliptext ) + 1;

			data = Z_Malloc( bufsize );
			Q_strncpyz( data, cliptext, bufsize );
			strtok( data, "\n\r\b" );
		}
		SDL_free( cliptext );
	}
	return data;
}

void Sys_SetClipboardBitmap( const byte *bitmap, int length ) {
}
