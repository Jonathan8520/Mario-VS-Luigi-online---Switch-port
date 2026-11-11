/* diag.c -- see diag.h. Frame-1 black-hang instrumentation.
 *
 * This software may be modified and distributed under the terms of the MIT
 * license. See the LICENSE file for details.
 */
#define _GNU_SOURCE
#include <switch.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#include "nx_bfmt.h"
#include "nx_gpu.h"
#include "diag.h"
#include "util.h"   /* stallPrintf -- see below, NOT debugPrintf */

/* EVERY line in this file must use stallPrintf, never debugPrintf.
 * debugPrintf takes a global mutex; the thread this file exists to report
 * on is typically blocked inside the filesystem while holding it, so a
 * single debugPrintf here deadlocks the watchdog against its own subject.
 * That is exactly what happened: an earlier pass converted the lines
 * starting "[wd]" but missed two that start "\n[wd]", and the watchdog went
 * silent for four runs -- stall.log contained only the arm line. */
#include "so_util.h" /* so_find_module_by_addr for backtrace symbolication */
#include "nx_signals.h"
#include "nx_bfmt.h"
#include "nx_autopsy.h"

/* ------------------------------------------------------------------ tunables */
#define DIAG_MAX_THREADS   96
#define DIAG_POLL_NS       (1000ull * 1000ull * 1000ull)   /* watchdog tick: 1s */
/* 3s, not 6s. The engine's slowest legitimate frame measured 3.9s during the
 * initial scene load, so 6s was chosen to avoid false positives -- but the load
 * frames are now identifiable by their own [loop] SLOW markers, and waiting 6s
 * into a 100% CPU spin risks the console being power-cycled before the dump
 * lands. False positives during load are cheap; a missed dump is not. */
#define DIAG_STALL_NS      (3000ull * 1000ull * 1000ull)   /* declare stall: 3s  */
#define DIAG_REDUMP_NS     (6000ull * 1000ull * 1000ull)   /* re-dump cadence    */
/* Frames 0 and 1 load the first scenes and last 2-4.2 s on every boot: at 3 s
 * the watchdog dumped every thread on 82 boots out of 84, ~270 lines written to
 * the card one by one during the load, and on 08/11 (0.1.42) that dump read the
 * stack of a short-lived thread which had just ended and crashed the game.
 * A real boot hang still gets its dump, at 20 s (the stall exit is at 30 s). */
#define DIAG_BOOT_STALL_NS (20000ull * 1000ull * 1000ull)

typedef struct {
  volatile int          in_use;
  uint64_t              tid;            /* svcGetThreadId — matches crash reports */
  Handle                handle;         /* real thread handle for svcGetThreadContext3 */
  pthread_t             pth;            /* host handle, for setname target match  */
  char                  name[32];
  const void           *entry;
  int                   is_main_engine;
  /* live wait beacon */
  volatile int          wait_kind;
  volatile const void  *wait_obj;
  volatile uint64_t     wait_since;     /* tick (raw) when current wait began     */
  /* liveness counters */
  volatile uint64_t     waits_total;
  volatile uint64_t     wakes_total;
  volatile uint64_t     futex_spins;
  volatile uint64_t     last_active;    /* tick of last beacon activity           */
} DiagThread;

static DiagThread       g_threads[DIAG_MAX_THREADS];
static Mutex            g_reg_lock;     /* zero-init libnx Mutex == unlocked       */
static __thread DiagThread *self;       /* this thread's slot (host TLS)           */

static volatile int      g_frame = -1;
static volatile uint64_t g_last_progress;   /* tick of last diag_frame()           */
static volatile int      g_wd_started;
static Thread            g_wd_thread;

/* ------------------------------------------------------------------ helpers */
static inline uint64_t now_tick(void) { return armGetSystemTick(); }
static inline uint64_t tick_to_ns(uint64_t t) { return armTicksToNs(t); }

static const char *wait_kind_name(int k) {
  switch (k) {
    case DIAG_W_COND:   return "cond_wait";
    case DIAG_W_JOIN:   return "join";
    case DIAG_W_SEM:    return "sem_wait";
    case DIAG_W_MUTEX:  return "mutex_lock";
    case DIAG_W_RWLOCK: return "rwlock";
    case DIAG_W_FUTEX:  return "futex_spin";
    default:            return "running";
  }
}

