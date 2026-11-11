/* config.h -- mvlo_nx (NSMB Mario vs Luigi Online, Switch wrapper) build config.
 *
 * Forked from cloverpit_nx's config.h (MIT), itself from the badpiggies_nx /
 * colorsheep_nx / laytonbmr_nx / vln_nx SoLoader lineage (MIT). The memory
 * constants below are engine-generation properties (Unity 6, IL2CPP, arm64) and
 * are kept exactly as cloverpit_nx validated them on hardware; the reasoning is
 * summarised next to each. Runtime-tunable behaviour lives in mvlo.ini on the
 * SD card instead (see nx_config.h), so a test cycle never needs a rebuild.
 *
 * Target: MvLO v2.2.0.1 (com.ipodtouch0218.NSMBMarioVsLuigi), Unity 6000.3.10f1
 * IL2CPP arm64-v8a, GLES3, built from the official sources by the android CI.
 */
#ifndef __CONFIG_H__
#define __CONFIG_H__

/* ---------------------------------------------------------------- memory
 * Unity reserves address space in big regions; with the 256MB->64MB region
 * granularity patch (mvlo_unity_offsets.h) those land in the overcommit window
 * (virtual only) and the small heap-backed arena. See cloverpit_nx notes:
 *   OC WINDOW  virtual, PROT_NONE in a stack-region hole: large on purpose
 *   OC POOL    real memory backing pages touched inside the window
 *   ARENA      heap-backed spill, small on purpose */
#define MEMORY_MB 768
/* Held back from newlib for switch-mesa/nouveau: a 2 MB margin wedged the
 * compositor on the original port. MvLO is a 2D game with small render
 * targets, so 192 MB is generous. */
#define GFX_RESERVE_MB 192u
/* MUST equal MVLO_REGION_GRANULARITY_MB (mvlo_unity_offsets.h). */
#define MMAP_ARENA_ALIGN    ((size_t)64 * 1024 * 1024)
#define MMAP_ARENA_RESERVE  ((size_t)192 * 1024 * 1024)
#define OC_WINDOW_BYTES     ((size_t)2048 * 1024 * 1024)
#define OC_POOL_BYTES       ((size_t) 896 * 1024 * 1024)
#define MMAP_VIRT_RESERVE   ((size_t)6144 * 1024 * 1024)
#define OVERCOMMIT_HEAP_MB  608u

/* ------------------------------------------------------- game identity
 * Answered to the engine's JNI Context queries (getPackageName, versionName,
 * versionCode). Must match ProjectSettings of the built tag. */
#define CS_PACKAGE       "com.ipodtouch0218.NSMBMarioVsLuigi"
#define CS_VERSION_NAME  "2.2.0.1"
#define CS_VERSION_CODE  1
#define CS_APP_GUID      "00000000-0000-0000-0000-000000000000"

#define GAME_HOME   "sdmc:/switch/mvlo"
#define LOG_NAME    "sdmc:/switch/mvlo/debug.log"
#define CONFIG_NAME "mvlo.ini"

/* Boehm GC: the default is decided at RUNTIME by [debug] gc in mvlo.ini
 * (bridge = emulated POSIX signals, nx_signals.c; disable = never collect).
 * Kept as a macro only because cloverpit_boot.c's tripwire checks for it. */
#define CP_DISABLE_IL2CPP_GC 0

/* ------------------------------------------------------------------ audio
 * Unity 6 Android routes FMOD through OpenSL ES (forced by patch [A]); opensles.c
 * serves it over SDL2. Values from cloverpit_nx, which tuned them on hardware. */
#define CP_AUDIO_PERIOD_FRAMES     256    /* forced FMOD OpenSL period */
#define CP_AUDIO_UPFRONT_BUFFERS   0      /* 0 = leave FMOD's own division */
#define CP_AUDIO_FRAMES_PER_BUFFER 64     /* PROPERTY_OUTPUT_FRAMES_PER_BUFFER */
#define CP_AUDIO_CALLBACK_FRAMES   1024   /* SDL callback size */
#define CP_AUDIO_DEVICE_RATE       24000  /* matches FMOD's OpenSL rate */
#define CP_PATCH_FMOD_BUFFER_GEOMETRY 1   /* compile-time gate; runtime gate in mvlo.ini */

/* ---------------------------------------------------------------- logging */
#define DEBUG_LOG  1      /* always on; verbosity is [debug] log_level */
#define TRACE_IO   0      /* per-call file tracing: slow, IO bugs only */
#define TRACE_MMAP 0

/* ------------------------------------------------------------- resolution
 * Default render size; [video] resolution in mvlo.ini overrides. "auto" picks
 * 1920x1080 docked and 1280x720 handheld at boot. */
#define CP_FORCE_SCREEN_W 1920
#define CP_FORCE_SCREEN_H 1080

extern int screen_width;
extern int screen_height;

#endif
