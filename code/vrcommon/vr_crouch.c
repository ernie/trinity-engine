#include "vr_crouch.h"

/* (CROUCH_VIEWHEIGHT + 24) / (DEFAULT_VIEWHEIGHT + 24) and the ducked box top, 40 / 50 */
#define CROUCH_LINE 0.72f
#define STAND_LINE  0.80f

/* cgame's DUCK_TIME */
#define RAMP_MS 100

void VR_CrouchSetStanding( vrCrouch_t *c, float standing ) {
	c->standing = standing;
}

int VR_CrouchUpdate( vrCrouch_t *c, float head, int eligible ) {
	if ( !eligible || c->standing <= 0.0f )
		c->active = 0;
	else if ( head < c->standing * CROUCH_LINE )
		c->active = 1;
	else if ( head > c->standing * STAND_LINE )
		c->active = 0;
	return c->active;
}

float VR_CrouchLift( vrCrouch_t *c, int commanded, int time, float full ) {
	float f;

	commanded = commanded != 0;
	if ( commanded != c->commanded ) {
		c->commanded = commanded;
		c->changeTime = time;
		c->changed = 1;
	}
	if ( !c->changed )
		return 0.0f;

	/* a restarted clock starts cgame with no duck offset, so the ramp is done */
	if ( time < c->changeTime ) {
		c->changeTime = time - RAMP_MS;
		return commanded ? full : 0.0f;
	}

	f = (float)( time - c->changeTime ) / RAMP_MS;
	if ( f > 1.0f )
		f = 1.0f;
	return commanded ? full * f : full * ( 1.0f - f );
}
