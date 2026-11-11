/* nx_splash.c -- loading screen during the engine's long first frames.
 *
 * The problem: nativeRender frame 1 runs MvLO's first scene load (~23 s on a
 * Switch SD card) and Unity presents nothing until it returns, so the screen
 * stays black for ~25 s and the console looks hung.
 *
 * How it draws without disturbing Unity: Unity's GL context and window surface
 * stay current on ONE thread (its gfx device worker, or the main thread). That
 * thread spends frame 1 mostly idle, parked in our futex shim. From there,
 * nx_splash_tick() switches that thread to OUR OWN EGL context on the same
 * window surface (allowed: same thread, same surface, compatible config),
 * draws a frame, presents it, and switches back to Unity's context. Unity's GL
 * state lives in its own context and is never touched. It only runs between
 * the creation of Unity's window surface and the end of nativeRender frame 1,
 * i.e. before Unity has drawn anything, so no Unity frame can be overwritten.
 *
 * What it shows is nx_splash_draw.c (also runnable on a PC through
 * tools/splash_preview). Its pictures come from sdmc:/switch/mvlo/splash/:
 *   studio.png          studio logo (pixel art, shown sharp)
 *   title.png           game logo
 *   slide*.jpg|png      the carousel, in name order (up to 8)
 * The CI downloads and converts them (tools/fetch_splash.py); they are never
 * stored in the repository. Any of them may be missing: the screen falls back
 * to a gradient and a pixel-font title. They are decoded by a low-priority
 * thread (stb_image) and uploaded one per frame, so the screen is up at once
 * and the pictures join as they become ready.
 *
 * Progress is the SD bytes the game has read so far against what the previous
 * boot read (sdmc:/switch/mvlo/.loadstats); unknown on the very first boot.
 *
 * At the end (nativeRender frame 2) the decoder stops and our context is
 * destroyed, which frees every texture.
 *
 * [video] loading_screen = 0 turns it off; loading_slide_s (seconds per
 * picture, default 5); loading_fps (frame cap, default 60).
 *
 * MIT -- part of mvlo_nx. */
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <switch.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "nx_splash.h"
#include "nx_splash_draw.h"
#include "nx_config.h"
#include "config.h"
#include "util.h"
#include "third_party/stb_image.h"

extern int screen_width, screen_height;
extern uint64_t nxio_total_sd_bytes(void);

static volatile int g_phase;            /* 1 while the splash may draw */
static volatile int g_done;             /* set once for good */
static volatile int g_busy;             /* a tick is drawing */
static int          g_enabled = -1;
static EGLDisplay   g_dpy = EGL_NO_DISPLAY;
static EGLConfig    g_cfg;
static EGLSurface   g_surf = EGL_NO_SURFACE;
static EGLContext   g_uctx = EGL_NO_CONTEXT;     /* Unity's */
static Handle       g_owner;                     /* thread that has it current */
static EGLContext   g_ctx = EGL_NO_CONTEXT;      /* ours */
static uint64_t     g_t0, g_last, g_min_ns = 16666666ull;
static uint64_t     g_io0, g_expect;
static unsigned     g_frames;
static SplashScene  g_sc;

static uint64_t now_ns(void) { return armTicksToNs(armGetSystemTick()); }

int nx_splash_enabled(void) {
  if (g_enabled < 0) g_enabled = nxcfg_int("video.loading_screen", 1) ? 1 : 0;
  return g_enabled;
}

/* ---- EGL tracking (called from the eglCreateWindowSurface / eglMakeCurrent
 * wrappers in imports.c) ---------------------------------------------------- */
void nx_splash_on_surface(EGLDisplay d, EGLConfig cfg, EGLSurface s) {
  g_dpy = d; g_cfg = cfg; g_surf = s;
}
void nx_splash_on_make_current(EGLDisplay d, EGLSurface draw, EGLContext ctx) {
  if (ctx != EGL_NO_CONTEXT && draw != EGL_NO_SURFACE && draw == g_surf && ctx != g_ctx) {
    g_dpy = d; g_uctx = ctx; g_owner = threadGetCurHandle();
  } else if (ctx == EGL_NO_CONTEXT && threadGetCurHandle() == g_owner) {
    g_uctx = EGL_NO_CONTEXT; g_owner = 0;
  }
}

