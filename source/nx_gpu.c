/* nx_gpu.c -- what the GPU stack is doing, and three safety knobs.
 *
 * Two problems on hardware motivate this file:
 *  - whole-console freezes (HOME dead, watchdog silent, music on) at the start
 *    of or a few minutes into the 2nd match;
 *  - 6-8 s at match start and 300-800 ms the first time each object shows up,
 *    with the SD idle: CPU work, most likely shader compilation.
 *
 * MEASUREMENT (always on, cheap):
 *  - GL entry points Unity resolves through our dlsym (nx_gpu_exports, first
 *    in the shim table): glCompileShader / glLinkProgram (mesa's GLSL front
 *    end, link and GLSL->TGSI), draws, texture and buffer uploads, map/sync.
 *  - mesa internals through `ld --wrap` (cross-object calls in libEGL.a):
 *    nvc0_program_translate (nouveau's TGSI -> GPU code, "codegen"),
 *    nvc0_program_upload, nvc0_screen_resize_text_area.
 *  - libnx nvidia calls made by libdrm_nouveau: nvmap objects, GPU address
 *    space mappings, GPU channels, kickoffs (and "busy" retries), fence waits.
 *  Nested time is attributed once: a codegen inside glLinkProgram counts as
 *  codegen, not link. A frame slower than [debug] hitch_ms gets a [hitch]
 *  line with the breakdown; every 2 s a [gpu] line with the counters.
 *
 * FENCE WAITS: libdrm_nouveau waits with an infinite timeout. Here an infinite
 * wait is done in 500 ms slices (same result), and every second still
 * waiting logs the fence and asks each GPU channel for its error notifier
 * (MMU fault, idle timeout, PBDMA error...), so a GPU hang that leaves the
 * system alive names itself in the log.
 *
 * KNOBS (mvlo.ini):
 *  [debug] nv_tmem_mb = 32   transfer memory handed to nvservices (libnx
 *        default 8 MB, raised from 3 MB by fincs in 2018 for mesa). It holds
 *        nvservices' own objects for this process (nvmap handles, address
 *        space and channel state); Unity creates thousands of buffers. Falls
 *        back to 8 MB if nvservices refuses the bigger size.
 *  [video] shader_code_mb = 16   size nouveau's shader code segment jumps to
 *        the first time it overflows. Stock mesa starts at 512 KB and doubles
 *        (1, 2, 4, 8 MB), evicting and re-uploading every shader each time,
 *        then evicts everything on each overflow at 8 MB -- the path behind
 *        "WARNING: out of code space, evicting all shaders" crashes reported
 *        against nouveau. One jump, early, instead of several mid-match.
 *        0 = stock behaviour.
 *  [video] compile_boost = 1   CPU at 1785 MHz (FastLoad) while shaders
 *        compile, back to normal 400 ms after the last compile.
 *  [video] nv_shader_opt = 3   nouveau optimisation level (NV50_PROG_OPTIMIZE,
 *        0..3): lower compiles faster, runs slower. Unset = mesa's 3.
 *
 * MIT -- part of mvlo_nx. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include "nx_bfmt.h"
#include "nx_gpu.h"
#include "nx_config.h"
#include "so_util.h"
#include "util.h"

extern void cpu_boost(int on);

static uint64_t now_ns(void) { return armTicksToNs(armGetSystemTick()); }

/* ------------------------------------------------------------- accounting */
enum { A_COMPILE, A_LINK, A_CODEGEN, A_UPLOAD, A_TEX, A_BUF, A_WAIT, A_DRAW,
       A_FOPEN, A_FSTAT, A_FFSTAT, A_FACCESS, A_N };
static const char *const k_acc_name[A_N] = {
  "GLSL compile", "link", "nouveau codegen", "code upload", "textures", "buffers", "GPU waits", "slow draws",
  "file open", "stat", "fstat", "access"
};
typedef struct { uint64_t ns; uint32_t n; uint64_t bytes; } Acc;
static Acc g_frame[A_N], g_period[A_N];   /* g_frame: any thread, atomics */

