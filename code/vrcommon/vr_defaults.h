#ifndef VR_DEFAULTS_H
#define VR_DEFAULTS_H

/* The Frame build launches in VR with no mirror, unarchived, so a +set plays flat for one session; other builds keep the player's choice. */
#ifdef TRINITY_FRAME
#define VR_ENABLED_DEFAULT "1"
#define VR_MIRROR_DEFAULT "0"
#define VR_MODE_CVAR_FLAGS ( CVAR_LATCH | CVAR_NORESTART )
#else
#define VR_ENABLED_DEFAULT "0"
#define VR_MIRROR_DEFAULT "1"
#define VR_MODE_CVAR_FLAGS ( CVAR_ARCHIVE | CVAR_LATCH | CVAR_NORESTART )
#endif

#endif
