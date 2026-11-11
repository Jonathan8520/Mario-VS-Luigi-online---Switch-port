/* nx_selftest.c -- [debug] smoke_test = 3: exercise the signal emulation
 * (nx_signals.c) with bdwgc's exact stop-the-world protocol, through the same
 * import table the game uses (so the shims under test are the real ones):
 *
 *   worker threads park in pthread_cond_wait / usleep / sem_wait / mutex
 *   collector: pthread_kill(SIGPWR) each -> handler records sp, sem_post(ack),
 *              sigsuspend() until SIGXCPU -> collector sem_wait()s N acks,
 *              then pthread_kill(SIGXCPU) each and waits N acks again.
 *
 * MIT -- part of mvlo_nx. */
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <switch.h>

#include <time.h>
#include "imports.h"
#include "nx_signals.h"
#include "so_util.h"
#include "util.h"

typedef int  (*fn_sigaction)(int, const void *, void *);
typedef int  (*fn_kill)(pthread_t, int);
typedef int  (*fn_sigsuspend)(const uint64_t *);
typedef int  (*fn_sem)(void **);
typedef int  (*fn_sem_timed)(void **, const struct timespec *);
typedef int  (*fn_sem_init)(void **, int, unsigned);
typedef int  (*fn_create)(pthread_t *, const void *, void *, void *);
typedef int  (*fn_usleep)(unsigned);
typedef int  (*fn_mtx)(void **);
typedef int  (*fn_cond_wait)(void **, void **);
typedef pthread_t (*fn_self)(void);

#define SIG_SUSP 30
#define SIG_REST 24
#define NW 5                        /* 4 parked in shims + 1 spinning in "game code" */
#define SPIN (NW)

/* A loop with no call at all -- like Quantum's TaskContext.EndFrame waiting
 * for its workers: it never reaches a safepoint, only forced delivery
 * (nx_signals.c) can stop it. x0 = { int run; int pad; u64 count; }. */
void nx_selftest_spin(volatile void *state);
extern const char nx_selftest_spin_end[];
__asm__(".text\n.balign 16\n.global nx_selftest_spin\n.type nx_selftest_spin, %function\n"
        "nx_selftest_spin:\n"
        "1: ldr w1, [x0]\n"
        "   cbz w1, 2f\n"
        "   ldr x2, [x0, #8]\n"
        "   add x2, x2, #1\n"
        "   str x2, [x0, #8]\n"
        "   b 1b\n"
        "2: ret\n"
        ".global nx_selftest_spin_end\n"
        "nx_selftest_spin_end:\n");
static volatile struct { int run, pad; uint64_t count; } g_spin = { 1, 0, 0 };
static volatile uintptr_t g_spin_frame;

static fn_sigsuspend g_sigsuspend;
static fn_sem g_post;
static fn_self g_self;
static void *g_ack;                 /* bionic sem_t storage (pointer slot) */
static volatile int g_world_stopped, g_stop_count;
static volatile uintptr_t g_sp[NW + 1];
static volatile int g_run = 1;
static pthread_t th[NW];

/* Who am I, the way bdwgc asks it: pthread_self() through the import table
 * (a proxy delivering for a paused thread answers with that thread's id). */
static int my_index(void) {
  pthread_t me = g_self();
  for (int i = 0; i < NW; i++) if (pthread_equal(th[i], me)) return i + 1;
  return 0;
}

static void susp_handler(int sig, void *info, void *ctx) {
  (void)info; (void)ctx;
  if (sig != SIG_SUSP) return;
  int my = g_stop_count;
  g_sp[my_index()] = (uintptr_t)__builtin_frame_address(0);
  g_post(&g_ack);
  uint64_t mask = ~0ull & ~(1ull << (SIG_REST - 1));
  do { g_sigsuspend(&mask); } while (g_world_stopped && g_stop_count == my);
  g_post(&g_ack);
}
static void rest_handler(int sig, void *info, void *ctx) { (void)sig; (void)info; (void)ctx; }

