/* nx_splash_draw.h -- the loading screen's picture (nx_splash_draw.c).
 *
 * Plain GLES2, no libnx: the same code runs on the console (nx_splash.c gives
 * it a context, textures and the clock) and on a PC (tools/splash_preview.c).
 *
 * MIT -- part of mvlo_nx. */
#ifndef NX_SPLASH_DRAW_H
#define NX_SPLASH_DRAW_H
#include <stdint.h>

#define SPLASH_MAX_SLIDES 8

typedef struct { unsigned tex; int w, h; } SplashTex;

typedef struct {
  /* Filled by the caller. Textures arrive while the screen is already up. */
  SplashTex slide[SPLASH_MAX_SLIDES];
  int       nready;            /* slides uploaded so far, in order */
  int       nexpected;         /* slides that exist (some may still decode) */
  SplashTex title;             /* game logo, RGBA; tex 0 = pixel-font title */
  SplashTex studio;            /* studio logo (pixel art), RGBA; tex 0 = none */
  int       studio_pending;    /* studio logo still decoding */
  float     progress;          /* 0..1, < 0 = unknown (first boot) */
  int       french;
  float     slide_s;           /* seconds each picture stays (default 5) */

  /* Animation state, owned by splash_draw(). Zero-initialise the struct. */
  int       started;
  double    last_t, intro_t0, main_t0;
  int       intro;             /* 0 undecided, 1 playing, 2 over / skipped */
  int       cur, nxt, trans;   /* slide indices, cross-fade in progress */
  double    cur_t0, nxt_t0;    /* when each started to show (Ken Burns) */
  unsigned  cur_seq, nxt_seq, seq;
  float     shown_progress;
} SplashScene;

/* Compiles the program on the current context. 0 on failure, with a reason
 * in err. */
int      splash_gl_init(char *err, int errlen);
/* Uploads pixels (comp 3 = RGB, 4 = RGBA) on the current context. */
unsigned splash_upload(const uint8_t *px, int w, int h, int comp, int nearest);
/* Draws one frame, t = seconds since the screen appeared (non-decreasing). */
void     splash_draw(SplashScene *sc, double t, int w, int h);

#endif
