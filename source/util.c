/* util.c -- misc utility functions
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "util.h"
#include <time.h>
#include "config.h"
#include "cloverpit_root.h"

/* ---------------------------------------------------------------------------
 * Logger -> <root>/debug.log  (mvlo_nx; adapted from the cloverpit_nx logger)
 *
 * Always compiled in. g_log_level (from [debug] log_level in mvlo.ini):
 *   0  only "important" lines (failures, phase changes, crash/watchdog)
 *   1  everything except the high-frequency dlsym/dlopen/JNI chatter (default)
 *   2  everything (slow: the SD card is the bottleneck)
 * Each line is prefixed with seconds since boot. The previous session's log is
 * kept as debug.prev.log so a crash log is not overwritten by the next launch.
 *
 * Buffered and flushed on a 2 s timer, on important lines, and by the crash
 * handler -- per-line fflush made the engine spend whole frames inside
 * fsFileWrite on the original port (see cloverpit_nx notes).
 *
 * Once debug_log_start_writer() has run, debugPrintf only formats into a RAM
 * queue and a "log" thread writes it out: now and then the SD card takes
 * 150-375 ms for one fsFileWrite, and the render loop's periodic [loop]/[gpu]
 * lines put those stalls on the game's main thread -- the in-match hitches of
 * the 2026-11-03 session (prof.log: debugPrintf <- fflush <- fsFileWrite).
 * ------------------------------------------------------------------------- */
int g_log_level = 1;
int g_log_sync = 0;    /* [debug] log_sync: flush every line + mirror to svcOutputDebugString */

static Mutex g_log_lock;
static int log_is_noisy(const char *t) {
  return !strncmp(t, "dlsym", 5) || !strncmp(t, "dlopen", 6) ||
         !strncmp(t, "JNI ", 4)  || !strncmp(t, "JNI:", 4) || !strncmp(t, "[jni]", 5);
}

static FILE   *g_logf = NULL;
static char    g_logbuf[64 * 1024];
static uint64_t g_log_last_flush_ns = 0;
static uint64_t g_log_t0 = 0;
static int      g_log_rotated = 0;

#define LOG_FLUSH_INTERVAL_NS  2000000000ull   /* 2s */

/* Two queues: debugPrintf appends to g_lq[g_lq_cur] under g_log_lock; a drain
 * swaps them and writes the full one under g_log_io_lock, which serialises
 * drains so the queue being written is never the one being filled. */
#define LOGQ_BYTES (256 * 1024)
static char     g_lq[2][LOGQ_BYTES];
static size_t   g_lq_len;
static int      g_lq_cur;
static size_t   g_lq_dropped;          /* bytes lost to a full queue (SD stuck) */
static Mutex    g_log_io_lock;
static CondVar  g_log_cv;
static int      g_log_kick;
static int      g_log_stop;            /* asks the log thread to drain once more and end */
static int      g_log_done;            /* debug.log closed for good (process exit) */
static volatile int g_log_writer;      /* 1 = the log thread drains the queue */
static Thread   g_log_thread;
static Handle   g_drain_owner;         /* thread inside log_drain (re-entry guard) */

/* Append to the current queue; caller holds g_log_lock. A piece that does not
 * fit is dropped whole and counted. */
static void lq_vappend(const char *fmt, va_list ap) {
  size_t room = LOGQ_BYTES - g_lq_len;
  int n = vsnprintf(g_lq[g_lq_cur] + g_lq_len, room, fmt, ap);
  if (n < 0) return;
  if ((size_t)n >= room) { g_lq_dropped += (size_t)n; return; }
  g_lq_len += (size_t)n;
}
static void lq_append(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  lq_vappend(fmt, ap);
  va_end(ap);
}

static uint64_t log_now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Lines that must reach the card immediately. */
static int log_is_important(const char *t) {
  while (*t == '\n' || *t == '\r') t++;
  return !strncmp(t, "[crash]", 7) || !strncmp(t, "[wd]", 4) ||
         !strncmp(t, "[loop]", 6) || !strncmp(t, "[vsync]", 7) ||
         !strncmp(t, "[gfx]", 5) || !strncmp(t, "[gc]", 4) ||
         !strncmp(t, "[sig]", 5) || !strncmp(t, "[net]", 5) ||
         !strncmp(t, "[gpua]", 6) || !strncmp(t, "[fmod]", 6) ||
         !strncmp(t, "[input]", 7) || !strncmp(t, "[nxport]", 8) ||
         !strncmp(t, "[boot]", 6)  || !strncmp(t, "[region]", 8) ||
         !strncmp(t, "[patch]", 7) || !strncmp(t, "[cfg]", 5) ||
         strstr(t, "FATAL") || strstr(t, "ABORT") || strstr(t, "failed") ||
         strstr(t, "Exception") || strstr(t, "ERROR");
}