/* Nesting: time spent in instrumented callees is not charged to the caller
 * (per thread). Every thread's time counts towards the current frame: the
 * GL work runs on Unity's gfx thread, the frame is timed on ours. */
static __thread uint64_t t_nest_ns;
typedef struct { uint64_t t0, saved; } Span;

static inline Span span_begin(void) {
  Span s = { now_ns(), t_nest_ns };
  t_nest_ns = 0;
  return s;
}
static inline uint64_t span_end(Span s, int acc, uint64_t bytes) {
  uint64_t dt = now_ns() - s.t0;
  uint64_t self = dt > t_nest_ns ? dt - t_nest_ns : 0;
  t_nest_ns = s.saved + dt;
  __atomic_add_fetch(&g_frame[acc].ns, self, __ATOMIC_RELAXED);
  __atomic_add_fetch(&g_frame[acc].n, 1, __ATOMIC_RELAXED);
  if (bytes) __atomic_add_fetch(&g_frame[acc].bytes, bytes, __ATOMIC_RELAXED);
  return dt;
}

/* ------------------------------------------------------------- config */
static int cfg(const char *key, int def) { return nxcfg_int(key, def); }
static int g_boost_cfg = -1, g_hitch_ms = -1;
static volatile int g_boosted, g_base_boost = 1;
static volatile uint64_t g_last_compile_ns;

static void compile_activity(void) {
  g_last_compile_ns = now_ns();
  if (g_boost_cfg < 0) g_boost_cfg = cfg("video.compile_boost", 1);
  if (g_boost_cfg && !g_boosted && !g_base_boost) { g_boosted = 1; cpu_boost(1); }
}

/* cloverpit_boot.c owns the load-time boost; compile boosts happen only after
 * it is released, and never switch it off under its feet. */
void nx_gpu_base_boost(int on) { g_base_boost = on; if (on) g_boosted = 0; }

void nx_gpu_early_init(void) {
  int opt = cfg("video.nv_shader_opt", -1);
  if (opt >= 0 && opt <= 3) {
    char v[4]; snprintf(v, sizeof v, "%d", opt);
    setenv("NV50_PROG_OPTIMIZE", v, 1);
    debugPrintf("[gpu] nouveau shader optimisation level %d (NV50_PROG_OPTIMIZE)\n", opt);
  }
}

/* ------------------------------------------------------------- libnx nv */
/* Strong definition: replaces libnx's weak 8 MB default; set in nvInitialize. */
u32 __nx_nv_transfermem_size = 0x800000;

static volatile uint32_t g_maps_live, g_maps_peak, g_as_live, g_ch_live, g_busy, g_kicks, g_nv_errors;
static volatile uint64_t g_map_bytes, g_iova_max;
static volatile uint64_t g_wait_since;     /* a fence wait in progress since (ns), 0 = none */
static NvGpuChannel *g_ch[8];

Result __real_nvInitialize(void);
Result __wrap_nvInitialize(void) {
  static int first = 1;
  if (!first) return __real_nvInitialize();
  first = 0;
  int mb = cfg("debug.nv_tmem_mb", 32);
  if (mb < 3) mb = 8;
  if (mb > 128) mb = 128;
  __nx_nv_transfermem_size = (u32)mb << 20;
  Result rc = __real_nvInitialize();
  if (R_FAILED(rc) && __nx_nv_transfermem_size != 0x800000) {
    debugPrintf("[nv] nvInitialize with %d MB of transfer memory failed (0x%x) -- retrying with 8 MB\n", mb, rc);
    __nx_nv_transfermem_size = 0x800000;
    rc = __real_nvInitialize();
  }
  debugPrintf("[nv] nvInitialize: rc=0x%x, transfer memory %u MB\n", rc, __nx_nv_transfermem_size >> 20);
  return rc;
}