/* ---- pictures: found at phase start, decoded by a thread ------------------- */
enum { K_STUDIO, K_TITLE, K_SLIDE };
typedef struct { char path[192]; int kind; } Item;
static Item       g_items[2 + SPLASH_MAX_SLIDES];
static int        g_nitems;
static Thread     g_thr;
static int        g_thr_on;
static volatile int g_stop;
/* one-slot mailbox, decoder -> tick */
static volatile int g_mb_full;
static uint8_t   *g_mb_px;
static int        g_mb_w, g_mb_h, g_mb_comp, g_mb_kind;

static int name_cmp(const void *a, const void *b) {
  return strcmp(((const Item *)a)->path, ((const Item *)b)->path);
}
static int has_ext(const char *n, const char *e) {
  size_t a = strlen(n), b = strlen(e);
  return a > b && !strcasecmp(n + a - b, e);
}
static int file_exists(const char *p) { FILE *f = fopen(p, "rb"); if (f) fclose(f); return f != NULL; }

static void find_pictures(void) {
  char dir[64];
  snprintf(dir, sizeof dir, "%s/splash", GAME_HOME);
  g_nitems = 0;
  Item it;
  snprintf(it.path, sizeof it.path, "%s/studio.png", dir); it.kind = K_STUDIO;
  if (file_exists(it.path)) { g_items[g_nitems++] = it; g_sc.studio_pending = 1; }
  snprintf(it.path, sizeof it.path, "%s/title.png", dir); it.kind = K_TITLE;
  if (file_exists(it.path)) g_items[g_nitems++] = it;
  int first = g_nitems;
  DIR *d = opendir(dir);
  struct dirent *e;
  while (d && (e = readdir(d)) && g_nitems < first + SPLASH_MAX_SLIDES) {
    if (strncasecmp(e->d_name, "slide", 5)) continue;
    if (!has_ext(e->d_name, ".jpg") && !has_ext(e->d_name, ".jpeg") && !has_ext(e->d_name, ".png")) continue;
    snprintf(it.path, sizeof it.path, "%s/%s", dir, e->d_name); it.kind = K_SLIDE;
    g_items[g_nitems++] = it;
  }
  if (d) closedir(d);
  qsort(g_items + first, g_nitems - first, sizeof *g_items, name_cmp);
  g_sc.nexpected = g_nitems - first;
  debugPrintf("[load] pictures: %s%s%d slide(s) in %s\n", g_sc.studio_pending ? "studio logo, " : "",
              first > (g_sc.studio_pending ? 1 : 0) ? "title, " : "", g_sc.nexpected, dir);
}

/* Halves the picture until it is at most 1920 wide and 1080 high. */
static uint8_t *shrink(uint8_t *px, int *w, int *h, int comp) {
  while (*w > 1920 || *h > 1080) {
    int nw = *w / 2, nh = *h / 2;
    uint8_t *o = malloc((size_t)nw * nh * comp);
    if (!o) break;
    for (int y = 0; y < nh; y++)
      for (int x = 0; x < nw; x++)
        for (int c = 0; c < comp; c++) {
          const uint8_t *s = px + ((size_t)(2 * y) * *w + 2 * x) * comp + c;
          size_t row = (size_t)*w * comp;
          o[((size_t)y * nw + x) * comp + c] = (uint8_t)((s[0] + s[comp] + s[row] + s[row + comp] + 2) / 4);
        }
    stbi_image_free(px);
    px = o; *w = nw; *h = nh;
  }
  return px;
}

static void decoder(void *arg) {
  (void)arg;
  for (int i = 0; i < g_nitems && !g_stop; i++) {
    const Item *it = &g_items[i];
    int comp = it->kind == K_SLIDE ? 3 : 4, w = 0, h = 0, c = 0;
    uint8_t *px = NULL;
    uint64_t t0 = now_ns();
    FILE *f = fopen(it->path, "rb");
    if (f) {
      fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
      uint8_t *buf = n > 0 && n < (32l << 20) ? malloc(n) : NULL;
      if (buf && fread(buf, 1, n, f) == (size_t)n)
        px = stbi_load_from_memory(buf, (int)n, &w, &h, &c, comp);
      free(buf);
      fclose(f);
    }
    if (px) px = shrink(px, &w, &h, comp);
    if (px) debugPrintf("[load] %s: %dx%d in %llu ms\n", it->path, w, h,
                        (unsigned long long)((now_ns() - t0) / 1000000ull));
    else    debugPrintf("[load] %s: cannot decode (%s) -- skipped\n", it->path, stbi_failure_reason());
    while (g_mb_full && !g_stop) svcSleepThread(5000000ull);
    if (g_stop) { if (px) free(px); break; }
    g_mb_px = px; g_mb_w = w; g_mb_h = h; g_mb_comp = comp; g_mb_kind = it->kind;
    __atomic_store_n(&g_mb_full, 1, __ATOMIC_RELEASE);
  }
}

