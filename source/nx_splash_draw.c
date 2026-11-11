/* nx_splash_draw.c -- what the loading screen looks like.
 *
 *   0.0 s   studio card: the studio logo, pixel-sharp, on black (2 s)
 *   then    the pictures in a loop: each one slowly zooms and drifts (Ken
 *           Burns) for slide_s seconds, then cross-fades into the next;
 *           the game logo floats at the top, the studio logo sits in the
 *           bottom-right corner; at the bottom, "CHARGEMENT..." with the
 *           percentage, a rounded progress bar with a moving shine, and one
 *           dot per picture.
 *
 * Without pictures it falls back to a night-blue gradient; without the logo
 * to the pixel-font title. Every animation is a function of time, not of
 * frames: the screen only gets a frame when Unity's GL thread is idle, so
 * frames come irregularly and nothing may depend on their count.
 *
 * One shader does everything: flat colour, textured quad (tinted), and
 * anti-aliased rounded rectangles (signed distance in pixels).
 *
 * MIT -- part of mvlo_nx. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <GLES2/gl2.h>

#include "nx_splash_draw.h"

#define FADE_S     1.1      /* picture cross-fade */
#define INTRO_S    2.0      /* studio card */
#define DESIGN_H   720.0f   /* layout is written for 1280x720, then scaled */

static GLuint g_prog;
static GLint  g_loc_scr, g_loc_tex;

/* ---- maths ----------------------------------------------------------------- */
static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
static float smooth(float e0, float e1, float x) {
  float t = clampf((x - e0) / (e1 - e0), 0, 1);
  return t * t * (3 - 2 * t);
}
static float ease_out_back(float x) {
  const float c1 = 1.70158f, c3 = c1 + 1;
  x = clampf(x, 0, 1) - 1;
  return 1 + c3 * x * x * x + c1 * x * x;
}