static void nv_error(const char *what, Result rc) {
  uint32_t n = __atomic_add_fetch(&g_nv_errors, 1, __ATOMIC_RELAXED);
  if (n <= 64)
    debugPrintf("[nv] %s FAILED rc=0x%x (maps live %u, %llu MB, AS maps %u, channels %u)\n", what, rc,
                g_maps_live, (unsigned long long)(g_map_bytes >> 20), g_as_live, g_ch_live);
}

Result __real_nvMapCreate(NvMap *m, void *cpu_addr, u32 size, u32 align, NvKind kind, bool cached);
Result __wrap_nvMapCreate(NvMap *m, void *cpu_addr, u32 size, u32 align, NvKind kind, bool cached) {
  Result rc = __real_nvMapCreate(m, cpu_addr, size, align, kind, cached);
  if (R_SUCCEEDED(rc)) {
    uint32_t n = __atomic_add_fetch(&g_maps_live, 1, __ATOMIC_RELAXED);
    if (n > g_maps_peak) g_maps_peak = n;
    __atomic_add_fetch(&g_map_bytes, size, __ATOMIC_RELAXED);
  } else nv_error("nvMapCreate", rc);
  return rc;
}
void __real_nvMapClose(NvMap *m);
void __wrap_nvMapClose(NvMap *m) {
  if (m && m->has_init) {
    __atomic_sub_fetch(&g_maps_live, 1, __ATOMIC_RELAXED);
    __atomic_sub_fetch(&g_map_bytes, m->size, __ATOMIC_RELAXED);
  }
  __real_nvMapClose(m);
}

Result __real_nvAddressSpaceMap(NvAddressSpace *a, u32 h, bool cacheable, NvKind kind, iova_t *out);
Result __wrap_nvAddressSpaceMap(NvAddressSpace *a, u32 h, bool cacheable, NvKind kind, iova_t *out) {
  Result rc = __real_nvAddressSpaceMap(a, h, cacheable, kind, out);
  if (R_SUCCEEDED(rc)) {
    __atomic_add_fetch(&g_as_live, 1, __ATOMIC_RELAXED);
    if (out && *out > g_iova_max) g_iova_max = *out;
  } else nv_error("nvAddressSpaceMap", rc);
  return rc;
}
Result __real_nvAddressSpaceUnmap(NvAddressSpace *a, iova_t iova);
Result __wrap_nvAddressSpaceUnmap(NvAddressSpace *a, iova_t iova) {
  Result rc = __real_nvAddressSpaceUnmap(a, iova);
  if (R_SUCCEEDED(rc)) __atomic_sub_fetch(&g_as_live, 1, __ATOMIC_RELAXED);
  else nv_error("nvAddressSpaceUnmap", rc);
  return rc;
}

Result __real_nvGpuChannelCreate(NvGpuChannel *c, struct NvAddressSpace *as, NvChannelPriority prio);
Result __wrap_nvGpuChannelCreate(NvGpuChannel *c, struct NvAddressSpace *as, NvChannelPriority prio) {
  Result rc = __real_nvGpuChannelCreate(c, as, prio);
  if (R_SUCCEEDED(rc)) {
    for (int i = 0; i < 8; i++) if (!g_ch[i]) { g_ch[i] = c; break; }
    uint32_t n = __atomic_add_fetch(&g_ch_live, 1, __ATOMIC_RELAXED);
    debugPrintf("[nv] GPU channel opened (%u live, priority %d)\n", n, (int)prio);
  } else nv_error("nvGpuChannelCreate", rc);
  return rc;
}
void __real_nvGpuChannelClose(NvGpuChannel *c);
void __wrap_nvGpuChannelClose(NvGpuChannel *c) {
  for (int i = 0; i < 8; i++) if (g_ch[i] == c) { g_ch[i] = NULL; __atomic_sub_fetch(&g_ch_live, 1, __ATOMIC_RELAXED); break; }
  debugPrintf("[nv] GPU channel closed (%u live)\n", g_ch_live);
  __real_nvGpuChannelClose(c);
}

