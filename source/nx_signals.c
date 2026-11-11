/* nx_signals.c -- POSIX signal emulation for IL2CPP's Boehm GC (and anyone
 * else who uses pthread_kill), delivered at SAFEPOINTS inside our shims.
 *
 * WHY
 * bdwgc stops the world by pthread_kill(SIGPWR)-ing every registered thread.
 * Each thread's handler records its stack pointer (GC_store_stack_ptr),
 * sem_posts an ack and parks in sigsuspend() until SIGXCPU restarts it. Horizon
 * has no signals at all. cloverpit_nx answered pthread_kill by posting the ack
 * itself without running the handler -- which leaves stop_info.stack_ptr unset,
 * and bdwgc then dies in GC_push_all_stacks ("sp not set!") or scans a stale
 * range. (Unity's bdwgc source, pthread_stop_world.c, makes this explicit.)
 *
 * WHAT THIS DOES INSTEAD
 * The real handler runs, on the target thread, the next time that thread
 * enters one of our blocking shims (futex, cond/sem waits, sleeps, mutex
 * contention, socket and pipe waits -- all of which are already sliced into
 * <=16 ms waits). That is a faithful signal delivery: GC_approx_sp() then
 * returns a point below every live game frame, the callee-saved registers are
 * spilled onto the stack first (on Linux the kernel's signal frame does that),
 * the ack goes through the game's own sem_post, and sigsuspend() blocks in our
 * shim until the restart signal arrives.
 *
 * A thread that never reaches a safepoint -- a spin loop in managed code, like
 * Quantum's TaskContext.EndFrame waiting for its workers while those workers
 * are parked by this very collection -- would deadlock the game. After
 * [debug] gc_force_ms the "forcer" thread pauses it with svcSetThreadActivity
 * (it must be executing game code, see force_one), reads its registers, and a
 * proxy thread runs the game's handler ON THE PAUSED THREAD'S STACK, below its
 * SP, after copying all its registers there, impersonating it (pthread_self,
 * TLS register). To bdwgc that is exactly a signal frame. When the handler
 * returns (restart signal) the thread is resumed where it was.
 *
 * All values crossing this boundary are Linux/bionic encoded: sigset_t is a
 * 64-bit mask with bit (sig-1), SIG_BLOCK=0/UNBLOCK=1/SETMASK=2, SA_SIGINFO=4,
 * errno EINTR=4 ETIMEDOUT=110 EAGAIN=11 EINVAL=22 ESRCH=3.
 *
 * MIT -- part of mvlo_nx.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <switch.h>

#include "nx_signals.h"
#include "nx_bfmt.h"
#include "nx_config.h"
#include "util.h"

#define L_EINTR 4
#define L_ESRCH 3
#define L_EAGAIN 11
#define L_EINVAL 22
#define L_ETIMEDOUT 110
#define L_SA_SIGINFO 4
#define L_SA_NODEFER 0x40000000
#define L_SA_RESETHAND 0x80000000
#define L_SIG_BLOCK 0
#define L_SIG_UNBLOCK 1
#define L_SIG_SETMASK 2
#define NSIG_L 65

/* bionic LP64 struct sigaction */
typedef struct {
  int      sa_flags;
  int      _pad;
  void    *sa_handler;      /* or sa_sigaction */
  uint64_t sa_mask;
  void    *sa_restorer;
} l_sigaction;

static l_sigaction g_act[NSIG_L];
static volatile int g_armed;       /* any handler installed for a signal someone kills with */

typedef struct ThreadRec {
  pthread_t id;
  volatile uint64_t pending;
  volatile uint64_t blocked;
  volatile uint64_t pending_since_ns;
  Mutex m;
  CondVar cv;
  /* who is running this thread's handlers: 0 nobody, 1 the thread itself,
   * 2 the forcer / a proxy on its behalf (the thread is paused), 3 the
   * sampling profiler reading its context (nx_prof.c). CAS only. */
  volatile int in_delivery;
  int used;
  const char *name;
  uint64_t tid;              /* kernel thread id: the "tid" column of stall.log */
  Handle handle;             /* for svcSetThreadActivity */
  uint64_t report_ns;        /* rate limit of nx_sig_report_stuck */
  volatile uint32_t forced;  /* deliveries made on its behalf */
  volatile uint32_t force_skips; /* paused, but not in game code: resumed untouched */
} ThreadRec;

#define MAXT 256
static ThreadRec g_thr[MAXT];
static Mutex g_thr_lock;
static __thread ThreadRec *t_self;
static __thread int t_claim;          /* this thread holds some record's in_delivery */
static __thread int t_imp;            /* proxy: pthread_self() answers t_imp_id */
static __thread pthread_t t_imp_id;

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static ThreadRec *find(pthread_t id) {
  for (int i = 0; i < MAXT; i++)
    if (g_thr[i].used && pthread_equal(g_thr[i].id, id)) return &g_thr[i];
  return NULL;
}