/* ---- 5x7 pixel font (the glyphs the screen uses) --------------------------- */
typedef struct { char c; uint8_t row[7]; } Glyph;
static const Glyph k_font[] = {
  {'A',{0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}}, {'C',{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}},
  {'D',{0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}}, {'E',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}},
  {'G',{0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}}, {'H',{0x11,0x11,0x11,0x1F,0x11,0x11,0x11}},
  {'I',{0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}}, {'L',{0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},
  {'M',{0x11,0x1B,0x15,0x15,0x11,0x11,0x11}}, {'N',{0x11,0x19,0x15,0x13,0x11,0x11,0x11}},
  {'O',{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}}, {'R',{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},
  {'S',{0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}}, {'T',{0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
  {'U',{0x11,0x11,0x11,0x11,0x11,0x11,0x0E}}, {'V',{0x11,0x11,0x11,0x11,0x11,0x0A,0x04}},
  {'X',{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}}, {'.',{0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}},
  {'%',{0x18,0x19,0x02,0x04,0x08,0x13,0x03}}, {'0',{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}},
  {'1',{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}}, {'2',{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}},
  {'3',{0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}}, {'4',{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}},
  {'5',{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}}, {'6',{0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}},
  {'7',{0x1F,0x01,0x02,0x04,0x08,0x08,0x08}}, {'8',{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}},
  {'9',{0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}},
};
static const uint8_t *glyph(char c) {
  for (size_t i = 0; i < sizeof k_font / sizeof *k_font; i++) if (k_font[i].c == c) return k_font[i].row;
  return NULL;
}

/* ---- batcher ------------------------------------------------------------------
 * Vertex: position (px), uv, colour, extra = (half width, half height, corner
 * radius, mode). Mode 0 flat, 1 texture x colour, 2 rounded rectangle whose
 * uv holds the pixel offset from its centre. */
enum { M_FLAT = 0, M_TEX = 1, M_ROUND = 2 };
typedef struct { float x, y, u, v, r, g, b, a, ex, ey, rad, mode; } Vtx;
#define MAXV 12000
static Vtx      g_vb[MAXV];
static int      g_n;
static unsigned g_btex;

/* Screen mapping: design units -> pixels. */
static float g_S, g_ox, g_W;     /* scale, x offset, design width */
#define PX(x) ((x) * g_S + g_ox)
#define PY(y) ((y) * g_S)

static void flush(void) {
  if (!g_n) return;
  glBindTexture(GL_TEXTURE_2D, g_btex);
  const GLsizei st = sizeof(Vtx);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, st, &g_vb[0].x);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, st, &g_vb[0].u);
  glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, st, &g_vb[0].r);
  glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, st, &g_vb[0].ex);
  glDrawArrays(GL_TRIANGLES, 0, g_n);
  g_n = 0;
}
static void use_tex(unsigned tex) {
  if (tex != g_btex) { flush(); g_btex = tex; }
}
/* One quad in PIXELS. ct = colour of the top edge, cb = bottom edge. */
static void quad(float x, float y, float w, float h, float u0, float v0, float u1, float v1,
                 const float ct[4], const float cb[4], int mode, float rad) {
  if (g_n + 6 > MAXV) flush();
  const float hx = w / 2, hy = h / 2;
  const float xs[4] = { x, x + w, x, x + w }, ys[4] = { y, y, y + h, y + h };
  float us[4], vs[4];
  if (mode == M_ROUND) {
    const float lu[4] = { -hx, hx, -hx, hx }, lv[4] = { -hy, -hy, hy, hy };
    memcpy(us, lu, sizeof us); memcpy(vs, lv, sizeof vs);
  } else {
    const float tu[4] = { u0, u1, u0, u1 }, tv[4] = { v0, v0, v1, v1 };
    memcpy(us, tu, sizeof us); memcpy(vs, tv, sizeof vs);
  }
  static const int idx[6] = { 0, 1, 2, 1, 3, 2 };
  for (int k = 0; k < 6; k++) {
    int i = idx[k];
    const float *c = i < 2 ? ct : cb;
    Vtx *p = &g_vb[g_n++];
    p->x = xs[i]; p->y = ys[i]; p->u = us[i]; p->v = vs[i];
    p->r = c[0]; p->g = c[1]; p->b = c[2]; p->a = c[3];
    p->ex = hx; p->ey = hy; p->rad = rad; p->mode = (float)mode;
  }
}

/* Design-unit helpers. */
static void d_rect(float x, float y, float w, float h, const float ct[4], const float cb[4]) {
  quad(PX(x), PY(y), w * g_S, h * g_S, 0, 0, 0, 0, ct, cb, M_FLAT, 0);
}
static void d_round(float x, float y, float w, float h, float rad, const float ct[4], const float cb[4]) {
  /* one pixel of padding so the anti-aliased edge is not cut */
  quad(PX(x) - 1, PY(y) - 1, w * g_S + 2, h * g_S + 2, 0, 0, 0, 0, ct, cb, M_ROUND, rad * g_S);
  Vtx *v = &g_vb[g_n - 6];
  for (int k = 0; k < 6; k++) { v[k].ex -= 1; v[k].ey -= 1; }
}
static void d_tex(unsigned tex, float x, float y, float w, float h,
                  float u0, float v0, float u1, float v1, const float c[4]) {
  use_tex(tex);
  quad(PX(x), PY(y), w * g_S, h * g_S, u0, v0, u1, v1, c, c, M_TEX, 0);
}

static float text_w(const char *s, float px) { return (float)strlen(s) * 6 * px - px; }
static void text(const char *s, float x, float y, float px, const float c[4], float shadow_a) {
  const float sh[4] = { 0, 0, 0, shadow_a * c[3] };
  /* snap to whole screen pixels: the font is pixel art */
  const float p = fmaxf(1, floorf(px * g_S + 0.5f)) / g_S;
  x = floorf(x * g_S + 0.5f) / g_S; y = floorf(y * g_S + 0.5f) / g_S;
  for (; *s; s++, x += 6 * p) {
    const uint8_t *g = glyph(*s);
    if (!g) continue;
    for (int r = 0; r < 7; r++)
      for (int b = 0; b < 5; b++)
        if (g[r] & (0x10 >> b)) {
          if (shadow_a > 0) d_rect(x + b * p + p * 0.5f, y + r * p + p * 0.5f, p, p, sh, sh);
          d_rect(x + b * p, y + r * p, p, p, c, c);
        }
  }
}