Result __real_nvioctlChannel_KickoffPb(u32 fd, nvioctl_gpfifo_entry *e, u32 n, u32 flags, nvioctl_fence *f);
Result __wrap_nvioctlChannel_KickoffPb(u32 fd, nvioctl_gpfifo_entry *e, u32 n, u32 flags, nvioctl_fence *f) {
  Result rc = __real_nvioctlChannel_KickoffPb(fd, e, n, flags, f);
  __atomic_add_fetch(&g_kicks, 1, __ATOMIC_RELAXED);
  if (rc == MAKERESULT(Module_LibnxNvidia, LibnxNvidiaError_Busy)) {
    uint32_t b = __atomic_add_fetch(&g_busy, 1, __ATOMIC_RELAXED);
    if (b <= 16 || (b % 100) == 0)
      debugPrintf("[nv] GPU command queue busy (%u so far): kickoff retried in 100 ms\n", b);
  } else if (R_FAILED(rc)) nv_error("kickoff", rc);
  return rc;
}

static void channel_errors(void) {
  for (int i = 0; i < 8; i++) {
    NvGpuChannel *c = g_ch[i];
    if (!c) continue;
    NvNotification nt;
    if (R_SUCCEEDED(nvGpuChannelGetErrorNotification(c, &nt)))
      debugPrintf("[nv] channel %d ERROR NOTIFICATION: type %u (8 idle timeout, 13 sw notify, 24 semaphore "
                  "timeout, 25 illegal, 31 MMU fault, 32 PBDMA) info16 0x%x\n", i, nt.info32, nt.info16);
    NvError er;
    if (R_SUCCEEDED(nvGpuChannelGetErrorInfo(c, &er)) && er.type)
      debugPrintf("[nv] channel %d error info: type %u info %08x %08x %08x %08x\n", i, er.type,
                  er.info[0], er.info[1], er.info[2], er.info[3]);
  }
}

Result __real_nvFenceWait(NvFence *f, s32 timeout_us);
Result __wrap_nvFenceWait(NvFence *f, s32 timeout_us) {
  const s32 SLICE = 500000;
  if (timeout_us >= 0 && timeout_us <= SLICE) {
    if (timeout_us == 0) return __real_nvFenceWait(f, 0);     /* poll: no accounting */
    Span s = span_begin();
    Result rc = __real_nvFenceWait(f, timeout_us);
    span_end(s, A_WAIT, 0);
    return rc;
  }
  Span s = span_begin();
  uint64_t start = now_ns(), next_log = start + 1000000000ull;
  g_wait_since = start;
  Result rc;
  for (;;) {
    rc = __real_nvFenceWait(f, SLICE);
    if (rc != MAKERESULT(Module_LibnxNvidia, LibnxNvidiaError_Timeout)) break;
    uint64_t t = now_ns();
    if (timeout_us >= 0 && t - start >= (uint64_t)timeout_us * 1000ull) break;
    if (t >= next_log) {
      next_log = t + 1000000000ull;
      debugPrintf("[nv] GPU has not reached fence {%u, %u} after %llu ms -- channel state follows\n",
                  f->id, f->value, (unsigned long long)((t - start) / 1000000ull));
      channel_errors();
    }
  }
  g_wait_since = 0;
  uint64_t dt = span_end(s, A_WAIT, 0);
  if (dt > 1000000000ull)
    debugPrintf("[nv] fence {%u, %u} reached after %llu ms\n", f->id, f->value, (unsigned long long)(dt / 1000000ull));
  if (R_FAILED(rc)) nv_error("nvFenceWait", rc);
  return rc;
}

/* ------------------------------------------------------------- mesa (nouveau) */
static uint32_t g_code_resizes;
static uint64_t g_code_bytes;

