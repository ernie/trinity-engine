#ifndef VR_DEFAULTS_H
#define VR_DEFAULTS_H

/* The Steam Frame build launches in VR with no desktop mirror, and neither choice is archived: a +set on the launch
 * line plays flat for one session without ever reaching the config. Every other build keeps the player's choice. */
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