static DiagThread *slot_alloc(void) {
  mutexLock(&g_reg_lock);
  DiagThread *t = NULL;
  for (int i = 0; i < DIAG_MAX_THREADS; i++) {
    if (!g_threads[i].in_use) { t = &g_threads[i]; break; }
  }
  if (t) {
    memset(t, 0, sizeof(*t));
    t->in_use = 1;
    t->pth = pthread_self();
    t->handle = threadGetCurHandle();   /* real handle, usable from the watchdog */
    uint64_t tid = 0;
    if (R_SUCCEEDED(svcGetThreadId(&tid, CUR_THREAD_HANDLE))) t->tid = tid;
    t->last_active = now_tick();
  }
  mutexUnlock(&g_reg_lock);
  return t;
}

/* Return this thread's slot, lazily allocating one if it ran code we didn't
 * trampoline (e.g. the process main thread). Never returns NULL unless the
 * registry is full (then beacons silently no-op). */
static DiagThread *diag_self(void) {
  if (self) return self;
  DiagThread *t = slot_alloc();
  if (t && t->name[0] == 0) {
    /* default label until a real name arrives */
    snprintf(t->name, sizeof(t->name), "T%llu", (unsigned long long)t->tid);
  }
  self = t;
  return t;
}

/* ------------------------------------------------------------------ public */
void diag_thread_register(const void *entry, int is_main_engine) {
  DiagThread *t = diag_self();
  if (!t) return;
  t->entry = entry;
  t->is_main_engine = is_main_engine;
  if (is_main_engine && t->name[0] == 'T')   /* keep until Unity renames it */
    snprintf(t->name, sizeof(t->name), "engine_main");
}

void diag_thread_unregister(void) {
  if (!self) return;
  mutexLock(&g_reg_lock);
  self->in_use = 0;
  mutexUnlock(&g_reg_lock);
  self = NULL;
}

void diag_set_name(void *target_pthread, const char *name) {
  if (!name) return;
  DiagThread *t = NULL;
  if (target_pthread) {
    pthread_t want = (pthread_t)target_pthread;
    mutexLock(&g_reg_lock);
    for (int i = 0; i < DIAG_MAX_THREADS; i++) {
      if (g_threads[i].in_use && pthread_equal(g_threads[i].pth, want)) { t = &g_threads[i]; break; }
    }
    mutexUnlock(&g_reg_lock);
  }
  if (!t) t = diag_self();   /* PR_SET_NAME / self-naming case */
  if (!t) return;
  strncpy(t->name, name, sizeof(t->name) - 1);
  t->name[sizeof(t->name) - 1] = 0;
}

void diag_wait_enter(int kind, const void *obj) {
  DiagThread *t = diag_self();
  if (!t) return;
  t->wait_kind  = kind;
  t->wait_obj   = obj;
  t->wait_since = now_tick();
  t->waits_total++;
  t->last_active = t->wait_since;
}

void diag_wait_exit(void) {
  DiagThread *t = self;          /* exit without a prior enter is harmless */
  if (!t) return;
  t->wait_kind = DIAG_W_NONE;
  t->wait_obj  = NULL;
  t->wakes_total++;
  t->last_active = now_tick();
}

void diag_futex_spin(const void *obj) {
  DiagThread *t = diag_self();
  if (!t) return;
  /* publish as a futex wait but keep counting spins so the watchdog can tell
   * "alive but never satisfied" from "hard-parked". Reset wait_since only on
   * the transition *into* a futex wait (or onto a different uaddr), so the
   * dumped "parked secs" measures the current spin episode. */
  int was_futex = (t->wait_kind == DIAG_W_FUTEX && t->wait_obj == obj);
  t->wait_kind  = DIAG_W_FUTEX;
  t->wait_obj   = obj;
  uint64_t now = now_tick();
  if (!was_futex) t->wait_since = now;
  t->futex_spins++;
  t->last_active = now;
}

void diag_frame(int frame) {
  g_frame = frame;
  g_last_progress = now_tick();
}
/* nx_alive.c: lock-free reads of the render loop's progress. */
int diag_frame_no(void) { return g_frame; }
uint64_t diag_idle_ns(void) {
  uint64_t lp = g_last_progress;
  return lp ? tick_to_ns(now_tick() - lp) : 0;
}