int __real_nvc0_screen_resize_text_area(void *screen, uint64_t size);
int __wrap_nvc0_screen_resize_text_area(void *screen, uint64_t size) {
  uint64_t want = (uint64_t)cfg("video.shader_code_mb", 16) << 20;
  uint64_t use = want > size ? want : size;
  int rc = __real_nvc0_screen_resize_text_area(screen, use);
  g_code_resizes++;
  g_code_bytes = use;
  debugPrintf("[gpu] shader code segment full: all shaders evicted, segment now %llu KB (mesa asked %llu KB) rc=%d\n",
              (unsigned long long)(use >> 10), (unsigned long long)(size >> 10), rc);
  return rc;
}

int __real_nvc0_program_translate(void *prog, uint16_t chipset, void *debug);
int __wrap_nvc0_program_translate(void *prog, uint16_t chipset, void *debug) {
  compile_activity();
  Span s = span_begin();
  int r = __real_nvc0_program_translate(prog, chipset, debug);
  span_end(s, A_CODEGEN, 0);
  return r;
}
int __real_nvc0_program_upload(void *nvc0, void *prog);
int __wrap_nvc0_program_upload(void *nvc0, void *prog) {
  Span s = span_begin();
  int r = __real_nvc0_program_upload(nvc0, prog);
  span_end(s, A_UPLOAD, 0);
  return r;
}

/* ------------------------------------------------------------- GL */
static uint32_t g_links, g_relinks, g_compiles;

/* shader id -> source hash, program id -> combined hash (open addressing). */
#define HT 8192
static struct { uint32_t id; uint64_t h; } g_sh[HT], g_seen[HT];
static uint64_t fnv(const char *s) {
  uint64_t h = 1469598103934665603ull;
  for (; *s; s++) h = (h ^ (uint8_t)*s) * 1099511628211ull;
  return h;
}
static void sh_put(uint32_t id, uint64_t h) {
  for (uint32_t i = id * 2654435761u % HT, k = 0; k < HT; k++, i = (i + 1) % HT)
    if (!g_sh[i].id || g_sh[i].id == id) { g_sh[i].id = id; g_sh[i].h = h; return; }
}
static uint64_t sh_get(uint32_t id) {
  for (uint32_t i = id * 2654435761u % HT, k = 0; k < HT && g_sh[i].id; k++, i = (i + 1) % HT)
    if (g_sh[i].id == id) return g_sh[i].h;
  return 0;
}
/* 1 if the program key was linked before in this session. */
static int seen_before(uint64_t key) {
  if (!key) return 0;
  for (uint32_t i = (uint32_t)(key % HT), k = 0; k < HT; k++, i = (i + 1) % HT) {
    if (g_seen[i].h == key) return 1;
    if (!g_seen[i].h) { g_seen[i].h = key; return 0; }
  }
  return 0;
}

/* Called by the glShaderSource shim (imports.c) with the final source. */
void nx_gpu_note_shader_source(GLuint sh, const char *src) { sh_put(sh, fnv(src)); }

static void w_glCompileShader(GLuint sh) {
  compile_activity();
  Span s = span_begin();
  glCompileShader(sh);
  span_end(s, A_COMPILE, 0);
  g_compiles++;
}

static void w_glLinkProgram(GLuint prog) {
  compile_activity();
  GLuint shs[4]; GLsizei n = 0;
  glGetAttachedShaders(prog, 4, &n, shs);
  uint64_t key = 0;
  for (GLsizei i = 0; i < n; i++) key = key * 31 + sh_get(shs[i]);
  Span s = span_begin();
  glLinkProgram(prog);
  uint64_t dt = span_end(s, A_LINK, 0);
  g_links++;
  if (seen_before(key)) {
    g_relinks++;
    if (g_relinks <= 16)
      debugPrintf("[gpu] program %u re-links sources already linked this session (%llu ms)\n",
                  prog, (unsigned long long)(dt / 1000000ull));
  }
}

