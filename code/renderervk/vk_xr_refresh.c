#include "../vrcommon/vr_float.h"
#include "vk_xr_refresh.h"
#include <string.h>
#include <math.h>
#include <stdio.h>
XrResult VK_XRRefresh_Init( vkXRRefresh_t *ctx, XrInstance instance, XrSession session,
							PFN_xrGetInstanceProcAddr proc, int enabled ) {
	XrResult result;
	uint32_t i;
	if ( !ctx )
		return XR_ERROR_VALIDATION_FAILURE;
	memset( ctx, 0, sizeof( *ctx ) );
	if ( !enabled )
		return XR_SUCCESS;
	if ( !instance || !session || !proc )
		return XR_ERROR_VALIDATION_FAILURE;
#define LOAD(field, name) \
	do { \
		PFN_xrVoidFunction fn = NULL; \
		result = proc( instance, #name, &fn ); \
		if ( XR_FAILED( result ) || !fn ) \
			return XR_FAILED( result ) ? result : XR_ERROR_FUNCTION_UNSUPPORTED; \
		ctx->field = (PFN_##name)fn; \
	} while ( 0 )
	LOAD( enumerate, xrEnumerateDisplayRefreshRatesFB );
	LOAD( get, xrGetDisplayRefreshRateFB );
	LOAD( request, xrRequestDisplayRefreshRateFB );
#undef LOAD
	result = ctx->enumerate( session, 32, &ctx->count, ctx->rates );
	if ( XR_FAILED( result ) )
		return result;
	if ( ctx->count > 32 )
		return XR_ERROR_LIMIT_REACHED;
	for ( i = 0; i < ctx->count; i++ )
		if ( !VR_FloatFinite( ctx->rates[i] ) || ctx->rates[i] <= 0 )
			return XR_ERROR_RUNTIME_FAILURE;
	result = ctx->get( session, &ctx->current );
	if ( XR_FAILED( result ) )
		return result;
	ctx->session = session;
	ctx->available = 1;
	return XR_SUCCESS;
}
XrResult VK_XRRefresh_Request( vkXRRefresh_t *ctx, float hz ) {
	uint32_t i;
	XrResult result;
	if ( !ctx || !ctx->available )
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	if ( !VR_FloatFinite( hz ) || hz < 0 )
		return XR_ERROR_VALIDATION_FAILURE;
	if ( hz != 0 ) {
		for ( i = 0; i < ctx->count; i++ )
			if ( fabsf( ctx->rates[i] - hz ) < .01f )
				break;
		if ( i == ctx->count )
			return XR_ERROR_DISPLAY_REFRESH_RATE_UNSUPPORTED_FB;
		hz = ctx->rates[i];
	}
	result = ctx->request( ctx->session, hz );
	/* Requests may be applied asynchronously; do not report the target as current. */
	if ( XR_SUCCEEDED( result ) )
		ctx->get( ctx->session, &ctx->current );
	return result;
}
void VK_XRRefresh_FormatRates( const vkXRRefresh_t *ctx, char *out, size_t size ) {
	uint32_t i;
	size_t used = 0;
	if ( !out || !size )
		return;
	out[0] = 0;
	if ( !ctx || !ctx->available )
		return;
	for ( i = 0; i < ctx->count; i++ ) {
		int n = snprintf( out + used, size - used, "%s%g", i ? " " : "", (double)ctx->rates[i] );
		if ( n < 0 || (size_t)n >= size - used ) {
			out[size - 1] = 0;
			return;
		}
		used += (size_t)n;
	}
}