/* Independent log for the watchdog (never shares debugPrintf's lock). */
static Mutex g_stall_lock;
static int   g_stall_init;

int stallPrintf(char *text, ...) {
  va_list list;
  char path[600];
  if (!g_stall_init) { mutexInit(&g_stall_lock); g_stall_init = 1; }
  mutexLock(&g_stall_lock);
  snprintf(path, sizeof path, "%s/stall.log", cp_game_root());
  FILE *f = fopen(path, "a");
  if (f) {
    va_start(list, text);
    vfprintf(f, text, list);
    va_end(list);
    fclose(f);
  }
  mutexUnlock(&g_stall_lock);
  return 0;
}

static void log_open_locked(void);

/* Write out what is queued. Never called with g_log_lock held. */
static void log_drain(void) {
  if (g_drain_owner == threadGetCurHandle()) return;   /* logged from inside fwrite */
  mutexLock(&g_log_io_lock);
  if (g_log_done) {                    /* the SD card may be gone: drop it */
    mutexLock(&g_log_lock);
    g_lq_len = 0; g_lq_dropped = 0;
    mutexUnlock(&g_log_lock);
    mutexUnlock(&g_log_io_lock);
    return;
  }
  g_drain_owner = threadGetCurHandle();
  mutexLock(&g_log_lock);
  log_open_locked();
  int q = g_lq_cur;
  size_t n = g_lq_len, dropped = g_lq_dropped;
  g_lq_cur ^= 1; g_lq_len = 0; g_lq_dropped = 0;
  g_log_last_flush_ns = log_now_ns();
  mutexUnlock(&g_log_lock);
  if (g_logf && (n || dropped)) {
    if (n) fwrite(g_lq[q], 1, n, g_logf);
    if (dropped) fprintf(g_logf, "[log] %u bytes of log lost (the SD card did not keep up)\n", (unsigned)dropped);
    fflush(g_logf);
  }
  g_drain_owner = 0;
  mutexUnlock(&g_log_io_lock);
}

void debug_log_flush(void) { log_drain(); }

/* The render loop's periodic flush: the log thread does it when it runs. */
void debug_log_kick(void) {
  if (!g_log_writer) { log_drain(); return; }
  mutexLock(&g_log_lock);
  g_log_kick = 1;
  condvarWakeOne(&g_log_cv);
  mutexUnlock(&g_log_lock);
}

/* From the crash handler: the faulting thread may hold either lock, so only
 * write if both are free right now. */
void debug_log_emergency(void) {
  if (!mutexTryLock(&g_log_io_lock)) return;
  if (mutexTryLock(&g_log_lock)) {
    int q = g_lq_cur;
    size_t n = g_lq_len;
    g_lq_len = 0;
    mutexUnlock(&g_log_lock);
    if (g_logf && n) { fwrite(g_lq[q], 1, n, g_logf); fflush(g_logf); }
  }
  mutexUnlock(&g_log_io_lock);
}

static void log_thread(void *arg) {
  (void)arg;
  for (;;) {
    mutexLock(&g_log_lock);
    if (!g_log_kick && !g_log_stop) condvarWaitTimeout(&g_log_cv, &g_log_lock, LOG_FLUSH_INTERVAL_NS);
    g_log_kick = 0;
    int stop = g_log_stop;
    mutexUnlock(&g_log_lock);
    log_drain();
    if (stop) return;
  }
}

/* libnx calls this from __libnx_exit, before it unmounts the SD card. The log
 * thread must be gone by then: in 0.1.40 it flushed into the unmounted card
 * and every exit ended on the Switch's error screen. Later lines are dropped. */
void userAppExit(void) {
  if (g_log_writer) {
    mutexLock(&g_log_lock);
    g_log_writer = 0;
    g_log_stop = 1;
    condvarWakeOne(&g_log_cv);
    mutexUnlock(&g_log_lock);
    threadWaitForExit(&g_log_thread);
    threadClose(&g_log_thread);
  }
  log_drain();
  mutexLock(&g_log_io_lock);
  g_log_done = 1;
  if (g_logf) { fclose(g_logf); g_logf = NULL; }
  mutexUnlock(&g_log_io_lock);
}