#define DRAW_SLOW_NS 3000000ull
#define TIMED_DRAW(call) do { \
    Span s_ = span_begin(); call; \
    uint64_t dt_ = now_ns() - s_.t0; \
    if (dt_ > DRAW_SLOW_NS) span_end(s_, A_DRAW, 0); \
    else t_nest_ns = s_.saved + dt_; \
  } while (0)

static void w_glDrawArrays(GLenum m, GLint f, GLsizei c) { TIMED_DRAW(glDrawArrays(m, f, c)); }
static void w_glDrawElements(GLenum m, GLsizei c, GLenum t, const void *i) { TIMED_DRAW(glDrawElements(m, c, t, i)); }

typedef void (*PFN_DAI)(GLenum, GLint, GLsizei, GLsizei);
typedef void (*PFN_DEI)(GLenum, GLsizei, GLenum, const void *, GLsizei);
typedef void (*PFN_DRE)(GLenum, GLuint, GLuint, GLsizei, GLenum, const void *);
typedef void (*PFN_TS2)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
typedef void (*PFN_CTI)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *);
typedef void (*PFN_CTS)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLsizei, const void *);
typedef void *(*PFN_MBR)(GLenum, GLintptr, GLsizeiptr, GLbitfield);
typedef GLenum (*PFN_CWS)(GLsync, GLbitfield, GLuint64);
#define REAL(type, name) static type r_; if (!r_) r_ = (type)eglGetProcAddress(name)

static void w_glDrawArraysInstanced(GLenum m, GLint f, GLsizei c, GLsizei n) {
  REAL(PFN_DAI, "glDrawArraysInstanced"); TIMED_DRAW(r_(m, f, c, n));
}
static void w_glDrawElementsInstanced(GLenum m, GLsizei c, GLenum t, const void *i, GLsizei n) {
  REAL(PFN_DEI, "glDrawElementsInstanced"); TIMED_DRAW(r_(m, c, t, i, n));
}
static void w_glDrawRangeElements(GLenum m, GLuint a, GLuint b, GLsizei c, GLenum t, const void *i) {
  REAL(PFN_DRE, "glDrawRangeElements"); TIMED_DRAW(r_(m, a, b, c, t, i));
}

static uint64_t px_bytes(GLsizei w, GLsizei h, GLenum type, GLenum fmt) {
  int c = (fmt == GL_RGBA || fmt == GL_RGBA_INTEGER) ? 4 : (fmt == GL_RGB) ? 3 :
          (fmt == GL_RG) ? 2 : 1;
  int b = (type == GL_FLOAT) ? 4 : (type == GL_HALF_FLOAT || type == GL_UNSIGNED_SHORT_5_6_5 ||
          type == GL_UNSIGNED_SHORT_4_4_4_4 || type == GL_UNSIGNED_SHORT_5_5_5_1) ? 2 : 1;
  if (type == GL_UNSIGNED_SHORT_5_6_5 || type == GL_UNSIGNED_SHORT_4_4_4_4 || type == GL_UNSIGNED_SHORT_5_5_5_1) c = 1;
  return (uint64_t)w * h * c * b;
}
static void w_glTexImage2D(GLenum t, GLint l, GLint ifmt, GLsizei w, GLsizei h, GLint b, GLenum f, GLenum ty, const void *p) {
  Span s = span_begin(); glTexImage2D(t, l, ifmt, w, h, b, f, ty, p); span_end(s, A_TEX, p ? px_bytes(w, h, ty, f) : 0);
}
static void w_glTexSubImage2D(GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum ty, const void *p) {
  Span s = span_begin(); glTexSubImage2D(t, l, x, y, w, h, f, ty, p); span_end(s, A_TEX, px_bytes(w, h, ty, f));
}
static void w_glCompressedTexImage2D(GLenum t, GLint l, GLenum f, GLsizei w, GLsizei h, GLint b, GLsizei n, const void *d) {
  Span s = span_begin(); glCompressedTexImage2D(t, l, f, w, h, b, n, d); span_end(s, A_TEX, (uint64_t)n);
}
static void w_glCompressedTexSubImage2D(GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLsizei n, const void *d) {
  REAL(PFN_CTS, "glCompressedTexSubImage2D");
  Span s = span_begin(); r_(t, l, x, y, w, h, f, n, d); span_end(s, A_TEX, (uint64_t)n);
}
static void w_glTexStorage2D(GLenum t, GLsizei lv, GLenum f, GLsizei w, GLsizei h) {
  REAL(PFN_TS2, "glTexStorage2D");
  Span s = span_begin(); r_(t, lv, f, w, h); span_end(s, A_TEX, 0);
}
static void w_glBufferData(GLenum t, GLsizeiptr n, const void *d, GLenum u) {
  Span s = span_begin(); glBufferData(t, n, d, u); span_end(s, A_BUF, (uint64_t)n);
}
static void w_glBufferSubData(GLenum t, GLintptr o, GLsizeiptr n, const void *d) {
  Span s = span_begin(); glBufferSubData(t, o, n, d); span_end(s, A_BUF, (uint64_t)n);
}
static void *w_glMapBufferRange(GLenum t, GLintptr o, GLsizeiptr n, GLbitfield a) {
  REAL(PFN_MBR, "glMapBufferRange");
  Span s = span_begin(); void *p = r_(t, o, n, a); span_end(s, A_BUF, 0);
  return p;
}
static GLenum w_glClientWaitSync(GLsync y, GLbitfield fl, GLuint64 to) {
  REAL(PFN_CWS, "glClientWaitSync");
  Span s = span_begin(); GLenum r = r_(y, fl, to); span_end(s, A_WAIT, 0);
  return r;
}

