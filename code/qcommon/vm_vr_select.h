#ifndef VM_VR_SELECT_H
#define VM_VR_SELECT_H

#include "q_shared.h"

// Major of the first TRINITY_VR_API/<major>.<minor> marker inside length; 0 when absent.
int VM_VRParseMarker( const byte *data, int length, int *minor );
// The engine runs a QVM whose major matches and whose minor it meets or exceeds.
qboolean VM_VRAccepts( int major, int minor, int engineMajor, int engineMinor );

#endif // VM_VR_SELECT_H
