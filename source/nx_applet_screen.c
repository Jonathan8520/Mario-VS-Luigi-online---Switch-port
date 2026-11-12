/* nx_applet_screen.c -- the screen shown when the game is started in applet
 * mode (hbmenu opened from the Album).
 *
 * The process only gets a few hundred MB there, so nothing heavy may run: no
 * EGL/mesa, no engine. It draws one picture on the CPU into a libnx
 * framebuffer:
 *   - background: the first loading-screen slide (switch/mvlo/splash/), cover
 *     scaled and darkened, or a plain gradient when it is missing;
 *   - the game logo (splash/title.png), or the name in text;
 *   - one line saying what to do (hold R while starting a game), one saying
 *     why, and a footer like the system's: version, (A)/(B) Exit.
 * Text uses the console's own system font (pl shared font, the one hbmenu and
 * the HOME menu use) rasterised by stb_truetype. French when the console is in
 * French, English otherwise. A, B or + returns to hbmenu.
 *
 * If the font or the framebuffer is unavailable, it falls back to
 * fatal_error()'s text console with the same instructions.
 *
 * MIT -- part of mvlo_nx. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "nx_applet_screen.h"
#include "cloverpit_root.h"
#include "error.h"
#include "util.h"
#include "version.h"
#include "third_party/stb_image.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "third_party/stb_truetype.h"
#pragma GCC diagnostic pop

#define W 1280
#define H 720

static uint8_t       *g_cv;          /* W*H RGBA, alpha always 255 */
static stbtt_fontinfo g_font;
static unsigned char  g_glyph[160 * 160];

/* ------------------------------------------------------------------ pixels */
static inline void put(int x, int y, uint32_t rgb, int a) {
  if ((unsigned)x >= W || (unsigned)y >= H || a <= 0) return;
  uint8_t *p = g_cv + ((size_t)y * W + x) * 4;
  const int r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
  if (a >= 255) { p[0] = r; p[1] = g; p[2] = b; return; }
  p[0] = (uint8_t)((p[0] * (255 - a) + r * a) / 255);
  p[1] = (uint8_t)((p[1] * (255 - a) + g * a) / 255);
  p[2] = (uint8_t)((p[2] * (255 - a) + b * a) / 255);
}

static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

/* Anti-aliased rounded rectangle [x0,x1) x [y0,y1). */
static void round_rect(float x0, float y0, float x1, float y1, float r, uint32_t rgb, int a) {
  for (int y = (int)floorf(y0); y < (int)ceilf(y1); y++)
    for (int x = (int)floorf(x0); x < (int)ceilf(x1); x++) {
      const float cx = x + 0.5f, cy = y + 0.5f;
      const float dx = fmaxf(fmaxf(x0 + r - cx, cx - (x1 - r)), 0.f);
      const float dy = fmaxf(fmaxf(y0 + r - cy, cy - (y1 - r)), 0.f);
      const float d  = sqrtf(dx * dx + dy * dy) - r;
      /* plus the straight edges' own coverage */
      const float ex = fminf(cx - x0, x1 - cx), ey = fminf(cy - y0, y1 - cy);
      const float cov = clamp01(0.5f - d) * clamp01(ex + 0.5f) * clamp01(ey + 0.5f);
      put(x, y, rgb, (int)(a * cov));
    }
}

static void circle(float cx, float cy, float r, uint32_t rgb, int a) {
  round_rect(cx - r, cy - r, cx + r, cy + r, r, rgb, a);
}

/* Bilinear sample of an RGB(A) image, comp 3 or 4; out = r,g,b,a. */
static void sample(const uint8_t *img, int iw, int ih, int comp, float u, float v, float out[4]) {
  if (u < 0) u = 0;
  if (v < 0) v = 0;
  if (u > iw - 1) u = (float)(iw - 1);
  if (v > ih - 1) v = (float)(ih - 1);
  const int x0 = (int)u, y0 = (int)v;
  const int x1 = x0 + 1 < iw ? x0 + 1 : x0, y1 = y0 + 1 < ih ? y0 + 1 : y0;
  const float fx = u - x0, fy = v - y0;
  for (int c = 0; c < 4; c++) {
    if (c == 3 && comp < 4) { out[3] = 255.f; break; }
    const float p00 = img[((size_t)y0 * iw + x0) * comp + c], p10 = img[((size_t)y0 * iw + x1) * comp + c];
    const float p01 = img[((size_t)y1 * iw + x0) * comp + c], p11 = img[((size_t)y1 * iw + x1) * comp + c];
    out[c] = (p00 * (1 - fx) + p10 * fx) * (1 - fy) + (p01 * (1 - fx) + p11 * fx) * fy;
  }
}

/* Background: the picture scaled to cover the screen, darkened, with a
 * vertical shade so the footer reads well. */
