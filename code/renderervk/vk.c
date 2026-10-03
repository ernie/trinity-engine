#ifdef _WIN32
// DisplayConfig OS-HDR query needs Win7 headers; set before tr_local.h, which pins
// _WIN32_WINNT to 0x0501 via q_platform.h and would hide these types on MSVC/old MinGW.
#undef _WIN32_WINNT
#undef WINVER
#define _WIN32_WINNT 0x0601
#define WINVER 0x0601
#endif

#include "tr_local.h"
#include "vk_hud_coverage.h"
#include "../vrcommon/vr_defaults.h"
#include <stdint.h>
#include <stdlib.h>

/* Subtraction is modulo the selected queue's valid timestamp width. A
 * measured frame must be shorter than one counter wrap period. */
static uint64_t VK_GPUTimeDelta( uint64_t start, uint64_t end, unsigned bits ) {
	uint64_t mask = bits == 64 ? UINT64_MAX : (((uint64_t)1 << bits) - 1);
	return (end - start) & mask;
}
static int VK_GPUTimeTicks( const uint64_t *q, unsigned bits, int mirror, int hud, double *ticks ) {
	double mainTicks, mirrorTicks = 0, hudTicks = 0;
	if ( !bits || bits > 64 || !q[1] || !q[3] )
		return 0;
	mainTicks = (double)VK_GPUTimeDelta( q[0], q[2], bits );
	if ( mirror ) {
		if ( !q[5] || !q[7] )
			return 0;
		mirrorTicks = (double)VK_GPUTimeDelta( q[4], q[6], bits );
		if ( mirrorTicks > mainTicks )
			return 0;
	}
	if ( hud ) {
		if ( !q[9] || !q[11] )
			return 0;
		hudTicks = (double)VK_GPUTimeDelta( q[8], q[10], bits );
	}
	*ticks = mainTicks - mirrorTicks + hudTicks;
	return 1;
}
static int VK_GPUTimeCompare( const void *a, const void *b ) {
	float x = *(const float *)a, y = *(const float *)b;
	return (x > y) - (x < y);
}
static int VK_GPUTimePercentile( int count, int percent ) {
	return count * percent / 100;
}

#include "vk_xr.h"
#include "vk.h"
#include "vk_foveation.h"

static PFN_vkCreateQueryPool qvkCreateQueryPool;
static PFN_vkDestroyQueryPool qvkDestroyQueryPool;
static PFN_vkGetQueryPoolResults qvkGetQueryPoolResults;
static PFN_vkCmdResetQueryPool qvkCmdResetQueryPool;
static PFN_vkCmdWriteTimestamp qvkCmdWriteTimestamp;
static VkQueryPool gpuTimePool;
static float gpuTimeSamples[4096];
static int gpuTimeCount, gpuTimeWindow;
static unsigned gpuTimeGeneration;
#define VK_GPU_TIME_QUERIES 10
/* Frame-order stamps after frame top; each segment ends at its query. */
static const struct { unsigned query; const char *name; } gpuSegments[] = {
	{ 6, "scene" }, { 8, "bloom blur" }, { 2, "post" },
	{ 3, "mirror" }, { 9, "eye output" }, { 1, "end" },
};
static float gpuSegmentSamples[ARRAY_LEN( gpuSegments )][4096];
static int gpuSegmentCount[ARRAY_LEN( gpuSegments )];
static void vk_gpu_time_stamp( VkCommandBuffer command, unsigned query );

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef __APPLE__
extern float Sys_MacOS_CurrentEDRHeadroom( void ); // sdl_macos_hdr.m
#endif

#if defined (_DEBUG)
#if defined (_WIN32)
#define USE_VK_VALIDATION
#include <windows.h> // for win32 debug callback
#endif
#endif

static int vkSamples = VK_SAMPLE_COUNT_1_BIT;
static int vkMaxSamples = VK_SAMPLE_COUNT_1_BIT;

static VkInstance vk_instance = VK_NULL_HANDLE;
static VkSurfaceKHR vk_surface = VK_NULL_HANDLE;

#ifndef NDEBUG
VkDebugReportCallbackEXT vk_debug_callback = VK_NULL_HANDLE;
#endif

//
// Vulkan API functions used by the renderer.
//
static PFN_vkCreateInstance								qvkCreateInstance;
static PFN_vkEnumerateInstanceExtensionProperties		qvkEnumerateInstanceExtensionProperties;

static PFN_vkCreateDevice								qvkCreateDevice;
static PFN_vkDestroyInstance							qvkDestroyInstance;
static PFN_vkEnumerateDeviceExtensionProperties			qvkEnumerateDeviceExtensionProperties;
static PFN_vkEnumeratePhysicalDevices					qvkEnumeratePhysicalDevices;
static PFN_vkGetDeviceProcAddr							qvkGetDeviceProcAddr;
static PFN_vkGetPhysicalDeviceFeatures					qvkGetPhysicalDeviceFeatures;
static PFN_vkGetPhysicalDeviceFormatProperties			qvkGetPhysicalDeviceFormatProperties;
static PFN_vkGetPhysicalDeviceMemoryProperties			qvkGetPhysicalDeviceMemoryProperties;
static PFN_vkGetPhysicalDeviceProperties				qvkGetPhysicalDeviceProperties;
static PFN_vkGetPhysicalDeviceQueueFamilyProperties		qvkGetPhysicalDeviceQueueFamilyProperties;
static PFN_vkDestroySurfaceKHR							qvkDestroySurfaceKHR;
static PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR	qvkGetPhysicalDeviceSurfaceCapabilitiesKHR;
static PFN_vkGetPhysicalDeviceSurfaceFormatsKHR			qvkGetPhysicalDeviceSurfaceFormatsKHR;
static PFN_vkGetPhysicalDeviceSurfacePresentModesKHR	qvkGetPhysicalDeviceSurfacePresentModesKHR;
static PFN_vkGetPhysicalDeviceSurfaceSupportKHR			qvkGetPhysicalDeviceSurfaceSupportKHR;
#ifdef USE_VK_VALIDATION
static PFN_vkCreateDebugReportCallbackEXT				qvkCreateDebugReportCallbackEXT;
static PFN_vkDestroyDebugReportCallbackEXT				qvkDestroyDebugReportCallbackEXT;
#endif
static PFN_vkAllocateCommandBuffers						qvkAllocateCommandBuffers;
static PFN_vkAllocateDescriptorSets						qvkAllocateDescriptorSets;
static PFN_vkAllocateMemory								qvkAllocateMemory;
static PFN_vkBeginCommandBuffer							qvkBeginCommandBuffer;
static PFN_vkBindBufferMemory							qvkBindBufferMemory;
static PFN_vkBindImageMemory							qvkBindImageMemory;
static PFN_vkCmdBeginRenderPass							qvkCmdBeginRenderPass;
static PFN_vkCmdBindDescriptorSets						qvkCmdBindDescriptorSets;
static PFN_vkCmdBindIndexBuffer							qvkCmdBindIndexBuffer;
static PFN_vkCmdBindPipeline							qvkCmdBindPipeline;
static PFN_vkCmdBindVertexBuffers						qvkCmdBindVertexBuffers;
static PFN_vkCmdBlitImage								qvkCmdBlitImage;
static PFN_vkCmdClearAttachments						qvkCmdClearAttachments;
static PFN_vkCmdCopyBuffer								qvkCmdCopyBuffer;
static PFN_vkCmdCopyBufferToImage						qvkCmdCopyBufferToImage;
static PFN_vkCmdCopyImage								qvkCmdCopyImage;
static PFN_vkCmdDraw									qvkCmdDraw;
static PFN_vkCmdDrawIndexed								qvkCmdDrawIndexed;
static PFN_vkCmdEndRenderPass							qvkCmdEndRenderPass;
static PFN_vkCmdNextSubpass								qvkCmdNextSubpass;
static PFN_vkCmdPipelineBarrier							qvkCmdPipelineBarrier;
static PFN_vkCmdPushConstants							qvkCmdPushConstants;
static PFN_vkCmdSetDepthBias							qvkCmdSetDepthBias;
static PFN_vkCmdSetScissor								qvkCmdSetScissor;
static PFN_vkCmdSetViewport								qvkCmdSetViewport;
static PFN_vkCreateBuffer								qvkCreateBuffer;
static PFN_vkCreateCommandPool							qvkCreateCommandPool;
static PFN_vkCreateDescriptorPool						qvkCreateDescriptorPool;
static PFN_vkCreateDescriptorSetLayout					qvkCreateDescriptorSetLayout;
static PFN_vkCreateFence								qvkCreateFence;
static PFN_vkCreateFramebuffer							qvkCreateFramebuffer;
static PFN_vkCreateGraphicsPipelines					qvkCreateGraphicsPipelines;
static PFN_vkCreateImage								qvkCreateImage;
static PFN_vkCreateImageView							qvkCreateImageView;
static PFN_vkCreatePipelineLayout						qvkCreatePipelineLayout;
static PFN_vkCreatePipelineCache						qvkCreatePipelineCache;
static PFN_vkCreateRenderPass							qvkCreateRenderPass;
static PFN_vkCreateSampler								qvkCreateSampler;
static PFN_vkCreateSemaphore							qvkCreateSemaphore;
static PFN_vkCreateShaderModule							qvkCreateShaderModule;
static PFN_vkDestroyBuffer								qvkDestroyBuffer;
static PFN_vkDestroyCommandPool							qvkDestroyCommandPool;
static PFN_vkDestroyDescriptorPool						qvkDestroyDescriptorPool;
static PFN_vkDestroyDescriptorSetLayout					qvkDestroyDescriptorSetLayout;
static PFN_vkDestroyDevice								qvkDestroyDevice;
static PFN_vkDestroyFence								qvkDestroyFence;
static PFN_vkDestroyFramebuffer							qvkDestroyFramebuffer;
static PFN_vkDestroyImage								qvkDestroyImage;
static PFN_vkDestroyImageView							qvkDestroyImageView;
static PFN_vkDestroyPipeline							qvkDestroyPipeline;
static PFN_vkDestroyPipelineCache						qvkDestroyPipelineCache;
static PFN_vkDestroyPipelineLayout						qvkDestroyPipelineLayout;
static PFN_vkDestroyRenderPass							qvkDestroyRenderPass;
static PFN_vkDestroySampler								qvkDestroySampler;
static PFN_vkDestroySemaphore							qvkDestroySemaphore;
static PFN_vkDestroyShaderModule						qvkDestroyShaderModule;
static PFN_vkDeviceWaitIdle								qvkDeviceWaitIdle;
static PFN_vkEndCommandBuffer							qvkEndCommandBuffer;
static PFN_vkFlushMappedMemoryRanges					qvkFlushMappedMemoryRanges;
static PFN_vkFreeCommandBuffers							qvkFreeCommandBuffers;
static PFN_vkFreeDescriptorSets							qvkFreeDescriptorSets;
static PFN_vkFreeMemory									qvkFreeMemory;
static PFN_vkGetBufferMemoryRequirements				qvkGetBufferMemoryRequirements;
static PFN_vkGetDeviceQueue								qvkGetDeviceQueue;
static PFN_vkGetImageMemoryRequirements					qvkGetImageMemoryRequirements;
static PFN_vkGetImageSubresourceLayout					qvkGetImageSubresourceLayout;
static PFN_vkInvalidateMappedMemoryRanges				qvkInvalidateMappedMemoryRanges;
static PFN_vkMapMemory									qvkMapMemory;
static PFN_vkQueueSubmit								qvkQueueSubmit;
static PFN_vkQueueWaitIdle								qvkQueueWaitIdle;
static PFN_vkResetCommandBuffer							qvkResetCommandBuffer;
static PFN_vkResetDescriptorPool						qvkResetDescriptorPool;
static PFN_vkResetFences								qvkResetFences;
static PFN_vkUnmapMemory								qvkUnmapMemory;
static PFN_vkUpdateDescriptorSets						qvkUpdateDescriptorSets;
static PFN_vkWaitForFences								qvkWaitForFences;
static PFN_vkAcquireNextImageKHR						qvkAcquireNextImageKHR;
static PFN_vkCreateSwapchainKHR							qvkCreateSwapchainKHR;
static PFN_vkDestroySwapchainKHR						qvkDestroySwapchainKHR;
static PFN_vkGetSwapchainImagesKHR						qvkGetSwapchainImagesKHR;
static PFN_vkQueuePresentKHR							qvkQueuePresentKHR;

static PFN_vkGetBufferMemoryRequirements2KHR			qvkGetBufferMemoryRequirements2KHR;
static PFN_vkGetImageMemoryRequirements2KHR				qvkGetImageMemoryRequirements2KHR;

static PFN_vkDebugMarkerSetObjectNameEXT				qvkDebugMarkerSetObjectNameEXT;

////////////////////////////////////////////////////////////////////////////

// forward declaration
static PFN_vkCmdClearColorImage qvkCmdClearColorImage;
VkPipeline create_pipeline( const Vk_Pipeline_Def *def, renderPass_t renderPassIndex, uint32_t def_index );

static uint32_t find_memory_type( uint32_t memory_type_bits, VkMemoryPropertyFlags properties ) {
	VkPhysicalDeviceMemoryProperties memory_properties;
	uint32_t i;

	qvkGetPhysicalDeviceMemoryProperties( vk.physical_device, &memory_properties );

	for ( i = 0; i < memory_properties.memoryTypeCount; i++ ) {
		if ((memory_type_bits & (1 << i)) != 0 &&
			(memory_properties.memoryTypes[i].propertyFlags & properties) == properties) {
			return i;
		}
	}
	ri.Error( ERR_FATAL, "Vulkan: failed to find matching memory type with requested properties" );
	return ~0U;
}


static uint32_t find_memory_type2( uint32_t memory_type_bits, VkMemoryPropertyFlags properties, VkMemoryPropertyFlags *outprops ) {
	VkPhysicalDeviceMemoryProperties memory_properties;
	uint32_t i;

	qvkGetPhysicalDeviceMemoryProperties( vk.physical_device, &memory_properties );

	for ( i = 0; i < memory_properties.memoryTypeCount; i++ ) {
		if ( (memory_type_bits & (1 << i)) != 0 && (memory_properties.memoryTypes[i].propertyFlags & properties) == properties ) {
			if ( outprops ) {
				*outprops = memory_properties.memoryTypes[i].propertyFlags;
			}
			return i;
		}
	}

	return ~0U;
}


static const char *pmode_to_str( VkPresentModeKHR mode )
{
	static char buf[32];

	switch ( mode ) {
		case VK_PRESENT_MODE_IMMEDIATE_KHR: return "IMMEDIATE";
		case VK_PRESENT_MODE_MAILBOX_KHR: return "MAILBOX";
		case VK_PRESENT_MODE_FIFO_KHR: return "FIFO";
		case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED";
		case VK_PRESENT_MODE_FIFO_LATEST_READY_EXT: return "FIFO_LATEST_READY";
		default: sprintf( buf, "mode#%x", mode ); return buf;
	};
}


#define CASE_STR(x) case (x): return #x

const char *vk_format_string( VkFormat format )
{
	static char buf[16];

	switch ( format ) {
		// color formats
		CASE_STR( VK_FORMAT_R5G5B5A1_UNORM_PACK16 );
		CASE_STR( VK_FORMAT_B5G5R5A1_UNORM_PACK16 );
		CASE_STR( VK_FORMAT_R5G6B5_UNORM_PACK16 );
		CASE_STR( VK_FORMAT_B5G6R5_UNORM_PACK16 );
		CASE_STR( VK_FORMAT_B8G8R8A8_SRGB );
		CASE_STR( VK_FORMAT_R8G8B8A8_SRGB );
		CASE_STR( VK_FORMAT_B8G8R8A8_SNORM );
		CASE_STR( VK_FORMAT_R8G8B8A8_SNORM );
		CASE_STR( VK_FORMAT_B8G8R8A8_UNORM );
		CASE_STR( VK_FORMAT_R8G8B8A8_UNORM );
		CASE_STR( VK_FORMAT_B4G4R4A4_UNORM_PACK16 );
		CASE_STR( VK_FORMAT_R4G4B4A4_UNORM_PACK16 );
		CASE_STR( VK_FORMAT_R16G16B16A16_UNORM );
		CASE_STR( VK_FORMAT_A2B10G10R10_UNORM_PACK32 );
		CASE_STR( VK_FORMAT_A2R10G10B10_UNORM_PACK32 );
		CASE_STR( VK_FORMAT_B10G11R11_UFLOAT_PACK32 );
		// depth formats
		CASE_STR( VK_FORMAT_D16_UNORM );
		CASE_STR( VK_FORMAT_D16_UNORM_S8_UINT );
		CASE_STR( VK_FORMAT_X8_D24_UNORM_PACK32 );
		CASE_STR( VK_FORMAT_D24_UNORM_S8_UINT );
		CASE_STR( VK_FORMAT_D32_SFLOAT );
		CASE_STR( VK_FORMAT_D32_SFLOAT_S8_UINT );
	default:
		Com_sprintf( buf, sizeof( buf ), "#%i", format );
		return buf;
	}
}


static const char *vk_result_string( VkResult code ) {
	static char buffer[32];

	switch ( code ) {
		CASE_STR( VK_SUCCESS );
		CASE_STR( VK_NOT_READY );
		CASE_STR( VK_TIMEOUT );
		CASE_STR( VK_EVENT_SET );
		CASE_STR( VK_EVENT_RESET );
		CASE_STR( VK_INCOMPLETE );
		CASE_STR( VK_ERROR_OUT_OF_HOST_MEMORY );
		CASE_STR( VK_ERROR_OUT_OF_DEVICE_MEMORY );
		CASE_STR( VK_ERROR_INITIALIZATION_FAILED );
		CASE_STR( VK_ERROR_DEVICE_LOST );
		CASE_STR( VK_ERROR_MEMORY_MAP_FAILED );
		CASE_STR( VK_ERROR_LAYER_NOT_PRESENT );
		CASE_STR( VK_ERROR_EXTENSION_NOT_PRESENT );
		CASE_STR( VK_ERROR_FEATURE_NOT_PRESENT );
		CASE_STR( VK_ERROR_INCOMPATIBLE_DRIVER );
		CASE_STR( VK_ERROR_TOO_MANY_OBJECTS );
		CASE_STR( VK_ERROR_FORMAT_NOT_SUPPORTED );
		CASE_STR( VK_ERROR_FRAGMENTED_POOL );
		CASE_STR( VK_ERROR_UNKNOWN );
		CASE_STR( VK_ERROR_OUT_OF_POOL_MEMORY );
		CASE_STR( VK_ERROR_INVALID_EXTERNAL_HANDLE );
		CASE_STR( VK_ERROR_FRAGMENTATION );
		CASE_STR( VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS );
		CASE_STR( VK_ERROR_SURFACE_LOST_KHR );
		CASE_STR( VK_ERROR_NATIVE_WINDOW_IN_USE_KHR );
		CASE_STR( VK_SUBOPTIMAL_KHR );
		CASE_STR( VK_ERROR_OUT_OF_DATE_KHR );
		CASE_STR( VK_ERROR_INCOMPATIBLE_DISPLAY_KHR );
		CASE_STR( VK_ERROR_VALIDATION_FAILED_EXT );
		CASE_STR( VK_ERROR_INVALID_SHADER_NV );
		CASE_STR( VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT );
		CASE_STR( VK_ERROR_NOT_PERMITTED_EXT );
		CASE_STR( VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT );
		CASE_STR( VK_THREAD_IDLE_KHR );
		CASE_STR( VK_THREAD_DONE_KHR );
		CASE_STR( VK_OPERATION_DEFERRED_KHR );
		CASE_STR( VK_OPERATION_NOT_DEFERRED_KHR );
		CASE_STR( VK_PIPELINE_COMPILE_REQUIRED_EXT );
	default:
		sprintf( buffer, "code %i", code );
		return buffer;
	}
}
#undef CASE_STR

#define VK_CHECK( function_call ) { \
	VkResult res = function_call; \
	if ( res < 0 ) { \
		ri.Error( ERR_FATAL, "Vulkan: %s returned %s", #function_call, vk_result_string( res ) ); \
	} \
}

/* FOVEATION BRIDGE */
static vkFovCaps_t vk_foveation_caps;
static vkFovResources_t vk_foveation;
static PFN_vkCreateRenderPass2 vk_foveation_create_pass;
static PFN_vkCmdSetFragmentShadingRateKHR vk_foveation_set_rate;
static uint32_t vk_instance_api;
static uint64_t vk_foveation_serial;
static VkCommandBuffer vk_foveation_rate_command;
static qboolean vk_foveation_rate_attachment;
static cvar_t *r_foveationDebugCvar, *vr_mirrorEnabled, *vr_desktopContentType, *vr_desktopContentFit, *vr_desktopMenuStyle;

/* Density maps: the stereo scene pass carries the map, and offsets slide it onto the gaze as the pass ends. */
static PFN_vkCmdEndRenderPass2 vk_fdm_end_pass;
static PFN_vkGetFramebufferTilePropertiesQCOM vk_fdm_tile_properties;
// what a map is drawn from, less the HUD carve
typedef struct {
	int strength, eyeTracked;
	qboolean scope;
	float radius[2], display[2][4];
} vkFdmDrawn_t;
static struct {
	vkFdmGeometry_t geometry;
	vkFdmDrawn_t drawn; // the uploaded map's
	int32_t ref[2][2], offset[2][2];
	float center[2][2];
	int strength;
	float hud[2][4]; // last frame's HUD quad per eye, GL-style NDC
	qboolean hudValid[2];
	qboolean passOpen; // the stereo scene pass, which carries the map, is recording
	VkPipelineLayout debugLayout;
} vk_fdm;

static qboolean vk_fdm_active( void ) {
	return vk_foveation.image && vk_foveation_caps.backend == VK_FOV_BACKEND_FDM;
}

static qboolean vk_fdm_offsets_supported( void ) {
	return vk_foveation_caps.backend == VK_FOV_BACKEND_FDM && vk_foveation_caps.fdmOffset && vk_fdm_end_pass &&
		   vk.multiview;
}

// direct mode draws into the runtime's swapchain, which offsets need flagged too
static qboolean vk_fdm_offsets( void ) {
	return vk_fdm_offsets_supported() &&
		   ( vk.fboActive ||
			 ( VK_XR_TargetCreateFlags() & VK_IMAGE_CREATE_FRAGMENT_DENSITY_MAP_OFFSET_BIT_QCOM ) );
}

static VkImageCreateFlags vk_fdm_attachment_flags( void ) {
	return vk_fdm_offsets() ? VK_IMAGE_CREATE_FRAGMENT_DENSITY_MAP_OFFSET_BIT_QCOM : 0;
}

static void vk_foveation_load_functions( void ) {
	vk_fdm_end_pass = NULL;
	vk_fdm_tile_properties = NULL;
	if ( vk_foveation_caps.backend != VK_FOV_BACKEND_FDM )
		return;
	if ( vk_foveation_caps.fdmOffset ) {
		vk_fdm_end_pass = (PFN_vkCmdEndRenderPass2)qvkGetDeviceProcAddr( vk.device, "vkCmdEndRenderPass2" );
		if ( !vk_fdm_end_pass )
			vk_fdm_end_pass = (PFN_vkCmdEndRenderPass2)qvkGetDeviceProcAddr( vk.device, "vkCmdEndRenderPass2KHR" );
	}
	if ( vk_foveation_caps.tileProperties )
		vk_fdm_tile_properties = (PFN_vkGetFramebufferTilePropertiesQCOM)qvkGetDeviceProcAddr(
			vk.device, "vkGetFramebufferTilePropertiesQCOM" );
}

static void vk_foveation_invalidate_rate( void ) {
	vk_foveation_rate_command = VK_NULL_HANDLE;
}

static void vk_foveation_rate( qboolean attachment ) {
	VkExtent2D size = {1, 1};
	VkFragmentShadingRateCombinerOpKHR ops[2] = {VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR,
												 attachment ? VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR
															: VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR};
	if ( vk_foveation_caps.backend != VK_FOV_BACKEND_SHADING_RATE || !vk_foveation_set_rate ) {
		return;
	}
	if ( vk_foveation_rate_command == vk.cmd->command_buffer && vk_foveation_rate_attachment == attachment ) {
		return;
	}
	vk_foveation_set_rate( vk.cmd->command_buffer, &size, ops );
	vk_foveation_rate_command = vk.cmd->command_buffer;
	vk_foveation_rate_attachment = attachment;
}

static VkCommandBuffer begin_command_buffer( void );
static void end_command_buffer( VkCommandBuffer command_buffer, const char *location );
static void vk_set_object_name( uint64_t obj, const char *objName, VkDebugReportObjectTypeEXT objType );

static void vk_fdm_create( void ) {
	VkPhysicalDeviceMemoryProperties memory;
	VkCommandBuffer command;
	VkResult result;
	qvkGetPhysicalDeviceMemoryProperties( vk.physical_device, &memory );
	result = VK_FovCreate( &vk_foveation, vk.device, qvkGetDeviceProcAddr, &memory, &vk_foveation_caps,
						   vk.sceneWidth, vk.sceneHeight, NUM_COMMAND_BUFFERS, vk_fdm_attachment_flags() );
	if ( result != VK_SUCCESS ) {
		ri.Printf( PRINT_WARNING, "Vulkan density map allocation unavailable (%d); using full density\n", result );
		return;
	}
	Com_Memset( &vk_fdm.geometry, 0, sizeof( vk_fdm.geometry ) );
	vk_fdm.geometry.width = vk.sceneWidth;
	vk_fdm.geometry.height = vk.sceneHeight;
	vk_fdm.geometry.texelWidth = vk_foveation_caps.texelWidth;
	vk_fdm.geometry.texelHeight = vk_foveation_caps.texelHeight;
	Com_Memset( vk_fdm.offset, 0, sizeof( vk_fdm.offset ) );
	Com_Memset( &vk_fdm.drawn, 0, sizeof( vk_fdm.drawn ) );
	vk_fdm.strength = 0;
	vk_set_object_name( (uint64_t)vk_foveation.image, "engine density map", VK_DEBUG_REPORT_OBJECT_TYPE_IMAGE_EXT );
	// the pass loads the map, so it starts at full density
	Com_Memset( vk_foveation.current, 0xFF, vk_foveation.bytes );
	command = begin_command_buffer();
	VK_FovCopyDensity( &vk_foveation, command, 0 );
	end_command_buffer( command, "density map" );
	ri.Printf( PRINT_ALL, "Vulkan density map: %ux%u x 2 eye layers, texels %ux%u, %s\n", vk_foveation.width,
			   vk_foveation.height, vk_foveation_caps.texelWidth, vk_foveation_caps.texelHeight,
			   vk_fdm_offsets() ? "held still and offset onto the gaze" : "redrawn around the gaze" );
}

static void vk_foveation_create( void ) {
	VkPhysicalDeviceMemoryProperties memory;
	VkResult result;
	r_foveationDebugCvar = ri.Cvar_Get( "r_foveationDebug", "0", 0 );
	if ( !vk_foveation_caps.supported || !vk.multiview ) {
		return;
	}
	if ( vk_foveation_caps.backend == VK_FOV_BACKEND_FDM ) {
		vk_fdm_create();
		VK_XR_FoveationCaps( vk_foveation.image != VK_NULL_HANDLE );
		return;
	}
	vk_foveation_create_pass =
		(PFN_vkCreateRenderPass2)qvkGetDeviceProcAddr( vk.device, "vkCreateRenderPass2" );
	if ( !vk_foveation_create_pass ) {
		vk_foveation_create_pass =
			(PFN_vkCreateRenderPass2)qvkGetDeviceProcAddr( vk.device, "vkCreateRenderPass2KHR" );
	}
	vk_foveation_set_rate = (PFN_vkCmdSetFragmentShadingRateKHR)qvkGetDeviceProcAddr(
		vk.device, "vkCmdSetFragmentShadingRateKHR" );
	if ( !vk_foveation_create_pass || !vk_foveation_set_rate ) {
		ri.Printf( PRINT_WARNING,
				   "Vulkan foveation disabled: missing device RenderPass2 or shading-rate command\n" );
		vk_foveation_caps.supported = 0;
		vk_foveation_caps.backend = VK_FOV_BACKEND_NONE;
		return;
	}
	qvkGetPhysicalDeviceMemoryProperties( vk.physical_device, &memory );
	result = VK_FovCreate( &vk_foveation, vk.device, qvkGetDeviceProcAddr, &memory, &vk_foveation_caps,
						   vk.sceneWidth, vk.sceneHeight, NUM_COMMAND_BUFFERS, 0 );
	if ( result != VK_SUCCESS ) {
		ri.Printf( PRINT_WARNING, "Vulkan foveation allocation unavailable (%d); using full rate\n", result );
	} else {
		ri.Printf( PRINT_ALL, "Vulkan foveation attachment: %ux%u x 2 eye layers, texels %ux%u\n",
				   vk_foveation.width, vk_foveation.height, vk_foveation_caps.texelWidth,
				   vk_foveation_caps.texelHeight );
	}
	VK_XR_FoveationCaps( vk_foveation.image != VK_NULL_HANDLE );
}

static VkResult vk_foveation_render_pass( const VkRenderPassCreateInfo *desc, VkRenderPass *pass ) {
	if ( vk_foveation.image && vk_foveation_caps.backend == VK_FOV_BACKEND_SHADING_RATE ) {
		return VK_FovCreateRenderPass( &vk_foveation, desc, vk_foveation_create_pass, pass );
	}
	return qvkCreateRenderPass( vk.device, desc, NULL, pass );
}

/* Turnip's GMEM bytes a pixel; zero for layouts the tile model does not cover. */
static uint32_t vk_fdm_gmem_cpp( VkFormat format ) {
	switch ( format ) {
		case VK_FORMAT_R8G8B8A8_UNORM:
		case VK_FORMAT_R8G8B8A8_SRGB:
		case VK_FORMAT_B8G8R8A8_UNORM:
		case VK_FORMAT_B8G8R8A8_SRGB:
		case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
		case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
		case VK_FORMAT_D24_UNORM_S8_UINT:
		case VK_FORMAT_X8_D24_UNORM_PACK32:
		case VK_FORMAT_D32_SFLOAT:
			return 4;
		case VK_FORMAT_R16G16B16A16_SFLOAT:
		case VK_FORMAT_R16G16B16A16_UNORM:
			return 8;
		default:
			return 0;
	}
}

/* The tiler applies the map one sample a bin, so the bin is the map's real resolution. */
static void vk_fdm_tile_size( VkFramebuffer framebuffer ) {
	VkTilePropertiesQCOM props = {VK_STRUCTURE_TYPE_TILE_PROPERTIES_QCOM};
	VkPhysicalDeviceProperties device;
	uint32_t count = 1, cpp[3], n = 0, maxW = 2016, maxH = 2032;
	const uint32_t samples = vk.msaaActive ? (uint32_t)vkSamples : 1;
	const char *source = "reported";
	vkFdmGeometry_t *g = &vk_fdm.geometry;
	if ( !vk_fdm_active() )
		return;
	g->tileWidth = g->tileHeight = 0;
	qvkGetPhysicalDeviceProperties( vk.physical_device, &device );
	if ( vk_fdm_tile_properties && framebuffer ) {
		if ( vk_fdm_tile_properties( vk.device, framebuffer, &count, &props ) < VK_SUCCESS || !count ) {
			ri.Printf( PRINT_WARNING, "Vulkan density map bins: tile properties query failed\n" );
			return;
		}
		g->tileWidth = props.tileSize.width;
		g->tileHeight = props.tileSize.height;
	} else if ( strstr( device.deviceName, "Turnip" ) && strstr( device.deviceName, "750" ) ) {
		// the MSAA resolve target stays out of GMEM
		cpp[n++] = vk_fdm_gmem_cpp( vk.fboActive ? vk.color_format : vk.mainColorFormat ) * samples;
		if ( vk.fboActive && vk.hdrActive )
			cpp[n++] = vk_fdm_gmem_cpp( VK_FORMAT_R16G16B16A16_SFLOAT ) * samples;
		cpp[n++] = vk_fdm_gmem_cpp( vk.depth_format ) * samples;
		if ( vk_fdm_offsets() )
			VK_FdmTurnipOffsetLimit( g->width, g->height, samples, &maxW, &maxH );
		if ( !VK_FdmTurnipTile( g->width, g->height, cpp, n, maxW, maxH, &g->tileWidth, &g->tileHeight ) ) {
			ri.Printf( PRINT_ALL, "Vulkan density map bins: unknown (attachment formats outside the Turnip model)\n" );
			return;
		}
		source = "assumed from Turnip's tiling";
	} else {
		ri.Printf( PRINT_ALL, "Vulkan density map bins: unknown\n" );
		return;
	}
	ri.Printf( PRINT_ALL, "Vulkan density map bins: %ux%u %s (%ux%u bins over %ux%u, %ux%u map texels a bin)\n",
			   g->tileWidth, g->tileHeight, source, (g->width + g->tileWidth - 1) / g->tileWidth,
			   (g->height + g->tileHeight - 1) / g->tileHeight, g->width, g->height, g->tileWidth / g->texelWidth,
			   g->tileHeight / g->texelHeight );
}

/* Offsets: the fixed map changes only with level, field of view, scope or bins, and the host reads it as
 * the pass is recorded, so those changes are uploaded and waited on out of band; a moved HUD carve goes in
 * the frame and shows a frame late. Without offsets the map is redrawn around the gaze inside the frame,
 * and a host-reading driver sees it a frame late. */
/* The scope's opening in pixels: the mod's reticle masks outside an ellipse across the 2D area
 * (CG_DrawWeapReticle, indentX 0.16), which spans the eye's width and VK_XR_ScopeScaleY of its height. */
#define VK_FDM_SCOPE_INDENT 0.16f
static qboolean vk_fdm_scope( float *radiusX, float *radiusY ) {
	const float aspect = glConfig.vidHeight ? (float)glConfig.vidWidth / glConfig.vidHeight : 1;
	float indentY;
	if ( !VK_XR_ScopeNeedsBands() )
		return qfalse;
	indentY = MAX( 0, 0.5f - (0.5f - VK_FDM_SCOPE_INDENT) * aspect );
	*radiusX = (0.5f - VK_FDM_SCOPE_INDENT) * vk.sceneWidth;
	*radiusY = (0.5f - indentY) * VK_XR_ScopeScaleY() * vk.sceneHeight;
	return qtrue;
}

static void vk_fdm_update( void ) {
	vkFovMap_t map;
	const vkFdmGeometry_t *g = &vk_fdm.geometry;
	const qboolean offsets = vk_fdm_offsets();
	vkFdmDrawn_t drawn;
	qboolean scope;
	float radiusX = 0, radiusY = 0;
	int e;
	Com_Memset( &map, 0, sizeof( map ) );
	map.width = vk.sceneWidth;
	map.height = vk.sceneHeight;
	map.texelWidth = g->texelWidth;
	map.texelHeight = g->texelHeight;
	map.samples = vkSamples;
	VK_XR_FoveationMap( &map );
	scope = map.strength > 0 && vk_fdm_scope( &radiusX, &radiusY );
	vk_fdm.strength = map.strength;
	Com_Memcpy( vk_fdm.center, map.center, sizeof( vk_fdm.center ) );
	// the scope's mask hides everything outside its opening, wherever the eyes look
	if ( scope ) {
		VK_FdmWriteScope( vk_foveation.scratch, vk_foveation.bytes, g, radiusX, radiusY );
	} else if ( offsets ) {
		// drawn in display angles, the angle at the eye, so a zoom never rewrites the map
		VK_FdmReference( g, (const float (*)[4])map.display, vk_fdm.ref );
		VK_FdmWriteFixed( vk_foveation.scratch, vk_foveation.bytes, g, (const float (*)[4])map.display,
						  (const int32_t (*)[2])vk_fdm.ref, map.strength, map.eyeTracked );
	} else {
		VK_FdmWriteGaze( vk_foveation.scratch, vk_foveation.bytes, g, (const float (*)[4])map.display,
						 (const float (*)[2])map.center, map.strength, map.eyeTracked );
	}
	if ( offsets ) {
		// the scope map is drawn where the scope is, so it never slides
		VK_FdmOffsets( g, (const float (*)[2])map.center, (const int32_t (*)[2])vk_fdm.ref,
					   vk_foveation_caps.fdmOffsetGranularity.width, vk_foveation_caps.fdmOffsetGranularity.height,
					   scope ? 0 : map.strength, vk_fdm.offset );
	} else {
		Com_Memset( vk_fdm.offset, 0, sizeof( vk_fdm.offset ) );
	}
	Com_Memset( &drawn, 0, sizeof( drawn ) );
	drawn.strength = map.strength;
	drawn.eyeTracked = map.eyeTracked;
	drawn.scope = scope;
	drawn.radius[0] = radiusX;
	drawn.radius[1] = radiusY;
	Com_Memcpy( drawn.display, map.display, sizeof( drawn.display ) );
	// direct mode draws the HUD inside the foveated pass, so its quad stays at full density unless the gaze drives the map
	for ( e = 0; e < 2; e++ ) {
		if ( vk.xrDirect && map.strength > 0 && !map.eyeTracked && vk_fdm.hudValid[e] )
			VK_FdmFullDensityNdc( vk_foveation.scratch, g, e, vk_fdm.hud[e], vk_fdm.offset[e] );
		vk_fdm.hudValid[e] = qfalse;
	}
	if ( !vk_foveation.uploaded || memcmp( vk_foveation.scratch, vk_foveation.current, vk_foveation.bytes ) ) {
		Com_Memcpy( vk_foveation.current, vk_foveation.scratch, vk_foveation.bytes );
		// only a moved HUD carve may land a frame late; anything else would show the old map for a frame
		if ( offsets && ( !vk_foveation.uploaded || memcmp( &drawn, &vk_fdm.drawn, sizeof( drawn ) ) ) ) {
			VkCommandBuffer command = begin_command_buffer();
			VK_FovCopyDensity( &vk_foveation, command, vk.cmd_index );
			end_command_buffer( command, "density map" );
		} else {
			VK_FovCopyDensity( &vk_foveation, vk.cmd->command_buffer, vk.cmd_index );
		}
		vk_fdm.drawn = drawn;
	}
}

static void vk_foveation_upload( void ) {
	vkFovMap_t map;
	/* The same command-buffer handle may start a new recording every frame. */
	vk_foveation_invalidate_rate();
	if ( !vk_foveation.image ) {
		return;
	}
	if ( vk_fdm_active() ) {
		vk_fdm_update();
		return;
	}
	Com_Memset( &map, 0, sizeof( map ) );
	map.width = vk.sceneWidth;
	map.height = vk.sceneHeight;
	map.texelWidth = vk_foveation_caps.texelWidth;
	map.texelHeight = vk_foveation_caps.texelHeight;
	map.samples = vkSamples;
	VK_XR_FoveationMap( &map );
	VK_CHECK(
		VK_FovUpload( &vk_foveation, vk.cmd->command_buffer, vk.cmd_index, ++vk_foveation_serial, &map ) );
}

static void vk_foveation_draw( void ) {
	qboolean attachment = qfalse;
	if ( vk_foveation.image && VK_XR_Drawing() && !VK_XR_Screen() && !backEnd.projection2D &&
		tess.shader != tr.hudShader &&
		(vk.renderPassIndex == RENDER_PASS_MAIN || vk.renderPassIndex == RENDER_PASS_POST_SCENE) ) {
		attachment = qtrue;
	}
	vk_foveation_rate( attachment );
}

/* MULTIVIEW */
/* Device capability and render-pass policy shared by the scene target paths. */
static VkPhysicalDeviceMultiviewFeatures vk_multiview_features;

static qboolean vk_query_multiview( VkPhysicalDevice physical, qboolean *extension ) {
	VkPhysicalDeviceFeatures2 features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
	VkPhysicalDeviceProperties2 properties = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
	VkPhysicalDeviceMultiviewProperties views = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES};
	PFN_vkGetPhysicalDeviceFeatures2 getFeatures;
	PFN_vkGetPhysicalDeviceProperties2 getProperties;
	getFeatures = (PFN_vkGetPhysicalDeviceFeatures2)ri.VK_GetInstanceProcAddr(
		vk_instance, "vkGetPhysicalDeviceFeatures2" );
	getProperties = (PFN_vkGetPhysicalDeviceProperties2)ri.VK_GetInstanceProcAddr(
		vk_instance, "vkGetPhysicalDeviceProperties2" );
	if ( !getFeatures )
		getFeatures = (PFN_vkGetPhysicalDeviceFeatures2)ri.VK_GetInstanceProcAddr(
			vk_instance, "vkGetPhysicalDeviceFeatures2KHR" );
	if ( !getProperties )
		getProperties = (PFN_vkGetPhysicalDeviceProperties2)ri.VK_GetInstanceProcAddr(
			vk_instance, "vkGetPhysicalDeviceProperties2KHR" );
	if ( !getFeatures || !getProperties )
		return qfalse;
	Com_Memset( &vk_multiview_features, 0, sizeof( vk_multiview_features ) );
	vk_multiview_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES;
	features.pNext = &vk_multiview_features;
	properties.pNext = &views;
	getFeatures( physical, &features );
	getProperties( physical, &properties );
	*extension =
		properties.properties.apiVersion < VK_API_VERSION_1_1 || vk_instance_api < VK_API_VERSION_1_1;
	if ( !vk_multiview_features.multiview || views.maxMultiviewViewCount < 2 )
		return qfalse;
	/* Only the feature actually used by our vertex/fragment pipelines. */
	vk_multiview_features.multiviewGeometryShader = VK_FALSE;
	vk_multiview_features.multiviewTessellationShader = VK_FALSE;
	return qtrue;
}

static VkFormat vk_unorm_twin( VkFormat format ) {
	switch ( format ) {
		case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
		case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
		default: return format;
	}
}

static void vk_multiview_pass( VkRenderPassCreateInfo *desc, VkRenderPassMultiviewCreateInfo *views,
							   const uint32_t *mask ) {
	Com_Memset( views, 0, sizeof( *views ) );
	views->sType = VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO;
	views->pNext = desc->pNext;
	views->subpassCount = desc->subpassCount;
	views->pViewMasks = mask;
	views->correlationMaskCount = 1;
	views->pCorrelationMasks = mask;
	if ( vk.multiview )
		desc->pNext = views;
}

/* MONO */
static qboolean vk_mono_source( void ) {
	return vk.mono.sourceActive;
}

static void vk_create_mono_pass( const VkRenderPassCreateInfo *source, qboolean foveated,
								 VkRenderPass *pass ) {
	VkRenderPassCreateInfo desc = *source;
	VkRenderPassMultiviewCreateInfo views = {VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO};
	uint32_t masks[2];

	if ( !vk.multiview )
		return;
	if ( desc.subpassCount > ARRAY_LEN( masks ) ) {
		ri.Error( ERR_FATAL, "%s: %u subpasses", __func__, desc.subpassCount );
		return;
	}
	masks[0] = masks[1] = VK_PassViewMask( qtrue, RENDER_PASS_MONO_MAIN );
	/* The caller's scene description starts with its multiview declaration. */
	views = *(const VkRenderPassMultiviewCreateInfo *)source->pNext;
	views.subpassCount = desc.subpassCount;
	views.pViewMasks = masks;
	views.pCorrelationMasks = masks;
	desc.pNext = &views;
	if ( foveated ) {
		VK_CHECK( vk_foveation_render_pass( &desc, pass ) );
	} else {
		VK_CHECK( qvkCreateRenderPass( vk.device, &desc, NULL, pass ) );
	}
}

static void vk_create_mono_framebuffer( const VkFramebufferCreateInfo *source, VkRenderPass pass,
										VkFramebuffer *framebuffer ) {
	VkFramebufferCreateInfo desc = *source;
	if ( !pass )
		return;
	desc.renderPass = pass;
	VK_CHECK( qvkCreateFramebuffer( vk.device, &desc, NULL, framebuffer ) );
}

static void vk_create_mono_pipeline( const VkGraphicsPipelineCreateInfo *source, VkRenderPass pass,
									 VkPipeline *pipeline ) {
	VkGraphicsPipelineCreateInfo desc = *source;
	if ( !pass )
		return;
	if ( *pipeline ) {
		vk_wait_idle();
		qvkDestroyPipeline( vk.device, *pipeline, NULL );
		*pipeline = VK_NULL_HANDLE;
	}
	desc.renderPass = pass;
	VK_CHECK( qvkCreateGraphicsPipelines( vk.device, VK_NULL_HANDLE, 1, &desc, NULL, pipeline ) );
}

static void vk_destroy_mono_framebuffers( vkMonoTargets_t *targets ) {
	unsigned i;
	qvkDestroyFramebuffer( vk.device, targets->framebuffer.main, NULL );
	qvkDestroyFramebuffer( vk.device, targets->framebuffer.post_scene, NULL );
	qvkDestroyFramebuffer( vk.device, targets->framebuffer.output, NULL );
	for ( i = 0; i < ARRAY_LEN( targets->framebuffer.blur ); i++ ) {
		qvkDestroyFramebuffer( vk.device, targets->framebuffer.blur[i], NULL );
	}
	Com_Memset( &targets->framebuffer, 0, sizeof( targets->framebuffer ) );
}

static void vk_destroy_mono_passes( vkMonoTargets_t *targets ) {
	unsigned i;
	qvkDestroyRenderPass( vk.device, targets->pass.main, NULL );
	qvkDestroyRenderPass( vk.device, targets->pass.post_scene, NULL );
	qvkDestroyRenderPass( vk.device, targets->pass.output, NULL );
	for ( i = 0; i < ARRAY_LEN( targets->pass.blur ); i++ ) {
		qvkDestroyRenderPass( vk.device, targets->pass.blur[i], NULL );
	}
	Com_Memset( &targets->pass, 0, sizeof( targets->pass ) );
}

static void vk_destroy_mono_pipelines( vkMonoTargets_t *targets ) {
	unsigned i;
	qvkDestroyPipeline( vk.device, targets->pipeline.composite, NULL );
	for ( i = 0; i < ARRAY_LEN( targets->pipeline.blur ); i++ ) {
		qvkDestroyPipeline( vk.device, targets->pipeline.blur[i], NULL );
	}
	Com_Memset( &targets->pipeline, 0, sizeof( targets->pipeline ) );
}

/*
static VkFlags get_composite_alpha( VkCompositeAlphaFlagsKHR flags )
{
	const VkCompositeAlphaFlagBitsKHR compositeFlags[] = {
		VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
		VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
		VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
		VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
	};
	int i;

	for ( i = 1; i < ARRAY_LEN( compositeFlags ); i++ ) {
		if ( flags & compositeFlags[i] ) {
			return compositeFlags[i];
		}
	}

	return compositeFlags[0];
}
*/


static VkCommandBuffer begin_command_buffer( void )
{
	VkCommandBufferBeginInfo begin_info;
	VkCommandBufferAllocateInfo alloc_info;
	VkCommandBuffer command_buffer;

	alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	alloc_info.pNext = NULL;
	alloc_info.commandPool = vk.command_pool;
	alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	alloc_info.commandBufferCount = 1;
	VK_CHECK( qvkAllocateCommandBuffers( vk.device, &alloc_info, &command_buffer ) );

	begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin_info.pNext = NULL;
	begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	begin_info.pInheritanceInfo = NULL;

	VK_CHECK( qvkBeginCommandBuffer( command_buffer, &begin_info ) );

	return command_buffer;
}


static void end_command_buffer( VkCommandBuffer command_buffer, const char *location )
{
#ifdef USE_UPLOAD_QUEUE
	const VkPipelineStageFlags wait_dst_stage_mask = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
	VkSemaphore waits;
#endif
	VkSubmitInfo submit_info;
	VkCommandBuffer cmdbuf[1];

	cmdbuf[0] = command_buffer;

	VK_CHECK( qvkEndCommandBuffer( command_buffer ) );

	submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit_info.pNext = NULL;
#ifdef USE_UPLOAD_QUEUE
	if ( vk.rendering_finished != VK_NULL_HANDLE ) {
		waits = vk.rendering_finished;
		vk.rendering_finished = VK_NULL_HANDLE;
		submit_info.waitSemaphoreCount = 1;
		submit_info.pWaitSemaphores = &waits;
		submit_info.pWaitDstStageMask = &wait_dst_stage_mask;
	} else 
#endif
	{
		submit_info.waitSemaphoreCount = 0;
		submit_info.pWaitSemaphores = NULL;
		submit_info.pWaitDstStageMask = NULL;
	}

	submit_info.commandBufferCount = 1;
	submit_info.pCommandBuffers = cmdbuf;
	submit_info.signalSemaphoreCount = 0;
	submit_info.pSignalSemaphores = NULL;

	VK_CHECK( qvkQueueSubmit( vk.queue, 1, &submit_info, VK_NULL_HANDLE ) );

	vk_queue_wait_idle();

	qvkFreeCommandBuffers( vk.device, vk.command_pool, 1, cmdbuf );
}


static void record_image_layout_transition( VkCommandBuffer command_buffer, VkImage image, VkImageAspectFlags image_aspect_flags, 
	VkImageLayout old_layout, VkImageLayout new_layout, uint32_t src_stage_override, uint32_t dst_stage_override ) {
	VkImageMemoryBarrier barrier;
	uint32_t src_stage, dst_stage;

	switch ( old_layout ) {
		case VK_IMAGE_LAYOUT_UNDEFINED:
			if ( src_stage_override != 0 )
				src_stage = src_stage_override;
			else
				src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
			barrier.srcAccessMask = VK_ACCESS_NONE;
			break;
		case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
			src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
			barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			break;
		case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
			src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
			barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			break;
		case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
			src_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
			barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
			break;
		case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
			src_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			break;
		case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
			src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
			barrier.srcAccessMask = VK_ACCESS_NONE;
			break;
		default:
			ri.Error( ERR_DROP, "unsupported old layout %i", old_layout );
			src_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
			barrier.srcAccessMask = VK_ACCESS_NONE;
			break;
	}

	switch ( new_layout ) {
		case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
			dst_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			break;
		case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
			dst_stage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
			barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
			break;
		case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
			dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
			barrier.dstAccessMask = VK_ACCESS_NONE;
			break;
		case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
			dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
			barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			break;
		case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
			dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
			barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			break;
		case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
			dst_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
			barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
			break;
		default:
			ri.Error( ERR_DROP, "unsupported new layout %i", new_layout);
			dst_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
			barrier.dstAccessMask = VK_ACCESS_NONE;
			break;
	}


	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.pNext = NULL;
	//barrier.srcAccessMask = src_access_flags;
	//barrier.dstAccessMask = dst_access_flags;
	barrier.oldLayout = old_layout;
	barrier.newLayout = new_layout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange.aspectMask = image_aspect_flags;
	barrier.subresourceRange.baseMipLevel = 0;
	barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
	barrier.subresourceRange.baseArrayLayer = 0;
	barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;

	qvkCmdPipelineBarrier( command_buffer, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &barrier );
}


// debug markers
#define SET_OBJECT_NAME(obj,objName,objType) vk_set_object_name( (uint64_t)(obj), (objName), (objType) )

static void vk_set_object_name( uint64_t obj, const char *objName, VkDebugReportObjectTypeEXT objType )
{
	if ( qvkDebugMarkerSetObjectNameEXT && obj )
	{
		VkDebugMarkerObjectNameInfoEXT info;
		info.sType = VK_STRUCTURE_TYPE_DEBUG_MARKER_OBJECT_NAME_INFO_EXT;
		info.pNext = NULL;
		info.objectType = objType;
		info.object = obj;
		info.pObjectName = objName;
		qvkDebugMarkerSetObjectNameEXT( vk.device, &info );
	}
}


static void vk_create_swapchain( VkPhysicalDevice physical_device, VkDevice device, VkSurfaceKHR surface, VkSurfaceFormatKHR surface_format, VkSwapchainKHR *swapchain, qboolean verbose ) {
	VkImageViewCreateInfo view;
	VkSurfaceCapabilitiesKHR surface_caps;
	VkExtent2D image_extent;
	uint32_t present_mode_count, i;
	VkPresentModeKHR present_mode;
	VkPresentModeKHR *present_modes;
	uint32_t image_count;
	VkSwapchainCreateInfoKHR desc;
	qboolean mailbox_supported = qfalse;
	qboolean immediate_supported = qfalse;
	qboolean fifo_relaxed_supported = qfalse;
	int v;

	VK_CHECK( qvkGetPhysicalDeviceSurfaceCapabilitiesKHR( physical_device, surface, &surface_caps ) );

	image_extent = surface_caps.currentExtent;
	if ( image_extent.width == 0xffffffff && image_extent.height == 0xffffffff ) {
		image_extent.width = MIN( surface_caps.maxImageExtent.width, MAX( surface_caps.minImageExtent.width, (uint32_t) glConfig.vidWidth ) );
		image_extent.height = MIN( surface_caps.maxImageExtent.height, MAX( surface_caps.minImageExtent.height, (uint32_t) glConfig.vidHeight ) );
	}

	vk.clearAttachment = qtrue;

	if ( !vk.fboActive && !vk.xrDirect ) {
		// VK_IMAGE_USAGE_TRANSFER_DST_BIT is required by image clear operations.
		if ( ( surface_caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT ) == 0 ) {
			vk.clearAttachment = qfalse;
			ri.Printf( PRINT_WARNING, "VK_IMAGE_USAGE_TRANSFER_DST_BIT is not supported by the swapchain, \\r_clear might not work\n" );
		}
		// VK_IMAGE_USAGE_TRANSFER_SRC_BIT is required in order to take screenshots.
		if ((surface_caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0) {
			ri.Error(ERR_FATAL, "create_swapchain: VK_IMAGE_USAGE_TRANSFER_SRC_BIT is not supported by the swapchain");
		}
	}

	// determine present mode and swapchain image count
	VK_CHECK(qvkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &present_mode_count, NULL));

	present_modes = (VkPresentModeKHR *) ri.Malloc( present_mode_count * sizeof( VkPresentModeKHR ) );
	VK_CHECK(qvkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &present_mode_count, present_modes));

	if ( verbose ) {
		ri.Printf( PRINT_ALL, "...presentation modes:" );
	}
	for ( i = 0; i < present_mode_count; i++ ) {
		if ( verbose ) {
			ri.Printf( PRINT_ALL, " %s", pmode_to_str( present_modes[i] ) );
		}
		if ( present_modes[i] == VK_PRESENT_MODE_MAILBOX_KHR )
			mailbox_supported = qtrue;
		else if ( present_modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR )
			immediate_supported = qtrue;
		else if ( present_modes[i] == VK_PRESENT_MODE_FIFO_RELAXED_KHR )
			fifo_relaxed_supported = qtrue;
	}
	if ( verbose ) {
		ri.Printf( PRINT_ALL, "\n" );
	}

	ri.Free( present_modes );

	if ( ( v = ri.Cvar_VariableIntegerValue( "r_swapInterval" ) ) != 0 ) {
		if ( v == 2 && mailbox_supported )
			present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
		else if ( fifo_relaxed_supported )
			present_mode = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
		else
			present_mode = VK_PRESENT_MODE_FIFO_KHR;
		image_count = MAX( MIN_SWAPCHAIN_IMAGES_FIFO, surface_caps.minImageCount );
	} else {
		if ( immediate_supported ) {
			present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
			image_count = MAX( MIN_SWAPCHAIN_IMAGES_IMM, surface_caps.minImageCount );
		} else if ( mailbox_supported ) {
			present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
			image_count = MAX( MIN_SWAPCHAIN_IMAGES_MAILBOX, surface_caps.minImageCount );
		} else if ( fifo_relaxed_supported ) {
			present_mode = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
			image_count = MAX( MIN_SWAPCHAIN_IMAGES_FIFO, surface_caps.minImageCount );
		} else {
			present_mode = VK_PRESENT_MODE_FIFO_KHR;
			image_count = MAX( MIN_SWAPCHAIN_IMAGES_FIFO, surface_caps.minImageCount );
		}
	}

	if ( image_count < 2 ) {
		image_count = 2;
	}

	if ( surface_caps.maxImageCount == 0 && present_mode == VK_PRESENT_MODE_FIFO_KHR ) {
		image_count = MAX( MIN_SWAPCHAIN_IMAGES_FIFO_0, surface_caps.minImageCount );
	} else if ( surface_caps.maxImageCount > 0 ) {
		image_count = MIN( MIN( image_count, surface_caps.maxImageCount ), MAX_SWAPCHAIN_IMAGES );
	}

	if ( verbose ) {
		ri.Printf( PRINT_ALL, "...selected presentation mode: %s, image count: %i\n", pmode_to_str( present_mode ), image_count );
	}

	// create swap chain
	desc.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	desc.pNext = NULL;
	desc.flags = 0;
	desc.surface = surface;
	desc.minImageCount = image_count;
	desc.imageFormat = surface_format.format;
	desc.imageColorSpace = surface_format.colorSpace;
	desc.imageExtent = image_extent;
	desc.imageArrayLayers = 1;
	desc.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	if ( !vk.fboActive && !vk.xrDirect ) {
		desc.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	}
	desc.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	desc.queueFamilyIndexCount = 0;
	desc.pQueueFamilyIndices = NULL;
	desc.preTransform = surface_caps.currentTransform;
	//desc.compositeAlpha = get_composite_alpha( surface_caps.supportedCompositeAlpha );
	desc.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	desc.presentMode = present_mode;
	desc.clipped = VK_TRUE;
	desc.oldSwapchain = VK_NULL_HANDLE;

	VK_CHECK( qvkCreateSwapchainKHR( device, &desc, NULL, swapchain ) );

#ifdef __APPLE__
	// The EDR flag only takes on the FP16 layer MoltenVK sets up here, not on the
	// 8-bit layer that exists during format selection.
	if ( vk.hdrActive )
		ri.VK_ConfigureHDR( qtrue );
#endif

	VK_CHECK( qvkGetSwapchainImagesKHR( vk.device, vk.swapchain, &vk.swapchain_image_count, NULL ) );
	vk.swapchain_image_count = MIN( vk.swapchain_image_count, MAX_SWAPCHAIN_IMAGES );
	VK_CHECK( qvkGetSwapchainImagesKHR( vk.device, vk.swapchain, &vk.swapchain_image_count, vk.swapchain_images ) );

	for ( i = 0; i < vk.swapchain_image_count; i++ ) {
		view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		view.pNext = NULL;
		view.flags = 0;
		view.image = vk.swapchain_images[i];
		view.viewType = VK_IMAGE_VIEW_TYPE_2D;
		view.format = vk.present_format.format;
		view.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
		view.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
		view.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
		view.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
		view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		view.subresourceRange.baseMipLevel = 0;
		view.subresourceRange.levelCount = 1;
		view.subresourceRange.baseArrayLayer = 0;
		view.subresourceRange.layerCount = 1;

		VK_CHECK( qvkCreateImageView( vk.device, &view, NULL, &vk.swapchain_image_views[i] ) );

		SET_OBJECT_NAME( vk.swapchain_images[i], va( "swapchain image %i", i ), VK_DEBUG_REPORT_OBJECT_TYPE_IMAGE_EXT );
		SET_OBJECT_NAME( vk.swapchain_image_views[i], va( "swapchain image %i", i ), VK_DEBUG_REPORT_OBJECT_TYPE_IMAGE_VIEW_EXT );
	}

	for ( i = 0; i < vk.swapchain_image_count; i++ ) {
		VkSemaphoreCreateInfo s;
		s.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		s.pNext = NULL;
		s.flags = 0;
		VK_CHECK( qvkCreateSemaphore( vk.device, &s, NULL, &vk.swapchain_rendering_finished[i] ) );
		SET_OBJECT_NAME( vk.swapchain_rendering_finished[i], va( "swapchain_rendering_finished semaphore %i", i ), VK_DEBUG_REPORT_OBJECT_TYPE_SEMAPHORE_EXT );
	}

#if 0
	if (vk.initSwapchainLayout != VK_IMAGE_LAYOUT_UNDEFINED) {
		VkCommandBuffer command_buffer = begin_command_buffer();

		for (i = 0; i < vk.swapchain_image_count; i++) {
			record_image_layout_transition(command_buffer, vk.swapchain_images[i],
				VK_IMAGE_ASPECT_COLOR_BIT,
				VK_IMAGE_LAYOUT_UNDEFINED, vk.initSwapchainLayout, 0, 0);
		}

		end_command_buffer(command_buffer, __func__);
	}
#endif

	for ( i = 0; i < vk.swapchain_image_count; i++ ) {
		if ( vk.initSwapchainLayout != VK_IMAGE_LAYOUT_UNDEFINED ) {
			// The Vulkan spec states : Use of a presentable image must occur only after the image is returned by vkAcquireNextImageKHR,
			// and before it is released by vkQueuePresentKHR.
			// This includes transitioning the image layout and rendering commands(https ://docs.vulkan.org/refpages/latest/refpages/source/VkSwapchainKHR.html#_description)
			vk.swapchain_images_inited[i] = qfalse;
		} else {
			vk.swapchain_images_inited[i] = qtrue; // assume undefined layout
		}
	}
}


static void vk_create_render_passes( void )
{
	VkRenderPassMultiviewCreateInfo views;
	const uint32_t viewMask = VK_PassViewMask( qtrue, RENDER_PASS_MAIN );
	VkAttachmentDescription attachments[6]; // color | depth | msaa color | emissive resolve | emissive msaa | density map
	VkRenderPassFragmentDensityMapCreateInfoEXT density;
	VkAttachmentReference colorResolveRef;
	VkAttachmentReference colorResolveRefs[2];
	VkAttachmentReference colorRef0;
	VkAttachmentReference colorRefs[2];
	VkAttachmentReference depthRef0;
	VkSubpassDescription subpass;
	VkSubpassDependency deps[3];
	VkSubpassDependency directDeps[2];
	VkRenderPassCreateInfo desc;
	VkFormat depth_format;
	VkDevice device;
	uint32_t i;

	depth_format = vk.depth_format;
	device = vk.device;

	if ( vk.xrDirect )
	{
		// the acquired XR image; the frame ends in this pass
		attachments[0].flags = 0;
		attachments[0].format = vk.mainColorFormat;
		attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
#ifdef USE_BUFFER_CLEAR
		attachments[0].loadOp = vk.msaaActive ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_CLEAR;
#else
		attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
#endif
		attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; // what xrReleaseSwapchainImage expects
	}
	else if ( !vk.fboActive )
	{
		// presentation
		attachments[0].flags = 0;
		attachments[0].format = vk.present_format.format;
		attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
#ifdef USE_BUFFER_CLEAR
		attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
#else
		attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;	// Assuming this will be completely overwritten
#endif
		attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;		// needed for presentation
		attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachments[0].initialLayout = vk.initSwapchainLayout;
		attachments[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	}
	else
	{
		// resolve/color buffer
		attachments[0].flags = 0;
		attachments[0].format = vk.color_format;
		attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;

#ifdef USE_BUFFER_CLEAR
		if ( vk.msaaActive )
			attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;	// Assuming this will be completely overwritten
		else
			attachments[ 0 ].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
#else
		attachments[ 0 ].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;	// Assuming this will be completely overwritten
#endif

		attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;   // needed for next render pass
		attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachments[0].initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		attachments[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}

	// depth buffer
	attachments[1].flags = 0;
	attachments[1].format = depth_format;
	attachments[1].samples = vkSamples;
	attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; // Need empty depth buffer before use
	attachments[1].stencilLoadOp = glConfig.stencilBits ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	colorRef0.attachment = 0;
	colorRef0.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	depthRef0.attachment = 1;
	depthRef0.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	Com_Memset( &subpass, 0, sizeof( subpass ) );
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &colorRef0;
	subpass.pDepthStencilAttachment = &depthRef0;

	Com_Memset( &desc, 0, sizeof( desc ) );
	desc.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	desc.pNext = NULL;
	desc.flags = 0;
	desc.pAttachments = attachments;
	desc.pSubpasses = &subpass;

	desc.subpassCount = 1;
	desc.attachmentCount = 2;

	if ( vk.hdrActive && !vk.msaaActive )
	{
		// resolved emissive layer, rendered directly as a 2nd color attachment
		attachments[2].flags = 0;
		attachments[2].format = VK_FORMAT_R16G16B16A16_SFLOAT;
		attachments[2].samples = VK_SAMPLE_COUNT_1_BIT;
		attachments[2].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		attachments[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE; // sampled by the gamma pass
		attachments[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachments[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		// mirror the color attachment so the post-scene pass can LOAD it
		attachments[2].initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		attachments[2].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

		colorRefs[0] = colorRef0;
		colorRefs[1].attachment = 2;
		colorRefs[1].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		subpass.colorAttachmentCount = 2;
		subpass.pColorAttachments = colorRefs;

		desc.attachmentCount = 3;
	}

	if ( vk.msaaActive )
	{
		attachments[2].flags = 0;
		attachments[2].format = vk.mainColorFormat;
		attachments[2].samples = vkSamples;
#ifdef USE_BUFFER_CLEAR
		attachments[2].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
#else
		attachments[2].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
#endif
		attachments[2].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; // only its resolve is read later
		attachments[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachments[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachments[2].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		attachments[2].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		desc.attachmentCount = 3;

		colorRef0.attachment = 2; // msaa image attachment
		colorRef0.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		colorResolveRef.attachment = 0; // resolve image attachment
		colorResolveRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		subpass.pResolveAttachments = &colorResolveRef;

		if ( vk.hdrActive )
		{
			// resolved emissive layer (resolve target, sampled by the gamma pass)
			attachments[3].flags = 0;
			attachments[3].format = VK_FORMAT_R16G16B16A16_SFLOAT;
			attachments[3].samples = VK_SAMPLE_COUNT_1_BIT;
			attachments[3].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
			attachments[3].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			attachments[3].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			attachments[3].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			// mirror the color resolve attachment so the post-scene pass can LOAD it
			attachments[3].initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			attachments[3].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

			// msaa emissive render target
			attachments[4].flags = 0;
			attachments[4].format = VK_FORMAT_R16G16B16A16_SFLOAT;
			attachments[4].samples = vkSamples;
			attachments[4].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
			attachments[4].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			attachments[4].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			attachments[4].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			attachments[4].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			attachments[4].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

			colorRefs[0] = colorRef0;        // attachment 2 (msaa color)
			colorRefs[1].attachment = 4;     // msaa emissive render target
			colorRefs[1].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			subpass.colorAttachmentCount = 2;
			subpass.pColorAttachments = colorRefs;

			colorResolveRefs[0] = colorResolveRef; // attachment 0 (color resolve)
			colorResolveRefs[1].attachment = 3;    // emissive resolve
			colorResolveRefs[1].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			subpass.pResolveAttachments = colorResolveRefs;

			desc.attachmentCount = 5;
		}
	}

	vk_multiview_pass( &desc, &views, &viewMask );

	// subpass dependencies

	Com_Memset( &deps, 0, sizeof( deps ) );

	deps[2].srcSubpass = VK_SUBPASS_EXTERNAL;
	deps[2].dstSubpass = 0;
	deps[2].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;	// What pipeline stage is waiting on the dependency
	deps[2].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;	// What pipeline stage is waiting on the dependency
	deps[2].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;			// What access scopes are influence the dependency
	deps[2].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;			// What access scopes are waiting on the dependency
	deps[2].dependencyFlags = 0;

	if ( !vk.fboActive && !vk.xrDirect )
	{
		desc.dependencyCount = 1;
		desc.pDependencies = &deps[2];

		VK_CHECK( vk_foveation_render_pass( &desc, &vk.render_pass.main ) );
		SET_OBJECT_NAME( vk.render_pass.main, "render pass - main", VK_DEBUG_REPORT_OBJECT_TYPE_RENDER_PASS_EXT );

		return;
	}

	desc.dependencyCount = 2;
	desc.pDependencies = &deps[0];

	deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
	deps[0].dstSubpass = 0;
	deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;			// What pipeline stage must have completed for the dependency
	deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;	// What pipeline stage is waiting on the dependency
	deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;						// What access scopes are influence the dependency
	deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; // What access scopes are waiting on the dependency
	deps[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;					// Only need the current fragment (or tile) synchronized, not the whole framebuffer

	deps[1].srcSubpass = 0;
	deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
	deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;	// Fragment data has been written
	deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;			// Don't start shading until data is available
	deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;			// Waiting for color data to be written
	deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;						// Don't read things from the shader before ready
	deps[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;					// Only need the current fragment (or tile) synchronized, not the whole framebuffer

	if ( vk.xrDirect ) {
		// deps[0..1] stay intact for the screenmap pass below
		directDeps[0] = deps[2];
		// the virtual-screen blit and the mirror read this image after the pass
		directDeps[1].srcSubpass = 0;
		directDeps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
		directDeps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		directDeps[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		directDeps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		directDeps[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
		directDeps[1].dependencyFlags = 0;
		desc.pDependencies = directDeps;
	}

	if ( vk_fdm_active() ) {
		// only the stereo scene pass carries the density map
		VkRenderPassCreateInfo scene = desc;
		VK_FovDensityAttachment( &scene, attachments, &density );
		VK_CHECK( qvkCreateRenderPass( device, &scene, NULL, &vk.render_pass.main ) );
	} else {
		VK_CHECK( vk_foveation_render_pass( &desc, &vk.render_pass.main ) );
	}
	vk_create_mono_pass( &desc, qtrue, &vk.mono.pass.main );
	SET_OBJECT_NAME( vk.render_pass.main, "render pass - main", VK_DEBUG_REPORT_OBJECT_TYPE_RENDER_PASS_EXT );

	if ( vk.fboActive && r_bloom->integer ) {

		// post-scene pass: coronas and 2D blend into the resolved scene image over their own depth
		VkAttachmentDescription postAttachments[3];
		const VkAttachmentReference postColors[2] = {{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
													 {2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
		const VkAttachmentReference postDepth = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
		VkSubpassDescription postSubpass;
		VkSubpassDependency postDeps[3];
		VkRenderPassCreateInfo post = desc;

		postAttachments[0] = attachments[0];
		postAttachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		// nothing after the scene depth-tests against the world; the pass clears this one for the 3D icons
		postAttachments[1] = attachments[1];
		postAttachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
		postAttachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		// keep the emitter energy the main pass accumulated
		postAttachments[2] = attachments[vk.msaaActive ? 3 : 2];
		postAttachments[2].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;

		Com_Memset( &postSubpass, 0, sizeof( postSubpass ) );
		postSubpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		postSubpass.colorAttachmentCount = vk.hdrActive ? 2 : 1;
		postSubpass.pColorAttachments = postColors;
		postSubpass.pDepthStencilAttachment = &postDepth;

		postDeps[0] = deps[0];
		postDeps[1] = deps[1];
		// last frame's icons wrote the depth this frame clears
		Com_Memset( &postDeps[2], 0, sizeof( postDeps[2] ) );
		postDeps[2].srcSubpass = VK_SUBPASS_EXTERNAL;
		postDeps[2].dstSubpass = 0;
		postDeps[2].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		postDeps[2].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		postDeps[2].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		postDeps[2].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

		post.attachmentCount = vk.hdrActive ? 3 : 2;
		post.pAttachments = postAttachments;
		post.pSubpasses = &postSubpass;
		post.dependencyCount = ARRAY_LEN( postDeps );
		post.pDependencies = postDeps;
		VK_CHECK( vk_foveation_render_pass( &post, &vk.render_pass.post_scene ) );
		vk_create_mono_pass( &post, qtrue, &vk.mono.pass.post_scene );
		SET_OBJECT_NAME( vk.render_pass.post_scene, "render pass - post scene", VK_DEBUG_REPORT_OBJECT_TYPE_RENDER_PASS_EXT );

		// bloom blur targets
		desc.attachmentCount = 1;

		colorRef0.attachment = 0;
		colorRef0.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		Com_Memset( &subpass, 0, sizeof( subpass ) );
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = 1;
		subpass.pColorAttachments = &colorRef0;

		attachments[0].flags = 0;
		attachments[0].format = vk.bloom_format;
		attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
		attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;	// Assuming this will be completely overwritten
		attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;		// needed for next render pass
		attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachments[0].initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		attachments[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

		for ( i = 0; i < ARRAY_LEN( vk.render_pass.blur ); i++ )
		{
			VK_CHECK( qvkCreateRenderPass( device, &desc, NULL, &vk.render_pass.blur[i] ) );
			vk_create_mono_pass( &desc, qfalse, &vk.mono.pass.blur[i] );
			SET_OBJECT_NAME( vk.render_pass.blur[i], va( "render pass - blur %i", i ), VK_DEBUG_REPORT_OBJECT_TYPE_RENDER_PASS_EXT );
		}
	}

	// capture render pass
	if ( vk.capture.image )
	{
		Com_Memset( &subpass, 0, sizeof( subpass ) );

		attachments[0].flags = 0;
		attachments[0].format = vk.capture_format;
		attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
		attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; // this will be completely overwritten
		attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;   // needed for next render pass
		attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		attachments[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

		colorRef0.attachment = 0;
		colorRef0.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = 1;
		subpass.pColorAttachments = &colorRef0;

		desc.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
		desc.pNext = NULL;
		desc.flags = 0;
		desc.pAttachments = attachments;
		desc.attachmentCount = 1;
		desc.pSubpasses = &subpass;
		desc.subpassCount = 1;

		VK_CHECK( qvkCreateRenderPass( device, &desc, NULL, &vk.render_pass.capture ) );
		if ( vk.xr_output.image ) {
			VkSubpassDependency xrDeps[2] = {{0}, {0}};
			const VkSubpassDependency *savedDeps = desc.pDependencies;
			uint32_t savedCount = desc.dependencyCount;
			// one dependency set serves the blit source and the swapchain target, keeping the passes compatible
			xrDeps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
			xrDeps[0].dstSubpass = 0;
			xrDeps[0].srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			xrDeps[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			xrDeps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			xrDeps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			xrDeps[1].srcSubpass = 0;
			xrDeps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
			xrDeps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			xrDeps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			xrDeps[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
			xrDeps[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			// the eye pass applies gamma to the finished scene and writes the encoded result through a UNORM view
			attachments[0].format = vk_unorm_twin( vk.xr_output.format );
			desc.dependencyCount = 2;
			desc.pDependencies = xrDeps;
			vk_multiview_pass( &desc, &views, &viewMask );
			VK_CHECK( qvkCreateRenderPass( device, &desc, NULL, &vk.xr_output.pass ) );
			SET_OBJECT_NAME( vk.xr_output.pass, "render pass - xr output", VK_DEBUG_REPORT_OBJECT_TYPE_RENDER_PASS_EXT );
			vk_create_mono_pass( &desc, qfalse, &vk.mono.pass.output );
			if ( vk.xr_output.eye_count ) {
				// xrReleaseSwapchainImage expects the attachment layout
				attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
				VK_CHECK( qvkCreateRenderPass( device, &desc, NULL, &vk.xr_output.eye_pass ) );
				SET_OBJECT_NAME( vk.xr_output.eye_pass, "render pass - xr eye output", VK_DEBUG_REPORT_OBJECT_TYPE_RENDER_PASS_EXT );
			}
			{
				// the virtual-screen composition alone pays for a depth buffer; the world's output stays color-only
				const VkAttachmentReference screenDepth = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
				attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
				attachments[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
				attachments[1].flags = 0;
				attachments[1].format = depth_format;
				attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
				attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
				attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
				attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
				attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
				attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
				attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
				xrDeps[0].srcStageMask |= VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
				xrDeps[0].srcAccessMask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
				xrDeps[0].dstStageMask |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
				xrDeps[0].dstAccessMask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
				subpass.pDepthStencilAttachment = &screenDepth;
				desc.attachmentCount = 2;
				VK_CHECK( qvkCreateRenderPass( device, &desc, NULL, &vk.xr_output.screen_pass ) );
				SET_OBJECT_NAME( vk.xr_output.screen_pass, "render pass - xr screen", VK_DEBUG_REPORT_OBJECT_TYPE_RENDER_PASS_EXT );
				if ( vk.xr_output.eye_count ) {
					attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
					VK_CHECK( qvkCreateRenderPass( device, &desc, NULL, &vk.xr_output.screen_eye_pass ) );
					SET_OBJECT_NAME( vk.xr_output.screen_eye_pass, "render pass - xr eye screen", VK_DEBUG_REPORT_OBJECT_TYPE_RENDER_PASS_EXT );
				}
				subpass.pDepthStencilAttachment = NULL;
				desc.attachmentCount = 1;
			}
			desc.pNext = NULL;
			desc.dependencyCount = savedCount;
			desc.pDependencies = savedDeps;
		}
		SET_OBJECT_NAME( vk.render_pass.capture, "render pass - capture", VK_DEBUG_REPORT_OBJECT_TYPE_RENDER_PASS_EXT );
	}

	colorRef0.attachment = 0;
	colorRef0.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	desc.attachmentCount = 1;

	Com_Memset( &subpass, 0, sizeof( subpass ) );
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &colorRef0;

	desc.pNext = NULL;
	// gamma post-processing
	attachments[0].flags = 0;
	attachments[0].format = vk.present_format.format;
	attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE; // needed for presentation
	attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[0].initialLayout = vk.initSwapchainLayout;
	attachments[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

	desc.dependencyCount = 1;
	desc.pDependencies = &deps[2];

	VK_CHECK( qvkCreateRenderPass( device, &desc, NULL, &vk.render_pass.gamma ) );
	SET_OBJECT_NAME( vk.render_pass.gamma, "render pass - gamma", VK_DEBUG_REPORT_OBJECT_TYPE_RENDER_PASS_EXT );

	// screenmap
	desc.dependencyCount = 2;
	desc.pDependencies = &deps[0];

	// screenmap resolve/color buffer
	attachments[0].flags = 0;
	attachments[0].format = vk.color_format;
	attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
#ifdef USE_BUFFER_CLEAR
	if ( vk.screenMapSamples > VK_SAMPLE_COUNT_1_BIT )
		attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	else
		attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
#else
	attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; // Assuming this will be completely overwritten
#endif
	attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;   // needed for next render pass
	attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[0].initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	attachments[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	// screenmap depth buffer
	attachments[1].flags = 0;
	attachments[1].format = depth_format;
	attachments[1].samples = vk.screenMapSamples;
	attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; // Need empty depth buffer before use
	attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	colorRef0.attachment = 0;
	colorRef0.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	depthRef0.attachment = 1;
	depthRef0.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	Com_Memset( &subpass, 0, sizeof( subpass ) );
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &colorRef0;
	subpass.pDepthStencilAttachment = &depthRef0;

	Com_Memset( &desc, 0, sizeof( desc ) );
	desc.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	desc.pNext = NULL;
	desc.flags = 0;
	desc.pAttachments = attachments;
	desc.pSubpasses = &subpass;
	desc.subpassCount = 1;
	desc.attachmentCount = 2;
	desc.dependencyCount = 2;
	desc.pDependencies = deps;

	if ( vk.screenMapSamples > VK_SAMPLE_COUNT_1_BIT ) {

		attachments[2].flags = 0;
		attachments[2].format = vk.color_format;
		attachments[2].samples = vk.screenMapSamples;
#ifdef USE_BUFFER_CLEAR
		attachments[2].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
#else
		attachments[2].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
#endif
		attachments[2].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachments[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachments[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachments[2].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		attachments[2].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		desc.attachmentCount = 3;

		colorRef0.attachment = 2; // screenmap msaa image attachment
		colorRef0.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		colorResolveRef.attachment = 0; // screenmap resolve image attachment
		colorResolveRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		subpass.pResolveAttachments = &colorResolveRef;
	}

	VK_CHECK( qvkCreateRenderPass( device, &desc, NULL, &vk.render_pass.screenmap ) );

	SET_OBJECT_NAME( vk.render_pass.screenmap, "render pass - screenmap", VK_DEBUG_REPORT_OBJECT_TYPE_RENDER_PASS_EXT );
}


static void allocate_and_bind_image_memory(VkImage image) {
	VkMemoryRequirements memory_requirements;
	VkDeviceSize alignment;
	ImageChunk *chunk;
	int i;

	qvkGetImageMemoryRequirements(vk.device, image, &memory_requirements);

	if ( memory_requirements.size > vk.image_chunk_size ) {
		ri.Error( ERR_FATAL, "Vulkan: could not allocate memory, image is too large (%ikbytes).",
			(int)(memory_requirements.size/1024) );
	}

	chunk = NULL;

	// Try to find an existing chunk of sufficient capacity.
	alignment = memory_requirements.alignment;
	for ( i = 0; i < vk_world.num_image_chunks; i++ ) {
		// ensure that memory region has proper alignment
		VkDeviceSize offset = PAD( vk_world.image_chunks[i].used, alignment );

		if ( offset + memory_requirements.size <= vk.image_chunk_size ) {
			chunk = &vk_world.image_chunks[i];
			chunk->used = offset + memory_requirements.size;
			break;
		}
	}

	// Allocate a new chunk in case we couldn't find suitable existing chunk.
	if (chunk == NULL) {
		VkMemoryAllocateInfo alloc_info;
		VkDeviceMemory memory;

		if (vk_world.num_image_chunks >= MAX_IMAGE_CHUNKS) {
			ri.Error(ERR_FATAL, "Vulkan: image chunk limit has been reached" );
		}

		alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		alloc_info.pNext = NULL;
		alloc_info.allocationSize = vk.image_chunk_size;
		alloc_info.memoryTypeIndex = find_memory_type( memory_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT );

		VK_CHECK( qvkAllocateMemory( vk.device, &alloc_info, NULL, &memory ) );

		chunk = &vk_world.image_chunks[vk_world.num_image_chunks];
		chunk->memory = memory;
		chunk->used = memory_requirements.size;

		SET_OBJECT_NAME( memory, va( "image memory chunk %i", vk_world.num_image_chunks ), VK_DEBUG_REPORT_OBJECT_TYPE_DEVICE_MEMORY_EXT );

		vk_world.num_image_chunks++;
	}

	VK_CHECK(qvkBindImageMemory(vk.device, image, chunk->memory, chunk->used - memory_requirements.size));
}


static void vk_clean_staging_buffer( void )
{
	if ( vk.staging_buffer.handle != VK_NULL_HANDLE ) {
		qvkDestroyBuffer( vk.device, vk.staging_buffer.handle, NULL );
		vk.staging_buffer.handle = VK_NULL_HANDLE;
	}

	//if ( vk.staging_buffer.ptr != NULL ) 
	//	qvkUnmapMemory( vk.device, vk.staging_buffer.memory ) {
	//	vk.staging_buffer.ptr = NULL;
	//}

	if ( vk.staging_buffer.memory != VK_NULL_HANDLE ) {
		qvkFreeMemory( vk.device, vk.staging_buffer.memory, NULL );
		vk.staging_buffer.memory = VK_NULL_HANDLE;
	}

	vk.staging_buffer.ptr = NULL;
	vk.staging_buffer.size = 0;
#ifdef USE_UPLOAD_QUEUE
	vk.staging_buffer.offset = 0;
#endif
}


#ifdef USE_UPLOAD_QUEUE
static qboolean vk_wait_staging_buffer( void )
{
	if ( vk.aux_fence_wait ) {
		VkResult res = qvkWaitForFences( vk.device, 1, &vk.aux_fence, VK_TRUE, 5 * 1000000000ULL );
		if ( res != VK_SUCCESS ) {
			ri.Error( ERR_FATAL, "vkWaitForFences() failed with %s at %s", vk_result_string( res ), __func__ );
		}
		qvkResetFences( vk.device, 1, &vk.aux_fence );
		VK_CHECK( qvkResetCommandBuffer( vk.staging_command_buffer, 0 ) );
		vk.staging_buffer.offset = 0; // FIXME: is this correct?
		vk.aux_fence_wait = qfalse;
		return qtrue;
	} else {
		return qfalse;
	}
}


static void vk_flush_staging_buffer( qboolean final )
{
	const VkPipelineStageFlags wait_dst_stage_mask = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
	VkSemaphore waits;
	VkSubmitInfo submit_info;
	VkResult res;

	if ( vk.staging_buffer.offset == 0 ) {
		return;
	}

	//ri.Printf( PRINT_WARNING, S_COLOR_CYAN ">>> flush %i bytes (final=%i)<<<\n", (int)vk_world.staging_buffer_offset, final );

	vk.staging_buffer.offset = 0;

	VK_CHECK( qvkEndCommandBuffer( vk.staging_command_buffer ) );

	submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit_info.pNext = NULL;

	if ( vk.rendering_finished != VK_NULL_HANDLE ) {
		// first call after previous queue submission?
		waits = vk.rendering_finished;
		vk.rendering_finished = VK_NULL_HANDLE;
		submit_info.waitSemaphoreCount = 1;
		submit_info.pWaitSemaphores = &waits;
		submit_info.pWaitDstStageMask = &wait_dst_stage_mask;
	} else {
		submit_info.waitSemaphoreCount = 0;
		submit_info.pWaitSemaphores = NULL;
		submit_info.pWaitDstStageMask = NULL;
	}

	submit_info.commandBufferCount = 1;
	submit_info.pCommandBuffers = &vk.staging_command_buffer;

	if ( vk.image_uploaded != VK_NULL_HANDLE ) {
		ri.Error( ERR_FATAL, "Vulkan: incorrect state during image upload" );
	}
	if ( final ) {
		// final submission before recording
		submit_info.signalSemaphoreCount = 1;
		submit_info.pSignalSemaphores = &vk.image_uploaded2;
		vk.image_uploaded = vk.image_uploaded2;
		VK_CHECK( qvkQueueSubmit( vk.queue, 1, &submit_info, vk.aux_fence ) );
		vk.aux_fence_wait = qtrue;
	} else {
		// if submission before another upload then do explicit wait
		submit_info.signalSemaphoreCount = 0;
		submit_info.pSignalSemaphores = NULL;
		VK_CHECK( qvkQueueSubmit( vk.queue, 1, &submit_info, vk.aux_fence ) );
		res = qvkWaitForFences( vk.device, 1, &vk.aux_fence, VK_TRUE, 5 * 1000000000ULL );
		if ( res != VK_SUCCESS ) {
			ri.Error( ERR_FATAL, "vkWaitForFences() failed with %s at %s", vk_result_string( res ), __func__ );
		}
		qvkResetFences( vk.device, 1, &vk.aux_fence );
		VK_CHECK( qvkResetCommandBuffer( vk.staging_command_buffer, 0 ) );
	}
}
#endif // USE_UPLOAD_QUEUE


static void vk_alloc_staging_buffer( VkDeviceSize size )
{
	VkBufferCreateInfo buffer_desc;
	VkMemoryRequirements memory_requirements;
	VkMemoryAllocateInfo alloc_info;
	uint32_t memory_type;
	void *data;

	vk_clean_staging_buffer();

	vk.staging_buffer.size = MAX( size, STAGING_BUFFER_SIZE );
	vk.staging_buffer.size = PAD( vk.staging_buffer.size, 1024 * 1024 );

	buffer_desc.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer_desc.pNext = NULL;
	buffer_desc.flags = 0;
	buffer_desc.size = vk.staging_buffer.size;
	buffer_desc.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	buffer_desc.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	buffer_desc.queueFamilyIndexCount = 0;
	buffer_desc.pQueueFamilyIndices = NULL;
	VK_CHECK(qvkCreateBuffer(vk.device, &buffer_desc, NULL, &vk.staging_buffer.handle));

	qvkGetBufferMemoryRequirements( vk.device, vk.staging_buffer.handle, &memory_requirements );

	memory_type = find_memory_type( memory_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT );

	alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc_info.pNext = NULL;
	alloc_info.allocationSize = memory_requirements.size;
	alloc_info.memoryTypeIndex = memory_type;

	VK_CHECK(qvkAllocateMemory(vk.device, &alloc_info, NULL, &vk.staging_buffer.memory));
	VK_CHECK(qvkBindBufferMemory(vk.device, vk.staging_buffer.handle, vk.staging_buffer.memory, 0));

	VK_CHECK(qvkMapMemory(vk.device, vk.staging_buffer.memory, 0, VK_WHOLE_SIZE, 0, &data));
	vk.staging_buffer.ptr = (byte*)data;
#ifdef USE_UPLOAD_QUEUE
	vk.staging_buffer.offset = 0;
#endif
	SET_OBJECT_NAME( vk.staging_buffer.handle, "staging buffer", VK_DEBUG_REPORT_OBJECT_TYPE_BUFFER_EXT );
	SET_OBJECT_NAME( vk.staging_buffer.memory, "staging buffer memory", VK_DEBUG_REPORT_OBJECT_TYPE_DEVICE_MEMORY_EXT );
}


#ifdef USE_VK_VALIDATION
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT object_type, uint64_t object, size_t location,
	int32_t message_code, const char* layer_prefix, const char* message, void* user_data) {
#ifdef _WIN32
	MessageBoxA( 0, message, layer_prefix, MB_ICONWARNING );
	OutputDebugString(message);
	OutputDebugString("\n");
	DebugBreak();
#endif
	return VK_FALSE;
}
#endif


static qboolean used_instance_extension( const char *ext )
{
	const char *u;

	// allow all VK_*_surface extensions
	u = strrchr( ext, '_' );
	if ( u && Q_stricmp( u + 1, "surface" ) == 0 )
		return qtrue;

	if ( Q_stricmp( ext, VK_KHR_DISPLAY_EXTENSION_NAME ) == 0 )
		return qtrue; // needed for KMSDRM instances/devices?

	if ( Q_stricmp( ext, VK_KHR_SWAPCHAIN_EXTENSION_NAME ) == 0 )
		return qtrue;

#ifdef USE_VK_VALIDATION
	if ( Q_stricmp( ext, VK_EXT_DEBUG_REPORT_EXTENSION_NAME ) == 0 )
		return qtrue;
#endif

	if ( Q_stricmp( ext, VK_EXT_DEBUG_UTILS_EXTENSION_NAME ) == 0 )
		return qtrue;

	if ( Q_stricmp( ext, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME ) == 0 )
		return qtrue;

	if ( Q_stricmp( ext, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME ) == 0 )
		return qtrue;

	if ( Q_stricmp( ext, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME ) == 0 )
		return qtrue;

	return qfalse;
}


static void create_instance( void )
{
#ifdef USE_VK_VALIDATION
	const char* validation_layer_name = "VK_LAYER_LUNARG_standard_validation";
	const char* validation_layer_name2 = "VK_LAYER_KHRONOS_validation";
#endif
	VkInstanceCreateInfo desc;
	VkInstanceCreateFlags flags;
	VkExtensionProperties *extension_properties;
	VkResult res;
	const char **extension_names;
	uint32_t i, n, count, extension_count;
	VkApplicationInfo appInfo;

	flags = 0;
	count = 0;
	extension_count = 0;
	VK_CHECK(qvkEnumerateInstanceExtensionProperties(NULL, &count, NULL));

	extension_properties = (VkExtensionProperties *)ri.Malloc(sizeof(VkExtensionProperties) * count);
	extension_names = (const char**)ri.Malloc(sizeof(char *) * count);

	VK_CHECK( qvkEnumerateInstanceExtensionProperties( NULL, &count, extension_properties ) );
	for ( i = 0; i < count; i++ ) {
		const char *ext = extension_properties[i].extensionName;

		if ( !used_instance_extension( ext ) ) {
			continue;
		}

		// search for duplicates
		for ( n = 0; n < extension_count; n++ ) {
			if ( Q_stricmp( ext, extension_names[ n ] ) == 0 ) {
				break;
			}
		}
		if ( n != extension_count ) {
			continue;
		}

		extension_names[ extension_count++ ] = ext;

		if ( Q_stricmp( ext, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME ) == 0 ) {
			flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
		}

		if ( Q_stricmp( ext, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME ) == 0 ) {
			vk.hdrColorspaceExt = qtrue;
		}

		ri.Printf(PRINT_DEVELOPER, "instance extension: %s\n", ext);
	}

	appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	appInfo.pNext = NULL;
	appInfo.pApplicationName = NULL; // Q3_VERSION;
	appInfo.applicationVersion = 0x0;
	appInfo.pEngineName = NULL;
	appInfo.engineVersion = 0x0;
#ifdef _DEBUG
	appInfo.apiVersion = VK_API_VERSION_1_1;
#else
	appInfo.apiVersion = VK_API_VERSION_1_0;
#endif

	// create instance
	appInfo.apiVersion = VK_XR_ApiVersion( appInfo.apiVersion );
	vk_instance_api = appInfo.apiVersion;
	desc.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	desc.pNext = NULL;
	desc.flags = flags;
	desc.pApplicationInfo = &appInfo;
	desc.enabledExtensionCount = extension_count;
	desc.ppEnabledExtensionNames = extension_names;

#ifdef USE_VK_VALIDATION
	desc.enabledLayerCount = 1;
	desc.ppEnabledLayerNames = &validation_layer_name;

	res = VK_XR_CreateInstance( qvkCreateInstance, &desc, &vk_instance );

	if ( res == VK_ERROR_LAYER_NOT_PRESENT ) {

		desc.enabledLayerCount = 1;
		desc.ppEnabledLayerNames = &validation_layer_name2;

		res = VK_XR_CreateInstance( qvkCreateInstance, &desc, &vk_instance );

		if ( res == VK_ERROR_LAYER_NOT_PRESENT ) {

			ri.Printf( PRINT_WARNING, "...validation layer is not available\n" );

			// try without validation layer
			desc.enabledLayerCount = 0;
			desc.ppEnabledLayerNames = NULL;

			res = VK_XR_CreateInstance( qvkCreateInstance, &desc, &vk_instance );
		}
	}
#else
	desc.enabledLayerCount = 0;
	desc.ppEnabledLayerNames = NULL;

	res = VK_XR_CreateInstance( qvkCreateInstance, &desc, &vk_instance );
#endif

	ri.Free( (void*)extension_names );
	ri.Free( extension_properties );

	if ( res != VK_SUCCESS ) {
		ri.Error( ERR_FATAL, "Vulkan: instance creation failed with %s", vk_result_string( res ) );
	}
}


static VkFormat get_depth_format( VkPhysicalDevice physical_device ) {
	VkFormatProperties props;
	VkFormat formats[2];
	int i;

	if ( glConfig.stencilBits > 0 ) {
		formats[0] = glConfig.depthBits == 16 ? VK_FORMAT_D16_UNORM_S8_UINT : VK_FORMAT_D24_UNORM_S8_UINT;
		formats[1] = VK_FORMAT_D32_SFLOAT_S8_UINT;
	} else {
		formats[0] = glConfig.depthBits == 16 ? VK_FORMAT_D16_UNORM : VK_FORMAT_X8_D24_UNORM_PACK32;
		formats[1] = VK_FORMAT_D32_SFLOAT;
	}

	for ( i = 0; i < ARRAY_LEN( formats ); i++ ) {
		qvkGetPhysicalDeviceFormatProperties( physical_device, formats[i], &props );
		if ( ( props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT ) != 0 ) {
			return formats[i];
		}
	}

	ri.Error( ERR_FATAL, "get_depth_format: failed to find depth attachment format" );
	return VK_FORMAT_UNDEFINED; // never get here
}


// Check if we can use vkCmdBlitImage for the given source and destination image formats.
static qboolean vk_blit_enabled( VkPhysicalDevice physical_device, const VkFormat srcFormat, const VkFormat dstFormat )
{
	VkFormatProperties formatProps;

	qvkGetPhysicalDeviceFormatProperties( physical_device, srcFormat, &formatProps );
	if ( ( formatProps.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT ) == 0 ) {
		return qfalse;
	}

	qvkGetPhysicalDeviceFormatProperties( physical_device, dstFormat, &formatProps );
	if ( ( formatProps.linearTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT ) == 0 ) {
		return qfalse;
	}

	return qtrue;
}


static VkFormat get_hdr_format( VkFormat base_format )
{
	if ( r_fbo->integer == 0 ) {
		return base_format;
	}

	if ( vk.hdrActive ) {
		// UNORM keeps the framebuffer clamped to [0,1], which Q3's
		// destination-dependent blends (e.g. GL_ONE_MINUS_DST_COLOR) require.
		return VK_FORMAT_R16G16B16A16_UNORM;
	}

	switch ( r_hdr->integer ) {
		case -1: return VK_FORMAT_B4G4R4A4_UNORM_PACK16;
		case 1: return VK_FORMAT_R16G16B16A16_UNORM;
		default: return base_format;
	}
}

typedef struct {
	int bits;
	VkFormat rgb;
	VkFormat bgr;
} present_format_t;

static const present_format_t present_formats[] = {
	//{12, VK_FORMAT_B4G4R4A4_UNORM_PACK16, VK_FORMAT_R4G4B4A4_UNORM_PACK16},
	//{15, VK_FORMAT_B5G5R5A1_UNORM_PACK16, VK_FORMAT_R5G5B5A1_UNORM_PACK16},
	{16, VK_FORMAT_B5G6R5_UNORM_PACK16, VK_FORMAT_R5G6B5_UNORM_PACK16},
	{24, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM},
	{30, VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2R10G10B10_UNORM_PACK32},
	//{32, VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_FORMAT_B10G11R11_UFLOAT_PACK32}
};

static void get_present_format( int present_bits, VkFormat *bgr, VkFormat *rgb ) {
	const present_format_t *pf, *sel;
	int i;

	sel = NULL;
	pf = present_formats;
	for ( i = 0; i < ARRAY_LEN( present_formats ); i++, pf++ ) {
		if ( pf->bits <= present_bits  ) {
			sel = pf;
		}
	}
	if ( !sel ) {
		*bgr = VK_FORMAT_B8G8R8A8_UNORM;
		*rgb = VK_FORMAT_R8G8B8A8_UNORM;
	} else {
		*bgr = sel->bgr;
		*rgb = sel->rgb;
	}
}


typedef enum {
	OSHDR_UNKNOWN = 0,	// could not determine (non-Windows, old OS, or query failed)
	OSHDR_ON,		// an HDR-capable output has the OS HDR switch on
	OSHDR_OFF,		// HDR-capable output present but the OS HDR switch is off
	OSHDR_UNSUPPORTED	// no HDR-capable output found
} osHdrState_t;

#ifdef _WIN32
// These DisplayConfig info-types are enum values (not macros), so they can't be
// probed with the preprocessor and are absent from some MinGW header sets. Use
// private structs matching the documented layouts and info-type values.
//
// On current Windows the classic info (type 9) "advancedColorEnabled" bit tracks
// Advanced Color, which is on for wide-gamut SDR as well, so it reports HDR even
// when the HDR switch is off. The _2 info (type 13) adds activeColorMode, which
// separates SDR (0) / WCG (1) / HDR (2); prefer it and fall back to the classic
// query on systems that lack it.
#define TR_DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO	9
#define TR_DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2	15
#define TR_ADVANCED_COLOR_MODE_HDR			2
typedef struct {
	DISPLAYCONFIG_DEVICE_INFO_HEADER	header;
	UINT32					value;	// bit 0 supported, bit 1 enabled
	UINT32					colorEncoding;
	UINT32					bitsPerColorChannel;
} trAdvancedColorInfo_t;

typedef struct {
	DISPLAYCONFIG_DEVICE_INFO_HEADER	header;
	UINT32					value;	// bit 4 HDR supported, bit 5 HDR user-enabled
	UINT32					colorEncoding;
	UINT32					bitsPerColorChannel;
	UINT32					activeColorMode;
} trAdvancedColorInfo2_t;

static osHdrState_t vk_query_os_hdr_state( void )
{
	UINT32 numPath = 0, numMode = 0;
	DISPLAYCONFIG_PATH_INFO *paths;
	DISPLAYCONFIG_MODE_INFO *modes;
	osHdrState_t result = OSHDR_UNSUPPORTED;
	UINT32 i;

	if ( GetDisplayConfigBufferSizes( QDC_ONLY_ACTIVE_PATHS, &numPath, &numMode ) != ERROR_SUCCESS || numPath == 0 )
		return OSHDR_UNKNOWN;

	paths = (DISPLAYCONFIG_PATH_INFO*)ri.Malloc( numPath * sizeof( *paths ) );
	modes = (DISPLAYCONFIG_MODE_INFO*)ri.Malloc( ( numMode ? numMode : 1 ) * sizeof( *modes ) );

	if ( QueryDisplayConfig( QDC_ONLY_ACTIVE_PATHS, &numPath, paths, &numMode, modes, NULL ) != ERROR_SUCCESS ) {
		ri.Free( paths );
		ri.Free( modes );
		return OSHDR_UNKNOWN;
	}

	for ( i = 0; i < numPath; i++ ) {
		trAdvancedColorInfo2_t info2;
		trAdvancedColorInfo_t info;

		// Preferred: type 13 separates HDR from wide-gamut SDR.
		Com_Memset( &info2, 0, sizeof( info2 ) );
		info2.header.type = (DISPLAYCONFIG_DEVICE_INFO_TYPE)TR_DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2;
		info2.header.size = sizeof( info2 );
		info2.header.adapterId = paths[i].targetInfo.adapterId;
		info2.header.id = paths[i].targetInfo.id;
		if ( DisplayConfigGetDeviceInfo( &info2.header ) == ERROR_SUCCESS ) {
			ri.Printf( PRINT_DEVELOPER, "...OS HDR query (path %u): type-13 value 0x%02x, activeColorMode %u\n",
				(unsigned)i, (unsigned)info2.value, (unsigned)info2.activeColorMode );
			if ( info2.value & 0x10 ) {	// HDR supported
				if ( info2.activeColorMode == TR_ADVANCED_COLOR_MODE_HDR ) {
					result = OSHDR_ON;
					break;
				}
				result = OSHDR_OFF;
			}
			continue;
		}

		// Fallback for systems without type 13.
		Com_Memset( &info, 0, sizeof( info ) );
		info.header.type = (DISPLAYCONFIG_DEVICE_INFO_TYPE)TR_DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO;
		info.header.size = sizeof( info );
		info.header.adapterId = paths[i].targetInfo.adapterId;
		info.header.id = paths[i].targetInfo.id;
		if ( DisplayConfigGetDeviceInfo( &info.header ) == ERROR_SUCCESS && ( info.value & 0x1 ) ) {
			ri.Printf( PRINT_DEVELOPER, "...OS HDR query (path %u): type-9 value 0x%02x\n",
				(unsigned)i, (unsigned)info.value );
			if ( info.value & 0x2 ) {
				result = OSHDR_ON;
				break;
			}
			result = OSHDR_OFF;
		}
	}

	ri.Free( paths );
	ri.Free( modes );
	return result;
}
#else
static osHdrState_t vk_query_os_hdr_state( void )
{
	return OSHDR_UNKNOWN;
}
#endif


static qboolean vk_select_surface_format( VkPhysicalDevice physical_device, VkSurfaceKHR surface )
{
	VkFormat base_bgr, base_rgb;
	VkFormat ext_bgr, ext_rgb;
	VkSurfaceFormatKHR *candidates;
	uint32_t format_count;
	VkResult res;

	res = qvkGetPhysicalDeviceSurfaceFormatsKHR( physical_device, surface, &format_count, NULL );
	if ( res < 0 ) {
		ri.Printf( PRINT_ERROR, "vkGetPhysicalDeviceSurfaceFormatsKHR returned %s\n", vk_result_string( res ) );
		return qfalse;
	}

	if ( format_count == 0 ) {
		ri.Printf( PRINT_ERROR, "...no surface formats found\n" );
		return qfalse;
	}

	candidates = (VkSurfaceFormatKHR*)ri.Malloc( format_count * sizeof(VkSurfaceFormatKHR) );

	VK_CHECK( qvkGetPhysicalDeviceSurfaceFormatsKHR( physical_device, surface, &format_count, candidates ) );

	get_present_format( 24, &base_bgr, &base_rgb );

	if ( r_fbo->integer ) {
		get_present_format( r_presentBits->integer, &ext_bgr, &ext_rgb );
	} else {
		ext_bgr = base_bgr;
		ext_rgb = base_rgb;
	}

	if ( format_count == 1 && candidates[0].format == VK_FORMAT_UNDEFINED ) {
		// special case that means we can choose any format
		vk.base_format.format = base_bgr;
		vk.base_format.colorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR;
		vk.present_format.format = ext_bgr;
		vk.present_format.colorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR;
	}
	else {
		uint32_t i;
		for ( i = 0; i < format_count; i++ ) {
			if ( ( candidates[i].format == base_bgr || candidates[i].format == base_rgb ) && candidates[i].colorSpace == VK_COLORSPACE_SRGB_NONLINEAR_KHR ) {
				vk.base_format = candidates[i];
				break;
			}
		}
		if ( i == format_count ) {
			vk.base_format = candidates[0];
		}
		for ( i = 0; i < format_count; i++ ) {
			if ( ( candidates[i].format == ext_bgr || candidates[i].format == ext_rgb ) && candidates[i].colorSpace == VK_COLORSPACE_SRGB_NONLINEAR_KHR ) {
				vk.present_format = candidates[i];
				break;
			}
		}
		if ( i == format_count ) {
			vk.present_format = vk.base_format;
		}
	}

	vk.hdrActive = qfalse;
	vk.hdrOsState = OSHDR_UNKNOWN;

	if ( r_hdrDisplay->integer && r_fbo->integer && vk.hdrColorspaceExt ) {
		uint32_t h;
		for ( h = 0; h < format_count; h++ ) {
			if ( candidates[h].format == VK_FORMAT_R16G16B16A16_SFLOAT &&
				candidates[h].colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT ) {
#ifdef __APPLE__
				// macOS reports HDR via the layer's EDR headroom, not DisplayConfig:
				// enable extended-dynamic-range on the presentation layer and commit
				// only if the display actually has headroom (else scRGB > 1 clamps).
				if ( ri.VK_ConfigureHDR( qtrue ) > 1.0f ) {
					vk.present_format = candidates[h];
					vk.hdrActive = qtrue;
				} else {
					ri.VK_ConfigureHDR( qfalse );
				}
#else
				// the scRGB colorspace is advertised even when the OS HDR switch is
				// off, which only makes the image look oversaturated; commit to HDR
				// output unless we positively detect HDR as off.
				vk.hdrOsState = vk_query_os_hdr_state();
				if ( vk.hdrOsState != OSHDR_OFF && vk.hdrOsState != OSHDR_UNSUPPORTED ) {
					vk.present_format = candidates[h];
					vk.hdrActive = qtrue;
				}
#endif
				break;
			}
		}
	}

	ri.Cvar_Set( "r_hdrActive", vk.hdrActive ? "1" : "0" );

	if ( !r_fbo->integer ) {
		vk.present_format = vk.base_format;
	}

	ri.Free( candidates );

	return qtrue;
}


static void setup_surface_formats( VkPhysicalDevice physical_device )
{
	vk.depth_format = get_depth_format( physical_device );

	vk.color_format = get_hdr_format( vk.base_format.format );

	if ( vk.hdrActive ) {
		ri.Printf( PRINT_ALL, "...HDR output: scRGB linear FP16 (EXTENDED_SRGB_LINEAR)\n" );
		if ( vk.hdrOsState == OSHDR_UNKNOWN ) {
#ifdef _WIN32
			ri.Printf( PRINT_ALL, "...ensure HDR is enabled in Windows display settings; if the image looks oversaturated, HDR is likely off\n" );
#else
			ri.Printf( PRINT_ALL, "...ensure HDR is enabled in your display settings; if the image looks oversaturated, HDR is likely off\n" );
#endif
		}
	} else if ( r_hdrDisplay->integer ) {
		if ( vk.hdrOsState == OSHDR_OFF ) {
#ifdef _WIN32
			ri.Printf( PRINT_ALL, "...HDR requested but the Windows HDR switch is off; using SDR. Enable HDR in Windows display settings and run \\vid_restart\n" );
#else
			ri.Printf( PRINT_ALL, "...HDR requested but your display's HDR switch is off; using SDR. Enable HDR in your display settings and run \\vid_restart\n" );
#endif
		} else if ( vk.hdrOsState == OSHDR_UNSUPPORTED ) {
			ri.Printf( PRINT_ALL, "...HDR requested but no HDR-capable display was found; using SDR\n" );
		} else {
			ri.Printf( PRINT_ALL, "...HDR output requested but unavailable (need VK_EXT_swapchain_colorspace + R16G16B16A16_SFLOAT/EXTENDED_SRGB_LINEAR + r_fbo); using SDR\n" );
		}
	}

	vk.capture_format = VK_FORMAT_R8G8B8A8_UNORM;

	vk.bloom_format = vk.base_format.format;

	vk.blitEnabled = vk_blit_enabled( physical_device, vk.color_format, vk.capture_format );

	if ( !vk.blitEnabled )
	{
		vk.capture_format = vk.color_format;
	}
}


static const char *renderer_name( const VkPhysicalDeviceProperties *props ) {
	static char buf[sizeof( props->deviceName ) + 64];
	const char *device_type;

	switch ( props->deviceType ) {
		case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: device_type = "Integrated"; break;
		case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: device_type = "Discrete"; break;
		case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: device_type = "Virtual"; break;
		case VK_PHYSICAL_DEVICE_TYPE_CPU: device_type = "CPU"; break;
		default: device_type = "OTHER"; break;
	}

	Com_sprintf( buf, sizeof( buf ), "%s %s, 0x%04x",
		device_type, props->deviceName, props->deviceID );

	return buf;
}


static qboolean vk_create_device( VkPhysicalDevice physical_device, int device_index ) {

#ifdef _DEBUG
	VkPhysicalDeviceTimelineSemaphoreFeatures timeline_semaphore;
	VkPhysicalDeviceVulkanMemoryModelFeatures memory_model;
	VkPhysicalDeviceBufferDeviceAddressFeatures devaddr_features;
	VkPhysicalDevice8BitStorageFeatures storage_8bit_features;
#endif

	ri.Printf( PRINT_ALL, "...selected physical device: %i\n", device_index );

	// select surface format
	if ( !vk_select_surface_format( physical_device, vk_surface ) ) {
		return qfalse;
	}

	setup_surface_formats( physical_device );

	// select queue family
	{
		VkQueueFamilyProperties *queue_families;
		uint32_t queue_family_count;
		uint32_t i;

		qvkGetPhysicalDeviceQueueFamilyProperties( physical_device, &queue_family_count, NULL );
		queue_families = (VkQueueFamilyProperties*)ri.Malloc( queue_family_count * sizeof( VkQueueFamilyProperties ) );
		qvkGetPhysicalDeviceQueueFamilyProperties( physical_device, &queue_family_count, queue_families );

		// select queue family with presentation and graphics support
		vk.queue_family_index = ~0U;
		for (i = 0; i < queue_family_count; i++) {
			VkBool32 presentation_supported;
			VK_CHECK( qvkGetPhysicalDeviceSurfaceSupportKHR( physical_device, i, vk_surface, &presentation_supported ) );

			if (presentation_supported && (queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
				vk.queue_family_index = i;
				vk.timestampValidBits = queue_families[i].timestampValidBits;
				break;
			}
		}

		ri.Free( queue_families );

		if ( vk.queue_family_index == ~0U ) {
			ri.Printf( PRINT_ERROR, "...failed to find graphics queue family\n" );

			return qfalse;
		}
	}

	// create VkDevice
	{
		const char *device_extension_list[24];
		uint32_t device_extension_count;
		const char *ext, *end;
		char *str;
		const float priority = 1.0;
		VkExtensionProperties *extension_properties;
		VkDeviceQueueCreateInfo queue_desc;
		VkPhysicalDeviceFeatures device_features;
		VkPhysicalDeviceFeatures features;
		VkDeviceCreateInfo device_desc;
		qboolean multiviewExtension = qfalse;
		qboolean multiviewAdvertised = qfalse;
		VkResult res;
		qboolean swapchainSupported = qfalse;
		qboolean dedicatedAllocation = qfalse;
		qboolean memoryRequirements2 = qfalse;
		qboolean debugMarker = qfalse;
		qboolean imageFormatList = qfalse;
#ifdef _DEBUG
		qboolean timelineSemaphore = qfalse;
		qboolean memoryModel = qfalse;
		qboolean devAddrFeat = qfalse;
		qboolean storage8bit = qfalse;
		const void** pNextPtr;
#endif
		uint32_t i, len, count = 0;

		VK_CHECK( qvkEnumerateDeviceExtensionProperties( physical_device, NULL, &count, NULL ) );
		extension_properties = (VkExtensionProperties*)ri.Malloc( count * sizeof( VkExtensionProperties ) );
		VK_CHECK( qvkEnumerateDeviceExtensionProperties( physical_device, NULL, &count, extension_properties ) );

		// fill glConfig.extensions_string
		str = glConfig.extensions_string; *str = '\0';
		end = &glConfig.extensions_string[ sizeof( glConfig.extensions_string ) - 1];

		for ( i = 0; i < count; i++ ) {
			ext = extension_properties[i].extensionName;
			if ( strcmp( ext, VK_KHR_SWAPCHAIN_EXTENSION_NAME ) == 0 ) {
				swapchainSupported = qtrue;
			} else if ( strcmp( ext, VK_KHR_MULTIVIEW_EXTENSION_NAME ) == 0 ) {
				multiviewAdvertised = qtrue;
			} else if ( strcmp( ext, VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME ) == 0 ) {
				dedicatedAllocation = qtrue;
			} else if ( strcmp( ext, VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME ) == 0 ) {
				memoryRequirements2 = qtrue;
			} else if ( strcmp( ext, VK_EXT_DEBUG_MARKER_EXTENSION_NAME ) == 0 ) {
				debugMarker = qtrue;
			} else if ( strcmp( ext, VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME ) == 0 ) {
				imageFormatList = qtrue;
#ifdef _DEBUG
			} else if ( strcmp( ext, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME ) == 0 ) {
				timelineSemaphore = qtrue;
			} else if ( strcmp( ext, VK_KHR_VULKAN_MEMORY_MODEL_EXTENSION_NAME ) == 0 ) {
				memoryModel = qtrue;
			} else if ( strcmp( ext, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME ) == 0 ) {
				devAddrFeat = qtrue;
			} else if ( strcmp( ext, VK_KHR_8BIT_STORAGE_EXTENSION_NAME ) == 0 ) {
				storage8bit = qtrue;
#endif
			}
			// add this device extension to glConfig
			if ( i != 0 ) {
				if ( str + 1 >= end )
					continue;
				str = Q_stradd( str, " " );
			}
			len = (uint32_t)strlen( ext );
			if ( str + len >= end )
				continue;
			str = Q_stradd( str, ext );
		}

		ri.Free( extension_properties );

		device_extension_count = 0;

		if ( !swapchainSupported ) {
			ri.Printf( PRINT_ERROR, "...required device extension is not available: %s\n", VK_KHR_SWAPCHAIN_EXTENSION_NAME );
			return qfalse;
		}

		if ( !memoryRequirements2 )
			dedicatedAllocation = qfalse;
		else
			vk.dedicatedAllocation = dedicatedAllocation;

#ifndef USE_DEDICATED_ALLOCATION
		vk.dedicatedAllocation = qfalse;
#endif

		device_extension_list[ device_extension_count++ ] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;

		if ( vk.dedicatedAllocation ) {
			device_extension_list[ device_extension_count++ ] = VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME;
			device_extension_list[ device_extension_count++ ] = VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME;
		}

		if ( debugMarker ) {
			device_extension_list[ device_extension_count++ ] = VK_EXT_DEBUG_MARKER_EXTENSION_NAME;
			vk.debugMarkers = qtrue;
		}

		if ( imageFormatList ) {
			device_extension_list[ device_extension_count++ ] = VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME;
			vk.imageFormatList = qtrue;
			ri.Printf( PRINT_ALL, "...using %s\n", VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME );
		}
#ifdef _DEBUG
		if ( timelineSemaphore ) {
			device_extension_list[ device_extension_count++ ] = VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME;
		}

		if ( memoryModel ) {
			device_extension_list[ device_extension_count++ ] = VK_KHR_VULKAN_MEMORY_MODEL_EXTENSION_NAME;
		}

		if ( devAddrFeat ) {
			device_extension_list[ device_extension_count++ ] = VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME;
		}

		if ( storage8bit ) {
			device_extension_list[ device_extension_count++ ] = VK_KHR_8BIT_STORAGE_EXTENSION_NAME;
		}
#endif // _DEBUG
		if ( VK_XR_Enabled() ) {
			if ( !vk_query_multiview( physical_device, &multiviewExtension ) ||
				(multiviewExtension && !multiviewAdvertised) ) {
				ri.Error( ERR_DROP, "VR requires Vulkan multiview with at least two views" );
				return qfalse;
			}
			vk.multiview = qtrue;
			if ( multiviewExtension )
				device_extension_list[device_extension_count++] = VK_KHR_MULTIVIEW_EXTENSION_NAME;
		}
		Com_Memset( &vk_foveation_caps, 0, sizeof( vk_foveation_caps ) );
		if ( VK_XR_Enabled() ) {
			PFN_vkGetInstanceProcAddr proc = (PFN_vkGetInstanceProcAddr)ri.VK_GetInstanceProcAddr(
				VK_NULL_HANDLE, "vkGetInstanceProcAddr" );
			VkResult result =
				VK_FovQuery( &vk_foveation_caps, vk_instance, physical_device, proc, vk_instance_api );
			if ( result != VK_SUCCESS ) {
				vk_foveation_caps.supported = 0;
				vk_foveation_caps.backend = VK_FOV_BACKEND_NONE;
				vk_foveation_caps.extensionCount = 0;
			}
			ri.Printf( PRINT_ALL, "Vulkan foveation: %s (API %u.%u, %u device extensions, result %d)\n",
					   vk_foveation_caps.reason ? vk_foveation_caps.reason : "capability query unavailable",
					   VK_VERSION_MAJOR( vk_instance_api ), VK_VERSION_MINOR( vk_instance_api ),
					   vk_foveation_caps.availableExtensions, (int)result );
			if ( vk_foveation_caps.fdmReason )
				ri.Printf( PRINT_ALL, "Vulkan foveation: no density map: %s\n", vk_foveation_caps.fdmReason );
			if ( vk_foveation_caps.backend == VK_FOV_BACKEND_FDM ) {
				ri.Printf( PRINT_ALL, "Vulkan foveation: FDM (texel %ux%u..%ux%u, density map 2 %s, offsets %s granularity %ux%u, tile properties %s)\n",
						   vk_foveation_caps.texelWidth, vk_foveation_caps.texelHeight,
						   vk_foveation_caps.fdmTexelMaxWidth, vk_foveation_caps.fdmTexelMaxHeight,
						   vk_foveation_caps.fdm2 ? "yes" : "no",
						   vk_foveation_caps.fdmOffset ? vk_foveation_caps.fdmOffsetExtension : "none",
						   vk_foveation_caps.fdmOffsetGranularity.width, vk_foveation_caps.fdmOffsetGranularity.height,
						   vk_foveation_caps.tileProperties ? "yes" : "no" );
			}
			for ( i = 0; i < vk_foveation_caps.extensionCount; i++ ) {
				device_extension_list[device_extension_count++] = vk_foveation_caps.extensions[i];
			}
		}
		qvkGetPhysicalDeviceFeatures( physical_device, &device_features );

		if ( device_features.fillModeNonSolid == VK_FALSE ) {
			ri.Printf( PRINT_ERROR, "...fillModeNonSolid feature is not supported\n" );
			return qfalse;
		}

		queue_desc.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		queue_desc.pNext = NULL;
		queue_desc.flags = 0;
		queue_desc.queueFamilyIndex = vk.queue_family_index;
		queue_desc.queueCount = 1;
		queue_desc.pQueuePriorities = &priority;

		Com_Memset( &features, 0, sizeof( features ) );
		features.fillModeNonSolid = VK_TRUE;

#ifdef _DEBUG
		if ( device_features.shaderInt64 ) {
			features.shaderInt64 = VK_TRUE;
		}
#endif
		if ( device_features.wideLines ) { // needed for RB_SurfaceAxis
			features.wideLines = VK_TRUE;
			vk.wideLines = qtrue;
		}

		if ( device_features.fragmentStoresAndAtomics && device_features.vertexPipelineStoresAndAtomics ) {
			features.vertexPipelineStoresAndAtomics = VK_TRUE;
			features.fragmentStoresAndAtomics = VK_TRUE;
			vk.fragmentStores = qtrue;
		}

		if ( r_ext_texture_filter_anisotropic->integer && device_features.samplerAnisotropy ) {
			features.samplerAnisotropy = VK_TRUE;
			vk.samplerAnisotropy = qtrue;
		}

		if ( device_features.depthClamp ) {
			features.depthClamp = VK_TRUE;
			vk.depthClamp = qtrue;
		}

		if ( device_features.independentBlend ) {
			// the HDR emissive MRT gives the emissive attachment a different blend
			// state than the color attachment
			features.independentBlend = VK_TRUE;
		}

		device_desc.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
		device_desc.pNext = NULL;
		device_desc.flags = 0;
		device_desc.queueCreateInfoCount = 1;
		device_desc.pQueueCreateInfos = &queue_desc;
		device_desc.enabledLayerCount = 0;
		device_desc.ppEnabledLayerNames = NULL;
		device_desc.enabledExtensionCount = device_extension_count;
		device_desc.ppEnabledExtensionNames = device_extension_list;
		device_desc.pEnabledFeatures = &features;

#ifdef _DEBUG
		pNextPtr = (const void **)&device_desc.pNext;

		if ( timelineSemaphore ) {
			*pNextPtr = &timeline_semaphore;
			timeline_semaphore.pNext = NULL;
			timeline_semaphore.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
			timeline_semaphore.timelineSemaphore = VK_TRUE;
			pNextPtr = (const void **)&timeline_semaphore.pNext;
		}

		if ( memoryModel ) {
			*pNextPtr = &memory_model;
			memory_model.pNext = NULL;
			memory_model.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES;
			memory_model.vulkanMemoryModel = VK_TRUE;
			memory_model.vulkanMemoryModelAvailabilityVisibilityChains = VK_FALSE;
			memory_model.vulkanMemoryModelDeviceScope = VK_TRUE;
			pNextPtr = (const void **)&memory_model.pNext;
		}

		if ( devAddrFeat ) {
			*pNextPtr = &devaddr_features;
			devaddr_features.pNext = NULL;
			devaddr_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
			devaddr_features.bufferDeviceAddress = VK_TRUE;
			devaddr_features.bufferDeviceAddressCaptureReplay = VK_FALSE;
			devaddr_features.bufferDeviceAddressMultiDevice = VK_FALSE;
			pNextPtr = (const void **)&devaddr_features.pNext;
		}

		if ( storage8bit ) {
			*pNextPtr = &storage_8bit_features;
			storage_8bit_features.pNext = NULL;
			storage_8bit_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES;
			storage_8bit_features.storageBuffer8BitAccess = VK_TRUE;
			storage_8bit_features.storagePushConstant8 = VK_FALSE;
			storage_8bit_features.uniformAndStorageBuffer8BitAccess = VK_TRUE;
			pNextPtr = (const void **)&storage_8bit_features.pNext;
		}
#endif
		device_desc.pNext = VK_FovFeatures( &vk_foveation_caps, (void *)device_desc.pNext );
		if ( vk.multiview ) {
			vk_multiview_features.pNext = (void *)device_desc.pNext;
			device_desc.pNext = &vk_multiview_features;
		}
		res = VK_XR_CreateDevice( qvkCreateDevice, physical_device, &device_desc, &vk.device );
		if ( res < 0 ) {
			ri.Printf( PRINT_ERROR, "vkCreateDevice returned %s\n", vk_result_string( res ) );
			return qfalse;
		}
	}

	return qtrue;
}


#define INIT_INSTANCE_FUNCTION(func) \
	q##func = /*(PFN_ ## func)*/ ri.VK_GetInstanceProcAddr(vk_instance, #func); \
	if (q##func == NULL) {											\
		ri.Error(ERR_FATAL, "Failed to find entrypoint %s", #func);	\
	}

#define INIT_INSTANCE_FUNCTION_EXT(func) \
	q##func = /*(PFN_ ## func)*/ ri.VK_GetInstanceProcAddr(vk_instance, #func);


#define INIT_DEVICE_FUNCTION(func) \
	q##func = (PFN_ ## func) qvkGetDeviceProcAddr(vk.device, #func);\
	if (q##func == NULL) {											\
		ri.Error(ERR_FATAL, "Failed to find entrypoint %s", #func);	\
	}

#define INIT_DEVICE_FUNCTION_EXT(func) \
	q##func = (PFN_ ## func) qvkGetDeviceProcAddr(vk.device, #func);


static void vk_destroy_instance( void ) {
	if ( vk_surface != VK_NULL_HANDLE ) {
		if ( qvkDestroySurfaceKHR != NULL ) {
			qvkDestroySurfaceKHR( vk_instance, vk_surface, NULL );
		}
		vk_surface = VK_NULL_HANDLE;
	}

#ifdef USE_VK_VALIDATION
	if ( vk_debug_callback ) {
		if ( qvkDestroyDebugReportCallbackEXT != NULL ) {
			qvkDestroyDebugReportCallbackEXT( vk_instance, vk_debug_callback, NULL );
		}
		vk_debug_callback = VK_NULL_HANDLE;
	}
#endif

	if ( vk_instance != VK_NULL_HANDLE ) {
		if ( qvkDestroyInstance ) {
			qvkDestroyInstance( vk_instance, NULL );
		}
		vk_instance = VK_NULL_HANDLE;
	}
}


static void init_vulkan_library( void )
{
	VkPhysicalDeviceProperties props;
	VkPhysicalDevice *physical_devices;
	uint32_t device_count;
	int device_index, i;
	VkResult res;

	Com_Memset( &vk, 0, sizeof( vk ) );

	if ( vk_instance == VK_NULL_HANDLE ) {

		// force cleanup
		vk_destroy_instance();

		// Get functions that do not depend on VkInstance (vk_instance == nullptr at this point).
		INIT_INSTANCE_FUNCTION( vkCreateInstance )
		INIT_INSTANCE_FUNCTION( vkEnumerateInstanceExtensionProperties )

		// Get instance level functions.
		create_instance();

		INIT_INSTANCE_FUNCTION( vkCreateDevice )
		INIT_INSTANCE_FUNCTION( vkDestroyInstance )
		INIT_INSTANCE_FUNCTION( vkEnumerateDeviceExtensionProperties )
		INIT_INSTANCE_FUNCTION( vkEnumeratePhysicalDevices )
		INIT_INSTANCE_FUNCTION( vkGetDeviceProcAddr )
		INIT_INSTANCE_FUNCTION( vkGetPhysicalDeviceFeatures )
		INIT_INSTANCE_FUNCTION( vkGetPhysicalDeviceFormatProperties )
		INIT_INSTANCE_FUNCTION( vkGetPhysicalDeviceMemoryProperties )
		INIT_INSTANCE_FUNCTION( vkGetPhysicalDeviceProperties )
		INIT_INSTANCE_FUNCTION( vkGetPhysicalDeviceQueueFamilyProperties )
		INIT_INSTANCE_FUNCTION( vkDestroySurfaceKHR )
		INIT_INSTANCE_FUNCTION( vkGetPhysicalDeviceSurfaceCapabilitiesKHR )
		INIT_INSTANCE_FUNCTION( vkGetPhysicalDeviceSurfaceFormatsKHR )
		INIT_INSTANCE_FUNCTION( vkGetPhysicalDeviceSurfacePresentModesKHR )
		INIT_INSTANCE_FUNCTION( vkGetPhysicalDeviceSurfaceSupportKHR )

#ifdef USE_VK_VALIDATION
		INIT_INSTANCE_FUNCTION_EXT( vkCreateDebugReportCallbackEXT )
		INIT_INSTANCE_FUNCTION_EXT( vkDestroyDebugReportCallbackEXT )

		// Create debug callback.
		if ( qvkCreateDebugReportCallbackEXT && qvkDestroyDebugReportCallbackEXT ) {
			VkDebugReportCallbackCreateInfoEXT desc;
			desc.sType = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT;
			desc.pNext = NULL;
			desc.flags = VK_DEBUG_REPORT_WARNING_BIT_EXT |
				VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT |
				VK_DEBUG_REPORT_ERROR_BIT_EXT;
			desc.pfnCallback = &debug_callback;
			desc.pUserData = NULL;

			VK_CHECK( qvkCreateDebugReportCallbackEXT( vk_instance, &desc, NULL, &vk_debug_callback ) );
		}
#endif

		// create surface
		if ( !ri.VK_CreateSurface( vk_instance, &vk_surface ) ) {
			ri.Error( ERR_FATAL, "Error creating Vulkan surface" );
			return;
		}
	} // vk_instance == VK_NULL_HANDLE

	res = qvkEnumeratePhysicalDevices( vk_instance, &device_count, NULL );
	if ( device_count == 0 ) {
		ri.Error( ERR_FATAL, "Vulkan: no physical devices found" );
		return;
	}
	else if ( res < 0 ) {
		ri.Error( ERR_FATAL, "vkEnumeratePhysicalDevices returned %s", vk_result_string( res ) );
		return;
	}

	physical_devices = (VkPhysicalDevice*)ri.Malloc( device_count * sizeof( VkPhysicalDevice ) );
	VK_CHECK( qvkEnumeratePhysicalDevices( vk_instance, &device_count, physical_devices ) );

	// initial physical device index
	device_index = r_device->integer;

	ri.Printf( PRINT_ALL, ".......................\nAvailable physical devices:\n" );
	for ( i = 0; i < device_count; i++ ) {
		qvkGetPhysicalDeviceProperties( physical_devices[ i ], &props );
		ri.Printf( PRINT_ALL, " %i: %s\n", i, renderer_name( &props ) );
		if ( device_index == -1 && props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ) {
			device_index = i;
		} else if ( device_index == -2 && props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ) {
			device_index = i;
		}
	}
	ri.Printf( PRINT_ALL, ".......................\n" );

	vk.physical_device = VK_NULL_HANDLE;
	if ( VK_XR_Enabled() ) {
		VkPhysicalDevice required = VK_XR_PhysicalDevice( vk_instance );
		for ( i = 0; i < device_count; i++ ) {
			if ( physical_devices[i] == required && vk_create_device( required, i ) ) {
				vk.physical_device = required;
				break;
			}
		}
	} else
		for ( i = 0; i < device_count; i++, device_index++ ) {
			if ( device_index >= device_count || device_index < 0 ) {
				device_index = 0;
			}
			if ( vk_create_device( physical_devices[device_index], device_index ) ) {
				vk.physical_device = physical_devices[device_index];
				break;
			}
		}

	ri.Free( physical_devices );

	if ( vk.physical_device == VK_NULL_HANDLE ) {
		ri.Error( ERR_FATAL, "Vulkan: unable to find any suitable physical device" );
		return;
	}

	//
	// Get device level functions.
	//
	INIT_DEVICE_FUNCTION(vkAllocateCommandBuffers)
	INIT_DEVICE_FUNCTION(vkAllocateDescriptorSets)
	INIT_DEVICE_FUNCTION(vkAllocateMemory)
	INIT_DEVICE_FUNCTION(vkBeginCommandBuffer)
	INIT_DEVICE_FUNCTION( vkCreateQueryPool )
	INIT_DEVICE_FUNCTION( vkDestroyQueryPool )
	INIT_DEVICE_FUNCTION( vkGetQueryPoolResults )
	INIT_DEVICE_FUNCTION( vkCmdResetQueryPool )
	INIT_DEVICE_FUNCTION( vkCmdWriteTimestamp )
	INIT_DEVICE_FUNCTION(vkBindBufferMemory)
	INIT_DEVICE_FUNCTION(vkBindImageMemory)
	INIT_DEVICE_FUNCTION(vkCmdBeginRenderPass)
	INIT_DEVICE_FUNCTION(vkCmdBindDescriptorSets)
	INIT_DEVICE_FUNCTION(vkCmdBindIndexBuffer)
	INIT_DEVICE_FUNCTION(vkCmdBindPipeline)
	INIT_DEVICE_FUNCTION(vkCmdBindVertexBuffers)
	INIT_DEVICE_FUNCTION(vkCmdBlitImage)
	INIT_DEVICE_FUNCTION(vkCmdClearAttachments)
	INIT_DEVICE_FUNCTION( vkCmdClearColorImage )
	INIT_DEVICE_FUNCTION(vkCmdCopyBuffer)
	INIT_DEVICE_FUNCTION(vkCmdCopyBufferToImage)
	INIT_DEVICE_FUNCTION(vkCmdCopyImage)
	INIT_DEVICE_FUNCTION(vkCmdDraw)
	INIT_DEVICE_FUNCTION(vkCmdDrawIndexed)
	INIT_DEVICE_FUNCTION(vkCmdEndRenderPass)
	INIT_DEVICE_FUNCTION(vkCmdNextSubpass)
	INIT_DEVICE_FUNCTION(vkCmdPipelineBarrier)
	INIT_DEVICE_FUNCTION(vkCmdPushConstants)
	INIT_DEVICE_FUNCTION(vkCmdSetDepthBias)
	INIT_DEVICE_FUNCTION(vkCmdSetScissor)
	INIT_DEVICE_FUNCTION(vkCmdSetViewport)
	INIT_DEVICE_FUNCTION(vkCreateBuffer)
	INIT_DEVICE_FUNCTION(vkCreateCommandPool)
	INIT_DEVICE_FUNCTION(vkCreateDescriptorPool)
	INIT_DEVICE_FUNCTION(vkCreateDescriptorSetLayout)
	INIT_DEVICE_FUNCTION(vkCreateFence)
	INIT_DEVICE_FUNCTION(vkCreateFramebuffer)
	INIT_DEVICE_FUNCTION(vkCreateGraphicsPipelines)
	INIT_DEVICE_FUNCTION(vkCreateImage)
	INIT_DEVICE_FUNCTION(vkCreateImageView)
	INIT_DEVICE_FUNCTION(vkCreatePipelineCache)
	INIT_DEVICE_FUNCTION(vkCreatePipelineLayout)
	INIT_DEVICE_FUNCTION(vkCreateRenderPass)
	INIT_DEVICE_FUNCTION(vkCreateSampler)
	INIT_DEVICE_FUNCTION(vkCreateSemaphore)
	INIT_DEVICE_FUNCTION(vkCreateShaderModule)
	INIT_DEVICE_FUNCTION(vkDestroyBuffer)
	INIT_DEVICE_FUNCTION(vkDestroyCommandPool)
	INIT_DEVICE_FUNCTION(vkDestroyDescriptorPool)
	INIT_DEVICE_FUNCTION(vkDestroyDescriptorSetLayout)
	INIT_DEVICE_FUNCTION(vkDestroyDevice)
	INIT_DEVICE_FUNCTION(vkDestroyFence)
	INIT_DEVICE_FUNCTION(vkDestroyFramebuffer)
	INIT_DEVICE_FUNCTION(vkDestroyImage)
	INIT_DEVICE_FUNCTION(vkDestroyImageView)
	INIT_DEVICE_FUNCTION(vkDestroyPipeline)
	INIT_DEVICE_FUNCTION(vkDestroyPipelineCache)
	INIT_DEVICE_FUNCTION(vkDestroyPipelineLayout)
	INIT_DEVICE_FUNCTION(vkDestroyRenderPass)
	INIT_DEVICE_FUNCTION(vkDestroySampler)
	INIT_DEVICE_FUNCTION(vkDestroySemaphore)
	INIT_DEVICE_FUNCTION(vkDestroyShaderModule)
	INIT_DEVICE_FUNCTION(vkDeviceWaitIdle)
	INIT_DEVICE_FUNCTION(vkEndCommandBuffer)
	INIT_DEVICE_FUNCTION(vkFlushMappedMemoryRanges)
	INIT_DEVICE_FUNCTION(vkFreeCommandBuffers)
	INIT_DEVICE_FUNCTION(vkFreeDescriptorSets)
	INIT_DEVICE_FUNCTION(vkFreeMemory)
	INIT_DEVICE_FUNCTION(vkGetBufferMemoryRequirements)
	INIT_DEVICE_FUNCTION(vkGetDeviceQueue)
	INIT_DEVICE_FUNCTION(vkGetImageMemoryRequirements)
	INIT_DEVICE_FUNCTION(vkGetImageSubresourceLayout)
	INIT_DEVICE_FUNCTION(vkInvalidateMappedMemoryRanges)
	INIT_DEVICE_FUNCTION(vkMapMemory)
	INIT_DEVICE_FUNCTION(vkQueueSubmit)
	INIT_DEVICE_FUNCTION(vkQueueWaitIdle)
	INIT_DEVICE_FUNCTION(vkResetCommandBuffer)
	INIT_DEVICE_FUNCTION(vkResetDescriptorPool)
	INIT_DEVICE_FUNCTION(vkResetFences)
	INIT_DEVICE_FUNCTION(vkUnmapMemory)
	INIT_DEVICE_FUNCTION(vkUpdateDescriptorSets)
	INIT_DEVICE_FUNCTION(vkWaitForFences)
	INIT_DEVICE_FUNCTION(vkAcquireNextImageKHR)
	INIT_DEVICE_FUNCTION(vkCreateSwapchainKHR)
	INIT_DEVICE_FUNCTION(vkDestroySwapchainKHR)
	INIT_DEVICE_FUNCTION(vkGetSwapchainImagesKHR)
	INIT_DEVICE_FUNCTION(vkQueuePresentKHR)

	if ( vk.dedicatedAllocation ) {
		INIT_DEVICE_FUNCTION_EXT(vkGetBufferMemoryRequirements2KHR);
		INIT_DEVICE_FUNCTION_EXT(vkGetImageMemoryRequirements2KHR);
		if ( !qvkGetBufferMemoryRequirements2KHR || !qvkGetImageMemoryRequirements2KHR ) {
			vk.dedicatedAllocation = qfalse;
		}
	}

	if ( vk.debugMarkers ) {
		INIT_DEVICE_FUNCTION_EXT(vkDebugMarkerSetObjectNameEXT)
	}

	vk_foveation_load_functions();
}

#undef INIT_INSTANCE_FUNCTION
#undef INIT_DEVICE_FUNCTION
#undef INIT_DEVICE_FUNCTION_EXT

static void deinit_instance_functions( void )
{
	qvkCreateInstance = NULL;
	qvkEnumerateInstanceExtensionProperties = NULL;

	// instance functions:
	qvkCreateDevice = NULL;
	qvkDestroyInstance = NULL;
	qvkEnumerateDeviceExtensionProperties = NULL;
	qvkEnumeratePhysicalDevices = NULL;
	qvkGetDeviceProcAddr = NULL;
	qvkGetPhysicalDeviceFeatures = NULL;
	qvkGetPhysicalDeviceFormatProperties = NULL;
	qvkGetPhysicalDeviceMemoryProperties = NULL;
	qvkGetPhysicalDeviceProperties = NULL;
	qvkGetPhysicalDeviceQueueFamilyProperties = NULL;
	qvkDestroySurfaceKHR = NULL;
	qvkGetPhysicalDeviceSurfaceCapabilitiesKHR = NULL;
	qvkGetPhysicalDeviceSurfaceFormatsKHR = NULL;
	qvkGetPhysicalDeviceSurfacePresentModesKHR = NULL;
	qvkGetPhysicalDeviceSurfaceSupportKHR = NULL;
#ifdef USE_VK_VALIDATION
	qvkCreateDebugReportCallbackEXT = NULL;
	qvkDestroyDebugReportCallbackEXT = NULL;
#endif
}


static void deinit_device_functions( void )
{
	// device functions:
	qvkAllocateCommandBuffers					= NULL;
	qvkAllocateDescriptorSets					= NULL;
	qvkAllocateMemory							= NULL;
	qvkBeginCommandBuffer						= NULL;
	qvkBindBufferMemory							= NULL;
	qvkBindImageMemory							= NULL;
	qvkCmdBeginRenderPass						= NULL;
	qvkCmdBindDescriptorSets					= NULL;
	qvkCmdBindIndexBuffer						= NULL;
	qvkCmdBindPipeline							= NULL;
	qvkCmdBindVertexBuffers						= NULL;
	qvkCmdBlitImage								= NULL;
	qvkCmdClearAttachments						= NULL;
	qvkCmdCopyBuffer							= NULL;
	qvkCmdCopyBufferToImage						= NULL;
	qvkCmdCopyImage								= NULL;
	qvkCmdDraw									= NULL;
	qvkCmdDrawIndexed							= NULL;
	qvkCmdEndRenderPass							= NULL;
	qvkCmdNextSubpass							= NULL;
	qvkCmdPipelineBarrier						= NULL;
	qvkCmdPushConstants							= NULL;
	qvkCmdSetDepthBias							= NULL;
	qvkCmdSetScissor							= NULL;
	qvkCmdSetViewport							= NULL;
	qvkCreateBuffer								= NULL;
	qvkCreateCommandPool						= NULL;
	qvkCreateDescriptorPool						= NULL;
	qvkCreateDescriptorSetLayout				= NULL;
	qvkCreateFence								= NULL;
	qvkCreateFramebuffer						= NULL;
	qvkCreateGraphicsPipelines					= NULL;
	qvkCreateImage								= NULL;
	qvkCreateImageView							= NULL;
	qvkCreatePipelineCache						= NULL;
	qvkCreatePipelineLayout						= NULL;
	qvkCreateRenderPass							= NULL;
	qvkCreateSampler							= NULL;
	qvkCreateSemaphore							= NULL;
	qvkCreateShaderModule						= NULL;
	qvkDestroyBuffer							= NULL;
	qvkDestroyCommandPool						= NULL;
	qvkDestroyDescriptorPool					= NULL;
	qvkDestroyDescriptorSetLayout				= NULL;
	qvkDestroyDevice							= NULL;
	qvkDestroyFence								= NULL;
	qvkDestroyFramebuffer						= NULL;
	qvkDestroyImage								= NULL;
	qvkDestroyImageView							= NULL;
	qvkDestroyPipeline							= NULL;
	qvkDestroyPipelineCache						= NULL;
	qvkDestroyPipelineLayout					= NULL;
	qvkDestroyRenderPass						= NULL;
	qvkDestroySampler							= NULL;
	qvkDestroySemaphore							= NULL;
	qvkDestroyShaderModule						= NULL;
	qvkDeviceWaitIdle							= NULL;
	qvkEndCommandBuffer							= NULL;
	qvkFlushMappedMemoryRanges					= NULL;
	qvkFreeCommandBuffers						= NULL;
	qvkFreeDescriptorSets						= NULL;
	qvkFreeMemory								= NULL;
	qvkGetBufferMemoryRequirements				= NULL;
	qvkGetDeviceQueue							= NULL;
	qvkGetImageMemoryRequirements				= NULL;
	qvkGetImageSubresourceLayout				= NULL;
	qvkInvalidateMappedMemoryRanges				= NULL;
	qvkMapMemory								= NULL;
	qvkQueueSubmit								= NULL;
	qvkQueueWaitIdle							= NULL;
	qvkResetCommandBuffer						= NULL;
	qvkResetDescriptorPool						= NULL;
	qvkResetFences								= NULL;
	qvkUnmapMemory								= NULL;
	qvkUpdateDescriptorSets						= NULL;
	qvkWaitForFences							= NULL;
	qvkAcquireNextImageKHR						= NULL;
	qvkCreateSwapchainKHR						= NULL;
	qvkDestroySwapchainKHR						= NULL;
	qvkGetSwapchainImagesKHR					= NULL;
	qvkQueuePresentKHR							= NULL;

	qvkGetBufferMemoryRequirements2KHR			= NULL;
	qvkGetImageMemoryRequirements2KHR			= NULL;

	qvkDebugMarkerSetObjectNameEXT				= NULL;
}


static VkShaderModule SHADER_MODULE(const uint8_t *bytes, const int count) {
	VkShaderModuleCreateInfo desc;
	VkShaderModule module;

	if ( count % 4 != 0 ) {
		ri.Error( ERR_FATAL, "Vulkan: SPIR-V binary buffer size is not a multiple of 4" );
	}

	desc.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	desc.pNext = NULL;
	desc.flags = 0;
	desc.codeSize = count;
	desc.pCode = (const uint32_t*)bytes;

	VK_CHECK(qvkCreateShaderModule(vk.device, &desc, NULL, &module));

	return module;
}


static void vk_create_layout_binding( int binding, VkDescriptorType type, VkShaderStageFlags flags, VkDescriptorSetLayout *layout )
{
	VkDescriptorSetLayoutBinding bind;
	VkDescriptorSetLayoutCreateInfo desc;

	bind.binding = binding;
	bind.descriptorType = type;
	bind.descriptorCount = 1;
	bind.stageFlags = flags;
	bind.pImmutableSamplers = NULL;

	desc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	desc.pNext = NULL;
	desc.flags = 0;
	desc.bindingCount = 1;
	desc.pBindings = &bind;

	VK_CHECK( qvkCreateDescriptorSetLayout(vk.device, &desc, NULL, layout ) );
}

static void vk_create_composite_layout( VkDescriptorSetLayout *layout ) {
	VkDescriptorSetLayoutBinding bindings[2 + VK_NUM_BLOOM_PASSES];
	VkDescriptorSetLayoutCreateInfo desc = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	uint32_t i;
	Com_Memset( bindings, 0, sizeof( bindings ) );
	for ( i = 0; i < ARRAY_LEN( bindings ); i++ ) {
		bindings[i].binding = i;
		bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		bindings[i].descriptorCount = 1;
		bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	}
	desc.bindingCount = ARRAY_LEN( bindings );
	desc.pBindings = bindings;
	VK_CHECK( qvkCreateDescriptorSetLayout( vk.device, &desc, NULL, layout ) );
}

static void vk_create_view_layout( VkDescriptorType type, VkDescriptorSetLayout *layout ) {
	VkDescriptorSetLayoutBinding bindings[2] = {{0}, {0}};
	VkDescriptorSetLayoutCreateInfo desc = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	bindings[0].binding = 0;
	bindings[0].descriptorType = type;
	bindings[0].descriptorCount = 1;
	bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT;
	bindings[1].binding = 1;
	bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	bindings[1].descriptorCount = 1;
	bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	desc.bindingCount = vk.multiview ? 2 : 1;
	desc.pBindings = bindings;
	VK_CHECK( qvkCreateDescriptorSetLayout( vk.device, &desc, NULL, layout ) );
}

static void vk_update_view_descriptor( VkDescriptorSet descriptor, VkBuffer buffer ) {
	VkDescriptorBufferInfo info = {buffer, 0, 2 * 16 * sizeof( float )};
	VkWriteDescriptorSet desc = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
	if ( !vk.multiview )
		return;
	desc.dstSet = descriptor;
	desc.dstBinding = 1;
	desc.descriptorCount = 1;
	desc.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	desc.pBufferInfo = &info;
	qvkUpdateDescriptorSets( vk.device, 1, &desc, 0, NULL );
}

void vk_update_uniform_descriptor( VkDescriptorSet descriptor, VkBuffer buffer )
{
	VkDescriptorBufferInfo info;
	VkWriteDescriptorSet desc;

	info.buffer = buffer;
	info.offset = 0;
	info.range = sizeof( vkUniform_t );

	desc.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	desc.dstSet = descriptor;
	desc.dstBinding = 0;
	desc.dstArrayElement = 0;
	desc.descriptorCount = 1;
	desc.pNext = NULL;
	desc.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	desc.pImageInfo = NULL;
	desc.pBufferInfo = &info;
	desc.pTexelBufferView = NULL;

	qvkUpdateDescriptorSets( vk.device, 1, &desc, 0, NULL );
	vk_update_view_descriptor( descriptor, buffer );
}


static VkSampler vk_find_sampler( const Vk_Sampler_Def *def ) {
	VkSamplerAddressMode address_mode;
	VkSamplerCreateInfo desc;
	VkSampler sampler;
	VkFilter mag_filter;
	VkFilter min_filter;
	VkSamplerMipmapMode mipmap_mode;
	float maxLod;
	int i;

	// Look for sampler among existing samplers.
	for ( i = 0; i < vk.samplers.count; i++ ) {
		const Vk_Sampler_Def *cur_def = &vk.samplers.def[i];
		if ( memcmp( cur_def, def, sizeof( *def ) ) == 0 ) {
			return vk.samplers.handle[i];
		}
	}

	// Create new sampler.
	if ( vk.samplers.count >= MAX_VK_SAMPLERS ) {
		ri.Error( ERR_DROP, "vk_find_sampler: MAX_VK_SAMPLERS hit\n" );
		// return VK_NULL_HANDLE;
	}

	address_mode = def->address_mode;

	if (def->gl_mag_filter == GL_NEAREST) {
		mag_filter = VK_FILTER_NEAREST;
	} else if (def->gl_mag_filter == GL_LINEAR) {
		mag_filter = VK_FILTER_LINEAR;
	} else {
		ri.Error(ERR_FATAL, "vk_find_sampler: invalid gl_mag_filter");
		return VK_NULL_HANDLE;
	}

	maxLod = vk.maxLod;

	if (def->gl_min_filter == GL_NEAREST) {
		min_filter = VK_FILTER_NEAREST;
		mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		maxLod = 0.25f; // used to emulate OpenGL's GL_LINEAR/GL_NEAREST minification filter
	} else if (def->gl_min_filter == GL_LINEAR) {
		min_filter = VK_FILTER_LINEAR;
		mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		maxLod = 0.25f; // used to emulate OpenGL's GL_LINEAR/GL_NEAREST minification filter
	} else if (def->gl_min_filter == GL_NEAREST_MIPMAP_NEAREST) {
		min_filter = VK_FILTER_NEAREST;
		mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	} else if (def->gl_min_filter == GL_LINEAR_MIPMAP_NEAREST) {
		min_filter = VK_FILTER_LINEAR;
		mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	} else if (def->gl_min_filter == GL_NEAREST_MIPMAP_LINEAR) {
		min_filter = VK_FILTER_NEAREST;
		mipmap_mode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	} else if (def->gl_min_filter == GL_LINEAR_MIPMAP_LINEAR) {
		min_filter = VK_FILTER_LINEAR;
		mipmap_mode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	} else {
		ri.Error(ERR_FATAL, "vk_find_sampler: invalid gl_min_filter");
		return VK_NULL_HANDLE;
	}

	if ( def->max_lod_1_0 ) {
		maxLod = 1.0f;
	}

	desc.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	desc.pNext = NULL;
	desc.flags = 0;
	desc.magFilter = mag_filter;
	desc.minFilter = min_filter;
	desc.mipmapMode = mipmap_mode;
	desc.addressModeU = address_mode;
	desc.addressModeV = address_mode;
	desc.addressModeW = address_mode;
	desc.mipLodBias = 0.0f;

	if ( def->noAnisotropy || mipmap_mode == VK_SAMPLER_MIPMAP_MODE_NEAREST || mag_filter == VK_FILTER_NEAREST ) {
		desc.anisotropyEnable = VK_FALSE;
		desc.maxAnisotropy = 1.0f;
	} else {
		desc.anisotropyEnable = (r_ext_texture_filter_anisotropic->integer && vk.samplerAnisotropy) ? VK_TRUE : VK_FALSE;
		if ( desc.anisotropyEnable ) {
			desc.maxAnisotropy = MIN( r_ext_max_anisotropy->integer, vk.maxAnisotropy );
		}
	}

	desc.compareEnable = VK_FALSE;
	desc.compareOp = VK_COMPARE_OP_ALWAYS;
	desc.minLod = 0.0f;
	desc.maxLod = (maxLod == vk.maxLod) ? VK_LOD_CLAMP_NONE : maxLod;
	desc.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
	desc.unnormalizedCoordinates = VK_FALSE;

	VK_CHECK( qvkCreateSampler( vk.device, &desc, NULL, &sampler ) );

	SET_OBJECT_NAME( sampler, va( "image sampler %i", vk.samplers.count ), VK_DEBUG_REPORT_OBJECT_TYPE_SAMPLER_EXT );

	vk.samplers.def[ vk.samplers.count ] = *def;
	vk.samplers.handle[ vk.samplers.count ] = sampler;
	vk.samplers.count++;

	return sampler;
}


void vk_destroy_samplers( void )
{
	int i;

	for ( i = 0; i < vk.samplers.count; i++ ) {
		qvkDestroySampler( vk.device, vk.samplers.handle[i], NULL );
		memset( &vk.samplers.def[i], 0x0, sizeof( vk.samplers.def[i] ) );
		vk.samplers.handle[i] = VK_NULL_HANDLE;
	}

	vk.samplers.count = 0;
}


void vk_update_attachment_descriptors( void ) {

	if ( vk.color_image_view )
	{
		VkDescriptorImageInfo info;
		VkWriteDescriptorSet desc;
		Vk_Sampler_Def sd;
		uint32_t n;

		Com_Memset( &sd, 0, sizeof( sd ) );
		sd.gl_mag_filter = sd.gl_min_filter = vk.blitFilter;
		sd.address_mode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		sd.max_lod_1_0 = qtrue;
		sd.noAnisotropy = qtrue;

		info.sampler = vk_find_sampler( &sd );
		info.imageView = vk.color_image_view;
		info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		desc.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		desc.dstSet = vk.color_descriptor;
		desc.dstBinding = 0;
		desc.dstArrayElement = 0;
		desc.descriptorCount = 1;
		desc.pNext = NULL;
		desc.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		desc.pImageInfo = &info;
		desc.pBufferInfo = NULL;
		desc.pTexelBufferView = NULL;

		qvkUpdateDescriptorSets( vk.device, 1, &desc, 0, NULL );

		// placeholder (color_image_view) when inactive; gamma_fs always declares texture1
		info.imageView = vk.hdrActive ? vk.emissive_image_view : vk.color_image_view;
		desc.dstSet = vk.emissive_descriptor;
		desc.dstBinding = 0;
		qvkUpdateDescriptorSets( vk.device, 1, &desc, 0, NULL );

		// the composite reads the scene and the emissive layer from its own set; the bloom levels follow below
		desc.dstSet = vk.composite_descriptor;
		desc.dstBinding = 1;
		qvkUpdateDescriptorSets( vk.device, 1, &desc, 0, NULL );
		info.imageView = vk.color_image_view;
		desc.dstBinding = 0;
		qvkUpdateDescriptorSets( vk.device, 1, &desc, 0, NULL );

		// the mirror and screenshots sample the finished eye image
		for ( n = 0; n < vk.xr_output.eye_count; n++ ) {
			info.imageView = vk.xr_output.eye_view[n];
			desc.dstSet = vk.xr_output.eye_descriptor[n];
			qvkUpdateDescriptorSets( vk.device, 1, &desc, 0, NULL );
		}
		if ( vk.xr_output.descriptor ) {
			info.imageView = vk.xr_output.unorm_view;
			desc.dstSet = vk.xr_output.descriptor;
			qvkUpdateDescriptorSets( vk.device, 1, &desc, 0, NULL );
		}

		// screenmap
		sd.gl_mag_filter = sd.gl_min_filter = GL_LINEAR;
		sd.max_lod_1_0 = qfalse;
		sd.noAnisotropy = qtrue;

		info.sampler = vk_find_sampler( &sd );

		info.imageView = vk.screenMap.color_image_view;
		desc.dstSet = vk.screenMap.color_descriptor;

		qvkUpdateDescriptorSets( vk.device, 1, &desc, 0, NULL );

		// bloom images
		if ( r_bloom->integer )
		{
			uint32_t i;
			for ( i = 1; i < ARRAY_LEN( vk.bloom_image_descriptor ); i++ )
			{
				info.imageView = vk.bloom_image_view[i];
				desc.dstSet = vk.bloom_image_descriptor[i];

				qvkUpdateDescriptorSets( vk.device, 1, &desc, 0, NULL );
			}
		}

		// the composite's bloom levels, sampled linearly like the blur's: the ones the blend sums, or scene placeholders
		desc.dstSet = vk.composite_descriptor;
		for ( n = 0; n < VK_NUM_BLOOM_PASSES; n++ ) {
			info.imageView = r_bloom->integer ? vk.bloom_image_view[(n+1)*2] : vk.color_image_view;
			desc.dstBinding = 2 + n;
			qvkUpdateDescriptorSets( vk.device, 1, &desc, 0, NULL );
		}
	}
	else if ( vk.xrDirect && vk.xr_direct.idle.descriptor )
	{
		VkDescriptorImageInfo info;
		VkWriteDescriptorSet desc;
		Vk_Sampler_Def sd;
		uint32_t i;

		Com_Memset( &sd, 0, sizeof( sd ) );
		sd.gl_mag_filter = sd.gl_min_filter = vk.blitFilter;
		sd.address_mode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		sd.max_lod_1_0 = qtrue;
		sd.noAnisotropy = qtrue;

		info.sampler = vk_find_sampler( &sd );
		info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		desc.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		desc.dstBinding = 0;
		desc.dstArrayElement = 0;
		desc.descriptorCount = 1;
		desc.pNext = NULL;
		desc.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		desc.pImageInfo = &info;
		desc.pBufferInfo = NULL;
		desc.pTexelBufferView = NULL;

		for ( i = 0; i <= vk.xr_direct.count; i++ )
		{
			const struct vkXRDirectTarget_s *t = i < vk.xr_direct.count ? &vk.xr_direct.target[i] : &vk.xr_direct.idle;
			info.imageView = t->view;
			desc.dstSet = t->descriptor;
			qvkUpdateDescriptorSets( vk.device, 1, &desc, 0, NULL );
		}
	}
}

static void vk_alloc_target_descriptor( VkDescriptorSet *set ) {
	VkDescriptorSetAllocateInfo alloc;

	alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	alloc.pNext = NULL;
	alloc.descriptorPool = vk.target_descriptor_pool;
	alloc.descriptorSetCount = 1;
	alloc.pSetLayouts = &vk.set_layout_sampler;

	VK_CHECK( qvkAllocateDescriptorSets( vk.device, &alloc, set ) );
}

static void vk_init_target_descriptors( void ) {
	uint32_t i;
	if ( !vk.target_descriptor_pool ) {
		VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 128};
		VkDescriptorPoolCreateInfo pool;
		Com_Memset( &pool, 0, sizeof( pool ) );
		pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		pool.maxSets = 128;
		pool.poolSizeCount = 1;
		pool.pPoolSizes = &size;
		VK_CHECK( qvkCreateDescriptorPool( vk.device, &pool, NULL, &vk.target_descriptor_pool ) );
	}
	if ( vk.color_image_view && !vk.color_descriptor ) {
		VkDescriptorSetAllocateInfo alloc = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
		vk_alloc_target_descriptor( &vk.color_descriptor );
		vk_alloc_target_descriptor( &vk.emissive_descriptor );
		alloc.descriptorPool = vk.target_descriptor_pool;
		alloc.descriptorSetCount = 1;
		alloc.pSetLayouts = &vk.set_layout_composite;
		VK_CHECK( qvkAllocateDescriptorSets( vk.device, &alloc, &vk.composite_descriptor ) );
		for ( i = 0; i < vk.xr_output.eye_count; i++ )
			vk_alloc_target_descriptor( &vk.xr_output.eye_descriptor[i] );
		if ( vk.xr_output.image )
			vk_alloc_target_descriptor( &vk.xr_output.descriptor );

		if ( r_bloom->integer ) {
			for ( i = 1; i < ARRAY_LEN( vk.bloom_image_descriptor ); i++ ) {
				vk_alloc_target_descriptor( &vk.bloom_image_descriptor[i] );
			}
		}

		vk_alloc_target_descriptor( &vk.screenMap.color_descriptor ); // screenmap

		vk_update_attachment_descriptors();
	}
	else if ( vk.xrDirect && !vk.xr_direct.idle.descriptor ) {
		for ( i = 0; i < vk.xr_direct.count; i++ )
			vk_alloc_target_descriptor( &vk.xr_direct.target[i].descriptor );
		vk_alloc_target_descriptor( &vk.xr_direct.idle.descriptor );

		vk_update_attachment_descriptors();
	}
}

void vk_init_descriptors( void )
{
	VkDescriptorSetAllocateInfo alloc;
	VkDescriptorBufferInfo info;
	VkWriteDescriptorSet desc;
	uint32_t i;

	alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	alloc.pNext = NULL;
	alloc.descriptorPool = vk.descriptor_pool;
	alloc.descriptorSetCount = 1;
	alloc.pSetLayouts = &vk.set_layout_storage;

	VK_CHECK( qvkAllocateDescriptorSets( vk.device, &alloc, &vk.storage.descriptor ) );

	info.buffer = vk.storage.buffer;
	info.offset = 0;
	info.range = sizeof( uint32_t );

	desc.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	desc.dstSet = vk.storage.descriptor;
	desc.dstBinding = 0;
	desc.dstArrayElement = 0;
	desc.descriptorCount = 1;
	desc.pNext = NULL;
	desc.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
	desc.pImageInfo = NULL;
	desc.pBufferInfo = &info;
	desc.pTexelBufferView = NULL;

	qvkUpdateDescriptorSets( vk.device, 1, &desc, 0, NULL );

	// allocated and update descriptor set
	for ( i = 0; i < NUM_COMMAND_BUFFERS; i++ )
	{
		alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		alloc.pNext = NULL;
		alloc.descriptorPool = vk.descriptor_pool;
		alloc.descriptorSetCount = 1;
		alloc.pSetLayouts = &vk.set_layout_uniform;

		VK_CHECK( qvkAllocateDescriptorSets( vk.device, &alloc, &vk.tess[i].uniform_descriptor ) );

		vk_update_uniform_descriptor( vk.tess[ i ].uniform_descriptor, vk.tess[ i ].vertex_buffer );
		if ( vk.multiview ) {
			alloc.pSetLayouts = &vk.set_layout_storage;
			VK_CHECK( qvkAllocateDescriptorSets( vk.device, &alloc, &vk.tess[i].storage_descriptor ) );
			desc.dstSet = vk.tess[i].storage_descriptor;
			info.range = 4 * sizeof( uint32_t ); // flare probes: fragments passed, then drawn, per eye
			qvkUpdateDescriptorSets( vk.device, 1, &desc, 0, NULL );
			vk_update_view_descriptor( vk.tess[i].storage_descriptor, vk.tess[i].vertex_buffer );
		}

		SET_OBJECT_NAME( vk.tess[i].uniform_descriptor, va( "uniform descriptor %i", i ),
						 VK_DEBUG_REPORT_OBJECT_TYPE_DESCRIPTOR_SET_EXT );
	}

	vk_init_target_descriptors();
}


static void vk_release_geometry_buffers( void )
{
	int i;

	for ( i = 0; i < NUM_COMMAND_BUFFERS; i++ ) {
		qvkDestroyBuffer( vk.device, vk.tess[i].vertex_buffer, NULL );
		vk.tess[i].vertex_buffer = VK_NULL_HANDLE;
	}

	qvkFreeMemory( vk.device, vk.geometry_buffer_memory, NULL );
	vk.geometry_buffer_memory = VK_NULL_HANDLE;
}


static void vk_create_geometry_buffers( VkDeviceSize size )
{
	VkMemoryRequirements vb_memory_requirements;
	VkMemoryAllocateInfo alloc_info;
	VkBufferCreateInfo desc;
	VkDeviceSize vertex_buffer_offset;
	uint32_t memory_type_bits;
	uint32_t memory_type;
	void *data;
	int i;

	desc.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	desc.pNext = NULL;
	desc.flags = 0;
	desc.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	desc.queueFamilyIndexCount = 0;
	desc.pQueueFamilyIndices = NULL;

	Com_Memset( &vb_memory_requirements, 0, sizeof( vb_memory_requirements ) );

	for ( i = 0 ; i < NUM_COMMAND_BUFFERS; i++ ) {
		desc.size = size;
		desc.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
		VK_CHECK( qvkCreateBuffer( vk.device, &desc, NULL, &vk.tess[i].vertex_buffer ) );

		qvkGetBufferMemoryRequirements( vk.device, vk.tess[i].vertex_buffer, &vb_memory_requirements );
	}

	memory_type_bits = vb_memory_requirements.memoryTypeBits;
	memory_type = find_memory_type( memory_type_bits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT );

	alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc_info.pNext = NULL;
	alloc_info.allocationSize = vb_memory_requirements.size * NUM_COMMAND_BUFFERS;
	alloc_info.memoryTypeIndex = memory_type;

	VK_CHECK( qvkAllocateMemory( vk.device, &alloc_info, NULL, &vk.geometry_buffer_memory ) );
	VK_CHECK( qvkMapMemory( vk.device, vk.geometry_buffer_memory, 0, VK_WHOLE_SIZE, 0, &data ) );

	vertex_buffer_offset = 0;

	for ( i = 0 ; i < NUM_COMMAND_BUFFERS; i++ ) {
		qvkBindBufferMemory( vk.device, vk.tess[i].vertex_buffer, vk.geometry_buffer_memory, vertex_buffer_offset );
		vk.tess[i].vertex_buffer_ptr = (byte*)data + vertex_buffer_offset;
		vk.tess[i].vertex_buffer_offset = 0;
		vertex_buffer_offset += vb_memory_requirements.size;

		SET_OBJECT_NAME( vk.tess[i].vertex_buffer, va( "geometry buffer %i", i ), VK_DEBUG_REPORT_OBJECT_TYPE_BUFFER_EXT );
	}

	SET_OBJECT_NAME( vk.geometry_buffer_memory, "geometry buffer memory", VK_DEBUG_REPORT_OBJECT_TYPE_BUFFER_EXT );

	vk.geometry_buffer_size = vb_memory_requirements.size;

	Com_Memset( &vk.stats, 0, sizeof( vk.stats ) );
}


static void vk_create_storage_buffer( uint32_t size )
{
	VkMemoryRequirements memory_requirements;
	VkMemoryAllocateInfo alloc_info;
	VkBufferCreateInfo desc;
	uint32_t memory_type_bits;
	uint32_t memory_type;

	desc.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	desc.pNext = NULL;
	desc.flags = 0;
	desc.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	desc.queueFamilyIndexCount = 0;
	desc.pQueueFamilyIndices = NULL;

	Com_Memset( &memory_requirements, 0, sizeof( memory_requirements ) );

	desc.size = size;
	desc.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	VK_CHECK( qvkCreateBuffer( vk.device, &desc, NULL, &vk.storage.buffer ) );

	qvkGetBufferMemoryRequirements( vk.device, vk.storage.buffer, &memory_requirements );

	memory_type_bits = memory_requirements.memoryTypeBits;
	memory_type = find_memory_type( memory_type_bits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT );

	alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc_info.pNext = NULL;
	alloc_info.allocationSize = memory_requirements.size;
	alloc_info.memoryTypeIndex = memory_type;

	VK_CHECK( qvkAllocateMemory( vk.device, &alloc_info, NULL, &vk.storage.memory ) );
	VK_CHECK( qvkMapMemory( vk.device, vk.storage.memory, 0, VK_WHOLE_SIZE, 0, (void**)&vk.storage.buffer_ptr ) );

	Com_Memset( vk.storage.buffer_ptr, 0, memory_requirements.size );

	qvkBindBufferMemory( vk.device, vk.storage.buffer, vk.storage.memory, 0 );

	SET_OBJECT_NAME( vk.storage.buffer, "storage buffer", VK_DEBUG_REPORT_OBJECT_TYPE_BUFFER_EXT );
	SET_OBJECT_NAME( vk.storage.descriptor, "storage buffer", VK_DEBUG_REPORT_OBJECT_TYPE_DESCRIPTOR_SET_EXT );
	SET_OBJECT_NAME( vk.storage.memory, "storage buffer memory", VK_DEBUG_REPORT_OBJECT_TYPE_DEVICE_MEMORY_EXT );
}


/* A device-local vertex and index buffer filled through the staging buffer. */
static void vk_create_static_buffer( const byte *data, VkDeviceSize size, VkBuffer *buffer, VkDeviceMemory *memory )
{
	VkMemoryRequirements vb_mem_reqs;
	VkMemoryAllocateInfo alloc_info;
	VkBufferCreateInfo desc;
	VkCommandBuffer command_buffer;
	VkBufferCopy copyRegion[1];
	VkDeviceSize uploadDone;

	desc.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	desc.pNext = NULL;
	desc.flags = 0;
	desc.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	desc.queueFamilyIndexCount = 0;
	desc.pQueueFamilyIndices = NULL;

	// device-local buffer
	desc.size = size;
	desc.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
	VK_CHECK( qvkCreateBuffer( vk.device, &desc, NULL, buffer ) );

	// memory requirements
	qvkGetBufferMemoryRequirements( vk.device, *buffer, &vb_mem_reqs );

	alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc_info.pNext = NULL;
	alloc_info.allocationSize = vb_mem_reqs.size;
	alloc_info.memoryTypeIndex = find_memory_type( vb_mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT );
	VK_CHECK( qvkAllocateMemory( vk.device, &alloc_info, NULL, memory ) );
	qvkBindBufferMemory( vk.device, *buffer, *memory, 0 );

	// staging buffers

#ifdef USE_UPLOAD_QUEUE
	vk_flush_staging_buffer( qfalse );
#endif
	// utilize existing staging buffer
	uploadDone = 0;
	while ( uploadDone < size ) {
		VkDeviceSize uploadSize = vk.staging_buffer.size;
		if ( uploadDone + uploadSize > size ) {
			uploadSize = size - uploadDone;
		}
		memcpy(vk.staging_buffer.ptr + 0, data + uploadDone, uploadSize);
		command_buffer = begin_command_buffer();
		copyRegion[0].srcOffset = 0;
		copyRegion[0].dstOffset = uploadDone;
		copyRegion[0].size = uploadSize;
		qvkCmdCopyBuffer( command_buffer, vk.staging_buffer.handle, *buffer, 1, &copyRegion[0] );
		end_command_buffer( command_buffer, __func__ );
		uploadDone += uploadSize;
	}
}


#ifdef USE_VBO
void vk_release_vbo( void )
{
	if ( vk.vbo.vertex_buffer )
		qvkDestroyBuffer( vk.device, vk.vbo.vertex_buffer, NULL );
	vk.vbo.vertex_buffer = VK_NULL_HANDLE;

	if ( vk.vbo.buffer_memory )
		qvkFreeMemory( vk.device, vk.vbo.buffer_memory, NULL );
	vk.vbo.buffer_memory = VK_NULL_HANDLE;
}


qboolean vk_alloc_vbo( const byte *vbo_data, int vbo_size )
{
	vk_release_vbo();

	vk_create_static_buffer( vbo_data, vbo_size, &vk.vbo.vertex_buffer, &vk.vbo.buffer_memory );

	SET_OBJECT_NAME( vk.vbo.vertex_buffer, "static VBO", VK_DEBUG_REPORT_OBJECT_TYPE_BUFFER_EXT );
	SET_OBJECT_NAME( vk.vbo.buffer_memory, "static VBO memory", VK_DEBUG_REPORT_OBJECT_TYPE_DEVICE_MEMORY_EXT );

	return qtrue;
}
#endif

#include "shaders/spirv/shader_data.c"
#define SHADER_MODULE(name) SHADER_MODULE(name,sizeof(name))

static void vk_create_shader_modules( void )
{
	int i, j, k, l;

	vk.modules.vert.gen[0][0][0][0] = SHADER_MODULE( vert_tx0 );
	vk.modules.vert.gen[0][0][0][1] = SHADER_MODULE( vert_tx0_fog );
	vk.modules.vert.gen[0][0][1][0] = SHADER_MODULE( vert_tx0_env );
	vk.modules.vert.gen[0][0][1][1] = SHADER_MODULE( vert_tx0_env_fog );

	vk.modules.vert.gen[1][0][0][0] = SHADER_MODULE( vert_tx1 );
	vk.modules.vert.gen[1][0][0][1] = SHADER_MODULE( vert_tx1_fog );
	vk.modules.vert.gen[1][0][1][0] = SHADER_MODULE( vert_tx1_env );
	vk.modules.vert.gen[1][0][1][1] = SHADER_MODULE( vert_tx1_env_fog );

	vk.modules.vert.gen[1][1][0][0] = SHADER_MODULE( vert_tx1_cl );
	vk.modules.vert.gen[1][1][0][1] = SHADER_MODULE( vert_tx1_cl_fog );
	vk.modules.vert.gen[1][1][1][0] = SHADER_MODULE( vert_tx1_cl_env );
	vk.modules.vert.gen[1][1][1][1] = SHADER_MODULE( vert_tx1_cl_env_fog );

	vk.modules.vert.gen[2][0][0][0] = SHADER_MODULE( vert_tx2 );
	vk.modules.vert.gen[2][0][0][1] = SHADER_MODULE( vert_tx2_fog );
	vk.modules.vert.gen[2][0][1][0] = SHADER_MODULE( vert_tx2_env );
	vk.modules.vert.gen[2][0][1][1] = SHADER_MODULE( vert_tx2_env_fog );

	vk.modules.vert.gen[2][1][0][0] = SHADER_MODULE( vert_tx2_cl );
	vk.modules.vert.gen[2][1][0][1] = SHADER_MODULE( vert_tx2_cl_fog );
	vk.modules.vert.gen[2][1][1][0] = SHADER_MODULE( vert_tx2_cl_env );
	vk.modules.vert.gen[2][1][1][1] = SHADER_MODULE( vert_tx2_cl_env_fog );

	for ( i = 0; i < 3; i++ ) {
		const char *tx[] = { "single", "double", "triple" };
		const char *cl[] = { "", "+cl" };
		const char *env[] = { "", "+env" };
		const char *fog[] = { "", "+fog" };
		for ( j = 0; j < 2; j++ ) {
			for ( k = 0; k < 2; k++ ) {
				for ( l = 0; l < 2; l++ ) {
					const char *s = va( "%s-texture%s%s%s vertex module", tx[i], cl[j], env[k], fog[l] );
					SET_OBJECT_NAME( vk.modules.vert.gen[i][j][k][l], s, VK_DEBUG_REPORT_OBJECT_TYPE_SHADER_MODULE_EXT );
				}
			}
		}
	}

	// specialized depth-fragment shader
	vk.modules.floor_grid_fs = SHADER_MODULE( floor_grid_frag_spv );
	vk.modules.virtualscreen_fs = SHADER_MODULE( virtualscreen_frag_spv );
	vk.modules.virtualreflect_fs = SHADER_MODULE( virtualreflect_frag_spv );
	vk.modules.frag.gen0_df = SHADER_MODULE( frag_tx0_df );
	SET_OBJECT_NAME( vk.modules.frag.gen0_df, "single-texture df fragment module", VK_DEBUG_REPORT_OBJECT_TYPE_SHADER_MODULE_EXT );

	// fixed-color (1.0) shader modules
	vk.modules.vert.ident1[0][0][0] = SHADER_MODULE( vert_tx0_ident1 );
	vk.modules.vert.ident1[0][0][1] = SHADER_MODULE( vert_tx0_ident1_fog );
	vk.modules.vert.ident1[0][1][0] = SHADER_MODULE( vert_tx0_ident1_env );
	vk.modules.vert.ident1[0][1][1] = SHADER_MODULE( vert_tx0_ident1_env_fog );
	vk.modules.vert.ident1[1][0][0] = SHADER_MODULE( vert_tx1_ident1 );
	vk.modules.vert.ident1[1][0][1] = SHADER_MODULE( vert_tx1_ident1_fog );
	vk.modules.vert.ident1[1][1][0] = SHADER_MODULE( vert_tx1_ident1_env );
	vk.modules.vert.ident1[1][1][1] = SHADER_MODULE( vert_tx1_ident1_env_fog );
	for ( i = 0; i < 2; i++ ) {
		const char *tx[] = { "single", "double" };
		const char *env[] = { "", "+env" };
		const char *fog[] = { "", "+fog" };
		for ( j = 0; j < 2; j++ ) {
			for ( k = 0; k < 2; k++ ) {
				const char *s = va( "%s-texture identity%s%s vertex module", tx[i], env[j], fog[k] );
				SET_OBJECT_NAME( vk.modules.vert.ident1[i][j][k], s, VK_DEBUG_REPORT_OBJECT_TYPE_SHADER_MODULE_EXT );
			}
		}
	}

	vk.modules.frag.ident1[0][0][0] = SHADER_MODULE( frag_tx0_ident1 );
	vk.modules.frag.ident1[0][0][1] = SHADER_MODULE( frag_tx0_ident1_fog );
	vk.modules.frag.ident1[0][1][0] = SHADER_MODULE( frag_tx0_ident1_ne );
	vk.modules.frag.ident1[0][1][1] = SHADER_MODULE( frag_tx0_ident1_fog_ne );
	vk.modules.frag.ident1[1][0][0] = SHADER_MODULE( frag_tx1_ident1 );
	vk.modules.frag.ident1[1][0][1] = SHADER_MODULE( frag_tx1_ident1_fog );
	vk.modules.frag.ident1[1][1][0] = SHADER_MODULE( frag_tx1_ident1_ne );
	vk.modules.frag.ident1[1][1][1] = SHADER_MODULE( frag_tx1_ident1_fog_ne );
	for ( i = 0; i < 2; i++ ) {
		const char *tx[] = { "single", "double" };
		const char *em[] = { "", " no-emissive" };
		const char *fog[] = { "", "+fog" };
		for ( j = 0; j < 2; j++ ) {
			for ( k = 0; k < 2; k++ ) {
				const char *s = va( "%s-texture identity%s%s fragment module", tx[i], fog[k], em[j] );
				SET_OBJECT_NAME( vk.modules.frag.ident1[i][j][k], s, VK_DEBUG_REPORT_OBJECT_TYPE_SHADER_MODULE_EXT );
			}
		}
	}

	vk.modules.vert.fixed[0][0][0] = SHADER_MODULE( vert_tx0_fixed );
	vk.modules.vert.fixed[0][0][1] = SHADER_MODULE( vert_tx0_fixed_fog );
	vk.modules.vert.fixed[0][1][0] = SHADER_MODULE( vert_tx0_fixed_env );
	vk.modules.vert.fixed[0][1][1] = SHADER_MODULE( vert_tx0_fixed_env_fog );
	vk.modules.vert.fixed[1][0][0] = SHADER_MODULE( vert_tx1_fixed );
	vk.modules.vert.fixed[1][0][1] = SHADER_MODULE( vert_tx1_fixed_fog );
	vk.modules.vert.fixed[1][1][0] = SHADER_MODULE( vert_tx1_fixed_env );
	vk.modules.vert.fixed[1][1][1] = SHADER_MODULE( vert_tx1_fixed_env_fog );
	for ( i = 0; i < 2; i++ ) {
		const char *tx[] = { "single", "double" };
		const char *env[] = { "", "+env" };
		const char *fog[] = { "", "+fog" };
		for ( j = 0; j < 2; j++ ) {
			for ( k = 0; k < 2; k++ ) {
				const char *s = va( "%s-texture fixed-color%s%s vertex module", tx[i], env[j], fog[k] );
				SET_OBJECT_NAME( vk.modules.vert.fixed[i][j][k], s, VK_DEBUG_REPORT_OBJECT_TYPE_SHADER_MODULE_EXT );
			}
		}
	}

	vk.modules.frag.fixed[0][0][0] = SHADER_MODULE( frag_tx0_fixed );
	vk.modules.frag.fixed[0][0][1] = SHADER_MODULE( frag_tx0_fixed_fog );
	vk.modules.frag.fixed[0][1][0] = SHADER_MODULE( frag_tx0_fixed_ne );
	vk.modules.frag.fixed[0][1][1] = SHADER_MODULE( frag_tx0_fixed_fog_ne );
	vk.modules.frag.fixed[1][0][0] = SHADER_MODULE( frag_tx1_fixed );
	vk.modules.frag.fixed[1][0][1] = SHADER_MODULE( frag_tx1_fixed_fog );
	vk.modules.frag.fixed[1][1][0] = SHADER_MODULE( frag_tx1_fixed_ne );
	vk.modules.frag.fixed[1][1][1] = SHADER_MODULE( frag_tx1_fixed_fog_ne );
	for ( i = 0; i < 2; i++ ) {
		const char *tx[] = { "single", "double" };
		const char *em[] = { "", " no-emissive" };
		const char *fog[] = { "", "+fog" };
		for ( j = 0; j < 2; j++ ) {
			for ( k = 0; k < 2; k++ ) {
				const char *s = va( "%s-texture fixed-color%s%s fragment module", tx[i], fog[k], em[j] );
				SET_OBJECT_NAME( vk.modules.frag.fixed[i][j][k], s, VK_DEBUG_REPORT_OBJECT_TYPE_SHADER_MODULE_EXT );
			}
		}
	}

	vk.modules.frag.ent[0][0][0] = SHADER_MODULE( frag_tx0_ent );
	vk.modules.frag.ent[0][0][1] = SHADER_MODULE( frag_tx0_ent_fog );
	vk.modules.frag.ent[0][1][0] = SHADER_MODULE( frag_tx0_ent_ne );
	vk.modules.frag.ent[0][1][1] = SHADER_MODULE( frag_tx0_ent_fog_ne );
	for ( i = 0; i < 1; i++ ) {
		const char *tx[] = { "single" /*, "double" */};
		const char *em[] = { "", " no-emissive" };
		const char *fog[] = { "", "+fog" };
		for ( j = 0; j < 2; j++ ) {
			for ( k = 0; k < 2; k++ ) {
				const char *s = va( "%s-texture entity-color%s%s fragment module", tx[i], fog[k], em[j] );
				SET_OBJECT_NAME( vk.modules.frag.ent[i][j][k], s, VK_DEBUG_REPORT_OBJECT_TYPE_SHADER_MODULE_EXT );
			}
		}
	}

	vk.modules.frag.gen[0][0][0][0] = SHADER_MODULE( frag_tx0 );
	vk.modules.frag.gen[0][0][0][1] = SHADER_MODULE( frag_tx0_fog );
	vk.modules.frag.gen[0][0][1][0] = SHADER_MODULE( frag_tx0_ne );
	vk.modules.frag.gen[0][0][1][1] = SHADER_MODULE( frag_tx0_fog_ne );

	vk.modules.frag.gen[1][0][0][0] = SHADER_MODULE( frag_tx1 );
	vk.modules.frag.gen[1][0][0][1] = SHADER_MODULE( frag_tx1_fog );
	vk.modules.frag.gen[1][0][1][0] = SHADER_MODULE( frag_tx1_ne );
	vk.modules.frag.gen[1][0][1][1] = SHADER_MODULE( frag_tx1_fog_ne );

	vk.modules.frag.gen[1][1][0][0] = SHADER_MODULE( frag_tx1_cl );
	vk.modules.frag.gen[1][1][0][1] = SHADER_MODULE( frag_tx1_cl_fog );
	vk.modules.frag.gen[1][1][1][0] = SHADER_MODULE( frag_tx1_cl_ne );
	vk.modules.frag.gen[1][1][1][1] = SHADER_MODULE( frag_tx1_cl_fog_ne );

	vk.modules.frag.gen[2][0][0][0] = SHADER_MODULE( frag_tx2 );
	vk.modules.frag.gen[2][0][0][1] = SHADER_MODULE( frag_tx2_fog );
	vk.modules.frag.gen[2][0][1][0] = SHADER_MODULE( frag_tx2_ne );
	vk.modules.frag.gen[2][0][1][1] = SHADER_MODULE( frag_tx2_fog_ne );

	vk.modules.frag.gen[2][1][0][0] = SHADER_MODULE( frag_tx2_cl );
	vk.modules.frag.gen[2][1][0][1] = SHADER_MODULE( frag_tx2_cl_fog );
	vk.modules.frag.gen[2][1][1][0] = SHADER_MODULE( frag_tx2_cl_ne );
	vk.modules.frag.gen[2][1][1][1] = SHADER_MODULE( frag_tx2_cl_fog_ne );

	for ( i = 0; i < 3; i++ ) {
		const char *tx[] = { "single", "double", "triple" };
		const char *cl[] = { "", "+cl" };
		const char *em[] = { "", " no-emissive" };
		const char *fog[] = { "", "+fog" };
		for ( j = 0; j < 2; j++ ) {
			for ( k = 0; k < 2; k++ ) {
				for ( l = 0; l < 2; l++ ) {
					const char *s = va( "%s-texture%s%s%s fragment module", tx[i], cl[j], fog[l], em[k] );
					SET_OBJECT_NAME( vk.modules.frag.gen[i][j][k][l], s, VK_DEBUG_REPORT_OBJECT_TYPE_SHADER_MODULE_EXT );
				}
			}
		}
	}


	vk.modules.vert.light[0] = SHADER_MODULE( vert_light );
	vk.modules.vert.light[1] = SHADER_MODULE( vert_light_fog );
	SET_OBJECT_NAME( vk.modules.vert.light[0], "light vertex module", VK_DEBUG_REPORT_OBJECT_TYPE_SHADER_MODULE_EXT );
	SET_OBJECT_NAME( vk.modules.vert.light[1], "light fog vertex module", VK_DEBUG_REPORT_OBJECT_TYPE_SHADER_MODULE_EXT );

	vk.modules.frag.light[0][0][0] = SHADER_MODULE( frag_light );
	vk.modules.frag.light[0][0][1] = SHADER_MODULE( frag_light_fog );
	vk.modules.frag.light[0][1][0] = SHADER_MODULE( frag_light_ne );
	vk.modules.frag.light[0][1][1] = SHADER_MODULE( frag_light_fog_ne );
	vk.modules.frag.light[1][0][0] = SHADER_MODULE( frag_light_line );
	vk.modules.frag.light[1][0][1] = SHADER_MODULE( frag_light_line_fog );
	vk.modules.frag.light[1][1][0] = SHADER_MODULE( frag_light_line_ne );
	vk.modules.frag.light[1][1][1] = SHADER_MODULE( frag_light_line_fog_ne );
	for ( i = 0; i < 2; i++ ) {
		const char *line[] = { "", "linear " };
		const char *em[] = { "", " no-emissive" };
		const char *fog[] = { "", " fog" };
		for ( j = 0; j < 2; j++ ) {
			for ( k = 0; k < 2; k++ ) {
				const char *s = va( "%slight%s%s fragment module", line[i], fog[k], em[j] );
				SET_OBJECT_NAME( vk.modules.frag.light[i][j][k], s, VK_DEBUG_REPORT_OBJECT_TYPE_SHADER_MODULE_EXT );
			}
		}
	}

	vk.modules.vert.overbright_vert[0] = SHADER_MODULE( vert_tx0_overbright );
	vk.modules.vert.overbright_vert[1] = SHADER_MODULE( vert_tx0_overbright_fog );
	vk.modules.frag.overbright_frag[0][0] = SHADER_MODULE( frag_tx0_overbright );
	vk.modules.frag.overbright_frag[0][1] = SHADER_MODULE( frag_tx0_overbright_fog );
	vk.modules.frag.overbright_frag[1][0] = SHADER_MODULE( frag_tx0_overbright_ne );
	vk.modules.frag.overbright_frag[1][1] = SHADER_MODULE( frag_tx0_overbright_fog_ne );

	vk.modules.color_fs = SHADER_MODULE( color_frag_spv );
	vk.modules.color_vs = SHADER_MODULE( color_vert_spv );

	SET_OBJECT_NAME( vk.modules.color_vs, "single-color vertex module", VK_DEBUG_REPORT_OBJECT_TYPE_SHADER_MODULE_EXT );
	SET_OBJECT_NAME( vk.modules.color_fs, "single-color fragment module", VK_DEBUG_REPORT_OBJECT_TYPE_SHADER_MODULE_EXT );

	vk.modules.fog_vs = SHADER_MODULE( fog_vert_spv );
	vk.modules.fog_fs = SHADER_MODULE( fog_frag_spv );

	SET_OBJECT_NAME( vk.modules.fog_vs, "fog-only vertex module", VK_DEBUG_REPORT_OBJECT_TYPE_SHADER_MODULE_EXT );
	SET_OBJECT_NAME( vk.modules.fog_fs, "fog-only fragment module", VK_DEBUG_REPORT_OBJECT_TYPE_SHADER_MODULE_EXT );

	vk.modules.dot_vs = SHADER_MODULE( dot_vert_spv );
	vk.modules.dot_fs = SHADER_MODULE( dot_frag_spv );

	SET_OBJECT_NAME( vk.modules.dot_vs, "dot vertex module", VK_DEBUG_REPORT_OBJECT_TYPE_SHADER_MODULE_EXT );
	SET_OBJECT_NAME( vk.modules.dot_fs, "dot fragment module", VK_DEBUG_REPORT_OBJECT_TYPE_SHADER_MODULE_EXT );

	vk.modules.blur_extract_fs = SHADER_MODULE( blur_extract_frag_spv );
	vk.modules.blur_fs = SHADER_MODULE( blur_frag_spv );

	SET_OBJECT_NAME( vk.modules.blur_extract_fs, "bloom extracting blur fragment module", VK_DEBUG_REPORT_OBJECT_TYPE_SHADER_MODULE_EXT );
	SET_OBJECT_NAME( vk.modules.blur_fs, "gaussian blur fragment module", VK_DEBUG_REPORT_OBJECT_TYPE_SHADER_MODULE_EXT );

	vk.modules.gamma_fs = SHADER_MODULE( gamma_frag_spv );
	vk.modules.gamma_composite_fs = SHADER_MODULE( gamma_composite_frag_spv );
	vk.modules.gamma_vs = SHADER_MODULE( gamma_vert_spv );

	SET_OBJECT_NAME( vk.modules.gamma_fs, "gamma post-processing fragment module", VK_DEBUG_REPORT_OBJECT_TYPE_SHADER_MODULE_EXT );
	SET_OBJECT_NAME( vk.modules.gamma_vs, "gamma post-processing vertex module", VK_DEBUG_REPORT_OBJECT_TYPE_SHADER_MODULE_EXT );
	if ( vk.multiview ) {
		vk.modules.vert_mv.gen[0][0][0][0] = SHADER_MODULE( vert_tx0_mv );
		vk.modules.vert_mv.gen[0][0][0][1] = SHADER_MODULE( vert_tx0_fog_mv );
		vk.modules.vert_mv.gen[0][0][1][0] = SHADER_MODULE( vert_tx0_env_mv );
		vk.modules.vert_mv.gen[0][0][1][1] = SHADER_MODULE( vert_tx0_env_fog_mv );
		vk.modules.vert_mv.gen[1][0][0][0] = SHADER_MODULE( vert_tx1_mv );
		vk.modules.vert_mv.gen[1][0][0][1] = SHADER_MODULE( vert_tx1_fog_mv );
		vk.modules.vert_mv.gen[1][0][1][0] = SHADER_MODULE( vert_tx1_env_mv );
		vk.modules.vert_mv.gen[1][0][1][1] = SHADER_MODULE( vert_tx1_env_fog_mv );
		vk.modules.vert_mv.gen[1][1][0][0] = SHADER_MODULE( vert_tx1_cl_mv );
		vk.modules.vert_mv.gen[1][1][0][1] = SHADER_MODULE( vert_tx1_cl_fog_mv );
		vk.modules.vert_mv.gen[1][1][1][0] = SHADER_MODULE( vert_tx1_cl_env_mv );
		vk.modules.vert_mv.gen[1][1][1][1] = SHADER_MODULE( vert_tx1_cl_env_fog_mv );
		vk.modules.vert_mv.gen[2][0][0][0] = SHADER_MODULE( vert_tx2_mv );
		vk.modules.vert_mv.gen[2][0][0][1] = SHADER_MODULE( vert_tx2_fog_mv );
		vk.modules.vert_mv.gen[2][0][1][0] = SHADER_MODULE( vert_tx2_env_mv );
		vk.modules.vert_mv.gen[2][0][1][1] = SHADER_MODULE( vert_tx2_env_fog_mv );
		vk.modules.vert_mv.gen[2][1][0][0] = SHADER_MODULE( vert_tx2_cl_mv );
		vk.modules.vert_mv.gen[2][1][0][1] = SHADER_MODULE( vert_tx2_cl_fog_mv );
		vk.modules.vert_mv.gen[2][1][1][0] = SHADER_MODULE( vert_tx2_cl_env_mv );
		vk.modules.vert_mv.gen[2][1][1][1] = SHADER_MODULE( vert_tx2_cl_env_fog_mv );
		vk.modules.vert_mv.ident1[0][0][0] = SHADER_MODULE( vert_tx0_ident1_mv );
		vk.modules.vert_mv.ident1[0][0][1] = SHADER_MODULE( vert_tx0_ident1_fog_mv );
		vk.modules.vert_mv.ident1[0][1][0] = SHADER_MODULE( vert_tx0_ident1_env_mv );
		vk.modules.vert_mv.ident1[0][1][1] = SHADER_MODULE( vert_tx0_ident1_env_fog_mv );
		vk.modules.vert_mv.ident1[1][0][0] = SHADER_MODULE( vert_tx1_ident1_mv );
		vk.modules.vert_mv.ident1[1][0][1] = SHADER_MODULE( vert_tx1_ident1_fog_mv );
		vk.modules.vert_mv.ident1[1][1][0] = SHADER_MODULE( vert_tx1_ident1_env_mv );
		vk.modules.vert_mv.ident1[1][1][1] = SHADER_MODULE( vert_tx1_ident1_env_fog_mv );
		vk.modules.vert_mv.fixed[0][0][0] = SHADER_MODULE( vert_tx0_fixed_mv );
		vk.modules.vert_mv.fixed[0][0][1] = SHADER_MODULE( vert_tx0_fixed_fog_mv );
		vk.modules.vert_mv.fixed[0][1][0] = SHADER_MODULE( vert_tx0_fixed_env_mv );
		vk.modules.vert_mv.fixed[0][1][1] = SHADER_MODULE( vert_tx0_fixed_env_fog_mv );
		vk.modules.vert_mv.fixed[1][0][0] = SHADER_MODULE( vert_tx1_fixed_mv );
		vk.modules.vert_mv.fixed[1][0][1] = SHADER_MODULE( vert_tx1_fixed_fog_mv );
		vk.modules.vert_mv.fixed[1][1][0] = SHADER_MODULE( vert_tx1_fixed_env_mv );
		vk.modules.vert_mv.fixed[1][1][1] = SHADER_MODULE( vert_tx1_fixed_env_fog_mv );
		vk.modules.vert_mv.light[0] = SHADER_MODULE( vert_light_mv );
		vk.modules.vert_mv.light[1] = SHADER_MODULE( vert_light_fog_mv );
		vk.modules.vert_mv.overbright_vert[0] = SHADER_MODULE( vert_tx0_overbright_mv );
		vk.modules.vert_mv.overbright_vert[1] = SHADER_MODULE( vert_tx0_overbright_fog_mv );
		vk.modules.color_vs_mv = SHADER_MODULE( color_vert_spv_mv );
		vk.modules.fog_vs_mv = SHADER_MODULE( fog_vert_spv_mv );
		vk.modules.dot_vs_mv = SHADER_MODULE( dot_vert_spv_mv );
		vk.modules.dot_fs_mv = SHADER_MODULE( dot_frag_spv_mv );
		vk.modules.dot_total_fs_mv = SHADER_MODULE( dot_total_frag_spv );
		vk.modules.blur_extract_fs_mv = SHADER_MODULE( blur_extract_frag_spv_mv );
		vk.modules.blur_fs_mv = SHADER_MODULE( blur_frag_spv_mv );
		vk.modules.gamma_fs_array = SHADER_MODULE( gamma_frag_spv_array );
		vk.modules.gamma_composite_fs_mv = SHADER_MODULE( gamma_composite_frag_spv_mv );
	}
}


static void vk_alloc_persistent_pipelines( void )
{
	unsigned int state_bits;
	Vk_Pipeline_Def def;

	// skybox
	{
		Com_Memset(&def, 0, sizeof(def));
		def.shader_type = TYPE_SINGLE_TEXTURE_FIXED_COLOR;
		def.color.rgb = tr.identityLightByte;
		def.color.alpha = tr.identityLightByte;
		def.face_culling = CT_FRONT_SIDED;
		def.polygon_offset = qfalse;
		def.mirror = qfalse;
		vk.skybox_pipeline = vk_find_pipeline_ext( 0, &def, qtrue );
	}

	// stencil shadows
	{
		cullType_t cull_types[2] = { CT_FRONT_SIDED, CT_BACK_SIDED };
		qboolean mirror_flags[2] = { qfalse, qtrue };
		int i, j;

		Com_Memset(&def, 0, sizeof(def));
		def.polygon_offset = qfalse;
		def.state_bits = 0;
		def.shader_type = TYPE_SINGLE_TEXTURE;
		def.shadow_phase = SHADOW_EDGES;

		for (i = 0; i < 2; i++) {
			def.face_culling = cull_types[i];
			for (j = 0; j < 2; j++) {
				def.mirror = mirror_flags[j];
				vk.shadow_volume_pipelines[i][j] = vk_find_pipeline_ext( 0, &def, r_shadows->integer ? qtrue: qfalse );
			}
		}
	}
	{
		Com_Memset( &def, 0, sizeof( def ) );
		def.face_culling = CT_FRONT_SIDED;
		def.polygon_offset = qfalse;
		def.state_bits = GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ZERO | GLS_DEPTHTEST_DISABLE;
		def.shader_type = TYPE_SINGLE_TEXTURE;
		def.mirror = qfalse;
		def.shadow_phase = SHADOW_FS_QUAD;
		def.primitives = TRIANGLE_STRIP;
		vk.shadow_finish_pipeline = vk_find_pipeline_ext( 0, &def, r_shadows->integer ? qtrue: qfalse );
	}

	// fog and dlights
	{
		unsigned int fog_state_bits[2] = {
			GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_DEPTHFUNC_EQUAL, // fogPass == FP_EQUAL
			GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA // fogPass == FP_LE
		};
		unsigned int dlight_state_bits[2] = {
			GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ONE | GLS_DEPTHFUNC_EQUAL,	// modulated
			GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHFUNC_EQUAL			// additive
		};
		qboolean polygon_offset[2] = { qfalse, qtrue };
		int i, j, k;
#ifdef USE_PMLIGHT
		int l;
#endif

		Com_Memset(&def, 0, sizeof(def));
		def.shader_type = TYPE_SINGLE_TEXTURE;
		def.mirror = qfalse;

		for ( i = 0; i < 2; i++ ) {
			unsigned fog_state = fog_state_bits[ i ];
			unsigned dlight_state = dlight_state_bits[ i ];

			for ( j = 0; j < 3; j++ ) {
				def.face_culling = j; // cullType_t value

				for ( k = 0; k < 2; k++ ) {
					def.polygon_offset = polygon_offset[ k ];
#ifdef USE_FOG_ONLY
					def.shader_type = TYPE_FOG_ONLY;
#else
					def.shader_type = TYPE_SINGLE_TEXTURE;
#endif
					def.state_bits = fog_state;
					vk.fog_pipelines[ i ][ j ][ k ] = vk_find_pipeline_ext( 0, &def, qtrue );

					def.shader_type = TYPE_SINGLE_TEXTURE;
					def.state_bits = dlight_state;
#ifdef USE_LEGACY_DLIGHTS
#ifdef USE_PMLIGHT
					vk.dlight_pipelines[ i ][ j ][ k ] = vk_find_pipeline_ext( 0, &def, r_dlightMode->integer == 0 ? qtrue : qfalse );
#else
					vk.dlight_pipelines[ i ][ j ][ k ] = vk_find_pipeline_ext( 0, &def, qtrue );
#endif
#endif
				}
			}
		}

#ifdef USE_PMLIGHT
		def.state_bits = GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHFUNC_EQUAL;
		//def.shader_type = TYPE_SINGLE_TEXTURE_LIGHTING;
		for (i = 0; i < 3; i++) { // cullType
			def.face_culling = i;
			for ( j = 0; j < 2; j++ ) { // polygonOffset
				def.polygon_offset = polygon_offset[j];
				for ( k = 0; k < 2; k++ ) {
					def.fog_stage = k; // fogStage
					for ( l = 0; l < 2; l++ ) {
						def.abs_light = l;
						def.shader_type = TYPE_SINGLE_TEXTURE_LIGHTING;
						vk.dlight_pipelines_x[i][j][k][l] = vk_find_pipeline_ext( 0, &def, qfalse );
						def.shader_type = TYPE_SINGLE_TEXTURE_LIGHTING_LINEAR;
						vk.dlight1_pipelines_x[i][j][k][l] = vk_find_pipeline_ext( 0, &def, qfalse );
					}
				}
			}
		}
#endif // USE_PMLIGHT
	}

	// RT_BEAM surface
	{
		Com_Memset(&def, 0, sizeof(def));
		def.state_bits = GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE;
		def.face_culling = CT_FRONT_SIDED;
		def.primitives = TRIANGLE_STRIP;
		vk.surface_beam_pipeline = vk_find_pipeline_ext( 0, &def, qfalse );
	}

	// axis for missing models
	{
		Com_Memset( &def, 0, sizeof( def ) );
		def.state_bits = GLS_DEFAULT;
		def.shader_type = TYPE_SINGLE_TEXTURE;
		def.face_culling = CT_TWO_SIDED;
		def.primitives = LINE_LIST;
		if ( vk.wideLines )
			def.line_width = 3;
		vk.surface_axis_pipeline = vk_find_pipeline_ext( 0, &def, qfalse );
	}

	// flare visibility test dot
	if ( vk.fragmentStores )
	{
		Com_Memset( &def, 0, sizeof( def ) );
		//def.state_bits = GLS_DEFAULT;
		def.face_culling = CT_TWO_SIDED;
		def.shader_type = TYPE_DOT;
		def.primitives = POINT_LIST;
		vk.dot_pipeline = vk_find_pipeline_ext( 0, &def, qtrue );
		if ( vk.multiview ) {
			def.state_bits = GLS_DEPTHTEST_DISABLE;
			vk.dot_total_pipeline = vk_find_pipeline_ext( 0, &def, qtrue );
		}
	}

	// DrawTris()
	state_bits = GLS_POLYMODE_LINE | GLS_DEPTHMASK_TRUE;
	{
		Com_Memset(&def, 0, sizeof(def));
		def.state_bits = state_bits;
		def.shader_type = TYPE_COLOR_WHITE;
		def.face_culling = CT_FRONT_SIDED;
		vk.tris_debug_pipeline = vk_find_pipeline_ext( 0, &def, qfalse );
	}
	{
		Com_Memset(&def, 0, sizeof(def));
		def.state_bits = state_bits;
		def.shader_type = TYPE_COLOR_WHITE;
		def.face_culling = CT_BACK_SIDED;
		vk.tris_mirror_debug_pipeline = vk_find_pipeline_ext( 0, &def, qfalse );
	}
	{
		Com_Memset(&def, 0, sizeof(def));
		def.state_bits = state_bits;
		def.shader_type = TYPE_COLOR_GREEN;
		def.face_culling = CT_FRONT_SIDED;
		vk.tris_debug_green_pipeline = vk_find_pipeline_ext( 0, &def, qfalse );
	}
	{
		Com_Memset(&def, 0, sizeof(def));
		def.state_bits = state_bits;
		def.shader_type = TYPE_COLOR_GREEN;
		def.face_culling = CT_BACK_SIDED;
		vk.tris_mirror_debug_green_pipeline = vk_find_pipeline_ext( 0, &def, qfalse );
	}
	{
		Com_Memset(&def, 0, sizeof(def));
		def.state_bits = state_bits;
		def.shader_type = TYPE_COLOR_RED;
		def.face_culling = CT_FRONT_SIDED;
		vk.tris_debug_red_pipeline = vk_find_pipeline_ext( 0, &def, qfalse );
	}
	{
		Com_Memset(&def, 0, sizeof(def));
		def.state_bits = state_bits;
		def.shader_type = TYPE_COLOR_RED;
		def.face_culling = CT_BACK_SIDED;
		vk.tris_mirror_debug_red_pipeline = vk_find_pipeline_ext( 0, &def, qfalse );
	}

	// DrawNormals()
	{
		Com_Memset(&def, 0, sizeof(def));
		def.state_bits = GLS_DEPTHMASK_TRUE;
		def.shader_type = TYPE_SINGLE_TEXTURE;
		def.primitives = LINE_LIST;
		vk.normals_debug_pipeline = vk_find_pipeline_ext( 0, &def, qfalse );
	}

	// RB_DebugPolygon()
	{
		Com_Memset(&def, 0, sizeof(def));
		def.state_bits = GLS_DEPTHMASK_TRUE | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE;
		def.shader_type = TYPE_SINGLE_TEXTURE;
		vk.surface_debug_pipeline_solid = vk_find_pipeline_ext( 0, &def, qfalse );
	}
	{
		Com_Memset(&def, 0, sizeof(def));
		def.state_bits = GLS_POLYMODE_LINE | GLS_DEPTHMASK_TRUE | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE;
		def.shader_type = TYPE_SINGLE_TEXTURE;
		def.primitives = LINE_LIST;
		vk.surface_debug_pipeline_outline = vk_find_pipeline_ext( 0, &def, qfalse );
	}

	// RB_ShowImages
	{
		Com_Memset(&def, 0, sizeof(def));
		def.state_bits = GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA;
		def.shader_type = TYPE_SINGLE_TEXTURE;
		def.primitives = TRIANGLE_STRIP;
		vk.images_debug_pipeline = vk_find_pipeline_ext( 0, &def, qfalse );

		def.state_bits = GLS_DEPTHTEST_DISABLE;
		def.shader_type = TYPE_COLOR_BLACK;
		def.primitives = TRIANGLE_STRIP;
		vk.images_debug_pipeline2 = vk_find_pipeline_ext( 0, &def, qfalse );
	}
}

void vk_create_blur_pipeline( uint32_t index, uint32_t width, uint32_t height, qboolean horizontal_pass );

/* Threshold native scene pixels in VR.
 * The desktop capture can be much smaller and has a different aspect ratio;
 * reducing the eye image to it before extraction aliases bright details. */
static uint32_t vk_bloom_width( void ) {
	return vk.multiview ? vk.sceneWidth : gls.captureWidth;
}

static uint32_t vk_bloom_height( void ) {
	return vk.multiview ? vk.sceneHeight : gls.captureHeight;
}

void vk_update_post_process_pipelines( void )
{
	if ( vk.fboActive ) {
		if ( !vk.multiview )
			vk_create_post_process_pipeline( VK_POST_DESKTOP_COMPOSITE, 0, 0 );
		if ( vk.xr_output.image ) {
			vk_create_post_process_pipeline( VK_POST_XR_COMPOSITE, vk.sceneWidth, vk.sceneHeight );
			vk_create_post_process_pipeline( VK_POST_SCREEN_MONO, gls.windowWidth, gls.windowHeight );
			for ( int eye = 0; eye < 2; eye++ ) {
				vk_create_post_process_pipeline( VK_POST_SCREEN_LEFT + eye, gls.windowWidth,
												 gls.windowHeight );
				vk_create_post_process_pipeline( VK_POST_EYE_MIRROR_LEFT + eye, gls.windowWidth,
												 gls.windowHeight );
			}
			if ( vk.capture.image )
				vk_create_post_process_pipeline( VK_POST_SCREEN_CAPTURE, gls.captureWidth, gls.captureHeight );
		}
		if ( vk.capture.image && !vk.multiview ) {
			// flatscreen screenshots composite the scene and bloom at the capture size
			vk_create_post_process_pipeline( VK_POST_CAPTURE, gls.captureWidth, gls.captureHeight );
		}
		if ( r_bloom->integer ) {
			// update bloom shaders
			uint32_t width = vk_bloom_width();
			uint32_t height = vk_bloom_height();
			uint32_t i;


			for ( i = 0; i < ARRAY_LEN( vk.blur_pipeline ); i += 2 ) {
				width /= 2;
				height /= 2;
				vk_create_blur_pipeline( i + 0, width, height, qtrue ); // horizontal
				vk_create_blur_pipeline( i + 1, width, height, qfalse ); // vertical
			}
		}
	} else if ( vk.xrDirect ) {
		int eye;
		vk_create_post_process_pipeline( VK_POST_SCREEN_MONO, gls.windowWidth, gls.windowHeight );
		for ( eye = 0; eye < 2; eye++ )
			vk_create_post_process_pipeline( VK_POST_SCREEN_LEFT + eye, gls.windowWidth, gls.windowHeight );
		if ( vk.capture.image )
			vk_create_post_process_pipeline( VK_POST_SCREEN_CAPTURE, gls.captureWidth, gls.captureHeight );
	}
}


typedef struct vk_attach_desc_s  {
	VkImage descriptor;
	VkImageView *image_view;
	VkImageUsageFlags usage;
	VkMemoryRequirements reqs;
	uint32_t memoryTypeIndex;
	VkDeviceSize  memory_offset;
	// for layout transition:
	VkImageAspectFlags aspect_flags;
	VkImageLayout image_layout;
	VkFormat image_format;
	uint32_t layers;
} vk_attach_desc_t;

static vk_attach_desc_t attachments[ MAX_ATTACHMENTS_IN_POOL ];
static uint32_t num_attachments = 0;


static void vk_clear_attachment_pool( void )
{
	num_attachments = 0;
}


static void vk_alloc_attachments( void )
{
	VkImageViewCreateInfo view_desc;
	VkMemoryDedicatedAllocateInfoKHR alloc_info2;
	VkMemoryAllocateInfo alloc_info;
	VkCommandBuffer command_buffer;
	VkDeviceMemory memory;
	VkDeviceSize offset;
	uint32_t memoryTypeBits;
	uint32_t memoryTypeIndex;
	uint32_t i;

	if ( num_attachments == 0 ) {
		return;
	}

	if ( vk.image_memory_count >= ARRAY_LEN( vk.image_memory ) ) {
		ri.Error( ERR_DROP, "vk.image_memory_count == %i", (int)ARRAY_LEN( vk.image_memory ) );
	}

	memoryTypeBits = ~0U;
	offset = 0;

	for ( i = 0; i < num_attachments; i++ ) {
#ifdef MIN_IMAGE_ALIGN
		VkDeviceSize alignment = MAX( attachments[ i ].reqs.alignment, MIN_IMAGE_ALIGN );
#else
		VkDeviceSize alignment = attachments[ i ].reqs.alignment;
#endif
		memoryTypeBits &= attachments[ i ].reqs.memoryTypeBits;
		offset = PAD( offset, alignment );
		attachments[ i ].memory_offset = offset;
		offset += attachments[ i ].reqs.size;
#ifdef _DEBUG
		ri.Printf( PRINT_ALL, S_COLOR_CYAN "[%i] type %i, size %i, align %i\n", i,
			attachments[ i ].reqs.memoryTypeBits,
			(int)attachments[ i ].reqs.size,
			(int)attachments[ i ].reqs.alignment );
#endif
	}

	if ( num_attachments == 1 && attachments[ 0 ].usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT ) {
		// try lazy memory
		memoryTypeIndex = find_memory_type2( memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT, NULL );
		if ( memoryTypeIndex == ~0U ) {
			memoryTypeIndex = find_memory_type( memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT );
		}
	} else {
		memoryTypeIndex = find_memory_type( memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT );
	}

#ifdef _DEBUG
	ri.Printf( PRINT_ALL, "memory type bits: %04x\n", memoryTypeBits );
	ri.Printf( PRINT_ALL, "memory type index: %04x\n", memoryTypeIndex );
	ri.Printf( PRINT_ALL, "total size: %i\n", (int)offset );
#endif

	alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc_info.pNext = NULL;
	alloc_info.allocationSize = offset;
	alloc_info.memoryTypeIndex = memoryTypeIndex;

	if ( num_attachments == 1 ) {
		if ( vk.dedicatedAllocation ) {
			Com_Memset( &alloc_info2, 0, sizeof( alloc_info2 ) );
			alloc_info2.sType =  VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO_KHR;
			alloc_info2.image = attachments[ 0 ].descriptor;
			alloc_info.pNext = &alloc_info2;
		}
	}

	// allocate and bind memory
	VK_CHECK( qvkAllocateMemory( vk.device, &alloc_info, NULL, &memory ) );

	vk.image_memory[ vk.image_memory_count++ ] = memory;

	for ( i = 0; i < num_attachments; i++ ) {

		VK_CHECK( qvkBindImageMemory( vk.device, attachments[i].descriptor, memory, attachments[i].memory_offset ) );

		// create color image view
		view_desc.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		view_desc.pNext = NULL;
		view_desc.flags = 0;
		view_desc.image = attachments[ i ].descriptor;
		view_desc.viewType = attachments[i].layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
		view_desc.format = attachments[ i ].image_format;
		view_desc.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
		view_desc.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
		view_desc.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
		view_desc.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
		view_desc.subresourceRange.aspectMask = attachments[ i ].aspect_flags;
		view_desc.subresourceRange.baseMipLevel = 0;
		view_desc.subresourceRange.levelCount = 1;
		view_desc.subresourceRange.baseArrayLayer = 0;
		view_desc.subresourceRange.layerCount = attachments[i].layers;

		VK_CHECK( qvkCreateImageView( vk.device, &view_desc, NULL, attachments[ i ].image_view ) );
	}

	// perform layout transition
	command_buffer = begin_command_buffer();
	for ( i = 0; i < num_attachments; i++ ) {
		record_image_layout_transition( command_buffer,
			attachments[i].descriptor,
			attachments[i].aspect_flags,
			VK_IMAGE_LAYOUT_UNDEFINED, // old_layout
			attachments[i].image_layout,
			0, 0 );
	}
	end_command_buffer( command_buffer, __func__ );

	num_attachments = 0;
}

static void vk_add_attachment_desc( VkImage desc, VkImageView *image_view, VkImageUsageFlags usage,
									VkMemoryRequirements *reqs, VkFormat image_format,
									VkImageAspectFlags aspect_flags, VkImageLayout image_layout,
									uint32_t layers ) {
	if ( num_attachments >= ARRAY_LEN( attachments ) ) {
		ri.Error( ERR_FATAL, "Attachments array overflow" );
	} else {
		attachments[ num_attachments ].descriptor = desc;
		attachments[ num_attachments ].image_view = image_view;
		attachments[ num_attachments ].usage = usage;
		attachments[ num_attachments ].reqs = *reqs;
		attachments[ num_attachments ].aspect_flags = aspect_flags;
		attachments[ num_attachments ].image_layout = image_layout;
		attachments[ num_attachments ].image_format = image_format;
		attachments[num_attachments].layers = layers;
		attachments[ num_attachments ].memory_offset = 0;
		num_attachments++;
	}
}

static void vk_get_image_memory_erquirements( VkImage image, VkMemoryRequirements *memory_requirements )
{
	if ( vk.dedicatedAllocation ) {
		VkMemoryRequirements2KHR memory_requirements2;
		VkImageMemoryRequirementsInfo2KHR image_requirements2;
		VkMemoryDedicatedRequirementsKHR mem_req2;

		Com_Memset( &mem_req2, 0, sizeof( mem_req2 ) );
		mem_req2.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS_KHR;

		image_requirements2.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2_KHR;
		image_requirements2.image = image;
		image_requirements2.pNext = NULL;

		Com_Memset( &memory_requirements2, 0, sizeof( memory_requirements2 ) );
		memory_requirements2.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2_KHR;
		memory_requirements2.pNext = &mem_req2;

		qvkGetImageMemoryRequirements2KHR( vk.device, &image_requirements2, &memory_requirements2 );

		*memory_requirements = memory_requirements2.memoryRequirements;
	} else {
		qvkGetImageMemoryRequirements( vk.device, image, memory_requirements );
	}
}

static void create_color_attachment_flags( VkImageCreateFlags flags, uint32_t width, uint32_t height,
										   VkSampleCountFlagBits samples, VkFormat format, VkImageUsageFlags usage,
										   VkImage *image, VkImageView *image_view, VkImageLayout image_layout,
										   qboolean multisample, uint32_t layers ) {
	VkImageCreateInfo create_desc;
	VkImageFormatListCreateInfoKHR list = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO_KHR};
	VkFormat formats[2];
	VkMemoryRequirements memory_requirements;

	if ( multisample && !( usage & VK_IMAGE_USAGE_SAMPLED_BIT ) )
		usage |= VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;

	// create color image
	create_desc.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	create_desc.pNext = NULL;
	if ( ( flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT ) && vk.imageFormatList ) {
		// a declared view set keeps Adreno framebuffer compression on a MUTABLE image
		formats[0] = format;
		formats[1] = vk_unorm_twin( format );
		list.viewFormatCount = 2;
		list.pViewFormats = formats;
		create_desc.pNext = &list;
	}
	create_desc.flags = flags;
	create_desc.imageType = VK_IMAGE_TYPE_2D;
	create_desc.format = format;
	create_desc.extent.width = width;
	create_desc.extent.height = height;
	create_desc.extent.depth = 1;
	create_desc.mipLevels = 1;
	create_desc.arrayLayers = layers;
	create_desc.samples = samples;
	create_desc.tiling = VK_IMAGE_TILING_OPTIMAL;
	create_desc.usage = usage;
	create_desc.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	create_desc.queueFamilyIndexCount = 0;
	create_desc.pQueueFamilyIndices = NULL;
	create_desc.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VK_CHECK( qvkCreateImage( vk.device, &create_desc, NULL, image ) );

	vk_get_image_memory_erquirements( *image, &memory_requirements );

	vk_add_attachment_desc( *image, image_view, usage, &memory_requirements, format,
							VK_IMAGE_ASPECT_COLOR_BIT, image_layout, layers );
}

static void create_color_attachment( uint32_t width, uint32_t height, VkSampleCountFlagBits samples,
									 VkFormat format, VkImageUsageFlags usage, VkImage *image,
									 VkImageView *image_view, VkImageLayout image_layout,
									 qboolean multisample, uint32_t layers ) {
	create_color_attachment_flags( 0, width, height, samples, format, usage, image, image_view, image_layout,
								   multisample, layers );
}

static void create_depth_attachment( VkImageCreateFlags flags, uint32_t width, uint32_t height,
									 VkSampleCountFlagBits samples, VkImage *image, VkImageView *image_view,
									 qboolean allowTransient, uint32_t layers ) {
	VkImageCreateInfo create_desc;
	VkMemoryRequirements memory_requirements;
	VkImageAspectFlags image_aspect_flags;

	// create depth image
	create_desc.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	create_desc.pNext = NULL;
	create_desc.flags = flags;
	create_desc.imageType = VK_IMAGE_TYPE_2D;
	create_desc.format = vk.depth_format;
	create_desc.extent.width = width;
	create_desc.extent.height = height;
	create_desc.extent.depth = 1;
	create_desc.mipLevels = 1;
	create_desc.arrayLayers = layers;
	create_desc.samples = samples;
	create_desc.tiling = VK_IMAGE_TILING_OPTIMAL;
	create_desc.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	if ( allowTransient ) {
		create_desc.usage |= VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
	}
	create_desc.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	create_desc.queueFamilyIndexCount = 0;
	create_desc.pQueueFamilyIndices = NULL;
	create_desc.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	image_aspect_flags = VK_IMAGE_ASPECT_DEPTH_BIT;
	if ( glConfig.stencilBits > 0 )
		image_aspect_flags |= VK_IMAGE_ASPECT_STENCIL_BIT;

	VK_CHECK( qvkCreateImage( vk.device, &create_desc, NULL, image ) );

	vk_get_image_memory_erquirements( *image, &memory_requirements );

	vk_add_attachment_desc( *image, image_view, create_desc.usage, &memory_requirements, vk.depth_format,
							image_aspect_flags, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, layers );
}

static void vk_create_attachments( void )
{
	uint32_t i;

	vk_clear_attachment_pool();

	// It looks like resulting performance depends from order you're creating/allocating
	// memory for attachments in vulkan i.e. similar images grouped together will provide best results
	// so [resolve0][resolve1][msaa0][msaa1][depth0][depth1] is most optimal
	// while cases like [resolve0][depth0][color0][...] is the worst

	// TODO: preallocate first image chunk in attachment' memory pool?
	if ( vk.fboActive ) {

		VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

		// bloom
		if ( r_bloom->integer ) {
			uint32_t width = vk_bloom_width();
			uint32_t height = vk_bloom_height();

			ri.Printf( PRINT_ALL, "Vulkan bloom extent: %ux%u (%s)\n", width, height,
					   vk.multiview ? "VR scene" : "desktop capture" );

			for ( i = 1; i < ARRAY_LEN( vk.bloom_image ); i += 2 ) {
				width /= 2;
				height /= 2;
				create_color_attachment( width, height, VK_SAMPLE_COUNT_1_BIT, vk.bloom_format, usage,
										 &vk.bloom_image[i + 0], &vk.bloom_image_view[i + 0],
										 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, qfalse,
										 (vk.multiview ? 2 : 1) );

				create_color_attachment( width, height, VK_SAMPLE_COUNT_1_BIT, vk.bloom_format, usage,
										 &vk.bloom_image[i + 1], &vk.bloom_image_view[i + 1],
										 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, qfalse,
										 (vk.multiview ? 2 : 1) );
			}
		}

		// post-processing/msaa-resolve
		create_color_attachment_flags( vk_fdm_attachment_flags(), vk.sceneWidth, vk.sceneHeight, VK_SAMPLE_COUNT_1_BIT,
									   vk.color_format, usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &vk.color_image,
								 &vk.color_image_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, qfalse,
								 (vk.multiview ? 2 : 1) );
		if ( vk.multiview ) {
			create_color_attachment_flags(
				VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, vk.sceneWidth, vk.sceneHeight, VK_SAMPLE_COUNT_1_BIT,
				vk.xr_output.format,
				VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
				&vk.xr_output.image, &vk.xr_output.view, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, qfalse, 2 );
		}

		if ( r_bloom->integer || vk.multiview ) {
			// post-scene 3D icons and the virtual screen's controllers test against their own depth, since the scene's is discarded
			create_depth_attachment( 0, vk.sceneWidth, vk.sceneHeight, VK_SAMPLE_COUNT_1_BIT, &vk.post_depth,
									 &vk.post_depth_view, qtrue, (vk.multiview ? 2 : 1) );
		}

		if ( vk.hdrActive ) {
			// resolved single-sample emissive layer, sampled by the gamma pass
			create_color_attachment_flags( vk_fdm_attachment_flags(), vk.sceneWidth, vk.sceneHeight,
										   VK_SAMPLE_COUNT_1_BIT, VK_FORMAT_R16G16B16A16_SFLOAT, usage, &vk.emissive_image,
									 &vk.emissive_image_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
									 qfalse, (vk.multiview ? 2 : 1) );

			if ( vk.msaaActive ) {
				create_color_attachment_flags( vk_fdm_attachment_flags(), vk.sceneWidth, vk.sceneHeight, vkSamples,
											   VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
											   &vk.emissive_image_msaa, &vk.emissive_image_view_msaa,
										 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, qtrue,
										 (vk.multiview ? 2 : 1) );
			}
		}

		// screenmap-msaa
		if ( vk.screenMapSamples > VK_SAMPLE_COUNT_1_BIT ) {
			create_color_attachment( vk.screenMapWidth, vk.screenMapHeight, vk.screenMapSamples,
									 vk.color_format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
									 &vk.screenMap.color_image_msaa, &vk.screenMap.color_image_view_msaa,
									 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, qtrue, 1 );
		}

		// screenmap/msaa-resolve
		create_color_attachment( vk.screenMapWidth, vk.screenMapHeight, VK_SAMPLE_COUNT_1_BIT,
								 vk.color_format, usage, &vk.screenMap.color_image,
								 &vk.screenMap.color_image_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
								 qfalse, 1 );

		// screenmap depth
		create_depth_attachment( 0, vk.screenMapWidth, vk.screenMapHeight, vk.screenMapSamples,
								 &vk.screenMap.depth_image, &vk.screenMap.depth_image_view, qtrue, 1 );

		if ( vk.msaaActive ) {
			create_color_attachment_flags( vk_fdm_attachment_flags(), vk.sceneWidth, vk.sceneHeight, vkSamples,
										   vk.color_format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &vk.msaa_image,
										   &vk.msaa_image_view,
									 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, qtrue, (vk.multiview ? 2 : 1) );
		}

		{
			// capture buffer: screenshots composite into it at the capture size
			usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
			create_color_attachment( gls.captureWidth, gls.captureHeight, VK_SAMPLE_COUNT_1_BIT,
									 vk.capture_format, usage, &vk.capture.image, &vk.capture.image_view,
									 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, qfalse, 1 );
		}
	} // if ( vk.fboActive )
	else if ( vk.xrDirect ) {
		if ( vk.msaaActive ) {
			// resolves into the XR image, so it must declare the same format
			create_color_attachment_flags( vk_fdm_attachment_flags(), vk.sceneWidth, vk.sceneHeight, vkSamples,
										   vk.mainColorFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &vk.msaa_image,
										   &vk.msaa_image_view, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, qtrue, 2 );
		}
		// the mirror samples this on frames without an acquired XR image
		create_color_attachment_flags( vk_fdm_attachment_flags(), vk.sceneWidth, vk.sceneHeight,
									   VK_SAMPLE_COUNT_1_BIT, vk.mainColorFormat,
									   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
									   &vk.xr_direct.idle.image, &vk.xr_direct.idle.view,
									   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, qfalse, 2 );
		// screenshots take the finished eye image through the capture pass
		create_color_attachment( gls.captureWidth, gls.captureHeight, VK_SAMPLE_COUNT_1_BIT, vk.capture_format,
								 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &vk.capture.image,
								 &vk.capture.image_view, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, qfalse, 1 );
		// the virtual screen's controllers; the scene depth may be multisampled
		create_depth_attachment( 0, vk.sceneWidth, vk.sceneHeight, VK_SAMPLE_COUNT_1_BIT, &vk.post_depth,
								 &vk.post_depth_view, qtrue, 2 );
	}

	//vk_alloc_attachments();

	create_depth_attachment( vk_fdm_attachment_flags(), vk.sceneWidth, vk.sceneHeight, vkSamples, &vk.depth_image,
							 &vk.depth_image_view,
							 qtrue, (vk.multiview ? 2 : 1) );

	vk_alloc_attachments();

	if ( vk.xr_output.image ) {
		VkImageViewCreateInfo view;
		Com_Memset( &view, 0, sizeof( view ) );
		view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		view.image = vk.xr_output.image;
		view.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
		view.format = vk_unorm_twin( vk.xr_output.format );
		view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		view.subresourceRange.levelCount = 1;
		view.subresourceRange.layerCount = 2;
		VK_CHECK( qvkCreateImageView( vk.device, &view, NULL, &vk.xr_output.unorm_view ) );
	}

	for ( i = 0; i < vk.image_memory_count; i++ )
	{
		SET_OBJECT_NAME( vk.image_memory[i], va( "framebuffer memory chunk %i", i ), VK_DEBUG_REPORT_OBJECT_TYPE_DEVICE_MEMORY_EXT );
	}

	SET_OBJECT_NAME( vk.depth_image, "depth attachment", VK_DEBUG_REPORT_OBJECT_TYPE_IMAGE_EXT );
	SET_OBJECT_NAME( vk.depth_image_view, "depth attachment", VK_DEBUG_REPORT_OBJECT_TYPE_IMAGE_VIEW_EXT );

	SET_OBJECT_NAME( vk.color_image, "color attachment", VK_DEBUG_REPORT_OBJECT_TYPE_IMAGE_EXT );
	SET_OBJECT_NAME( vk.color_image_view, "color attachment", VK_DEBUG_REPORT_OBJECT_TYPE_IMAGE_VIEW_EXT );

	SET_OBJECT_NAME( vk.capture.image, "capture image", VK_DEBUG_REPORT_OBJECT_TYPE_IMAGE_VIEW_EXT );
	SET_OBJECT_NAME( vk.capture.image_view, "capture image view", VK_DEBUG_REPORT_OBJECT_TYPE_IMAGE_VIEW_EXT );

	for ( i = 0; i < ARRAY_LEN( vk.bloom_image ); i++ )
	{
		SET_OBJECT_NAME( vk.bloom_image[i], va( "bloom attachment %i", i ), VK_DEBUG_REPORT_OBJECT_TYPE_IMAGE_EXT );
		SET_OBJECT_NAME( vk.bloom_image_view[i], va( "bloom attachment %i", i ), VK_DEBUG_REPORT_OBJECT_TYPE_IMAGE_VIEW_EXT );
	}
}


static void vk_xr_direct_framebuffer( struct vkXRDirectTarget_s *t, const char *name ) {
	VkImageView attachments[4]; // color | depth | msaa color | shading rate
	VkFramebufferCreateInfo desc;

	Com_Memset( &desc, 0, sizeof( desc ) );
	desc.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	desc.renderPass = vk.render_pass.main;
	desc.width = vk.sceneWidth;
	desc.height = vk.sceneHeight;
	desc.layers = 1; // multiview selects the layer
	desc.pAttachments = attachments;
	attachments[0] = t->view;
	attachments[1] = vk.depth_image_view;
	desc.attachmentCount = 2;
	if ( vk.msaaActive )
		attachments[desc.attachmentCount++] = vk.msaa_image_view;
	if ( vk_foveation.image )
		attachments[desc.attachmentCount++] = vk_foveation.view;
	VK_CHECK( qvkCreateFramebuffer( vk.device, &desc, NULL, &t->main ) );
	SET_OBJECT_NAME( t->main, name, VK_DEBUG_REPORT_OBJECT_TYPE_FRAMEBUFFER_EXT );
	// the mono pass carries no density map
	if ( vk_fdm_active() )
		desc.attachmentCount--;
	vk_create_mono_framebuffer( &desc, vk.mono.pass.main, &t->mono );
}

static void vk_xr_direct_destroy_framebuffers( struct vkXRDirectTarget_s *t ) {
	if ( t->main )
		qvkDestroyFramebuffer( vk.device, t->main, NULL );
	if ( t->mono )
		qvkDestroyFramebuffer( vk.device, t->mono, NULL );
	if ( t->screen )
		qvkDestroyFramebuffer( vk.device, t->screen, NULL );
	t->main = t->mono = t->screen = VK_NULL_HANDLE;
}


static void vk_create_framebuffers( void )
{
	VkImageView attachments[6];
	VkFramebufferCreateInfo desc;
	uint32_t n;

	desc.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	desc.pNext = NULL;
	desc.flags = 0;
	desc.pAttachments = attachments;
	desc.layers = 1;

	for ( n = 0; n < vk.swapchain_image_count; n++ )
	{
		desc.renderPass = vk.render_pass.main;
		desc.attachmentCount = 2;
		if ( vk.xrDirect )
		{
			// main framebuffers live in vk.xr_direct
		}
		else if ( !vk.fboActive )
		{
			desc.width = gls.windowWidth;
			desc.height = gls.windowHeight;
			attachments[0] = vk.swapchain_image_views[n];
			attachments[1] = vk.depth_image_view;
			VK_CHECK( qvkCreateFramebuffer( vk.device, &desc, NULL, &vk.framebuffers.main[n] ) );

			SET_OBJECT_NAME( vk.framebuffers.main[n], va( "framebuffer - main %i", n ), VK_DEBUG_REPORT_OBJECT_TYPE_FRAMEBUFFER_EXT );
		}
		else
		{
			// same framebuffer configuration for the main and post-scene render passes
			if ( n == 0 )
			{
				desc.width = vk.sceneWidth;
				desc.height = vk.sceneHeight;
				attachments[0] = vk.color_image_view;
				attachments[1] = vk.depth_image_view;
				if ( vk.msaaActive )
				{
					desc.attachmentCount = 3;
					attachments[2] = vk.msaa_image_view;
				}
				if ( vk.hdrActive )
				{
					// same attachment order the main/post-scene render passes declare
					if ( vk.msaaActive )
					{
						attachments[3] = vk.emissive_image_view;      // resolve
						attachments[4] = vk.emissive_image_view_msaa; // msaa render target
						desc.attachmentCount = 5;
					}
					else
					{
						attachments[2] = vk.emissive_image_view;      // resolve
						desc.attachmentCount = 3;
					}
				}
				if ( vk_foveation.image ) {
					attachments[desc.attachmentCount++] = vk_foveation.view;
				}
				VK_CHECK( qvkCreateFramebuffer( vk.device, &desc, NULL, &vk.framebuffers.main[n] ) );
				SET_OBJECT_NAME( vk.framebuffers.main[n], "framebuffer - main", VK_DEBUG_REPORT_OBJECT_TYPE_FRAMEBUFFER_EXT );
				if ( vk_fdm_active() ) {
					desc.attachmentCount--; // the mono pass carries no density map
				}
				vk_create_mono_framebuffer( &desc, vk.mono.pass.main, &vk.mono.framebuffer.main );
				if ( vk.render_pass.post_scene ) {
					// the resolved scene over its own depth, plus the emissive layer; never the density map
					desc.renderPass = vk.render_pass.post_scene;
					desc.attachmentCount = 2;
					attachments[0] = vk.color_image_view;
					attachments[1] = vk.post_depth_view;
					if ( vk.hdrActive ) {
						attachments[desc.attachmentCount++] = vk.emissive_image_view;
					}
					if ( vk_foveation.image && !vk_fdm_active() ) {
						attachments[desc.attachmentCount++] = vk_foveation.view;
					}
					VK_CHECK( qvkCreateFramebuffer( vk.device, &desc, NULL, &vk.framebuffers.post_scene ) );
					SET_OBJECT_NAME( vk.framebuffers.post_scene, "framebuffer - post scene", VK_DEBUG_REPORT_OBJECT_TYPE_FRAMEBUFFER_EXT );
					vk_create_mono_framebuffer( &desc, vk.mono.pass.post_scene, &vk.mono.framebuffer.post_scene );
					desc.renderPass = vk.render_pass.main;
				}
			}
			else
			{
				vk.framebuffers.main[n] = vk.framebuffers.main[0];
			}
		}

		if ( vk.fboActive || vk.xrDirect )
		{
			// gamma correction
			desc.renderPass = vk.render_pass.gamma;
			desc.attachmentCount = 1;
			desc.width = gls.windowWidth;
			desc.height = gls.windowHeight;
			attachments[0] = vk.swapchain_image_views[n];
			VK_CHECK( qvkCreateFramebuffer( vk.device, &desc, NULL, &vk.framebuffers.gamma[n] ) );

			SET_OBJECT_NAME( vk.framebuffers.gamma[n], "framebuffer - gamma-correction", VK_DEBUG_REPORT_OBJECT_TYPE_FRAMEBUFFER_EXT );
		}
	}

	if ( vk.xrDirect )
	{
		for ( n = 0; n < vk.xr_direct.count; n++ )
			vk_xr_direct_framebuffer( &vk.xr_direct.target[n], va( "xr direct %u", n ) );
		vk_xr_direct_framebuffer( &vk.xr_direct.idle, "xr direct idle" );
	}
	vk_fdm_tile_size( vk.xrDirect ? vk.xr_direct.idle.main : vk.framebuffers.main[0] );

	// screenshots in the FBO and direct-mode flows both draw into the capture image
	if ( vk.capture.image != VK_NULL_HANDLE )
	{
		attachments[0] = vk.capture.image_view;

		desc.renderPass = vk.render_pass.capture;
		desc.pAttachments = attachments;
		desc.attachmentCount = 1;
		desc.width = gls.captureWidth;
		desc.height = gls.captureHeight;

		VK_CHECK( qvkCreateFramebuffer( vk.device, &desc, NULL, &vk.framebuffers.capture ) );
		SET_OBJECT_NAME( vk.framebuffers.capture, "framebuffer - capture", VK_DEBUG_REPORT_OBJECT_TYPE_FRAMEBUFFER_EXT );
	}

	if ( vk.fboActive )
	{
		// screenmap
		desc.renderPass = vk.render_pass.screenmap;
		desc.attachmentCount = 2;
		desc.width = vk.screenMapWidth;
		desc.height = vk.screenMapHeight;
		attachments[0] = vk.screenMap.color_image_view;
		attachments[1] = vk.screenMap.depth_image_view;
		if ( vk.screenMapSamples > VK_SAMPLE_COUNT_1_BIT )
		{
			desc.attachmentCount = 3;
			attachments[2] = vk.screenMap.color_image_view_msaa;
		}
		VK_CHECK( qvkCreateFramebuffer( vk.device, &desc, NULL, &vk.framebuffers.screenmap ) );
		SET_OBJECT_NAME( vk.framebuffers.screenmap, "framebuffer - screenmap", VK_DEBUG_REPORT_OBJECT_TYPE_FRAMEBUFFER_EXT );

		if ( vk.xr_output.image ) {
			attachments[0] = vk.xr_output.unorm_view;
			desc.attachmentCount = 1;
			desc.width = vk.sceneWidth;
			desc.height = vk.sceneHeight;
			desc.renderPass = vk.xr_output.pass;
			VK_CHECK( qvkCreateFramebuffer( vk.device, &desc, NULL, &vk.xr_output.framebuffer ) );
			vk_create_mono_framebuffer( &desc, vk.mono.pass.output, &vk.mono.framebuffer.output );
			desc.renderPass = vk.xr_output.eye_pass;
			for ( n = 0; n < vk.xr_output.eye_count; n++ ) {
				attachments[0] = vk.xr_output.eye_view[n];
				VK_CHECK( qvkCreateFramebuffer( vk.device, &desc, NULL, &vk.xr_output.eye_framebuffer[n] ) );
			}
			attachments[0] = vk.xr_output.unorm_view;
			attachments[1] = vk.post_depth_view;
			desc.attachmentCount = 2;
			desc.renderPass = vk.xr_output.screen_pass;
			VK_CHECK( qvkCreateFramebuffer( vk.device, &desc, NULL, &vk.xr_output.screen_framebuffer ) );
			desc.renderPass = vk.xr_output.screen_eye_pass;
			for ( n = 0; n < vk.xr_output.eye_count; n++ ) {
				attachments[0] = vk.xr_output.eye_view[n];
				VK_CHECK( qvkCreateFramebuffer( vk.device, &desc, NULL, &vk.xr_output.screen_eye_framebuffer[n] ) );
			}
		}

		if ( r_bloom->integer )
		{
			uint32_t width = vk_bloom_width();
			uint32_t height = vk_bloom_height();

			desc.attachmentCount = 1;

			for ( n = 0; n < ARRAY_LEN( vk.framebuffers.blur ); n += 2 )
			{
				width /= 2;
				height /= 2;

				desc.renderPass = vk.render_pass.blur[n];
				desc.width = width;
				desc.height = height;

				desc.attachmentCount = 1;

				attachments[0] = vk.bloom_image_view[n+0+1];
				VK_CHECK( qvkCreateFramebuffer( vk.device, &desc, NULL, &vk.framebuffers.blur[n+0] ) );
				vk_create_mono_framebuffer( &desc, vk.mono.pass.blur[n + 0],
											&vk.mono.framebuffer.blur[n + 0] );

				attachments[0] = vk.bloom_image_view[n+1+1];
				VK_CHECK( qvkCreateFramebuffer( vk.device, &desc, NULL, &vk.framebuffers.blur[n+1] ) );
				vk_create_mono_framebuffer( &desc, vk.mono.pass.blur[n + 1],
											&vk.mono.framebuffer.blur[n + 1] );

				SET_OBJECT_NAME( vk.framebuffers.blur[n+0], va( "framebuffer - blur %i", n+0 ), VK_DEBUG_REPORT_OBJECT_TYPE_FRAMEBUFFER_EXT );
				SET_OBJECT_NAME( vk.framebuffers.blur[n+1], va( "framebuffer - blur %i", n+1 ), VK_DEBUG_REPORT_OBJECT_TYPE_FRAMEBUFFER_EXT );
			}
		}
	}
}


static void vk_create_sync_primitives( void ) {
	VkSemaphoreCreateInfo desc;
	VkFenceCreateInfo fence_desc;
	uint32_t i;

	desc.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	desc.pNext = NULL;
	desc.flags = 0;

#ifdef USE_UPLOAD_QUEUE
	VK_CHECK( qvkCreateSemaphore( vk.device, &desc, NULL, &vk.image_uploaded2 ) );
#endif

	// all commands submitted
	for ( i = 0; i < NUM_COMMAND_BUFFERS; i++ )
	{
		desc.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		desc.pNext = NULL;
		desc.flags = 0;

		// swapchain image acquired
		VK_CHECK( qvkCreateSemaphore( vk.device, &desc, NULL, &vk.tess[i].image_acquired ) );

#ifdef USE_UPLOAD_QUEUE
		// second semaphore to synchronize additional tasks (e.g. image upload)
		VK_CHECK( qvkCreateSemaphore( vk.device, &desc, NULL, &vk.tess[i].rendering_finished2 ) );
#endif
		fence_desc.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		fence_desc.pNext = NULL;
		//fence_desc.flags = VK_FENCE_CREATE_SIGNALED_BIT; // so it can be used to start rendering
		fence_desc.flags = 0; // non-signalled state

		VK_CHECK( qvkCreateFence( vk.device, &fence_desc, NULL, &vk.tess[i].rendering_finished_fence ) );
		vk.tess[i].waitForFence = qfalse;

		SET_OBJECT_NAME( vk.tess[i].image_acquired, va( "image_acquired semaphore %i", i ), VK_DEBUG_REPORT_OBJECT_TYPE_SEMAPHORE_EXT );
#ifdef USE_UPLOAD_QUEUE
		SET_OBJECT_NAME( vk.tess[i].rendering_finished2, va( "rendering_finished2 semaphore %i", i ), VK_DEBUG_REPORT_OBJECT_TYPE_SEMAPHORE_EXT );
#endif
		SET_OBJECT_NAME( vk.tess[i].rendering_finished_fence, va( "rendering_finished fence %i", i ), VK_DEBUG_REPORT_OBJECT_TYPE_FENCE_EXT );
	}

	fence_desc.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fence_desc.pNext = NULL;
	fence_desc.flags = 0;

#ifdef USE_UPLOAD_QUEUE
	VK_CHECK( qvkCreateFence( vk.device, &fence_desc, NULL, &vk.aux_fence ) );
	SET_OBJECT_NAME( vk.aux_fence, "aux fence", VK_DEBUG_REPORT_OBJECT_TYPE_FENCE_EXT );

	vk.rendering_finished = VK_NULL_HANDLE;
	vk.image_uploaded = VK_NULL_HANDLE;
	vk.aux_fence_wait = qfalse;
#endif
}


static void vk_destroy_sync_primitives( void  ) {
	uint32_t i;
	if ( gpuTimePool ) {
		qvkDestroyQueryPool( vk.device, gpuTimePool, NULL );
		gpuTimePool = VK_NULL_HANDLE;
	}
	gpuTimeCount = gpuTimeWindow = 0;
	Com_Memset( gpuSegmentCount, 0, sizeof( gpuSegmentCount ) );
	++gpuTimeGeneration;

#ifdef USE_UPLOAD_QUEUE
	qvkDestroySemaphore( vk.device, vk.image_uploaded2, NULL );
#endif

	for ( i = 0; i < NUM_COMMAND_BUFFERS; i++ ) {
		qvkDestroySemaphore( vk.device, vk.tess[i].image_acquired, NULL );
#ifdef USE_UPLOAD_QUEUE
		qvkDestroySemaphore( vk.device, vk.tess[i].rendering_finished2, NULL );
#endif
		qvkDestroyFence( vk.device, vk.tess[i].rendering_finished_fence, NULL );
		vk.tess[i].waitForFence = qfalse;
		vk.tess[i].swapchain_image_acquired = qfalse;
		vk.tess[i].gpu_time_armed = vk.tess[i].gpu_time_pending = qfalse;
	}

#ifdef USE_UPLOAD_QUEUE
	qvkDestroyFence( vk.device, vk.aux_fence, NULL );

	vk.rendering_finished = VK_NULL_HANDLE;
	vk.image_uploaded = VK_NULL_HANDLE;
#endif
}


static void vk_destroy_framebuffers( void ) {
	uint32_t n;

	vk_destroy_mono_framebuffers( &vk.mono );
	if ( vk.xr_output.framebuffer ) {
		qvkDestroyFramebuffer( vk.device, vk.xr_output.framebuffer, NULL );
		vk.xr_output.framebuffer = VK_NULL_HANDLE;
	}
	if ( vk.xr_output.screen_framebuffer ) {
		qvkDestroyFramebuffer( vk.device, vk.xr_output.screen_framebuffer, NULL );
		vk.xr_output.screen_framebuffer = VK_NULL_HANDLE;
	}
	for ( n = 0; n < vk.xr_output.eye_count; n++ ) {
		if ( vk.xr_output.eye_framebuffer[n] ) {
			qvkDestroyFramebuffer( vk.device, vk.xr_output.eye_framebuffer[n], NULL );
			vk.xr_output.eye_framebuffer[n] = VK_NULL_HANDLE;
		}
		if ( vk.xr_output.screen_eye_framebuffer[n] ) {
			qvkDestroyFramebuffer( vk.device, vk.xr_output.screen_eye_framebuffer[n], NULL );
			vk.xr_output.screen_eye_framebuffer[n] = VK_NULL_HANDLE;
		}
	}
	// the target views outlive this: they are rebuilt only with the XR swapchain
	for ( n = 0; n < vk.xr_direct.count; n++ )
		vk_xr_direct_destroy_framebuffers( &vk.xr_direct.target[n] );
	vk_xr_direct_destroy_framebuffers( &vk.xr_direct.idle );

	for ( n = 0; n < vk.swapchain_image_count; n++ ) {
		if ( vk.framebuffers.main[n] != VK_NULL_HANDLE ) {
			if ( !vk.fboActive || n == 0 ) {
				qvkDestroyFramebuffer( vk.device, vk.framebuffers.main[n], NULL );
			}
			vk.framebuffers.main[n] = VK_NULL_HANDLE;
		}
		if ( vk.framebuffers.gamma[n] != VK_NULL_HANDLE ) {
			qvkDestroyFramebuffer( vk.device, vk.framebuffers.gamma[n], NULL );
			vk.framebuffers.gamma[n] = VK_NULL_HANDLE;
		}
	}


	if ( vk.framebuffers.post_scene != VK_NULL_HANDLE ) {
		qvkDestroyFramebuffer( vk.device, vk.framebuffers.post_scene, NULL );
		vk.framebuffers.post_scene = VK_NULL_HANDLE;
	}

	if ( vk.framebuffers.screenmap != VK_NULL_HANDLE ) {
		qvkDestroyFramebuffer( vk.device, vk.framebuffers.screenmap, NULL );
		vk.framebuffers.screenmap = VK_NULL_HANDLE;
	}

	if ( vk.framebuffers.capture != VK_NULL_HANDLE ) {
		qvkDestroyFramebuffer( vk.device, vk.framebuffers.capture, NULL );
		vk.framebuffers.capture = VK_NULL_HANDLE;
	}

	for ( n = 0; n < ARRAY_LEN( vk.framebuffers.blur ); n++ ) {
		if ( vk.framebuffers.blur[n] != VK_NULL_HANDLE ) {
			qvkDestroyFramebuffer( vk.device, vk.framebuffers.blur[n], NULL );
			vk.framebuffers.blur[n] = VK_NULL_HANDLE;
		}
	}
	if ( vk_foveation.debugPipeline ) {
		qvkDestroyPipeline( vk.device, vk_foveation.debugPipeline, NULL );
		vk_foveation.debugPipeline = VK_NULL_HANDLE;
	}
	if ( vk_fdm.debugLayout ) {
		qvkDestroyPipelineLayout( vk.device, vk_fdm.debugLayout, NULL );
		vk_fdm.debugLayout = VK_NULL_HANDLE;
	}
	VK_FovDestroy( &vk_foveation );
}


static void vk_destroy_swapchain( void ) {
	uint32_t i;

	for ( i = 0; i < vk.swapchain_image_count; i++ ) {
		if ( vk.swapchain_image_views[i] != VK_NULL_HANDLE ) {
			qvkDestroyImageView( vk.device, vk.swapchain_image_views[i], NULL );
			vk.swapchain_image_views[i] = VK_NULL_HANDLE;
		}
		if ( vk.swapchain_rendering_finished[i] != VK_NULL_HANDLE ) {
			qvkDestroySemaphore( vk.device, vk.swapchain_rendering_finished[i], NULL );
			vk.swapchain_rendering_finished[i] = VK_NULL_HANDLE;
		}
	}

	qvkDestroySwapchainKHR( vk.device, vk.swapchain, NULL );
}

static void vk_destroy_attachments( void );
static void vk_destroy_render_passes( void );
static void vk_destroy_pipelines( qboolean resetCount );

static void vk_screen_composition_shutdown( void );
static void vk_screen_target_init( void );

static void vk_restart_swapchain( const char *funcname, VkResult res )
{
	uint32_t i;

#ifdef _DEBUG
	ri.Printf( PRINT_WARNING, "%s(%s): restarting swapchain...\n", funcname, vk_result_string( res ) );
#else
	ri.Printf(PRINT_WARNING, "%s(): restarting swapchain...\n", funcname );
#endif

	vk_wait_idle();

	for ( i = 0; i < NUM_COMMAND_BUFFERS; i++ ) {
		qvkResetCommandBuffer( vk.tess[i].command_buffer, 0 );
	}

#ifdef USE_UPLOAD_QUEUE
	qvkResetCommandBuffer( vk.staging_command_buffer, 0 );
#endif

	vk_destroy_pipelines( qfalse );
	vk_screen_composition_shutdown();
	vk_destroy_framebuffers();
	vk_destroy_render_passes();
	vk_destroy_attachments();
	vk_destroy_swapchain();
	vk_destroy_sync_primitives();

	vk_select_surface_format( vk.physical_device, vk_surface );
	setup_surface_formats( vk.physical_device );
	// the XR target views keep their format across this restart
	if ( !vk.xrDirect )
		vk.mainColorFormat = vk.color_format;

	vk_create_sync_primitives();
	vk_create_swapchain( vk.physical_device, vk.device, vk_surface, vk.present_format, &vk.swapchain, qfalse );
	vk_create_attachments();
	vk_foveation_create();
	vk_create_render_passes();
	vk_create_framebuffers();

	vk_update_attachment_descriptors();
	vk_screen_target_init();

	vk_update_post_process_pipelines();
}


static void vk_set_render_scale( void )
{
	if ( gls.windowWidth != glConfig.vidWidth || gls.windowHeight != glConfig.vidHeight )
	{
		if ( r_renderScale->integer > 0 )
		{
			int scaleMode = r_renderScale->integer - 1;
			if ( scaleMode & 1 )
			{
				// preserve aspect ratio (black bars on sides)
				float windowAspect = (float) gls.windowWidth / (float) gls.windowHeight;
				float renderAspect = (float) glConfig.vidWidth / (float) glConfig.vidHeight;
				if ( windowAspect >= renderAspect )
				{
					float scale = (float)gls.windowHeight / ( float ) glConfig.vidHeight;
					int bias = ( gls.windowWidth - scale * (float) glConfig.vidWidth ) / 2;
					vk.blitX0 += bias;
				}
				else
				{
					float scale = (float)gls.windowWidth / ( float ) glConfig.vidWidth;
					int bias = ( gls.windowHeight - scale * (float) glConfig.vidHeight ) / 2;
					vk.blitY0 += bias;
				}
			}
			// linear filtering
			if ( scaleMode & 2 )
				vk.blitFilter = GL_LINEAR;
			else
				vk.blitFilter = GL_NEAREST;
		}

		vk.windowAdjusted = qtrue;
	}

	if ( r_fbo->integer && r_ext_supersample->integer && !r_renderScale->integer )
	{
		vk.blitFilter = GL_LINEAR;
	}
}

/* HUD */
#define VK_HUD_WIDTH 1280
#define VK_HUD_HEIGHT 960
static VkResult VK_HUD_EnsureDescriptor( VkDevice device, VkDescriptorPool pool, VkDescriptorSetLayout layout,
										 VkDescriptorSet *set ) {
	VkDescriptorSetAllocateInfo info = {0};
	if ( *set != VK_NULL_HANDLE ) {
		return VK_SUCCESS;
	}
	info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	info.descriptorPool = pool;
	info.descriptorSetCount = 1;
	info.pSetLayouts = &layout;
	return qvkAllocateDescriptorSets( device, &info, set );
}

typedef struct {
	image_t image;
	VkImage depth;
	VkImageView depthView;
	VkDeviceMemory colorMemory, depthMemory;
	VkFramebuffer framebuffer;
	VkRenderPass pass, clearPass;
	VkCommandBuffer mainCommand;
	renderPass_t savedPass;
	int width, height;
	float scaleX, scaleY;
	qboolean recording;
} vkHudTarget_t;
static vkHudTarget_t vk_hud;
static qboolean vk_hud_direct;
static int vk_hud_eye;
// Each eye's GL-style NDC bounds of what the open overlay HUD has drawn; empty while min exceeds max
static float vk_hud_box[2][4];

void vk_hud_set_direct( qboolean enabled ) {
	int e;
	if ( enabled && !vk_hud_direct ) {
		for ( e = 0; e < 2; e++ ) {
			vk_hud_box[e][0] = vk_hud_box[e][1] = 1e9f;
			vk_hud_box[e][2] = vk_hud_box[e][3] = -1e9f;
		}
	} else if ( !enabled && vk_hud_direct ) {
		// the HUD reads as one surface, so a density map keeps its whole drawn extent, gaps included
		qboolean valid[2];
		for ( e = 0; e < 2; e++ )
			valid[e] = vk_hud_box[e][2] > vk_hud_box[e][0] && vk_hud_box[e][3] > vk_hud_box[e][1];
		vk_foveation_hud_rect( (const float (*)[4])vk_hud_box, valid );
	}
	vk_hud_direct = enabled;
}

// Grows the box by a rectangle in the overlay's logical NDC (Vulkan y down), through each eye's HUD matrix
static void vk_hud_box_grow( float x0, float y0, float x1, float y1 ) {
	const float in[4] = {x0, y0, x1, y1};
	float m[16], r[4];
	int e;
	if ( !vk_hud_direct || !VK_XR_Drawing() )
		return;
	for ( e = 0; e < 2; e++ ) {
		VK_XR_HudMatrix( e, m );
		if ( !VK_FdmPlaneBounds( m, in, r ) )
			continue;
		vk_hud_box[e][0] = MIN( vk_hud_box[e][0], r[0] );
		vk_hud_box[e][1] = MIN( vk_hud_box[e][1], r[1] );
		vk_hud_box[e][2] = MAX( vk_hud_box[e][2], r[2] );
		vk_hud_box[e][3] = MAX( vk_hud_box[e][3], r[3] );
	}
}

void vk_hud_keep_2d( float x, float y, float w, float h ) {
	if ( glConfig.vidWidth <= 0 || glConfig.vidHeight <= 0 )
		return;
	vk_hud_box_grow( 2 * x / glConfig.vidWidth - 1, 2 * y / glConfig.vidHeight - 1,
					 2 * (x + w) / glConfig.vidWidth - 1, 2 * (y + h) / glConfig.vidHeight - 1 );
}

// A 3D icon lands in its viewport through VK_HudViewportMatrix
void vk_hud_keep_view( void ) {
	const float y = glConfig.vidHeight - backEnd.viewParms.viewportY - backEnd.viewParms.viewportHeight;
	vk_hud_keep_2d( backEnd.viewParms.viewportX, y, backEnd.viewParms.viewportWidth,
					backEnd.viewParms.viewportHeight );
}

static void vk_hud_attachment( VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect,
							   VkImage *image, VkImageView *view, VkDeviceMemory *memory, int width,
							   int height, uint32_t mipLevels ) {
	VkImageCreateInfo ci;
	VkImageViewCreateInfo vi;
	VkMemoryRequirements requirements;
	VkMemoryAllocateInfo allocation;
	Com_Memset( &ci, 0, sizeof( ci ) );
	ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ci.imageType = VK_IMAGE_TYPE_2D;
	ci.format = format;
	ci.extent.width = width;
	ci.extent.height = height;
	ci.extent.depth = 1;
	ci.mipLevels = mipLevels;
	ci.arrayLayers = 1;
	ci.samples = VK_SAMPLE_COUNT_1_BIT;
	ci.tiling = VK_IMAGE_TILING_OPTIMAL;
	ci.usage = usage;
	ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VK_CHECK( qvkCreateImage( vk.device, &ci, NULL, image ) );
	qvkGetImageMemoryRequirements( vk.device, *image, &requirements );
	Com_Memset( &allocation, 0, sizeof( allocation ) );
	allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocation.allocationSize = requirements.size;
	allocation.memoryTypeIndex =
		find_memory_type( requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT );
	VK_CHECK( qvkAllocateMemory( vk.device, &allocation, NULL, memory ) );
	VK_CHECK( qvkBindImageMemory( vk.device, *image, *memory, 0 ) );
	Com_Memset( &vi, 0, sizeof( vi ) );
	vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	vi.image = *image;
	vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vi.format = format;
	vi.subresourceRange.aspectMask = aspect;
	vi.subresourceRange.levelCount = mipLevels;
	vi.subresourceRange.layerCount = 1;
	VK_CHECK( qvkCreateImageView( vk.device, &vi, NULL, view ) );
}

/* Enough levels for the reflection's blurred LOD. */
#define VK_SCREEN_MIP_LEVELS 5
/* Must match SPAN in virtualreflect.frag. */
#define VK_SCREEN_REFLECT_SPAN 0.25f
/* Store the floating screen at per-eye resolution to avoid downsampling menus. */
typedef struct {
	image_t image;
	uint32_t mips;
	VkDeviceMemory memory;
	VkRenderPass pass;
} vkScreenTarget_t;
static vkScreenTarget_t vk_screen;

/* Trilinear with no LOD clamp, so the reflection can sample its blurred mip. */
static void vk_screen_write_descriptor( void ) {
	Vk_Sampler_Def def;
	VkDescriptorImageInfo info;
	VkWriteDescriptorSet write;
	Com_Memset( &def, 0, sizeof( def ) );
	def.address_mode = vk_screen.image.wrapClampMode;
	def.gl_mag_filter = GL_LINEAR;
	def.gl_min_filter = GL_LINEAR_MIPMAP_LINEAR;
	def.noAnisotropy = qtrue;
	info.sampler = vk_find_sampler( &def );
	info.imageView = vk_screen.image.view;
	info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	Com_Memset( &write, 0, sizeof( write ) );
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = vk_screen.image.descriptor;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = &info;
	qvkUpdateDescriptorSets( vk.device, 1, &write, 0, NULL );
}

static void vk_screen_target_init( void ) {
	vk_screen.image.imgName = vk_screen.image.imgName2 = "*virtualScreenBuffer";
	if ( !vk.multiview ) {
		if ( tr.blackImage )
			vk_screen.image.descriptor = tr.blackImage->descriptor;
		return;
	}
	if ( !vk_screen.image.handle ) {
		VkCommandBuffer initial;
		VkClearColorValue black = {{0, 0, 0, 1}};
		VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, 1};
		const VkFormatFeatureFlags mipFeatures = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
												 VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
		VkFormatProperties properties;
		vk_screen.image.width = vk_screen.image.uploadWidth = vk.sceneWidth;
		vk_screen.image.height = vk_screen.image.uploadHeight = vk.sceneHeight;
		/* The mips come from linear blits; without them the reflection is sharp instead of blurred. */
		qvkGetPhysicalDeviceFormatProperties( vk.physical_device, vk.color_format, &properties );
		vk_screen.mips = (properties.optimalTilingFeatures & mipFeatures) == mipFeatures ? VK_SCREEN_MIP_LEVELS : 1;
		vk_hud_attachment( vk.color_format,
						   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
							   VK_IMAGE_USAGE_TRANSFER_DST_BIT,
						   VK_IMAGE_ASPECT_COLOR_BIT, &vk_screen.image.handle, &vk_screen.image.view,
						   &vk_screen.memory, vk_screen.image.uploadWidth, vk_screen.image.uploadHeight,
						   vk_screen.mips );
		initial = begin_command_buffer();
		record_image_layout_transition( initial, vk_screen.image.handle, VK_IMAGE_ASPECT_COLOR_BIT,
										VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
										VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT );
		qvkCmdClearColorImage( initial, vk_screen.image.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black,
							   1, &range );
		record_image_layout_transition( initial, vk_screen.image.handle, VK_IMAGE_ASPECT_COLOR_BIT,
										VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
										VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
										VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT );
		end_command_buffer( initial, __func__ );
		ri.Printf( PRINT_ALL, "VR virtual screen capture: %dx%d, %u mips\n", vk_screen.image.uploadWidth,
				   vk_screen.image.uploadHeight, vk_screen.mips );
	}
	if ( !vk_screen.pass ) {
		VkAttachmentDescription attachments[2] = {{0}, {0}};
		VkAttachmentReference color = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
		VkAttachmentReference depth = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
		VkImageView views[2];
		VkSubpassDescription subpass = {0};
		VkRenderPassCreateInfo pass = {0};
		VkRenderPassMultiviewCreateInfo multiview;
		const uint32_t mask = VK_PassViewMask( qtrue, RENDER_PASS_VR_SCREEN );
		VkFramebufferCreateInfo framebuffer = {0};
		VkSubpassDependency dependencies[2] = {{0}, {0}};
		attachments[0].format = vk.mainColorFormat;
		attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
		attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachments[0].initialLayout = attachments[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		if ( vk.xrDirect ) {
			attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		}
		// the controllers sort by depth; nothing reads it afterwards
		attachments[1].format = vk.depth_format;
		attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
		attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = 1;
		subpass.pColorAttachments = &color;
		subpass.pDepthStencilAttachment = &depth;
		dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
		dependencies[0].dstSubpass = 0;
		dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
		dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		if ( vk.xrDirect ) {
			// the blit reads this same image just before the clear
			dependencies[0].srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
			dependencies[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		}
		dependencies[0].srcStageMask |= VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		dependencies[0].srcAccessMask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		dependencies[0].dstStageMask |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		dependencies[0].dstAccessMask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		dependencies[1].srcSubpass = 0;
		dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
		dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
		pass.attachmentCount = 2;
		pass.pAttachments = attachments;
		pass.subpassCount = 1;
		pass.pSubpasses = &subpass;
		pass.dependencyCount = 2;
		pass.pDependencies = dependencies;
		vk_multiview_pass( &pass, &multiview, &mask );
		VK_CHECK( qvkCreateRenderPass( vk.device, &pass, NULL, &vk_screen.pass ) );
		framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		framebuffer.renderPass = vk_screen.pass;
		framebuffer.attachmentCount = 2;
		framebuffer.pAttachments = views;
		views[1] = vk.post_depth_view;
		framebuffer.width = vk.sceneWidth;
		framebuffer.height = vk.sceneHeight;
		framebuffer.layers = 1;
		if ( vk.xrDirect ) {
			uint32_t i;
			for ( i = 0; i <= vk.xr_direct.count; i++ ) {
				struct vkXRDirectTarget_s *t =
					i < vk.xr_direct.count ? &vk.xr_direct.target[i] : &vk.xr_direct.idle;
				views[0] = t->view;
				VK_CHECK( qvkCreateFramebuffer( vk.device, &framebuffer, NULL, &t->screen ) );
			}
		}
	}
	vk_screen.image.flags = IMGFLAG_CLAMPTOEDGE;
	vk_screen.image.wrapClampMode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	VK_CHECK( VK_HUD_EnsureDescriptor( vk.device, vk.target_descriptor_pool, vk.set_layout_sampler,
									   &vk_screen.image.descriptor ) );
	vk_screen_write_descriptor();
}
image_t *vk_screen_image( void ) {
	return vk_screen.image.descriptor ? &vk_screen.image : NULL;
}
/* The composition framebuffers borrow the direct-mode target views.
 * Retire them before swapchain recreation destroys those views. Keep the mono
 * capture image and descriptor alive across desktop resizes. */
static void vk_screen_composition_shutdown( void ) {
	uint32_t i;
	for ( i = 0; i < vk.xr_direct.count; i++ ) {
		if ( vk.xr_direct.target[i].screen )
			qvkDestroyFramebuffer( vk.device, vk.xr_direct.target[i].screen, NULL );
		vk.xr_direct.target[i].screen = VK_NULL_HANDLE;
	}
	if ( vk.xr_direct.idle.screen )
		qvkDestroyFramebuffer( vk.device, vk.xr_direct.idle.screen, NULL );
	vk.xr_direct.idle.screen = VK_NULL_HANDLE;
	if ( vk_screen.pass )
		qvkDestroyRenderPass( vk.device, vk_screen.pass, NULL );
	vk_screen.pass = VK_NULL_HANDLE;
}
static void vk_screen_target_shutdown( void ) {
	vk_screen_composition_shutdown();
	if ( vk_screen.image.view )
		qvkDestroyImageView( vk.device, vk_screen.image.view, NULL );
	if ( vk_screen.image.handle )
		qvkDestroyImage( vk.device, vk_screen.image.handle, NULL );
	if ( vk_screen.memory )
		qvkFreeMemory( vk.device, vk_screen.memory, NULL );
	Com_Memset( &vk_screen, 0, sizeof( vk_screen ) );
}

void vk_hud_init( void ) {
	vk_screen_target_init();
	vk_hud.image.imgName = vk_hud.image.imgName2 = "*hudBuffer";
	vk_hud.image.width = vk_hud.image.uploadWidth = VK_HUD_WIDTH;
	vk_hud.image.height = vk_hud.image.uploadHeight = VK_HUD_HEIGHT;
	if ( !vk.multiview ) {
		/* Refresh the borrowed fallback after a game-media descriptor reset. */
		if ( tr.blackImage ) {
			vk_hud.image.descriptor = tr.blackImage->descriptor;
		}
		return;
	}
	if ( !vk_hud.framebuffer ) {
		VkAttachmentDescription attachments[2];
		VkAttachmentReference color = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
		VkAttachmentReference depth = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
		VkSubpassDescription subpass;
		VkSubpassDependency dependencies[2];
		VkRenderPassCreateInfo pass;
		VkFramebufferCreateInfo fb;
		VkImageView views[2];
		VkCommandBufferAllocateInfo commands;
		VkCommandBuffer initial;
		VkClearColorValue transparent = {{0, 0, 0, 0}};
		VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		int i;
		vk_hud_attachment( vk.color_format,
						   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
							   VK_IMAGE_USAGE_TRANSFER_DST_BIT,
						   VK_IMAGE_ASPECT_COLOR_BIT, &vk_hud.image.handle, &vk_hud.image.view,
						   &vk_hud.colorMemory, VK_HUD_WIDTH, VK_HUD_HEIGHT, 1 );
		vk_hud_attachment(
			vk.depth_format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
			VK_IMAGE_ASPECT_DEPTH_BIT | (glConfig.stencilBits ? VK_IMAGE_ASPECT_STENCIL_BIT : 0),
			&vk_hud.depth, &vk_hud.depthView, &vk_hud.depthMemory, VK_HUD_WIDTH, VK_HUD_HEIGHT, 1 );
		Com_Memset( attachments, 0, sizeof( attachments ) );
		attachments[0].format = vk.color_format;
		attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
		attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachments[0].initialLayout = attachments[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		attachments[1].format = vk.depth_format;
		attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
		attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		Com_Memset( &subpass, 0, sizeof( subpass ) );
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = 1;
		subpass.pColorAttachments = &color;
		subpass.pDepthStencilAttachment = &depth;
		Com_Memset( dependencies, 0, sizeof( dependencies ) );
		dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
		dependencies[0].dstSubpass = 0;
		dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
									   VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
									   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
									   VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
									   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		dependencies[0].srcAccessMask =
			VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		dependencies[0].dstAccessMask =
			VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
			VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		dependencies[1].srcSubpass = 0;
		dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
		dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
									   VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
									   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
									   VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
									   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		dependencies[1].srcAccessMask =
			VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
										VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
										VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		Com_Memset( &pass, 0, sizeof( pass ) );
		pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
		pass.attachmentCount = 2;
		pass.pAttachments = attachments;
		pass.subpassCount = 1;
		pass.pSubpasses = &subpass;
		pass.dependencyCount = 2;
		pass.pDependencies = dependencies;
		VK_CHECK( qvkCreateRenderPass( vk.device, &pass, NULL, &vk_hud.pass ) );
		attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		VK_CHECK( qvkCreateRenderPass( vk.device, &pass, NULL, &vk_hud.clearPass ) );
		views[0] = vk_hud.image.view;
		views[1] = vk_hud.depthView;
		Com_Memset( &fb, 0, sizeof( fb ) );
		fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		fb.renderPass = vk_hud.pass;
		fb.attachmentCount = 2;
		fb.pAttachments = views;
		fb.width = VK_HUD_WIDTH;
		fb.height = VK_HUD_HEIGHT;
		fb.layers = 1;
		VK_CHECK( qvkCreateFramebuffer( vk.device, &fb, NULL, &vk_hud.framebuffer ) );
		Com_Memset( &commands, 0, sizeof( commands ) );
		commands.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		commands.commandPool = vk.command_pool;
		commands.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		commands.commandBufferCount = 1;
		for ( i = 0; i < NUM_COMMAND_BUFFERS; i++ ) {
			VK_CHECK( qvkAllocateCommandBuffers( vk.device, &commands, &vk.tess[i].hud_command_buffer ) );
		}
		initial = begin_command_buffer();
		record_image_layout_transition( initial, vk_hud.image.handle, VK_IMAGE_ASPECT_COLOR_BIT,
										VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
										VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT );
		qvkCmdClearColorImage( initial, vk_hud.image.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
							   &transparent, 1, &range );
		record_image_layout_transition( initial, vk_hud.image.handle, VK_IMAGE_ASPECT_COLOR_BIT,
										VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
										VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
										VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT );
		end_command_buffer( initial, __func__ );
	}
	vk_hud.image.flags = IMGFLAG_CLAMPTOEDGE;
	vk_hud.image.wrapClampMode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	/* Target descriptors survive game-media resets with their images. */
	VK_CHECK( VK_HUD_EnsureDescriptor( vk.device, vk.target_descriptor_pool, vk.set_layout_sampler,
									   &vk_hud.image.descriptor ) );
	vk_update_descriptor_set( &vk_hud.image, qfalse );
}

/* Their samplers don't live in tr.images, so GL_TextureMode's sampler rebuild leaves these two stale. */
void vk_update_screen_hud_descriptors( void ) {
	if ( vk_screen.image.view ) {
		vk_screen_write_descriptor();
	}
	if ( vk_hud.image.view ) {
		vk_update_descriptor_set( &vk_hud.image, qfalse );
	}
}

static void vk_hud_invalidate_caches( void ) {
	vk.cmd->last_pipeline = VK_NULL_HANDLE;
	vk.cmd->curr_index_buffer = VK_NULL_HANDLE;
	Com_Memset( vk.cmd->buf_offset, 0, sizeof( vk.cmd->buf_offset ) );
	Com_Memset( vk.cmd->vbo_offset, 0, sizeof( vk.cmd->vbo_offset ) );
	Com_Memset( vk.cmd->descriptor_set.current, 0, sizeof( vk.cmd->descriptor_set.current ) );
	vk.cmd->descriptor_set.start = ~0U;
	vk.cmd->descriptor_set.end = 0;
	vk_update_descriptor( VK_DESC_UNIFORM, vk.cmd->uniform_descriptor );
	vk.cmd->depth_range = DEPTH_RANGE_COUNT;
}

void vk_hud_begin( qboolean clear ) {
	VkCommandBufferBeginInfo begin;
	VkRenderPassBeginInfo pass;
	VkClearValue values[2];
	if ( !vk_hud.framebuffer || !vk.frame_count || vk_hud.recording ) {
		ri.Error( ERR_DROP, "Invalid HUD render target/lifecycle" );
		return;
	}
	if ( !vk.cmd->hud_begun ) {
		Com_Memset( &begin, 0, sizeof( begin ) );
		begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		VK_CHECK( qvkBeginCommandBuffer( vk.cmd->hud_command_buffer, &begin ) );
		vk_gpu_time_stamp( vk.cmd->hud_command_buffer, 4 );
		vk.cmd->hud_begun = qtrue;
	}
	vk_hud.mainCommand = vk.cmd->command_buffer;
	vk_hud.savedPass = vk.renderPassIndex;
	vk_hud.width = vk.renderWidth;
	vk_hud.height = vk.renderHeight;
	vk_hud.scaleX = vk.renderScaleX;
	vk_hud.scaleY = vk.renderScaleY;
	vk.cmd->command_buffer = vk.cmd->hud_command_buffer;
	vk_hud.recording = qtrue;
	vk.renderPassIndex = RENDER_PASS_HUD;
	vk.renderWidth = VK_HUD_WIDTH;
	vk.renderHeight = VK_HUD_HEIGHT;
	vk.renderScaleX = (float)VK_HUD_WIDTH / glConfig.vidWidth;
	vk.renderScaleY = (float)VK_HUD_HEIGHT / glConfig.vidHeight;
	Com_Memset( values, 0, sizeof( values ) );
#ifndef USE_REVERSED_DEPTH
	values[1].depthStencil.depth = 1;
#endif
	Com_Memset( &pass, 0, sizeof( pass ) );
	pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	pass.renderPass = clear ? vk_hud.clearPass : vk_hud.pass;
	pass.framebuffer = vk_hud.framebuffer;
	pass.renderArea.extent.width = VK_HUD_WIDTH;
	pass.renderArea.extent.height = VK_HUD_HEIGHT;
	pass.clearValueCount = 2;
	pass.pClearValues = values;
	qvkCmdBeginRenderPass( vk.cmd->command_buffer, &pass, VK_SUBPASS_CONTENTS_INLINE );
	vk_hud_invalidate_caches();
}

void vk_hud_end( void ) {
	if ( !vk_hud.recording ) {
		return;
	}
	qvkCmdEndRenderPass( vk.cmd->command_buffer );
	vk.cmd->command_buffer = vk_hud.mainCommand;
	vk.renderPassIndex = vk_hud.savedPass;
	vk.renderWidth = vk_hud.width;
	vk.renderHeight = vk_hud.height;
	vk.renderScaleX = vk_hud.scaleX;
	vk.renderScaleY = vk_hud.scaleY;
	vk_hud.recording = qfalse;
	vk_hud_invalidate_caches();
}

/* Shader stages must retain this stable image address across a flat map load.
 * Its black fallback descriptor needs no HUD attachment allocation. */
image_t *vk_hud_image( void ) {
	return vk_hud.image.descriptor ? &vk_hud.image : NULL;
}
qboolean vk_hud_recording( void ) {
	return vk_hud.recording;
}

static void vk_hud_shutdown( void ) {
	vk_screen_target_shutdown();
	if ( vk_hud.framebuffer ) {
		qvkDestroyFramebuffer( vk.device, vk_hud.framebuffer, NULL );
	}
	if ( vk_hud.pass ) {
		qvkDestroyRenderPass( vk.device, vk_hud.pass, NULL );
	}
	if ( vk_hud.clearPass ) {
		qvkDestroyRenderPass( vk.device, vk_hud.clearPass, NULL );
	}
	if ( vk_hud.image.view ) {
		qvkDestroyImageView( vk.device, vk_hud.image.view, NULL );
	}
	if ( vk_hud.depthView ) {
		qvkDestroyImageView( vk.device, vk_hud.depthView, NULL );
	}
	if ( vk_hud.image.handle ) {
		qvkDestroyImage( vk.device, vk_hud.image.handle, NULL );
	}
	if ( vk_hud.depth ) {
		qvkDestroyImage( vk.device, vk_hud.depth, NULL );
	}
	if ( vk_hud.colorMemory ) {
		qvkFreeMemory( vk.device, vk_hud.colorMemory, NULL );
	}
	if ( vk_hud.depthMemory ) {
		qvkFreeMemory( vk.device, vk_hud.depthMemory, NULL );
	}
	Com_Memset( &vk_hud, 0, sizeof( vk_hud ) );
}

static void vk_xr_direct_init( void ) {
	VkImage images[VK_XR_DIRECT_MAX_IMAGES];
	VkImageViewCreateInfo desc;
	uint32_t i;

	Com_Memset( &vk.xr_direct, 0, sizeof( vk.xr_direct ) );
	if ( !VK_XR_SwapchainImages( &vk.xr_direct.imageFormat, &vk.xr_direct.count, images ) )
		return; // no swapchain yet: every frame draws into idle
	// MUTABLE_FORMAT usage allows the UNORM view; the scene is already display-encoded
	vk.xr_direct.viewFormat = vk_unorm_twin( vk.xr_direct.imageFormat );
	if ( vk.xr_direct.viewFormat == vk.xr_direct.imageFormat ) {
		// the runtime treats a UNORM image as linear and encodes the display-encoded scene again
		ri.Printf( PRINT_WARNING, "XR swapchain format %s has no sRGB encoding, rendering through the framebuffer\n",
			vk_format_string( vk.xr_direct.imageFormat ) );
		Com_Memset( &vk.xr_direct, 0, sizeof( vk.xr_direct ) );
		vk.xrDirect = qfalse;
		vk.fboActive = qtrue;
		return;
	}
	vk.mainColorFormat = vk.xr_direct.viewFormat;
	qvkGetPhysicalDeviceFormatProperties( vk.physical_device, vk.xr_direct.imageFormat, &vk.xr_direct.imageFormatProperties );
	Com_Memset( &desc, 0, sizeof( desc ) );
	desc.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	desc.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
	desc.format = vk.xr_direct.viewFormat;
	desc.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	desc.subresourceRange.levelCount = 1;
	desc.subresourceRange.layerCount = 2;
	for ( i = 0; i < vk.xr_direct.count; i++ ) {
		vk.xr_direct.target[i].image = images[i];
		desc.image = images[i];
		VK_CHECK( qvkCreateImageView( vk.device, &desc, NULL, &vk.xr_direct.target[i].view ) );
		SET_OBJECT_NAME( vk.xr_direct.target[i].view, va( "xr direct view %u", i ), VK_DEBUG_REPORT_OBJECT_TYPE_IMAGE_VIEW_EXT );
	}
}

struct vkXRDirectTarget_s *vk_xr_direct_target( void ) {
	if ( VK_XR_Drawing() && VK_XR_AcquiredIndex() < vk.xr_direct.count )
		return &vk.xr_direct.target[VK_XR_AcquiredIndex()];
	return &vk.xr_direct.idle;
}

/* A same-size sRGB swapchain takes the output pass directly; anything else is blitted. */
static void vk_xr_output_init( void ) {
	VkImage images[VK_XR_DIRECT_MAX_IMAGES];
	VkImageViewCreateInfo desc;
	VkFormat format;
	uint32_t count, width = 0, height = 0, i;

	vk.xr_output.format = VK_FORMAT_R8G8B8A8_SRGB;
	vk.xr_output.eye_count = 0;
	if ( !VK_XR_SwapchainImages( &format, &count, images ) )
		return;
	VK_XR_TargetSize( &width, &height );
	if ( ( format != VK_FORMAT_R8G8B8A8_SRGB && format != VK_FORMAT_B8G8R8A8_SRGB ) || width != vk.sceneWidth ||
		 height != vk.sceneHeight || count > VK_XR_DIRECT_MAX_IMAGES ) {
		ri.Printf( PRINT_ALL, "XR eye output: blit\n" );
		return;
	}
	vk.xr_output.format = format;
	Com_Memset( &desc, 0, sizeof( desc ) );
	desc.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	desc.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
	// MUTABLE_FORMAT usage allows the UNORM view; the eye pass writes display-encoded values
	desc.format = vk_unorm_twin( format );
	desc.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	desc.subresourceRange.levelCount = 1;
	desc.subresourceRange.layerCount = 2;
	for ( i = 0; i < count; i++ ) {
		vk.xr_output.eye_image[i] = images[i];
		desc.image = images[i];
		VK_CHECK( qvkCreateImageView( vk.device, &desc, NULL, &vk.xr_output.eye_view[i] ) );
		SET_OBJECT_NAME( vk.xr_output.eye_view[i], va( "xr eye view %u", i ), VK_DEBUG_REPORT_OBJECT_TYPE_IMAGE_VIEW_EXT );
	}
	vk.xr_output.eye_count = count;
	ri.Printf( PRINT_ALL, "XR eye output: direct to swapchain (%s)\n", vk_format_string( format ) );
}

void vk_initialize( void )
{
	char buf[64], driver_version[64];
	const char *vendor_name;
	VkPhysicalDeviceProperties props;
	uint32_t major;
	uint32_t minor;
	uint32_t patch;
	uint32_t maxSize;
	uint32_t i;

	init_vulkan_library();

	qvkGetDeviceQueue( vk.device, vk.queue_family_index, 0, &vk.queue );

	qvkGetPhysicalDeviceProperties( vk.physical_device, &props );
	vk.deviceLimits = props.limits;
	qvkGetPhysicalDeviceFormatProperties( vk.physical_device, VK_FORMAT_R8G8B8A8_SRGB,
										  &vk.screenFormatProperties );

	vk.cmd = vk.tess + 0;
	vk.uniform_alignment = props.limits.minUniformBufferOffsetAlignment;
	vk.uniform_item_size = PAD( (uint32_t)sizeof( vkUniform_t ), vk.uniform_alignment );

	// for flare visibility tests
	vk.storage_alignment = MAX( props.limits.minStorageBufferOffsetAlignment, 4 * sizeof( uint32_t ) );

	vk.maxAnisotropy = props.limits.maxSamplerAnisotropy;

	vk.blitFilter = GL_NEAREST;
	vk.windowAdjusted = qfalse;
	vk.blitX0 = vk.blitY0 = 0;

	vk_set_render_scale();

	vr_mirrorEnabled = ri.Cvar_Get( "vr_mirrorEnabled", VR_MIRROR_DEFAULT, 0 );
	vr_desktopContentType = ri.Cvar_Get( "vr_desktopContentType", "0", 0 );
	vr_desktopContentFit = ri.Cvar_Get( "vr_desktopContentFit", "1", 0 );
	vr_desktopMenuStyle = ri.Cvar_Get( "vr_desktopMenuStyle", "0", 0 );

	vk.fboActive = r_fbo->integer ? qtrue : qfalse;
	vk.xrDirect = ( VK_XR_Enabled() && !vk.fboActive ) ? qtrue : qfalse;
	if ( r_ext_multisample->integer && ( vk.fboActive || vk.xrDirect ) ) {
		vk.msaaActive = qtrue;
	}

	// multisampling

	vkMaxSamples = MIN( props.limits.sampledImageColorSampleCounts, props.limits.sampledImageDepthSampleCounts );

	if ( /*vk.fboActive &&*/ vk.msaaActive ) {
		VkSampleCountFlags mask = vkMaxSamples;
		vkSamples = MAX( log2pad( r_ext_multisample->integer, 1 ), VK_SAMPLE_COUNT_2_BIT );
		while ( vkSamples > mask )
				vkSamples >>= 1;
		ri.Printf( PRINT_ALL, "...using %ix MSAA\n", vkSamples );
	} else {
		vkSamples = VK_SAMPLE_COUNT_1_BIT;
	}

	vk.screenMapSamples = MIN( vkMaxSamples, VK_SAMPLE_COUNT_4_BIT );

	vk.screenMapWidth = (float) glConfig.vidWidth / 16.0;
	if ( vk.screenMapWidth < 4 )
		vk.screenMapWidth = 4;

	vk.screenMapHeight = (float) glConfig.vidHeight / 16.0;
	if ( vk.screenMapHeight < 4 )
		vk.screenMapHeight = 4;

	vk.defaults.geometry_size = VERTEX_BUFFER_SIZE;
	vk.defaults.staging_size = STAGING_BUFFER_SIZE;

	// get memory size & defaults
	{
		VkPhysicalDeviceMemoryProperties props;
		VkDeviceSize maxDedicatedSize = 0;
		VkDeviceSize maxBARSize = 0;
		qvkGetPhysicalDeviceMemoryProperties( vk.physical_device, &props );
		for ( i = 0; i < props.memoryTypeCount; i++ ) {
			if ( props.memoryTypes[i].propertyFlags == VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ) {
				maxDedicatedSize = props.memoryHeaps[props.memoryTypes[i].heapIndex].size;
			}
			else if ( props.memoryTypes[i].propertyFlags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ) {
				if ( maxDedicatedSize == 0 || props.memoryHeaps[props.memoryTypes[i].heapIndex].size > maxDedicatedSize ) {
					maxDedicatedSize = props.memoryHeaps[props.memoryTypes[i].heapIndex].size;
				}
			}
			if ( props.memoryTypes[i].propertyFlags == (VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ) {
				maxBARSize = props.memoryHeaps[props.memoryTypes[i].heapIndex].size;
			}
			else if ( (props.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) == (VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ) {
				if ( maxBARSize == 0 ) {
					maxBARSize = props.memoryHeaps[props.memoryTypes[i].heapIndex].size;
				}
			}
		}

		if ( maxDedicatedSize != 0 ) {
			ri.Printf( PRINT_ALL, "...device memory size: %iMB\n", (int)((maxDedicatedSize + (1024 * 1024) - 1) / (1024 * 1024)) );
		}
		if ( maxBARSize != 0 ) {
			if ( maxBARSize >= 128 * 1024 * 1024 ) {
				// user larger buffers to avoid potential reallocations
				vk.defaults.geometry_size = VERTEX_BUFFER_SIZE_HI;
				vk.defaults.staging_size = STAGING_BUFFER_SIZE_HI;
			}
#ifdef _DEBUG
			ri.Printf( PRINT_ALL, "...BAR memory size: %iMB\n", (int)((maxBARSize + (1024 * 1024) - 1) / (1024 * 1024)) );
#endif
		}
	}

	// fill glConfig information

	// maxTextureSize must not exceed IMAGE_CHUNK_SIZE
	maxSize = sqrtf( IMAGE_CHUNK_SIZE / 4 );
	// round down to next power of 2
	glConfig.maxTextureSize = MIN( props.limits.maxImageDimension2D, log2pad( maxSize, 0 ) );

	if ( glConfig.maxTextureSize > MAX_TEXTURE_SIZE )
		glConfig.maxTextureSize = MAX_TEXTURE_SIZE; // ResampleTexture() relies on that maximum

	// default chunk size, may be doubled on demand
	vk.image_chunk_size = IMAGE_CHUNK_SIZE;

	vk.maxLod = 1 + Q_log2( glConfig.maxTextureSize );

	if ( props.limits.maxPerStageDescriptorSamplers != 0xFFFFFFFF )
		glConfig.numTextureUnits = props.limits.maxPerStageDescriptorSamplers;
	else
		glConfig.numTextureUnits = props.limits.maxBoundDescriptorSets;
	if ( glConfig.numTextureUnits > MAX_TEXTURE_UNITS )
		glConfig.numTextureUnits = MAX_TEXTURE_UNITS;

	vk.maxBoundDescriptorSets = props.limits.maxBoundDescriptorSets;

	if ( r_ext_texture_env_add->integer != 0 )
		glConfig.textureEnvAddAvailable = qtrue;
	else
		glConfig.textureEnvAddAvailable = qfalse;

	glConfig.textureCompression = TC_NONE;

	major = VK_VERSION_MAJOR(props.apiVersion);
	minor = VK_VERSION_MINOR(props.apiVersion);
	patch = VK_VERSION_PATCH(props.apiVersion);

	// decode driver version
	switch ( props.vendorID ) {
		case 0x10DE: // NVidia
			Com_sprintf( driver_version, sizeof( driver_version ), "%i.%i.%i.%i",
				(props.driverVersion >> 22) & 0x3FF,
				(props.driverVersion >> 14) & 0x0FF,
				(props.driverVersion >> 6) & 0x0FF,
				(props.driverVersion >> 0) & 0x03F );
			break;
#ifdef _WIN32
		case 0x8086: // Intel
			Com_sprintf( driver_version, sizeof( driver_version ), "%i.%i",
				(props.driverVersion >> 14),
				(props.driverVersion >> 0) & 0x3FFF );
			break;
#endif
		default:
			Com_sprintf( driver_version, sizeof( driver_version ), "%i.%i.%i",
				(props.driverVersion >> 22),
				(props.driverVersion >> 12) & 0x3FF,
				(props.driverVersion >> 0) & 0xFFF );
	}

	Com_sprintf( glConfig.version_string, sizeof( glConfig.version_string ), "API: %i.%i.%i, Driver: %s",
		major, minor, patch, driver_version );

#ifdef _WIN32
	// Intel iGPU drivers from 101.5333 to 101.6737 have a known bug that causes
	// VK_ERROR_DEVICE_LOST during vkQueueSubmit, see https://github.com/ec-/Quake3e/issues/312
	if ( props.vendorID == 0x8086 ) {
		uint32_t drvMajor = props.driverVersion >> 14;
		uint32_t drvMinor = props.driverVersion & 0x3FFF;
		if ( drvMajor == 101 && drvMinor >= 5333 && drvMinor <= 6737 ) {
			Com_sprintf( vk.driverNote, sizeof( vk.driverNote ), S_COLOR_WARNING
				"\nWARNING: Intel driver %i.%i is known to cause Vulkan crashes.\n"
				"Consider updating to driver >= 101.6790 or downgrading to <= 101.5186.\n",
				drvMajor, drvMinor );
		}
	}
#endif

	vk.offscreenRender = qtrue;

	if ( props.vendorID == 0x1002 ) {
		vendor_name = "Advanced Micro Devices, Inc.";
	} else if ( props.vendorID == 0x106B ) {
		vendor_name = "Apple Inc.";
	} else if ( props.vendorID == 0x10DE ) {
		// https://github.com/SaschaWillems/Vulkan/issues/493
		// we can't render to offscreen presentation surfaces on nvidia
		vk.offscreenRender = qfalse;
		vendor_name = "NVIDIA";
	} else if ( props.vendorID == 0x14E4 ) {
		vendor_name = "Broadcom Inc.";
	} else if ( props.vendorID == 0x1AE0 ) {
		vendor_name = "Google Inc.";
	} else if ( props.vendorID == 0x8086 ) {
		vendor_name = "Intel Corporation";
	} else if ( props.vendorID == VK_VENDOR_ID_MESA ) {
		vendor_name = "MESA";
	} else {
		Com_sprintf( buf, sizeof( buf ), "VendorID: %04x", props.vendorID );
		vendor_name = buf;
	}

	Q_strncpyz( glConfig.vendor_string, vendor_name, sizeof( glConfig.vendor_string ) );
	Q_strncpyz( glConfig.renderer_string, renderer_name( &props ), sizeof( glConfig.renderer_string ) );

	SET_OBJECT_NAME( (intptr_t)vk.device, glConfig.renderer_string, VK_DEBUG_REPORT_OBJECT_TYPE_DEVICE_EXT );

	// do early texture mode setup to avoid redundant descriptor updates in GL_SetDefaultState()
	vk.samplers.filter_min = -1;
	vk.samplers.filter_max = -1;
	GL_TextureMode( r_textureMode->string );
	r_textureMode->modified = qfalse;

	//
	// Sync primitives.
	//
	vk_create_sync_primitives();

	//
	// Command pool.
	//
	{
		VkCommandPoolCreateInfo desc;

		desc.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		desc.pNext = NULL;
		desc.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		desc.queueFamilyIndex = vk.queue_family_index;

		VK_CHECK( qvkCreateCommandPool( vk.device, &desc, NULL, &vk.command_pool ) );

		SET_OBJECT_NAME( vk.command_pool, "command pool", VK_DEBUG_REPORT_OBJECT_TYPE_COMMAND_POOL_EXT );
	}

#ifdef USE_UPLOAD_QUEUE
	{
		VkCommandBufferAllocateInfo alloc_info;

		alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		alloc_info.pNext = NULL;
		alloc_info.commandPool = vk.command_pool;
		alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		alloc_info.commandBufferCount = 1;

		VK_CHECK( qvkAllocateCommandBuffers( vk.device, &alloc_info, &vk.staging_command_buffer ) );
	}
#endif

	//
	// Command buffers and color attachments.
	//
	for ( i = 0; i < NUM_COMMAND_BUFFERS; i++ )
	{
		VkCommandBufferAllocateInfo alloc_info;

		alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		alloc_info.pNext = NULL;
		alloc_info.commandPool = vk.command_pool;
		alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		alloc_info.commandBufferCount = 1;

		VK_CHECK( qvkAllocateCommandBuffers( vk.device, &alloc_info, &vk.tess[i].command_buffer ) );

		//SET_OBJECT_NAME( vk.tess[i].command_buffer, va( "command buffer %i", i ), VK_DEBUG_REPORT_OBJECT_TYPE_COMMAND_BUFFER_EXT );
	}

	//
	// Descriptor pool.
	//
	{
		VkDescriptorPoolSize pool_size[3];
		VkDescriptorPoolCreateInfo desc;
		uint32_t i, maxSets;

		pool_size[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		pool_size[0].descriptorCount = MAX_DRAWIMAGES + 1 + 1 + 1 + 1 + VK_NUM_BLOOM_PASSES * 2; // color, emissive, screenmap, bloom descriptors

		pool_size[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
		pool_size[1].descriptorCount = NUM_COMMAND_BUFFERS * 3 + 1;

		//pool_size[2].type = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
		//pool_size[2].descriptorCount = NUM_COMMAND_BUFFERS;

		pool_size[2].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
		pool_size[2].descriptorCount = 1 + NUM_COMMAND_BUFFERS;

		for ( i = 0, maxSets = 0; i < ARRAY_LEN( pool_size ); i++ ) {
			maxSets += pool_size[i].descriptorCount;
		}

		desc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		desc.pNext = NULL;
		desc.flags = 0;
		desc.maxSets = maxSets;
		desc.poolSizeCount = ARRAY_LEN( pool_size );
		desc.pPoolSizes = pool_size;

		VK_CHECK( qvkCreateDescriptorPool( vk.device, &desc, NULL, &vk.descriptor_pool ) );
	}

	//
	// Descriptor set layout.
	//
	vk_create_layout_binding( 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, &vk.set_layout_sampler );
	vk_create_composite_layout( &vk.set_layout_composite );
	vk_create_view_layout( VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, &vk.set_layout_uniform );
	vk_create_view_layout( VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, &vk.set_layout_storage );
	//vk_create_layout_binding( 0, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, VK_SHADER_STAGE_FRAGMENT_BIT, &vk.set_layout_input );

	//
	// Pipeline layouts.
	//
	{
		VkDescriptorSetLayout set_layouts[6];
		VkPipelineLayoutCreateInfo desc;
		VkPushConstantRange push_ranges[2];

		push_ranges[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
		push_ranges[0].offset = 0;
		push_ranges[0].size = 64; // 16 floats (mvp)

		push_ranges[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
		push_ranges[1].offset = 64;
		push_ranges[1].size = 12; // emissiveFactor, desktopWhiteScale, desktopOverbright

		// standard pipelines
		set_layouts[0] = vk.set_layout_uniform; // fog/dlight parameters
		set_layouts[1] = vk.set_layout_sampler; // diffuse
		set_layouts[2] = vk.set_layout_sampler; // lightmap / fog-only
		set_layouts[3] = vk.set_layout_sampler; // blend
		set_layouts[4] = vk.set_layout_sampler; // collapsed fog texture
		desc.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		desc.pNext = NULL;
		desc.flags = 0;
		desc.setLayoutCount = (vk.maxBoundDescriptorSets >= VK_DESC_COUNT) ? VK_DESC_COUNT : 4;
		desc.pSetLayouts = set_layouts;
		desc.pushConstantRangeCount = 2;
		desc.pPushConstantRanges = push_ranges;

		VK_CHECK(qvkCreatePipelineLayout(vk.device, &desc, NULL, &vk.pipeline_layout));

		// flare test pipeline
		set_layouts[0] = vk.set_layout_storage; // dynamic storage buffer

		desc.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		desc.pNext = NULL;
		desc.flags = 0;
		desc.setLayoutCount = 1;
		desc.pSetLayouts = set_layouts;
		desc.pushConstantRangeCount = 2;
		desc.pPushConstantRanges = push_ranges;

		VK_CHECK( qvkCreatePipelineLayout( vk.device, &desc, NULL, &vk.pipeline_layout_storage ) );

		// post-processing pipeline
		set_layouts[0] = vk.set_layout_sampler; // sampler - set 0: scene color
		set_layouts[1] = vk.set_layout_sampler; // sampler - set 1: emissive highlight layer
		set_layouts[2] = vk.set_layout_sampler; // sampler
		set_layouts[3] = vk.set_layout_sampler; // sampler

		desc.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		desc.pNext = NULL;
		desc.flags = 0;
		desc.setLayoutCount = 2;
		desc.pSetLayouts = set_layouts;
		VkPushConstantRange pp_push_range;
		pp_push_range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
		pp_push_range.offset = 0;
		pp_push_range.size = 2 * sizeof( int32_t ) + 6 * sizeof( float ); // hdrCalibrate + paperWhite, hdrPeak, hdrHighlight, hdrSaturation, hdrSaturationFull, hdrSoftKnee + bloomActive
		desc.pushConstantRangeCount = 1;
		desc.pPushConstantRanges = &pp_push_range;

		VK_CHECK( qvkCreatePipelineLayout( vk.device, &desc, NULL, &vk.pipeline_layout_post_process ) );

		desc.setLayoutCount = 1;
		desc.pSetLayouts = &vk.set_layout_composite;

		VK_CHECK( qvkCreatePipelineLayout( vk.device, &desc, NULL, &vk.pipeline_layout_composite ) );

		SET_OBJECT_NAME( vk.pipeline_layout, "pipeline layout - main", VK_DEBUG_REPORT_OBJECT_TYPE_PIPELINE_LAYOUT_EXT );
		SET_OBJECT_NAME( vk.pipeline_layout_post_process, "pipeline layout - post-processing", VK_DEBUG_REPORT_OBJECT_TYPE_PIPELINE_LAYOUT_EXT );
	}

	vk.geometry_buffer_size_new = vk.defaults.geometry_size;
	vk_create_geometry_buffers( vk.geometry_buffer_size_new );
	vk.geometry_buffer_size_new = 0;

	vk_create_storage_buffer( NUM_COMMAND_BUFFERS * MAX_FLARES * vk.storage_alignment );

	vk_create_shader_modules();

	{
		VkPipelineCacheCreateInfo ci;
		Com_Memset( &ci, 0, sizeof( ci ) );
		ci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
		VK_CHECK( qvkCreatePipelineCache( vk.device, &ci, NULL, &vk.pipelineCache ) );
	}

	vk.renderPassIndex = RENDER_PASS_MAIN; // default render pass

	// swapchain
	vk.initSwapchainLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	//vk.initSwapchainLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	vk_create_swapchain( vk.physical_device, vk.device, vk_surface, vk.present_format, &vk.swapchain, qtrue );
	VK_XR_Bind( vk_instance, vk.physical_device, vk.device, vk.queue_family_index,
				vk.xrDirect && vk_fdm_offsets_supported() ? VK_IMAGE_CREATE_FRAGMENT_DENSITY_MAP_OFFSET_BIT_QCOM : 0 );
	vk.sceneWidth = glConfig.vidWidth;
	vk.sceneHeight = glConfig.vidHeight;
	VK_XR_TargetSize( &vk.sceneWidth, &vk.sceneHeight );
	if ( vk.multiview ) {
		/* Game/UI coordinates are one eye, independent of the desktop mirror. */
		glConfig.vidWidth = vk.sceneWidth;
		glConfig.vidHeight = vk.sceneHeight;
		glConfig.windowAspect = (float)glConfig.vidWidth / glConfig.vidHeight;
	}
	if ( VK_XR_Enabled() ) {
		ri.Printf( PRINT_ALL, "XR multiview extent: %u x %u (logical %d x %d)\n", vk.sceneWidth,
				   vk.sceneHeight, glConfig.vidWidth, glConfig.vidHeight );
	}
	vk.mainColorFormat = vk.color_format;
	if ( vk.xrDirect )
		vk_xr_direct_init();
	if ( vk.fboActive && vk.multiview )
		vk_xr_output_init();

	// color/depth attachments
	vk_create_attachments();

	// renderpasses
	vk_foveation_create();
	vk_create_render_passes();

	// framebuffers for each swapchain image
	vk_create_framebuffers();

	// preallocate staging buffer
	if ( vk.defaults.staging_size == STAGING_BUFFER_SIZE_HI ) {
		vk_alloc_staging_buffer( vk.defaults.staging_size );
	}

	vk.active = qtrue;
}


void vk_create_pipelines( void )
{
	vk_alloc_persistent_pipelines();

	vk.pipelines_world_base = vk.pipelines_count;
}


static void vk_destroy_attachments( void )
{
	uint32_t i;

	if ( vk.bloom_image[1] ) {
		for ( i = 1; i < ARRAY_LEN( vk.bloom_image ); i++ ) {
			qvkDestroyImage( vk.device, vk.bloom_image[i], NULL );
			qvkDestroyImageView( vk.device, vk.bloom_image_view[i], NULL );
			vk.bloom_image[i] = VK_NULL_HANDLE;
			vk.bloom_image_view[i] = VK_NULL_HANDLE;
		}
	}

	if ( vk.color_image ) {
		qvkDestroyImage( vk.device, vk.color_image, NULL );
		qvkDestroyImageView( vk.device, vk.color_image_view, NULL );
		vk.color_image = VK_NULL_HANDLE;
		vk.color_image_view = VK_NULL_HANDLE;
	}

	if ( vk.emissive_image ) {
		qvkDestroyImage( vk.device, vk.emissive_image, NULL );
		qvkDestroyImageView( vk.device, vk.emissive_image_view, NULL );
		vk.emissive_image = VK_NULL_HANDLE;
		vk.emissive_image_view = VK_NULL_HANDLE;
	}

	if ( vk.emissive_image_msaa ) {
		qvkDestroyImage( vk.device, vk.emissive_image_msaa, NULL );
		qvkDestroyImageView( vk.device, vk.emissive_image_view_msaa, NULL );
		vk.emissive_image_msaa = VK_NULL_HANDLE;
		vk.emissive_image_view_msaa = VK_NULL_HANDLE;
	}

	if ( vk.msaa_image ) {
		qvkDestroyImage( vk.device, vk.msaa_image, NULL );
		qvkDestroyImageView( vk.device, vk.msaa_image_view, NULL );
		vk.msaa_image = VK_NULL_HANDLE;
		vk.msaa_image_view = VK_NULL_HANDLE;
	}

	qvkDestroyImage( vk.device, vk.depth_image, NULL );
	qvkDestroyImageView( vk.device, vk.depth_image_view, NULL );
	vk.depth_image = VK_NULL_HANDLE;
	vk.depth_image_view = VK_NULL_HANDLE;

	if ( vk.screenMap.color_image ) {
		qvkDestroyImage( vk.device, vk.screenMap.color_image, NULL );
		qvkDestroyImageView( vk.device, vk.screenMap.color_image_view, NULL );
		vk.screenMap.color_image = VK_NULL_HANDLE;
		vk.screenMap.color_image_view = VK_NULL_HANDLE;
	}

	if ( vk.screenMap.color_image_msaa ) {
		qvkDestroyImage( vk.device, vk.screenMap.color_image_msaa, NULL );
		qvkDestroyImageView( vk.device, vk.screenMap.color_image_view_msaa, NULL );
		vk.screenMap.color_image_msaa = VK_NULL_HANDLE;
		vk.screenMap.color_image_view_msaa = VK_NULL_HANDLE;
	}

	if ( vk.screenMap.depth_image ) {
		qvkDestroyImage( vk.device, vk.screenMap.depth_image, NULL );
		qvkDestroyImageView( vk.device, vk.screenMap.depth_image_view, NULL );
		vk.screenMap.depth_image = VK_NULL_HANDLE;
		vk.screenMap.depth_image_view = VK_NULL_HANDLE;
	}

	if ( vk.capture.image ) {
		qvkDestroyImage( vk.device, vk.capture.image, NULL );
		qvkDestroyImageView( vk.device, vk.capture.image_view, NULL );
		vk.capture.image = VK_NULL_HANDLE;
		vk.capture.image_view = VK_NULL_HANDLE;
	}
	if ( vk.xr_output.image ) {
		qvkDestroyImageView( vk.device, vk.xr_output.unorm_view, NULL );
		qvkDestroyImageView( vk.device, vk.xr_output.view, NULL );
		qvkDestroyImage( vk.device, vk.xr_output.image, NULL );
		vk.xr_output.image = VK_NULL_HANDLE;
		vk.xr_output.view = VK_NULL_HANDLE;
		vk.xr_output.unorm_view = VK_NULL_HANDLE;
	}
	if ( vk.post_depth ) {
		qvkDestroyImageView( vk.device, vk.post_depth_view, NULL );
		qvkDestroyImage( vk.device, vk.post_depth, NULL );
		vk.post_depth = VK_NULL_HANDLE;
		vk.post_depth_view = VK_NULL_HANDLE;
	}
	if ( vk.xr_direct.idle.image ) {
		qvkDestroyImageView( vk.device, vk.xr_direct.idle.view, NULL );
		qvkDestroyImage( vk.device, vk.xr_direct.idle.image, NULL );
		vk.xr_direct.idle.image = VK_NULL_HANDLE;
		vk.xr_direct.idle.view = VK_NULL_HANDLE;
	}

	for ( i = 0; i < vk.image_memory_count; i++ ) {
		qvkFreeMemory( vk.device, vk.image_memory[i], NULL );
	}

	vk.image_memory_count = 0;
}


static void vk_destroy_render_passes( void )
{
	uint32_t i;

	vk_destroy_mono_passes( &vk.mono );

	if ( vk.render_pass.main != VK_NULL_HANDLE ) {
		qvkDestroyRenderPass( vk.device, vk.render_pass.main, NULL );
		vk.render_pass.main = VK_NULL_HANDLE;
	}


	for ( i = 0; i < ARRAY_LEN( vk.render_pass.blur ); i++ ) {
		if ( vk.render_pass.blur[i] != VK_NULL_HANDLE ) {
			qvkDestroyRenderPass( vk.device, vk.render_pass.blur[i], NULL );
			vk.render_pass.blur[i] = VK_NULL_HANDLE;
		}
	}

	if ( vk.render_pass.post_scene != VK_NULL_HANDLE ) {
		qvkDestroyRenderPass( vk.device, vk.render_pass.post_scene, NULL );
		vk.render_pass.post_scene = VK_NULL_HANDLE;
	}

	if ( vk.render_pass.screenmap != VK_NULL_HANDLE ) {
		qvkDestroyRenderPass( vk.device, vk.render_pass.screenmap, NULL );
		vk.render_pass.screenmap = VK_NULL_HANDLE;
	}

	if ( vk.render_pass.gamma != VK_NULL_HANDLE ) {
		qvkDestroyRenderPass( vk.device, vk.render_pass.gamma, NULL );
		vk.render_pass.gamma = VK_NULL_HANDLE;
	}

	if ( vk.render_pass.capture != VK_NULL_HANDLE ) {
		qvkDestroyRenderPass( vk.device, vk.render_pass.capture, NULL );
		vk.render_pass.capture = VK_NULL_HANDLE;
	}
	if ( vk.xr_output.pass ) {
		qvkDestroyRenderPass( vk.device, vk.xr_output.pass, NULL );
		vk.xr_output.pass = VK_NULL_HANDLE;
	}
	if ( vk.xr_output.eye_pass ) {
		qvkDestroyRenderPass( vk.device, vk.xr_output.eye_pass, NULL );
		vk.xr_output.eye_pass = VK_NULL_HANDLE;
	}
	if ( vk.xr_output.screen_pass ) {
		qvkDestroyRenderPass( vk.device, vk.xr_output.screen_pass, NULL );
		vk.xr_output.screen_pass = VK_NULL_HANDLE;
	}
	if ( vk.xr_output.screen_eye_pass ) {
		qvkDestroyRenderPass( vk.device, vk.xr_output.screen_eye_pass, NULL );
		vk.xr_output.screen_eye_pass = VK_NULL_HANDLE;
	}
}


static void vk_destroy_pipelines( qboolean resetCounter )
{
	uint32_t i, j;

	vk_destroy_mono_pipelines( &vk.mono );

	for ( i = 0; i < vk.pipelines_count; i++ ) {
		for ( j = 0; j < RENDER_PASS_COUNT; j++ ) {
			if ( vk.pipelines[i].handle[j] != VK_NULL_HANDLE ) {
				qvkDestroyPipeline( vk.device, vk.pipelines[i].handle[j], NULL );
				vk.pipelines[i].handle[j] = VK_NULL_HANDLE;
				vk.pipeline_create_count--;
			}
		}
	}

	if ( resetCounter ) {
		Com_Memset( &vk.pipelines, 0, sizeof( vk.pipelines ) );
		vk.pipelines_count = 0;
	}

	if ( vk.capture_pipeline ) {
		qvkDestroyPipeline( vk.device, vk.capture_pipeline, NULL );
		vk.capture_pipeline = VK_NULL_HANDLE;
	}
	if ( vk.xr_output.composite_pipeline ) {
		qvkDestroyPipeline( vk.device, vk.xr_output.composite_pipeline, NULL );
		vk.xr_output.composite_pipeline = VK_NULL_HANDLE;
	}
	if ( vk.gamma_composite_pipeline ) {
		qvkDestroyPipeline( vk.device, vk.gamma_composite_pipeline, NULL );
		vk.gamma_composite_pipeline = VK_NULL_HANDLE;
	}
	if ( vk.xr_output.screen_mono_pipeline ) {
		qvkDestroyPipeline( vk.device, vk.xr_output.screen_mono_pipeline, NULL );
		vk.xr_output.screen_mono_pipeline = VK_NULL_HANDLE;
	}
	if ( vk.xr_output.screen_capture_pipeline ) {
		qvkDestroyPipeline( vk.device, vk.xr_output.screen_capture_pipeline, NULL );
		vk.xr_output.screen_capture_pipeline = VK_NULL_HANDLE;
	}
	for ( i = 0; i < 2; i++ ) {
		qvkDestroyPipeline( vk.device, vk.xr_output.screen_mirror_eye[i], NULL );
		qvkDestroyPipeline( vk.device, vk.xr_output.eye_mirror[i], NULL );
		vk.xr_output.screen_mirror_eye[i] = vk.xr_output.eye_mirror[i] = VK_NULL_HANDLE;
	}

	for ( i = 0; i < ARRAY_LEN( vk.blur_pipeline ); i++ ) {
		if ( vk.blur_pipeline[i] != VK_NULL_HANDLE ) {
			qvkDestroyPipeline( vk.device, vk.blur_pipeline[i], NULL );
			vk.blur_pipeline[i] = VK_NULL_HANDLE;
		}
	}
}

void vk_shutdown( refShutdownCode_t code )
{
	int i, j, k, l;
	uint32_t n;
	vk_hud_shutdown();
	if ( vk.xrDirect ) {
		// release everything on the runtime's images before the session destroys them
		vk_wait_idle();
		for ( n = 0; n < vk.xr_direct.count; n++ ) {
			vk_xr_direct_destroy_framebuffers( &vk.xr_direct.target[n] );
			if ( vk.xr_direct.target[n].view )
				qvkDestroyImageView( vk.device, vk.xr_direct.target[n].view, NULL );
		}
		vk.xr_direct.count = 0;
	}
	if ( vk.xr_output.eye_count ) {
		// the eye views wrap the runtime's images, which the session destroys
		vk_wait_idle();
		for ( n = 0; n < vk.xr_output.eye_count; n++ ) {
			if ( vk.xr_output.eye_framebuffer[n] )
				qvkDestroyFramebuffer( vk.device, vk.xr_output.eye_framebuffer[n], NULL );
			if ( vk.xr_output.screen_eye_framebuffer[n] )
				qvkDestroyFramebuffer( vk.device, vk.xr_output.screen_eye_framebuffer[n], NULL );
			qvkDestroyImageView( vk.device, vk.xr_output.eye_view[n], NULL );
			vk.xr_output.eye_framebuffer[n] = VK_NULL_HANDLE;
			vk.xr_output.screen_eye_framebuffer[n] = VK_NULL_HANDLE;
			vk.xr_output.eye_view[n] = VK_NULL_HANDLE;
		}
		vk.xr_output.eye_count = 0;
	}
	VK_XR_ShutdownSession();

	if ( qvkQueuePresentKHR == NULL ) { // not fully initialized
		goto __cleanup;
	}

	vk_destroy_framebuffers();

	vk_destroy_pipelines( qtrue ); // reset counter

	vk_destroy_render_passes();

	vk_destroy_attachments();

	vk_destroy_swapchain();

	if ( vk.pipelineCache != VK_NULL_HANDLE ) {
		qvkDestroyPipelineCache( vk.device, vk.pipelineCache, NULL );
		vk.pipelineCache = VK_NULL_HANDLE;
	}

	qvkDestroyCommandPool( vk.device, vk.command_pool, NULL );

	qvkDestroyDescriptorPool(vk.device, vk.descriptor_pool, NULL);
	qvkDestroyDescriptorPool( vk.device, vk.target_descriptor_pool, NULL );

	qvkDestroyDescriptorSetLayout(vk.device, vk.set_layout_sampler, NULL);
	qvkDestroyDescriptorSetLayout(vk.device, vk.set_layout_uniform, NULL);
	qvkDestroyDescriptorSetLayout(vk.device, vk.set_layout_storage, NULL);
	qvkDestroyDescriptorSetLayout( vk.device, vk.set_layout_composite, NULL );

	qvkDestroyPipelineLayout(vk.device, vk.pipeline_layout, NULL);
	qvkDestroyPipelineLayout(vk.device, vk.pipeline_layout_storage, NULL);
	qvkDestroyPipelineLayout(vk.device, vk.pipeline_layout_post_process, NULL);
	qvkDestroyPipelineLayout( vk.device, vk.pipeline_layout_composite, NULL );

#ifdef USE_VBO
	vk_release_vbo();
#endif

	vk_clean_staging_buffer();

	vk_release_geometry_buffers();

	vk_destroy_samplers();

	vk_destroy_sync_primitives();

	qvkDestroyBuffer( vk.device, vk.storage.buffer, NULL );
	qvkFreeMemory( vk.device, vk.storage.memory, NULL );

	for ( i = 0; i < 3; i++ ) {
		for ( j = 0; j < 2; j++ ) {
			for ( k = 0; k < 2; k++ ) {
				for ( l = 0; l < 2; l++ ) {
					if ( vk.modules.vert.gen[i][j][k][l] != VK_NULL_HANDLE ) {
						qvkDestroyShaderModule( vk.device, vk.modules.vert.gen[i][j][k][l], NULL );
						vk.modules.vert.gen[i][j][k][l] = VK_NULL_HANDLE;
					}
				}
			}
		}
	}
	for ( i = 0; i < 3; i++ ) {
		for ( j = 0; j < 2; j++ ) {
			for ( k = 0; k < 2; k++ ) {
				for ( l = 0; l < 2; l++ ) {
					if ( vk.modules.frag.gen[i][j][k][l] != VK_NULL_HANDLE ) {
						qvkDestroyShaderModule( vk.device, vk.modules.frag.gen[i][j][k][l], NULL );
						vk.modules.frag.gen[i][j][k][l] = VK_NULL_HANDLE;
					}
				}
			}
		}
	}
	for ( i = 0; i < 2; i++ ) {
		if ( vk.modules.vert.light[i] != VK_NULL_HANDLE ) {
			qvkDestroyShaderModule( vk.device, vk.modules.vert.light[i], NULL );
			vk.modules.vert.light[i] = VK_NULL_HANDLE;
		}
		for ( j = 0; j < 2; j++ ) {
			for ( k = 0; k < 2; k++ ) {
				if ( vk.modules.frag.light[i][j][k] != VK_NULL_HANDLE ) {
					qvkDestroyShaderModule( vk.device, vk.modules.frag.light[i][j][k], NULL );
					vk.modules.frag.light[i][j][k] = VK_NULL_HANDLE;
				}
			}
		}
	}

	for ( i = 0; i < 2; i++ ) {
		if ( vk.modules.vert.overbright_vert[i] != VK_NULL_HANDLE ) {
			qvkDestroyShaderModule( vk.device, vk.modules.vert.overbright_vert[i], NULL );
			vk.modules.vert.overbright_vert[i] = VK_NULL_HANDLE;
		}
		for ( j = 0; j < 2; j++ ) {
			if ( vk.modules.frag.overbright_frag[i][j] != VK_NULL_HANDLE ) {
				qvkDestroyShaderModule( vk.device, vk.modules.frag.overbright_frag[i][j], NULL );
				vk.modules.frag.overbright_frag[i][j] = VK_NULL_HANDLE;
			}
		}
	}

	for ( i = 0; i < 2; i++ ) {
		for ( j = 0; j < 2; j++ ) {
			for ( k = 0; k < 2; k++ ) {
				qvkDestroyShaderModule( vk.device, vk.modules.vert.ident1[i][j][k], NULL );
				vk.modules.vert.ident1[i][j][k] = VK_NULL_HANDLE;
				qvkDestroyShaderModule( vk.device, vk.modules.frag.ident1[i][j][k], NULL );
				vk.modules.frag.ident1[i][j][k] = VK_NULL_HANDLE;
			}
		}
	}

	for ( i = 0; i < 2; i++ ) {
		for ( j = 0; j < 2; j++ ) {
			for ( k = 0; k < 2; k++ ) {
				qvkDestroyShaderModule( vk.device, vk.modules.vert.fixed[i][j][k], NULL );
				vk.modules.vert.fixed[i][j][k] = VK_NULL_HANDLE;
				qvkDestroyShaderModule( vk.device, vk.modules.frag.fixed[i][j][k], NULL );
				vk.modules.frag.fixed[i][j][k] = VK_NULL_HANDLE;
			}
		}
	}

	for ( i = 0; i < 1; i++ ) {
		for ( j = 0; j < 2; j++ ) {
			for ( k = 0; k < 2; k++ ) {
				qvkDestroyShaderModule( vk.device, vk.modules.frag.ent[i][j][k], NULL );
				vk.modules.frag.ent[i][j][k] = VK_NULL_HANDLE;
			}
		}
	}

	qvkDestroyShaderModule( vk.device, vk.modules.floor_grid_fs, NULL );
	qvkDestroyShaderModule( vk.device, vk.modules.virtualscreen_fs, NULL );
	qvkDestroyShaderModule( vk.device, vk.modules.virtualreflect_fs, NULL );
	qvkDestroyShaderModule( vk.device, vk.modules.frag.gen0_df, NULL );

	qvkDestroyShaderModule( vk.device, vk.modules.color_fs, NULL );
	qvkDestroyShaderModule( vk.device, vk.modules.color_vs, NULL );

	qvkDestroyShaderModule(vk.device, vk.modules.fog_vs, NULL);
	qvkDestroyShaderModule(vk.device, vk.modules.fog_fs, NULL);

	qvkDestroyShaderModule(vk.device, vk.modules.dot_vs, NULL);
	qvkDestroyShaderModule(vk.device, vk.modules.dot_fs, NULL);

	qvkDestroyShaderModule(vk.device, vk.modules.blur_extract_fs, NULL);
	qvkDestroyShaderModule(vk.device, vk.modules.blur_fs, NULL);

	qvkDestroyShaderModule(vk.device, vk.modules.gamma_vs, NULL);
	qvkDestroyShaderModule(vk.device, vk.modules.gamma_fs, NULL);
	qvkDestroyShaderModule(vk.device, vk.modules.gamma_composite_fs, NULL);

	{
		VkShaderModule *module = (VkShaderModule *)&vk.modules.vert_mv;
		for ( i = 0; i < sizeof( vk.modules.vert_mv ) / sizeof( *module ); i++ )
			qvkDestroyShaderModule( vk.device, module[i], NULL );
		qvkDestroyShaderModule( vk.device, vk.modules.color_vs_mv, NULL );
		qvkDestroyShaderModule( vk.device, vk.modules.fog_vs_mv, NULL );
		qvkDestroyShaderModule( vk.device, vk.modules.dot_vs_mv, NULL );
		qvkDestroyShaderModule( vk.device, vk.modules.dot_fs_mv, NULL );
		qvkDestroyShaderModule( vk.device, vk.modules.dot_total_fs_mv, NULL );
		qvkDestroyShaderModule( vk.device, vk.modules.blur_extract_fs_mv, NULL );
		qvkDestroyShaderModule( vk.device, vk.modules.blur_fs_mv, NULL );
		qvkDestroyShaderModule( vk.device, vk.modules.gamma_fs_array, NULL );
		qvkDestroyShaderModule( vk.device, vk.modules.gamma_composite_fs_mv, NULL );
	}

__cleanup:
	if ( vk.device != VK_NULL_HANDLE ) {
		qvkDestroyDevice( vk.device, NULL );
	}

	deinit_device_functions();

	Com_Memset( &vk, 0, sizeof( vk ) );
	Com_Memset( &vk_world, 0, sizeof( vk_world ) );
	
	if ( code != REF_KEEP_CONTEXT ) {
		vk_destroy_instance();
		deinit_instance_functions();
		VK_XR_ShutdownInstance();
	}
}


void vk_wait_idle( void )
{
	VK_CHECK( qvkDeviceWaitIdle( vk.device ) );
}


void vk_queue_wait_idle( void )
{
	VK_CHECK( qvkQueueWaitIdle( vk.queue ) );
}


void vk_release_resources( void ) {
	int i, j;

	vk_wait_idle();

	// tr still names the controller models' buffers; R_Init clears it next
	for ( i = 0; i < (int)ARRAY_LEN( tr.xrAssets ); i++ ) {
		if ( tr.xrAssets[i].buffer )
			qvkDestroyBuffer( vk.device, tr.xrAssets[i].buffer, NULL );
		if ( tr.xrAssets[i].memory )
			qvkFreeMemory( vk.device, tr.xrAssets[i].memory, NULL );
		tr.xrAssets[i].buffer = VK_NULL_HANDLE;
		tr.xrAssets[i].memory = VK_NULL_HANDLE;
	}

	for (i = 0; i < vk_world.num_image_chunks; i++)
		qvkFreeMemory(vk.device, vk_world.image_chunks[i].memory, NULL);

	vk_clean_staging_buffer();

	// vk_destroy_samplers();

	for ( i = vk.pipelines_world_base; i < vk.pipelines_count; i++ ) {
		for ( j = 0; j < RENDER_PASS_COUNT; j++ ) {
			if ( vk.pipelines[i].handle[j] != VK_NULL_HANDLE ) {
				qvkDestroyPipeline( vk.device, vk.pipelines[i].handle[j], NULL );
				vk.pipelines[i].handle[j] = VK_NULL_HANDLE;
				vk.pipeline_create_count--;
			}
		}
		Com_Memset( &vk.pipelines[i], 0, sizeof( vk.pipelines[0] ) );
	}
	vk.pipelines_count = vk.pipelines_world_base;

	VK_CHECK( qvkResetDescriptorPool( vk.device, vk.descriptor_pool, 0 ) );

	if ( vk_world.num_image_chunks > 1 ) {
		// if we allocated more than 2 image chunks - use doubled default size
		vk.image_chunk_size = (IMAGE_CHUNK_SIZE * 2);
	}
#if 0 // do not reduce chunk size
	else if ( vk_world.num_image_chunks == 1 ) {
		// otherwise set to default if used less than a half
		if ( vk_world.image_chunks[0].used < ( IMAGE_CHUNK_SIZE - (IMAGE_CHUNK_SIZE / 10) ) ) {
			vk.image_chunk_size = IMAGE_CHUNK_SIZE;
		}
	}
#endif

	Com_Memset( &vk_world, 0, sizeof( vk_world ) );

	// Reset geometry buffers offsets
	for ( i = 0; i < NUM_COMMAND_BUFFERS; i++ ) {
		vk.tess[i].uniform_read_offset = 0;
		vk.tess[i].vertex_buffer_offset = 0;
	}

	Com_Memset( vk.cmd->buf_offset, 0, sizeof( vk.cmd->buf_offset ) );
	Com_Memset( vk.cmd->vbo_offset, 0, sizeof( vk.cmd->vbo_offset ) );

	Com_Memset( &vk.stats, 0, sizeof( vk.stats ) );
}

#if 0
static void record_buffer_memory_barrier(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize size, VkDeviceSize offset,
		VkPipelineStageFlags src_stages, VkPipelineStageFlags dst_stages,
		VkAccessFlags src_access, VkAccessFlags dst_access) {

	VkBufferMemoryBarrier barrier;
	barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	barrier.pNext = NULL;
	barrier.srcAccessMask = src_access;
	barrier.dstAccessMask = dst_access;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.buffer = buffer;
	barrier.offset = offset;
	barrier.size = size;

	qvkCmdPipelineBarrier( cb, src_stages, dst_stages, 0, 0, NULL, 1, &barrier, 0, NULL );
}
#endif

void vk_create_image( image_t *image, int width, int height, int mip_levels ) {

	VkFormat format = image->internalFormat;

	if ( image->handle ) {
		qvkDestroyImage( vk.device, image->handle, NULL );
		image->handle = VK_NULL_HANDLE;
	}

	if ( image->view ) {
		qvkDestroyImageView( vk.device, image->view, NULL );
		image->view = VK_NULL_HANDLE;
	}

	// create image
	{
		VkImageCreateInfo desc;

		desc.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		desc.pNext = NULL;
		desc.flags = 0;
		desc.imageType = VK_IMAGE_TYPE_2D;
		desc.format = format;
		desc.extent.width = width;
		desc.extent.height = height;
		desc.extent.depth = 1;
		desc.mipLevels = mip_levels;
		desc.arrayLayers = 1;
		desc.samples = VK_SAMPLE_COUNT_1_BIT;
		desc.tiling = VK_IMAGE_TILING_OPTIMAL;
		desc.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		desc.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		desc.queueFamilyIndexCount = 0;
		desc.pQueueFamilyIndices = NULL;
		desc.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

		VK_CHECK( qvkCreateImage( vk.device, &desc, NULL, &image->handle ) );

		allocate_and_bind_image_memory( image->handle );
	}

	// create image view
	{
		VkImageViewCreateInfo desc;

		desc.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		desc.pNext = NULL;
		desc.flags = 0;
		desc.image = image->handle;
		desc.viewType = VK_IMAGE_VIEW_TYPE_2D;
		desc.format = format;
		desc.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
		desc.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
		desc.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
		desc.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
		desc.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		desc.subresourceRange.baseMipLevel = 0;
		desc.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
		desc.subresourceRange.baseArrayLayer = 0;
		desc.subresourceRange.layerCount = 1;

		VK_CHECK( qvkCreateImageView( vk.device, &desc, NULL, &image->view ) );
	}

	// create associated descriptor set
	if ( image->descriptor == VK_NULL_HANDLE ) {
		VkDescriptorSetAllocateInfo desc;

		desc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		desc.pNext = NULL;
		desc.descriptorPool = vk.descriptor_pool;
		desc.descriptorSetCount = 1;
		desc.pSetLayouts = &vk.set_layout_sampler;

		VK_CHECK( qvkAllocateDescriptorSets( vk.device, &desc, &image->descriptor ) );
	}

	vk_update_descriptor_set( image, mip_levels > 1 ? qtrue : qfalse );

	SET_OBJECT_NAME( image->handle, image->imgName, VK_DEBUG_REPORT_OBJECT_TYPE_IMAGE_EXT );
	SET_OBJECT_NAME( image->view, image->imgName, VK_DEBUG_REPORT_OBJECT_TYPE_IMAGE_VIEW_EXT );
	SET_OBJECT_NAME( image->descriptor, image->imgName, VK_DEBUG_REPORT_OBJECT_TYPE_DESCRIPTOR_SET_EXT );
}


static byte *resample_image_data( const int target_format, byte *data, const int data_size, int *bytes_per_pixel )
{
	byte* buffer;
	uint16_t* p;
	int i, n;

	switch ( target_format ) {
	case VK_FORMAT_B4G4R4A4_UNORM_PACK16:
		buffer = (byte*)ri.Hunk_AllocateTempMemory( data_size / 2 );
		p = (uint16_t*)buffer;
		for ( i = 0; i < data_size; i += 4, p++ ) {
			byte r = data[i + 0];
			byte g = data[i + 1];
			byte b = data[i + 2];
			byte a = data[i + 3];
			*p = (uint32_t)((a / 255.0) * 15.0 + 0.5) |
				((uint32_t)((r / 255.0) * 15.0 + 0.5) << 4) |
				((uint32_t)((g / 255.0) * 15.0 + 0.5) << 8) |
				((uint32_t)((b / 255.0) * 15.0 + 0.5) << 12);
		}
		*bytes_per_pixel = 2;
		return buffer; // must be freed after upload!

	case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
		buffer = (byte*)ri.Hunk_AllocateTempMemory( data_size / 2 );
		p = (uint16_t*)buffer;
		for ( i = 0; i < data_size; i += 4, p++ ) {
			byte r = data[i + 0];
			byte g = data[i + 1];
			byte b = data[i + 2];
			*p = (uint32_t)((b / 255.0) * 31.0 + 0.5) |
				((uint32_t)((g / 255.0) * 31.0 + 0.5) << 5) |
				((uint32_t)((r / 255.0) * 31.0 + 0.5) << 10) |
				(1 << 15);
		}
		*bytes_per_pixel = 2;
		return buffer; // must be freed after upload!

	case VK_FORMAT_B8G8R8A8_UNORM:
		buffer = (byte*)ri.Hunk_AllocateTempMemory( data_size );
		for ( i = 0; i < data_size; i += 4 ) {
			buffer[i + 0] = data[i + 2];
			buffer[i + 1] = data[i + 1];
			buffer[i + 2] = data[i + 0];
			buffer[i + 3] = data[i + 3];
		}
		*bytes_per_pixel = 4;
		return buffer;

	case VK_FORMAT_R8G8B8_UNORM: {
		buffer = (byte*)ri.Hunk_AllocateTempMemory( (data_size * 3) / 4 );
		for ( i = 0, n = 0; i < data_size; i += 4, n += 3 ) {
			buffer[n + 0] = data[i + 0];
			buffer[n + 1] = data[i + 1];
			buffer[n + 2] = data[i + 2];
		}
		*bytes_per_pixel = 3;
		return buffer;
	}

	default:
		*bytes_per_pixel = 4;
		return data;
	}
}


void vk_upload_image_data( image_t *image, int x, int y, int width, int height, int mipmaps, byte *pixels, int size, qboolean update ) {

	VkCommandBuffer   command_buffer;
	VkBufferImageCopy regions[16];
	VkBufferImageCopy region;
	byte *buf;
	int n;

	int num_regions = 0;
	int buffer_size = 0;

	buf = resample_image_data( image->internalFormat, pixels, size, &n /*bpp*/ );

	while (qtrue) {
		Com_Memset(&region, 0, sizeof(region));
		region.bufferOffset = buffer_size;
		region.bufferRowLength = 0;
		region.bufferImageHeight = 0;
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.mipLevel = num_regions;
		region.imageSubresource.baseArrayLayer = 0;
		region.imageSubresource.layerCount = 1;
		region.imageOffset.x = x;
		region.imageOffset.y = y;
		region.imageOffset.z = 0;
		region.imageExtent.width = width;
		region.imageExtent.height = height;
		region.imageExtent.depth = 1;

		regions[num_regions] = region;
		num_regions++;

		buffer_size += width * height * n;

		if ( num_regions >= mipmaps || (width == 1 && height == 1) || num_regions >= ARRAY_LEN( regions ) )
			break;

		x >>= 1;
		y >>= 1;

		width >>= 1;
		if (width < 1) width = 1;

		height >>= 1;
		if (height < 1) height = 1;
	}

#ifdef USE_UPLOAD_QUEUE
	if ( vk_wait_staging_buffer() ) {
		// wait for vkQueueSubmit() completion before new upload
	}

	if ( vk.staging_buffer.size - vk.staging_buffer.offset < buffer_size ) {
		// try to flush staging buffer and reset offset
		vk_flush_staging_buffer( qfalse );
	}

	if ( vk.staging_buffer.size /* - vk_world.staging_buffer_offset */ < buffer_size ) {
		// if still not enough - reallocate staging buffer
		vk_alloc_staging_buffer( buffer_size );
	}

	for ( n = 0; n < num_regions; n++ ) {
		regions[n].bufferOffset += vk.staging_buffer.offset;
	}

	Com_Memcpy( vk.staging_buffer.ptr + vk.staging_buffer.offset, buf, buffer_size );

	if ( vk.staging_buffer.offset == 0 ) {
		VkCommandBufferBeginInfo begin_info;
		begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		begin_info.pNext = NULL;
		begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		begin_info.pInheritanceInfo = NULL;
		VK_CHECK( qvkBeginCommandBuffer( vk.staging_command_buffer, &begin_info ) );
	}

	//ri.Printf( PRINT_WARNING, "batch @%6i + %i %s \n", (int)vk_world.staging_buffer_offset, (int)buffer_size, image->imgName );
	vk.staging_buffer.offset += buffer_size;

	command_buffer = vk.staging_command_buffer;

	if ( update ) {
		record_image_layout_transition( command_buffer, image->handle, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 0 );
	} else {
		record_image_layout_transition( command_buffer, image->handle, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_HOST_BIT, 0 );
	}

	qvkCmdCopyBufferToImage( command_buffer, vk.staging_buffer.handle, image->handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, num_regions, regions );

	// final transition after upload comleted
	record_image_layout_transition( command_buffer, image->handle, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 0 );
#else
	if ( vk.staging_buffer.size < buffer_size ) {
		vk_alloc_staging_buffer( buffer_size );
	}

	Com_Memcpy( vk.staging_buffer.ptr, buf, buffer_size );

	command_buffer = begin_command_buffer();
	// record_buffer_memory_barrier( command_buffer, vk_world.staging_buffer, VK_WHOLE_SIZE, 0, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT );
	if ( update ) {
		record_image_layout_transition( command_buffer, image->handle, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 0 );
	} else {
		record_image_layout_transition( command_buffer, image->handle, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_HOST_BIT, 0 );
	}
	qvkCmdCopyBufferToImage( command_buffer, vk.staging_buffer.handle, image->handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, num_regions, regions );
	record_image_layout_transition( command_buffer, image->handle, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 0 );
	end_command_buffer( command_buffer, __func__ );
#endif

	if ( buf != pixels ) {
		ri.Hunk_FreeTempMemory( buf );
	}
}


void vk_update_descriptor_set( image_t *image, qboolean mipmap ) {
	Vk_Sampler_Def sampler_def;
	VkDescriptorImageInfo image_info;
	VkWriteDescriptorSet descriptor_write;

	if ( image->descriptor == VK_NULL_HANDLE ) {
		ri.Error( ERR_DROP, "Texture descriptor is not allocated: %s", image->imgName );
		return;
	}
	Com_Memset( &sampler_def, 0, sizeof( sampler_def ) );

	sampler_def.address_mode = image->wrapClampMode;

	if ( mipmap ) {
		sampler_def.gl_mag_filter = gl_filter_max;
		sampler_def.gl_min_filter = gl_filter_min;
	} else {
		sampler_def.gl_mag_filter = GL_LINEAR;
		sampler_def.gl_min_filter = GL_LINEAR;
		// no anisotropy without mipmaps
		sampler_def.noAnisotropy = qtrue;
	}

	image_info.sampler = vk_find_sampler( &sampler_def );
	image_info.imageView = image->view;
	image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	descriptor_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	descriptor_write.dstSet = image->descriptor;
	descriptor_write.dstBinding = 0;
	descriptor_write.dstArrayElement = 0;
	descriptor_write.descriptorCount = 1;
	descriptor_write.pNext = NULL;
	descriptor_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	descriptor_write.pImageInfo = &image_info;
	descriptor_write.pBufferInfo = NULL;
	descriptor_write.pTexelBufferView = NULL;

	qvkUpdateDescriptorSets( vk.device, 1, &descriptor_write, 0, NULL );
}


void vk_destroy_image_resources( VkImage *image, VkImageView *imageView )
{
	if ( image != NULL ) {
		if ( *image != VK_NULL_HANDLE ) {
			qvkDestroyImage( vk.device, *image, NULL );
			*image = VK_NULL_HANDLE;
		}
	}
	if ( imageView != NULL ) {
		if ( *imageView != VK_NULL_HANDLE ) {
			qvkDestroyImageView( vk.device, *imageView, NULL );
			*imageView = VK_NULL_HANDLE;
		}
	}
}


static void set_shader_stage_desc(VkPipelineShaderStageCreateInfo *desc, VkShaderStageFlagBits stage, VkShaderModule shader_module, const char *entry) {
	desc->sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	desc->pNext = NULL;
	desc->flags = 0;
	desc->stage = stage;
	desc->module = shader_module;
	desc->pName = entry;
	desc->pSpecializationInfo = NULL;
}


#define FORMAT_DEPTH(format, r_bits, g_bits, b_bits) case(VK_FORMAT_##format): *r = r_bits; *b = b_bits; *g = g_bits; return qtrue;
static qboolean vk_surface_format_color_depth( VkFormat format, int *r, int *g, int *b ) {
	switch (format) {
		// Common formats from https://vulkan.gpuinfo.org/listsurfaceformats.php
		FORMAT_DEPTH(B8G8R8A8_UNORM, 255, 255, 255)
			FORMAT_DEPTH(B8G8R8A8_SRGB, 255, 255, 255)
			FORMAT_DEPTH(A2B10G10R10_UNORM_PACK32, 1023, 1023, 1023)
			FORMAT_DEPTH(R8G8B8A8_UNORM, 255, 255, 255)
			FORMAT_DEPTH(R8G8B8A8_SRGB, 255, 255, 255)
			FORMAT_DEPTH(A2R10G10B10_UNORM_PACK32, 1023, 1023, 1023)
			FORMAT_DEPTH(R5G6B5_UNORM_PACK16, 31, 63, 31)
			FORMAT_DEPTH(R8G8B8A8_SNORM, 255, 255, 255)
			FORMAT_DEPTH(A8B8G8R8_UNORM_PACK32, 255, 255, 255)
			FORMAT_DEPTH(A8B8G8R8_SNORM_PACK32, 255, 255, 255)
			FORMAT_DEPTH(A8B8G8R8_SRGB_PACK32, 255, 255, 255)
			FORMAT_DEPTH(R16G16B16A16_UNORM, 65535, 65535, 65535)
			FORMAT_DEPTH(R16G16B16A16_SNORM, 65535, 65535, 65535)
			FORMAT_DEPTH(R16G16B16A16_SFLOAT, 65535, 65535, 65535)
			FORMAT_DEPTH(B5G6R5_UNORM_PACK16, 31, 63, 31)
			FORMAT_DEPTH(B8G8R8A8_SNORM, 255, 255, 255)
			FORMAT_DEPTH(R4G4B4A4_UNORM_PACK16, 15, 15, 15)
			FORMAT_DEPTH(B4G4R4A4_UNORM_PACK16, 15, 15, 15)
			FORMAT_DEPTH(A1R5G5B5_UNORM_PACK16, 31, 31, 31)
			FORMAT_DEPTH(R5G5B5A1_UNORM_PACK16, 31, 31, 31)
			FORMAT_DEPTH(B5G5R5A1_UNORM_PACK16, 31, 31, 31)
	default:
		*r = 255; *g = 255; *b = 255; return qfalse;
	}
}

void vk_create_post_process_pipeline( vkPostProgram_t program_index, uint32_t width, uint32_t height ) {
	const VkDynamicState mirror_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo mirror_dynamic = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
	VkPipelineShaderStageCreateInfo shader_stages[2];
	VkPipelineVertexInputStateCreateInfo vertex_input_state;
	VkPipelineInputAssemblyStateCreateInfo input_assembly_state;
	VkPipelineRasterizationStateCreateInfo rasterization_state;
	VkPipelineDepthStencilStateCreateInfo depth_stencil_state;
	VkPipelineViewportStateCreateInfo viewport_state;
	VkPipelineMultisampleStateCreateInfo multisample_state;
	VkPipelineColorBlendStateCreateInfo blend_state;
	VkPipelineColorBlendAttachmentState attachment_blend_state;
	VkGraphicsPipelineCreateInfo create_info;
	VkViewport viewport;
	VkRect2D scissor;
	VkSpecializationMapEntry spec_entries[16];
	VkSpecializationInfo frag_spec_info;
	VkPipeline *pipeline;
	VkShaderModule fsmodule;
	VkRenderPass renderpass;
	VkPipelineLayout layout;
	VkSampleCountFlagBits samples;
	const char *pipeline_name;

	struct FragSpecData {
		float gamma;
		float overbright;
		float greyscale;
		float bloom_threshold;
		float bloom_intensity;
		int bloom_threshold_mode;
		int bloom_modulate;
		int dither;
		int depth_r;
		int depth_g;
		int depth_b;
		int hdr_mode;
		int linear_sdr_output;
		int screen_source;
		int source_layer;
		int bloom_enabled;
	} frag_spec_data;

	switch ( program_index ) {
	case VK_POST_SCREEN_LEFT:
	case VK_POST_SCREEN_RIGHT:
	case VK_POST_EYE_MIRROR_LEFT:
	case VK_POST_EYE_MIRROR_RIGHT:
		pipeline = program_index < VK_POST_EYE_MIRROR_LEFT
					   ? &vk.xr_output.screen_mirror_eye[program_index - VK_POST_SCREEN_LEFT]
					   : &vk.xr_output.eye_mirror[program_index - VK_POST_EYE_MIRROR_LEFT];
		fsmodule = vk.modules.gamma_fs_array;
		renderpass = vk.render_pass.gamma;
		layout = vk.pipeline_layout_post_process;
		samples = VK_SAMPLE_COUNT_1_BIT;
		pipeline_name = "eye layer desktop presentation";
		break;
	case VK_POST_CAPTURE: // flatscreen screenshots: scene, bloom and gamma into the capture image
		pipeline = &vk.capture_pipeline;
		fsmodule = vk.modules.gamma_composite_fs;
		renderpass = vk.render_pass.capture;
		layout = vk.pipeline_layout_composite;
		samples = VK_SAMPLE_COUNT_1_BIT;
		pipeline_name = "capture buffer pipeline";
		break;
	case VK_POST_XR_COMPOSITE: // scene, bloom and headset SDR output in one draw
		pipeline = &vk.xr_output.composite_pipeline;
		fsmodule = vk.modules.gamma_composite_fs_mv;
		renderpass = vk.xr_output.pass;
		layout = vk.pipeline_layout_composite;
		samples = VK_SAMPLE_COUNT_1_BIT;
		pipeline_name = "XR bloom composite pipeline";
		break;
	case VK_POST_DESKTOP_COMPOSITE: // flatscreen scene, bloom and gamma in one draw
		pipeline = &vk.gamma_composite_pipeline;
		fsmodule = vk.modules.gamma_composite_fs;
		renderpass = vk.render_pass.gamma;
		layout = vk.pipeline_layout_composite;
		samples = VK_SAMPLE_COUNT_1_BIT;
		pipeline_name = "desktop bloom composite pipeline";
		break;
	case VK_POST_SCREEN_MONO:
	case VK_POST_SCREEN_CAPTURE: // Already-processed virtual-screen content.
		pipeline = program_index == VK_POST_SCREEN_MONO ? &vk.xr_output.screen_mono_pipeline
														: &vk.xr_output.screen_capture_pipeline;
		fsmodule = vk.modules.gamma_fs;
		renderpass = program_index == VK_POST_SCREEN_CAPTURE ? vk.render_pass.capture : vk.render_pass.gamma;
		layout = vk.pipeline_layout_post_process;
		samples = VK_SAMPLE_COUNT_1_BIT;
		pipeline_name = "virtual screen presentation";
		break;
	default:
		ri.Error( ERR_FATAL, "%s: unknown post-process program %d", __func__, program_index );
		return;
	}

	// the eye pass composite already has its multiview module; the other multiview outputs sample an array image
	if ( vk.multiview && program_index != VK_POST_SCREEN_MONO && program_index != VK_POST_XR_COMPOSITE )
		fsmodule = vk.modules.gamma_fs_array;

	if ( *pipeline != VK_NULL_HANDLE ) {
		vk_wait_idle();
		qvkDestroyPipeline( vk.device, *pipeline, NULL );
		*pipeline = VK_NULL_HANDLE;
	}

	vertex_input_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertex_input_state.pNext = NULL;
	vertex_input_state.flags = 0;
	vertex_input_state.vertexBindingDescriptionCount = 0;
	vertex_input_state.pVertexBindingDescriptions = NULL;
	vertex_input_state.vertexAttributeDescriptionCount = 0;
	vertex_input_state.pVertexBindingDescriptions = NULL;

	// shaders
	set_shader_stage_desc( shader_stages+0, VK_SHADER_STAGE_VERTEX_BIT, vk.modules.gamma_vs, "main" );
	set_shader_stage_desc( shader_stages+1, VK_SHADER_STAGE_FRAGMENT_BIT, fsmodule, "main" );

	frag_spec_data.gamma = 1.0 / (r_gamma->value);
	frag_spec_data.overbright = (float)(1 << tr.overbrightBits);
	frag_spec_data.greyscale = r_greyscale->value;
	frag_spec_data.bloom_threshold = r_bloom_threshold->value;
	frag_spec_data.bloom_intensity = r_bloom_intensity->value;
	frag_spec_data.bloom_threshold_mode = r_bloom_threshold_mode->integer;
	frag_spec_data.bloom_modulate = r_bloom_modulate->integer;
	frag_spec_data.dither = r_dither->integer;
	// Screenshot/video capture and headset output remain SDR.
	frag_spec_data.hdr_mode =
		(vk.hdrActive && program_index != VK_POST_CAPTURE && program_index != VK_POST_XR_COMPOSITE &&
		 program_index != VK_POST_SCREEN_CAPTURE)
			? ( program_index >= VK_POST_EYE_MIRROR_LEFT ? 3 : 1 )
			: 0;
	// the eye pass writes through a UNORM view, so its output stays encoded
	frag_spec_data.linear_sdr_output = 0;
	frag_spec_data.bloom_enabled = r_bloom->integer ? 1 : 0;
	frag_spec_data.screen_source =
		program_index == VK_POST_SCREEN_MONO ? 2
		: ( program_index == VK_POST_SCREEN_CAPTURE || program_index >= VK_POST_SCREEN_LEFT )
			? 1
			: 0;
	// the HDR eye mirror reconstructs from the encoded eye image instead of presenting it as is
	if ( frag_spec_data.hdr_mode == 3 )
		frag_spec_data.screen_source = 0;
	frag_spec_data.source_layer = program_index == VK_POST_SCREEN_RIGHT || program_index == VK_POST_EYE_MIRROR_RIGHT ? 1 : 0;

	if ( !vk_surface_format_color_depth( vk.present_format.format, &frag_spec_data.depth_r, &frag_spec_data.depth_g, &frag_spec_data.depth_b ) )
		ri.Printf( PRINT_ALL, "Format %s not recognized, dither to assume 8bpc\n", vk_format_string( vk.base_format.format ) );
	if ( program_index == VK_POST_XR_COMPOSITE ) {
		frag_spec_data.depth_r = frag_spec_data.depth_g = frag_spec_data.depth_b = 255;
	}

	spec_entries[0].constantID = 0;
	spec_entries[0].offset = offsetof( struct FragSpecData, gamma );
	spec_entries[0].size = sizeof( frag_spec_data.gamma );

	spec_entries[1].constantID = 1;
	spec_entries[1].offset = offsetof( struct FragSpecData, overbright );
	spec_entries[1].size = sizeof( frag_spec_data.overbright );

	spec_entries[2].constantID = 2;
	spec_entries[2].offset = offsetof( struct FragSpecData, greyscale );
	spec_entries[2].size = sizeof( frag_spec_data.greyscale );

	spec_entries[3].constantID = 3;
	spec_entries[3].offset = offsetof( struct FragSpecData, bloom_threshold );
	spec_entries[3].size = sizeof( frag_spec_data.bloom_threshold );

	spec_entries[4].constantID = 4;
	spec_entries[4].offset = offsetof( struct FragSpecData, bloom_intensity );
	spec_entries[4].size = sizeof( frag_spec_data.bloom_intensity );

	spec_entries[5].constantID = 5;
	spec_entries[5].offset = offsetof( struct FragSpecData, bloom_threshold_mode );
	spec_entries[5].size = sizeof( frag_spec_data.bloom_threshold_mode );

	spec_entries[6].constantID = 6;
	spec_entries[6].offset = offsetof( struct FragSpecData, bloom_modulate );
	spec_entries[6].size = sizeof( frag_spec_data.bloom_modulate );

	spec_entries[7].constantID = 7;
	spec_entries[7].offset = offsetof( struct FragSpecData, dither );
	spec_entries[7].size = sizeof( frag_spec_data.dither );

	spec_entries[8].constantID = 8;
	spec_entries[8].offset = offsetof( struct FragSpecData, depth_r );
	spec_entries[8].size = sizeof( frag_spec_data.depth_r );

	spec_entries[9].constantID = 9;
	spec_entries[9].offset = offsetof(struct FragSpecData, depth_g);
	spec_entries[9].size = sizeof(frag_spec_data.depth_g);

	spec_entries[10].constantID = 10;
	spec_entries[10].offset = offsetof(struct FragSpecData, depth_b);
	spec_entries[10].size = sizeof(frag_spec_data.depth_b);

	spec_entries[11].constantID = 11;
	spec_entries[11].offset = offsetof( struct FragSpecData, hdr_mode );
	spec_entries[11].size = sizeof( frag_spec_data.hdr_mode );

	spec_entries[12].constantID = 12;
	spec_entries[12].offset = offsetof( struct FragSpecData, linear_sdr_output );
	spec_entries[12].size = sizeof( frag_spec_data.linear_sdr_output );
	spec_entries[13].constantID = 13;
	spec_entries[13].offset = offsetof( struct FragSpecData, screen_source );
	spec_entries[13].size = sizeof( frag_spec_data.screen_source );
	spec_entries[14].constantID = 14;
	spec_entries[14].offset = offsetof( struct FragSpecData, source_layer );
	spec_entries[14].size = sizeof( int );
	spec_entries[15].constantID = 15;
	spec_entries[15].offset = offsetof( struct FragSpecData, bloom_enabled );
	spec_entries[15].size = sizeof( int );
	frag_spec_info.mapEntryCount = 16;
	frag_spec_info.pMapEntries = spec_entries;
	frag_spec_info.dataSize = sizeof( frag_spec_data );
	frag_spec_info.pData = &frag_spec_data;

	shader_stages[1].pSpecializationInfo = &frag_spec_info;

	//
	// Primitive assembly.
	//
	input_assembly_state.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	input_assembly_state.pNext = NULL;
	input_assembly_state.flags = 0;
	input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
	input_assembly_state.primitiveRestartEnable = VK_FALSE;

	//
	// Viewport.
	//
	if ( program_index == VK_POST_DESKTOP_COMPOSITE || program_index == VK_POST_SCREEN_MONO ||
		program_index >= VK_POST_SCREEN_LEFT ) {
		// window output
		viewport.x = 0.0 + vk.blitX0;
		viewport.y = 0.0 + vk.blitY0;
		viewport.width = gls.windowWidth - vk.blitX0 * 2;
		viewport.height = gls.windowHeight - vk.blitY0 * 2;
	} else {
		// other post-processing
		viewport.x = 0.0;
		viewport.y = 0.0;
		viewport.width = width;
		viewport.height = height;
	}

	viewport.minDepth = 0.0;
	viewport.maxDepth = 1.0;

	scissor.offset.x = viewport.x;
	scissor.offset.y = viewport.y;
	scissor.extent.width = viewport.width;
	scissor.extent.height = viewport.height;

	viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewport_state.pNext = NULL;
	viewport_state.flags = 0;
	viewport_state.viewportCount = 1;
	viewport_state.pViewports = &viewport;
	viewport_state.scissorCount = 1;
	viewport_state.pScissors = &scissor;

	//
	// Rasterization.
	//
	rasterization_state.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterization_state.pNext = NULL;
	rasterization_state.flags = 0;
	rasterization_state.depthClampEnable = VK_FALSE;
	rasterization_state.rasterizerDiscardEnable = VK_FALSE;
	rasterization_state.polygonMode = VK_POLYGON_MODE_FILL;
	//rasterization_state.cullMode = VK_CULL_MODE_BACK_BIT; // VK_CULL_MODE_NONE;
	rasterization_state.cullMode = VK_CULL_MODE_NONE;
	rasterization_state.frontFace = VK_FRONT_FACE_CLOCKWISE; // Q3 defaults to clockwise vertex order
	rasterization_state.depthBiasEnable = VK_FALSE;
	rasterization_state.depthBiasConstantFactor = 0.0f;
	rasterization_state.depthBiasClamp = 0.0f;
	rasterization_state.depthBiasSlopeFactor = 0.0f;
	rasterization_state.lineWidth = 1.0f;

	multisample_state.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample_state.pNext = NULL;
	multisample_state.flags = 0;
	multisample_state.rasterizationSamples = samples;
	multisample_state.sampleShadingEnable = VK_FALSE;
	multisample_state.minSampleShading = 1.0f;
	multisample_state.pSampleMask = NULL;
	multisample_state.alphaToCoverageEnable = VK_FALSE;
	multisample_state.alphaToOneEnable = VK_FALSE;

	Com_Memset(&attachment_blend_state, 0, sizeof(attachment_blend_state));
	attachment_blend_state.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	attachment_blend_state.blendEnable = VK_FALSE;

	blend_state.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	blend_state.pNext = NULL;
	blend_state.flags = 0;
	blend_state.logicOpEnable = VK_FALSE;
	blend_state.logicOp = VK_LOGIC_OP_COPY;

	blend_state.attachmentCount = 1;
	blend_state.pAttachments = &attachment_blend_state;
	blend_state.blendConstants[0] = 0.0f;
	blend_state.blendConstants[1] = 0.0f;
	blend_state.blendConstants[2] = 0.0f;
	blend_state.blendConstants[3] = 0.0f;

	Com_Memset( &depth_stencil_state, 0, sizeof( depth_stencil_state ) );

	depth_stencil_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depth_stencil_state.pNext = NULL;
	depth_stencil_state.flags = 0;
	depth_stencil_state.depthTestEnable = VK_FALSE;
	depth_stencil_state.depthWriteEnable = VK_FALSE;
	depth_stencil_state.depthCompareOp = VK_COMPARE_OP_NEVER;
	depth_stencil_state.depthBoundsTestEnable = VK_FALSE;
	depth_stencil_state.stencilTestEnable = VK_FALSE;
	depth_stencil_state.minDepthBounds = 0.0f;
	depth_stencil_state.maxDepthBounds = 1.0f;

	create_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	create_info.pNext = NULL;
	create_info.flags = 0;
	create_info.stageCount = 2;
	create_info.pStages = shader_stages;
	create_info.pVertexInputState = &vertex_input_state;
	create_info.pInputAssemblyState = &input_assembly_state;
	create_info.pTessellationState = NULL;
	create_info.pViewportState = &viewport_state;
	create_info.pRasterizationState = &rasterization_state;
	create_info.pMultisampleState = &multisample_state;
	create_info.pDepthStencilState = &depth_stencil_state;
	create_info.pColorBlendState = &blend_state;
	mirror_dynamic.dynamicStateCount = ARRAY_LEN( mirror_states );
	mirror_dynamic.pDynamicStates = mirror_states;
	create_info.pDynamicState = (program_index == VK_POST_DESKTOP_COMPOSITE || program_index == VK_POST_SCREEN_MONO ||
								 program_index >= VK_POST_SCREEN_LEFT)
									? &mirror_dynamic
									: NULL;
	create_info.layout = layout;
	create_info.renderPass = renderpass;
	create_info.subpass = 0;
	create_info.basePipelineHandle = VK_NULL_HANDLE;
	create_info.basePipelineIndex = -1;

	VK_CHECK( qvkCreateGraphicsPipelines( vk.device, VK_NULL_HANDLE, 1, &create_info, NULL, pipeline ) );

	if ( program_index == VK_POST_XR_COMPOSITE )
		vk_create_mono_pipeline( &create_info, vk.mono.pass.output, &vk.mono.pipeline.composite );

	SET_OBJECT_NAME( *pipeline, pipeline_name, VK_DEBUG_REPORT_OBJECT_TYPE_PIPELINE_EXT );
}

void vk_create_blur_pipeline( uint32_t index, uint32_t width, uint32_t height, qboolean horizontal_pass )
{
	VkPipelineShaderStageCreateInfo shader_stages[2];
	VkPipelineVertexInputStateCreateInfo vertex_input_state;
	VkPipelineInputAssemblyStateCreateInfo input_assembly_state;
	VkPipelineRasterizationStateCreateInfo rasterization_state;
	VkPipelineViewportStateCreateInfo viewport_state;
	VkPipelineMultisampleStateCreateInfo multisample_state;
	VkPipelineColorBlendStateCreateInfo blend_state;
	VkPipelineColorBlendAttachmentState attachment_blend_state;
	VkGraphicsPipelineCreateInfo create_info;
	VkViewport viewport;
	VkRect2D scissor;
	struct BlurSpec {
		float offset[3]; // x-offset, y-offset, correction
		float threshold;
		int mode, modulate;
	} spec;
	static const struct { uint32_t id; size_t offset, size; } fields[6] = {
		{ 0, offsetof( struct BlurSpec, offset[0] ), sizeof( float ) },
		{ 1, offsetof( struct BlurSpec, offset[1] ), sizeof( float ) },
		{ 2, offsetof( struct BlurSpec, offset[2] ), sizeof( float ) },
		{ 3, offsetof( struct BlurSpec, threshold ), sizeof( float ) },
		{ 5, offsetof( struct BlurSpec, mode ), sizeof( int ) },
		{ 6, offsetof( struct BlurSpec, modulate ), sizeof( int ) },
	};
	VkSpecializationMapEntry spec_entries[ARRAY_LEN( fields )];
	VkSpecializationInfo frag_spec_info;
	VkShaderModule fsmodule;
	VkPipeline *pipeline;
	uint32_t n;

	pipeline = &vk.blur_pipeline[ index ];

	if ( *pipeline != VK_NULL_HANDLE ) {
		vk_wait_idle();
		qvkDestroyPipeline( vk.device, *pipeline, NULL );
		*pipeline = VK_NULL_HANDLE;
	}

	vertex_input_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertex_input_state.pNext = NULL;
	vertex_input_state.flags = 0;
	vertex_input_state.vertexBindingDescriptionCount = 0;
	vertex_input_state.pVertexBindingDescriptions = NULL;
	vertex_input_state.vertexAttributeDescriptionCount = 0;
	vertex_input_state.pVertexBindingDescriptions = NULL;

	// shaders
	set_shader_stage_desc( shader_stages+0, VK_SHADER_STAGE_VERTEX_BIT, vk.modules.gamma_vs, "main" );
	// the first pass samples the scene and applies the bright pass to each tap
	if ( index == 0 )
		fsmodule = vk.multiview ? vk.modules.blur_extract_fs_mv : vk.modules.blur_extract_fs;
	else
		fsmodule = vk.multiview ? vk.modules.blur_fs_mv : vk.modules.blur_fs;
	set_shader_stage_desc( shader_stages + 1, VK_SHADER_STAGE_FRAGMENT_BIT, fsmodule, "main" );

	// blur.frag offsets the coordinate it samples the source with, so the
	// offset is in source texels. Each horizontal pass reads the previous
	// octave at twice its own width, the verticals read their own resolution
	spec.offset[0] = 1.2 / (float) ( width * 2 ); // x offset
	spec.offset[1] = 1.2 / (float) height; // y offset
	spec.offset[2] = 1.0; // intensity?

	if ( horizontal_pass ) {
		spec.offset[1] = 0.0;
	} else {
		spec.offset[0] = 0.0;
	}

	spec.threshold = r_bloom_threshold->value;
	spec.mode = r_bloom_threshold_mode->integer;
	spec.modulate = r_bloom_modulate->integer;

	for ( n = 0; n < ARRAY_LEN( fields ); n++ ) {
		spec_entries[n].constantID = fields[n].id;
		spec_entries[n].offset = (uint32_t)fields[n].offset;
		spec_entries[n].size = fields[n].size;
	}

	frag_spec_info.mapEntryCount = index == 0 ? ARRAY_LEN( fields ) : 3;
	frag_spec_info.pMapEntries = spec_entries;
	frag_spec_info.dataSize = sizeof( spec );
	frag_spec_info.pData = &spec;

	shader_stages[1].pSpecializationInfo = &frag_spec_info;

	//
	// Primitive assembly.
	//
	input_assembly_state.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	input_assembly_state.pNext = NULL;
	input_assembly_state.flags = 0;
	input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
	input_assembly_state.primitiveRestartEnable = VK_FALSE;

	//
	// Viewport.
	//
	viewport.x = 0.0;
	viewport.y = 0.0;
	viewport.width = width;
	viewport.height = height;
	viewport.minDepth = 0.0;
	viewport.maxDepth = 1.0;

	scissor.offset.x = viewport.x;
	scissor.offset.y = viewport.y;
	scissor.extent.width = viewport.width;
	scissor.extent.height = viewport.height;

	viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewport_state.pNext = NULL;
	viewport_state.flags = 0;
	viewport_state.viewportCount = 1;
	viewport_state.pViewports = &viewport;
	viewport_state.scissorCount = 1;
	viewport_state.pScissors = &scissor;

	//
	// Rasterization.
	//
	rasterization_state.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterization_state.pNext = NULL;
	rasterization_state.flags = 0;
	rasterization_state.depthClampEnable = VK_FALSE;
	rasterization_state.rasterizerDiscardEnable = VK_FALSE;
	rasterization_state.polygonMode = VK_POLYGON_MODE_FILL;
	//rasterization_state.cullMode = VK_CULL_MODE_BACK_BIT; // VK_CULL_MODE_NONE;
	rasterization_state.cullMode = VK_CULL_MODE_NONE;
	rasterization_state.frontFace = VK_FRONT_FACE_CLOCKWISE; // Q3 defaults to clockwise vertex order
	rasterization_state.depthBiasEnable = VK_FALSE;
	rasterization_state.depthBiasConstantFactor = 0.0f;
	rasterization_state.depthBiasClamp = 0.0f;
	rasterization_state.depthBiasSlopeFactor = 0.0f;
	rasterization_state.lineWidth = 1.0f;

	multisample_state.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample_state.pNext = NULL;
	multisample_state.flags = 0;
	multisample_state.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	multisample_state.sampleShadingEnable = VK_FALSE;
	multisample_state.minSampleShading = 1.0f;
	multisample_state.pSampleMask = NULL;
	multisample_state.alphaToCoverageEnable = VK_FALSE;
	multisample_state.alphaToOneEnable = VK_FALSE;

	Com_Memset(&attachment_blend_state, 0, sizeof(attachment_blend_state));
	attachment_blend_state.blendEnable = VK_FALSE;
	attachment_blend_state.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

	blend_state.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	blend_state.pNext = NULL;
	blend_state.flags = 0;
	blend_state.logicOpEnable = VK_FALSE;
	blend_state.logicOp = VK_LOGIC_OP_COPY;
	blend_state.attachmentCount = 1;
	blend_state.pAttachments = &attachment_blend_state;
	blend_state.blendConstants[0] = 0.0f;
	blend_state.blendConstants[1] = 0.0f;
	blend_state.blendConstants[2] = 0.0f;
	blend_state.blendConstants[3] = 0.0f;

	create_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	create_info.pNext = NULL;
	create_info.flags = 0;
	create_info.stageCount = 2;
	create_info.pStages = shader_stages;
	create_info.pVertexInputState = &vertex_input_state;
	create_info.pInputAssemblyState = &input_assembly_state;
	create_info.pTessellationState = NULL;
	create_info.pViewportState = &viewport_state;
	create_info.pRasterizationState = &rasterization_state;
	create_info.pMultisampleState = &multisample_state;
	create_info.pDepthStencilState = NULL;
	create_info.pColorBlendState = &blend_state;
	create_info.pDynamicState = NULL;
	create_info.layout = vk.pipeline_layout_post_process; // one input attachment
	create_info.renderPass = vk.render_pass.blur[ index ];
	create_info.subpass = 0;
	create_info.basePipelineHandle = VK_NULL_HANDLE;
	create_info.basePipelineIndex = -1;

	VK_CHECK( qvkCreateGraphicsPipelines( vk.device, VK_NULL_HANDLE, 1, &create_info, NULL, pipeline ) );
	vk_create_mono_pipeline( &create_info, vk.mono.pass.blur[index], &vk.mono.pipeline.blur[index] );

	SET_OBJECT_NAME( *pipeline, va( "%s blur pipeline %i", horizontal_pass ? "horizontal" : "vertical", index/2 + 1 ), VK_DEBUG_REPORT_OBJECT_TYPE_PIPELINE_EXT );
}


static VkVertexInputBindingDescription bindings[8];
static VkVertexInputAttributeDescription attribs[8];
static uint32_t num_binds;
static uint32_t num_attrs;

static void push_bind( uint32_t binding, uint32_t stride )
{
	bindings[ num_binds ].binding = binding;
	bindings[ num_binds ].stride = stride;
	bindings[ num_binds ].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
	num_binds++;
}

static void push_attr( uint32_t location, uint32_t binding, VkFormat format )
{
	attribs[ num_attrs ].location = location;
	attribs[ num_attrs ].binding = binding;
	attribs[ num_attrs ].format = format;
	attribs[ num_attrs ].offset = 0;
	num_attrs++;
}


static qboolean vk_blend_reads_destination( VkBlendFactor factor ) {
	return factor == VK_BLEND_FACTOR_DST_COLOR || factor == VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR ||
		   factor == VK_BLEND_FACTOR_DST_ALPHA || factor == VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA ||
		   factor == VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
}

/* Post-scene draws blend into the scene image as they always did; their alpha instead tracks what they let through
 * (whatever multiplies the destination), so the composite can add bloom underneath them. Exact for scalar
 * multipliers, through the source's luminance for per-channel ones. Returns the shader's alpha mode. */
static int vk_transmittance_blend( VkPipelineColorBlendAttachmentState *t ) {
	const VkBlendFactor src = t->srcColorBlendFactor, dst = t->dstColorBlendFactor;
	const qboolean srcAlphaColor = src == VK_BLEND_FACTOR_SRC_ALPHA || src == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA ||
								   dst == VK_BLEND_FACTOR_SRC_ALPHA || dst == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	int mode = 0;
	t->srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
	t->alphaBlendOp = VK_BLEND_OP_ADD;
	if ( t->colorWriteMask )
		t->colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	if ( !t->blendEnable ) {
		// opaque: none of the scene shows through
		t->blendEnable = VK_TRUE;
		t->srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
		t->dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
		t->colorBlendOp = VK_BLEND_OP_ADD;
		t->dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
		return 0;
	}
	if ( src == VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR && dst == VK_BLEND_FACTOR_ONE ) {
		// screen: the scene keeps one minus the source
		t->dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		return 1;
	}
	if ( src == VK_BLEND_FACTOR_DST_COLOR && dst == VK_BLEND_FACTOR_ZERO ) {
		// filter: the scene is multiplied by the source
		t->dstAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		return 1;
	}
	switch ( dst ) {
		case VK_BLEND_FACTOR_ZERO:
		case VK_BLEND_FACTOR_ONE:
		case VK_BLEND_FACTOR_SRC_ALPHA:
		case VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA:
			t->dstAlphaBlendFactor = dst;
			break;
		case VK_BLEND_FACTOR_SRC_COLOR:
		case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR:
			if ( srcAlphaColor ) {
				// the color blend needs the real alpha, so the scene shows through unscaled
				ri.Printf( PRINT_DEVELOPER, "post-scene blend %i/%i scales the scene per channel; bloom keeps it\n", src, dst );
				t->dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
			} else {
				t->dstAlphaBlendFactor = dst == VK_BLEND_FACTOR_SRC_COLOR ? VK_BLEND_FACTOR_SRC_ALPHA
																		  : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
				mode = 1;
			}
			break;
		default:
			t->dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
			break;
	}
	if ( src == VK_BLEND_FACTOR_DST_COLOR ) {
		// the destination is scaled by the source plus that factor (the skin shine); over the opaque stage below it
		// the transmittance is already zero, so keeping it is exact there
		t->dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		mode = 0;
	} else if ( vk_blend_reads_destination( src ) || vk_blend_reads_destination( dst ) ) {
		ri.Printf( PRINT_DEVELOPER, "post-scene blend %i/%i reads the scene; bloom keeps it\n", src, dst );
		t->dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		mode = 0;
	}
	return mode;
}

VkPipeline create_pipeline( const Vk_Pipeline_Def *def, renderPass_t renderPassIndex, uint32_t def_index ) {
	const qboolean multiview = VK_PassViewMask( vk.multiview, renderPassIndex ) != 0;
	vkVertexModules_t *vertex = multiview ? &vk.modules.vert_mv : &vk.modules.vert;
	VkShaderModule *vs_module = NULL;
	VkShaderModule *fs_module = NULL;
	//int32_t vert_spec_data[1]; // clippping
	floatint_t frag_spec_data[14]; // 0:alpha-test-func, 1:alpha-test-value, 2:depth-fragment,
								   // 3:alpha-to-coverage, 4:color_mode, 5:abs_light, 6:multitexture mode,
								   // 7:discard mode, 8: ident.color, 9 - ident.alpha, 10 - acff,
								   // 11..12: hud/desktop, 13: post-scene transmittance output
	VkSpecializationMapEntry spec_entries[15];
	//VkSpecializationInfo vert_spec_info;
	VkSpecializationInfo frag_spec_info;
	VkPipelineVertexInputStateCreateInfo vertex_input_state;
	VkPipelineInputAssemblyStateCreateInfo input_assembly_state;
	VkPipelineRasterizationStateCreateInfo rasterization_state;
	VkPipelineViewportStateCreateInfo viewport_state;
	VkPipelineMultisampleStateCreateInfo multisample_state;
	VkPipelineDepthStencilStateCreateInfo depth_stencil_state;
	VkPipelineColorBlendStateCreateInfo blend_state;
	VkPipelineColorBlendAttachmentState attachment_blend_state;
	VkPipelineColorBlendAttachmentState blend_attachments[2];
	VkPipelineDynamicStateCreateInfo dynamic_state;
	VkDynamicState dynamic_state_array[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
											VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR};
	VkGraphicsPipelineCreateInfo create_info;
	VkPipeline pipeline;
	VkPipelineShaderStageCreateInfo shader_stages[2];
	VkBool32 alphaToCoverage = VK_FALSE;
	unsigned int atest_bits;
	unsigned int state_bits = def->state_bits;

	// the emissive attachment only exists in these passes; fragment-module
	// choice below must agree with the blend-state attachmentCount decision
	// or the pipeline gets an unconsumed location-1 output
	const qboolean emissiveActive =
		(renderPassIndex == RENDER_PASS_MAIN || renderPassIndex == RENDER_PASS_MONO_MAIN ||
		 VK_PassIsPostScene( renderPassIndex )) &&
		vk.hdrActive;
	const int fs_em = emissiveActive ? 0 : 1;

	switch ( def->shader_type ) {

		case TYPE_SINGLE_TEXTURE_LIGHTING:
			vs_module = &vertex->light[0];
			fs_module = &vk.modules.frag.light[0][fs_em][0];
			break;

		case TYPE_SINGLE_TEXTURE_LIGHTING_LINEAR:
			vs_module = &vertex->light[0];
			fs_module = &vk.modules.frag.light[1][fs_em][0];
			break;

		case TYPE_SINGLE_TEXTURE_LIGHTING_OVERBRIGHT:
			vs_module = &vertex->overbright_vert[0];
			fs_module = &vk.modules.frag.overbright_frag[fs_em][0];
			break;

		case TYPE_SINGLE_TEXTURE_DF:
			state_bits |= GLS_DEPTHMASK_TRUE;
			vs_module = &vertex->ident1[0][0][0];
			fs_module = &vk.modules.frag.gen0_df;
			break;

		case TYPE_SINGLE_TEXTURE_FIXED_COLOR:
			vs_module = &vertex->fixed[0][0][0];
			fs_module = &vk.modules.frag.fixed[0][fs_em][0];
			break;

		case TYPE_SINGLE_TEXTURE_FIXED_COLOR_ENV:
			vs_module = &vertex->fixed[0][1][0];
			fs_module = &vk.modules.frag.fixed[0][fs_em][0];
			break;

		case TYPE_SINGLE_TEXTURE_ENT_COLOR:
			vs_module = &vertex->fixed[0][0][0];
			fs_module = &vk.modules.frag.ent[0][fs_em][0];
			break;

		case TYPE_SINGLE_TEXTURE_ENT_COLOR_ENV:
			vs_module = &vertex->fixed[0][1][0];
			fs_module = &vk.modules.frag.ent[0][fs_em][0];
			break;

		case TYPE_VR_SCREEN:
		case TYPE_VR_REFLECTION:
		case TYPE_VR_FLOOR_GRID:
			vs_module = &vertex->ident1[0][0][0];
			if ( def->shader_type == TYPE_VR_SCREEN )
				fs_module = &vk.modules.virtualscreen_fs;
			else if ( def->shader_type == TYPE_VR_REFLECTION )
				fs_module = &vk.modules.virtualreflect_fs;
			else
				fs_module = &vk.modules.floor_grid_fs;
			break;

		case TYPE_SINGLE_TEXTURE:
			vs_module = &vertex->gen[0][0][0][0];
			fs_module = &vk.modules.frag.gen[0][0][fs_em][0];
			break;

		case TYPE_SINGLE_TEXTURE_ENV:
			vs_module = &vertex->gen[0][0][1][0];
			fs_module = &vk.modules.frag.gen[0][0][fs_em][0];
			break;

		case TYPE_SINGLE_TEXTURE_IDENTITY:
			vs_module = &vertex->ident1[0][0][0];
			fs_module = &vk.modules.frag.ident1[0][fs_em][0];
			break;

		case TYPE_SINGLE_TEXTURE_IDENTITY_ENV:
			vs_module = &vertex->ident1[0][1][0];
			fs_module = &vk.modules.frag.ident1[0][fs_em][0];
			break;

		case TYPE_MULTI_TEXTURE_ADD2_IDENTITY:
		case TYPE_MULTI_TEXTURE_MUL2_IDENTITY:
			vs_module = &vertex->ident1[1][0][0];
			fs_module = &vk.modules.frag.ident1[1][fs_em][0];
			break;

		case TYPE_MULTI_TEXTURE_ADD2_IDENTITY_ENV:
		case TYPE_MULTI_TEXTURE_MUL2_IDENTITY_ENV:
			vs_module = &vertex->ident1[1][1][0];
			fs_module = &vk.modules.frag.ident1[1][fs_em][0];
			break;

		case TYPE_MULTI_TEXTURE_ADD2_FIXED_COLOR:
		case TYPE_MULTI_TEXTURE_MUL2_FIXED_COLOR:
			vs_module = &vertex->fixed[1][0][0];
			fs_module = &vk.modules.frag.fixed[1][fs_em][0];
			break;

		case TYPE_MULTI_TEXTURE_ADD2_FIXED_COLOR_ENV:
		case TYPE_MULTI_TEXTURE_MUL2_FIXED_COLOR_ENV:
			vs_module = &vertex->fixed[1][1][0];
			fs_module = &vk.modules.frag.fixed[1][fs_em][0];
			break;

		case TYPE_MULTI_TEXTURE_MUL2:
		case TYPE_MULTI_TEXTURE_ADD2_1_1:
		case TYPE_MULTI_TEXTURE_ADD2:
			vs_module = &vertex->gen[1][0][0][0];
			fs_module = &vk.modules.frag.gen[1][0][fs_em][0];
			break;

		case TYPE_MULTI_TEXTURE_MUL2_ENV:
		case TYPE_MULTI_TEXTURE_ADD2_1_1_ENV:
		case TYPE_MULTI_TEXTURE_ADD2_ENV:
			vs_module = &vertex->gen[1][0][1][0];
			fs_module = &vk.modules.frag.gen[1][0][fs_em][0];
			break;

		case TYPE_MULTI_TEXTURE_MUL3:
		case TYPE_MULTI_TEXTURE_ADD3_1_1:
		case TYPE_MULTI_TEXTURE_ADD3:
			vs_module = &vertex->gen[2][0][0][0];
			fs_module = &vk.modules.frag.gen[2][0][fs_em][0];
			break;

		case TYPE_MULTI_TEXTURE_MUL3_ENV:
		case TYPE_MULTI_TEXTURE_ADD3_1_1_ENV:
		case TYPE_MULTI_TEXTURE_ADD3_ENV:
			vs_module = &vertex->gen[2][0][1][0];
			fs_module = &vk.modules.frag.gen[2][0][fs_em][0];
			break;

		case TYPE_BLEND2_ADD:
		case TYPE_BLEND2_MUL:
		case TYPE_BLEND2_ALPHA:
		case TYPE_BLEND2_ONE_MINUS_ALPHA:
		case TYPE_BLEND2_MIX_ALPHA:
		case TYPE_BLEND2_MIX_ONE_MINUS_ALPHA:
		case TYPE_BLEND2_DST_COLOR_SRC_ALPHA:
			vs_module = &vertex->gen[1][1][0][0];
			fs_module = &vk.modules.frag.gen[1][1][fs_em][0];
			break;

		case TYPE_BLEND2_ADD_ENV:
		case TYPE_BLEND2_MUL_ENV:
		case TYPE_BLEND2_ALPHA_ENV:
		case TYPE_BLEND2_ONE_MINUS_ALPHA_ENV:
		case TYPE_BLEND2_MIX_ALPHA_ENV:
		case TYPE_BLEND2_MIX_ONE_MINUS_ALPHA_ENV:
		case TYPE_BLEND2_DST_COLOR_SRC_ALPHA_ENV:
			vs_module = &vertex->gen[1][1][1][0];
			fs_module = &vk.modules.frag.gen[1][1][fs_em][0];
			break;

		case TYPE_BLEND3_ADD:
		case TYPE_BLEND3_MUL:
		case TYPE_BLEND3_ALPHA:
		case TYPE_BLEND3_ONE_MINUS_ALPHA:
		case TYPE_BLEND3_MIX_ALPHA:
		case TYPE_BLEND3_MIX_ONE_MINUS_ALPHA:
		case TYPE_BLEND3_DST_COLOR_SRC_ALPHA:
			vs_module = &vertex->gen[2][1][0][0];
			fs_module = &vk.modules.frag.gen[2][1][fs_em][0];
			break;

		case TYPE_BLEND3_ADD_ENV:
		case TYPE_BLEND3_MUL_ENV:
		case TYPE_BLEND3_ALPHA_ENV:
		case TYPE_BLEND3_ONE_MINUS_ALPHA_ENV:
		case TYPE_BLEND3_MIX_ALPHA_ENV:
		case TYPE_BLEND3_MIX_ONE_MINUS_ALPHA_ENV:
		case TYPE_BLEND3_DST_COLOR_SRC_ALPHA_ENV:
			vs_module = &vertex->gen[2][1][1][0];
			fs_module = &vk.modules.frag.gen[2][1][fs_em][0];
			break;

		case TYPE_COLOR_BLACK:
		case TYPE_COLOR_WHITE:
		case TYPE_COLOR_GREEN:
		case TYPE_COLOR_RED:
			vs_module = (multiview ? &vk.modules.color_vs_mv : &vk.modules.color_vs);
			fs_module = &vk.modules.color_fs;
			break;

		case TYPE_FOG_ONLY:
			vs_module = (multiview ? &vk.modules.fog_vs_mv : &vk.modules.fog_vs);
			fs_module = &vk.modules.fog_fs;
			break;

		case TYPE_DOT:
			vs_module = (multiview ? &vk.modules.dot_vs_mv : &vk.modules.dot_vs);
			if ( !multiview )
				fs_module = &vk.modules.dot_fs;
			else
				fs_module = (state_bits & GLS_DEPTHTEST_DISABLE) ? &vk.modules.dot_total_fs_mv : &vk.modules.dot_fs_mv;
			break;

		default:
			ri.Error(ERR_DROP, "create_pipeline: unknown shader type %i\n", def->shader_type);
			return 0;
	}

	if ( def->fog_stage ) {
		switch ( def->shader_type ) {
			case TYPE_FOG_ONLY:
			case TYPE_DOT:
			case TYPE_SINGLE_TEXTURE_DF:
			case TYPE_VR_SCREEN:
			case TYPE_VR_REFLECTION:
			case TYPE_VR_FLOOR_GRID:
			case TYPE_COLOR_BLACK:
			case TYPE_COLOR_WHITE:
			case TYPE_COLOR_GREEN:
			case TYPE_COLOR_RED:
				break;
			default:
				// switch to fogged modules
				vs_module++;
				fs_module++;
				break;
		}
	}

	set_shader_stage_desc(shader_stages+0, VK_SHADER_STAGE_VERTEX_BIT, *vs_module, "main");
	set_shader_stage_desc(shader_stages+1, VK_SHADER_STAGE_FRAGMENT_BIT, *fs_module, "main");

	//Com_Memset( vert_spec_data, 0, sizeof( vert_spec_data ) );
	Com_Memset( frag_spec_data, 0, sizeof( frag_spec_data ) );

	//vert_spec_data[0] = def->clipping_plane ? 1 : 0;

	// fragment shader specialization data
	atest_bits = state_bits & GLS_ATEST_BITS;
	switch ( atest_bits ) {
		case GLS_ATEST_GT_0:
			frag_spec_data[0].i = 1; // not equal
			frag_spec_data[1].f = 0.0f;
			break;
		case GLS_ATEST_LT_80:
			frag_spec_data[0].i = 2; // less than
			frag_spec_data[1].f = 0.5f;
			break;
		case GLS_ATEST_GE_80:
			frag_spec_data[0].i = 3; // greater or equal
			frag_spec_data[1].f = 0.5f;
			break;
		default:
			frag_spec_data[0].i = 0;
			frag_spec_data[1].f = 0.0f;
			break;
	};

	// depth fragment threshold
	frag_spec_data[2].f = 0.85f;

#if 0
	if ( r_ext_alpha_to_coverage->integer && vkSamples != VK_SAMPLE_COUNT_1_BIT && frag_spec_data[0].i ) {
		frag_spec_data[3].i = 1;
		alphaToCoverage = VK_TRUE;
	}
#endif

	// constant color
	switch ( def->shader_type ) {
		default: frag_spec_data[4].i = 0; break;
		case TYPE_COLOR_WHITE: frag_spec_data[4].i = 1; break;
		case TYPE_COLOR_GREEN: frag_spec_data[4].i = 2; break;
		case TYPE_COLOR_RED:   frag_spec_data[4].i = 3; break;
	}

	// abs lighting
	switch ( def->shader_type ) {
		case TYPE_SINGLE_TEXTURE_LIGHTING:
		case TYPE_SINGLE_TEXTURE_LIGHTING_LINEAR:
			frag_spec_data[5].i = def->abs_light ? 1 : 0;
		default:
			break;
	}

	// multutexture mode
	switch ( def->shader_type ) {
		case TYPE_MULTI_TEXTURE_MUL2_IDENTITY:
		case TYPE_MULTI_TEXTURE_MUL2_IDENTITY_ENV:
		case TYPE_MULTI_TEXTURE_MUL2_FIXED_COLOR:
		case TYPE_MULTI_TEXTURE_MUL2_FIXED_COLOR_ENV:
		case TYPE_MULTI_TEXTURE_MUL2:
		case TYPE_MULTI_TEXTURE_MUL2_ENV:
		case TYPE_MULTI_TEXTURE_MUL3:
		case TYPE_MULTI_TEXTURE_MUL3_ENV:
		case TYPE_BLEND2_MUL:
		case TYPE_BLEND2_MUL_ENV:
		case TYPE_BLEND3_MUL:
		case TYPE_BLEND3_MUL_ENV:
			frag_spec_data[6].i = 0;
			break;

		case TYPE_MULTI_TEXTURE_ADD2_IDENTITY:
		case TYPE_MULTI_TEXTURE_ADD2_IDENTITY_ENV:
		case TYPE_MULTI_TEXTURE_ADD2_FIXED_COLOR:
		case TYPE_MULTI_TEXTURE_ADD2_FIXED_COLOR_ENV:
		case TYPE_MULTI_TEXTURE_ADD2_1_1:
		case TYPE_MULTI_TEXTURE_ADD2_1_1_ENV:
		case TYPE_MULTI_TEXTURE_ADD3_1_1:
		case TYPE_MULTI_TEXTURE_ADD3_1_1_ENV:
			frag_spec_data[6].i = 1;
			break;

		case TYPE_MULTI_TEXTURE_ADD2:
		case TYPE_MULTI_TEXTURE_ADD2_ENV:
		case TYPE_MULTI_TEXTURE_ADD3:
		case TYPE_MULTI_TEXTURE_ADD3_ENV:
		case TYPE_BLEND2_ADD:
		case TYPE_BLEND2_ADD_ENV:
		case TYPE_BLEND3_ADD:
		case TYPE_BLEND3_ADD_ENV:
			frag_spec_data[6].i = 2;
			break;

		case TYPE_BLEND2_ALPHA:
		case TYPE_BLEND2_ALPHA_ENV:
		case TYPE_BLEND3_ALPHA:
		case TYPE_BLEND3_ALPHA_ENV:
			frag_spec_data[6].i = 3;
			break;

		case TYPE_BLEND2_ONE_MINUS_ALPHA:
		case TYPE_BLEND2_ONE_MINUS_ALPHA_ENV:
		case TYPE_BLEND3_ONE_MINUS_ALPHA:
		case TYPE_BLEND3_ONE_MINUS_ALPHA_ENV:
			frag_spec_data[6].i = 4;
			break;

		case TYPE_BLEND2_MIX_ALPHA:
		case TYPE_BLEND2_MIX_ALPHA_ENV:
		case TYPE_BLEND3_MIX_ALPHA:
		case TYPE_BLEND3_MIX_ALPHA_ENV:
			frag_spec_data[6].i = 5;
			break;

		case TYPE_BLEND2_MIX_ONE_MINUS_ALPHA:
		case TYPE_BLEND2_MIX_ONE_MINUS_ALPHA_ENV:
		case TYPE_BLEND3_MIX_ONE_MINUS_ALPHA:
		case TYPE_BLEND3_MIX_ONE_MINUS_ALPHA_ENV:
			frag_spec_data[6].i = 6;
			break;

		case TYPE_BLEND2_DST_COLOR_SRC_ALPHA:
		case TYPE_BLEND2_DST_COLOR_SRC_ALPHA_ENV:
		case TYPE_BLEND3_DST_COLOR_SRC_ALPHA:
		case TYPE_BLEND3_DST_COLOR_SRC_ALPHA_ENV:
			frag_spec_data[6].i = 7;
			break;

		default:
			break;
	}

	frag_spec_data[8].f = ((float)def->color.rgb) / 255.0;
	frag_spec_data[9].f = ((float)def->color.alpha) / 255.0;

	if ( def->fog_stage ) {
		frag_spec_data[10].i = def->acff;
	} else {
		frag_spec_data[10].i = 0;
	}

	//
	// vertex module specialization data
	//
#if 0
	spec_entries[0].constantID = 0; // clip_plane
	spec_entries[0].offset = 0 * sizeof( int32_t );
	spec_entries[0].size = sizeof( int32_t );

	vert_spec_info.mapEntryCount = 1;
	vert_spec_info.pMapEntries = spec_entries + 0;
	vert_spec_info.dataSize = 1 * sizeof( int32_t );
	vert_spec_info.pData = &vert_spec_data[0];
	shader_stages[0].pSpecializationInfo = &vert_spec_info;
#endif
	shader_stages[0].pSpecializationInfo = NULL;

	//
	// fragment module specialization data
	//

	spec_entries[1].constantID = 0;  // alpha-test-function
	spec_entries[1].offset = 0 * sizeof( int32_t );
	spec_entries[1].size = sizeof( int32_t );

	spec_entries[2].constantID = 1; // alpha-test-value
	spec_entries[2].offset = 1 * sizeof( int32_t );
	spec_entries[2].size = sizeof( float );

	spec_entries[3].constantID = 2; // depth-fragment
	spec_entries[3].offset = 2 * sizeof( int32_t );
	spec_entries[3].size = sizeof( float );

	spec_entries[4].constantID = 3; // alpha-to-coverage
	spec_entries[4].offset = 3 * sizeof( int32_t );
	spec_entries[4].size = sizeof( int32_t );

	spec_entries[5].constantID = 4; // color_mode
	spec_entries[5].offset = 4 * sizeof( int32_t );
	spec_entries[5].size = sizeof( int32_t );

	spec_entries[6].constantID = 5; // abs_light
	spec_entries[6].offset = 5 * sizeof( int32_t );
	spec_entries[6].size = sizeof( int32_t );

	spec_entries[7].constantID = 6; // multitexture mode
	spec_entries[7].offset = 6 * sizeof( int32_t );
	spec_entries[7].size = sizeof( int32_t );

	spec_entries[8].constantID = 7; // discard mode
	spec_entries[8].offset = 7 * sizeof( int32_t );
	spec_entries[8].size = sizeof( int32_t );

	spec_entries[9].constantID = 8; // fixed color
	spec_entries[9].offset = 8 * sizeof( int32_t );
	spec_entries[9].size = sizeof( float );

	spec_entries[10].constantID = 9; // fixed alpha
	spec_entries[10].offset = 9 * sizeof( int32_t );
	spec_entries[10].size = sizeof( float );

	spec_entries[11].constantID = 10; // acff
	spec_entries[11].offset = 10 * sizeof( int32_t );
	spec_entries[11].size = sizeof( int32_t );

	spec_entries[12].constantID = 11;
	spec_entries[12].offset = 11 * sizeof( int32_t );
	spec_entries[12].size = sizeof( int32_t );
	frag_spec_data[11].i = renderPassIndex == RENDER_PASS_HUD && def->hud_coverage == 1;
	spec_entries[13].constantID = 12;
	spec_entries[13].offset = 12 * sizeof( int32_t );
	spec_entries[13].size = sizeof( int32_t );
	frag_spec_data[12].i = renderPassIndex != RENDER_PASS_DESKTOP ? 0
						   : (vk.hdrActive || vk.present_format.format == VK_FORMAT_B8G8R8A8_SRGB ||
							  vk.present_format.format == VK_FORMAT_R8G8B8A8_SRGB)
							   ? 2
							   : 1;
	spec_entries[14].constantID = 13;
	spec_entries[14].offset = 13 * sizeof( int32_t );
	spec_entries[14].size = sizeof( int32_t );
	frag_spec_data[13].i = 0;
	frag_spec_info.mapEntryCount = 14;
	frag_spec_info.pMapEntries = spec_entries + 1;
	frag_spec_info.dataSize = sizeof( int32_t ) * 14;
	frag_spec_info.pData = &frag_spec_data[0];
	shader_stages[1].pSpecializationInfo = &frag_spec_info;

	//
	// Vertex input
	//
	num_binds = num_attrs = 0;
	switch ( def->shader_type ) {

		case TYPE_FOG_ONLY:
		case TYPE_DOT:
			push_bind( 0, sizeof( vec4_t ) );					// xyz array
			push_attr( 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT );
			break;

		case TYPE_COLOR_BLACK:
		case TYPE_COLOR_WHITE:
		case TYPE_COLOR_GREEN:
		case TYPE_COLOR_RED:
			push_bind( 0, sizeof( vec4_t ) );					// xyz array
			push_attr( 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT );
			break;

		case TYPE_VR_SCREEN:
		case TYPE_VR_REFLECTION:
		case TYPE_VR_FLOOR_GRID:
		case TYPE_SINGLE_TEXTURE_DF:
		case TYPE_SINGLE_TEXTURE_IDENTITY:
		case TYPE_SINGLE_TEXTURE_FIXED_COLOR:
		case TYPE_SINGLE_TEXTURE_ENT_COLOR:
			push_bind( 0, sizeof( vec4_t ) );					// xyz array
			push_bind( 2, sizeof( vec2_t ) );					// st0 array
			push_attr( 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT );
			push_attr( 2, 2, VK_FORMAT_R32G32_SFLOAT );
			break;

		case TYPE_SINGLE_TEXTURE:
			push_bind( 0, sizeof( vec4_t ) );					// xyz array
			push_bind( 1, sizeof( color4ub_t ) );				// color array
			push_bind( 2, sizeof( vec2_t ) );					// st0 array
			push_attr( 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT );
			push_attr( 1, 1, VK_FORMAT_R8G8B8A8_UNORM );
			push_attr( 2, 2, VK_FORMAT_R32G32_SFLOAT );
			break;

		case TYPE_SINGLE_TEXTURE_LIGHTING_OVERBRIGHT:
			push_bind( 0, sizeof( vec4_t ) );					// xyz array
			push_bind( 1, sizeof( color4ub_t ) );				// color array
			push_bind( 2, sizeof( vec2_t ) );					// st0 array
			push_bind( 5, sizeof( float ) );					// overbright array
			push_attr( 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT );
			push_attr( 1, 1, VK_FORMAT_R8G8B8A8_UNORM );
			push_attr( 2, 2, VK_FORMAT_R32G32_SFLOAT );
			push_attr( 8, 5, VK_FORMAT_R32_SFLOAT );			// GLSL location 8, binding 5
			break;

		case TYPE_SINGLE_TEXTURE_ENV:
			push_bind( 0, sizeof( vec4_t ) );					// xyz array
			push_bind( 1, sizeof( color4ub_t ) );				// color array
			//push_bind( 2, sizeof( vec2_t ) );					// st0 array
			push_bind( 5, sizeof( vec4_t ) );					// normals
			push_attr( 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT );
			push_attr( 1, 1, VK_FORMAT_R8G8B8A8_UNORM );
			//push_attr( 2, 2, VK_FORMAT_R8G8B8A8_UNORM );
			push_attr( 5, 5, VK_FORMAT_R32G32B32A32_SFLOAT );
			break;

		case TYPE_SINGLE_TEXTURE_IDENTITY_ENV:
		case TYPE_SINGLE_TEXTURE_FIXED_COLOR_ENV:
		case TYPE_SINGLE_TEXTURE_ENT_COLOR_ENV:
			push_bind( 0, sizeof( vec4_t ) );					// xyz array
			push_bind( 5, sizeof( vec4_t ) );					// normals
			push_attr( 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT );
			push_attr( 5, 5, VK_FORMAT_R32G32B32A32_SFLOAT );
			break;

		case TYPE_SINGLE_TEXTURE_LIGHTING:
		case TYPE_SINGLE_TEXTURE_LIGHTING_LINEAR:
			push_bind( 0, sizeof( vec4_t ) );					// xyz array
			push_bind( 1, sizeof( vec2_t ) );					// st0 array
			push_bind( 2, sizeof( vec4_t ) );					// normals array
			push_attr( 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT );
			push_attr( 1, 1, VK_FORMAT_R32G32_SFLOAT );
			push_attr( 2, 2, VK_FORMAT_R32G32B32A32_SFLOAT );
			break;

		case TYPE_MULTI_TEXTURE_MUL2_IDENTITY:
		case TYPE_MULTI_TEXTURE_ADD2_IDENTITY:
		case TYPE_MULTI_TEXTURE_MUL2_FIXED_COLOR:
		case TYPE_MULTI_TEXTURE_ADD2_FIXED_COLOR:
			push_bind( 0, sizeof( vec4_t ) );					// xyz array
			push_bind( 2, sizeof( vec2_t ) );					// st0 array
			push_bind( 3, sizeof( vec2_t ) );					// st1 array
			push_attr( 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT );
			push_attr( 2, 2, VK_FORMAT_R32G32_SFLOAT );
			push_attr( 3, 3, VK_FORMAT_R32G32_SFLOAT );
			break;

		case TYPE_MULTI_TEXTURE_MUL2_IDENTITY_ENV:
		case TYPE_MULTI_TEXTURE_ADD2_IDENTITY_ENV:
		case TYPE_MULTI_TEXTURE_MUL2_FIXED_COLOR_ENV:
		case TYPE_MULTI_TEXTURE_ADD2_FIXED_COLOR_ENV:
			push_bind( 0, sizeof( vec4_t ) );					// xyz array
			push_bind( 3, sizeof( vec2_t ) );					// st1 array
			push_bind( 5, sizeof( vec4_t ) );					// normals
			push_attr( 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT );
			push_attr( 3, 3, VK_FORMAT_R32G32_SFLOAT );
			push_attr( 5, 5, VK_FORMAT_R32G32B32A32_SFLOAT );
			break;

		case TYPE_MULTI_TEXTURE_MUL2:
		case TYPE_MULTI_TEXTURE_ADD2_1_1:
		case TYPE_MULTI_TEXTURE_ADD2:
			push_bind( 0, sizeof( vec4_t ) );					// xyz array
			push_bind( 1, sizeof( color4ub_t ) );				// color array
			push_bind( 2, sizeof( vec2_t ) );					// st0 array
			push_bind( 3, sizeof( vec2_t ) );					// st1 array
			push_attr( 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT );
			push_attr( 1, 1, VK_FORMAT_R8G8B8A8_UNORM );
			push_attr( 2, 2, VK_FORMAT_R32G32_SFLOAT );
			push_attr( 3, 3, VK_FORMAT_R32G32_SFLOAT );
			break;

		case TYPE_MULTI_TEXTURE_MUL2_ENV:
		case TYPE_MULTI_TEXTURE_ADD2_1_1_ENV:
		case TYPE_MULTI_TEXTURE_ADD2_ENV:
			push_bind( 0, sizeof( vec4_t ) );					// xyz array
			push_bind( 1, sizeof( color4ub_t ) );				// color array
			//push_bind( 2, sizeof( vec2_t ) );					// st0 array
			push_bind( 3, sizeof( vec2_t ) );					// st1 array
			push_bind( 5, sizeof( vec4_t ) );					// normals
			push_attr( 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT );
			push_attr( 1, 1, VK_FORMAT_R8G8B8A8_UNORM );
			//push_attr( 2, 2, VK_FORMAT_R32G32_SFLOAT );
			push_attr( 3, 3, VK_FORMAT_R32G32_SFLOAT );
			push_attr( 5, 5, VK_FORMAT_R32G32B32A32_SFLOAT );
			break;

		case TYPE_MULTI_TEXTURE_MUL3:
		case TYPE_MULTI_TEXTURE_ADD3_1_1:
		case TYPE_MULTI_TEXTURE_ADD3:
			push_bind( 0, sizeof( vec4_t ) );					// xyz array
			push_bind( 1, sizeof( color4ub_t ) );				// color array
			push_bind( 2, sizeof( vec2_t ) );					// st0 array
			push_bind( 3, sizeof( vec2_t ) );					// st1 array
			push_bind( 4, sizeof( vec2_t ) );					// st2 array
			push_attr( 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT );
			push_attr( 1, 1, VK_FORMAT_R8G8B8A8_UNORM );
			push_attr( 2, 2, VK_FORMAT_R32G32_SFLOAT );
			push_attr( 3, 3, VK_FORMAT_R32G32_SFLOAT );
			push_attr( 4, 4, VK_FORMAT_R32G32_SFLOAT );
			break;

		case TYPE_MULTI_TEXTURE_MUL3_ENV:
		case TYPE_MULTI_TEXTURE_ADD3_1_1_ENV:
		case TYPE_MULTI_TEXTURE_ADD3_ENV:
			push_bind( 0, sizeof( vec4_t ) );					// xyz array
			push_bind( 1, sizeof( color4ub_t ) );				// color array
			//push_bind( 2, sizeof( vec2_t ) );					// st0 array
			push_bind( 3, sizeof( vec2_t ) );					// st1 array
			push_bind( 4, sizeof( vec2_t ) );					// st2 array
			push_bind( 5, sizeof( vec4_t ) );					// normals
			push_attr( 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT );
			push_attr( 1, 1, VK_FORMAT_R8G8B8A8_UNORM );
			//push_attr( 2, 2, VK_FORMAT_R32G32_SFLOAT );
			push_attr( 3, 3, VK_FORMAT_R32G32_SFLOAT );
			push_attr( 4, 4, VK_FORMAT_R32G32_SFLOAT );
			push_attr( 5, 5, VK_FORMAT_R32G32B32A32_SFLOAT );
			break;

		case TYPE_BLEND2_ADD:
		case TYPE_BLEND2_MUL:
		case TYPE_BLEND2_ALPHA:
		case TYPE_BLEND2_ONE_MINUS_ALPHA:
		case TYPE_BLEND2_MIX_ALPHA:
		case TYPE_BLEND2_MIX_ONE_MINUS_ALPHA:
		case TYPE_BLEND2_DST_COLOR_SRC_ALPHA:
			push_bind( 0, sizeof( vec4_t ) );					// xyz array
			push_bind( 1, sizeof( color4ub_t ) );				// color0 array
			push_bind( 2, sizeof( vec2_t ) );					// st0 array
			push_bind( 3, sizeof( vec2_t ) );					// st1 array
			push_bind( 6, sizeof( color4ub_t ) );				// color1 array
			push_attr( 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT );
			push_attr( 1, 1, VK_FORMAT_R8G8B8A8_UNORM );
			push_attr( 2, 2, VK_FORMAT_R32G32_SFLOAT );
			push_attr( 3, 3, VK_FORMAT_R32G32_SFLOAT );
			push_attr( 6, 6, VK_FORMAT_R8G8B8A8_UNORM );
			break;

		case TYPE_BLEND2_ADD_ENV:
		case TYPE_BLEND2_MUL_ENV:
		case TYPE_BLEND2_ALPHA_ENV:
		case TYPE_BLEND2_ONE_MINUS_ALPHA_ENV:
		case TYPE_BLEND2_MIX_ALPHA_ENV:
		case TYPE_BLEND2_MIX_ONE_MINUS_ALPHA_ENV:
		case TYPE_BLEND2_DST_COLOR_SRC_ALPHA_ENV:
			push_bind( 0, sizeof( vec4_t ) );					// xyz array
			push_bind( 1, sizeof( color4ub_t ) );				// color0 array
			//push_bind( 2, sizeof( vec2_t ) );					// st0 array
			push_bind( 3, sizeof( vec2_t ) );					// st1 array
			push_bind( 5, sizeof( vec4_t ) );					// normals
			push_bind( 6, sizeof( color4ub_t ) );				// color1 array
			push_attr( 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT );
			push_attr( 1, 1, VK_FORMAT_R8G8B8A8_UNORM );
			//push_attr( 2, 2, VK_FORMAT_R32G32_SFLOAT );
			push_attr( 3, 3, VK_FORMAT_R32G32_SFLOAT );
			push_attr( 5, 5, VK_FORMAT_R32G32B32A32_SFLOAT );
			push_attr( 6, 6, VK_FORMAT_R8G8B8A8_UNORM );
			break;

		case TYPE_BLEND3_ADD:
		case TYPE_BLEND3_MUL:
		case TYPE_BLEND3_ALPHA:
		case TYPE_BLEND3_ONE_MINUS_ALPHA:
		case TYPE_BLEND3_MIX_ALPHA:
		case TYPE_BLEND3_MIX_ONE_MINUS_ALPHA:
		case TYPE_BLEND3_DST_COLOR_SRC_ALPHA:
			push_bind( 0, sizeof( vec4_t ) );					// xyz array
			push_bind( 1, sizeof( color4ub_t ) );				// color0 array
			push_bind( 2, sizeof( vec2_t ) );					// st0 array
			push_bind( 3, sizeof( vec2_t ) );					// st1 array
			push_bind( 4, sizeof( vec2_t ) );					// st2 array
			push_bind( 6, sizeof( color4ub_t ) );				// color1 array
			push_bind( 7, sizeof( color4ub_t ) );				// color2 array
			push_attr( 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT );
			push_attr( 1, 1, VK_FORMAT_R8G8B8A8_UNORM );
			push_attr( 2, 2, VK_FORMAT_R32G32_SFLOAT );
			push_attr( 3, 3, VK_FORMAT_R32G32_SFLOAT );
			push_attr( 4, 4, VK_FORMAT_R32G32_SFLOAT );
			push_attr( 6, 6, VK_FORMAT_R8G8B8A8_UNORM );
			push_attr( 7, 7, VK_FORMAT_R8G8B8A8_UNORM );
			break;

		case TYPE_BLEND3_ADD_ENV:
		case TYPE_BLEND3_MUL_ENV:
		case TYPE_BLEND3_ALPHA_ENV:
		case TYPE_BLEND3_ONE_MINUS_ALPHA_ENV:
		case TYPE_BLEND3_MIX_ALPHA_ENV:
		case TYPE_BLEND3_MIX_ONE_MINUS_ALPHA_ENV:
		case TYPE_BLEND3_DST_COLOR_SRC_ALPHA_ENV:
			push_bind( 0, sizeof( vec4_t ) );					// xyz array
			push_bind( 1, sizeof( color4ub_t ) );				// color0 array
			//push_bind( 2, sizeof( vec2_t ) );					// st0 array
			push_bind( 3, sizeof( vec2_t ) );					// st1 array
			push_bind( 4, sizeof( vec2_t ) );					// st2 array
			push_bind( 5, sizeof( vec4_t ) );					// normals
			push_bind( 6, sizeof( color4ub_t ) );				// color1 array
			push_bind( 7, sizeof( color4ub_t ) );				// color2 array
			push_attr( 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT );
			push_attr( 1, 1, VK_FORMAT_R8G8B8A8_UNORM );
			//push_attr( 2, 2, VK_FORMAT_R32G32_SFLOAT );
			push_attr( 3, 3, VK_FORMAT_R32G32_SFLOAT );
			push_attr( 4, 4, VK_FORMAT_R32G32_SFLOAT );
			push_attr( 5, 5, VK_FORMAT_R32G32B32A32_SFLOAT );
			push_attr( 6, 6, VK_FORMAT_R8G8B8A8_UNORM );
			push_attr( 7, 7, VK_FORMAT_R8G8B8A8_UNORM );
			break;

		default:
			ri.Error( ERR_DROP, "%s: invalid shader type - %i", __func__, def->shader_type );
			break;
	}

	vertex_input_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertex_input_state.pNext = NULL;
	vertex_input_state.flags = 0;
	vertex_input_state.pVertexBindingDescriptions = bindings;
	vertex_input_state.pVertexAttributeDescriptions = attribs;
	vertex_input_state.vertexBindingDescriptionCount = num_binds;
	vertex_input_state.vertexAttributeDescriptionCount = num_attrs;

	//
	// Primitive assembly.
	//
	input_assembly_state.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	input_assembly_state.pNext = NULL;
	input_assembly_state.flags = 0;
	input_assembly_state.primitiveRestartEnable = VK_FALSE;

	switch ( def->primitives ) {
		case LINE_LIST: input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
		case POINT_LIST:
			input_assembly_state.topology = VK_FlareProbeTriangles( vk.multiview, renderPassIndex )
												? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
												: VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
			break;
		case TRIANGLE_STRIP: input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
		default: input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
	}

	//
	// Viewport.
	//
	viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewport_state.pNext = NULL;
	viewport_state.flags = 0;
	viewport_state.viewportCount = 1;
	viewport_state.pViewports = NULL; // dynamic viewport state
	viewport_state.scissorCount = 1;
	viewport_state.pScissors = NULL; // dynamic scissor state

	//
	// Rasterization.
	//
	rasterization_state.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterization_state.pNext = NULL;
	rasterization_state.flags = 0;
	rasterization_state.depthClampEnable = ((def->shadow_phase == SHADOW_FS_QUAD || def->shadow_phase == SHADOW_EDGES) && vk.depthClamp) ? VK_TRUE : VK_FALSE;
	rasterization_state.rasterizerDiscardEnable = VK_FALSE;
	if ( def->shader_type == TYPE_DOT ) {
		rasterization_state.polygonMode = multiview ? VK_POLYGON_MODE_FILL : VK_POLYGON_MODE_POINT;
	} else {
		rasterization_state.polygonMode = (state_bits & GLS_POLYMODE_LINE) ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
	}

	switch ( def->face_culling ) {
		case CT_TWO_SIDED:
			rasterization_state.cullMode = VK_CULL_MODE_NONE;
			break;
		case CT_FRONT_SIDED:
			rasterization_state.cullMode = (def->mirror ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_BACK_BIT);
			break;
		case CT_BACK_SIDED:
			rasterization_state.cullMode = (def->mirror ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_FRONT_BIT);
			break;
		default:
			ri.Error( ERR_DROP, "create_pipeline: invalid face culling mode %i\n", def->face_culling );
			break;
	}

	rasterization_state.frontFace = VK_FRONT_FACE_CLOCKWISE; // Q3 defaults to clockwise vertex order

	 // depth bias state
	if ( def->polygon_offset ) {
		rasterization_state.depthBiasEnable = VK_TRUE;
		rasterization_state.depthBiasClamp = 0.0f;
#ifdef USE_REVERSED_DEPTH
		rasterization_state.depthBiasConstantFactor = -r_offsetUnits->value;
		rasterization_state.depthBiasSlopeFactor = -r_offsetFactor->value;
#else
		rasterization_state.depthBiasConstantFactor = r_offsetUnits->value;
		rasterization_state.depthBiasSlopeFactor = r_offsetFactor->value;
#endif
	} else {
		rasterization_state.depthBiasEnable = VK_FALSE;
		rasterization_state.depthBiasClamp = 0.0f;
		rasterization_state.depthBiasConstantFactor = 0.0f;
		rasterization_state.depthBiasSlopeFactor = 0.0f;
	}

	if ( def->line_width )
		rasterization_state.lineWidth = (float)def->line_width;
	else
		rasterization_state.lineWidth = 1.0f;

	multisample_state.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample_state.pNext = NULL;
	multisample_state.flags = 0;

	multisample_state.rasterizationSamples = (renderPassIndex == RENDER_PASS_SCREENMAP) ? vk.screenMapSamples : vkSamples;
	if ( renderPassIndex == RENDER_PASS_HUD || VK_PassIsVRScreen( renderPassIndex ) ||
		renderPassIndex == RENDER_PASS_DESKTOP || VK_PassIsPostScene( renderPassIndex ) ) {
		multisample_state.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	}

	multisample_state.sampleShadingEnable = VK_FALSE;
	multisample_state.minSampleShading = 1.0f;
	multisample_state.pSampleMask = NULL;
	multisample_state.alphaToCoverageEnable = alphaToCoverage;
	multisample_state.alphaToOneEnable = VK_FALSE;

	Com_Memset( &depth_stencil_state, 0, sizeof( depth_stencil_state ) );

	depth_stencil_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depth_stencil_state.pNext = NULL;
	depth_stencil_state.flags = 0;
	depth_stencil_state.depthTestEnable = (state_bits & GLS_DEPTHTEST_DISABLE) ? VK_FALSE : VK_TRUE;
	depth_stencil_state.depthWriteEnable = (state_bits & GLS_DEPTHMASK_TRUE) ? VK_TRUE : VK_FALSE;
#ifdef USE_REVERSED_DEPTH
	depth_stencil_state.depthCompareOp = (state_bits & GLS_DEPTHFUNC_EQUAL) ? VK_COMPARE_OP_EQUAL : VK_COMPARE_OP_GREATER_OR_EQUAL;
#else
	depth_stencil_state.depthCompareOp = (state_bits & GLS_DEPTHFUNC_EQUAL) ? VK_COMPARE_OP_EQUAL : VK_COMPARE_OP_LESS_OR_EQUAL;
#endif
	depth_stencil_state.depthBoundsTestEnable = VK_FALSE;
	depth_stencil_state.stencilTestEnable = (def->shadow_phase != SHADOW_DISABLED || def->stencil_mark) ? VK_TRUE : VK_FALSE;

	if (def->stencil_mark) {
		// mark entity pixels with stencil bit 0x80 so shadow finish skips them
		depth_stencil_state.front.failOp = VK_STENCIL_OP_KEEP;
		depth_stencil_state.front.passOp = VK_STENCIL_OP_REPLACE;
		depth_stencil_state.front.depthFailOp = VK_STENCIL_OP_KEEP;
		depth_stencil_state.front.compareOp = VK_COMPARE_OP_ALWAYS;
		depth_stencil_state.front.compareMask = 0xFF;
		depth_stencil_state.front.writeMask = 0x80;
		depth_stencil_state.front.reference = 0x80;

		depth_stencil_state.back = depth_stencil_state.front;

	} else if (def->shadow_phase == SHADOW_EDGES) {
		// z-fail (Carmack's reverse): count fragments behind scene geometry.
		// WRAP keeps the count modular across nested volumes and draw order;
		// writeMask 0x7F preserves the 0x80 entity-mark bit. Needs a closed
		// volume (near + far caps) and depth clamp.
		depth_stencil_state.front.failOp = VK_STENCIL_OP_KEEP;
		depth_stencil_state.front.passOp = VK_STENCIL_OP_KEEP;
		depth_stencil_state.front.depthFailOp = (def->face_culling == CT_FRONT_SIDED) ? VK_STENCIL_OP_DECREMENT_AND_WRAP : VK_STENCIL_OP_INCREMENT_AND_WRAP;
		depth_stencil_state.front.compareOp = VK_COMPARE_OP_EQUAL;
		depth_stencil_state.front.compareMask = 0x80;  // skip entity-marked pixels
		depth_stencil_state.front.writeMask = 0x7F;    // only write shadow count to bits 0-6
		depth_stencil_state.front.reference = 0;        // pass where bit 7 == 0 (not entity)

		depth_stencil_state.back = depth_stencil_state.front;

	} else if (def->shadow_phase == SHADOW_FS_QUAD) {
		depth_stencil_state.front.failOp = VK_STENCIL_OP_KEEP;
		depth_stencil_state.front.passOp = VK_STENCIL_OP_KEEP;
		depth_stencil_state.front.depthFailOp = VK_STENCIL_OP_KEEP;
		depth_stencil_state.front.compareOp = VK_COMPARE_OP_NOT_EQUAL;
		depth_stencil_state.front.compareMask = 0x7F;  // check shadow bits 0-6 only
		depth_stencil_state.front.writeMask = 0x7F;
		depth_stencil_state.front.reference = 0;

		depth_stencil_state.back = depth_stencil_state.front;
	}

	depth_stencil_state.minDepthBounds = 0.0f;
	depth_stencil_state.maxDepthBounds = 1.0f;

	Com_Memset(&attachment_blend_state, 0, sizeof(attachment_blend_state));
	attachment_blend_state.blendEnable = (state_bits & (GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS)) ? VK_TRUE : VK_FALSE;

	if ( def->shadow_phase == SHADOW_EDGES || def->shader_type == TYPE_SINGLE_TEXTURE_DF || def->shader_type == TYPE_DOT ) {
		attachment_blend_state.colorWriteMask = 0;
	} else {
		attachment_blend_state.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	}

	if ( attachment_blend_state.blendEnable ) {
		switch (state_bits & GLS_SRCBLEND_BITS) {
			case GLS_SRCBLEND_ZERO:
				attachment_blend_state.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
				break;
			case GLS_SRCBLEND_ONE:
				attachment_blend_state.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
				break;
			case GLS_SRCBLEND_DST_COLOR:
				attachment_blend_state.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
				break;
			case GLS_SRCBLEND_ONE_MINUS_DST_COLOR:
				attachment_blend_state.srcColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
				break;
			case GLS_SRCBLEND_SRC_ALPHA:
				attachment_blend_state.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
				break;
			case GLS_SRCBLEND_ONE_MINUS_SRC_ALPHA:
				attachment_blend_state.srcColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
				break;
			case GLS_SRCBLEND_DST_ALPHA:
				attachment_blend_state.srcColorBlendFactor = VK_BLEND_FACTOR_DST_ALPHA;
				break;
			case GLS_SRCBLEND_ONE_MINUS_DST_ALPHA:
				attachment_blend_state.srcColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
				break;
			case GLS_SRCBLEND_ALPHA_SATURATE:
				attachment_blend_state.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
				break;
			default:
				ri.Error( ERR_DROP, "create_pipeline: invalid src blend state bits\n" );
				break;
		}
		switch (state_bits & GLS_DSTBLEND_BITS) {
			case GLS_DSTBLEND_ZERO:
				attachment_blend_state.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
				break;
			case GLS_DSTBLEND_ONE:
				attachment_blend_state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
				break;
			case GLS_DSTBLEND_SRC_COLOR:
				attachment_blend_state.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
				break;
			case GLS_DSTBLEND_ONE_MINUS_SRC_COLOR:
				attachment_blend_state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
				break;
			case GLS_DSTBLEND_SRC_ALPHA:
				attachment_blend_state.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
				break;
			case GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA:
				attachment_blend_state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
				break;
			case GLS_DSTBLEND_DST_ALPHA:
				attachment_blend_state.dstColorBlendFactor = VK_BLEND_FACTOR_DST_ALPHA;
				break;
			case GLS_DSTBLEND_ONE_MINUS_DST_ALPHA:
				attachment_blend_state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
				break;
			default:
				ri.Error( ERR_DROP, "create_pipeline: invalid dst blend state bits\n" );
				break;
		}

		attachment_blend_state.srcAlphaBlendFactor = attachment_blend_state.srcColorBlendFactor;
		attachment_blend_state.dstAlphaBlendFactor = attachment_blend_state.dstColorBlendFactor;
		attachment_blend_state.colorBlendOp = VK_BLEND_OP_ADD;
		attachment_blend_state.alphaBlendOp = VK_BLEND_OP_ADD;
		if ( renderPassIndex == RENDER_PASS_HUD ) {
			VK_HudAlphaBlend( state_bits, def->hud_coverage, &attachment_blend_state );
		}

		if ( def->allow_discard && vkSamples != VK_SAMPLE_COUNT_1_BIT && depth_stencil_state.depthWriteEnable == VK_FALSE ) {
			// try to reduce pixel fillrate for transparent surfaces, this yields 1..10% fps increase when multisampling in enabled
			if ( attachment_blend_state.srcColorBlendFactor == VK_BLEND_FACTOR_SRC_ALPHA && attachment_blend_state.dstColorBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA ) {
				frag_spec_data[7].i = 1;
			} else if ( attachment_blend_state.srcColorBlendFactor == VK_BLEND_FACTOR_ONE && attachment_blend_state.dstColorBlendFactor == VK_BLEND_FACTOR_ONE ) {
				frag_spec_data[7].i = 2;
			}
		}
	}

	blend_state.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	blend_state.pNext = NULL;
	blend_state.flags = 0;
	blend_state.logicOpEnable = VK_FALSE;
	blend_state.logicOp = VK_LOGIC_OP_COPY;

	if ( VK_PassIsPostScene( renderPassIndex ) ||
		 ( def->scene_alpha == 3 && ( renderPassIndex == RENDER_PASS_MAIN || renderPassIndex == RENDER_PASS_MONO_MAIN ) ) ) {
		// the shader's alpha feeds the scene image's transmittance, which gates bloom and HDR highlights in the
		// composite; 2D drawn in the scene pass (bloom off, frames without a world) lowers it the same way
		frag_spec_data[13].i = 1 + vk_transmittance_blend( &attachment_blend_state );
	} else if ( renderPassIndex == RENDER_PASS_MAIN || renderPassIndex == RENDER_PASS_MONO_MAIN ) {
		// the scene image's alpha is 1 after the scene: full transmittance until post-scene draws lower it.
		// Stages feeding a later destination-alpha blend keep writing it, and their shader's last stage
		// writes 1 back, through the shader when its own color blend doesn't read the alpha
		switch ( def->scene_alpha ) {
		case 1:
			break;
		case 2:
			if ( attachment_blend_state.srcColorBlendFactor != VK_BLEND_FACTOR_SRC_ALPHA &&
				 attachment_blend_state.srcColorBlendFactor != VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA &&
				 attachment_blend_state.dstColorBlendFactor != VK_BLEND_FACTOR_SRC_ALPHA &&
				 attachment_blend_state.dstColorBlendFactor != VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA )
				frag_spec_data[11].i = 1;
			attachment_blend_state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
			attachment_blend_state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
			attachment_blend_state.alphaBlendOp = VK_BLEND_OP_ADD;
			break;
		default:
			attachment_blend_state.colorWriteMask &= ~VK_COLOR_COMPONENT_A_BIT;
			break;
		}
	}

	blend_attachments[0] = attachment_blend_state;

	if ( emissiveActive ) {
		// the emissive layer mirrors the color blend, so it only needs writes from
		// stages that can add light or that cover an emitter without depth
		// occlusion. Gen stages emit only when blended (emissive_factor is nonzero
		// only for dstblend ONE); depth-test-disabled 2D replaces what it covers.
		// Opaque depth-tested gen geometry is masked off: the layer clears each
		// frame and depth already excludes hidden emitters. The lighting and
		// dynamic-light shaders always write out_emissive (dlight glow) and keep
		// the mask unconditionally.
		VkPipelineColorBlendAttachmentState em = attachment_blend_state;
		if ( ( def->shader_type >= TYPE_GENERIC_BEGIN
				&& ( ( def->state_bits & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS ) )
					|| ( def->state_bits & GLS_DEPTHTEST_DISABLE ) ) )
			|| def->shader_type == TYPE_SINGLE_TEXTURE_LIGHTING
			|| def->shader_type == TYPE_SINGLE_TEXTURE_LIGHTING_LINEAR
			|| def->shader_type == TYPE_SINGLE_TEXTURE_LIGHTING_OVERBRIGHT )
			em.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
		else
			em.colorWriteMask = 0;
		blend_attachments[1] = em;
		blend_state.attachmentCount = 2;
	} else {
		blend_state.attachmentCount = 1;
	}
	blend_state.pAttachments = blend_attachments;
	blend_state.blendConstants[0] = 0.0f;
	blend_state.blendConstants[1] = 0.0f;
	blend_state.blendConstants[2] = 0.0f;
	blend_state.blendConstants[3] = 0.0f;

	dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamic_state.pNext = NULL;
	dynamic_state.flags = 0;
	dynamic_state.dynamicStateCount = vk_foveation_caps.backend == VK_FOV_BACKEND_SHADING_RATE ? 3 : 2;
	dynamic_state.pDynamicStates = dynamic_state_array;

	create_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	create_info.pNext = NULL;
	create_info.flags = 0;
	create_info.stageCount = ARRAY_LEN(shader_stages);
	create_info.pStages = shader_stages;
	create_info.pVertexInputState = &vertex_input_state;
	create_info.pInputAssemblyState = &input_assembly_state;
	create_info.pTessellationState = NULL;
	create_info.pViewportState = &viewport_state;
	create_info.pRasterizationState = &rasterization_state;
	create_info.pMultisampleState = &multisample_state;
	create_info.pDepthStencilState = &depth_stencil_state;
	create_info.pColorBlendState = &blend_state;
	create_info.pDynamicState = &dynamic_state;

	if ( def->shader_type == TYPE_DOT )
		create_info.layout = vk.pipeline_layout_storage;
	else
		create_info.layout = vk.pipeline_layout;

	if ( renderPassIndex == RENDER_PASS_MONO_MAIN )
		create_info.renderPass = vk.mono.pass.main;
	else if ( renderPassIndex == RENDER_PASS_DESKTOP )
		create_info.renderPass = vk.render_pass.gamma;
	else if ( renderPassIndex == RENDER_PASS_VR_SCREEN )
		create_info.renderPass = vk_screen.pass;
	else if ( renderPassIndex == RENDER_PASS_HUD )
		create_info.renderPass = vk_hud.pass;
	else if ( renderPassIndex == RENDER_PASS_SCREENMAP )
		create_info.renderPass = vk.render_pass.screenmap;
	else if ( renderPassIndex == RENDER_PASS_VR_SCREEN_EYE )
		create_info.renderPass = vk.xr_output.screen_pass;
	else if ( renderPassIndex == RENDER_PASS_POST_SCENE && vk.render_pass.post_scene )
		create_info.renderPass = vk.render_pass.post_scene;
	else if ( renderPassIndex == RENDER_PASS_MONO_POST_SCENE && vk.mono.pass.post_scene )
		create_info.renderPass = vk.mono.pass.post_scene;
	else
		create_info.renderPass = vk.render_pass.main;

	create_info.subpass = 0;
	create_info.basePipelineHandle = VK_NULL_HANDLE;
	create_info.basePipelineIndex = -1;

	{
		VkResult res = qvkCreateGraphicsPipelines( vk.device, vk.pipelineCache, 1, &create_info, NULL, &pipeline );
		if ( res < 0 ) {
			// deviceWaitIdle tells a bad create apart from an earlier device loss
			const VkResult idle = qvkDeviceWaitIdle( vk.device );
			ri.Error( ERR_FATAL, "Vulkan: vkCreateGraphicsPipelines returned %s for def#%i "
				"(type=%i state=0x%08X pass=%i fog=%i mirror=%i shadow=%i cull=%i "
				"pofs=%i prim=%i acff=%i stencil=%i color=%02X/%02X), deviceWaitIdle=%s",
				vk_result_string( res ), def_index,
				def->shader_type, def->state_bits, renderPassIndex,
				def->fog_stage, def->mirror, def->shadow_phase, def->face_culling,
				def->polygon_offset, def->primitives, def->acff, def->stencil_mark,
				def->color.rgb, def->color.alpha,
				vk_result_string( idle ) );
		}
	}

	SET_OBJECT_NAME( pipeline, va( "pipeline def#%i, pass#%i", def_index, renderPassIndex ), VK_DEBUG_REPORT_OBJECT_TYPE_PIPELINE_EXT );

	vk.pipeline_create_count++;

	return pipeline;
}


static uint32_t vk_alloc_pipeline( const Vk_Pipeline_Def *def ) {
	VK_Pipeline_t *pipeline;
	if ( vk.pipelines_count >= MAX_VK_PIPELINES ) {
		ri.Error( ERR_DROP, "alloc_pipeline: MAX_VK_PIPELINES reached" );
		return 0;
	} else {
		int j;
		pipeline = &vk.pipelines[ vk.pipelines_count ];
		pipeline->def = *def;
		for ( j = 0; j < RENDER_PASS_COUNT; j++ ) {
			pipeline->handle[j] = VK_NULL_HANDLE;
		}
		return vk.pipelines_count++;
	}
}


VkPipeline vk_gen_pipeline( uint32_t index ) {
	if ( index < vk.pipelines_count ) {
		VK_Pipeline_t *pipeline = vk.pipelines + index;
		const renderPass_t pass = vk.renderPassIndex;
		if ( pipeline->handle[ pass ] == VK_NULL_HANDLE ) {
			pipeline->handle[ pass ] = create_pipeline( &pipeline->def, pass, index );
		}
		return pipeline->handle[ pass ];
	} else {
		ri.Error( ERR_FATAL, "%s(%i): NULL pipeline", __func__, index );
		return VK_NULL_HANDLE;
	}
}


uint32_t vk_find_pipeline_ext( uint32_t base, const Vk_Pipeline_Def *def, qboolean use ) {
	const Vk_Pipeline_Def *cur_def;
	uint32_t index;

	for ( index = base; index < vk.pipelines_count; index++ ) {
		cur_def = &vk.pipelines[ index ].def;
		if ( memcmp( cur_def, def, sizeof( *def ) ) == 0 ) {
			goto found;
		}
	}

	index = vk_alloc_pipeline( def );
found:

	if ( use )
		vk_gen_pipeline( index );

	return index;
}


void vk_get_pipeline_def( uint32_t pipeline, Vk_Pipeline_Def *def ) {
	if ( pipeline >= vk.pipelines_count ) {
		Com_Memset( def, 0, sizeof( *def ) );
	} else {
		Com_Memcpy( def, &vk.pipelines[ pipeline ].def, sizeof( *def ) );
	}
}


static void get_viewport_rect(VkRect2D *r)
{
	if ( vk_hud_direct ) {
		r->offset.x = 0;
		r->offset.y = 0;
		r->extent.width = vk.renderWidth;
		r->extent.height = vk.renderHeight;
		return;
	}
	if ( backEnd.projection2D )
	{
		r->offset.x = 0;
		r->offset.y = 0;
		r->extent.width = vk.renderWidth;
		r->extent.height = vk.renderHeight;
	}
	else
	{
		if ( vk_hud.recording ) {
			/* The VR module emits mode-1 HUD coordinates directly in 1280x960. */
			r->offset.x = backEnd.viewParms.viewportX;
			r->offset.y = glConfig.vidHeight - backEnd.viewParms.viewportY - backEnd.viewParms.viewportHeight;
			r->extent.width = backEnd.viewParms.viewportWidth;
			r->extent.height = backEnd.viewParms.viewportHeight;
			return;
		}
		r->offset.x = backEnd.viewParms.viewportX * vk.renderScaleX;
		r->offset.y = vk.renderHeight - (backEnd.viewParms.viewportY + backEnd.viewParms.viewportHeight) * vk.renderScaleY;
		r->extent.width = (float)backEnd.viewParms.viewportWidth * vk.renderScaleX;
		r->extent.height = (float)backEnd.viewParms.viewportHeight * vk.renderScaleY;
	}
}

static void get_viewport(VkViewport *viewport, Vk_Depth_Range depth_range) {
	VkRect2D r;

	get_viewport_rect( &r );

	viewport->x = (float)r.offset.x;
	viewport->y = (float)r.offset.y;
	viewport->width = (float)r.extent.width;
	viewport->height = (float)r.extent.height;

	switch ( depth_range ) {
		default:
#ifdef USE_REVERSED_DEPTH
		//case DEPTH_RANGE_NORMAL:
			viewport->minDepth = 0.0f;
			viewport->maxDepth = 1.0f;
			break;
		case DEPTH_RANGE_ZERO:
			viewport->minDepth = 1.0f;
			viewport->maxDepth = 1.0f;
			break;
		case DEPTH_RANGE_ONE:
			viewport->minDepth = 0.0f;
			viewport->maxDepth = 0.0f;
			break;
		case DEPTH_RANGE_WEAPON:
			viewport->minDepth = 0.6f;
			viewport->maxDepth = 1.0f;
			break;
#else
		//case DEPTH_RANGE_NORMAL:
			viewport->minDepth = 0.0f;
			viewport->maxDepth = 1.0f;
			break;
		case DEPTH_RANGE_ZERO:
			viewport->minDepth = 0.0f;
			viewport->maxDepth = 0.0f;
			break;
		case DEPTH_RANGE_ONE:
			viewport->minDepth = 1.0f;
			viewport->maxDepth = 1.0f;
			break;
		case DEPTH_RANGE_WEAPON:
			viewport->minDepth = 0.0f;
			viewport->maxDepth = 0.3f;
			break;
#endif
	}
}

static void VK_ClampScissor( VkRect2D *r, int width, int height ) {
	/* The HUD target has its own dimensions. Clamping against the desktop can
	 * underflow an unsigned extent when an icon lies below its bottom edge. */
	if ( r->offset.x < 0 ) {
		r->offset.x = 0;
	}
	if ( r->offset.y < 0 ) {
		r->offset.y = 0;
	}
	if ( r->offset.x >= width || r->offset.y >= height ) {
		r->offset.x = r->offset.y = 0;
		r->extent.width = r->extent.height = 0;
		return;
	}
	if ( r->extent.width > (uint32_t)(width - r->offset.x) ) {
		r->extent.width = width - r->offset.x;
	}
	if ( r->extent.height > (uint32_t)(height - r->offset.y) ) {
		r->extent.height = height - r->offset.y;
	}
}

static void get_scissor_rect(VkRect2D *r) {

	if ( backEnd.viewParms.portalView != PV_NONE )
	{
		r->offset.x = backEnd.viewParms.scissorX * vk.renderScaleX;
		r->offset.y = vk.renderHeight -
					  (backEnd.viewParms.scissorY + backEnd.viewParms.scissorHeight) * vk.renderScaleY;
		r->extent.width = backEnd.viewParms.scissorWidth * vk.renderScaleX;
		r->extent.height = backEnd.viewParms.scissorHeight * vk.renderScaleY;
	}
	else
	{
		get_viewport_rect(r);

		if (r->offset.x < 0)
			r->offset.x = 0;
		if (r->offset.y < 0)
			r->offset.y = 0;

	}
	VK_ClampScissor( r, vk.renderWidth, vk.renderHeight );
}

/* Embed a model icon's viewport into the logical HUD before applying the
 * per-eye HUD transform. Rasterization itself stays at native eye resolution. */
static void VK_HudViewportMatrix( float x, float y, float w, float h, float width, float height,
								  float m[16] ) {
	int i;
	for ( i = 0; i < 16; i++ )
		m[i] = 0;
	m[0] = w / width;
	m[5] = h / height;
	m[10] = m[15] = 1;
	m[12] = (2 * x + w) / width - 1;
	m[13] = (2 * y + h) / height - 1;
}

/* Quake projections already carry Vulkan depth, but use OpenGL's Y direction. */
static void VK_FlipProjectionY( const float in[16], float out[16] ) {
	int i;
	for ( i = 0; i < 16; i++ )
		out[i] = in[i];
	/* Flip the whole Y row, including the off-axis lens-center term. */
	for ( i = 1; i < 16; i += 4 )
		out[i] = -in[i];
}

static void get_mvp_transform( float *mvp )
{
	if ( backEnd.projection2D )
	{
		float mvp0 = 2.0f / (vk_hud.recording ? VK_HUD_WIDTH : glConfig.vidWidth);
		float mvp5 = 2.0f / (vk_hud.recording ? VK_HUD_HEIGHT : glConfig.vidHeight);

		mvp[0]  =  mvp0; mvp[1]  =  0.0f; mvp[2]  = 0.0f; mvp[3]  = 0.0f;
		mvp[4]  =  0.0f; mvp[5]  =  mvp5; mvp[6]  = 0.0f; mvp[7]  = 0.0f;
#ifdef USE_REVERSED_DEPTH
		mvp[8]  =  0.0f; mvp[9]  =  0.0f; mvp[10] = 0.0f; mvp[11] = 0.0f;
		mvp[12] = -1.0f; mvp[13] = -1.0f; mvp[14] = 1.0f; mvp[15] = 1.0f;
#else
		mvp[8]  =  0.0f; mvp[9]  =  0.0f; mvp[10] = 1.0f; mvp[11] = 0.0f;
		mvp[12] = -1.0f; mvp[13] = -1.0f; mvp[14] = 0.0f; mvp[15] = 1.0f;
#endif
		if ( !vk_hud.recording && vk.renderPassIndex != RENDER_PASS_DESKTOP ) {
			float scale = VK_XR_ScopeScaleY();
			mvp[5] *= scale;
			mvp[13] *= scale;
		}
	}
	else
	{
		const float *p = backEnd.viewParms.projectionMatrix;
		float proj[16];
		VK_FlipProjectionY( p, proj );

		// update q3's proj matrix (opengl) to vulkan conventions: z - [0, 1] instead of [-1, 1] and invert y direction
		//proj[10] = ( p[10] - 1.0f ) / 2.0f;
		//proj[14] = p[14] / 2.0f;
		myGlMultMatrix( vk_world.modelview_transform, proj, mvp );
	}
	if ( vk_hud_direct ) {
		float hud[16], logical[16], result[16];
		int i;
		VK_XR_HudMatrix( vk_hud_eye, hud );
		if ( !backEnd.projection2D ) {
			float viewport[16];
			VK_HudViewportMatrix( backEnd.viewParms.viewportX,
								  glConfig.vidHeight - backEnd.viewParms.viewportY -
									  backEnd.viewParms.viewportHeight,
								  backEnd.viewParms.viewportWidth, backEnd.viewParms.viewportHeight,
								  glConfig.vidWidth, glConfig.vidHeight, viewport );
			myGlMultMatrix( mvp, viewport, logical );
		} else {
			Com_Memcpy( logical, mvp, sizeof( logical ) );
		}
		myGlMultMatrix( logical, hud, result );
		/* Model icons retain their own depth ordering; flat graphics use the
		 * HUD plane's depth. Never flatten the head's front and back faces. */
		if ( !backEnd.projection2D ) {
			for ( i = 0; i < 4; i++ )
				result[i * 4 + 2] = logical[i * 4 + 2];
		}
		Com_Memcpy( mvp, result, sizeof( result ) );
	}
}


void vk_clear_color( const vec4_t color ) {

	VkClearAttachment attachment;
	VkClearRect clear_rect;

	if ( !vk.active )
		return;

	attachment.colorAttachment = 0;
	attachment.clearValue.color.float32[0] = color[0];
	attachment.clearValue.color.float32[1] = color[1];
	attachment.clearValue.color.float32[2] = color[2];
	// the scene image's alpha is transmittance, not the caller's alpha: a clear hides the scene in the post-scene
	// pass and is the scene elsewhere
	attachment.clearValue.color.float32[3] = VK_PassIsPostScene( vk.renderPassIndex ) ? 0.0f : 1.0f;
	attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;

	get_scissor_rect( &clear_rect.rect );
	if ( !clear_rect.rect.extent.width || !clear_rect.rect.extent.height ) {
		return;
	}
	clear_rect.baseArrayLayer = 0;
	clear_rect.layerCount = 1;

	qvkCmdClearAttachments( vk.cmd->command_buffer, 1, &attachment, 1, &clear_rect );
}


void vk_clear_depth( qboolean clear_stencil ) {

	VkClearAttachment attachment;
	VkClearRect clear_rect[1];

	if ( !vk.active )
		return;

	if ( vk_world.dirty_depth_attachment == 0 )
		return;

	attachment.colorAttachment = 0;
#ifdef USE_REVERSED_DEPTH
	attachment.clearValue.depthStencil.depth = 0.0f;
#else
	attachment.clearValue.depthStencil.depth = 1.0f;
#endif
	attachment.clearValue.depthStencil.stencil = 0;
	if ( clear_stencil && glConfig.stencilBits > 0 ) {
		attachment.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
	} else {
		attachment.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	}

	get_scissor_rect( &clear_rect[0].rect );
	if ( !clear_rect[0].rect.extent.width || !clear_rect[0].rect.extent.height ) {
		return;
	}
	clear_rect[0].baseArrayLayer = 0;
	clear_rect[0].layerCount = 1;

	qvkCmdClearAttachments( vk.cmd->command_buffer, 1, &attachment, 1, clear_rect );
}


void vk_update_mvp( const float *m ) {
	float push[16], eyes[2][16];
	uint32_t viewMask = VK_PassViewMask( vk.multiview, vk.renderPassIndex );
	qboolean arrayPass = viewMask != 0;
	/* A single-bit mask executes layer 0 alone and yields one mono image. */
	qboolean stereoPass = viewMask == 3;
	int eye;

	if ( m ) {
		Com_Memcpy( push, m, sizeof( push ) );
	} else if ( !(arrayPass && vk_hud_direct) ) {
		get_mvp_transform( push );
	}
	if ( vk.multiview ) {
		/* Mono source passes retain the array descriptor ABI but use identity
		 * view matrices: their ordinary camera transform is already in push. */
		Com_Memset( eyes, 0, sizeof( eyes ) );
		for ( eye = 0; eye < 2; eye++ ) {
			eyes[eye][0] = eyes[eye][5] = eyes[eye][10] = eyes[eye][15] = 1;
		}
		if ( arrayPass && vk_hud_direct ) {
			int savedEye = vk_hud_eye;
			for ( eye = 0; eye < 2; eye++ ) {
				vk_hud_eye = eye;
				get_mvp_transform( eyes[eye] );
			}
			vk_hud_eye = savedEye;
			Com_Memset( push, 0, sizeof( push ) );
			push[0] = push[5] = push[10] = push[15] = 1;
		} else if ( stereoPass && backEnd.projection2D && VK_XR_Drawing() && !VK_XR_ScopeNeedsBands() ) {
			/* Screen-space 2D lies on a plane a unit ahead of the head, so each
			 * eye reprojects that plane through its own cant and fov. */
			for ( eye = 0; eye < 2; eye++ ) {
				VK_XR_ScreenMatrix( eye, eyes[eye] );
			}
		} else if ( arrayPass && !backEnd.projection2D && backEnd.viewParms.xrMultiview ) {
			Com_Memcpy( eyes, backEnd.viewParms.eyeProjection, sizeof( eyes ) );
			if ( !VK_PassIsVRScreen( vk.renderPassIndex ) ) {
				for ( eye = 0; eye < 2; eye++ ) {
					VK_FlipProjectionY( backEnd.viewParms.eyeProjection[eye], eyes[eye] );
				}
			}
			if ( !m ) {
				Com_Memcpy( push, vk_world.modelview_transform, sizeof( push ) );
			}
		}
		if ( !vk.cmd->view_valid || memcmp( eyes, vk.cmd->view_matrices, sizeof( eyes ) ) ) {
			uint32_t offset = PAD( vk.cmd->vertex_buffer_offset, vk.uniform_alignment );
			if ( offset + sizeof( eyes ) > vk.geometry_buffer_size ) {
				vk.geometry_buffer_size_new = log2pad( offset + sizeof( eyes ), 1 );
				return;
			}
			Com_Memcpy( vk.cmd->vertex_buffer_ptr + offset, eyes, sizeof( eyes ) );
			Com_Memcpy( vk.cmd->view_matrices, eyes, sizeof( eyes ) );
			vk.cmd->vertex_buffer_offset = offset + sizeof( eyes );
			vk.cmd->view_offset = offset;
			vk.cmd->view_valid = qtrue;
			// The dynamic offset moved, so the uniform set must be bound again.
			vk.cmd->descriptor_set.current[VK_DESC_UNIFORM] = vk.cmd->uniform_descriptor;
			vk.cmd->descriptor_set.start = 0;
		}
	}
	qvkCmdPushConstants( vk.cmd->command_buffer, vk.pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
						 sizeof( push ), push );
	vk.stats.push_size += sizeof( push );
}


static VkBuffer shade_bufs[8];
static int bind_base;
static int bind_count;

static void vk_bind_index_attr( int index )
{
	if ( bind_base == -1 ) {
		bind_base = index;
		bind_count = 1;
	} else {
		bind_count = index - bind_base + 1;
	}
}


static void vk_bind_attr( int index, unsigned int item_size, const void *src ) {
	const uint32_t offset = PAD( vk.cmd->vertex_buffer_offset, 32 );
	const uint32_t size = tess.numVertexes * item_size;

	if ( offset + size > vk.geometry_buffer_size ) {
		// schedule geometry buffer resize
		vk.geometry_buffer_size_new = log2pad( offset + size, 1 );
	} else {
		vk.cmd->buf_offset[ index ] = offset;
		Com_Memcpy( vk.cmd->vertex_buffer_ptr + offset, src, size );
		vk.cmd->vertex_buffer_offset = (VkDeviceSize)offset + size;
	}

	vk_bind_index_attr( index );
}


uint32_t vk_tess_index( uint32_t numIndexes, const void *src ) {
	const uint32_t offset = vk.cmd->vertex_buffer_offset;
	const uint32_t size = numIndexes * sizeof( tess.indexes[0] );

	if ( offset + size > vk.geometry_buffer_size ) {
		// schedule geometry buffer resize
		vk.geometry_buffer_size_new = log2pad( offset + size, 1 );
		return ~0U;
	} else {
		Com_Memcpy( vk.cmd->vertex_buffer_ptr + offset, src, size );
		vk.cmd->vertex_buffer_offset = (VkDeviceSize)offset + size;
		return offset;
	}
}


void vk_bind_index_buffer( VkBuffer buffer, uint32_t offset )
{
	if ( vk.cmd->curr_index_buffer != buffer || vk.cmd->curr_index_offset != offset )
		qvkCmdBindIndexBuffer( vk.cmd->command_buffer, buffer, offset, VK_INDEX_TYPE_UINT32 );

	vk.cmd->curr_index_buffer = buffer;
	vk.cmd->curr_index_offset = offset;
}


#ifdef USE_VBO
void vk_draw_indexed( uint32_t indexCount, uint32_t firstIndex )
{
	qvkCmdDrawIndexed( vk.cmd->command_buffer, indexCount, 1, firstIndex, 0, 0 );
}
#endif


void vk_bind_index( void )
{
#ifdef USE_VBO
	if ( tess.vboIndex ) {
		vk.cmd->num_indexes = 0;
		//qvkCmdBindIndexBuffer( vk.cmd->command_buffer, vk.vbo.index_buffer, tess.shader->iboOffset, VK_INDEX_TYPE_UINT32 );
		return;
	}
#endif

	vk_bind_index_ext( tess.numIndexes, tess.indexes );
}


void vk_bind_index_ext( const int numIndexes, const uint32_t *indexes )
{
	uint32_t offset	= vk_tess_index( numIndexes, indexes );
	if ( offset != ~0U ) {
		vk_bind_index_buffer( vk.cmd->vertex_buffer, offset );
		vk.cmd->num_indexes = numIndexes;
	} else {
		// overflowed
		vk.cmd->num_indexes = 0;
	}
}


void vk_bind_geometry( uint32_t flags )
{
	//unsigned int size;
	bind_base = -1;
	bind_count = 0;

	if ( ( flags & ( TESS_XYZ | TESS_RGBA0 | TESS_ST0 | TESS_ST1 | TESS_ST2 | TESS_NNN | TESS_RGBA1 | TESS_RGBA2 ) ) == 0 )
		return;

#ifdef USE_VBO
	if ( tess.vboIndex ) {

		shade_bufs[0] = shade_bufs[1] = shade_bufs[2] = shade_bufs[3] = shade_bufs[4] = shade_bufs[5] = shade_bufs[6] = shade_bufs[7] = vk.vbo.vertex_buffer;

		if ( flags & TESS_XYZ ) {  // 0
			vk.cmd->vbo_offset[0] = tess.shader->vboOffset + 0;
			vk_bind_index_attr( 0 );
		}

		if ( flags & TESS_RGBA0 ) { // 1
			vk.cmd->vbo_offset[1] = tess.shader->stages[ tess.vboStage ]->rgb_offset[0];
			vk_bind_index_attr( 1 );
		}

		if ( flags & TESS_ST0 ) {  // 2
			vk.cmd->vbo_offset[2] = tess.shader->stages[ tess.vboStage ]->tex_offset[0];
			vk_bind_index_attr( 2 );
		}

		if ( flags & TESS_ST1 ) {  // 3
			vk.cmd->vbo_offset[3] = tess.shader->stages[ tess.vboStage ]->tex_offset[1];
			vk_bind_index_attr( 3 );
		}

		if ( flags & TESS_ST2 ) {  // 4
			vk.cmd->vbo_offset[4] = tess.shader->stages[ tess.vboStage ]->tex_offset[2];
			vk_bind_index_attr( 4 );
		}

		if ( flags & TESS_NNN ) { // 5
			vk.cmd->vbo_offset[5] = tess.shader->normalOffset;
			vk_bind_index_attr( 5 );
		}

		if ( flags & TESS_RGBA1 ) { // 6
			vk.cmd->vbo_offset[6] = tess.shader->stages[ tess.vboStage ]->rgb_offset[1];
			vk_bind_index_attr( 6 );
		}

		if ( flags & TESS_RGBA2 ) { // 7
			vk.cmd->vbo_offset[7] = tess.shader->stages[ tess.vboStage ]->rgb_offset[2];
			vk_bind_index_attr( 7 );
		}

		qvkCmdBindVertexBuffers( vk.cmd->command_buffer, bind_base, bind_count, shade_bufs, vk.cmd->vbo_offset + bind_base );

	} else
#endif // USE_VBO
	{
		shade_bufs[0] = shade_bufs[1] = shade_bufs[2] = shade_bufs[3] = shade_bufs[4] = shade_bufs[5] = shade_bufs[6] = shade_bufs[7] = vk.cmd->vertex_buffer;

		if ( flags & TESS_XYZ ) {
			vk_bind_attr(0, sizeof(tess.xyz[0]), &tess.xyz[0]);
		}

		if ( flags & TESS_RGBA0 ) {
			vk_bind_attr(1, sizeof( color4ub_t ), tess.svars.colors[0][0].rgba);
		}

		if ( flags & TESS_ST0 ) {
			vk_bind_attr(2, sizeof( vec2_t ), tess.svars.texcoordPtr[0]);
		}

		if ( flags & TESS_ST1 ) {
			vk_bind_attr(3, sizeof( vec2_t ), tess.svars.texcoordPtr[1]);
		}

		if ( flags & TESS_ST2 ) {
			vk_bind_attr(4, sizeof( vec2_t ), tess.svars.texcoordPtr[2]);
		}

		// binding 5; must follow the lower-numbered binds because vk_bind_attr sets
		// bind_count from the last (highest) index it sees, not the max of all of them
		if ( flags & TESS_OVERBRIGHT ) {
			vk_bind_attr( 5, sizeof( float ), tess.svars.overbright );
		}

		if ( flags & TESS_NNN ) {
			vk_bind_attr(5, sizeof(tess.normal[0]), tess.normal);
		}

		if ( flags & TESS_RGBA1 ) {
			vk_bind_attr(6, sizeof( color4ub_t ), tess.svars.colors[1][0].rgba);
		}

		if ( flags & TESS_RGBA2 ) {
			vk_bind_attr(7, sizeof( color4ub_t ), tess.svars.colors[2][0].rgba);
		}

		qvkCmdBindVertexBuffers( vk.cmd->command_buffer, bind_base, bind_count, shade_bufs, vk.cmd->buf_offset + bind_base );
	}
}


void vk_bind_lighting( int stage, int bundle )
{
	bind_base = -1;
	bind_count = 0;

#ifdef USE_VBO
	if ( tess.vboIndex ) {

		shade_bufs[0] = shade_bufs[1] = shade_bufs[2] = vk.vbo.vertex_buffer;

		vk.cmd->vbo_offset[0] = tess.shader->vboOffset + 0;
		vk.cmd->vbo_offset[1] = tess.shader->stages[ stage ]->tex_offset[ bundle ];
		vk.cmd->vbo_offset[2] = tess.shader->normalOffset;

		qvkCmdBindVertexBuffers( vk.cmd->command_buffer, 0, 3, shade_bufs, vk.cmd->vbo_offset + 0 );

	}
	else
#endif // USE_VBO
	{
		shade_bufs[0] = shade_bufs[1] = shade_bufs[2] = vk.cmd->vertex_buffer;

		vk_bind_attr( 0, sizeof( tess.xyz[0] ), &tess.xyz[0] );
		vk_bind_attr( 1, sizeof( vec2_t ), tess.svars.texcoordPtr[ bundle ] );
		vk_bind_attr( 2, sizeof( tess.normal[0] ), tess.normal );

		qvkCmdBindVertexBuffers( vk.cmd->command_buffer, bind_base, bind_count, shade_bufs, vk.cmd->buf_offset + bind_base );
	}
}


void vk_reset_descriptor( int index )
{
	vk.cmd->descriptor_set.current[ index ] = VK_NULL_HANDLE;
}


void vk_update_descriptor( int index, VkDescriptorSet descriptor )
{
	if ( vk.cmd->descriptor_set.current[ index ] != descriptor ) {
		vk.cmd->descriptor_set.start = ( index < vk.cmd->descriptor_set.start ) ? index : vk.cmd->descriptor_set.start;
		vk.cmd->descriptor_set.end = ( index > vk.cmd->descriptor_set.end ) ? index : vk.cmd->descriptor_set.end;
	}
	vk.cmd->descriptor_set.current[ index ] = descriptor;
}


void vk_update_descriptor_offset( int index, uint32_t offset )
{
	vk.cmd->descriptor_set.offset[ index ] = offset;
}


void vk_bind_descriptor_sets( void )
{
	uint32_t offsets[2], offset_count;
	uint32_t start, end, count, i;

	start = vk.cmd->descriptor_set.start;
	if ( start == ~0U )
		return;

	end = vk.cmd->descriptor_set.end;

	offset_count = 0;
	if ( /*start == VK_DESC_STORAGE || */ start == VK_DESC_UNIFORM ) { // uniform offset or storage offset
		offsets[ offset_count++ ] = vk.cmd->descriptor_set.offset[ start ];
		if ( vk.multiview )
			offsets[offset_count++] = vk.cmd->view_offset;
	}

	count = end - start + 1;

	// fill NULL descriptor gaps
	for ( i = start + 1; i < end; i++ ) {
		if ( vk.cmd->descriptor_set.current[i] == VK_NULL_HANDLE ) {
			vk.cmd->descriptor_set.current[i] = tr.whiteImage->descriptor;
		}
	}

	qvkCmdBindDescriptorSets( vk.cmd->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.pipeline_layout, start, count, vk.cmd->descriptor_set.current + start, offset_count, offsets );

	vk.cmd->descriptor_set.end = 0;
	vk.cmd->descriptor_set.start = ~0U;
}


void vk_bind_pipeline( uint32_t pipeline ) {
	VkPipeline vkpipe;

	vkpipe = vk_gen_pipeline( pipeline );

	if ( vkpipe != vk.cmd->last_pipeline ) {
		qvkCmdBindPipeline( vk.cmd->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vkpipe );
		vk.cmd->last_pipeline = vkpipe;
	}

	vk_world.dirty_depth_attachment |= ( vk.pipelines[ pipeline ].def.state_bits & GLS_DEPTHMASK_TRUE );
}

/* FOVEATION DEBUG */
/* Tints by gl_FragSizeEXT, the fragment the density map produced, over the bins and the gaze. */
static void vk_draw_density_debug( void ) {
	const vkFdmGeometry_t *g = &vk_fdm.geometry;
	VkViewport viewport;
	VkRect2D scissor;
	float push[12];
	uint32_t eye;
	if ( !vk_fdm.debugLayout ) {
		VkPushConstantRange range = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof( push )};
		VkPipelineLayoutCreateInfo layout = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		layout.pushConstantRangeCount = 1;
		layout.pPushConstantRanges = &range;
		VK_CHECK( qvkCreatePipelineLayout( vk.device, &layout, NULL, &vk_fdm.debugLayout ) );
	}
	if ( !vk_foveation.debugPipeline ) {
		VkShaderModule fragment = SHADER_MODULE( foveationdensity_frag_spv );
		VkPipelineShaderStageCreateInfo stages[2];
		VkPipelineVertexInputStateCreateInfo vertex = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
		VkPipelineInputAssemblyStateCreateInfo assembly = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
			.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP};
		VkPipelineViewportStateCreateInfo view = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
		VkPipelineRasterizationStateCreateInfo raster = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
			.polygonMode = VK_POLYGON_MODE_FILL,
			.cullMode = VK_CULL_MODE_NONE,
			.frontFace = VK_FRONT_FACE_CLOCKWISE,
			.lineWidth = 1};
		VkPipelineMultisampleStateCreateInfo samples = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = vkSamples};
		VkPipelineDepthStencilStateCreateInfo depth = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
		VkPipelineColorBlendAttachmentState colors[2] = {{0}, {0}};
		VkPipelineColorBlendStateCreateInfo blend = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
													 .attachmentCount = vk.hdrActive ? 2 : 1,
													 .pAttachments = colors};
		VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
		VkPipelineDynamicStateCreateInfo dynamic = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
													.dynamicStateCount = ARRAY_LEN( states ),
													.pDynamicStates = states};
		VkGraphicsPipelineCreateInfo info = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
		VkResult result;
		colors[0].blendEnable = VK_TRUE;
		colors[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		colors[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		colors[0].colorBlendOp = colors[0].alphaBlendOp = VK_BLEND_OP_ADD;
		colors[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
		colors[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		colors[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
		set_shader_stage_desc( stages, VK_SHADER_STAGE_VERTEX_BIT, vk.modules.gamma_vs, "main" );
		set_shader_stage_desc( stages + 1, VK_SHADER_STAGE_FRAGMENT_BIT, fragment, "main" );
		info.stageCount = 2;
		info.pStages = stages;
		info.pVertexInputState = &vertex;
		info.pInputAssemblyState = &assembly;
		info.pViewportState = &view;
		info.pRasterizationState = &raster;
		info.pMultisampleState = &samples;
		info.pDepthStencilState = &depth;
		info.pColorBlendState = &blend;
		info.pDynamicState = &dynamic;
		info.layout = vk_fdm.debugLayout;
		info.renderPass = vk.render_pass.main;
		info.basePipelineIndex = -1;
		result = qvkCreateGraphicsPipelines( vk.device, VK_NULL_HANDLE, 1, &info, NULL, &vk_foveation.debugPipeline );
		qvkDestroyShaderModule( vk.device, fragment, NULL );
		if ( result != VK_SUCCESS ) {
			ri.Printf( PRINT_WARNING, "Foveation debug pipeline unavailable (%d)\n", result );
			ri.Cvar_Set( "r_foveationDebug", "0" );
			return;
		}
	}
	// offsets carry the bins' corners with the gaze
	Com_Memset( push, 0, sizeof( push ) );
	for ( eye = 0; eye < 2; eye++ ) {
		push[eye * 2 + 0] = ( vk_fdm.center[eye][0] + 1.0f ) * 0.5f * (float)g->width;
		push[eye * 2 + 1] = ( vk_fdm.center[eye][1] + 1.0f ) * 0.5f * (float)g->height;
		push[4 + eye * 2 + 0] = (float)vk_fdm.offset[eye][0];
		push[4 + eye * 2 + 1] = (float)vk_fdm.offset[eye][1];
	}
	push[8] = (float)g->tileWidth;
	push[9] = (float)g->tileHeight;
	push[10] = vk_fdm.strength > 0 ? 1.0f : 0.0f;
	get_viewport( &viewport, DEPTH_RANGE_NORMAL );
	get_scissor_rect( &scissor );
	qvkCmdBindPipeline( vk.cmd->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_foveation.debugPipeline );
	qvkCmdPushConstants( vk.cmd->command_buffer, vk_fdm.debugLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof( push ),
						 push );
	qvkCmdSetViewport( vk.cmd->command_buffer, 0, 1, &viewport );
	qvkCmdSetScissor( vk.cmd->command_buffer, 0, 1, &scissor );
	qvkCmdDraw( vk.cmd->command_buffer, 4, 1, 0, 0 );
	vk.cmd->last_pipeline = VK_NULL_HANDLE;
	vk.cmd->depth_range = DEPTH_RANGE_COUNT;
}

void vk_foveation_hud_rect( const float rect[2][4], const qboolean valid[2] ) {
	int e;
	for ( e = 0; e < 2; e++ ) {
		if ( !valid[e] )
			continue;
		if ( !vk_fdm.hudValid[e] ) {
			Com_Memcpy( vk_fdm.hud[e], rect[e], sizeof( vk_fdm.hud[e] ) );
			vk_fdm.hudValid[e] = qtrue;
			continue;
		}
		vk_fdm.hud[e][0] = MIN( vk_fdm.hud[e][0], rect[e][0] );
		vk_fdm.hud[e][1] = MIN( vk_fdm.hud[e][1], rect[e][1] );
		vk_fdm.hud[e][2] = MAX( vk_fdm.hud[e][2], rect[e][2] );
		vk_fdm.hud[e][3] = MAX( vk_fdm.hud[e][3], rect[e][3] );
	}
}

/* Fragment edge the density map asks for at an eye's GL-style NDC position; 0 while no map foveates. */
int vk_foveation_block_at( int eye, float ndcX, float ndcY ) {
	const vkFdmGeometry_t *g = &vk_fdm.geometry;
	if ( !vk_fdm_active() || vk_fdm.strength <= 0 || !vk_foveation.uploaded )
		return 0;
	eye = eye ? 1 : 0;
	return VK_FdmBlockAtNdc( vk_foveation.current, g, eye, ndcX, ndcY, vk_fdm.offset[eye] );
}

/* A fullscreen tint samples gl_ShadingRateEXT in the scene pass, where the
 * attachment is bound. It is created lazily and shares the target lifetime. */
void vk_draw_foveation_debug( void ) {
	VkViewport viewport;
	VkRect2D scissor;
	if ( !vk_foveation.image || !VK_XR_Drawing() || VK_XR_Screen() || vk_hud.recording ||
		backEnd.projection2D || vk.renderPassIndex != RENDER_PASS_MAIN ||
		!r_foveationDebugCvar->integer )
		return;
	if ( vk_fdm_active() ) {
		if ( vk_fdm.passOpen )
			vk_draw_density_debug();
		return;
	}
	if ( !vk_foveation.debugPipeline ) {
		VkShaderModule fragment = SHADER_MODULE( foveationdebug_frag_spv );
		VkPipelineShaderStageCreateInfo stages[2];
		VkPipelineVertexInputStateCreateInfo vertex = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
		VkPipelineInputAssemblyStateCreateInfo assembly = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
			.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP};
		VkPipelineViewportStateCreateInfo view = {.sType =
													  VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
												  .viewportCount = 1,
												  .scissorCount = 1};
		VkPipelineRasterizationStateCreateInfo raster = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
			.polygonMode = VK_POLYGON_MODE_FILL,
			.cullMode = VK_CULL_MODE_NONE,
			.frontFace = VK_FRONT_FACE_CLOCKWISE,
			.lineWidth = 1};
		VkPipelineMultisampleStateCreateInfo samples = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
			.rasterizationSamples = vkSamples};
		VkPipelineDepthStencilStateCreateInfo depth = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
		VkPipelineColorBlendAttachmentState colors[2] = {{0}, {0}};
		VkPipelineColorBlendStateCreateInfo blend = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
			.attachmentCount = vk.hdrActive ? 2 : 1,
			.pAttachments = colors};
		VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
								   VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR};
		VkPipelineDynamicStateCreateInfo dynamic = {.sType =
														VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
													.dynamicStateCount = ARRAY_LEN( states ),
													.pDynamicStates = states};
		VkGraphicsPipelineCreateInfo info = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
		VkResult result;
		colors[0].blendEnable = VK_TRUE;
		colors[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		colors[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		colors[0].colorBlendOp = colors[0].alphaBlendOp = VK_BLEND_OP_ADD;
		colors[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
		colors[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		colors[0].colorWriteMask =
			VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
		set_shader_stage_desc( stages, VK_SHADER_STAGE_VERTEX_BIT, vk.modules.gamma_vs, "main" );
		set_shader_stage_desc( stages + 1, VK_SHADER_STAGE_FRAGMENT_BIT, fragment, "main" );
		info.stageCount = 2;
		info.pStages = stages;
		info.pVertexInputState = &vertex;
		info.pInputAssemblyState = &assembly;
		info.pViewportState = &view;
		info.pRasterizationState = &raster;
		info.pMultisampleState = &samples;
		info.pDepthStencilState = &depth;
		info.pColorBlendState = &blend;
		info.pDynamicState = &dynamic;
		info.layout = vk.pipeline_layout;
		info.renderPass = vk.render_pass.main;
		info.basePipelineIndex = -1;
		result = qvkCreateGraphicsPipelines( vk.device, VK_NULL_HANDLE, 1, &info, NULL,
											 &vk_foveation.debugPipeline );
		qvkDestroyShaderModule( vk.device, fragment, NULL );
		if ( result != VK_SUCCESS ) {
			ri.Printf( PRINT_WARNING, "Foveation debug pipeline unavailable (%d)\n", result );
			ri.Cvar_Set( "r_foveationDebug", "0" );
			return;
		}
	}
	get_viewport( &viewport, DEPTH_RANGE_NORMAL );
	get_scissor_rect( &scissor );
	qvkCmdBindPipeline( vk.cmd->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_foveation.debugPipeline );
	qvkCmdSetViewport( vk.cmd->command_buffer, 0, 1, &viewport );
	qvkCmdSetScissor( vk.cmd->command_buffer, 0, 1, &scissor );
	vk_foveation_rate( qtrue );
	qvkCmdDraw( vk.cmd->command_buffer, 4, 1, 0, 0 );
	vk.cmd->last_pipeline = VK_NULL_HANDLE;
	vk.cmd->depth_range = DEPTH_RANGE_COUNT;
}

static void vk_update_depth_range( Vk_Depth_Range depth_range )
{
	vk_foveation_draw();
	if ( vk.cmd->depth_range != depth_range ) {
		VkRect2D scissor_rect;
		VkViewport viewport;

		vk.cmd->depth_range = depth_range;

		get_scissor_rect( &scissor_rect );

		/* Command-buffer switches invalidate dynamic viewport and scissor state. */
		qvkCmdSetScissor( vk.cmd->command_buffer, 0, 1, &scissor_rect );

		get_viewport( &viewport, depth_range );
		qvkCmdSetViewport( vk.cmd->command_buffer, 0, 1, &viewport );
	}
}


void vk_draw_geometry( Vk_Depth_Range depth_range, qboolean indexed ) {

	if ( vk.geometry_buffer_size_new ) {
		// geometry buffer overflow happened this frame
		return;
	}

	vk_bind_descriptor_sets();

	// configure pipeline's dynamic state
	vk_update_depth_range( depth_range );

	qvkCmdPushConstants( vk.cmd->command_buffer, vk.pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
		64, sizeof( float ), &vk.cmd->emissive_factor );

	// issue draw call(s)
#ifdef USE_VBO
	if ( tess.vboIndex )
		VBO_RenderIBOItems();
	else
#endif
	if ( indexed ) {
		qvkCmdDrawIndexed( vk.cmd->command_buffer, vk.cmd->num_indexes, 1, 0, 0, 0 );
	} else {
		qvkCmdDraw( vk.cmd->command_buffer, tess.numVertexes, 1, 0, 0 );
	}
}


void vk_draw_dot( uint32_t storage_offset )
{
	if ( vk.geometry_buffer_size_new ) {
		// geometry buffer overflow happened this frame
		return;
	}

	{
		uint32_t offsets[2] = {storage_offset, vk.cmd->view_offset};
		VkDescriptorSet descriptor = vk.multiview ? vk.cmd->storage_descriptor : vk.storage.descriptor;
		qvkCmdBindDescriptorSets( vk.cmd->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
								  vk.pipeline_layout_storage, VK_DESC_STORAGE, 1, &descriptor,
								  vk.multiview ? 2 : 1, offsets );
		// The storage set displaced the uniform set at slot 0.
		vk.cmd->view_valid = qfalse;
	}

	// configure pipeline's dynamic state
	vk_update_depth_range( DEPTH_RANGE_NORMAL );

	qvkCmdDraw( vk.cmd->command_buffer, tess.numVertexes, 1, 0, 0 );
}


static void vk_begin_render_pass( VkRenderPass renderPass, VkFramebuffer frameBuffer, qboolean clearValues, uint32_t width, uint32_t height )
{
	VkRenderPassBeginInfo render_pass_begin_info;
	VkClearValue clear_values[5];

	// Begin render pass.

	render_pass_begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	render_pass_begin_info.pNext = NULL;
	render_pass_begin_info.renderPass = renderPass;
	render_pass_begin_info.framebuffer = frameBuffer;
	render_pass_begin_info.renderArea.offset.x = 0;
	render_pass_begin_info.renderArea.offset.y = 0;
	render_pass_begin_info.renderArea.extent.width = width;
	render_pass_begin_info.renderArea.extent.height = height;

	if ( clearValues ) {
		// attachments layout:
		// [0] - resolve/color/presentation
		// [1] - depth/stencil
		// [2] - multisampled color, optional
		// [2|3] - resolved emissive, optional (non-msaa: 2, msaa: 3)
		// [4] - multisampled emissive, optional (msaa only)
		Com_Memset( clear_values, 0, sizeof( clear_values ) );
		// the scene image's alpha starts at full transmittance for the post-scene draws
		clear_values[0].color.float32[3] = clear_values[2].color.float32[3] = 1.0f;
#ifndef USE_REVERSED_DEPTH
		clear_values[1].depthStencil.depth = 1.0;
#endif
		if ( vk.hdrActive )
			render_pass_begin_info.clearValueCount = vk.msaaActive ? 5 : 3;
		else
			render_pass_begin_info.clearValueCount = vk.msaaActive ? 3 : 2;
		render_pass_begin_info.pClearValues = clear_values;

		vk_world.dirty_depth_attachment = 0;
	} else {
		render_pass_begin_info.clearValueCount = 0;
		render_pass_begin_info.pClearValues = NULL;
	}

	qvkCmdBeginRenderPass( vk.cmd->command_buffer, &render_pass_begin_info, VK_SUBPASS_CONTENTS_INLINE );
	// a frame abandoned inside the scene pass must not end this pass with density map offsets
	vk_fdm.passOpen = qfalse;
	/* Postprocess passes bind static-rate pipelines; do not reuse draw state
	 * from an earlier scene pass when returning to dynamic scene pipelines. */
	vk_foveation_invalidate_rate();

	vk.cmd->last_pipeline = VK_NULL_HANDLE;
	vk.cmd->depth_range = DEPTH_RANGE_COUNT;
}


void vk_begin_main_render_pass( void )
{
	qboolean mono = vk_mono_source();
	VkFramebuffer frameBuffer;

	if ( vk.xrDirect ) {
		struct vkXRDirectTarget_s *t = vk_xr_direct_target();
		frameBuffer = mono ? t->mono : t->main;
	} else {
		frameBuffer = mono ? vk.mono.framebuffer.main : vk.framebuffers.main[vk.cmd->swapchain_image_index];
	}

	vk.renderPassIndex = mono ? RENDER_PASS_MONO_MAIN : RENDER_PASS_MAIN;

	vk.renderWidth = vk.sceneWidth;
	vk.renderHeight = vk.sceneHeight;

	//vk.renderScaleX = (float)vk.renderWidth / (float)glConfig.vidWidth;
	//vk.renderScaleY = (float)vk.renderHeight / (float)glConfig.vidHeight;
	vk.renderScaleX = (float)vk.renderWidth / glConfig.vidWidth;
	vk.renderScaleY = (float)vk.renderHeight / glConfig.vidHeight;

	vk_begin_render_pass( mono ? vk.mono.pass.main : vk.render_pass.main, frameBuffer, qtrue, vk.renderWidth,
						  vk.renderHeight );
	vk_fdm.passOpen = !mono && vk_fdm_active();
}


static void vk_begin_post_scene_render_pass( void )
{
	const qboolean mono = vk_mono_source();

	vk.renderPassIndex = mono ? RENDER_PASS_MONO_POST_SCENE : RENDER_PASS_POST_SCENE;

	vk.renderWidth = vk.sceneWidth;
	vk.renderHeight = vk.sceneHeight;

	vk.renderScaleX = (float)vk.renderWidth / glConfig.vidWidth;
	vk.renderScaleY = (float)vk.renderHeight / glConfig.vidHeight;

	vk_begin_render_pass( mono ? vk.mono.pass.post_scene : vk.render_pass.post_scene,
						  mono ? vk.mono.framebuffer.post_scene : vk.framebuffers.post_scene, qtrue,
						  vk.renderWidth, vk.renderHeight );
}


void vk_begin_blur_render_pass( uint32_t index )
{
	qboolean mono = vk_mono_source();
	VkFramebuffer frameBuffer = mono ? vk.mono.framebuffer.blur[index] : vk.framebuffers.blur[index];

	//vk.renderPassIndex = RENDER_PASS_BLOOM_EXTRACT; // doesn't matter, we will use dedicated pipelines

	vk.renderWidth = vk_bloom_width() / (2 << (index / 2));
	vk.renderHeight = vk_bloom_height() / (2 << (index / 2));

	//vk.renderScaleX = (float)vk.renderWidth / (float)glConfig.vidWidth;
	//vk.renderScaleY = (float)vk.renderHeight / (float)glConfig.vidHeight;
	vk.renderScaleX = vk.renderScaleY = 1.0f;

	vk_begin_render_pass( mono ? vk.mono.pass.blur[index] : vk.render_pass.blur[index], frameBuffer, qfalse,
						  vk.renderWidth, vk.renderHeight );
}


static void vk_begin_screenmap_render_pass( void )
{
	VkFramebuffer frameBuffer = vk.framebuffers.screenmap;

	vk.renderPassIndex = RENDER_PASS_SCREENMAP;

	vk.renderWidth = vk.screenMapWidth;
	vk.renderHeight = vk.screenMapHeight;

	vk.renderScaleX = (float)vk.renderWidth / (float)glConfig.vidWidth;
	vk.renderScaleY = (float)vk.renderHeight / (float)glConfig.vidHeight;

	vk_begin_render_pass( vk.render_pass.screenmap, frameBuffer, qtrue, vk.renderWidth, vk.renderHeight );
}


void vk_end_render_pass( void )
{
	// the stereo scene pass ends sliding each eye's density map onto its gaze
	if ( vk_fdm.passOpen && vk_fdm_offsets() ) {
		VkSubpassFragmentDensityMapOffsetEndInfoQCOM offsets = {
			VK_STRUCTURE_TYPE_SUBPASS_FRAGMENT_DENSITY_MAP_OFFSET_END_INFO_QCOM};
		VkSubpassEndInfo end = {VK_STRUCTURE_TYPE_SUBPASS_END_INFO};
		VkOffset2D eyes[2];
		uint32_t eye;
		for ( eye = 0; eye < 2; eye++ ) {
			eyes[eye].x = vk_fdm.offset[eye][0];
			eyes[eye].y = vk_fdm.offset[eye][1];
		}
		offsets.fragmentDensityOffsetCount = 2;
		offsets.pFragmentDensityOffsets = eyes;
		end.pNext = &offsets;
		vk_fdm_end_pass( vk.cmd->command_buffer, &end );
	} else {
		qvkCmdEndRenderPass( vk.cmd->command_buffer );
	}
	vk_fdm.passOpen = qfalse;

//	vk.renderPassIndex = RENDER_PASS_MAIN;
}


static qboolean vk_find_screenmap_drawsurfs( void )
{
	const void *curCmd = &backEndData->commands.cmds;
	const drawBufferCommand_t *db_cmd;
	const drawSurfsCommand_t *ds_cmd;

	for ( ;; ) {
		curCmd = PADP( curCmd, sizeof(void *) );
		switch ( *(const int *)curCmd ) {
			case RC_DRAW_BUFFER:
				db_cmd = (const drawBufferCommand_t *)curCmd;
				curCmd = (const void *)(db_cmd + 1);
				break;
			case RC_DRAW_SURFS:
				ds_cmd = (const drawSurfsCommand_t *)curCmd;
				return ds_cmd->refdef.needScreenMap;
			default:
				return qfalse;
		}
	}
}


#ifndef UINT64_MAX
#define UINT64_MAX 0xFFFFFFFFFFFFFFFFULL
#endif

/* vk_flares.c owns generation-tagged probe metadata for each fenced slot. */
void RB_BeginFlareFrame( qboolean completed );

static void vk_gpu_segments_add( const uint64_t *main, const uint64_t *marks, int window ) {
	uint64_t prev = main[0];
	uint32_t s;
	// frames that skipped bloom (menus, loading) would fold the scene into another pass's median
	if ( vk.fboActive && r_bloom->integer && !marks[1] )
		return;
	for ( s = 0; s < ARRAY_LEN( gpuSegments ); s++ ) {
		const unsigned q = gpuSegments[s].query;
		const uint64_t *slot = q >= 6 ? &marks[( q - 6 ) * 2] : &main[q * 2];
		if ( !slot[1] || gpuSegmentCount[s] >= window )
			continue;
		gpuSegmentSamples[s][gpuSegmentCount[s]++] = (float)( (double)VK_GPUTimeDelta( prev, slot[0], vk.timestampValidBits ) *
															  vk.deviceLimits.timestampPeriod * 1e-6 );
		prev = slot[0];
	}
}

static void vk_gpu_segments_print( int window ) {
	const qboolean sceneSplit = gpuSegmentCount[0] > 0;
	char line[512];
	uint32_t s;
	Com_sprintf( line, sizeof( line ), "GPU passes over %i frames (median ms):", window );
	for ( s = 0; s < ARRAY_LEN( gpuSegments ); s++ ) {
		const int n = gpuSegmentCount[s];
		if ( !n )
			continue;
		qsort( gpuSegmentSamples[s], n, sizeof( float ), VK_GPUTimeCompare );
		Q_strcat( line, sizeof( line ),
				  va( " %s%s %.2f,", !sceneSplit && gpuSegments[s].query == 2 ? "scene+" : "",
					  gpuSegments[s].name, gpuSegmentSamples[s][VK_GPUTimePercentile( n, 50 )] ) );
		gpuSegmentCount[s] = 0;
	}
	line[strlen( line ) - 1] = '\n';
	ri.Printf( PRINT_ALL, "%s", line );
}

/* Query pairs: full main commands, desktop mirror, separately submitted HUD; 6..9 split main by pass.
 * All stamps are outside render passes (multiview replicates in-pass queries).
 * Results are consumed only after the slot's existing completion fence. */
static void vk_gpu_time_collect( qboolean completed ) {
	uint64_t queries[12], marks[8];
	VkResult res;
	double ticks;
	int window = r_gpuTimeLog->integer;
	if ( window < 0 )
		window = 0;
	if ( window > (int)ARRAY_LEN( gpuTimeSamples ) )
		window = ARRAY_LEN( gpuTimeSamples );
	if ( window != gpuTimeWindow || r_gpuTimeLog->modified ) {
		gpuTimeWindow = window;
		gpuTimeCount = 0;
		Com_Memset( gpuSegmentCount, 0, sizeof( gpuSegmentCount ) );
		++gpuTimeGeneration;
		r_gpuTimeLog->modified = qfalse;
	}
	if ( !vk.cmd->gpu_time_pending )
		return;
	vk.cmd->gpu_time_pending = qfalse;
	if ( !completed || !window || vk.cmd->gpu_time_generation != gpuTimeGeneration )
		return;
	Com_Memset( queries, 0, sizeof( queries ) );
	Com_Memset( marks, 0, sizeof( marks ) );
	/* Main-buffer stamps a frame never wrote stay reset, hence unavailable (VK_NOT_READY). */
	res = qvkGetQueryPoolResults( vk.device, gpuTimePool, vk.cmd_index * VK_GPU_TIME_QUERIES, 4,
								  8 * sizeof( uint64_t ), queries, 2 * sizeof( uint64_t ),
								  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT );
	if ( res != VK_SUCCESS && res != VK_NOT_READY )
		return;
	res = qvkGetQueryPoolResults( vk.device, gpuTimePool, vk.cmd_index * VK_GPU_TIME_QUERIES + 6, 4,
								  sizeof( marks ), marks, 2 * sizeof( uint64_t ),
								  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT );
	if ( res != VK_SUCCESS && res != VK_NOT_READY )
		return;
	if ( vk.cmd->hud_begun &&
		qvkGetQueryPoolResults( vk.device, gpuTimePool, vk.cmd_index * VK_GPU_TIME_QUERIES + 4, 2, 4 * sizeof( uint64_t ),
								queries + 8, 2 * sizeof( uint64_t ),
								VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT ) != VK_SUCCESS )
		return;
	if ( !VK_GPUTimeTicks( queries, vk.timestampValidBits, vk.cmd->gpu_time_mirror, vk.cmd->hud_begun, &ticks ) )
		return;
	gpuTimeSamples[gpuTimeCount++] = (float)(ticks * vk.deviceLimits.timestampPeriod * 1e-6);
	vk_gpu_segments_add( queries, marks, window );
	if ( gpuTimeCount == window ) {
		qsort( gpuTimeSamples, window, sizeof( float ), VK_GPUTimeCompare );
		ri.Printf( PRINT_ALL,
				   "GPU time over %i frames: frame median %.2f ms, p99 %.2f, max %.2f; (main including eye "
				   "output/copy, minus desktop mirror, plus HUD; excludes compositor)\n",
				   window, gpuTimeSamples[VK_GPUTimePercentile( window, 50 )],
				   gpuTimeSamples[VK_GPUTimePercentile( window, 99 )], gpuTimeSamples[window - 1] );
		gpuTimeCount = 0;
		vk_gpu_segments_print( window );
	}
}

static void vk_gpu_time_begin( void ) {
	vk.cmd->gpu_time_armed = vk.cmd->gpu_time_pending = vk.cmd->gpu_time_mirror = qfalse;
	if ( !gpuTimeWindow || !vk.timestampValidBits || !vk.deviceLimits.timestampComputeAndGraphics )
		return;
	if ( !gpuTimePool ) {
		VkQueryPoolCreateInfo desc;
		Com_Memset( &desc, 0, sizeof( desc ) );
		desc.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
		desc.queryType = VK_QUERY_TYPE_TIMESTAMP;
		desc.queryCount = NUM_COMMAND_BUFFERS * VK_GPU_TIME_QUERIES;
		VK_CHECK( qvkCreateQueryPool( vk.device, &desc, NULL, &gpuTimePool ) );
	}
	vk.cmd->gpu_time_armed = qtrue;
	vk.cmd->gpu_time_generation = gpuTimeGeneration;
	/* The HUD reset belongs in its own buffer: HUD executes before main. */
	qvkCmdResetQueryPool( vk.cmd->command_buffer, gpuTimePool, vk.cmd_index * VK_GPU_TIME_QUERIES, 4 );
	qvkCmdResetQueryPool( vk.cmd->command_buffer, gpuTimePool, vk.cmd_index * VK_GPU_TIME_QUERIES + 6, 4 );
	qvkCmdWriteTimestamp( vk.cmd->command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, gpuTimePool,
						  vk.cmd_index * VK_GPU_TIME_QUERIES );
}

static void vk_gpu_time_stamp( VkCommandBuffer command, unsigned query ) {
	if ( !vk.cmd->gpu_time_armed )
		return;
	if ( query == 4 ) {
		qvkCmdResetQueryPool( command, gpuTimePool, vk.cmd_index * VK_GPU_TIME_QUERIES + 4, 2 );
	}
	qvkCmdWriteTimestamp(
		command, query == 4 ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
		gpuTimePool, vk.cmd_index * VK_GPU_TIME_QUERIES + query );
}

void vk_begin_frame( void )
{
	VkCommandBufferBeginInfo begin_info;
	VkResult res;
	qboolean flareResultsCompleted = qfalse;

	if ( vk.frame_count++ ) // might happen during stereo rendering
		return;
	RB_ClearDeferredHud();

#ifdef USE_UPLOAD_QUEUE
	vk_flush_staging_buffer( qtrue );
#endif

	vk.cmd = &vk.tess[ vk.cmd_index ];

	if ( vk.cmd->waitForFence ) {
		vk.cmd->waitForFence = qfalse;
		res = qvkWaitForFences( vk.device, 1, &vk.cmd->rendering_finished_fence, VK_FALSE, 1e10 );
		flareResultsCompleted = (res == VK_SUCCESS);
		if ( res != VK_SUCCESS ) {
			if ( res == VK_ERROR_DEVICE_LOST ) {
				// silently discard previous command buffer
				ri.Printf( PRINT_WARNING, "Vulkan: %s returned %s", "vkWaitForFences", vk_result_string( res ) );
			}
			else {
				ri.Error( ERR_FATAL, "Vulkan: %s returned %s", "vkWaitForFences", vk_result_string( res ) );
			}
		}
		VK_CHECK( qvkResetFences( vk.device, 1, &vk.cmd->rendering_finished_fence ) );
	}
	RB_BeginFlareFrame( flareResultsCompleted );
	vk_gpu_time_collect( flareResultsCompleted );
	vk.cmd->hud_begun = qfalse;

	if ( !ri.CL_IsMinimized() && !vk.cmd->swapchain_image_acquired ) {
		qboolean retry = qfalse;
_retry:
		res = qvkAcquireNextImageKHR( vk.device, vk.swapchain, 1 * 1000000000ULL, vk.cmd->image_acquired, VK_NULL_HANDLE, &vk.cmd->swapchain_image_index );
		// when running via RDP: "Application has already acquired the maximum number of images (0x2)"
		// probably caused by "device lost" errors
		if ( res < 0 ) {
			if ( res == VK_ERROR_OUT_OF_DATE_KHR && retry == qfalse ) {
				// swapchain re-creation needed
				retry = qtrue;
				vk_restart_swapchain( __func__, res );
				goto _retry;
			} else {
				ri.Error( ERR_FATAL, "vkAcquireNextImageKHR returned %s", vk_result_string( res ) );
			}
		}
		vk.cmd->swapchain_image_acquired = qtrue;
	}

	begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin_info.pNext = NULL;
	begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	begin_info.pInheritanceInfo = NULL;

	VK_CHECK( qvkBeginCommandBuffer( vk.cmd->command_buffer, &begin_info ) );
	vk_gpu_time_begin();
	vk_foveation_upload();

	if ( vk.swapchain_images_inited[ vk.cmd->swapchain_image_index ] == qfalse ) {
		// perform initial swapchain image layout transition
		vk.swapchain_images_inited[ vk.cmd->swapchain_image_index ] = qtrue;
		record_image_layout_transition( vk.cmd->command_buffer, vk.swapchain_images[ vk.cmd->swapchain_image_index ],
			VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, vk.initSwapchainLayout,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0 );
	}

	// Ensure visibility of geometry buffers writes.
	//record_buffer_memory_barrier( vk.cmd->command_buffer, vk.cmd->vertex_buffer, vk.geometry_buffer_size, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT );

#if 0
	// add explicit layout transition dependency
	if ( vk.fboActive ) {
		record_image_layout_transition( vk.cmd->command_buffer, vk.color_image, VK_IMAGE_ASPECT_COLOR_BIT,
			VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 0 );
	} else {
		record_image_layout_transition( vk.cmd->command_buffer, vk.swapchain_images[ vk.swapchain_image_index ], VK_IMAGE_ASPECT_COLOR_BIT,
			VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 0, 0 );
	}
#endif

	if ( vk.cmd->vertex_buffer_offset > vk.stats.vertex_buffer_max ) {
		vk.stats.vertex_buffer_max = vk.cmd->vertex_buffer_offset;
	}

	if ( vk.stats.push_size > vk.stats.push_size_max ) {
		vk.stats.push_size_max = vk.stats.push_size;
	}

	vk.cmd->last_pipeline = VK_NULL_HANDLE;

	backEnd.screenMapDone = qfalse;
	vk.mono.sourceActive = vk.multiview && VK_XR_Screen() != NULL;
	vk.postFlow = ( !vk.fboActive || !vk.multiview || !vk.xr_output.pass ) ? VK_POST_FLOW_LEGACY
				  : vk.mono.sourceActive ? VK_POST_FLOW_SCREEN : VK_POST_FLOW_WORLD;
	vk.postOpen = qfalse;

	if ( vk_find_screenmap_drawsurfs() ) {
		vk_begin_screenmap_render_pass();
	} else {
		vk_begin_main_render_pass();
	}

	// dynamic vertex buffer layout
	vk.cmd->uniform_read_offset = 0;
	vk.cmd->view_valid = qfalse;
	vk.cmd->vertex_buffer_offset = 0;
	Com_Memset( vk.cmd->buf_offset, 0, sizeof( vk.cmd->buf_offset ) );
	Com_Memset( vk.cmd->vbo_offset, 0, sizeof( vk.cmd->vbo_offset ) );
	vk.cmd->curr_index_buffer = VK_NULL_HANDLE;
	vk.cmd->curr_index_offset = 0;
	vk.cmd->num_indexes = 0;
	vk.cmd->emissive_factor = 0.0f;

	Com_Memset( &vk.cmd->descriptor_set, 0, sizeof( vk.cmd->descriptor_set ) );
	vk.cmd->descriptor_set.start = ~0U;
	//vk.cmd->descriptor_set.end = 0;

	// other stats
	vk.stats.push_size = 0;
}


static void vk_resize_geometry_buffer( void )
{
	int i;
	RB_ClearDeferredHud();
	vk.cmd->gpu_time_armed = vk.cmd->gpu_time_pending = qfalse;
	/* None of this recording reaches the GPU, including its rate-map upload. */
	VK_FovDiscardUpload( &vk_foveation );

	/* Discard the auxiliary HUD recording with the overflowing main frame.
	 * Otherwise its next BeginCommandBuffer targets a buffer mid-recording. */
	vk_hud_end();
	if ( vk.cmd->hud_begun ) {
		VK_CHECK( qvkEndCommandBuffer( vk.cmd->hud_command_buffer ) );
		VK_CHECK( qvkResetCommandBuffer( vk.cmd->hud_command_buffer, 0 ) );
		vk.cmd->hud_begun = qfalse;
	}
	vk_end_render_pass();

	VK_CHECK( qvkEndCommandBuffer( vk.cmd->command_buffer ) );

	qvkResetCommandBuffer( vk.cmd->command_buffer, 0 );

	vk_wait_idle();

	vk_release_geometry_buffers();

	vk_create_geometry_buffers( vk.geometry_buffer_size_new );
	vk.geometry_buffer_size_new = 0;

	for ( i = 0; i < NUM_COMMAND_BUFFERS; i++ ) {
		vk_update_uniform_descriptor( vk.tess[ i ].uniform_descriptor, vk.tess[ i ].vertex_buffer );
		vk_update_view_descriptor( vk.tess[i].storage_descriptor, vk.tess[i].vertex_buffer );
	}

	ri.Printf( PRINT_DEVELOPER, "...geometry buffer resized to %iK\n", (int)( vk.geometry_buffer_size / 1024 ) );
}


// Paper-white level (nits) the gamma pass maps SDR-white to, pushed to the HDR shader.
static float vk_hdr_paper_white( void )
{
	if ( r_hdrPaperWhite->value > 0.0f )
		return r_hdrPaperWhite->value;
	// auto: BT.2408 HLG reference white for the panel peak. Luminance of a
	// 75% HLG signal (scene-linear 0.264964) at the display system gamma.
	double sysgamma = 1.2 + 0.42 * log10( r_hdrPeak->value / 1000.0 );
	return (float)( r_hdrPeak->value * pow( 0.264964, sysgamma ) );
}

/* Multiview render-pass boundaries invalidate push constants. Initialize all
 * HDR values at each output draw, including fields inactive in SDR. */
static void vk_post_process_push( VkPipelineLayout layout ) {
	struct {
		int32_t hdrCalibrate;
		float paperWhite, hdrPeak, hdrHighlight, hdrSaturation;
		float hdrSaturationFull, hdrSoftKnee;
		int32_t bloomActive;
	} push;
	push.hdrCalibrate = (vk.hdrActive && r_hdrCalibrate->integer) ? 1 : 0;
	// the blur ran this frame exactly when the post-scene pass opened
	push.bloomActive = vk.postOpen ? 1 : 0;
#ifdef __APPLE__
	/* EDR is SDR-white-relative, so both values stay on the paper-white/80 scale. */
	{
		float edr = Sys_MacOS_CurrentEDRHeadroom();
		if ( edr < 1.0f )
			edr = 1.0f;
		push.paperWhite = 80.0f;
		push.hdrPeak = 80.0f * edr;
	}
#else
	push.paperWhite = vk_hdr_paper_white();
	push.hdrPeak = r_hdrPeak->value;
#endif
	push.hdrHighlight = r_hdrHighlight->value;
	push.hdrSaturation = r_hdrSaturation->value;
	push.hdrSaturationFull = r_hdrSaturationFull->value;
	push.hdrSoftKnee = r_hdrSoftKnee->value;
	qvkCmdPushConstants( vk.cmd->command_buffer, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof( push ),
						 &push );
}

/* SCREEN */
static void vk_render_scope_bands( void ) {
	float scale = VK_XR_ScopeScaleY();
	VkClearAttachment colors[2];
	VkClearRect rects[2];
	uint32_t band;

	if ( !VK_XR_ScopeNeedsBands() || scale >= 1 || scale <= 0 )
		return;
	band = (uint32_t)((1 - scale) * 0.5f * vk.sceneHeight);
	if ( !band )
		return;
	if ( tess.numIndexes ) {
		RB_EndSurface();
		tess.shader = NULL;
	}
	Com_Memset( colors, 0, sizeof( colors ) );
	Com_Memset( rects, 0, sizeof( rects ) );
	colors[0].aspectMask = colors[1].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	// black; in the post-scene pass zero alpha also keeps bloom out of the bands
	colors[0].clearValue.color.float32[3] = VK_PassIsPostScene( vk.renderPassIndex ) ? 0.0f : 1.0f;
	colors[1].colorAttachment = 1;
	rects[0].rect.extent.width = rects[1].rect.extent.width = vk.sceneWidth;
	rects[0].rect.extent.height = rects[1].rect.extent.height = band;
	rects[1].rect.offset.y = vk.sceneHeight - band;
	rects[0].layerCount = rects[1].layerCount = 1;
	qvkCmdClearAttachments( vk.cmd->command_buffer, vk.hdrActive ? 2 : 1, colors, 2, rects );
}

static void vk_screen_mip_barrier( uint32_t base, uint32_t count, VkImageLayout oldLayout, VkImageLayout newLayout,
								   VkAccessFlags srcAccess, VkAccessFlags dstAccess, VkPipelineStageFlags dstStage ) {
	VkImageMemoryBarrier barrier;
	Com_Memset( &barrier, 0, sizeof( barrier ) );
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcAccessMask = srcAccess;
	barrier.dstAccessMask = dstAccess;
	barrier.oldLayout = oldLayout;
	barrier.newLayout = newLayout;
	barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = vk_screen.image.handle;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.baseMipLevel = base;
	barrier.subresourceRange.levelCount = count;
	barrier.subresourceRange.layerCount = 1;
	qvkCmdPipelineBarrier( vk.cmd->command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, dstStage, 0, 0, NULL, 0, NULL, 1,
						   &barrier );
}

/* Crops the gamma-corrected mono output into the virtual-screen texture and rebuilds its mips. */
static void vk_capture_screen_source( VkImage srcImage, VkFormatProperties properties ) {
	VkImageBlit copy;
	int i, crop[4];

	record_image_layout_transition( vk.cmd->command_buffer, vk_screen.image.handle, VK_IMAGE_ASPECT_COLOR_BIT,
									VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
									VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
									VK_ACCESS_TRANSFER_WRITE_BIT );
	Com_Memset( &copy, 0, sizeof( copy ) );
	copy.srcSubresource.aspectMask = copy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	copy.srcSubresource.layerCount = copy.dstSubresource.layerCount = 1;
	VK_XR_ScreenCaptureRect( vk.sceneWidth, vk.sceneHeight, crop );
	copy.srcOffsets[0].x = crop[0];
	copy.srcOffsets[0].y = crop[1];
	copy.srcOffsets[1].x = crop[2];
	copy.srcOffsets[1].y = crop[3];
	copy.srcOffsets[1].z = 1;
	copy.dstOffsets[1].x = vk_screen.image.uploadWidth;
	copy.dstOffsets[1].y = vk_screen.image.uploadHeight;
	copy.dstOffsets[1].z = 1;
	qvkCmdBlitImage( vk.cmd->command_buffer, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					 vk_screen.image.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy,
					 properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT
						 ? VK_FILTER_LINEAR
						 : VK_FILTER_NEAREST );
	for ( i = 1; i < (int)vk_screen.mips; i++ ) {
		vk_screen_mip_barrier( i - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
							   VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT );
		Com_Memset( &copy, 0, sizeof( copy ) );
		copy.srcSubresource.aspectMask = copy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copy.srcSubresource.layerCount = copy.dstSubresource.layerCount = 1;
		copy.srcSubresource.mipLevel = i - 1;
		copy.dstSubresource.mipLevel = i;
		copy.srcOffsets[1].x = MAX( 1, vk_screen.image.uploadWidth >> (i - 1) );
		copy.srcOffsets[1].y = MAX( 1, vk_screen.image.uploadHeight >> (i - 1) );
		copy.srcOffsets[1].z = 1;
		copy.dstOffsets[1].x = MAX( 1, vk_screen.image.uploadWidth >> i );
		copy.dstOffsets[1].y = MAX( 1, vk_screen.image.uploadHeight >> i );
		copy.dstOffsets[1].z = 1;
		qvkCmdBlitImage( vk.cmd->command_buffer, vk_screen.image.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
						 vk_screen.image.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy, VK_FILTER_LINEAR );
	}
	/* Every level but the last was a blit source. */
	if ( vk_screen.mips > 1 )
		vk_screen_mip_barrier( 0, vk_screen.mips - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
							   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
							   VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT );
	vk_screen_mip_barrier( vk_screen.mips - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
						   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
						   VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT );

}

/* CONTROLLER MODELS AND POINTER RAYS */
#define VK_XR_MODEL_IMAGES 16
#define VK_XR_MODEL_TEXTURE_MAX 4096

/* One of an asset's base color textures; the white image when it won't decode. The decoded pixels stay with the
 * model, so a renderer restart only uploads them again; a width below zero marks a failure. */
static image_t *vk_xr_model_image( const vkXRModel_t *model, int asset, int index ) {
	vrModelImage_t *source = &model->model.images[index];
	char name[MAX_QPATH];
	image_t *image;
	Com_sprintf( name, sizeof( name ), "*xrmodel%d_%d", asset, index );
	if ( !source->pixels && !source->width ) {
		byte *pic = NULL;
		int width = 0, height = 0, i, count = 0;
		source->width = -1;
		/* decoding takes several times the pixels in memory, so the header's size is checked first */
		if ( VR_ModelImageSize( source, &width, &height ) && width > 0 && height > 0 &&
			 width <= VK_XR_MODEL_TEXTURE_MAX && height <= VK_XR_MODEL_TEXTURE_MAX ) {
			if ( source->format == VR_MODEL_IMAGE_JPEG )
				ri.CL_DecodeJPG( name, source->data, source->size, &pic, &width, &height );
			else if ( source->format == VR_MODEL_IMAGE_PNG )
				R_DecodePNG( name, source->data, source->size, &pic, &width, &height );
		}
		if ( pic && width > 0 && height > 0 && width <= VK_XR_MODEL_TEXTURE_MAX && height <= VK_XR_MODEL_TEXTURE_MAX ) {
			count = width * height;
			source->pixels = VK_XR_ModelAlloc( (size_t)count * 4 );
		}
		if ( source->pixels ) {
			Com_Memcpy( source->pixels, pic, (size_t)count * 4 );
			// glTF's opaque and masked materials ignore whatever the texture's alpha holds
			for ( i = 0; i < count; i++ )
				source->pixels[i * 4 + 3] = 255;
			source->width = width;
			source->height = height;
		} else {
			ri.Printf( PRINT_WARNING, "OpenXR controller model texture %d did not decode; drawing it untextured\n", index );
		}
		if ( pic )
			ri.Free( pic );
	}
	if ( !source->pixels )
		return tr.whiteImage;
	image = R_CreateImage( name, NULL, source->pixels, source->width, source->height,
						   IMGFLAG_MIPMAP | IMGFLAG_NOLIGHTSCALE | IMGFLAG_NO_COMPRESSION );
	return image ? image : tr.whiteImage;
}

/* The textures, shaders and static geometry for one asset. A shader per texture serves every material on it,
 * since each material's color is already in its vertices. */
static void vk_xr_model_asset( const vkXRModel_t *model, int asset ) {
	const size_t size = VR_ModelPack( &model->model, NULL, NULL );
	int i, m;
	if ( size ) {
		byte *packed = ri.Hunk_AllocateTempMemory( (int)size );
		/* hunk memory goes with the rest of tr at the next renderer restart */
		tr.xrAssets[asset].offsets = ri.Hunk_Alloc( model->model.primitiveCount * sizeof( unsigned ), h_low );
		VR_ModelPack( &model->model, packed, tr.xrAssets[asset].offsets );
		vk_create_static_buffer( packed, size, &tr.xrAssets[asset].buffer, &tr.xrAssets[asset].memory );
		ri.Hunk_FreeTempMemory( packed );
	}
	Com_Memcpy( tr.xrAssets[asset].cacheId, model->cacheId, sizeof( tr.xrAssets[asset].cacheId ) );
	tr.xrAssets[asset].packed = size;
	tr.xrAssets[asset].plain = R_XRModelShader( va( "*xrmodel%d_%d", asset, -1 ), tr.whiteImage, qfalse );
	for ( i = 0; i < VK_XR_MODEL_IMAGES; i++ ) {
		image_t *image = tr.whiteImage;
		/* only a texture some material draws with is decoded */
		for ( m = 0; m < model->model.materialCount && model->model.materials[m].image != i; m++ )
			;
		if ( i < model->model.imageCount && m < model->model.materialCount )
			image = vk_xr_model_image( model, asset, i );
		tr.xrAssets[asset].shaders[i] =
			image == tr.whiteImage ? NULL : R_XRModelShader( va( "*xrmodel%d_%d", asset, i ), image, qfalse );
	}
	tr.xrAssets[asset].loaded = qtrue;
}

/* Frontend, once a frame: updates the models and makes sure whichever are loaded have their drawing resources. */
void vk_prepare_xr_models( void ) {
	int slot, asset;
	qboolean busy;
	if ( !VK_XR_Enabled() || !tr.registered )
		return;
	/* fetching a model, decoding a texture and uploading one each stall the frame: one of them a frame */
	busy = VK_XR_UpdateModels();
	for ( slot = 0; slot < VK_XR_MODELS_MAX; slot++ ) {
		const vkXRModel_t *model = VK_XR_Model( slot );
		if ( !model ) {
			tr.xrModels[slot].serial = 0;
			continue;
		}
		if ( tr.xrModels[slot].serial == model->serial )
			continue;
		/* a controller that slept and woke brings the same asset back under a new model; the size is compared
		 * too, for a runtime that gives different files one ID */
		for ( asset = 0; asset < (int)ARRAY_LEN( tr.xrAssets ); asset++ )
			if ( tr.xrAssets[asset].loaded &&
				 !memcmp( tr.xrAssets[asset].cacheId, model->cacheId, sizeof( model->cacheId ) ) &&
				 tr.xrAssets[asset].packed == VR_ModelPack( &model->model, NULL, NULL ) )
				break;
		if ( asset == (int)ARRAY_LEN( tr.xrAssets ) ) {
			for ( asset = 0; asset < (int)ARRAY_LEN( tr.xrAssets ) && tr.xrAssets[asset].loaded; asset++ )
				;
			if ( asset == (int)ARRAY_LEN( tr.xrAssets ) ) {
				ri.Printf( PRINT_WARNING, "OpenXR controller model not drawn: %d different models are already loaded\n", asset );
				asset = -1;
			} else if ( busy ) {
				continue;
			} else {
				vk_xr_model_asset( model, asset );
				busy = qtrue;
			}
		}
		tr.xrModels[slot].serial = model->serial;
		tr.xrModels[slot].asset = asset;
	}
}

#define VK_XR_POOL_SEGMENTS 20
#define VK_XR_POOL_RINGS 3
/* The pointer's red, and the blue it takes on instead: the ray, then the pool's center and each ring out from it. */
static const byte vk_xr_pointer_rgb[2][3] = {{255, 48, 40}, {77, 128, 255}};
static const byte vk_xr_pool_rgba[2][VK_XR_POOL_RINGS + 1][4] = {
	{{255, 190, 170, 255}, {255, 64, 52, 230}, {255, 48, 40, 110}, {255, 48, 40, 0}},
	{{170, 200, 255, 255}, {90, 140, 255, 230}, {77, 128, 255, 110}, {77, 128, 255, 0}}};

/* Each hand's pointer: a strip along its ray, turned toward the head and widening with distance to look even,
 * and, while the ray is on the screen, its cursor as a pool of light there, hot at the point a click lands and
 * fading out. Both test against the controllers' depth, so they draw after them. */
static void vk_draw_screen_pointers( const vrScreenGeometry_t *screen, const vec3_t eye ) {
	/* each ring's radius as a share of the screen's height */
	static const float radius[VK_XR_POOL_RINGS] = {0.005f, 0.011f, 0.024f};
	qboolean begun = qfalse;
	int hand, i, ring;
	for ( hand = 0; hand < 2; hand++ ) {
		const vkXRPointer_t *pointer = VK_XR_Pointer( hand );
		vec3_t along, toEye, side, center, left, right;
		color4ub_t faint, bright;
		const byte( *color )[4];
		int ndx;
		if ( !pointer )
			continue;
		color = vk_xr_pool_rgba[pointer->blue ? 1 : 0];
		if ( !begun ) {
			RB_BeginSurface( tr.xrPointerShader, 0 );
			begun = qtrue;
		}
		VectorSubtract( pointer->end, pointer->origin, along );
		VectorSubtract( eye, pointer->origin, toEye );
		CrossProduct( along, toEye, side );
		if ( VectorNormalize( side ) >= 0.000001f ) {
			RB_CHECKOVERFLOW( 4, 6 );
#ifdef USE_VBO
			tess.surfType = SF_TRIANGLES;
#endif
			for ( i = 0; i < 3; i++ )
				faint.rgba[i] = bright.rgba[i] = vk_xr_pointer_rgb[pointer->blue ? 1 : 0][i];
			faint.rgba[3] = 40;
			bright.rgba[3] = 200;
			ndx = tess.numVertexes;
			for ( i = 0; i < 3; i++ ) {
				tess.xyz[ndx][i] = pointer->origin[i] + side[i] * 0.0015f;
				tess.xyz[ndx + 1][i] = pointer->origin[i] - side[i] * 0.0015f;
				tess.xyz[ndx + 2][i] = pointer->end[i] - side[i] * 0.004f;
				tess.xyz[ndx + 3][i] = pointer->end[i] + side[i] * 0.004f;
			}
			for ( i = 0; i < 4; i++ ) {
				tess.texCoords[0][ndx + i][0] = tess.texCoords[1][ndx + i][0] = 0;
				tess.texCoords[0][ndx + i][1] = tess.texCoords[1][ndx + i][1] = 0;
				VectorCopy( side, tess.normal[ndx + i] );
				tess.vertexColors[ndx + i] = i < 2 ? faint : bright;
			}
			tess.indexes[tess.numIndexes + 0] = ndx + 0;
			tess.indexes[tess.numIndexes + 1] = ndx + 1;
			tess.indexes[tess.numIndexes + 2] = ndx + 3;
			tess.indexes[tess.numIndexes + 3] = ndx + 3;
			tess.indexes[tess.numIndexes + 4] = ndx + 1;
			tess.indexes[tess.numIndexes + 5] = ndx + 2;
			tess.numVertexes += 4;
			tess.numIndexes += 6;
		}

		if ( !pointer->pool )
			continue;
		/* the pool lies in the screen's surface: across is the screen's own tangent there, up is the world's */
		VR_ScreenPoint( screen, pointer->cursor[0], pointer->cursor[1], center );
		VR_ScreenPoint( screen, pointer->cursor[0] - 0.01f, pointer->cursor[1], left );
		VR_ScreenPoint( screen, pointer->cursor[0] + 0.01f, pointer->cursor[1], right );
		VectorSubtract( right, left, side );
		if ( VectorNormalize( side ) < 0.000001f )
			continue;
		RB_CHECKOVERFLOW( 1 + VK_XR_POOL_RINGS * VK_XR_POOL_SEGMENTS, ( 2 * VK_XR_POOL_RINGS - 1 ) * VK_XR_POOL_SEGMENTS * 3 );
#ifdef USE_VBO
		tess.surfType = SF_TRIANGLES;
#endif
		ndx = tess.numVertexes;
		VectorCopy( center, tess.xyz[ndx] );
		Com_Memcpy( tess.vertexColors[ndx].rgba, color[0], 4 );
		for ( ring = 0; ring < VK_XR_POOL_RINGS; ring++ ) {
			const float r = radius[ring] * screen->height;
			for ( i = 0; i < VK_XR_POOL_SEGMENTS; i++ ) {
				const float angle = i * ( 2 * (float)M_PI / VK_XR_POOL_SEGMENTS );
				const int v = ndx + 1 + ring * VK_XR_POOL_SEGMENTS + i, next = ( i + 1 ) % VK_XR_POOL_SEGMENTS;
				VectorMA( center, r * cosf( angle ), side, tess.xyz[v] );
				tess.xyz[v][1] += r * sinf( angle );
				Com_Memcpy( tess.vertexColors[v].rgba, color[ring + 1], 4 );
				if ( ring == 0 ) {
					tess.indexes[tess.numIndexes++] = ndx;
					tess.indexes[tess.numIndexes++] = v;
					tess.indexes[tess.numIndexes++] = ndx + 1 + next;
				} else {
					const int inner = v - VK_XR_POOL_SEGMENTS, innerNext = ndx + 1 + ( ring - 1 ) * VK_XR_POOL_SEGMENTS + next;
					tess.indexes[tess.numIndexes++] = inner;
					tess.indexes[tess.numIndexes++] = v;
					tess.indexes[tess.numIndexes++] = innerNext;
					tess.indexes[tess.numIndexes++] = innerNext;
					tess.indexes[tess.numIndexes++] = v;
					tess.indexes[tess.numIndexes++] = ndx + 1 + ring * VK_XR_POOL_SEGMENTS + next;
				}
			}
		}
		for ( i = ndx; i < ndx + 1 + VK_XR_POOL_RINGS * VK_XR_POOL_SEGMENTS; i++ ) {
			tess.texCoords[0][i][0] = tess.texCoords[1][i][0] = 0;
			tess.texCoords[0][i][1] = tess.texCoords[1][i][1] = 0;
			VectorClear( tess.normal[i] );
		}
		tess.numVertexes += 1 + VK_XR_POOL_RINGS * VK_XR_POOL_SEGMENTS;
	}
	if ( begun )
		RB_EndSurface();
}

/* Each part draws from the asset's static buffer under its node's matrix; only its headlight colors, four bytes a
 * vertex, are made each frame. Both sides of every triangle draw and the pass's depth buffer orders them. */
static void vk_draw_screen_models( const vec3_t eye ) {
	static float world[VR_MODEL_MAX_NODES * 16];
	static unsigned char visible[VR_MODEL_MAX_NODES];
	const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
	qboolean drawn = qfalse;
	int slot, n, p;
	for ( slot = 0; slot < VK_XR_MODELS_MAX; slot++ ) {
		const vkXRModel_t *model = VK_XR_Model( slot );
		const vrModel_t *m;
		int asset;
		if ( !model || !model->drawable || tr.xrModels[slot].serial != model->serial || tr.xrModels[slot].asset < 0 )
			continue;
		asset = tr.xrModels[slot].asset;
		m = &model->model;
		if ( !tr.xrAssets[asset].buffer )
			continue;
		VR_ModelPose( m, &model->root, model->states, model->map, model->nodeCount, world, visible );
		for ( n = 0; n < m->nodeCount; n++ ) {
			const vrModelMesh_t *mesh;
			if ( m->nodes[n].mesh < 0 || m->nodes[n].mesh >= m->meshCount || !visible[n] )
				continue;
			mesh = &m->meshes[m->nodes[n].mesh];
			for ( p = mesh->firstPrimitive; p < mesh->firstPrimitive + mesh->primitiveCount; p++ ) {
				const vrModelPrimitive_t *primitive = &m->primitives[p];
				const int index = m->materials[primitive->material].image;
				const shader_t *shader = index >= 0 && index < VK_XR_MODEL_IMAGES && tr.xrAssets[asset].shaders[index]
											 ? tr.xrAssets[asset].shaders[index]
											 : tr.xrAssets[asset].plain;
				const shaderStage_t *stage = shader->stages[0];
				const uint32_t colors = PAD( vk.cmd->vertex_buffer_offset, 32 ), size = primitive->vertexCount * 4;
				const VkDeviceSize start = tr.xrAssets[asset].offsets[p];
				VkBuffer buffers[3];
				VkDeviceSize offsets[3];
				/* a primitive without a triangle has no indices to bind */
				if ( primitive->indexCount < 3 )
					continue;
				if ( colors + size > vk.geometry_buffer_size ) {
					// schedule geometry buffer resize
					vk.geometry_buffer_size_new = log2pad( colors + size, 1 );
					return;
				}
				VR_ModelShade( m, p, world + n * 16, eye, vk.cmd->vertex_buffer_ptr + colors );
				vk.cmd->vertex_buffer_offset = (VkDeviceSize)colors + size;
				buffers[0] = buffers[2] = tr.xrAssets[asset].buffer;
				buffers[1] = vk.cmd->vertex_buffer;
				offsets[0] = start;
				offsets[1] = colors;
				offsets[2] = start + VR_MODEL_PACK_ST( primitive->vertexCount );
				GL_SelectTexture( 0 );
				GL_Bind( stage->bundle[0].image[0] );
				vk_update_mvp( world + n * 16 );
				vk.cmd->emissive_factor = 0.0f;
				vk_bind_pipeline( stage->vk_pipeline[0] );
				qvkCmdBindVertexBuffers( vk.cmd->command_buffer, 0, 3, buffers, offsets );
				vk_bind_index_buffer( tr.xrAssets[asset].buffer, start + VR_MODEL_PACK_INDEX( primitive->vertexCount ) );
				vk.cmd->num_indexes = primitive->indexCount;
				vk_draw_geometry( DEPTH_RANGE_NORMAL, qtrue );
				drawn = qtrue;
			}
		}
	}
	/* what follows is in tracking space again */
	if ( drawn )
		vk_update_mvp( identity );
}

/* Draws the floor, the screen's reflection and the screen in stereo into the open pass. */
static void vk_draw_screen_composition( const vrScreenGeometry_t *screen, qboolean clear ) {
	const vec4_t black = {0, 0, 0, 1};
	float matrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
	viewParms_t savedView;
	trRefdef_t savedRefdef;
	const trRefEntity_t *savedEntity;
	qboolean saved2D;
	color4ub_t white;
	int eye, i;

	savedView = backEnd.viewParms;
	savedRefdef = backEnd.refdef;
	savedEntity = backEnd.currentEntity;
	saved2D = backEnd.projection2D;
	if ( clear ) {
		backEnd.projection2D = qtrue;
		vk_clear_color( black );
	}
	backEnd.projection2D = qfalse;
	backEnd.currentEntity = &tr.worldEntity;
	backEnd.viewParms.portalView = PV_NONE;
	white.u32 = ~0U;
	backEnd.viewParms.xrMultiview = qtrue;
	for ( eye = 0; eye < 2; eye++ ) {
		VK_XR_EyeMatrix( eye, backEnd.viewParms.eyeProjection[eye] );
	}
	{
		int segments = screen->curved ? 64 : 1;
		backEnd.refdef.stereoFrame = STEREO_CENTER;
		backEnd.viewParms.viewportX = 0;
		backEnd.viewParms.viewportY = 0;
		backEnd.viewParms.viewportWidth = glConfig.vidWidth;
		backEnd.viewParms.viewportHeight = glConfig.vidHeight;
		vk.cmd->depth_range = DEPTH_RANGE_COUNT;
		tess.depthRange = DEPTH_RANGE_NORMAL;
		/* Eye matrices already map tracking-space geometry to clip space. */
		vk_update_mvp( matrix );
		RB_BeginSurface( tr.virtualReflectionShader, 0 );
		for ( i = 0; i < segments; i++ ) {
			float u0 = (float)i / segments, u1 = (float)(i + 1) / segments;
			vec3_t a, b, origin, left, up = {0, 0, 0};
			int axis;
			/* The screen's bottom band mirrored below the floor; t = 1 touches the floor. */
			VR_ScreenPoint( screen, u0, 1.0f, a );
			VR_ScreenPoint( screen, u1, 1.0f, b );
			for ( axis = 0; axis < 3; axis++ ) {
				origin[axis] = (a[axis] + b[axis]) * 0.5f;
				left[axis] = (a[axis] - b[axis]) * 0.5f;
			}
			origin[1] = -screen->height * VK_SCREEN_REFLECT_SPAN * 0.5f;
			up[1] = screen->height * VK_SCREEN_REFLECT_SPAN * 0.5f;
			RB_AddQuadStampExt( origin, left, up, white, u0, 1.0f, u1, 1.0f - VK_SCREEN_REFLECT_SPAN );
		}
		RB_EndSurface();
		RB_BeginSurface( tr.virtualFloorShader, 0 );
				{
			vec3_t origin, left = {15, 0, 0}, up = {0, 0, 15};
			VK_XR_FloorOrigin( origin );
			RB_AddQuadStampExt( origin, left, up, white, 0, 0, 1, 1 );
		}
		RB_EndSurface();
		RB_BeginSurface( tr.virtualScreenShader, 0 );
		for ( i = 0; i < segments; i++ ) {
			float u0 = (float)i / segments, u1 = (float)(i + 1) / segments;
			vec3_t a, b, origin, left, up = {0, 0, 0};
			int axis;
			/* Bottom points sit at y = 0; raise the origin by half the height to their center. */
			VR_ScreenPoint( screen, u0, 1.0f, a );
			VR_ScreenPoint( screen, u1, 1.0f, b );
			for ( axis = 0; axis < 3; axis++ ) {
				origin[axis] = (a[axis] + b[axis]) * 0.5f;
				left[axis] = (a[axis] - b[axis]) * 0.5f;
			}
			origin[1] = screen->height * 0.5f;
			up[1] = screen->height * 0.5f;
			RB_AddQuadStampExt( origin, left, up, white, u0, 0, u1, 1 );
		}
		RB_EndSurface();
		{
			/* the controllers sit in front of the screen; each ray hides behind whatever part of them covers it */
			vec3_t eye;
			VK_XR_HeadPosition( eye );
			vk_draw_screen_models( eye );
			vk_draw_screen_pointers( screen, eye );
		}
		tess.shader = NULL;
	}
	backEnd.viewParms = savedView;
	backEnd.refdef = savedRefdef;
	backEnd.currentEntity = savedEntity;
	backEnd.projection2D = saved2D;
	vk.cmd->depth_range = DEPTH_RANGE_COUNT;
}

/* Direct mode's virtual screen: capture the mono frame from the swapchain image, then compose into it. */
static void vk_render_virtual_screen( vkMonoTargets_t *source ) {
	const vrScreenGeometry_t *screen = VK_XR_Screen();
	VkFormatProperties properties;
	VkImage srcImage;
	VkFramebuffer composition;
	struct vkXRDirectTarget_s *t;

	if ( !screen || !vk.xrDirect )
		return;
	t = vk_xr_direct_target();
	if ( !tr.virtualScreenShader || !tr.virtualReflectionShader || !vk_screen.image.handle || !t->screen ) {
		ri.Error( ERR_DROP, "Virtual screen requires the Vulkan scene and HUD targets" );
		return;
	}
	if ( tess.numIndexes ) {
		RB_EndSurface();
		tess.shader = NULL;
	}
	vk_end_render_pass();
	vk_hud_invalidate_caches();
	properties = vk.xr_direct.imageFormatProperties;
	if ( !(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) ) {
		ri.Error( ERR_DROP, "XR swapchain format cannot be a blit source" );
		return;
	}
	/* The blit reads the sRGB image and decodes it, as it does the FBO path's output. */
	record_image_layout_transition( vk.cmd->command_buffer, t->image, VK_IMAGE_ASPECT_COLOR_BIT,
									VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
									VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 0 );
	srcImage = t->image;
	composition = t->screen;

	if ( !vk.repeatScreen )
		vk_capture_screen_source( srcImage, properties );

	/* Screen metadata remains live, but subsequent passes consume stereo output. */
	source->sourceActive = qfalse;

	/* The floor, screen and controllers compose in a single-sample pass over its own depth. */
	vk.renderPassIndex = RENDER_PASS_VR_SCREEN;
	vk.renderWidth = vk.sceneWidth;
	vk.renderHeight = vk.sceneHeight;
	vk.renderScaleX = (float)vk.sceneWidth / glConfig.vidWidth;
	vk.renderScaleY = (float)vk.sceneHeight / glConfig.vidHeight;
	vk_begin_render_pass( vk_screen.pass, composition, qtrue, vk.sceneWidth, vk.sceneHeight );
	vk_draw_screen_composition( screen, qtrue );
}

/* MIRROR */
/* Pure layout policy: each draw samples one complete array layer. The
 * viewport performs fit/fill and the scissor bounds its destination slot. */
typedef struct {
	float viewport[4];
	int scissor[4];
	int layer; // ignored for the mono menu capture
} vkMirrorDraw_t;
typedef struct {
	int count, mono;
	vkMirrorDraw_t draw[2];
} vkMirrorPlan_t;

static vkMirrorPlan_t VK_MirrorPlan( int enabled, int type, int fill, int flatMenu, int sourceWidth,
									 int sourceHeight, int width, int height ) {
	vkMirrorPlan_t plan = {0};
	int count, i;
	if ( !enabled || sourceWidth < 1 || sourceHeight < 1 || width < 1 || height < 1 )
		return plan;
	plan.mono = flatMenu != 0;
	if ( type < 0 || type > 2 )
		type = 0;
	count = !flatMenu && type == 2 && width >= 2 ? 2 : 1;
	for ( i = 0; i < count; i++ ) {
		vkMirrorDraw_t *draw = &plan.draw[i];
		int slotX = count == 2 && i ? width / 2 : 0;
		int slotWidth = count == 2 ? (i ? width - width / 2 : width / 2) : width;
		int eye = count == 2 ? i : type == 1;
		double sourceAspect = flatMenu ? 4.0 / 3.0 : (double)sourceWidth / sourceHeight;
		double scaleX = 1, scaleY = 1, destAspect = (double)slotWidth / height;
		double x = slotX, y = 0, w = slotWidth, h = height;
		if ( fill && !flatMenu ) {
			if ( destAspect > sourceAspect )
				scaleY = sourceAspect / destAspect;
			else
				scaleX = destAspect / sourceAspect;
		} else {
			if ( destAspect > sourceAspect )
				w = height * sourceAspect;
			else
				h = slotWidth / sourceAspect;
			if ( w < 1 )
				w = 1;
			if ( h < 1 )
				h = 1;
			x += (slotWidth - w) * .5;
			y = (height - h) * .5;
		}
		draw->scissor[0] = (int)x;
		draw->scissor[1] = (int)y;
		draw->scissor[2] = (int)w;
		draw->scissor[3] = (int)h;
		/* Use the integer destination for an exact pixel-aligned fit. */
		x = draw->scissor[0];
		y = draw->scissor[1];
		w = draw->scissor[2];
		h = draw->scissor[3];
		draw->layer = flatMenu ? 0 : eye;
		draw->viewport[2] = (float)(w / scaleX);
		draw->viewport[3] = (float)(h / scaleY);
		draw->viewport[0] = (float)(x - draw->viewport[2] * (1 - scaleX) * .5);
		draw->viewport[1] = (float)(y - draw->viewport[3] * (1 - scaleY) * .5);
	}
	plan.count = count;
	return plan;
}

static void vk_draw_desktop_mirror( void ) {
	VkViewport viewport = {0};
	VkRect2D scissor;
	vkMirrorPlan_t plan;
	const VkPhysicalDeviceLimits *limits = &vk.deviceLimits;
	int i;
	if ( !VK_XR_Drawing() ) {
		viewport.x = vk.blitX0;
		viewport.y = vk.blitY0;
		viewport.width = gls.windowWidth - vk.blitX0 * 2;
		viewport.height = gls.windowHeight - vk.blitY0 * 2;
		viewport.maxDepth = 1;
		scissor.offset.x = vk.blitX0;
		scissor.offset.y = vk.blitY0;
		scissor.extent.width = (uint32_t)viewport.width;
		scissor.extent.height = (uint32_t)viewport.height;
		qvkCmdSetViewport( vk.cmd->command_buffer, 0, 1, &viewport );
		qvkCmdSetScissor( vk.cmd->command_buffer, 0, 1, &scissor );
		qvkCmdDraw( vk.cmd->command_buffer, 4, 1, 0, 0 );
		return;
	}
	{
		VkClearAttachment clear = {0};
		VkClearRect rect = {0};
		clear.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		clear.clearValue.color.float32[3] = 1;
		rect.rect.extent.width = gls.windowWidth;
		rect.rect.extent.height = gls.windowHeight;
		rect.layerCount = 1;
		/* Keep acquire/submit/present balanced with mirror output disabled. */
		qvkCmdClearAttachments( vk.cmd->command_buffer, 1, &clear, 1, &rect );
	}
	plan = VK_MirrorPlan( vr_mirrorEnabled->integer, vr_desktopContentType->integer, vr_desktopContentFit->integer,
						  VK_XR_Screen() && (!vr_desktopMenuStyle->integer || r_hdrCalibrate->integer),
						  vk.sceneWidth, vk.sceneHeight, gls.windowWidth, gls.windowHeight );
	if ( !plan.count )
		return;
	if ( plan.mono ) {
		qvkCmdBindPipeline( vk.cmd->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
							vk.xr_output.screen_mono_pipeline );
	}
	if ( plan.mono ) {
		image_t *image = vk_screen_image();
		if ( !image || !image->descriptor )
			return;
		qvkCmdBindDescriptorSets( vk.cmd->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
								  vk.pipeline_layout_post_process, 0, 1, &image->descriptor, 0, NULL );
		/* The saved menu image has no corresponding emissive attachment. */
		qvkCmdBindDescriptorSets( vk.cmd->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
								  vk.pipeline_layout_post_process, 1, 1, &tr.blackImage->descriptor, 0, NULL );
	}
	for ( i = 0; i < plan.count; i++ ) {
		const vkMirrorDraw_t *draw = &plan.draw[i];
		if ( !plan.mono ) {
			/* Every mirror source is already processed; the world eye image also reconstructs HDR highlights. */
			qvkCmdBindPipeline( vk.cmd->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
								vk.postFlow == VK_POST_FLOW_WORLD ? vk.xr_output.eye_mirror[draw->layer]
																   : vk.xr_output.screen_mirror_eye[draw->layer] );
		}
		viewport.x = draw->viewport[0];
		viewport.y = draw->viewport[1];
		viewport.width = draw->viewport[2];
		viewport.height = draw->viewport[3];
		viewport.maxDepth = 1;
		/* Extreme aspect ratios must never produce invalid Vulkan viewports. */
		if ( viewport.width > limits->maxViewportDimensions[0] ||
			viewport.height > limits->maxViewportDimensions[1] ||
			viewport.x < limits->viewportBoundsRange[0] || viewport.y < limits->viewportBoundsRange[0] ||
			viewport.x + viewport.width > limits->viewportBoundsRange[1] ||
			viewport.y + viewport.height > limits->viewportBoundsRange[1] )
			continue;
		scissor.offset.x = draw->scissor[0];
		scissor.offset.y = draw->scissor[1];
		scissor.extent.width = draw->scissor[2];
		scissor.extent.height = draw->scissor[3];
		qvkCmdSetViewport( vk.cmd->command_buffer, 0, 1, &viewport );
		qvkCmdSetScissor( vk.cmd->command_buffer, 0, 1, &scissor );
		qvkCmdDraw( vk.cmd->command_buffer, 4, 1, 0, 0 );
	}
}

static qboolean desktopTrackingVisible;
static qhandle_t desktopTrackingFont, desktopTrackingIcon;
void VK_DesktopTrackingStatus( qboolean visible, qhandle_t font, qhandle_t icon ) {
	desktopTrackingVisible = visible;
	desktopTrackingFont = font;
	desktopTrackingIcon = icon;
}
/* Only the desktop presentation attachment is bound here. Never the eye or
 * virtual-screen capture targets; headset output rebinds its own pipeline. */
static void vk_draw_tracking_status( void ) {
	renderPass_t savedPass = vk.renderPassIndex;
	float outputScale[2] = {1.0f, (float)(1 << tr.overbrightBits)};
	if ( !desktopTrackingVisible || !desktopTrackingFont )
		return;
#ifndef __APPLE__
	if ( vk.hdrActive )
		outputScale[0] = vk_hdr_paper_white() / 80.0f;
#endif
	/* Use normal material pipelines against the desktop attachment. */
	vk.renderPassIndex = RENDER_PASS_DESKTOP;
	vk_hud_invalidate_caches();
	qvkCmdPushConstants( vk.cmd->command_buffer, vk.pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 68,
						 sizeof( outputScale ), outputScale );
	RB_DrawTrackingStatus( desktopTrackingFont, desktopTrackingIcon,
						   floorf( MIN( gls.windowWidth / 40.0f, MAX( 12.0f, gls.windowHeight / 40.0f ) ) ) );
	vk.renderPassIndex = savedPass;
}

/* The acquired eye image the eye pass writes, or eye_count for the intermediate; the index is stale while the headset draws nothing. */
static uint32_t vk_eye_output_index( void ) {
	const uint32_t index = VK_XR_AcquiredIndex();
	return ( VK_XR_Drawing() && vk.xr_output.eye_pass && index < vk.xr_output.eye_count ) ? index : vk.xr_output.eye_count;
}

/* After the eye pass ends: screenshot and desktop mirror from the eye image, then hand it to the runtime. */
static void vk_finish_eye_frame( qboolean world )
{
	const uint32_t index = vk_eye_output_index();
	const qboolean direct = index < vk.xr_output.eye_count;
	const VkImage image = direct ? vk.xr_output.eye_image[index] : vk.xr_output.image;
	const VkImageLayout layout = direct ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	VkDescriptorSet *eye = direct ? &vk.xr_output.eye_descriptor[index] : &vk.xr_output.descriptor;
	const qboolean capture = backEnd.screenshotMask && vk.capture.image;
	const qboolean sampled = capture || !ri.CL_IsMinimized();

	vk_gpu_time_stamp( vk.cmd->command_buffer, 2 );

	if ( sampled )
		record_image_layout_transition( vk.cmd->command_buffer, image, VK_IMAGE_ASPECT_COLOR_BIT, layout,
										VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 0 );
	if ( capture )
	{
		vk_begin_render_pass( vk.render_pass.capture, vk.framebuffers.capture, qfalse, gls.captureWidth, gls.captureHeight );
		qvkCmdBindPipeline( vk.cmd->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.xr_output.screen_capture_pipeline );
		qvkCmdBindDescriptorSets( vk.cmd->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.pipeline_layout_post_process, 0, 1, eye, 0, NULL );
		// gamma_fs declares texture1; the capture runs SDR and never samples it
		qvkCmdBindDescriptorSets( vk.cmd->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.pipeline_layout_post_process, 1, 1, eye, 0, NULL );
		vk_post_process_push( vk.pipeline_layout_post_process );
		qvkCmdDraw( vk.cmd->command_buffer, 4, 1, 0, 0 );
		vk_end_render_pass();
	}
	if ( !ri.CL_IsMinimized() )
	{
		vk.renderWidth = gls.windowWidth;
		vk.renderHeight = gls.windowHeight;
		vk.renderScaleX = vk.renderScaleY = 1.0;
		vk.cmd->gpu_time_mirror = vk.cmd->gpu_time_armed;

		vk_begin_render_pass( vk.render_pass.gamma, vk.framebuffers.gamma[ vk.cmd->swapchain_image_index ], qfalse, vk.renderWidth, vk.renderHeight );
		qvkCmdBindPipeline( vk.cmd->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
							world ? vk.xr_output.eye_mirror[0] : vk.xr_output.screen_mirror_eye[0] );
		// the world's HDR mirror reconstructs from the eye image and the emissive layer; the composition has no emitters
		qvkCmdBindDescriptorSets( vk.cmd->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.pipeline_layout_post_process, 0, 1, eye, 0, NULL );
		qvkCmdBindDescriptorSets( vk.cmd->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.pipeline_layout_post_process, 1, 1,
								  world ? &vk.emissive_descriptor : eye, 0, NULL );
		vk_post_process_push( vk.pipeline_layout_post_process );
		vk_draw_desktop_mirror();
		vk_draw_tracking_status();
		vk_end_render_pass();
		if ( vk.cmd->gpu_time_mirror )
			vk_gpu_time_stamp( vk.cmd->command_buffer, 3 );
	}
	if ( sampled )
		record_image_layout_transition( vk.cmd->command_buffer, image, VK_IMAGE_ASPECT_COLOR_BIT,
										VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, layout, 0, 0 );

	if ( direct )
		VK_XR_Rendered();
	else
		VK_XR_CopyEyes( vk.cmd->command_buffer, vk.xr_output.image, vk.xr_output.format, vk.sceneWidth,
						vk.sceneHeight, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL );
	vk_gpu_time_stamp( vk.cmd->command_buffer, 9 );
	vk.postOpen = qfalse;
}


void vk_end_frame( void )
{
	VkCommandBuffer commands[2];
	uint32_t commandCount = 0;
#ifdef USE_UPLOAD_QUEUE
	VkSemaphore waits[2], signals[2];
	const VkPipelineStageFlags wait_dst_stage_mask[2] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
#else
	const VkPipelineStageFlags wait_dst_stage_mask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
#endif
	VkSubmitInfo submit_info;

	if ( vk.frame_count == 0 )
		return;

	vk.frame_count = 0;

	if ( vk.geometry_buffer_size_new )
	{
		vk_resize_geometry_buffer();
		// issue: one frame may be lost during video recording
		// solution: re-record all commands again? (might be complicated though)
		return;
	}

	if ( vk.postFlow == VK_POST_FLOW_SCREEN )
	{
		const vrScreenGeometry_t *screen = VK_XR_Screen();
		const uint32_t index = vk_eye_output_index();
		const qboolean direct = index < vk.xr_output.eye_count;
		const VkFormatProperties properties = vk.screenFormatProperties;

		// a 2D batch still pending when the frame ends without a swap
		if ( tess.numIndexes ) {
			RB_EndSurface();
			tess.shader = NULL;
		}
		vk.cmd->last_pipeline = VK_NULL_HANDLE;
		vk_begin_post_scene_pass(); // catch: frames without a 2D pass
		RB_RenderDeferredFlares();
		RB_DrawDeferredHud();
		vk_render_scope_bands();
		vk_end_render_pass();
		vk_hud_invalidate_caches();

		if ( !screen || !tr.virtualScreenShader || !tr.virtualReflectionShader || !vk_screen.image.handle ) {
			ri.Error( ERR_DROP, "Virtual screen requires the Vulkan scene and HUD targets" );
			return;
		}
		if ( !(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) ||
			!(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) ) {
			ri.Error( ERR_DROP, "Virtual screen color format does not support capture blit" );
			return;
		}

		// the mono output pass composites the mono scene, its bloom and gamma into the screen source
		vk.renderWidth = vk.sceneWidth;
		vk.renderHeight = vk.sceneHeight;
		vk.renderScaleX = (float)vk.sceneWidth / glConfig.vidWidth;
		vk.renderScaleY = (float)vk.sceneHeight / glConfig.vidHeight;
		vk_begin_render_pass( vk.mono.pass.output, vk.mono.framebuffer.output, qfalse, vk.sceneWidth, vk.sceneHeight );
		qvkCmdBindPipeline( vk.cmd->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.mono.pipeline.composite );
		qvkCmdBindDescriptorSets( vk.cmd->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.pipeline_layout_composite, 0, 1, &vk.composite_descriptor, 0, NULL );
		vk_post_process_push( vk.pipeline_layout_composite );
		qvkCmdDraw( vk.cmd->command_buffer, 4, 1, 0, 0 );
		vk_end_render_pass();
		if ( !vk.repeatScreen )
			vk_capture_screen_source( vk.xr_output.image, properties );
		/* Screen metadata remains live, but the composition consumes stereo output. */
		vk.mono.sourceActive = qfalse;

		// the pass clears to black and brings the depth buffer the controllers need
		vk_begin_render_pass( direct ? vk.xr_output.screen_eye_pass : vk.xr_output.screen_pass,
							  direct ? vk.xr_output.screen_eye_framebuffer[index] : vk.xr_output.screen_framebuffer, qtrue,
							  vk.sceneWidth, vk.sceneHeight );
		vk.renderPassIndex = RENDER_PASS_VR_SCREEN_EYE;
		vk_foveation_invalidate_rate();
		vk_hud_invalidate_caches();
		vk_draw_screen_composition( screen, qfalse );
		vk_end_render_pass();
		vk_finish_eye_frame( qfalse );
	}
	else if ( vk.postFlow == VK_POST_FLOW_WORLD )
	{
		const uint32_t index = vk_eye_output_index();
		const qboolean direct = index < vk.xr_output.eye_count;

		// a 2D batch still pending when the frame ends without a swap
		if ( tess.numIndexes ) {
			RB_EndSurface();
			tess.shader = NULL;
		}
		vk.cmd->last_pipeline = VK_NULL_HANDLE;
		vk_begin_post_scene_pass(); // catch: frames without a 2D pass
		RB_RenderDeferredFlares();
		RB_DrawDeferredHud();
		vk_render_scope_bands();
		vk_end_render_pass();

		// the eye pass composites the finished scene image, its bloom and gamma
		vk.renderWidth = vk.sceneWidth;
		vk.renderHeight = vk.sceneHeight;
		vk.renderScaleX = (float)vk.sceneWidth / glConfig.vidWidth;
		vk.renderScaleY = (float)vk.sceneHeight / glConfig.vidHeight;
		vk_begin_render_pass( direct ? vk.xr_output.eye_pass : vk.xr_output.pass,
							  direct ? vk.xr_output.eye_framebuffer[index] : vk.xr_output.framebuffer, qfalse,
							  vk.sceneWidth, vk.sceneHeight );
		qvkCmdBindPipeline( vk.cmd->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.xr_output.composite_pipeline );
		qvkCmdBindDescriptorSets( vk.cmd->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.pipeline_layout_composite, 0, 1, &vk.composite_descriptor, 0, NULL );
		vk_post_process_push( vk.pipeline_layout_composite );
		qvkCmdDraw( vk.cmd->command_buffer, 4, 1, 0, 0 );
		vk_end_render_pass();
		vk_finish_eye_frame( qtrue );
	}
	else if ( vk.fboActive )
	{
		vk.cmd->last_pipeline = VK_NULL_HANDLE; // the post-scene pass rebinds everything
		vk_begin_post_scene_pass(); // catch: frames without a 2D pass

		RB_RenderDeferredFlares();
		RB_DrawDeferredHud();
		vk_render_scope_bands();

		if ( backEnd.screenshotMask && vk.capture.image )
		{
			vk_end_render_pass();

			// composite the scene, bloom and gamma into the capture image
			vk_begin_render_pass( vk.render_pass.capture, vk.framebuffers.capture, qfalse, gls.captureWidth, gls.captureHeight );
			qvkCmdBindPipeline( vk.cmd->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.capture_pipeline );
			qvkCmdBindDescriptorSets( vk.cmd->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.pipeline_layout_composite, 0, 1, &vk.composite_descriptor, 0, NULL );
			vk_post_process_push( vk.pipeline_layout_composite );
			qvkCmdDraw( vk.cmd->command_buffer, 4, 1, 0, 0 );
		}

		if ( !ri.CL_IsMinimized() )
		{
			vk_end_render_pass();

			vk.renderWidth = gls.windowWidth;
			vk.renderHeight = gls.windowHeight;
			vk_gpu_time_stamp( vk.cmd->command_buffer, 2 );
			vk.cmd->gpu_time_mirror = vk.cmd->gpu_time_armed;

			vk.renderScaleX = 1.0;
			vk.renderScaleY = 1.0;

			vk_begin_render_pass( vk.render_pass.gamma, vk.framebuffers.gamma[ vk.cmd->swapchain_image_index ], qfalse, vk.renderWidth, vk.renderHeight );
			qvkCmdBindPipeline( vk.cmd->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.gamma_composite_pipeline );
			qvkCmdBindDescriptorSets( vk.cmd->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.pipeline_layout_composite, 0, 1, &vk.composite_descriptor, 0, NULL );

			vk_post_process_push( vk.pipeline_layout_composite );
			vk_draw_desktop_mirror();
			vk_draw_tracking_status();
		}
	}
	else if ( vk.xrDirect )
	{
		struct vkXRDirectTarget_s *t = vk_xr_direct_target();
		const qboolean capture = backEnd.screenshotMask && vk.capture.image;
		const qboolean sampled = capture || !ri.CL_IsMinimized();

		vk.cmd->last_pipeline = VK_NULL_HANDLE;

		// the same fallbacks the FBO branch runs, drawn into the still-open scene pass
		RB_RenderDeferredFlares();
		RB_DrawDeferredHud();
		vk_render_scope_bands();
		vk_render_virtual_screen( &vk.mono );

		vk_end_render_pass();

		// the finished eye image, or idle while no headset image is acquired, feeds the screenshot and the mirror
		if ( sampled )
			record_image_layout_transition( vk.cmd->command_buffer, t->image, VK_IMAGE_ASPECT_COLOR_BIT,
				VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 0 );
		if ( capture )
		{
			vk_begin_render_pass( vk.render_pass.capture, vk.framebuffers.capture, qfalse, gls.captureWidth, gls.captureHeight );
			qvkCmdBindPipeline( vk.cmd->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.xr_output.screen_capture_pipeline );
			qvkCmdBindDescriptorSets( vk.cmd->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.pipeline_layout_post_process, 0, 1, &t->descriptor, 0, NULL );
			// gamma_fs declares texture1; the capture runs SDR and never samples it
			qvkCmdBindDescriptorSets( vk.cmd->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.pipeline_layout_post_process, 1, 1, &t->descriptor, 0, NULL );
			vk_post_process_push( vk.pipeline_layout_post_process );
			qvkCmdDraw( vk.cmd->command_buffer, 4, 1, 0, 0 );
			vk_end_render_pass();
		}
		if ( !ri.CL_IsMinimized() )
		{
			vk.renderWidth = gls.windowWidth;
			vk.renderHeight = gls.windowHeight;
			vk.renderScaleX = vk.renderScaleY = 1.0;
			vk_gpu_time_stamp( vk.cmd->command_buffer, 2 );
			vk.cmd->gpu_time_mirror = vk.cmd->gpu_time_armed;

			vk_begin_render_pass( vk.render_pass.gamma, vk.framebuffers.gamma[ vk.cmd->swapchain_image_index ], qfalse, vk.renderWidth, vk.renderHeight );
			qvkCmdBindPipeline( vk.cmd->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.xr_output.screen_mirror_eye[0] );
			qvkCmdBindDescriptorSets( vk.cmd->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.pipeline_layout_post_process, 0, 1, &t->descriptor, 0, NULL );
			// gamma_fs declares texture1; it is sampled only under HDR
			qvkCmdBindDescriptorSets( vk.cmd->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.pipeline_layout_post_process, 1, 1, &t->descriptor, 0, NULL );
			vk_post_process_push( vk.pipeline_layout_post_process );
			vk_draw_desktop_mirror();
			vk_draw_tracking_status();
			vk_end_render_pass();
			if ( vk.cmd->gpu_time_mirror )
				vk_gpu_time_stamp( vk.cmd->command_buffer, 3 );
		}
		// xrReleaseSwapchainImage requires the attachment layout
		if ( sampled )
			record_image_layout_transition( vk.cmd->command_buffer, t->image, VK_IMAGE_ASPECT_COLOR_BIT,
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 0 );

		if ( VK_XR_Drawing() )
			VK_XR_Rendered();
	}

	if ( !vk.xrDirect && vk.postFlow == VK_POST_FLOW_LEGACY )
	{
		vk_end_render_pass();
		if ( vk.cmd->gpu_time_mirror )
			vk_gpu_time_stamp( vk.cmd->command_buffer, 3 );
		else
			vk_gpu_time_stamp( vk.cmd->command_buffer, 2 ); // hidden mirror: marks the post-scene end
	}

	vk_gpu_time_stamp( vk.cmd->command_buffer, 1 );
	VK_CHECK( qvkEndCommandBuffer( vk.cmd->command_buffer ) );

	submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit_info.pNext = NULL;
	if ( vk.cmd->hud_begun ) {
		vk_gpu_time_stamp( vk.cmd->hud_command_buffer, 5 );
		VK_CHECK( qvkEndCommandBuffer( vk.cmd->hud_command_buffer ) );
		commands[commandCount++] = vk.cmd->hud_command_buffer;
	}
	commands[commandCount++] = vk.cmd->command_buffer;
	submit_info.commandBufferCount = commandCount;
	submit_info.pCommandBuffers = commands;
	if ( !ri.CL_IsMinimized() ) {
#ifdef USE_UPLOAD_QUEUE
		if ( vk.image_uploaded != VK_NULL_HANDLE ) {
			waits[0] = vk.cmd->image_acquired;
			waits[1] = vk.image_uploaded;
			submit_info.waitSemaphoreCount = 2;
			submit_info.pWaitSemaphores = &waits[0];
			submit_info.pWaitDstStageMask = &wait_dst_stage_mask[0];
			signals[0] = vk.swapchain_rendering_finished[ vk.cmd->swapchain_image_index ];
			signals[1] = vk.cmd->rendering_finished2;
			submit_info.signalSemaphoreCount = 2;
			submit_info.pSignalSemaphores = &signals[0];

			vk.rendering_finished = vk.cmd->rendering_finished2;
			vk.image_uploaded = VK_NULL_HANDLE;
		} else if ( vk.rendering_finished != VK_NULL_HANDLE ) {
			waits[0] = vk.cmd->image_acquired;
			waits[1] = vk.rendering_finished;
			submit_info.waitSemaphoreCount = 2;
			submit_info.pWaitSemaphores = &waits[0];
			submit_info.pWaitDstStageMask = &wait_dst_stage_mask[0];
			signals[0] = vk.swapchain_rendering_finished[ vk.cmd->swapchain_image_index ];
			signals[1] = vk.cmd->rendering_finished2;
			submit_info.signalSemaphoreCount = 2;
			submit_info.pSignalSemaphores = &signals[0];

			vk.rendering_finished = vk.cmd->rendering_finished2;
		} else {
			submit_info.waitSemaphoreCount = 1;
			submit_info.pWaitSemaphores = &vk.cmd->image_acquired;
			submit_info.pWaitDstStageMask = &wait_dst_stage_mask[0];
			submit_info.signalSemaphoreCount = 1;
			submit_info.pSignalSemaphores = &vk.swapchain_rendering_finished[ vk.cmd->swapchain_image_index ];
		}
#else
		submit_info.waitSemaphoreCount = 1;
		submit_info.pWaitSemaphores = &vk.cmd->image_acquired;
		submit_info.pWaitDstStageMask = &wait_dst_stage_mask;
		submit_info.signalSemaphoreCount = 1;
		submit_info.pSignalSemaphores = &vk.swapchain_rendering_finished[ vk.cmd->swapchain_image_index ];
#endif
	} else {
		submit_info.waitSemaphoreCount = 0;
		submit_info.pWaitSemaphores = NULL;
		submit_info.pWaitDstStageMask = NULL;
		submit_info.signalSemaphoreCount = 0;
		submit_info.pSignalSemaphores = NULL;
	}

	VK_CHECK( qvkQueueSubmit( vk.queue, 1, &submit_info, vk.cmd->rendering_finished_fence ) );
	vk.cmd->gpu_time_pending = vk.cmd->gpu_time_armed;
	vk.cmd->gpu_time_armed = qfalse;
	VK_XR_Submitted();
	vk.cmd->waitForFence = qtrue;

	// presentation may take undefined time to complete, we can't measure it in a reliable way
	backEnd.pc.msec = ri.Milliseconds() - backEnd.pc.msec;

	vk.renderPassIndex = RENDER_PASS_MAIN;
}


/* A loading frame with the screen as last captured: the composition redraws the floor, the screen, the controllers
 * and the pointers for the head pose of the XR frame the client has begun, and the frame submits. Nothing here
 * reaches the shader or image registries, so it may run from inside a load. */
qboolean RE_XRLoadingFrameDue( void ) {
	return tr.registered && !vk.frame_count && VK_XR_ScreenVisible() && vk_screen.image.handle && VK_XR_LoadingFrameDue();
}

qboolean RE_XRRedrawEnvironment( void ) {
	if ( !tr.registered || vk.frame_count || !VK_XR_Screen() || !vk_screen.image.handle )
		return qfalse;
	vk_prepare_xr_models(); // the controllers' poses for this frame's display time
	vk.repeatScreen = qtrue;
	vk_begin_frame();
	vk_end_frame();
	vk_present_frame();
	vk.repeatScreen = qfalse;
	return qtrue;
}

void vk_present_frame( void )
{
	VkPresentInfoKHR present_info;
	VkResult res;

	if ( !vk.cmd->waitForFence ) {
		// nothing has been submitted this frame due to geometry buffer overflow?
		return;
	}

	// The headset paces frames while the mirror is hidden.
	if ( !ri.CL_IsMinimized() && vk.cmd->swapchain_image_acquired ) {
		present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
		present_info.pNext = NULL;
		present_info.waitSemaphoreCount = 1;
		present_info.pWaitSemaphores = &vk.swapchain_rendering_finished[ vk.cmd->swapchain_image_index ];
		present_info.swapchainCount = 1;
		present_info.pSwapchains = &vk.swapchain;
		present_info.pImageIndices = &vk.cmd->swapchain_image_index;
		present_info.pResults = NULL;

		vk.cmd->swapchain_image_acquired = qfalse;

		res = qvkQueuePresentKHR( vk.queue, &present_info );
		switch ( res ) {
			case VK_SUCCESS:
				break;
			case VK_SUBOPTIMAL_KHR:
			case VK_ERROR_OUT_OF_DATE_KHR:
				// swapchain re-creation needed
				vk_restart_swapchain( __func__, res );
				return;
			case VK_ERROR_DEVICE_LOST:
				// we can ignore that
				ri.Printf( PRINT_DEVELOPER, "vkQueuePresentKHR: device lost\n" );
				break;
			default:
				// or we don't
				ri.Error( ERR_FATAL, "vkQueuePresentKHR returned %s", vk_result_string( res ) );
		}
	}

	// pickup next command buffer for rendering
	vk.cmd_index++;
	vk.cmd_index %= NUM_COMMAND_BUFFERS;
	vk.cmd = &vk.tess[ vk.cmd_index ];
}


static qboolean is_bgr( VkFormat format ) {
	switch ( format ) {
		case VK_FORMAT_B8G8R8A8_UNORM:
		case VK_FORMAT_B8G8R8A8_SNORM:
		case VK_FORMAT_B8G8R8A8_UINT:
		case VK_FORMAT_B8G8R8A8_SINT:
		case VK_FORMAT_B8G8R8A8_SRGB:
		case VK_FORMAT_B4G4R4A4_UNORM_PACK16:
			return qtrue;
		default:
			return qfalse;
	}
}


void vk_read_pixels( byte *buffer, uint32_t width, uint32_t height )
{
	VkCommandBuffer command_buffer;
	VkDeviceMemory memory;
	VkMemoryRequirements memory_requirements;
	VkMemoryPropertyFlags memory_reqs;
	VkMemoryPropertyFlags memory_flags;
	VkMemoryAllocateInfo alloc_info;
	VkImageSubresource subresource;
	VkSubresourceLayout layout;
	VkImageCreateInfo desc;
	VkImage srcImage;
	VkImageLayout srcImageLayout;
	VkImage dstImage;
	byte *buffer_ptr;
	byte *data;
	uint32_t pixel_width;
	uint32_t i, n;
	qboolean invalidate_ptr;

	VK_CHECK( qvkWaitForFences( vk.device, 1, &vk.cmd->rendering_finished_fence, VK_FALSE, 1e12 ) );

	if ( vk.fboActive ) {
		// the composited frame, already at the capture size
		srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		srcImage = vk.capture.image;
	} else if ( vk.xrDirect ) {
		// the left eye, drawn into the capture image at the end of the frame
		srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		srcImage = vk.capture.image;
	} else {
		srcImageLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		srcImage = vk.swapchain_images[ vk.cmd->swapchain_image_index ];
	}

	Com_Memset( &desc, 0, sizeof( desc ) );

	// Create image in host visible memory to serve as a destination for framebuffer pixels.
	desc.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	desc.pNext = NULL;
	desc.flags = 0;
	desc.imageType = VK_IMAGE_TYPE_2D;
	desc.format = vk.capture_format;
	desc.extent.width = width;
	desc.extent.height = height;
	desc.extent.depth = 1;
	desc.mipLevels = 1;
	desc.arrayLayers = 1;
	desc.samples = VK_SAMPLE_COUNT_1_BIT;
	desc.tiling = VK_IMAGE_TILING_LINEAR;
	desc.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	desc.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	desc.queueFamilyIndexCount = 0;
	desc.pQueueFamilyIndices = NULL;
	desc.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	VK_CHECK( qvkCreateImage( vk.device, &desc, NULL, &dstImage ) );

	qvkGetImageMemoryRequirements( vk.device, dstImage, &memory_requirements );

	alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc_info.pNext = NULL;
	alloc_info.allocationSize = memory_requirements.size;

	// host_cached bit is desirable for fast reads
	memory_reqs = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
	alloc_info.memoryTypeIndex = find_memory_type2( memory_requirements.memoryTypeBits, memory_reqs, &memory_flags );
	if ( alloc_info.memoryTypeIndex == ~0 ) {
		// try less explicit flags, without host_coherent
		memory_reqs = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
		alloc_info.memoryTypeIndex = find_memory_type2( memory_requirements.memoryTypeBits, memory_reqs, &memory_flags );
		if ( alloc_info.memoryTypeIndex == ~0U ) {
			// slowest case
			memory_reqs = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
			alloc_info.memoryTypeIndex = find_memory_type2( memory_requirements.memoryTypeBits, memory_reqs, &memory_flags );
			if ( alloc_info.memoryTypeIndex == ~0U ) {
				ri.Error( ERR_FATAL, "%s(): failed to find matching memory type for image capture", __func__ );
			}
		}
	}

	if ( memory_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT ) {
		invalidate_ptr = qfalse;
	} else {
		 // according to specification - must be performed if host_coherent is not set
		invalidate_ptr = qtrue;
	}

	VK_CHECK(qvkAllocateMemory(vk.device, &alloc_info, NULL, &memory));
	VK_CHECK(qvkBindImageMemory(vk.device, dstImage, memory, 0));

	command_buffer = begin_command_buffer();

	if ( srcImageLayout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ) {
		record_image_layout_transition( command_buffer, srcImage,
			VK_IMAGE_ASPECT_COLOR_BIT,
			srcImageLayout,
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			0, 0);
	}

	record_image_layout_transition( command_buffer, dstImage,
		VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_UNDEFINED,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 0 );

	// end_command_buffer( command_buffer );

	// command_buffer = begin_command_buffer();

	if ( vk.blitEnabled ) {
		VkImageBlit region;

		region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.srcSubresource.mipLevel = 0;
		region.srcSubresource.baseArrayLayer = 0;
		region.srcSubresource.layerCount = 1;
		region.srcOffsets[0].x = 0;
		region.srcOffsets[0].y = 0;
		region.srcOffsets[0].z = 0;
		region.srcOffsets[1].x = width;
		region.srcOffsets[1].y = height;
		region.srcOffsets[1].z = 1;
		region.dstSubresource = region.srcSubresource;
		region.dstOffsets[0] = region.srcOffsets[0];
		region.dstOffsets[1] = region.srcOffsets[1];

		qvkCmdBlitImage( command_buffer, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_NEAREST );

	} else {
		VkImageCopy region;

		region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.srcSubresource.mipLevel = 0;
		region.srcSubresource.baseArrayLayer = 0;
		region.srcSubresource.layerCount = 1;
		region.srcOffset.x = 0;
		region.srcOffset.y = 0;
		region.srcOffset.z = 0;
		region.dstSubresource = region.srcSubresource;
		region.dstOffset = region.srcOffset;
		region.extent.width = width;
		region.extent.height = height;
		region.extent.depth = 1;

		qvkCmdCopyImage( command_buffer, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region );
	}

	end_command_buffer( command_buffer, __func__ );

	// Copy data from destination image to memory buffer.
	subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	subresource.mipLevel = 0;
	subresource.arrayLayer = 0;

	qvkGetImageSubresourceLayout( vk.device, dstImage, &subresource, &layout );

	VK_CHECK( qvkMapMemory( vk.device, memory, 0, VK_WHOLE_SIZE, 0, (void**)&data ) );

	if ( invalidate_ptr )
	{
		VkMappedMemoryRange range;
		range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
		range.pNext = NULL;
		range.memory = memory;
		range.size = VK_WHOLE_SIZE;
		range.offset = 0;
		qvkInvalidateMappedMemoryRanges( vk.device, 1, &range );
	}

	data += layout.offset;

	switch ( vk.capture_format ) {
		case VK_FORMAT_B4G4R4A4_UNORM_PACK16: pixel_width = 2; break;
		case VK_FORMAT_R16G16B16A16_UNORM: pixel_width = 8; break;
		default: pixel_width = 4; break;
	}

	buffer_ptr = buffer + width * (height - 1) * 3;
	for ( i = 0; i < height; i++ ) {
		switch ( pixel_width ) {
			case 2: {
				uint16_t *src = (uint16_t*)data;
				for ( n = 0; n < width; n++ ) {
					buffer_ptr[n*3+0] = ((src[n]>>12)&0xF)<<4;
					buffer_ptr[n*3+1] = ((src[n]>>8)&0xF)<<4;
					buffer_ptr[n*3+2] = ((src[n]>>4)&0xF)<<4;
				}
			} break;

			case 4: {
				for ( n = 0; n < width; n++ ) {
					Com_Memcpy( &buffer_ptr[n*3], &data[n*4], 3 );
					//buffer_ptr[n*3+0] = data[n*4+0];
					//buffer_ptr[n*3+1] = data[n*4+1];
					//buffer_ptr[n*3+2] = data[n*4+2];
				}
			} break;

			case 8: {
				const uint16_t *src = (uint16_t*)data;
				for ( n = 0; n < width; n++ ) {
					buffer_ptr[n*3+0] = src[n*4+0]>>8;
					buffer_ptr[n*3+1] = src[n*4+1]>>8;
					buffer_ptr[n*3+2] = src[n*4+2]>>8;
				}
			} break;
		}
		buffer_ptr -= width * 3;
		data += layout.rowPitch;
	}

	if ( is_bgr( vk.capture_format ) ) {
		buffer_ptr = buffer;
		for ( i = 0; i < width * height; i++ ) {
			byte tmp = buffer_ptr[0];
			buffer_ptr[0] = buffer_ptr[2];
			buffer_ptr[2] = tmp;
			buffer_ptr += 3;
		}
	}

	qvkDestroyImage( vk.device, dstImage, NULL );
	qvkFreeMemory( vk.device, memory, NULL );

	// restore previous layout
	if ( srcImageLayout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ) {
		command_buffer = begin_command_buffer();

		record_image_layout_transition( command_buffer, srcImage,
			VK_IMAGE_ASPECT_COLOR_BIT,
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			srcImageLayout, 0, 0 );

		end_command_buffer( command_buffer, "restore layout" );
	}
}


static void vk_bloom_blur( void )
{
	uint32_t i;

	for ( i = 0; i < VK_NUM_BLOOM_PASSES*2; i+=2 ) {
		// horizontal blur
		vk_begin_blur_render_pass( i+0 );
		qvkCmdBindPipeline( vk.cmd->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
							vk_mono_source() ? vk.mono.pipeline.blur[i + 0] : vk.blur_pipeline[i + 0] );
		qvkCmdBindDescriptorSets( vk.cmd->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.pipeline_layout_post_process, 0, 1,
								  i == 0 ? &vk.color_descriptor : &vk.bloom_image_descriptor[i+0], 0, NULL );
		qvkCmdDraw( vk.cmd->command_buffer, 4, 1, 0, 0 );
		vk_end_render_pass();

		// vertical blur
		vk_begin_blur_render_pass( i+1 );
		qvkCmdBindPipeline( vk.cmd->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
							vk_mono_source() ? vk.mono.pipeline.blur[i + 1] : vk.blur_pipeline[i + 1] );
		qvkCmdBindDescriptorSets( vk.cmd->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.pipeline_layout_post_process, 0, 1, &vk.bloom_image_descriptor[i+1], 0, NULL );
		qvkCmdDraw( vk.cmd->command_buffer, 4, 1, 0, 0 );
		vk_end_render_pass();
	}
}

/* Ends the scene pass once bloom has blurred and opens the post-scene pass over the resolved image, where coronas
 * and 2D blend before gamma and leave their transmittance in alpha. Without bloom the scene pass keeps them. */
void vk_begin_post_scene_pass( void )
{
	if ( vk.postOpen || vk.renderPassIndex == RENDER_PASS_SCREENMAP )
		return;
	if ( !r_bloom->integer || !backEnd.doneSurfaces || !vk.fboActive )
		return;

	vk_end_render_pass(); // end main
	vk_gpu_time_stamp( vk.cmd->command_buffer, 6 );
	vk_bloom_blur();
	vk_gpu_time_stamp( vk.cmd->command_buffer, 8 );

	// coronas and 2D blend into the scene image here; the composite adds bloom underneath them
	vk_begin_post_scene_render_pass();

	// the blur bound other layouts and the scene pipelines don't fit this pass, so draws rebind everything
	vk_foveation_invalidate_rate();
	vk_hud_invalidate_caches();
	vk_update_mvp( NULL );

	backEnd.doneBloom = qtrue;
	vk.postOpen = qtrue;
}