/* ------------------------------------------------------------------ watchdog */
static uint64_t prev_waits[DIAG_MAX_THREADS];
static uint64_t prev_wakes[DIAG_MAX_THREADS];
static uint64_t prev_spins[DIAG_MAX_THREADS];

/* ---- CPU-context snapshot: see *where in libunity/il2cpp* a thread is wedged.
 * The shim beacons only show which sync primitive a thread sits in; when the
 * hang is inside native engine code (our case: main thread parked inside
 * Unity_nativeRender), this backtrace is what actually pinpoints it. */

/* Our own NRO code region, resolved once via svcQueryMemory on a local fn. */
static uint64_t g_nro_base, g_nro_size;
static void nro_range_init(void) {
  MemoryInfo mi; u32 pi;
  if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, (u64)(uintptr_t)&nro_range_init)) && mi.size) {
    g_nro_base = mi.addr; g_nro_size = mi.size;
  }
}
/* Write a symbolicated label for `addr` into buf: "libX+0xoff" / "NRO+0xoff"
 * / raw absolute. */
static void resolve_addr(char *buf, size_t n, uint64_t addr) {
  so_module *m = so_find_module_by_addr((const void *)(uintptr_t)addr);
  if (m) {
    /* m->name is the full sdmc path; the leading dirs are identical for every
     * loaded .so, so a fixed-width truncation makes libunity and libil2cpp
     * indistinguishable. Print the basename instead. */
    const char *base = m->name, *p;
    for (p = m->name; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;
    snprintf(buf, n, "%s+0x%llx", base,
             (unsigned long long)(addr - (uint64_t)(uintptr_t)m->load_virtbase));
  } else if (g_nro_size && addr >= g_nro_base && addr < g_nro_base + g_nro_size) {
    snprintf(buf, n, "NRO+0x%llx", (unsigned long long)(addr - g_nro_base));
  } else {
    snprintf(buf, n, "0x%llx", (unsigned long long)addr);
  }
}

/* [lo, hi) of the readable memory block holding `sp`; 0/0 if there is none.
 * svcQueryMemory succeeds on unmapped addresses too (type Unmapped, the size of
 * the hole), so a stack that is already gone must be told apart by its type. */
static void stack_block(uint64_t sp, uint64_t *lo, uint64_t *hi) {
  MemoryInfo mi; u32 pi;
  *lo = *hi = 0;
  if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, sp)) && mi.size >= 16 &&
      mi.type != MemType_Unmapped && (mi.perm & Perm_R)) {
    *lo = mi.addr;
    *hi = mi.addr + mi.size;
  }
}

/* What the dump needs from another thread's stack, copied WHILE that thread is
 * paused. Read after the resume, the stack can be gone: a thread that ends is
 * closed and libnx unmaps its stack (threadClose). That is the 08/11 crash
 * (0.1.42): the boot dump took ~8 s of card writes, short-lived "T18532" ended
 * meanwhile, and the watchdog's read of its frame faulted and killed the game.
 * Only memory reads and lock-free lookups run while the thread is paused, so a
 * lock it holds cannot deadlock the watchdog. */
typedef struct {
  ThreadContext ctx;
  int nbt, nret;
  uint64_t bt[32];
  struct { uint32_t off; uint8_t blr; uint64_t v; } ret[24];
} WdSnap;
static WdSnap g_snap;   /* watchdog thread only */