DynLibFunction nx_gpu_exports[] = {
  { "glCompileShader",           (uintptr_t)&w_glCompileShader },
  { "glLinkProgram",             (uintptr_t)&w_glLinkProgram },
  { "glDrawArrays",              (uintptr_t)&w_glDrawArrays },
  { "glDrawElements",            (uintptr_t)&w_glDrawElements },
  { "glDrawArraysInstanced",     (uintptr_t)&w_glDrawArraysInstanced },
  { "glDrawElementsInstanced",   (uintptr_t)&w_glDrawElementsInstanced },
  { "glDrawRangeElements",       (uintptr_t)&w_glDrawRangeElements },
  { "glTexImage2D",              (uintptr_t)&w_glTexImage2D },
  { "glTexSubImage2D",           (uintptr_t)&w_glTexSubImage2D },
  { "glCompressedTexImage2D",    (uintptr_t)&w_glCompressedTexImage2D },
  { "glCompressedTexSubImage2D", (uintptr_t)&w_glCompressedTexSubImage2D },
  { "glTexStorage2D",            (uintptr_t)&w_glTexStorage2D },
  { "glBufferData",              (uintptr_t)&w_glBufferData },
  { "glBufferSubData",           (uintptr_t)&w_glBufferSubData },
  { "glMapBufferRange",          (uintptr_t)&w_glMapBufferRange },
  { "glClientWaitSync",          (uintptr_t)&w_glClientWaitSync },
};
int nx_gpu_exports_num = sizeof(nx_gpu_exports) / sizeof(nx_gpu_exports[0]);

/* ------------------------------------------------------------- reports */
static void acc_fmt(char *out, size_t len, const Acc *a, uint64_t *sum) {
  size_t o = 0;
  out[0] = 0;
  for (int i = 0; i < A_N && o < len; i++) {
    if (!a[i].n) continue;
    *sum += a[i].ns;
    if (a[i].bytes >= (1u << 20))
      o += snprintf(out + o, len - o, " | %s %llu ms (%u, %.1f MB)", k_acc_name[i],
                    (unsigned long long)(a[i].ns / 1000000ull), a[i].n, a[i].bytes / 1048576.0);
    else
      o += snprintf(out + o, len - o, " | %s %llu ms (%u)", k_acc_name[i],
                    (unsigned long long)(a[i].ns / 1000000ull), a[i].n);
  }
}

