/* SDR engine output is encoded; SRGB targets require linear shader values. */
#ifndef VK_XR_COLOR_H
#define VK_XR_COLOR_H
float VKXR_SdrLinear( float value ) {
	if ( value <= 0 ) {
		return 0;
	}
	if ( value >= 1 ) {
		return 1;
	}
	return value <= .04045 ? value / 12.92 : pow( (value + .055) / 1.055, 2.4 );
}
float VKXR_SdrEncoded( float value ) {
	return value < .0031308 ? value * 12.92 : 1.055 * pow( value, 1.0 / 2.4 ) - .055;
}
#endif
