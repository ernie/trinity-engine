#ifndef VR_RATE_LIST_H
#define VR_RATE_LIST_H

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Appends the rate, written whole, unless an entry lies within half a hertz of it; 1 when the list changed. */
static inline int VR_RateListOffer( char *list, size_t size, double measured ) {
	const char *p = list;
	char *end;
	size_t used;
	int n;
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
		p = end;
	}
	used = strlen( list );
	n = snprintf( list + used, size - used, "%s%.0f", used ? " " : "", measured );
	if ( n < 0 || (size_t)n >= size - used ) {
		list[used] = 0;
		return 0;
	}
	return 1;
}
#endif
