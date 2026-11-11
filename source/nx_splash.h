/* nx_splash.h -- loading screen during the engine's first frames (nx_splash.c).
 * MIT -- part of mvlo_nx. */
#ifndef NX_SPLASH_H
#define NX_SPLASH_H
#include <EGL/egl.h>
int  nx_splash_enabled(void);                       /* [video] loading_screen */
void nx_splash_set_phase(int active);               /* main loop: on before frame 0, off after frame 1 */
void nx_splash_tick(void);                          /* cheap; draws only on the GL thread, <= 30 Hz */
void nx_splash_on_surface(EGLDisplay d, EGLConfig cfg, EGLSurface s);
void nx_splash_on_make_current(EGLDisplay d, EGLSurface draw, EGLContext ctx);
void nx_splash_early_start(void);                  /* main(): own window surface when Unity's config is known */
int  nx_splash_early_handover(EGLConfig unity_cfg); /* imports.c: Unity asks for the window surface */
void nx_splash_note_unity_config(EGLDisplay d, EGLConfig cfg);
#endif
