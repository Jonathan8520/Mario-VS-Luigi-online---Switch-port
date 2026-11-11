/* nx_prof.c -- where UnityMain spends a slow frame ([debug] prof_ms).
 *
 * WHY
 * The first Room Settings, the first match start and the first power-up
 * sprites each freeze the game for seconds. The [hitch] breakdown (nx_gpu.c)
 * times GL calls, file opens and stats, but file reads, audio decoding and
 * the engine's own work all land in "other" -- which is most of it. A
 * sampling profiler says where that time goes.
 *
 * HOW
 * A thread checks every 4 ms how long the current frame has lasted. Past
 * prof_ms it samples the engine main thread and Unity's two loader threads
 * (UnityMain mostly waits on them at a match start): pause, read PC/LR and
 * the frame-pointer chain (up to PROF_DEPTH addresses), resume. The thread's
 * signal record is held meanwhile (nx_sig_sample_lock), so the GC's forced
 * delivery never touches it at the same time. When the frame ends, its
 * samples are appended to sdmc:/switch/mvlo/prof.log as module+offset
 * addresses, one sample per line tagged T0 (UnityMain), T1
 * (Loading.AsyncRead) or T2 (Loading.PreloadManager); tools/switch/prof_report.py names them
 * with the build's .elf and the CI symbols and ranks functions.
 *
 * [debug] prof_ms = 0 (off) | N ms; prof_from_frame = 300 (skip the boot).
 *
 * MIT -- part of mvlo_nx. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "cloverpit_root.h"
#include "nx_autopsy.h"
#include "nx_config.h"
#include "nx_signals.h"
#include "util.h"

extern int diag_frame_no(void);
extern uint64_t diag_idle_ns(void);
extern Handle diag_engine_main_handle(void);
extern Handle diag_handle_by_name(const char *name);
extern int diag_bfmt_addr(char *buf, unsigned n, uint64_t addr);
extern int diag_is_code_addr(uint64_t a);

#define PROF_DEPTH 12        /* PC, LR, then the frame-pointer chain */
#define PROF_MAX   8192      /* ~11 s of samples at 4 ms, three threads */
#define PROF_NTH   3
#define PROF_TICK_NS 4000000ull
#define PROF_STACK_WINDOW (512u * 1024u)   /* how far above SP frame records may be */

typedef struct {
  uint32_t t_ms;             /* how long the frame had lasted at this sample */
  uint16_t n;
  uint16_t th;               /* 0 UnityMain, 1 Loading.AsyncRead, 2 Loading.PreloadManager */
  uint64_t a[PROF_DEPTH];
} Sample;

static Sample g_s[PROF_MAX];
static Thread g_thr;
static int g_prof_ms, g_from_frame;
static unsigned g_frames_logged;

static int take(Handle h, Sample *s, uint32_t t_ms) {
  void *ck;
  if (!nx_sig_sample_lock(h, &ck)) return 0;
  if (R_FAILED(svcSetThreadActivity(h, ThreadActivity_Paused))) { nx_sig_sample_unlock(ck); return 0; }
  ThreadContext ctx;
  Result gr = svcGetThreadContext3(&ctx, h);
  uint32_t n = 0;
  if (R_SUCCEEDED(gr)) {
    s->a[n++] = ctx.pc.x;
    s->a[n++] = ctx.lr;
    /* Frame records live just above SP. libunity/libil2cpp are built without
     * frame pointers, so FP is often an arbitrary value: only follow it inside
     * a window above SP, within SP's memory block, and stop at the first
     * return address that is not in loaded code. */
    MemoryInfo mi; u32 pi;
    uint64_t lo = 0, hi = 0;
    if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, ctx.sp)) && mi.size && (mi.perm & Perm_R)) {
      lo = ctx.sp;
      hi = mi.addr + mi.size;
      if (hi > ctx.sp + PROF_STACK_WINDOW) hi = ctx.sp + PROF_STACK_WINDOW;
    }
    uint64_t fp = ctx.fp;
    /* fp <= hi - 16, not fp + 16 <= hi: FP = -16 wrapped around and passed
     * (the actual cause of both v0.1.31-32 crashes: a load from 0xff..f8). */
    while (n < PROF_DEPTH && lo && hi - lo >= 16 && (fp & 7) == 0 && fp >= lo && fp <= hi - 16) {
      uint64_t nfp = ((const uint64_t *)(uintptr_t)fp)[0];
      uint64_t lr  = ((const uint64_t *)(uintptr_t)fp)[1];
      if (!lr || !diag_is_code_addr(lr)) break;
      s->a[n++] = lr;
      if (nfp <= fp) break;
      fp = nfp;
    }
  }
  svcSetThreadActivity(h, ThreadActivity_Runnable);
  nx_sig_sample_unlock(ck);
  s->n = (uint16_t)n;
  s->t_ms = t_ms;
  return n > 0;
}

