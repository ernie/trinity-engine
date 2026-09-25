#include "vm_vr_select.h"

static int VM_VRParseNumber( const byte *data, int length, int *p ) {
	int value = 0;

	while ( *p < length && data[*p] >= '0' && data[*p] <= '9' ) {
		if ( value < 100000 )
			value = value * 10 + data[*p] - '0';
		( *p )++;
	}
	return value;
}

int VM_VRParseMarker( const byte *data, int length, int *minor ) {
	static const char needle[] = "TRINITY_VR_API/";
	const int needleLen = (int)sizeof( needle ) - 1;
	int i, p, major;

	*minor = 0;
	for ( i = 0; i + needleLen <= length; i++ ) {
		if ( data[i] != needle[0] || memcmp( data + i, needle, needleLen ) )
			continue;
		p = i + needleLen;
		major = VM_VRParseNumber( data, length, &p );
		if ( p < length && data[p] == '.' ) {
			p++;
			*minor = VM_VRParseNumber( data, length, &p );
		}
		return major;
	}
	return 0;
}

qboolean VM_VRAccepts( int major, int minor, int engineMajor, int engineMinor ) {
	return major == engineMajor && minor <= engineMinor;
}