/* Pixel-art scale: the largest k <= want for which one texel covers a whole
 * number of screen pixels. */
static float pixel_scale(float want) { return fmaxf(1, floorf(want * g_S)) / g_S; }

/* ---- GL ---------------------------------------------------------------------- */
static GLuint compile(GLenum type, const char *src, char *err, int errlen) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) { glGetShaderInfoLog(s, errlen, NULL, err); return 0; }
  return s;
}

int splash_gl_init(char *err, int errlen) {
  static const char *vs =
    "attribute vec2 p; attribute vec2 t; attribute vec4 c; attribute vec4 e;\n"
    "uniform vec2 scr; varying vec2 vt; varying vec4 vc; varying vec4 ve;\n"
    "void main() {\n"
    "  vt = t; vc = c; ve = e;\n"
    "  gl_Position = vec4(p.x / scr.x * 2.0 - 1.0, 1.0 - p.y / scr.y * 2.0, 0.0, 1.0);\n"
    "}\n";
  static const char *fs =
    "#ifdef GL_FRAGMENT_PRECISION_HIGH\nprecision highp float;\n#else\nprecision mediump float;\n#endif\n"
    "uniform sampler2D tex; varying vec2 vt; varying vec4 vc; varying vec4 ve;\n"
    "void main() {\n"
    /* No branches: Eden's shader recompiler (the CI emulator) aborts on the
     * early-return flow they compile to ("PRET flow analysis"). */
    "  float is_tex = step(0.5, ve.w) * (1.0 - step(1.5, ve.w));\n"
    "  float is_round = step(1.5, ve.w);\n"
    "  vec2 q = abs(vt) - ve.xy + vec2(ve.z);\n"
    "  float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - ve.z;\n"
    "  vec4 c = vc * mix(vec4(1.0), texture2D(tex, vt), is_tex);\n"
    "  c.a *= mix(1.0, clamp(0.5 - d, 0.0, 1.0), is_round);\n"
    "  gl_FragColor = c;\n"
    "}\n";
  if (err && errlen) err[0] = 0;
  GLuint v = compile(GL_VERTEX_SHADER, vs, err, errlen);
  GLuint f = v ? compile(GL_FRAGMENT_SHADER, fs, err, errlen) : 0;
  if (!v || !f) return 0;
  g_prog = glCreateProgram();
  glAttachShader(g_prog, v);
  glAttachShader(g_prog, f);
  glBindAttribLocation(g_prog, 0, "p");
  glBindAttribLocation(g_prog, 1, "t");
  glBindAttribLocation(g_prog, 2, "c");
  glBindAttribLocation(g_prog, 3, "e");
  glLinkProgram(g_prog);
  GLint ok = 0; glGetProgramiv(g_prog, GL_LINK_STATUS, &ok);
  if (!ok) { glGetProgramInfoLog(g_prog, errlen, NULL, err); return 0; }
  g_loc_scr = glGetUniformLocation(g_prog, "scr");
  g_loc_tex = glGetUniformLocation(g_prog, "tex");
  return 1;
}

unsigned splash_upload(const uint8_t *px, int w, int h, int comp, int nearest) {
  GLuint t = 0;
  glGenTextures(1, &t);
  glBindTexture(GL_TEXTURE_2D, t);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  GLenum fmt = comp == 4 ? GL_RGBA : GL_RGB;
  glTexImage2D(GL_TEXTURE_2D, 0, fmt, w, h, 0, fmt, GL_UNSIGNED_BYTE, px);
  GLint f = nearest ? GL_NEAREST : GL_LINEAR;
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  g_btex = (unsigned)-1;         /* the batcher must rebind */
  return t;
}