/* Append one slow frame's samples to prof.log (this thread, after the frame). */
static void dump(int frame, uint64_t frame_ms, int ns) {
  char path[256];
  snprintf(path, sizeof path, "%s/prof.log", cp_game_root());
  FILE *f = fopen(path, "a");
  if (!f) { debugPrintf("[prof] cannot open %s\n", path); return; }
  fprintf(f, "=== frame %d: %llu ms, %d samples every 4 ms past %d ms ===\n", frame,
          (unsigned long long)frame_ms, ns, g_prof_ms);
  char line[PROF_DEPTH * 40 + 16], a[40];
  for (int i = 0; i < ns; i++) {
    int o = snprintf(line, sizeof line, "%u T%u", g_s[i].t_ms, g_s[i].th);
    for (uint32_t k = 0; k < g_s[i].n && o < (int)sizeof line; k++) {
      diag_bfmt_addr(a, sizeof a, g_s[i].a[k]);
      o += snprintf(line + o, sizeof line - o, " %s", a);
    }
    fprintf(f, "%s\n", line);
  }
  fclose(f);
  g_frames_logged++;
  debugPrintf("[prof] frame %d: %llu ms, %d samples -> prof.log\n", frame, (unsigned long long)frame_ms, ns);
}

static void prof_main(void *arg) {
  (void)arg;
  static uint8_t tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  install_bionic_tls(tls);
  nx_autopsy_name_self("profiler");
  static const char *const k_names[PROF_NTH] = { NULL, "Loading.AsyncRead", "Loading.PreloadManager" };
  Handle h[PROF_NTH] = { 0 };
  int cur = -1, ns = 0;
  uint64_t longest_ns = 0;
  for (unsigned tick = 0;; tick++) {
    svcSleepThread(PROF_TICK_NS);
    if ((tick & 255) == 0)                /* (re)learn the handles once a second */
      for (int k = 0; k < PROF_NTH; k++) h[k] = k ? diag_handle_by_name(k_names[k]) : diag_engine_main_handle();
    if (!h[0]) continue;
    int f = diag_frame_no();
    uint64_t idle = diag_idle_ns();
    if (f != cur) {                       /* the frame we sampled has ended */
      if (ns > 0) dump(cur + 1, longest_ns / 1000000ull, ns);
      cur = f; ns = 0; longest_ns = 0;
      continue;
    }
    if (f < g_from_frame || idle < (uint64_t)g_prof_ms * 1000000ull) continue;
    if (idle > 60000000000ull) continue;  /* suspended (HOME menu), not a slow frame */
    if (idle > longest_ns) longest_ns = idle;
    for (int k = 0; k < PROF_NTH && ns < PROF_MAX; k++)
      if (h[k] && take(h[k], &g_s[ns], (uint32_t)(idle / 1000000ull))) { g_s[ns].th = (uint16_t)k; ns++; }
  }
}

void nx_prof_start(void) {
  g_prof_ms = nxcfg_int("debug.prof_ms", 0);
  g_from_frame = nxcfg_int("debug.prof_from_frame", 300);
  if (g_prof_ms <= 0) return;
  /* Like the watchdog: above the engine if allowed, on core 2 when possible. */
  static const int PRIOS[] = { 0x28, 0x2A, 0x2B, 0x2C };
  Result rc = 0xe001;
  for (unsigned i = 0; i < sizeof PRIOS / sizeof PRIOS[0] && R_FAILED(rc); i++) {
    rc = threadCreate(&g_thr, prof_main, NULL, NULL, 0x8000, PRIOS[i], 2);
    if (R_FAILED(rc)) rc = threadCreate(&g_thr, prof_main, NULL, NULL, 0x8000, PRIOS[i], -2);
  }
  if (R_FAILED(rc) || R_FAILED(threadStart(&g_thr))) {
    debugPrintf("[prof] thread failed rc=0x%x\n", rc);
    return;
  }
  debugPrintf("[prof] sampling UnityMain every 4 ms in frames longer than %d ms (from frame %d) -> prof.log\n",
              g_prof_ms, g_from_frame);
}