/* On the GL thread, our context current: at most one picture per frame. */
static void take_picture(void) {
  if (!__atomic_load_n(&g_mb_full, __ATOMIC_ACQUIRE)) return;
  SplashTex t = { 0, g_mb_w, g_mb_h };
  if (g_mb_px) {
    t.tex = splash_upload(g_mb_px, g_mb_w, g_mb_h, g_mb_comp, g_mb_kind == K_STUDIO);
    free(g_mb_px);
    g_mb_px = NULL;
  }
  switch (g_mb_kind) {
  case K_STUDIO: if (t.tex) g_sc.studio = t; g_sc.studio_pending = 0; break;
  case K_TITLE:  if (t.tex) g_sc.title = t; break;
  default:
    if (t.tex && g_sc.nready < SPLASH_MAX_SLIDES) g_sc.slide[g_sc.nready++] = t;
    else if (g_sc.nexpected > g_sc.nready) g_sc.nexpected--;
  }
  __atomic_store_n(&g_mb_full, 0, __ATOMIC_RELEASE);
}

/* ---- progress expectation ---------------------------------------------------- */
static void load_expectation(void) {
  char p[300];
  snprintf(p, sizeof p, "%s/.loadstats", GAME_HOME);
  FILE *f = fopen(p, "r");
  unsigned long long v = 0;
  if (f) { if (fscanf(f, "%llu", &v) != 1) v = 0; fclose(f); }
  g_expect = v;
}
static void save_expectation(uint64_t bytes) {
  if (bytes < (1u << 20)) return;
  char p[300];
  snprintf(p, sizeof p, "%s/.loadstats", GAME_HOME);
  FILE *f = fopen(p, "w");
  if (f) { fprintf(f, "%llu\n", (unsigned long long)bytes); fclose(f); }
}

/* Scene, clock, pictures: once, from the early screen or from frame 0. */
static int g_begun;
static void splash_begin(void) {
  if (g_begun) return;
  g_begun = 1;
  g_t0 = now_ns(); g_io0 = nxio_total_sd_bytes();
  load_expectation();
  memset(&g_sc, 0, sizeof g_sc);
  u64 code = 0;
  if (R_SUCCEEDED(setInitialize())) { setGetSystemLanguage(&code); setExit(); }
  g_sc.french = !strncmp((const char *)&code, "fr", 2);
  g_sc.slide_s = (float)nxcfg_int("video.loading_slide_s", 5);
  int fps = nxcfg_int("video.loading_fps", 60);
  if (fps < 10) fps = 10;
  if (fps > 60) fps = 60;
  g_min_ns = 1000000000ull / (uint64_t)fps;
  find_pictures();
  if (g_nitems && R_SUCCEEDED(threadCreate(&g_thr, decoder, NULL, NULL, 0x20000, 0x2E, -2))) {
    if (R_SUCCEEDED(threadStart(&g_thr))) g_thr_on = 1;
    else threadClose(&g_thr);
  }
  if (g_nitems && !g_thr_on) debugPrintf("[load] cannot start the picture decoder -- plain screen\n");
}

/* ---- public ---------------------------------------------------------------- */
void nx_splash_set_phase(int active) {
  if (g_done || !nx_splash_enabled()) return;
  if (active && !g_phase) splash_begin();
  if (!active && g_phase) {
    /* No tick may run past this point: raise done, then wait for one that
     * had already started (ticks set busy before they check done). */
    __atomic_store_n(&g_done, 1, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&g_busy, __ATOMIC_SEQ_CST)) svcSleepThread(1000000ull);
    uint64_t io = nxio_total_sd_bytes() - g_io0;
    debugPrintf("[load] loading screen: %u frames over %llu ms, %d/%d picture(s) shown (%llu MB read; previous boot %llu MB)\n",
                g_frames, (unsigned long long)((now_ns() - g_t0) / 1000000ull), g_sc.nready, g_sc.nexpected,
                (unsigned long long)(io >> 20), (unsigned long long)(g_expect >> 20));
    save_expectation(io);
    if (g_thr_on) {
      g_stop = 1;
      threadWaitForExit(&g_thr);
      threadClose(&g_thr);
      g_thr_on = 0;
    }
    if (g_mb_full && g_mb_px) { free(g_mb_px); g_mb_px = NULL; }
    if (g_ctx != EGL_NO_CONTEXT) {               /* frees our textures */
      if (!eglDestroyContext(g_dpy, g_ctx)) debugPrintf("[load] eglDestroyContext failed 0x%x\n", eglGetError());
      g_ctx = EGL_NO_CONTEXT;
    }
  }
  g_phase = active;
}