void nx_sig_thread_register(const char *name) {
  if (t_self) return;
  mutexLock(&g_thr_lock);
  pthread_t me = pthread_self();
  ThreadRec *r = find(me);
  /* A record already holding our pthread_t belongs to a dead thread that never
   * unregistered (pthread_t values are recycled): its pending/blocked/in_delivery
   * state must not leak into this one, or GC signals may never be delivered. */
  int stale = r != NULL;
  if (!r)
    for (int i = 0; i < MAXT; i++) if (!g_thr[i].used) { r = &g_thr[i]; break; }
  if (r) {
    memset(r, 0, sizeof *r);
    r->id = me;
    r->used = 1;
    r->name = name;
    uint64_t tid = 0;
    if (R_SUCCEEDED(svcGetThreadId(&tid, CUR_THREAD_HANDLE))) r->tid = tid;
    r->handle = threadGetCurHandle();
  }
  mutexUnlock(&g_thr_lock);
  if (stale) debugPrintf("[sig] stale record for pthread %p reset (its previous thread never unregistered)\n", (void *)me);
  t_self = r;
  if (!r) debugPrintf("[sig] WARNING: thread table full -- this thread cannot receive signals\n");
}

void nx_sig_thread_unregister(void) {
  ThreadRec *r = t_self;
  if (!r) return;
  mutexLock(&g_thr_lock);
  r->used = 0;
  mutexUnlock(&g_thr_lock);
  t_self = NULL;
}

/* ---------------------------------------------------------- delivery */
typedef void (*h1_t)(int);
typedef void (*h3_t)(int, void *, void *);

/* Spill x19-x29 and d8-d15 to the stack before entering the handler: on Linux
 * the kernel's signal frame holds them, and bdwgc relies on finding pointers
 * that only live in callee-saved registers of an interrupted frame. */
static __attribute__((noinline)) void call_handler(int sig, const l_sigaction *a) {
  volatile uint64_t spill[20];
  __asm__ volatile(
      "stp x19, x20, [%0, #0]\n\t"  "stp x21, x22, [%0, #16]\n\t"
      "stp x23, x24, [%0, #32]\n\t" "stp x25, x26, [%0, #48]\n\t"
      "stp x27, x28, [%0, #64]\n\t" "str x29, [%0, #80]\n\t"
      "stp d8, d9, [%0, #88]\n\t"   "stp d10, d11, [%0, #104]\n\t"
      "stp d12, d13, [%0, #120]\n\t" "stp d14, d15, [%0, #136]\n\t"
      : : "r"(spill) : "memory");
  if (a->sa_flags & L_SA_SIGINFO) {
    uint8_t info[128];
    memset(info, 0, sizeof info);
    *(int *)info = sig;                    /* si_signo */
    *(int *)(info + 8) = -6;               /* si_code = SI_TKILL */
    ((h3_t)a->sa_handler)(sig, info, NULL);
  } else {
    ((h1_t)a->sa_handler)(sig);
  }
  __asm__ volatile("" : : "r"(spill) : "memory");
}

static void deliver_one(ThreadRec *r, int sig) {
  l_sigaction a = g_act[sig];
  if ((uintptr_t)a.sa_handler <= 1) return;     /* SIG_DFL / SIG_IGN: drop */
  uint64_t saved = r->blocked;
  uint64_t add = a.sa_mask;
  if (!(a.sa_flags & L_SA_NODEFER)) add |= 1ull << (sig - 1);
  r->blocked = saved | add;
  int saved_errno = errno;
  static int ndeliv;
  if (ndeliv++ < 8) debugPrintf("[sig] delivering %d to %s (handler %p)\n", sig, r->name ? r->name : "?", a.sa_handler);
  call_handler(sig, &a);
  errno = saved_errno;
  r->blocked = saved;
  if (a.sa_flags & L_SA_RESETHAND) g_act[sig].sa_handler = NULL;
}

/* Deliver everything pending and unblocked. Returns the number delivered. */
static int deliver_pending(ThreadRec *r) {
  int n = 0;
  for (;;) {
    uint64_t p = r->pending & ~r->blocked;
    if (!p) break;
    int sig = __builtin_ctzll(p) + 1;
    __atomic_and_fetch(&r->pending, ~(1ull << (sig - 1)), __ATOMIC_ACQ_REL);
    if (!r->pending) r->pending_since_ns = 0;
    deliver_one(r, sig);
    n++;
  }
  return n;
}