static void draw_background(const uint8_t *img, int iw, int ih) {
  const float s  = img ? fmaxf((float)W / iw, (float)H / ih) : 1.f;
  const float ox = img ? (iw * s - W) / 2 : 0, oy = img ? (ih * s - H) / 2 : 0;
  for (int y = 0; y < H; y++) {
    const float t = (float)y / (H - 1);
    for (int x = 0; x < W; x++) {
      uint8_t *p = g_cv + ((size_t)y * W + x) * 4;
      float rgb[4];
      if (img) {
        sample(img, iw, ih, 3, (x + 0.5f + ox) / s - 0.5f, (y + 0.5f + oy) / s - 0.5f, rgb);
        const float k = 0.42f - 0.22f * t;           /* darker towards the bottom */
        rgb[0] *= k; rgb[1] *= k; rgb[2] *= k;
      } else {                                        /* #262645 -> #0e0e1c */
        rgb[0] = 38 + (14 - 38) * t; rgb[1] = 38 + (14 - 38) * t; rgb[2] = 69 + (28 - 69) * t;
      }
      p[0] = (uint8_t)rgb[0]; p[1] = (uint8_t)rgb[1]; p[2] = (uint8_t)rgb[2]; p[3] = 255;
    }
  }
}

/* RGBA image fitted into a box, centred, alpha-blended. */
static void draw_fit(const uint8_t *img, int iw, int ih, float bx, float by, float bw, float bh) {
  const float s = fminf(bw / iw, bh / ih);
  const float dw = iw * s, dh = ih * s, dx = bx + (bw - dw) / 2, dy = by + (bh - dh) / 2;
  for (int y = (int)dy; y < (int)(dy + dh); y++)
    for (int x = (int)dx; x < (int)(dx + dw); x++) {
      float c[4];
      sample(img, iw, ih, 4, (x + 0.5f - dx) / s - 0.5f, (y + 0.5f - dy) / s - 0.5f, c);
      put(x, y, ((uint32_t)c[0] << 16) | ((uint32_t)c[1] << 8) | (uint32_t)c[2], (int)c[3]);
    }
}

/* -------------------------------------------------------------------- text */
static uint32_t utf8_next(const char **s) {
  const unsigned char *p = (const unsigned char *)*s;
  uint32_t c = *p++;
  int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
  if (extra) c &= 0x3F >> extra;
  while (extra-- && (*p & 0xC0) == 0x80) c = (c << 6) | (*p++ & 0x3F);
  *s = (const char *)p;
  return c;
}

/* Draws (or only measures, draw = 0) one line at baseline `base`; returns its
 * width. "\x01" followed by a letter draws a controller button: a light disc
 * with the letter, like the system's own button glyphs. */
static int text(float x, float base, float px, const char *s, uint32_t rgb, int a, int draw) {
  const float sc = stbtt_ScaleForPixelHeight(&g_font, px);
  float pen = x;
  int prev = 0;
  while (*s) {
    if (*s == '\x01' && s[1]) {
      const float r = px * 0.46f, cx = pen + r + 1, cy = base - px * 0.33f;
      if (draw) {
        const char l[2] = { s[1], 0 };
        const float lp = px * 0.66f;
        circle(cx, cy, r, 0xEDEDF2, a);
        const float lw = (float)text(0, 0, lp, l, 0, 0, 0);
        text(cx - lw / 2, cy + lp * 0.35f, lp, l, 0x1B1B24, a, 1);
      }
      pen += 2 * r + 2 + px * 0.04f;
      s += 2;
      prev = 0;
      continue;
    }
    const uint32_t cp = utf8_next(&s);
    const int g = stbtt_FindGlyphIndex(&g_font, (int)cp);
    if (prev) pen += sc * stbtt_GetGlyphKernAdvance(&g_font, prev, g);
    int adv, lsb;
    stbtt_GetGlyphHMetrics(&g_font, g, &adv, &lsb);
    if (draw) {
      const float fx = floorf(pen), sx = pen - fx;
      int x0, y0, x1, y1;
      stbtt_GetGlyphBitmapBoxSubpixel(&g_font, g, sc, sc, sx, 0, &x0, &y0, &x1, &y1);
      const int gw = x1 - x0, gh = y1 - y0;
      if (gw > 0 && gh > 0 && (size_t)gw * gh <= sizeof g_glyph) {
        stbtt_MakeGlyphBitmapSubpixel(&g_font, g_glyph, gw, gh, gw, sc, sc, sx, 0, g);
        for (int yy = 0; yy < gh; yy++)
          for (int xx = 0; xx < gw; xx++)
            put((int)fx + x0 + xx, (int)base + y0 + yy, rgb, g_glyph[yy * gw + xx] * a / 255);
      }
    }
    pen += adv * sc;
    prev = g;
  }
  return (int)(pen - x + 0.5f);
}

/* ----------------------------------------------------------------- content */
typedef struct {
  const char *line1, *line2, *exit;
} Strings;

static const Strings EN = {
  "To play, hold \x01R while starting any game",
  "Started from the Album, the game doesn’t get enough memory.",
  "Exit",
};