static void tick_body(void) {
  if (threadGetCurHandle() != g_owner) return;
  uint64_t now = now_ns();
  if (now - g_last < g_min_ns) return;
  g_last = now;
  /* Unity's binding on this thread, to put back afterwards. */
  EGLContext uc = eglGetCurrentContext();
  EGLSurface ud = eglGetCurrentSurface(EGL_DRAW), ur = eglGetCurrentSurface(EGL_READ);
  if (uc != g_uctx || ud != g_surf) return;     /* not in the expected state: skip */
  if (g_ctx == EGL_NO_CONTEXT) {
    static const EGLint attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    g_ctx = eglCreateContext(g_dpy, g_cfg, EGL_NO_CONTEXT, attr);
    if (g_ctx == EGL_NO_CONTEXT) {
      debugPrintf("[load] eglCreateContext failed (0x%x) -- loading screen off\n", eglGetError());
      g_done = 1; return;
    }
    char err[256];
    if (!eglMakeCurrent(g_dpy, g_surf, g_surf, g_ctx) || !splash_gl_init(err, sizeof err)) {
      eglMakeCurrent(g_dpy, ud, ur, uc);
      debugPrintf("[load] cannot use our context (0x%x) %s -- loading screen off\n", eglGetError(), err);
      g_done = 1; return;
    }
    debugPrintf("[load] loading screen on (Unity context on thread 0x%x)\n", g_owner);
  } else if (!eglMakeCurrent(g_dpy, g_surf, g_surf, g_ctx)) {
    debugPrintf("[load] eglMakeCurrent(ours) failed 0x%x -- loading screen off\n", eglGetError());
    g_done = 1; return;
  }
  take_picture();
  g_sc.progress = g_expect ? (float)(nxio_total_sd_bytes() - g_io0) / (float)g_expect : -1.0f;
  int w = screen_width > 0 ? screen_width : 1280, h = screen_height > 0 ? screen_height : 720;
  splash_draw(&g_sc, (double)(now - g_t0) / 1e9, w, h);
  eglSwapBuffers(g_dpy, g_surf);
  g_frames++;
  if (!eglMakeCurrent(g_dpy, ud, ur, uc)) {
    debugPrintf("[load] FAILED to restore Unity's context (0x%x)\n", eglGetError());
    g_done = 1;
  }
}

void nx_splash_tick(void) {
  if (!g_phase || g_done || g_uctx == EGL_NO_CONTEXT || g_surf == EGL_NO_SURFACE) return;
  if (__atomic_exchange_n(&g_busy, 1, __ATOMIC_SEQ_CST)) return;   /* another thread is in */
  if (!__atomic_load_n(&g_done, __ATOMIC_SEQ_CST)) tick_body();
  __atomic_store_n(&g_busy, 0, __ATOMIC_SEQ_CST);
}

/* ---- early start: the loading screen from the first second of boot ----------
 *
 * Until Unity creates its window surface (~4.3 s on hardware) the window has
 * no producer, and it can only ever have ONE: the framebuffer-then-mesa
 * hand-over and a second EGL window surface both broke the buffer queue (the
 * first deadlocked Unity's first frame under Eden). So when Unity's EGL config
 * is known (recorded at the previous boot in .eglconfig), this thread creates
 * THE window surface with that exact config -- mesa requires context and
 * surface to share one config -- draws the loading screen on it with our own
 * context, and when Unity calls eglCreateWindowSurface the wrapper in
 * imports.c hands it that same surface after the thread has let go of it.
 * From then on the usual ticks (Unity's GL thread) continue with the same
 * context, textures and clock.
 *
 * [video] early_splash = 0 turns it off. */
extern void nx_window_prepare(void);
extern void nx_egl_adopt_window_surface(EGLDisplay d, EGLConfig cfg, EGLSurface s, void *win);

static Thread       g_early_thr;
static volatile int g_early_on, g_early_stop;

static int config_path(char *p, size_t n) { return snprintf(p, n, "%s/.eglconfig", GAME_HOME) > 0; }

