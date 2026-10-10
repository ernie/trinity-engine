// A C interface to Basis Universal's C++ KTX2 transcoder
#include "vr_ktx2.h"

#include "../thirdparty/basisu/transcoder/basisu_transcoder.h"

#define VR_KTX2_MAX_SIDE 8192

extern "C" int VR_KTX2Decode( const void *data, int size, unsigned char **rgba, int *width, int *height,
							  void *( *alloc )( size_t ), void ( *release )( void * ) )
{
	static bool initialized;
	basist::ktx2_transcoder transcoder;
	unsigned char *out;
	uint32_t w, h;

	*rgba = NULL;
	*width = *height = 0;
	if ( !data || size <= 0 )
	{
		return 0;
	}
	if ( !initialized )
	{
		basist::basisu_transcoder_init();
		initialized = true;
	}
	if ( !transcoder.init( data, (uint32_t)size ) || !transcoder.start_transcoding() )
	{
		return 0;
	}
	w = transcoder.get_width();
	h = transcoder.get_height();
	if ( !w || !h || w > VR_KTX2_MAX_SIDE || h > VR_KTX2_MAX_SIDE )
	{
		return 0;
	}
	out = (unsigned char *)alloc( (size_t)w * h * 4 );
	if ( !out )
	{
		return 0;
	}
	if ( !transcoder.transcode_image_level( 0, 0, 0, out, w * h, basist::transcoder_texture_format::cTFRGBA32 ) )
	{
		release( out );
		return 0;
	}
	*rgba = out;
	*width = (int)w;
	*height = (int)h;
	return 1;
}
