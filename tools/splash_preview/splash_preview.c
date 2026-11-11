/* splash_preview.c -- renders the loading screen on a PC (Mesa, no window),
 * with the exact drawing code the console runs (source/nx_splash_draw.c).
 *
 *   tools/splash_preview/build.sh
 *   ./splash_preview <splash dir> <out dir> [width height] [progress|-1]
 *
 * <splash dir> is laid out like sdmc:/switch/mvlo/splash/ (slide*.jpg|png,
 * title.png, studio.png). Writes frame_<ms>.ppm at a few moments of a
 * simulated 24 s load (30 fps clock), progress rising linearly (or unknown
 * with -1).
 *
 * MIT -- part of mvlo_nx. */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include "nx_splash_draw.h"
#include "third_party/stb_image.h"

static uint8_t *slurp(const char *path, int *len) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  uint8_t *b = malloc(n);
  if (fread(b, 1, n, f) != (size_t)n) { free(b); b = NULL; }
  fclose(f); *len = (int)n;
  return b;
}
static SplashTex load(const char *dir, const char *name, int comp, int nearest) {
  SplashTex t = { 0, 0, 0 };
  char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, name);
  int len, c; uint8_t *b = slurp(p, &len);
  if (!b) return t;
  uint8_t *px = stbi_load_from_memory(b, len, &t.w, &t.h, &c, comp);
  free(b);
  if (!px) { fprintf(stderr, "%s: %s\n", p, stbi_failure_reason()); return t; }
  t.tex = splash_upload(px, t.w, t.h, comp, nearest);
  stbi_image_free(px);
  return t;
}
static int cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

int main(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "usage: %s <splash dir> <out dir> [w h] [progress|-1]\n", argv[0]); return 1; }
  int W = argc > 4 ? atoi(argv[3]) : 1280, H = argc > 4 ? atoi(argv[4]) : 720;
  int unknown = argc > 5 && atof(argv[5]) < 0;

  PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
  EGLDisplay d = gpd ? gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (!eglInitialize(d, NULL, NULL)) { fprintf(stderr, "eglInitialize failed\n"); return 1; }
  eglBindAPI(EGL_OPENGL_ES_API);
  static const EGLint ca[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
  EGLConfig cfg = (EGLConfig)0; EGLint n = 0;   /* EGL_NO_CONFIG_KHR is fine: we draw to an FBO */
  if (!eglChooseConfig(d, ca, &cfg, 1, &n) || !n) cfg = (EGLConfig)0;
  static const EGLint xa[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
  EGLContext c = eglCreateContext(d, cfg, EGL_NO_CONTEXT, xa);
  if (!eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, c)) { fprintf(stderr, "makecurrent failed\n"); return 1; }

  GLuint fbt, fb;
  glGenTextures(1, &fbt); glBindTexture(GL_TEXTURE_2D, fbt);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glGenFramebuffers(1, &fb); glBindFramebuffer(GL_FRAMEBUFFER, fb);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbt, 0);
  char err[512];
  if (!splash_gl_init(err, sizeof err)) { fprintf(stderr, "shader: %s\n", err); return 1; }

  SplashScene sc; memset(&sc, 0, sizeof sc);
  sc.slide_s = 5; sc.french = 1;
  sc.studio = load(argv[1], "studio.png", 4, 1);
  sc.title = load(argv[1], "title.png", 4, 0);
  char *names[64]; int nn = 0;
  DIR *dir = opendir(argv[1]); struct dirent *e;
  while (dir && (e = readdir(dir)) && nn < 64)
    if (!strncmp(e->d_name, "slide", 5)) names[nn++] = strdup(e->d_name);
  if (dir) closedir(dir);
  qsort(names, nn, sizeof *names, cmp);
  for (int i = 0; i < nn && sc.nready < SPLASH_MAX_SLIDES; i++) {
    SplashTex t = load(argv[1], names[i], 3, 0);
    if (t.tex) sc.slide[sc.nready++] = t;
  }
  sc.nexpected = sc.nready;
  printf("studio %dx%d, title %dx%d, %d slides\n", sc.studio.w, sc.studio.h, sc.title.w, sc.title.h, sc.nready);

  static const int shots[] = { 300, 1000, 1700, 2300, 3200, 6500, 7100, 7400, 12000, 20000, 23900 };
  uint8_t *px = malloc((size_t)W * H * 4), *rgb = malloc((size_t)W * H * 3);
  int next = 0;
  for (int ms = 0; ms <= 24000 && next < (int)(sizeof shots / sizeof *shots); ms += 33) {
    sc.progress = unknown ? -1 : ms / 24000.0f;
    int last = ms + 33 > shots[next];
    splash_draw(&sc, ms / 1000.0, W, H);
    if (!last) continue;
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (int y = 0; y < H; y++)               /* GL rows are bottom-up */
      for (int x = 0; x < W; x++)
        memcpy(&rgb[((size_t)y * W + x) * 3], &px[((size_t)(H - 1 - y) * W + x) * 4], 3);
    char p[1024]; snprintf(p, sizeof p, "%s/frame_%05d.ppm", argv[2], shots[next]);
    FILE *f = fopen(p, "wb");
    fprintf(f, "P6 %d %d 255\n", W, H); fwrite(rgb, 1, (size_t)W * H * 3, f); fclose(f);
    printf("wrote %s\n", p);
    next++;
  }
  return 0;
}
