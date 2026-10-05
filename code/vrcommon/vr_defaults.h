#ifndef VR_DEFAULTS_H
#define VR_DEFAULTS_H

/* The Frame build launches in VR with no mirror. */
#ifdef TRINITY_FRAME
#define VR_ENABLED_DEFAULT "1"
#define VR_MIRROR_DEFAULT "0"
#else
#define VR_ENABLED_DEFAULT "0"
#define VR_MIRROR_DEFAULT "1"
#endif
#define VR_MODE_CVAR_FLAGS ( CVAR_ARCHIVE_NOCLI | CVAR_LATCH | CVAR_NORESTART )

#endif
