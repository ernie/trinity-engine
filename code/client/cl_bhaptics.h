#ifndef CL_BHAPTICS_H
#define CL_BHAPTICS_H
#include <stddef.h>
typedef struct {
	float x, y;
	int intensity, motorCount;
} clBHPoint_t;
typedef struct {
	void *library;
	int initialized, playing;
	void (*destroy)(void);
	void (*stop)(void);
	void (*submit)(const char *, int, clBHPoint_t *, size_t, int);
} clBHaptics_t;
/* Missing libraries or exports leave the context uninitialized. */
int CL_BHaptics_Open( clBHaptics_t * );
void CL_BHaptics_Stop( clBHaptics_t * );
void CL_BHaptics_Close( clBHaptics_t * );
void CL_BHaptics_Event( clBHaptics_t *, const char *event, int position, int intensity, float yaw,
						float height, float scale, int rightHanded, int menuLeftHanded );
#endif