static int claim(ThreadRec *r, int who) {
  int z = 0;
  return __atomic_compare_exchange_n(&r->in_delivery, &z, who, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
static void unclaim(ThreadRec *r) { __atomic_store_n(&r->in_delivery, 0, __ATOMIC_RELEASE); }

/* The sampling profiler (nx_prof.c) pauses a thread for a context read. It
 * holds the thread's record meanwhile (in_delivery = 3), so the forcer never
 * pauses/resumes the same thread at the same time and no delivery runs on it.
 * 0 = a delivery is in progress: skip this sample. */
int nx_sig_sample_lock(uint32_t h, void **cookie) {
  *cookie = NULL;
  for (int i = 0; i < MAXT; i++) {
    ThreadRec *r = &g_thr[i];
    if (!r->used || r->handle != h) continue;
    if (!claim(r, 3)) return 0;
    *cookie = r;
    return 1;
  }
  return 1;   /* not a signal-registered thread: nothing to coordinate with */
}
void nx_sig_sample_unlock(void *cookie) { if (cookie) unclaim((ThreadRec *)cookie); }

void nx_sig_safepoint(void) {
  if (!g_armed) return;
  ThreadRec *r = t_self;
  if (!r || t_claim) return;
  if (!(r->pending & ~r->blocked)) return;
  if (!claim(r, 1)) return;          /* the forcer is looking at us: next safepoint */
  t_claim = 1;
  deliver_pending(r);
  t_claim = 0;
  unclaim(r);
}

void nx_sig_sleep_ms(int ms) {
  uint64_t end = now_ns() + (uint64_t)ms * 1000000ull;
  for (;;) {
    nx_sig_safepoint();
    uint64_t now = now_ns();
    if (now >= end) return;
    uint64_t left = end - now;
    svcSleepThread(left > 10000000ull ? 10000000ull : left);
  }
}

/* ------------------------------------------------- forced delivery
 *
 * The forcer wakes when a signal is queued for another thread, then checks
 * every 2 ms while something is pending. A record pending for gc_force_ms is
 * claimed (in_delivery 0 -> 2, so the thread cannot start delivering itself),
 * the thread is paused (Mesosphere's SetThreadActivity returns only once the
 * thread is off every core, kern_k_thread.cpp) and its context read
 * (svcGetThreadContext3 requires the pause). If its PC is not in a loaded game
 * module (it is in our shims, libnx, newlib -- possibly holding one of OUR
 * locks, or about to reach a safepoint anyway) it is resumed untouched.
 * Otherwise a proxy thread takes it from there (proxy_run). */
#define NPROXY 8
#define SAVE_BYTES (32 * 8 + 32 * 16)     /* x0-x28, fp, lr, pc + q0-q31 */
#define STACK_NEED (48 * 1024)            /* free stack the handler may use */

typedef struct {
  Thread thr;
  int started;
  Mutex m; CondVar cv;
  ThreadRec *volatile job;
  ThreadContext ctx;
} Proxy;

static int g_force_ms;                    /* 0 = off */
static Thread g_forcer;
static Mutex g_force_m;
static CondVar g_force_cv;
static Proxy g_proxy[NPROXY];
static volatile uint32_t g_forced_total, g_force_skipped, g_force_failed;
static volatile uintptr_t g_allow_lo, g_allow_hi;   /* nx_selftest's spin loop */

void nx_sig_force_allow(uintptr_t lo, uintptr_t hi) { g_allow_lo = lo; g_allow_hi = hi; }

pthread_t nx_sig_pthread_self(void) { return t_imp ? t_imp_id : pthread_self(); }

/* x0 = new sp, x1 = fn, x2 = arg: fn(arg) on another stack, back on ours after. */
void nx_run_on_stack(uintptr_t sp, void (*fn)(void *), void *arg);
__asm__(".text\n.balign 16\n.global nx_run_on_stack\n.type nx_run_on_stack, %function\n"
        "nx_run_on_stack:\n"
        "  stp x29, x30, [sp, #-16]!\n"
        "  mov x29, sp\n"
        "  mov x9, sp\n"
        "  and x0, x0, #~15\n"
        "  sub x0, x0, #16\n"
        "  mov sp, x0\n"
        "  str x9, [sp]\n"
        "  mov x0, x2\n"
        "  blr x1\n"
        "  ldr x9, [sp]\n"
        "  mov sp, x9\n"
        "  ldp x29, x30, [sp], #16\n"
        "  ret\n"
        ".size nx_run_on_stack, . - nx_run_on_stack\n");

static void proxy_body(void *arg) { deliver_pending((ThreadRec *)arg); }

/* On a proxy thread, with r's thread paused and r claimed (2). */
static void proxy_run(Proxy *px, ThreadRec *r) {
  const ThreadContext *c = &px->ctx;
  uintptr_t top = (c->sp & ~(uintptr_t)15) - 128;
  uint64_t *save = (uint64_t *)(top - SAVE_BYTES);
  /* Everything the thread holds in registers, where the collector scans:
   * between our SP and the thread's (a signal frame on Linux). */
  for (int i = 0; i < 29; i++) save[i] = c->cpu_gprs[i].x;
  save[29] = c->fp; save[30] = c->lr; save[31] = c->pc.x;
  memcpy(&save[32], c->fpu_gprs, 32 * 16);
  void *tp = armGetTlsRw();
  if (c->tpidr) armSetTlsRw((void *)c->tpidr);   /* its bionic TLS block */
  ThreadRec *me = t_self;
  t_self = r; t_imp_id = r->id; t_imp = 1; t_claim = 1;
  nx_run_on_stack((uintptr_t)save, proxy_body, r);
  t_claim = 0; t_imp = 0; t_self = me;
  armSetTlsRw(tp);
  __atomic_add_fetch(&r->forced, 1, __ATOMIC_RELAXED);
  __atomic_add_fetch(&g_forced_total, 1, __ATOMIC_RELAXED);
  Result rc = svcSetThreadActivity(r->handle, ThreadActivity_Runnable);
  unclaim(r);
  if (R_FAILED(rc)) debugPrintf("[sig] FAILED to resume %s after a forced delivery (0x%x)\n", r->name ? r->name : "?", rc);
}

static void proxy_entry(void *arg) {
  Proxy *px = arg;
  for (;;) {
    mutexLock(&px->m);
    while (!px->job) condvarWait(&px->cv, &px->m);
    ThreadRec *r = px->job;
    mutexUnlock(&px->m);
    proxy_run(px, r);
    mutexLock(&px->m);
    px->job = NULL;
    mutexUnlock(&px->m);
  }
}

static Proxy *proxy_get(void) {
  for (int i = 0; i < NPROXY; i++) {
    Proxy *px = &g_proxy[i];
    if (!px->started) {
      mutexInit(&px->m); condvarInit(&px->cv);
      if (R_FAILED(threadCreate(&px->thr, proxy_entry, px, NULL, 0x10000, 0x28, -2))) return NULL;
      if (R_FAILED(threadStart(&px->thr))) { threadClose(&px->thr); return NULL; }
      px->started = 1;
    }
    if (!px->job) return px;
  }
  return NULL;
}

static int in_game_code(uintptr_t pc) {
  if (pc >= g_allow_lo && pc < g_allow_hi) return 1;
  return so_find_module_by_addr((const void *)pc) != NULL;
}

static int stack_room(uintptr_t sp) {
  MemoryInfo mi; u32 pi;
  uintptr_t lo = sp - STACK_NEED;
  if (R_FAILED(svcQueryMemory(&mi, &pi, lo))) return 0;
  return (mi.perm & Perm_Rw) == Perm_Rw && mi.addr <= lo && mi.addr + mi.size >= sp;
}

static void force_one(ThreadRec *r) {
  if (!r->handle || !claim(r, 2)) return;
  if (!(r->pending & ~r->blocked)) { unclaim(r); return; }
  Proxy *px = proxy_get();
  if (!px) { unclaim(r); return; }
  Result rc = svcSetThreadActivity(r->handle, ThreadActivity_Paused);
  if (R_FAILED(rc)) {
    if (g_force_failed++ < 4) debugPrintf("[sig] cannot pause %s (0x%x)\n", r->name ? r->name : "?", rc);
    unclaim(r);
    return;
  }
  rc = svcGetThreadContext3(&px->ctx, r->handle);
  int ok = R_SUCCEEDED(rc) && in_game_code(px->ctx.pc.x) && stack_room(px->ctx.sp);
  if (!ok) {
    svcSetThreadActivity(r->handle, ThreadActivity_Runnable);
    unclaim(r);
    if (R_FAILED(rc) && g_force_failed++ < 4) debugPrintf("[sig] cannot read the context of %s (0x%x)\n", r->name ? r->name : "?", rc);
    if (R_SUCCEEDED(rc)) { r->force_skips++; g_force_skipped++; }
    return;
  }
  static unsigned logged;
  if (logged++ < 16)
    debugPrintf("[sig] %s stuck %llu ms in game code (pc %p, sp %p): delivering its signals 0x%llx on its behalf\n",
                r->name ? r->name : "?", (unsigned long long)((now_ns() - r->pending_since_ns) / 1000000ull),
                (void *)px->ctx.pc.x, (void *)px->ctx.sp, (unsigned long long)r->pending);
  mutexLock(&px->m);
  px->job = r;
  condvarWakeOne(&px->cv);
  mutexUnlock(&px->m);
}

static void forcer_entry(void *arg) {
  (void)arg;
  uint64_t lim = (uint64_t)g_force_ms * 1000000ull;
  for (;;) {
    int any = 0;
    uint64_t n = now_ns();
    for (int i = 0; i < MAXT; i++) {
      ThreadRec *r = &g_thr[i];
      if (!r->used || !(r->pending & ~r->blocked)) continue;
      any = 1;
      uint64_t since = r->pending_since_ns;
      if (since && n - since >= lim && !r->in_delivery) force_one(r);
    }
    if (any) { svcSleepThread(2000000ull); continue; }
    mutexLock(&g_force_m);
    condvarWaitTimeout(&g_force_cv, &g_force_m, 1000000000ull);
    mutexUnlock(&g_force_m);
  }
}

static void forcer_wake(void) {
  if (!g_force_ms) return;
  mutexLock(&g_force_m);
  condvarWakeOne(&g_force_cv);
  mutexUnlock(&g_force_m);
}

void nx_sig_force_stats(uint32_t *forced, uint32_t *skipped, uint32_t *failed) {
  if (forced) *forced = g_forced_total;
  if (skipped) *skipped = g_force_skipped;
  if (failed) *failed = g_force_failed;
}

/* ------------------------------------------------------------ imports */
static int sigaction_fake(int sig, const l_sigaction *act, l_sigaction *old) {
  if (sig < 1 || sig >= NSIG_L) { errno = L_EINVAL; return -1; }
  if (old) *old = g_act[sig];
  if (act) {
    g_act[sig] = *act;
    if ((uintptr_t)act->sa_handler > 1) {
      static int logged;
      if (logged++ < 24)
        debugPrintf("[sig] sigaction(%d) handler=%p flags=0x%x -- emulated at safepoints\n",
                    sig, act->sa_handler, act->sa_flags);
    }
  }
  return 0;
}

static void *signal_fake(int sig, void *handler) {
  if (sig < 1 || sig >= NSIG_L) return (void *)-1;
  void *old = g_act[sig].sa_handler;
  memset(&g_act[sig], 0, sizeof g_act[sig]);
  g_act[sig].sa_handler = handler;
  return old;
}

static int pthread_kill_emul(pthread_t t, int sig) {
  if (sig < 0 || sig >= NSIG_L) return L_EINVAL;
  mutexLock(&g_thr_lock);
  ThreadRec *r = find(t);
  mutexUnlock(&g_thr_lock);
  if (!r) {
    static int warned;
    if (sig && warned++ < 8) debugPrintf("[sig] pthread_kill(%p, %d): unknown thread -> ESRCH\n", (void *)t, sig);
    return L_ESRCH;
  }
  if (sig == 0) return 0;
  if ((uintptr_t)g_act[sig].sa_handler <= 1) return 0;   /* no handler: default action ignored */
  g_armed = 1;
  if (!r->pending) r->pending_since_ns = now_ns();
  __atomic_or_fetch(&r->pending, 1ull << (sig - 1), __ATOMIC_ACQ_REL);
  if (r == t_self) { nx_sig_safepoint(); return 0; }
  mutexLock(&r->m);
  condvarWakeAll(&r->cv);
  mutexUnlock(&r->m);
  forcer_wake();
  static unsigned n;
  if (n++ < 4) debugPrintf("[gc] pthread_kill(%s, sig %d) queued for safepoint delivery\n",
                           r->name ? r->name : "thread", sig);
  return 0;
}

static int sigmask_common(int how, const uint64_t *set, uint64_t *old) {
  ThreadRec *r = t_self;
  uint64_t cur = r ? r->blocked : 0;
  if (old) *old = cur;
  if (set && r) {
    if (how == L_SIG_BLOCK) r->blocked = cur | *set;
    else if (how == L_SIG_UNBLOCK) r->blocked = cur & ~*set;
    else if (how == L_SIG_SETMASK) r->blocked = *set;
    else return L_EINVAL;
    nx_sig_safepoint();
  }
  return 0;
}
static int pthread_sigmask_fake2(int how, const uint64_t *set, uint64_t *old) { return sigmask_common(how, set, old); }
static int sigprocmask_fake(int how, const uint64_t *set, uint64_t *old) {
  int e = sigmask_common(how, set, old);
  if (e) { errno = e; return -1; }
  return 0;
}

static volatile int g_parked;            /* threads in sigsuspend: a stop-the-world in progress */
static volatile uint64_t g_parked_since_ns;

static int sigsuspend_fake(const uint64_t *mask) {
  ThreadRec *r = t_self;
  if (!r) { svcSleepThread(1000000ull); errno = L_EINTR; return -1; }
  /* Outside a handler, take the record (wait out a forcer that is checking
   * whether we are in game code: microseconds). Inside one, we already own it. */
  int own = !t_claim;
  if (own) {
    while (!claim(r, 1)) svcSleepThread(1000000ull);
    t_claim = 1;
  }
  uint64_t saved = r->blocked;
  r->blocked = mask ? *mask : 0;
  __atomic_add_fetch(&g_parked, 1, __ATOMIC_RELAXED);
  if (!g_parked_since_ns) g_parked_since_ns = now_ns();
  for (;;) {
    if (r->pending & ~r->blocked) {
      deliver_pending(r);
      break;
    }
    mutexLock(&r->m);
    if (!(r->pending & ~r->blocked)) condvarWaitTimeout(&r->cv, &r->m, 5000000ull);
    mutexUnlock(&r->m);
  }
  if (__atomic_sub_fetch(&g_parked, 1, __ATOMIC_RELAXED) == 0) g_parked_since_ns = 0;
  r->blocked = saved;
  if (own) { t_claim = 0; unclaim(r); }
  errno = L_EINTR;
  return -1;
}

static int raise_fake(int sig) {
  if (!t_self) return 0;
  return pthread_kill_emul(pthread_self(), sig) ? -1 : 0;
}

static int sigemptyset_fake(uint64_t *s) { if (s) *s = 0; return 0; }
static int sigfillset_fake(uint64_t *s) { if (s) *s = ~0ull; return 0; }
static int sigaddset_fake(uint64_t *s, int sig) { if (!s || sig < 1 || sig >= NSIG_L) { errno = L_EINVAL; return -1; } *s |= 1ull << (sig - 1); return 0; }
static int sigdelset_fake(uint64_t *s, int sig) { if (!s || sig < 1 || sig >= NSIG_L) { errno = L_EINVAL; return -1; } *s &= ~(1ull << (sig - 1)); return 0; }
static int sigismember_fake(const uint64_t *s, int sig) { if (!s || sig < 1 || sig >= NSIG_L) { errno = L_EINVAL; return -1; } return (int)((*s >> (sig - 1)) & 1); }
static int sigaltstack_fake(const void *ss, void *old) { (void)ss; if (old) memset(old, 0, 24); return 0; }

/* Sleeps are safepoints too (bdwgc's resend loop usleep()s while it waits). */
static int nanosleep_fake(const struct timespec *req, struct timespec *rem) {
  if (!req) { errno = L_EINVAL; return -1; }
  uint64_t ns = (uint64_t)req->tv_sec * 1000000000ull + (uint64_t)req->tv_nsec;
  uint64_t end = now_ns() + ns;
  for (;;) {
    nx_sig_safepoint();
    uint64_t now = now_ns();
    if (now >= end) break;
    uint64_t left = end - now;
    svcSleepThread(left > 10000000ull ? 10000000ull : left);
  }
  if (rem) { rem->tv_sec = 0; rem->tv_nsec = 0; }
  return 0;
}
static int clock_nanosleep_fake(int clk, int flags, const struct timespec *req, struct timespec *rem) {
  (void)clk;
  if (flags & 1 /* TIMER_ABSTIME */) {
    struct timespec now;
    clock_gettime(clk == 0 ? CLOCK_REALTIME : CLOCK_MONOTONIC, &now);
    int64_t ns = (int64_t)(req->tv_sec - now.tv_sec) * 1000000000ll + (req->tv_nsec - now.tv_nsec);
    if (ns <= 0) return 0;
    struct timespec rel = { ns / 1000000000ll, ns % 1000000000ll };
    nanosleep_fake(&rel, NULL);
    return 0;
  }
  nanosleep_fake(req, rem);
  return 0;
}
static int usleep_fake(unsigned us) { struct timespec t = { us / 1000000u, (long)(us % 1000000u) * 1000 }; return nanosleep_fake(&t, NULL); }
static unsigned sleep_fake(unsigned s) { struct timespec t = { s, 0 }; nanosleep_fake(&t, NULL); return 0; }
static int sched_yield_safe(void) { nx_sig_safepoint(); svcSleepThread(0); return 0; }

/* ------------------------------------------------- semaphores (bionic) */
/* bionic sem_t is 16 bytes on LP64; we store a pointer to our own object.
 * Own implementation (mutex+condvar+count) instead of libnx Semaphore so we
 * get real timed waits and a safepoint between slices. */
typedef struct { Mutex m; CondVar cv; volatile int count; } NxSem;

static int sem_init_fake2(void **s, int pshared, unsigned value) {
  (void)pshared;
  NxSem *x = calloc(1, sizeof *x);
  if (!x) { errno = 12; return -1; }
  mutexInit(&x->m); condvarInit(&x->cv);
  x->count = (int)value;
  *s = x;
  return 0;
}
static NxSem *sem_get(void **s) {
  if (!s) return NULL;
  if (!*s) sem_init_fake2(s, 0, 0);    /* zero-initialised static sem_t */
  return (NxSem *)*s;
}
static int sem_destroy_fake2(void **s) { if (s && *s) { free(*s); *s = NULL; } return 0; }
static int sem_post_fake2(void **s) {
  NxSem *x = sem_get(s);
  if (!x) { errno = L_EINVAL; return -1; }
  mutexLock(&x->m);
  x->count++;
  condvarWakeOne(&x->cv);
  mutexUnlock(&x->m);
  return 0;
}
static int sem_trywait_fake2(void **s) {
  NxSem *x = sem_get(s);
  if (!x) { errno = L_EINVAL; return -1; }
  mutexLock(&x->m);
  int ok = x->count > 0;
  if (ok) x->count--;
  mutexUnlock(&x->m);
  if (!ok) { errno = L_EAGAIN; return -1; }
  return 0;
}
/* deadline_ns: 0 = forever (CLOCK_MONOTONIC based) */
static int sem_wait_until(void **s, uint64_t deadline_ns) {
  NxSem *x = sem_get(s);
  if (!x) { errno = L_EINVAL; return -1; }
  for (;;) {
    mutexLock(&x->m);
    if (x->count > 0) { x->count--; mutexUnlock(&x->m); return 0; }
    uint64_t slice = 10000000ull;
    if (deadline_ns) {
      uint64_t n = now_ns();
      if (n >= deadline_ns) { mutexUnlock(&x->m); errno = L_ETIMEDOUT; return -1; }
      if (deadline_ns - n < slice) slice = deadline_ns - n;
    }
    condvarWaitTimeout(&x->cv, &x->m, slice);
    int got = x->count > 0;
    if (got) x->count--;
    mutexUnlock(&x->m);
    if (got) return 0;
    nx_sig_safepoint();
  }
}
static int sem_wait_fake2(void **s) { nx_sig_safepoint(); return sem_wait_until(s, 0); }
static int sem_timedwait_fake2(void **s, const struct timespec *abs) {
  if (!abs) return sem_wait_fake2(s);
  struct timespec rt;
  clock_gettime(CLOCK_REALTIME, &rt);
  int64_t rel = (int64_t)(abs->tv_sec - rt.tv_sec) * 1000000000ll + (abs->tv_nsec - rt.tv_nsec);
  if (abs->tv_nsec < 0 || abs->tv_nsec >= 1000000000l) {
    /* bdwgc adds 10 ms to tv_nsec without normalising; tolerate it like glibc would not,
     * but never fail with EINVAL on a value it built itself. */
    rel = (int64_t)(abs->tv_sec - rt.tv_sec) * 1000000000ll + (abs->tv_nsec - rt.tv_nsec);
  }
  if (rel < 0) rel = 0;
  nx_sig_safepoint();
  return sem_wait_until(s, now_ns() + (uint64_t)rel);
}
static int sem_getvalue_fake2(void **s, int *v) {
  NxSem *x = sem_get(s);
  if (v) *v = x ? x->count : 0;
  return 0;
}

/* ------------------------------------------------------------ watchdog */
/* Heartbeat (nx_netbeat.c): no lock, no allocation. " | gc: idle, 23 threads"
 * or " | gc: 7 parked for 1234ms, 1 not acked (main 1230ms), forced 3". */
int nx_sig_beacon(char *buf, unsigned len) {
  uint64_t n = now_ns();
  int threads = 0, waiting = 0;
  uint64_t oldest = 0;
  const char *who = "?";
  for (int i = 0; i < MAXT; i++) {
    ThreadRec *r = &g_thr[i];
    if (!r->used) continue;
    threads++;
    uint64_t since = r->pending_since_ns;
    if (r->pending && since && !r->in_delivery) {
      waiting++;
      if (!oldest || since < oldest) { oldest = since; who = r->name ? r->name : "?"; }
    }
  }
  int parked = g_parked;
  uint64_t ps = g_parked_since_ns;
  int o;
  if (!parked && !waiting)
    o = nx_bfmt(buf, len, " | gc: idle, %d threads, forced %u", threads, g_forced_total);
  else
    o = nx_bfmt(buf, len, " | gc: %d parked for %llums, %d not acked (%s %llums), %d threads, forced %u", parked,
                ps ? (unsigned long long)((n - ps) / 1000000ull) : 0ull, waiting, waiting ? who : "-",
                oldest ? (unsigned long long)((n - oldest) / 1000000ull) : 0ull, threads, g_forced_total);
  return o;
}

void nx_sig_report_stuck(void) {
  if (!g_armed) return;
  uint64_t n = now_ns();
  for (int i = 0; i < MAXT; i++) {
    ThreadRec *r = &g_thr[i];
    if (!r->used || !r->pending || !r->pending_since_ns) continue;
    uint64_t age = n - r->pending_since_ns;
    if (age > 2000000000ull && n - r->report_ns > 2000000000ull) {
      stallPrintf("[sig] thread %s (%p) has had signals 0x%llx pending for %llu ms without reaching a "
                  "safepoint -- a GC collection may be waiting on it\n",
                  r->name ? r->name : "?", (void *)r->id, (unsigned long long)r->pending,
                  (unsigned long long)(age / 1000000ull));
      r->report_ns = n;
    }
  }
}

/* Called by the watchdog on a stall: the render loop that normally calls
 * nx_sig_report_stuck() is the one that is stuck when a GC waits for an ack. */
void nx_sig_dump(void) {
  if (!g_armed) return;
  uint64_t n = now_ns();
  stallPrintf("[sig] --- emulated signal state (pending/blocked: bit sig-1; 30=SIGPWR suspend, 24=SIGXCPU restart) ---\n");
  for (int i = 0; i < MAXT; i++) {
    ThreadRec *r = &g_thr[i];
    if (!r->used) continue;
    uint64_t p = r->pending, b = r->blocked, since = r->pending_since_ns;
    if (!p && !b && !r->in_delivery && !r->forced) continue;
    stallPrintf("[sig] tid %-8llu %-8s pthread %p pending 0x%llx blocked 0x%llx in_delivery %d forced %u skipped %u\n",
                (unsigned long long)r->tid, r->name ? r->name : "?", (void *)r->id,
                (unsigned long long)p, (unsigned long long)b, r->in_delivery, r->forced, r->force_skips);
    if (p)
      stallPrintf("[sig]     pending for %llu ms%s\n",
                  since ? (unsigned long long)((n - since) / 1000000ull) : 0ull,
                  (p & ~b) ? "" : "  (ALL BLOCKED: can never be delivered)");
  }
}

DynLibFunction nx_signal_exports[] = {
  { "sigaction", (uintptr_t)&sigaction_fake },
  { "signal", (uintptr_t)&signal_fake },
  { "bsd_signal", (uintptr_t)&signal_fake },
  { "pthread_kill", (uintptr_t)&pthread_kill_emul },
  { "pthread_sigmask", (uintptr_t)&pthread_sigmask_fake2 },
  { "sigprocmask", (uintptr_t)&sigprocmask_fake },
  { "sigsuspend", (uintptr_t)&sigsuspend_fake },
  { "raise", (uintptr_t)&raise_fake },
  { "sigemptyset", (uintptr_t)&sigemptyset_fake },
  { "sigfillset", (uintptr_t)&sigfillset_fake },
  { "sigaddset", (uintptr_t)&sigaddset_fake },
  { "sigdelset", (uintptr_t)&sigdelset_fake },
  { "sigismember", (uintptr_t)&sigismember_fake },
  { "sigaltstack", (uintptr_t)&sigaltstack_fake },
  { "nanosleep", (uintptr_t)&nanosleep_fake },
  { "clock_nanosleep", (uintptr_t)&clock_nanosleep_fake },
  { "usleep", (uintptr_t)&usleep_fake },
  { "sleep", (uintptr_t)&sleep_fake },
  { "sched_yield", (uintptr_t)&sched_yield_safe },
  { "sem_init", (uintptr_t)&sem_init_fake2 },
  { "sem_destroy", (uintptr_t)&sem_destroy_fake2 },
  { "sem_post", (uintptr_t)&sem_post_fake2 },
  { "sem_wait", (uintptr_t)&sem_wait_fake2 },
  { "sem_trywait", (uintptr_t)&sem_trywait_fake2 },
  { "sem_timedwait", (uintptr_t)&sem_timedwait_fake2 },
  { "sem_getvalue", (uintptr_t)&sem_getvalue_fake2 },
};
const int nx_signal_exports_num = sizeof(nx_signal_exports) / sizeof(nx_signal_exports[0]);

void nx_sig_init(void) {
  mutexInit(&g_thr_lock);
  nx_sig_thread_register("main");
  g_force_ms = nxcfg_int("debug.gc_force_ms", 30);
  if (g_force_ms < 0) g_force_ms = 0;
  if (g_force_ms) {
    mutexInit(&g_force_m); condvarInit(&g_force_cv);
    if (R_FAILED(threadCreate(&g_forcer, forcer_entry, NULL, NULL, 0x4000, 0x28, -2)) ||
        R_FAILED(threadStart(&g_forcer))) {
      debugPrintf("[sig] cannot start the forcer thread -- gc_force_ms off\n");
      g_force_ms = 0;
    }
  }
  debugPrintf("[sig] threads stuck in game code get their GC signals on their behalf after %d ms%s\n",
              g_force_ms, g_force_ms ? "" : " (off: [debug] gc_force_ms = 0)");
}