static void *mtx; static void *cnd;
static void *worker(void *arg) {
  int i = (int)(uintptr_t)arg;
  if (i == SPIN) {
    volatile int here;
    g_spin_frame = (uintptr_t)&here;
    nx_selftest_spin(&g_spin);
    return NULL;
  }
  fn_usleep us = (fn_usleep)dynlib_find_export("usleep");
  fn_cond_wait cw = (fn_cond_wait)dynlib_find_export("pthread_cond_wait");
  fn_mtx lk = (fn_mtx)dynlib_find_export("pthread_mutex_lock");
  fn_mtx ul = (fn_mtx)dynlib_find_export("pthread_mutex_unlock");
  while (g_run) {
    if (i % 2) us(50000);
    else { lk(&mtx); cw(&cnd, &mtx); ul(&mtx); }
  }
  return NULL;
}

/* n acks within ms; returns how many arrived. */
static int wait_acks(fn_sem_timed tw, int n, int ms) {
  int got = 0;
  for (; got < n; got++) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000; ts.tv_nsec += (long)(ms % 1000) * 1000000l;
    if (ts.tv_nsec >= 1000000000l) { ts.tv_sec++; ts.tv_nsec -= 1000000000l; }
    if (tw(&g_ack, &ts) != 0) break;
  }
  return got;
}

int nx_selftest_signals(void) {
  struct { int flags, pad; void *h; uint64_t mask; void *r; } act;
  fn_sigaction sa = (fn_sigaction)dynlib_find_export("sigaction");
  fn_kill kill = (fn_kill)dynlib_find_export("pthread_kill");
  fn_sem_init si = (fn_sem_init)dynlib_find_export("sem_init");
  fn_sem_timed tw = (fn_sem_timed)dynlib_find_export("sem_timedwait");
  fn_create create = (fn_create)dynlib_find_export("pthread_create");
  g_sigsuspend = (fn_sigsuspend)dynlib_find_export("sigsuspend");
  g_post = (fn_sem)dynlib_find_export("sem_post");
  g_self = (fn_self)dynlib_find_export("pthread_self");
  if (!sa || !kill || !si || !tw || !create || !g_sigsuspend || !g_post || !g_self) {
    debugPrintf("[selftest] FAILED: shim lookup\n");
    return -1;
  }
  si(&g_ack, 0, 0);
  memset(&act, 0, sizeof act); act.flags = 4; act.h = (void *)susp_handler; sa(SIG_SUSP, &act, NULL);
  act.h = (void *)rest_handler; sa(SIG_REST, &act, NULL);
  nx_sig_force_allow((uintptr_t)nx_selftest_spin, (uintptr_t)nx_selftest_spin_end);

  for (int i = 0; i < NW; i++) create(&th[i], NULL, (void *)worker, (void *)(uintptr_t)(i + 1));
  svcSleepThread(200000000ull);

  int ok = 0;
  for (int round = 0; round < 5; round++) {
    uint64_t t0 = armTicksToNs(armGetSystemTick());
    g_stop_count += 2; g_world_stopped = 1;
    memset((void *)g_sp, 0, sizeof g_sp);
    for (int i = 0; i < NW; i++) kill(th[i], SIG_SUSP);
    int a1 = wait_acks(tw, NW, 3000);
    uint64_t t1 = armTicksToNs(armGetSystemTick());
    int sps = 0;
    for (int i = 1; i <= NW; i++) sps += g_sp[i] != 0;
    /* The spinner's handler ran on ITS stack, below the frame it spins in. */
    uintptr_t ssp = g_sp[SPIN];
    int spin_ok = ssp && ssp < g_spin_frame && g_spin_frame - ssp < 64 * 1024;
    uint64_t c0 = g_spin.count;
    g_world_stopped = 0;
    for (int i = 0; i < NW; i++) kill(th[i], SIG_REST);
    int a2 = wait_acks(tw, NW, 3000);
    svcSleepThread(20000000ull);
    int resumed = g_spin.count != c0;
    uint64_t t2 = armTicksToNs(armGetSystemTick());
    debugPrintf("[selftest] round %d: stop %llu us (%d/%d acks), %d/%d stack pointers, spinner sp %s, "
                "restart %llu us (%d/%d acks), spinner %s\n",
                round, (unsigned long long)((t1 - t0) / 1000), a1, NW, sps, NW,
                spin_ok ? "on its own stack" : "WRONG", (unsigned long long)((t2 - t1) / 1000), a2, NW,
                resumed ? "running again" : "NOT RESUMED");
    ok += sps == NW && a1 == NW && a2 == NW && spin_ok && resumed;
    if (a1 != NW || a2 != NW) break;   /* acks out of step: later rounds mean nothing */
  }
  g_run = 0;
  g_spin.run = 0;
  uint32_t fo = 0, sk = 0, fa = 0;
  nx_sig_force_stats(&fo, &sk, &fa);
  debugPrintf("[selftest] signal emulation: %s (%d/5 rounds; forced deliveries %u, skipped %u, failed %u)\n",
              ok == 5 ? "PASSED" : "FAILED", ok, fo, sk, fa);
  return ok == 5 ? 0 : -1;
}