static void early_thread_entry(void *arg) {
  (void)arg;
  if (!eglMakeCurrent(g_dpy, g_surf, g_surf, g_ctx)) {
    debugPrintf("[load] early loading screen: eglMakeCurrent failed 0x%x\n", eglGetError());
    return;
  }
  int w = screen_width > 0 ? screen_width : 1280, h = screen_height > 0 ? screen_height : 720;
  while (!g_early_stop) {
    uint64_t now = now_ns();
    take_picture();
    g_sc.progress = g_expect ? (float)(nxio_total_sd_bytes() - g_io0) / (float)g_expect : -1.0f;
    splash_draw(&g_sc, (double)(now - g_t0) / 1e9, w, h);
    eglSwapBuffers(g_dpy, g_surf);
    g_frames++;
    uint64_t dt = now_ns() - now;
    if (dt < g_min_ns) svcSleepThread(g_min_ns - dt);
  }
  eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

void nx_splash_early_start(void) {
  if (!nx_splash_enabled() || !nxcfg_int("video.early_splash", 1)) return;
  char p[128]; config_path(p, sizeof p);
  int id = 0;
  FILE *f = fopen(p, "r");
  if (f) { if (fscanf(f, "%d", &id) != 1) id = 0; fclose(f); }
  if (id <= 0) { debugPrintf("[load] early loading screen: Unity's EGL config not known yet (next boot)\n"); return; }

  EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (d == EGL_NO_DISPLAY || !eglInitialize(d, NULL, NULL)) {
    debugPrintf("[load] early loading screen: EGL unavailable (0x%x)\n", eglGetError());
    return;
  }
  eglBindAPI(EGL_OPENGL_ES_API);
  const EGLint want[] = { EGL_CONFIG_ID, id, EGL_NONE };
  EGLConfig cfg; EGLint n = 0;
  if (!eglChooseConfig(d, want, &cfg, 1, &n) || n != 1) {
    debugPrintf("[load] early loading screen: EGL config %d not found -- off\n", id);
    remove(p);
    return;
  }
  nx_window_prepare();
  void *win = nwindowGetDefault();
  EGLSurface s = eglCreateWindowSurface(d, cfg, win, NULL);
  if (s == EGL_NO_SURFACE) { debugPrintf("[load] early loading screen: no window surface (0x%x)\n", eglGetError()); return; }
  nx_egl_adopt_window_surface(d, cfg, s, win);   /* the one surface Unity will get */
  static const EGLint attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
  EGLContext c = eglCreateContext(d, cfg, EGL_NO_CONTEXT, attr);
  char err[256] = "";
  if (c == EGL_NO_CONTEXT || !eglMakeCurrent(d, s, s, c) || !splash_gl_init(err, sizeof err)) {
    debugPrintf("[load] early loading screen: no context (0x%x) %s -- the surface stays for Unity\n", eglGetError(), err);
    eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (c != EGL_NO_CONTEXT) eglDestroyContext(d, c);
    return;
  }
  g_ctx = c;
  splash_begin();
  /* The drawing thread needs the context current on it: release it here. */
  eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (R_FAILED(threadCreate(&g_early_thr, early_thread_entry, NULL, NULL, 0x20000, 0x2C, -2)) ||
      R_FAILED(threadStart(&g_early_thr))) {
    debugPrintf("[load] early loading screen: no thread\n");
    return;
  }
  g_early_on = 1;
  debugPrintf("[load] early loading screen on (EGL config %d, %dx%d)\n", id, screen_width, screen_height);
}

/* imports.c, when Unity asks for its window surface. 1 = it may have ours. */
int nx_splash_early_handover(EGLConfig unity_cfg) {
  if (!g_early_on) return 1;
  g_early_stop = 1;
  threadWaitForExit(&g_early_thr);
  threadClose(&g_early_thr);
  g_early_on = 0;
  debugPrintf("[load] early loading screen handed to Unity after %llu ms, %u frames\n",
              (unsigned long long)((now_ns() - g_t0) / 1000000ull), g_frames);
  if (unity_cfg != g_cfg) {
    char p[128]; config_path(p, sizeof p); remove(p);
    debugPrintf("[load] WARNING: Unity chose another EGL config than recorded -- early screen off from next boot\n");
    return 0;
  }
  return 1;
}

/* imports.c: the config Unity renders with, kept for the next boot. */
void nx_splash_note_unity_config(EGLDisplay d, EGLConfig cfg) {
  EGLint id = 0;
  if (!eglGetConfigAttrib(d, cfg, EGL_CONFIG_ID, &id) || id <= 0) return;
  char p[128]; config_path(p, sizeof p);
  int old = 0;
  FILE *f = fopen(p, "r");
  if (f) { if (fscanf(f, "%d", &old) != 1) old = 0; fclose(f); }
  if (old == id) return;
  f = fopen(p, "w");
  if (f) { fprintf(f, "%d\n", id); fclose(f); }
}
