#ifndef VR_DEFAULTS_H
#define VR_DEFAULTS_H

/* The Frame and Android builds are VR-only, with the mirror off by default. */
#if defined(TRINITY_FRAME) || defined(__ANDROID__)
#define VR_ONLY 1
#define VR_ENABLED_DEFAULT "1"
#define VR_ENABLED_MIN "1"
#define VR_MIRROR_DEFAULT "0"
#else
#define VR_ONLY 0
#define VR_ENABLED_DEFAULT "0"
#define VR_ENABLED_MIN "0"
#define VR_MIRROR_DEFAULT "1"
#endif
/* An Android headset has no desktop to mirror to. */
#ifdef __ANDROID__
#define VR_MIRROR_MAX "0"
#else
#define VR_MIRROR_MAX "1"
#endif
#define VR_MODE_CVAR_FLAGS ( CVAR_ARCHIVE_NOCLI | CVAR_LATCH | CVAR_NORESTART )

#endif