/* ---- pieces of the picture --------------------------------------------------- */

/* One picture filling the screen ("cover"), zoomed and drifting. */
static void draw_slide(const SplashScene *sc, const SplashTex *s, double t0, unsigned seq,
                       double t, float alpha) {
  if (!s->tex || alpha <= 0) return;
  static const float dir[4][2] = { { 1.0f, 0.35f }, { -1.0f, -0.25f }, { 0.45f, -1.0f }, { -0.6f, 1.0f } };
  const float life = sc->slide_s + 2 * (float)FADE_S;
  const float p = clampf((float)(t - t0) / life, 0, 1);
  const float zoom = 1.06f + 0.10f * p;
  const float scr_a = g_W / DESIGN_H, img_a = (float)s->w / (float)s->h;
  float fw = 1, fh = 1;                         /* visible fraction at zoom 1 */
  if (img_a > scr_a) fw = scr_a / img_a; else fh = img_a / scr_a;
  const float hw = fw / zoom / 2, hh = fh / zoom / 2;
  const float *d = dir[seq & 3];
  const float cx = 0.5f + d[0] * (0.5f - hw) * 0.9f * (p * 2 - 1);
  const float cy = 0.5f + d[1] * (0.5f - hh) * 0.9f * (p * 2 - 1);
  const float c[4] = { 1, 1, 1, alpha };
  d_tex(s->tex, 0, 0, g_W, DESIGN_H, cx - hw, cy - hh, cx + hw, cy + hh, c);
}

/* Advances the carousel. */
static void carousel(SplashScene *sc, double t) {
  if (sc->nready <= 0 || t < sc->main_t0) return;
  if (sc->cur < 0 && !sc->trans) {             /* first picture fades in */
    sc->nxt = 0; sc->nxt_t0 = t; sc->nxt_seq = sc->seq++; sc->trans = 1;
  }
  if (sc->trans && t - sc->nxt_t0 >= FADE_S) {
    sc->cur = sc->nxt; sc->cur_t0 = sc->nxt_t0; sc->cur_seq = sc->nxt_seq; sc->trans = 0;
  }
  const int n = sc->nexpected > sc->nready ? sc->nexpected : sc->nready;
  if (!sc->trans && sc->cur >= 0 && n > 1 && t - sc->cur_t0 >= FADE_S + sc->slide_s) {
    int k = (sc->cur + 1) % n;
    if (k < sc->nready) {                       /* else: hold until it is decoded */
      sc->nxt = k; sc->nxt_t0 = t; sc->nxt_seq = sc->seq++; sc->trans = 1;
    }
  }
}

static void draw_title(const SplashScene *sc, double t, float a) {
  if (a <= 0) return;
  const float e = (float)(t - sc->main_t0);
  const float k = 0.86f + 0.14f * ease_out_back(e / 0.9f);
  const float bob = sinf((float)t * 1.6f) * 4.0f;
  if (sc->title.tex) {
    const float w = 560 * k, h = w * sc->title.h / sc->title.w;
    const float x = (g_W - w) / 2, y = 26 + (1 - k) * 60 + bob;
    const float shadow[4] = { 0, 0, 0, 0.35f * a }, white[4] = { 1, 1, 1, a };
    d_tex(sc->title.tex, x + 3, y + 8, w, h, 0, 0, 1, 1, shadow);
    d_tex(sc->title.tex, x, y, w, h, 0, 0, 1, 1, white);
  } else {
    const float white[4] = { 1, 1, 1, a }, gold[4] = { 1.0f, 0.82f, 0.25f, a };
    const char *t1 = "MARIO VS LUIGI", *t2 = "ONLINE";
    text(t1, (g_W - text_w(t1, 9)) / 2, 120 + bob, 9, white, 0.55f);
    text(t2, (g_W - text_w(t2, 7)) / 2, 120 + 90 + bob, 7, gold, 0.55f);
  }
}

