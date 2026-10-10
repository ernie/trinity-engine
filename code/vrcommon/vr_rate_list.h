#ifndef VR_RATE_LIST_H
#define VR_RATE_LIST_H

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Sorts the runtime's rates lowest first, so the menus list them in order. */
static inline void VR_RateSort( float *rates, unsigned count ) {
	unsigned i, j;
	for ( i = 1; i < count; i++ ) {
		const float rate = rates[i];
		for ( j = i; j > 0 && rates[j - 1] > rate; j-- )
			rates[j] = rates[j - 1];
		rates[j] = rate;
	}
}

/* Inserts the rate, rounded to whole hertz, in ascending order unless one within half a hertz is already listed. Returns 1 when the list changed. */
static inline int VR_RateListOffer( char *list, size_t size, double measured ) {
	const char *p = list;
	const char *at = NULL;
	char *end;
	char entry[16];
	size_t used, n;
	for ( ;; ) {
		double rate;
		while ( *p == ' ' )
			p++;
		if ( !*p )
			break;
		rate = strtod( p, &end );
		if ( end == p ) {
			while ( *p && *p != ' ' )
				p++;
			continue;
		}
		if ( fabs( rate - measured ) <= 0.5 )
			return 0;
		if ( !at && rate > measured )
			at = p;
		p = end;
	}
	used = strlen( list );
	snprintf( entry, sizeof( entry ), "%.0f", measured );
	n = strlen( entry ) + ( used ? 1 : 0 );
	if ( used + n >= size )
		return 0;
	if ( at ) {
		const size_t offset = (size_t)( at - list );
		memmove( list + offset + n, list + offset, used - offset + 1 );
		memcpy( list + offset, entry, n - 1 );
		list[offset + n - 1] = ' ';
	} else {
		if ( used )
			list[used++] = ' ';
		strcpy( list + used, entry );
	}
	return 1;
}
#endif