void debug_log_start_writer(void) {
  if (g_log_writer || g_log_sync) return;
  condvarInit(&g_log_cv);
  /* Below the game's threads: it only has to keep up over seconds. */
  if (R_FAILED(threadCreate(&g_log_thread, log_thread, NULL, NULL, 0x8000, 0x3B, -2)) ||
      R_FAILED(threadStart(&g_log_thread))) {
    debugPrintf("[log] writer thread failed -- logging stays on the caller's thread\n");
    return;
  }
  g_log_writer = 1;
  debugPrintf("[log] writer thread started (the game no longer waits on the SD card for its log)\n");
}

/* stall.log and prof.log are appended to by every launch: past 1 MB the file
 * becomes <name>.prev.log (one generation kept). They had reached 3.8 and
 * 4.6 MB by 05/11. */
static void log_rotate_big(const char *name) {
  char cur[600], prev[640];
  struct stat st;
  snprintf(cur, sizeof cur, "%s/%s.log", cp_game_root(), name);
  if (stat(cur, &st) != 0 || st.st_size <= 1024 * 1024) return;
  snprintf(prev, sizeof prev, "%s/%s.prev.log", cp_game_root(), name);
  remove(prev);
  rename(cur, prev);
}

static void log_open_locked(void) {
  if (g_logf) return;
  if (!g_log_rotated) {
    char prev[640];
    g_log_rotated = 1;
    snprintf(prev, sizeof prev, "%s/debug.prev.log", cp_game_root());
    remove(prev);
    rename(cp_log_path(), prev);
    log_rotate_big("stall");
    log_rotate_big("prof");
  }
  g_logf = fopen(cp_log_path(), "w");
  if (g_logf) setvbuf(g_logf, g_logbuf, _IOFBF, sizeof g_logbuf);
  g_log_last_flush_ns = log_now_ns();
  if (!g_log_t0) g_log_t0 = g_log_last_flush_ns;
}

int debugPrintf(char *text, ...) {
  va_list list;
  int want_flush = 0, important;
  if (!text) return 0;
  important = log_is_important(text);
  if (g_log_level <= 0 && !important) return 0;
  if (g_log_level == 1 && log_is_noisy(text)) return 0;
  mutexLock(&g_log_lock);
  uint64_t now = log_now_ns();
  if (!g_log_t0) g_log_t0 = g_log_last_flush_ns = now;
  /* Timestamp only at the start of a line (callers sometimes log in pieces). */
  static int at_line_start = 1;
  if (at_line_start) {
    uint64_t ms = (now - g_log_t0) / 1000000ull;
    lq_append("%5llu.%03llu ", (unsigned long long)(ms / 1000), (unsigned long long)(ms % 1000));
  }
  if (g_log_sync) {
    char line[1024];
    va_start(list, text);
    int ln = vsnprintf(line, sizeof line, text, list);
    va_end(list);
    if (ln > (int)sizeof line - 1) ln = sizeof line - 1;
    lq_append("%s", line);
    if (ln > 0) svcOutputDebugString(line, (u64)ln);
    important = 1;
  } else {
    va_start(list, text);
    lq_vappend(text, list);
    va_end(list);
  }
  size_t tl = strlen(text);
  at_line_start = tl > 0 && text[tl - 1] == '\n';
  want_flush = important || now - g_log_last_flush_ns >= LOG_FLUSH_INTERVAL_NS ||
               g_lq_len > LOGQ_BYTES / 2;
  if (want_flush && g_log_writer) { g_log_kick = 1; condvarWakeOne(&g_log_cv); }
  mutexUnlock(&g_log_lock);
  if (want_flush && !g_log_writer) log_drain();
  if (g_log_sync) fsdevCommitDevice("sdmc");
  return 0;
}

// Per-thread bionic TLS. The engine reads its stack canary from tpidr_el0+0x28;
// every thread that runs engine code needs its OWN zeroed block here. A single
// shared block races: one thread's TLS writes (including the guard slot) corrupt
// another thread's in-flight canary, tripping a false __stack_chk_fail. `buf`
// must outlive the thread (TPIDR_EL0 points into it until the thread exits).
void install_bionic_tls(void *buf) {
  memset(buf, 0, BIONIC_TLS_SIZE);
  armSetTlsRw((uint8_t *)buf + BIONIC_TLS_TP_OFFSET);
}

// boost the CPU to 1785MHz while loading
volatile int g_cpu_boost_on;   /* last mode asked for (netlog heartbeat) */
void cpu_boost(int on) {
  g_cpu_boost_on = on;
  appletSetCpuBoostMode(on ? ApmCpuBoostMode_FastLoad : ApmCpuBoostMode_Normal);
}

int ret0(void) { return 0; }

int retm1(void) { return -1; }