static void collect_stack(WdSnap *s, int scan) {
  s->nbt = s->nret = 0;
  uint64_t slo, shi;
  stack_block(s->ctx.sp, &slo, &shi);
  if (!slo) return;
  /* Clean backtrace via the frame-pointer (x29) chain: [fp]=caller fp, [fp+8]=lr.
   * fp <= shi - 16, not fp + 16 <= shi (wraps for fp = -16, seen in nx_prof.c). */
  uint64_t fp = s->ctx.fp;
  while (s->nbt < 32 && (fp & 7) == 0 && fp >= slo && fp <= shi - 16) {
    const uint64_t nextfp = ((const uint64_t *)(uintptr_t)fp)[0];
    const uint64_t lr     = ((const uint64_t *)(uintptr_t)fp)[1];
    if (!lr) break;
    s->bt[s->nbt++] = lr;
    if (nextfp <= fp) break;   /* fp must climb up the stack */
    fp = nextfp;
  }
  /* Unity's hand-written wait stubs clobber the FP chain, so the bt[] above
   * often dead-ends in our glue. Raw-scan the top of the stack for any slot that
   * points into libunity / libil2cpp code -- those are return addresses the FP
   * walk missed, and they reveal what the thread is actually wedged inside.
   * Caller gates this (main/loader threads only) to keep the log readable. */
  if (!scan) return;
  uint64_t sp = s->ctx.sp & ~7ull;
  if (sp < slo) sp = slo;
  uint64_t top = sp + 0x2000;            /* ~1024 slots is plenty for the active frames */
  if (top > shi) top = shi;
  for (uint64_t addr = sp; addr + 8 <= top && s->nret < 24; addr += 8) {
    uint64_t v = ((const uint64_t *)(uintptr_t)addr)[0];
    so_module *m = so_find_module_by_addr((const void *)(uintptr_t)v);
    if (!m) continue;
    if (!strstr(m->name, "unity") && !strstr(m->name, "il2cpp")) continue;  /* skip glue/main */
    if (v - 4 < (uint64_t)(uintptr_t)m->load_virtbase) continue;
    /* A real return address points to the instruction *after* a call, so the
     * 4 bytes at v-4 must be BL (0b100101 imm26) or BLR (0xD63F0000 mask).
     * Without this check the scan reports jump-table targets, vtable pointers
     * and stale frames -- all of which look like code addresses but are NOT on
     * the live call chain. This filter is what makes the backtrace trustworthy. */
    uint32_t prev = ((const uint32_t *)(uintptr_t)(v - 4))[0];
    int is_bl  = (prev & 0xFC000000u) == 0x94000000u;
    int is_blr = (prev & 0xFFFFFC1Fu) == 0xD63F0000u;
    if (!is_bl && !is_blr) continue;
    s->ret[s->nret].off = (uint32_t)(addr - sp);
    s->ret[s->nret].blr = (uint8_t)is_blr;
    s->ret[s->nret].v   = v;
    s->nret++;
  }
}

static void dump_thread_context(const char *name, const WdSnap *s) {
  const ThreadContext *ctx = &s->ctx;
  char a[40], b[40];
  resolve_addr(a, sizeof a, ctx->pc.x);
  resolve_addr(b, sizeof b, ctx->lr);
  stallPrintf("[wd]   %s  PC=%s  LR=%s\n", name, a, b);
  stallPrintf("[wd]     SP=0x%llx FP=0x%llx X0=0x%llx X1=0x%llx X2=0x%llx\n",
              (unsigned long long)ctx->sp, (unsigned long long)ctx->fp,
              (unsigned long long)ctx->cpu_gprs[0].x,
              (unsigned long long)ctx->cpu_gprs[1].x,
              (unsigned long long)ctx->cpu_gprs[2].x);
  /* For a thread parked in svcArbitrateLock, X1 is typically the mutex address
   * and X0 the owner's thread-handle tag -> identifies who holds the lock. */
  for (int i = 0; i < s->nbt; i++) {
    char t[40]; resolve_addr(t, sizeof t, s->bt[i]);
    stallPrintf("[wd]     bt[%d] %s\n", i, t);
  }
  for (int i = 0; i < s->nret; i++) {
    char t[48]; resolve_addr(t, sizeof t, s->ret[i].v);
    stallPrintf("[wd]     ret@0x%-4x %s%s\n", (unsigned)s->ret[i].off, t,
                s->ret[i].blr ? " (via blr)" : "");
  }
}

/* Pause just long enough to snapshot and copy the stack, RESUME before printing
 * (so the watchdog can't deadlock on a stdio/heap lock the paused thread was
 * holding). */