static void draw_bottom(SplashScene *sc, double t, float dt, float a) {
  const float bw = 480, bh = 12, bx = (g_W - bw) / 2, by = 668, r = bh / 2;

  /* label: CHARGEMENT + 0..3 dots, percentage on the right */
  const float white[4] = { 1, 1, 1, a }, dim[4] = { 1, 1, 1, 0.80f * a };
  char lab[24];
  int dots = (int)(t * 2.5) % 4;
  snprintf(lab, sizeof lab, "%s%.*s", sc->french ? "CHARGEMENT" : "LOADING", dots, "...");
  text(lab, bx, by - 30, 3, white, 0.6f);

  if (sc->progress >= 0) {
    float target = clampf(sc->progress, 0, 0.99f);
    if (target > sc->shown_progress)
      sc->shown_progress += (target - sc->shown_progress) * (1 - expf(-dt * 3.5f));
    char pc[8];
    snprintf(pc, sizeof pc, "%d%%", (int)(sc->shown_progress * 100 + 0.5f));
    text(pc, bx + bw - text_w(pc, 3), by - 30, 3, dim, 0.6f);
  }

  /* track: thin light rim, dark inside */
  const float rim[4] = { 1, 1, 1, 0.30f * a }, in[4] = { 0, 0, 0, 0.55f * a };
  d_round(bx - 2, by - 2, bw + 4, bh + 4, r + 2, rim, rim);
  d_round(bx, by, bw, bh, r, in, in);

  const float gt[4] = { 1.00f, 0.88f, 0.36f, a }, gb[4] = { 0.98f, 0.62f, 0.10f, a };
  float f0, f1;                                  /* filled span */
  if (sc->progress >= 0) {
    f0 = bx; f1 = bx + bw * sc->shown_progress;
  } else {                                       /* unknown: a gliding segment */
    float u = 0.5f - 0.5f * cosf((float)t * 1.9f);
    float seg = bw * 0.28f;
    f0 = bx + u * (bw - seg); f1 = f0 + seg;
  }
  if (f1 - f0 > 2 * r - 0.5f) {
    d_round(f0, by, f1 - f0, bh, r, gt, gb);
    /* shine sweeping across the filled part */
    const float sw = 70, period = bw + 2 * sw;
    float sx = bx - sw + fmodf((float)t * 320.0f, period);
    float s0 = fmaxf(sx, f0 + r), s1 = fminf(sx + sw, f1 - r);
    if (s1 - s0 > 1) {
      const float sh[4] = { 1, 1, 1, 0.38f * a }, sh2[4] = { 1, 1, 1, 0.0f };
      d_round(s0, by + 1, s1 - s0, bh * 0.45f, bh * 0.22f, sh, sh2);
    }
  }

  /* one dot per picture */
  const int n = sc->nexpected > sc->nready ? sc->nexpected : sc->nready;
  if (n > 1) {
    const float gap = 16, y = by + bh + 16;
    float x = (g_W - gap * (n - 1)) / 2;
    float fade = sc->trans ? smooth(0, (float)FADE_S, (float)(t - sc->nxt_t0)) : 0;
    for (int i = 0; i < n; i++, x += gap) {
      float on = 0;
      if (i == sc->cur) on = 1 - fade;
      if (sc->trans && i == sc->nxt) on = sc->cur < 0 ? fade : fmaxf(on, fade);
      float rad = 3.0f + 1.5f * on;
      const float c[4] = { 1, 1, 1, (0.30f + 0.70f * on) * a };
      d_round(x - rad, y - rad, 2 * rad, 2 * rad, rad, c, c);
    }
  }
}

