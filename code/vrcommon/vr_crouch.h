#ifndef VR_CROUCH_H
#define VR_CROUCH_H

typedef struct {
	float standing;
	int   active;
	int   commanded;
	int   changeTime;
	int   changed;
} vrCrouch_t;

void  VR_CrouchSetStanding( vrCrouch_t *c, float standing );
int   VR_CrouchUpdate( vrCrouch_t *c, float head, int eligible );
float VR_CrouchLift( vrCrouch_t *c, int commanded, int time, float full );

#endif