/* [debug] gc_spin_test = N: at frame N, a thread attached to IL2CPP spins in a
 * loop with no call while the engine thread runs a full collection -- the
 * shape of the Quantum deadlock, with the real collector. Without forced
 * delivery il2cpp_gc_collect never returns (the watchdog then shows it). */
static void *gc_spinner(void *arg) {
  extern so_module il2cpp_mod;
  void *(*dom)(void) = (void *)so_try_find_addr_rx(&il2cpp_mod, "il2cpp_domain_get");
  void *(*att)(void *) = (void *)so_try_find_addr_rx(&il2cpp_mod, "il2cpp_thread_attach");
  void (*det)(void *) = (void *)so_try_find_addr_rx(&il2cpp_mod, "il2cpp_thread_detach");
  void *t = att(dom());
  g_spin_frame = 1;
  nx_selftest_spin(&g_spin);
  det(t);
  g_spin_frame = 2;
  (void)arg;
  return NULL;
}

void nx_selftest_gc_spin(void) {
  extern so_module il2cpp_mod;
  void (*collect)(int) = (void *)so_try_find_addr_rx(&il2cpp_mod, "il2cpp_gc_collect");
  if (!collect || !so_try_find_addr_rx(&il2cpp_mod, "il2cpp_thread_attach") ||
      !so_try_find_addr_rx(&il2cpp_mod, "il2cpp_domain_get") || !so_try_find_addr_rx(&il2cpp_mod, "il2cpp_thread_detach")) {
    debugPrintf("[selftest] GC with a spinning thread: il2cpp exports missing -- skipped\n");
    return;
  }
  fn_create create = (fn_create)dynlib_find_export("pthread_create");
  nx_sig_force_allow((uintptr_t)nx_selftest_spin, (uintptr_t)nx_selftest_spin_end);
  g_spin.run = 1; g_spin.count = 0; g_spin_frame = 0;
  pthread_t t;
  create(&t, NULL, (void *)gc_spinner, NULL);
  for (int i = 0; i < 200 && g_spin_frame != 1; i++) svcSleepThread(5000000ull);
  svcSleepThread(50000000ull);
  uint32_t f0 = 0; nx_sig_force_stats(&f0, NULL, NULL);
  debugPrintf("[selftest] GC with a spinning thread: collecting (spinner at %llu loops)...\n",
              (unsigned long long)g_spin.count);
  uint64_t t0 = armTicksToNs(armGetSystemTick());
  collect(0);
  uint64_t t1 = armTicksToNs(armGetSystemTick());
  uint64_t c0 = g_spin.count;
  svcSleepThread(20000000ull);
  int resumed = g_spin.count != c0;
  uint32_t f1 = 0; nx_sig_force_stats(&f1, NULL, NULL);
  g_spin.run = 0;
  for (int i = 0; i < 200 && g_spin_frame != 2; i++) svcSleepThread(5000000ull);
  debugPrintf("[selftest] GC with a spinning thread: collect returned after %llu ms, %u forced deliveries, "
              "spinner %s, detached %s -- %s\n",
              (unsigned long long)((t1 - t0) / 1000000ull), f1 - f0, resumed ? "running again" : "NOT RESUMED",
              g_spin_frame == 2 ? "yes" : "NO", (resumed && f1 > f0 && g_spin_frame == 2) ? "PASSED" : "FAILED");
}