static void snapshot_thread(DiagThread *t) {
  if (!t->handle || t->handle == threadGetCurHandle()) return;
  /* Scan the stack only for the threads whose wait we actually need to diagnose:
   * the main render/UI thread and the async loaders. */
  int scan = (strstr(t->name, "Main") || strstr(t->name, "Preload") ||
              strstr(t->name, "AsyncRead") || t->is_main_engine) ? 1 : 0;
  WdSnap *s = &g_snap;
  Result pr = svcSetThreadActivity(t->handle, ThreadActivity_Paused);
  Result gr = R_SUCCEEDED(pr) ? svcGetThreadContext3(&s->ctx, t->handle) : pr;
  if (R_SUCCEEDED(gr)) collect_stack(s, scan);
  if (R_SUCCEEDED(pr)) svcSetThreadActivity(t->handle, ThreadActivity_Runnable);
  if (R_FAILED(gr)) { stallPrintf("[wd]   %-16s (snapshot failed rc=0x%x)\n", t->name, gr); return; }
  dump_thread_context(t->name[0] ? t->name : "?", s);
}

/* nx_crash_handler.c: is the faulting thread the watchdog? Its work is only
 * diagnostics, so a fault there stops the watchdog and leaves the game alone. */
int diag_thread_is_watchdog(void) {
  return g_wd_started && g_wd_thread.handle && g_wd_thread.handle == threadGetCurHandle();
}

static void dump_threads(int episode, uint64_t now) {
  /* Announce entry before touching anything. dump_threads walks other threads'
   * contexts and stacks; a bad pointer there kills this thread outright, and
   * that is what happened -- stall.log held one beacon and then nothing, with no
   * stall header, so the crash was inside the dump rather than the detector. */
  stallPrintf("[wd] dump_threads: entered\n");
  uint64_t stalled_ns = tick_to_ns(now - g_last_progress);
  stallPrintf("\n[wd] ===== STALL #%d : no frame progress for %llu.%llus (last frame=%d) =====\n",
              episode, (unsigned long long)(stalled_ns / 1000000000ull),
              (unsigned long long)((stalled_ns % 1000000000ull) / 100000000ull), g_frame);
  stallPrintf("[wd] %-16s %-10s %-11s %-18s %7s  d_wait d_wake d_spin\n",
              "name", "tid", "state", "wait_obj", "secs");
  for (int i = 0; i < DIAG_MAX_THREADS; i++) {
    DiagThread *t = &g_threads[i];
    if (!t->in_use) continue;
    int kind = t->wait_kind;
    uint64_t since = t->wait_since;
    uint64_t parked_ns = (kind != DIAG_W_NONE && since) ? tick_to_ns(now - since) : 0;
    uint64_t dwait = t->waits_total - prev_waits[i];
    uint64_t dwake = t->wakes_total - prev_wakes[i];
    uint64_t dspin = t->futex_spins - prev_spins[i];
    prev_waits[i] = t->waits_total;
    prev_wakes[i] = t->wakes_total;
    prev_spins[i] = t->futex_spins;
    stallPrintf("[wd] %-16s %-10llu %-11s 0x%-16llx %3llu.%llu  %6llu %6llu %6llu%s\n",
                t->name[0] ? t->name : "?",
                (unsigned long long)t->tid,
                wait_kind_name(kind),
                (unsigned long long)(uintptr_t)t->wait_obj,
                (unsigned long long)(parked_ns / 1000000000ull),
                (unsigned long long)((parked_ns % 1000000000ull) / 100000000ull),
                (unsigned long long)dwait, (unsigned long long)dwake,
                (unsigned long long)dspin,
                t->is_main_engine ? "  <engine_main>" : "");
  }
  stallPrintf("[wd] legend: d_* = delta since previous dump (0/0/0 == hard-parked; "
              "d_spin>0 == alive on futex; d_wait>d_wake == entered a wait it hasn't left)\n");
  nx_sig_dump();   /* a GC stop-the-world waiting on an ack shows up here */
  /* native backtrace: where each thread is wedged inside libunity/il2cpp/NRO */
  stallPrintf("[wd] --- thread CPU contexts (frame-pointer backtrace) ---\n");
  for (int i = 0; i < DIAG_MAX_THREADS; i++) {
    if (g_threads[i].in_use) snapshot_thread(&g_threads[i]);
  }
  stallPrintf("\n");
}