/* ---- the frame --------------------------------------------------------------- */
void splash_draw(SplashScene *sc, double t, int w, int h) {
  if (!sc->started) {
    sc->started = 1; sc->last_t = t; sc->cur = sc->nxt = -1;
    if (sc->intro == 0) { sc->intro_t0 = -1; sc->main_t0 = 0; }
    if (sc->slide_s < 1) sc->slide_s = 5;
  }
  float dt = (float)(t - sc->last_t);
  if (dt < 0) dt = 0;
  sc->last_t = t;

  g_S = h / DESIGN_H; g_W = w / g_S; g_ox = 0;

  /* studio card: starts when its logo is there, unless that takes too long */
  if (sc->intro == 0) {
    if (sc->studio.tex && t < 0.8) { sc->intro = 1; sc->intro_t0 = t; sc->main_t0 = t + INTRO_S - 0.4; }
    else if (!sc->studio_pending || t >= 0.8) { sc->intro = 2; sc->main_t0 = t; }
  }
  if (sc->intro == 1 && t >= sc->intro_t0 + INTRO_S) sc->intro = 2;
  carousel(sc, t);

  glViewport(0, 0, w, h);
  glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST);
  glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
  glUseProgram(g_prog);
  glUniform2f(g_loc_scr, (float)w, (float)h);
  glUniform1i(g_loc_tex, 0);
  glActiveTexture(GL_TEXTURE0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  for (int i = 0; i < 4; i++) glEnableVertexAttribArray(i);
  g_btex = 0; g_n = 0;

  /* main scene, faded in after the studio card */
  const float ma = smooth((float)sc->main_t0, (float)sc->main_t0 + 0.6f, (float)t);
  if (t >= sc->main_t0) {
    const float top[4] = { 0.06f, 0.13f, 0.32f, 1 }, bot[4] = { 0.02f, 0.04f, 0.12f, 1 };
    d_rect(0, 0, g_W, DESIGN_H, top, bot);
    if (sc->cur >= 0) draw_slide(sc, &sc->slide[sc->cur], sc->cur_t0, sc->cur_seq, t, 1);
    if (sc->trans) draw_slide(sc, &sc->slide[sc->nxt], sc->nxt_t0, sc->nxt_seq, t,
                              smooth(0, (float)FADE_S, (float)(t - sc->nxt_t0)));
    /* shade behind the logo and the bar */
    const float k0[4] = { 0, 0, 0, 0.50f }, k1[4] = { 0, 0, 0, 0 }, k2[4] = { 0, 0, 0, 0.88f };
    d_rect(0, 0, g_W, 300, k0, k1);
    d_rect(0, 430, g_W, 290, k1, k2);
    draw_title(sc, t, smooth((float)sc->main_t0 + 0.1f, (float)sc->main_t0 + 0.8f, (float)t));
    if (sc->studio.tex) {                       /* studio logo, corner, whole pixels */
      const float k = pixel_scale(2), sw = sc->studio.w * k, shh = sc->studio.h * k;
      const float c[4] = { 1, 1, 1, 0.80f };
      d_tex(sc->studio.tex, floorf((g_W - sw - 28) * g_S) / g_S, floorf((DESIGN_H - shh - 26) * g_S) / g_S,
            sw, shh, 0, 0, 1, 1, c);
    }
    draw_bottom(sc, t, dt, 1);
    /* fade from black */
    const float b[4] = { 0, 0, 0, 1 - ma };
    if (ma < 1) d_rect(0, 0, g_W, DESIGN_H, b, b);
  }

  /* studio card on top while it plays */
  if (sc->intro == 1 || (sc->intro_t0 >= 0 && t < sc->main_t0 + 0.6)) {
    const float e = (float)(t - sc->intro_t0);
    const float la = smooth(0.15f, 0.55f, e) * (1 - smooth(INTRO_S - 0.55f, INTRO_S - 0.15f, e));
    if (t < sc->main_t0) {
      const float b[4] = { 0, 0, 0, 1 };
      d_rect(0, 0, g_W, DESIGN_H, b, b);
    }
    if (la > 0) {
      const float k = pixel_scale(3), sw = sc->studio.w * k, shh = sc->studio.h * k;
      const float c[4] = { 1, 1, 1, la };
      d_tex(sc->studio.tex, floorf((g_W - sw) / 2 * g_S) / g_S, floorf((DESIGN_H - shh) / 2 * g_S) / g_S,
            sw, shh, 0, 0, 1, 1, c);
    }
  }
  flush();
}