/* [debug] smoke_test = 4: DNS + UDP round trip through the bionic-ABI socket
 * shims (exactly what IL2CPP's System.Net.Sockets calls): resolve the Photon
 * name server, then an SNTP request to pool.ntp.org:123 with SO_RCVTIMEO. */
typedef struct l_ai { int flags, family, socktype, protocol; uint32_t addrlen; char *canon; void *addr; struct l_ai *next; } l_ai;
int nx_selftest_net(void) {
  int (*gai)(const char *, const char *, const l_ai *, l_ai **) = (void *)dynlib_find_export("getaddrinfo");
  void (*fai)(l_ai *) = (void *)dynlib_find_export("freeaddrinfo");
  int (*sock)(int, int, int) = (void *)dynlib_find_export("socket");
  long (*sto)(int, const void *, size_t, int, const void *, unsigned) = (void *)dynlib_find_export("sendto");
  long (*rfrom)(int, void *, size_t, int, void *, unsigned *) = (void *)dynlib_find_export("recvfrom");
  int (*sso)(int, int, int, const void *, unsigned) = (void *)dynlib_find_export("setsockopt");
  int (*cls)(int) = (void *)dynlib_find_export("close");
  int *(*err)(void) = (void *)dynlib_find_export("__errno");
  const char *hosts[] = { "ns.photonengine.io", "ns.exitgames.com", "pool.ntp.org" };
  int dns_ok = 0;
  for (unsigned i = 0; i < 3; i++) {
    l_ai hints = {0}, *res = NULL;
    hints.family = 2; hints.socktype = 2;
    int rc = gai(hosts[i], "123", &hints, &res);
    debugPrintf("[selftest] getaddrinfo(%s) = %d %s\n", hosts[i], rc, res ? "(got address)" : "");
    if (rc == 0 && res) dns_ok++;
    if (i == 2 && rc == 0 && res) {
      int fd = sock(2, 2, 0);
      int64_t tv[2] = { 3, 0 };
      sso(fd, 1 /*SOL_SOCKET*/, 20 /*SO_RCVTIMEO*/, tv, 16);
      uint8_t pkt[48] = { 0x1b };   /* SNTP v3 client request */
      long s = sto(fd, pkt, sizeof pkt, 0, res->addr, res->addrlen);
      uint8_t in[64]; uint8_t from[16]; unsigned fl = sizeof from;
      long r = rfrom(fd, in, sizeof in, 0, from, &fl);
      debugPrintf("[selftest] UDP NTP: sent %ld, received %ld (errno %d), mode=%d -- %s\n", s, r,
                  r < 0 ? *err() : 0, r > 0 ? (in[0] & 7) : -1, r == 48 ? "UDP PASSED" : "UDP FAILED");
      cls(fd);
    }
    if (res) fai(res);
  }
  debugPrintf("[selftest] DNS: %d/3 names resolved\n", dns_ok);
  {
    extern unsigned nx_bsdmon_report(char *buf, unsigned len, int last_ops);
    static char rep[8192];
    nx_bsdmon_report(rep, sizeof rep, 24);
    debugPrintf("[selftest] %s", rep);
  }
  {
    int (*conn)(int, const void *, unsigned) = (void *)dynlib_find_export("connect");
    int fd = sock(2, 1, 0);
    uint8_t sa[16] = { 2, 0, 0, 1, 127, 0, 0, 1 };   /* AF_INET, port 1, 127.0.0.1 */
    int r = conn(fd, sa, 16);
    debugPrintf("[selftest] TCP connect 127.0.0.1:1 -> %d errno %d (Linux ECONNREFUSED = 111) -- %s\n",
                r, *err(), (r < 0 && *err() == 111) ? "errno PASSED" : "errno CHECK");
    cls(fd);
  }
  return 0;
}