/* ---- autopsy (nx_autopsy.c) ------------------------------------------------
 * The same picture as dump_threads, written into a caller buffer with nx_bfmt:
 * no stdio, no SD card, no lock. It runs on the rumble thread while the
 * watchdog itself is one of the stuck threads. */
#define ADD(...) do { if (o < len) o += (unsigned)nx_bfmt(buf + o, len - o, __VA_ARGS__); } while (0)
static unsigned clamp_len(unsigned o, unsigned len) { return o < len ? o : (len ? len - 1 : 0); }

int diag_bfmt_addr(char *buf, unsigned n, uint64_t addr) {
  so_module *m = so_find_module_by_addr((const void *)(uintptr_t)addr);
  if (m) {
    const char *base = m->name, *p;
    for (p = m->name; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;
    return nx_bfmt(buf, n, "%s+0x%llx", base, (unsigned long long)(addr - (uint64_t)(uintptr_t)m->load_virtbase));
  }
  if (g_nro_size && addr >= g_nro_base && addr < g_nro_base + g_nro_size)
    return nx_bfmt(buf, n, "NRO+0x%llx", (unsigned long long)(addr - g_nro_base));
  return nx_bfmt(buf, n, "0x%llx", (unsigned long long)addr);
}

const char *diag_name_by_handle(Handle h) {
  for (int i = 0; i < DIAG_MAX_THREADS; i++)
    if (g_threads[i].in_use && g_threads[i].handle == h) return g_threads[i].name[0] ? g_threads[i].name : "?";
  return NULL;
}

/* nx_prof.c: is `a` inside loaded code (a game library or our NRO)? */
int diag_is_code_addr(uint64_t a) {
  if (so_find_module_by_addr((const void *)(uintptr_t)a)) return 1;
  return g_nro_size && a >= g_nro_base && a < g_nro_base + g_nro_size;
}

/* nx_prof.c: a registered thread by its exact name, 0 if none. */
Handle diag_handle_by_name(const char *name) {
  for (int i = 0; i < DIAG_MAX_THREADS; i++)
    if (g_threads[i].in_use && !strcmp(g_threads[i].name, name)) return g_threads[i].handle;
  return 0;
}

/* nx_prof.c: the thread that runs the engine's frames (UnityMain), 0 if not yet known. */
Handle diag_engine_main_handle(void) {
  for (int i = 0; i < DIAG_MAX_THREADS; i++)
    if (g_threads[i].in_use && g_threads[i].is_main_engine) return g_threads[i].handle;
  return 0;
}

/* "      PC=.. LR=.. SP=.. in svc 0x21 | bt ..." for thread `h`. Paused only for
 * the context read. A thread inside a system call has a partial context (no
 * x0-x18) and its PC is the svc instruction itself (seen on hardware: PC =
 * svcSleepThread+0), hence "in svc N" from the instruction at PC. */
unsigned diag_bfmt_context(Handle h, char *buf, unsigned len) {
  unsigned o = 0;
  if (!h || h == threadGetCurHandle()) return 0;
  ThreadContext ctx;
  uint64_t bt[14];
  int nbt = 0;
  Result pr = svcSetThreadActivity(h, ThreadActivity_Paused);
  Result gr = svcGetThreadContext3(&ctx, h);   /* also fine if the GC code paused it already */
  if (R_SUCCEEDED(gr)) {
    /* Walk the frames before the resume: once it runs on, the thread can end
     * and its stack be unmapped (see collect_stack). */
    uint64_t slo, shi;
    stack_block(ctx.sp, &slo, &shi);
    uint64_t fp = ctx.fp;
    /* fp <= shi - 16: "fp + 16 <= shi" wraps for fp = -16 (seen 02/11 in nx_prof.c) */
    while (slo && nbt < 14 && (fp & 7) == 0 && fp >= slo && fp <= shi - 16) {
      const uint64_t nextfp = ((const uint64_t *)(uintptr_t)fp)[0];
      const uint64_t lr     = ((const uint64_t *)(uintptr_t)fp)[1];
      if (!lr) break;
      bt[nbt++] = lr;
      if (nextfp <= fp) break;
      fp = nextfp;
    }
  }
  if (R_SUCCEEDED(pr)) svcSetThreadActivity(h, ThreadActivity_Runnable);
  if (R_FAILED(gr)) { ADD("      (no context: pause rc 0x%x, read rc 0x%x)\n", pr, gr); return clamp_len(o, len); }
  char a[64], b[64];
  diag_bfmt_addr(a, sizeof a, ctx.pc.x);
  diag_bfmt_addr(b, sizeof b, ctx.lr);
  ADD("      PC=%s LR=%s SP=0x%llx", a, b, (unsigned long long)ctx.sp);
  MemoryInfo mi; u32 pi;
  if (ctx.pc.x >= 0x1000 && (ctx.pc.x & 3) == 0 && R_SUCCEEDED(svcQueryMemory(&mi, &pi, ctx.pc.x)) &&
      mi.type != MemType_Unmapped && (mi.perm & Perm_R)) {
    uint32_t insn = *(const uint32_t *)(uintptr_t)ctx.pc.x;
    if ((insn & 0xFFE0001Fu) == 0xD4000001u) ADD(" in svc 0x%x", (insn >> 5) & 0xFFFF);
  }
  ADD("\n      bt");
  for (int i = 0; i < nbt; i++) {
    diag_bfmt_addr(a, sizeof a, bt[i]);
    ADD(" %s", a);
  }
  ADD("\n");
  return clamp_len(o, len);
}

unsigned diag_autopsy_threads(char *buf, unsigned len) {
  unsigned o = 0;
  uint64_t now = now_tick();
  for (int i = 0; i < DIAG_MAX_THREADS; i++) {
    DiagThread *t = &g_threads[i];
    if (!t->in_use) continue;
    int kind = t->wait_kind;
    uint64_t since = t->wait_since;
    uint64_t ms = (kind != DIAG_W_NONE && since) ? tick_to_ns(now - since) / 1000000ull : 0;
    ADD("  %-24s tid %-5llu h 0x%-6x %s", t->name[0] ? t->name : "?", (unsigned long long)t->tid, t->handle,
        wait_kind_name(kind));
    if (ms) ADD(" %llu.%llu s on 0x%llx", (unsigned long long)(ms / 1000), (unsigned long long)((ms % 1000) / 100),
                (unsigned long long)(uintptr_t)t->wait_obj);
    if (o < len) o += (unsigned)nx_autopsy_journal_bfmt(t->handle, buf + o, len - o, now);
    ADD("\n");
    if (o < len) o += diag_bfmt_context(t->handle, buf + o, len - o);
  }
  return clamp_len(o, len);
}
#undef ADD

static void watchdog_main(void *unused) {
  (void)unused;
  /* Own bionic TLS block. Every other thread that can reach engine-adjacent code
   * has one (main, clock, audio, the pthread shim); this thread never did. A
   * stack-protector prologue anywhere it reaches would read TPIDR_EL0+0x28 with
   * TPIDR unset and fault -- killing the watchdog exactly when it fires, which
   * matches the observed behaviour: one beacon, then silence, no stall header. */
  static uint8_t wd_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  install_bionic_tls(wd_tls);
  nx_autopsy_name_self("watchdog");
  int episode = 0;
  uint64_t last_dump = 0;
  /* prime so we don't false-trigger before the first frame */
  if (g_last_progress == 0) g_last_progress = now_tick();
  for (;;) {
    svcSleepThread(DIAG_POLL_NS);
    uint64_t now = now_tick();
    uint64_t idle = now - g_last_progress;
    const uint64_t limit = g_frame < 2 ? DIAG_BOOT_STALL_NS : DIAG_STALL_NS;

    /* Liveness beacon. Every diagnostic in this port has failed silently at
     * least once; a periodic "I am alive and here is the frame counter" line
     * makes the NEXT failure distinguishable from a dead watchdog thread. */
    {
      static uint64_t last_beat;
      if (tick_to_ns(now - last_beat) >= 2000000000ull) {
        last_beat = now;
        char gpu[128], fs[176];
        nx_gpu_beacon(gpu, sizeof gpu);
        extern void nx_fs_beacon(char *buf, unsigned len);
        nx_fs_beacon(fs, sizeof fs);
        stallPrintf("[wd] alive: frame=%d idle=%llums %s%s%s\n",
                    g_frame, (unsigned long long)(tick_to_ns(idle) / 1000000ull), gpu, fs,
                    tick_to_ns(idle) >= limit ? "  STALLED" : "");
      }
    }

    if (tick_to_ns(idle) >= limit) {
      if (last_dump == 0 || tick_to_ns(now - last_dump) >= DIAG_REDUMP_NS) {
        dump_threads(++episode, now);
        last_dump = now;
      }
    } else {
      /* progress resumed: reset so a later stall dumps fresh */
      last_dump = 0;
    }
  }
}

/* One line of live state for nx_netbeat.c (UDP to a PC). No file I/O, no
 * locks, no allocation: the whole-console freezes stop everything that writes
 * to the SD card, the watchdog's stall.log included, so the last state has to
 * leave the console by another way. Threads parked for over 1 s are listed. */
int diag_netbeat_line(char *buf, unsigned len) {
  uint64_t now = now_tick();
  char gpu[128], fs[176];
  nx_gpu_beacon(gpu, sizeof gpu);
  extern void nx_fs_beacon(char *b, unsigned l);
  nx_fs_beacon(fs, sizeof fs);
  int o = nx_bfmt(buf, len, "frame=%d idle=%llums %s%s |", g_frame,
                   (unsigned long long)(tick_to_ns(now - g_last_progress) / 1000000ull), gpu, fs);
  for (int i = 0; i < DIAG_MAX_THREADS && o > 0 && (unsigned)o < len - 64; i++) {
    DiagThread *t = &g_threads[i];
    if (!t->in_use) continue;
    int kind = t->wait_kind;
    uint64_t since = t->wait_since;
    if (kind == DIAG_W_NONE || !since) continue;
    uint64_t ms = tick_to_ns(now - since) / 1000000ull;
    if (ms < 1000) continue;
    o += nx_bfmt(buf + o, len - o, " %s:%s:%llums", t->name[0] ? t->name : "?",
                  wait_kind_name(kind), (unsigned long long)ms);
  }
  return o;
}

void diag_watchdog_start(void) {
  if (g_wd_started) return;
  g_wd_started = 1;
  nro_range_init();
  if (g_last_progress == 0) g_last_progress = now_tick();
  /* libnx thread: deliberately NOT via the pthread shim under test.
   * 16 KiB stack, priority 0x2C (same band as main), default core. */
  /* Try to outrank the engine, but NEVER fail closed.
   *
   * The failure being chased is a 100% CPU spin, and at equal priority (0x2C)
   * the watchdog is simply never scheduled against it -- two beacons then
   * silence, every run. But a blind jump to 0x18 was refused outright
   * (rc=0xe001, invalid priority: hbloader only permits a narrow band around
   * its own 0x2C) and the watchdog did not start AT ALL, which is strictly
   * worse than being starved.
   *
   * So: walk from the most useful priority down to the known-good one and take
   * the first that works. A modest bump still wins the scheduler against a
   * same-priority spinner. */
  static const int PRIOS[] = { 0x28, 0x2A, 0x2B, 0x2C };
  Result rc = 0xe001;
  int used = 0;
  for (unsigned i = 0; i < sizeof(PRIOS) / sizeof(PRIOS[0]); i++) {
    /* core 2 explicitly, not -2 (= "default"). The engine's main thread and its
     * job workers saturate the default core; giving the watchdog its own means
     * a spinner cannot hide from it even at equal priority. */
    rc = threadCreate(&g_wd_thread, watchdog_main, NULL, NULL, 0x4000, PRIOS[i], 2);
    if (R_FAILED(rc))   /* core 2 unavailable in this configuration */
      rc = threadCreate(&g_wd_thread, watchdog_main, NULL, NULL, 0x4000, PRIOS[i], -2);
    if (R_SUCCEEDED(rc)) { used = PRIOS[i]; break; }
  }
  if (R_SUCCEEDED(rc) && R_SUCCEEDED(threadStart(&g_wd_thread)))
    stallPrintf("[wd] watchdog armed (stall=%llus, poll=1s, prio=0x%02x)\n",
                (unsigned long long)(DIAG_STALL_NS / 1000000000ull), used);
  else
    stallPrintf("[wd] watchdog FAILED to start rc=0x%x\n", rc);
}