static const Strings FR = {
  "Pour jouer, maintenez \x01R en lançant n’importe quel jeu",
  "Lancé depuis l’Album, le jeu n’a pas assez de mémoire.",
  "Quitter",
};

static uint8_t *load_image(const char *name, int comp, int *w, int *h) {
  char path[300];
  snprintf(path, sizeof path, "%s/splash/%s", cp_game_root(), name);
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *buf = (n > 0 && n < 16 * 1024 * 1024) ? malloc((size_t)n) : NULL;
  uint8_t *px = NULL;
  int c;
  if (buf && fread(buf, 1, (size_t)n, f) == (size_t)n)
    px = stbi_load_from_memory(buf, (int)n, w, h, &c, comp);
  free(buf);
  fclose(f);
  return px;
}

static void text_centred(float base, float px, const char *s, uint32_t rgb) {
  const int w = text(0, 0, px, s, 0, 0, 0);
  text((W - w) / 2.f, base, px, s, rgb, 255, 1);
}

static void compose(const Strings *t) {
  int iw = 0, ih = 0;
  uint8_t *bg = load_image("slide1.jpg", 3, &iw, &ih);
  if (!bg) bg = load_image("slide1.png", 3, &iw, &ih);
  draw_background(bg, iw, ih);
  free(bg);

  /* the game logo, or its name */
  uint8_t *logo = load_image("title.png", 4, &iw, &ih);
  if (logo) {
    draw_fit(logo, iw, ih, 340, 110, 600, 240);
    free(logo);
  } else {
    text_centred(270, 64, "Mario vs Luigi Online", 0xFFFFFF);
  }

  /* what to do, and why */
  text_centred(458, 36, t->line1, 0xFFFFFF);
  text_centred(504, 25, t->line2, 0xB4B4C4);

  /* footer, like the system's: thin rule, version on the left, (A)/(B) Exit
   * on the right (+ works too) */
  for (int x = 40; x < W - 40; x++) put(x, 662, 0xFFFFFF, 48);
  text(52, 700, 22, "v" MVLO_NX_VERSION " (MvLO 2.2.0.1)", 0x9A9AB0, 255, 1);
  char ex[64];
  snprintf(ex, sizeof ex, "\x01" "A / " "\x01" "B %s", t->exit);
  const int ew = text(0, 0, 27, ex, 0, 0, 0);
  text(W - 52 - ew, 701, 27, ex, 0xFFFFFF, 255, 1);
}

static int system_is_french(void) {
  u64 code = 0;
  if (R_SUCCEEDED(setInitialize())) { setGetSystemLanguage(&code); setExit(); }
  return !strncmp((const char *)&code, "fr", 2);
}

/* ------------------------------------------------------------------- entry */
static const char FALLBACK[] =
  "\nMario vs Luigi Online can't run in applet mode\n"
  "(hbmenu opened from the Album): it only gets a small part\n"
  "of the console's memory there.\n\n"
  "To play:\n"
  "  1. Go back to the HOME menu.\n"
  "  2. Hold R while starting any game.\n"
  "  3. hbmenu opens with full memory: pick Mario vs Luigi Online.";

void nx_applet_mode_screen(void) {
  debugPrintf("[boot] APPLET MODE: not enough memory; showing how to launch the game "
              "(HOME, hold R while starting a game, pick Mario vs Luigi Online)\n");
  debug_log_flush();

  PlFontData fd;
  int font_ok = R_SUCCEEDED(plInitialize(PlServiceType_User)) &&
                R_SUCCEEDED(plGetSharedFontByType(&fd, PlSharedFontType_Standard)) &&
                stbtt_InitFont(&g_font, fd.address, stbtt_GetFontOffsetForIndex(fd.address, 0));
  g_cv = font_ok ? malloc((size_t)W * H * 4) : NULL;
  Framebuffer fb;
  if (!g_cv || R_FAILED(framebufferCreate(&fb, nwindowGetDefault(), W, H, PIXEL_FORMAT_RGBA_8888, 2))) {
    debugPrintf("[boot] applet screen: font=%d canvas=%d -> text fallback\n", font_ok, g_cv != NULL);
    if (font_ok) plExit();
    fatal_error("%s", FALLBACK);
  }
  framebufferMakeLinear(&fb);
  compose(system_is_french() ? &FR : &EN);

  PadState pad;
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&pad);
  while (appletMainLoop()) {
    padUpdate(&pad);
    if (padGetButtonsDown(&pad) & (HidNpadButton_A | HidNpadButton_B | HidNpadButton_Plus)) break;
    u32 stride;
    uint8_t *dst = framebufferBegin(&fb, &stride);
    for (int y = 0; y < H; y++) memcpy(dst + (size_t)y * stride, g_cv + (size_t)y * W * 4, (size_t)W * 4);
    framebufferEnd(&fb);
  }
  framebufferClose(&fb);
  free(g_cv);
  plExit();
  debugPrintf("[boot] applet screen closed, back to hbmenu\n");
  debug_log_flush();
  exit(0);
}