void nx_gpu_frame_end(uint64_t frame_ns, uint64_t frame_no) {
  if (g_hitch_ms < 0) g_hitch_ms = cfg("debug.hitch_ms", 100);
  if (g_hitch_ms > 0 && frame_ns >= (uint64_t)g_hitch_ms * 1000000ull) {
    char buf[600]; uint64_t sum = 0;
    Acc snap[A_N];
    memcpy(snap, (const void *)g_frame, sizeof snap);
    acc_fmt(buf, sizeof buf, snap, &sum);
    debugPrintf("[hitch] frame %llu: %llu ms%s | other %llu ms\n", (unsigned long long)frame_no,
                (unsigned long long)(frame_ns / 1000000ull), buf,
                (unsigned long long)(frame_ns > sum ? (frame_ns - sum) / 1000000ull : 0));
  }
  for (int i = 0; i < A_N; i++) {
    uint64_t ns = __atomic_exchange_n(&g_frame[i].ns, 0, __ATOMIC_RELAXED);
    uint32_t n = __atomic_exchange_n(&g_frame[i].n, 0, __ATOMIC_RELAXED);
    uint64_t by = __atomic_exchange_n(&g_frame[i].bytes, 0, __ATOMIC_RELAXED);
    g_period[i].ns += ns; g_period[i].n += n; g_period[i].bytes += by;
  }
  if (g_boosted && now_ns() - g_last_compile_ns > 400000000ull) { g_boosted = 0; cpu_boost(0); }
}

void nx_gpu_report(void) {
  char buf[600]; uint64_t sum = 0;
  acc_fmt(buf, sizeof buf, g_period, &sum);
  debugPrintf("[gpu] nv: %u maps live (%llu MB, peak %u), %u GPU mappings (top 0x%llx), %u channel(s), %u kickoffs, "
              "%u busy, %u failed | shaders: %u compiled, %u linked (%u identical re-links), code segment %llu KB%s\n",
              g_maps_live, (unsigned long long)(g_map_bytes >> 20), g_maps_peak, g_as_live,
              (unsigned long long)g_iova_max, g_ch_live, g_kicks, g_busy, g_nv_errors,
              g_compiles, g_links, g_relinks, (unsigned long long)((g_code_bytes ? g_code_bytes : 512 * 1024) >> 10),
              g_boosted ? " | compile boost ON" : "");
  if (sum) debugPrintf("[gpu] last period:%s\n", buf);
  memset(g_period, 0, sizeof g_period);
  g_kicks = 0;
}

/* File-system time (libc_shim.c) in the same [hitch] breakdown: on hardware a
 * 24 s frame was 99.6 % "other" while the watchdog caught UnityMain inside an
 * FS IPC under File::Open. kind: 0 = open, 1 = stat, 2 = fstat, 3 = access. */
void nx_gpu_account_fs(int kind, uint64_t ns) {
  int a = kind == 1 ? A_FSTAT : kind == 2 ? A_FFSTAT : kind == 3 ? A_FACCESS : A_FOPEN;
  __atomic_add_fetch(&g_frame[a].ns, ns, __ATOMIC_RELAXED);
  __atomic_add_fetch(&g_frame[a].n, 1, __ATOMIC_RELAXED);
}

void nx_gpu_beacon(char *buf, unsigned len) {
  uint64_t w = g_wait_since;
  int o = nx_bfmt(buf, len, "gpu: maps=%u (%lluMB) ch=%u busy=%u failed=%u", g_maps_live,
                   (unsigned long long)(g_map_bytes >> 20), g_ch_live, g_busy, g_nv_errors);
  if (w && o > 0 && (unsigned)o < len)
    nx_bfmt(buf + o, len - o, " fence-wait=%llums", (unsigned long long)((now_ns() - w) / 1000000ull));
}
