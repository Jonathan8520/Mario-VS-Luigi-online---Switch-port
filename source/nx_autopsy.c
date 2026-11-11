/* nx_autopsy.c -- what each thread of this process waits on when the game
 * freezes, written where it can still be read.
 *
 * WHY
 * The freeze seen ~7 min after every launch (STATUS, 29/10) stops the render
 * loop, the watchdog (stall.log, SD card) and the UDP heartbeat within a second
 * of each other and HOME stops answering, yet the process lives: the stall
 * rumble (nx_alive.c) keeps vibrating. The watchdog cannot report it because
 * it is one of the stopped threads. So the thread known to keep running does:
 *
 *  1. A journal of the blocking system calls, one slot per thread, filled by
 *     ld --wrap (Makefile): IPC requests with their session handle, lock waits
 *     with the owner's handle, handle / condition / address waits, since when
 *     and from which caller. The kernel does not give back x0 for a thread
 *     parked in a system call (svcGetThreadContext3 only returns x19-x30, sp
 *     and pc then), so the session a thread waits on is recorded on the way in.
 *  2. At the freeze, from the rumble thread: every thread's journal entry and
 *     frame-pointer backtrace (diag.c), the service sessions of this process,
 *     the beacons, the last heartbeats, and probes -- one small call per
 *     system service, each in its own thread created in advance -- showing
 *     which services still answer (SD card, sockets, time, network manager,
 *     applet manager, GPU, temperature). libnx shares 3 sessions between all
 *     threads for fs and for bsd, so a probe can also be stuck behind our own
 *     threads; the journal tells the two cases apart.
 *  3. The report goes to <root>/autopsy.log through a file opened in advance
 *     (fsFileWrite, no newlib) and to the [debug] netlog PC over UDP. The
 *     controller then gives one long vibration if the SD write went through,
 *     three short ones if it did not.
 *
 * Once armed nothing here allocates or takes a lock; text goes through
 * nx_bfmt. [debug] autopsy = 0 turns the report off (the journal stays: a few
 * stores per system call).
 *
 * MIT -- part of mvlo_nx. */
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <switch.h>
#include <switch/services/bsd.h>   /* not pulled in by switch.h */

#include "nx_autopsy.h"
#include "nx_bfmt.h"
#include "nx_config.h"
#include "nx_signals.h"
#include "cloverpit_root.h"
#include "version.h"
#include "util.h"

extern int diag_netbeat_line(char *buf, unsigned len);
extern int diag_frame_no(void);
extern unsigned diag_autopsy_threads(char *buf, unsigned len);
extern unsigned diag_bfmt_context(Handle h, char *buf, unsigned len);
extern const char *diag_name_by_handle(Handle h);
extern int diag_bfmt_addr(char *buf, unsigned n, uint64_t addr);
extern void nx_net_beacon(char *buf, unsigned len);
extern int nx_kmon_beacon(char *buf, unsigned len);
extern int nx_kmon_rl_beacon(char *buf, unsigned len);
extern int nx_bsdmon_beacon(char *buf, unsigned len);
extern unsigned nx_bsdmon_report(char *buf, unsigned len, int last_ops);
extern unsigned nx_net_photon_report(char *buf, unsigned len, int window_ms);
extern int nx_netbeat_dst(struct sockaddr_in *out);

static inline uint64_t tick(void) { return armGetSystemTick(); }
static inline uint64_t tick_ms(uint64_t t) { return armTicksToNs(t) / 1000000ull; }

/* Append with nx_bfmt; `o` may run past `len`, clamp() fixes the count. */
#define ADD(...) do { if (o < len) o += (unsigned)nx_bfmt(buf + o, len - o, __VA_ARGS__); } while (0)
static unsigned clamp(unsigned o, unsigned len) { return o < len ? o : (len ? len - 1 : 0); }

/* ---- system-call journal --------------------------------------------------- */
enum { J_NONE, J_IPC, J_IPCBUF, J_WAITSYNC, J_LOCK, J_CONDVAR, J_ADDR, J_N };
static const char *const j_name[J_N] = {
  "-", "IPC", "IPC (user buffer)", "WaitSynchronization", "ArbitrateLock", "WaitProcessWideKey", "WaitForAddress",
};
#define J_SLOTS 512   /* threads are never removed; Photon makes a few per connection */
typedef struct {
  volatile uint32_t handle;      /* owner thread, 0 = free */
  volatile uint32_t what;
  volatile uint32_t arg;         /* session / lock owner / first waited handle */
  volatile int32_t  count;
  volatile uint64_t addr;
  volatile uint64_t timeout;
  volatile uint64_t caller;
  volatile uint64_t since;       /* tick | 1 while inside the call, 0 otherwise */
  volatile uint64_t calls;
  const char *volatile name;
} JSlot;
static JSlot g_j[J_SLOTS];
static volatile int g_j_full;

static inline uint32_t j_hash(uint32_t h) { return (h ^ (h >> 15)) & (J_SLOTS - 1); }

static JSlot *j_find(uint32_t h) {
  uint32_t i = j_hash(h);
  for (int n = 0; n < J_SLOTS; n++, i = (i + 1) & (J_SLOTS - 1)) {
    uint32_t cur = g_j[i].handle;
    if (cur == h) return &g_j[i];
    if (cur == 0) return NULL;
  }
  return NULL;
}

static JSlot *j_self(void) {
  uint32_t h = threadGetCurHandle();
  if (!h) return NULL;
  uint32_t i = j_hash(h);
  for (int n = 0; n < J_SLOTS; n++, i = (i + 1) & (J_SLOTS - 1)) {
    uint32_t cur = g_j[i].handle;
    if (cur == h) return &g_j[i];
    if (cur == 0) {
      uint32_t expect = 0;
      if (__atomic_compare_exchange_n(&g_j[i].handle, &expect, h, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) ||
          expect == h)
        return &g_j[i];
    }
  }
  g_j_full = 1;
  return NULL;
}

static inline JSlot *j_enter(uint32_t what, uint32_t arg, int32_t count, uint64_t addr, uint64_t timeout,
                             void *caller) {
  JSlot *s = j_self();
  if (!s) return NULL;
  s->what = what; s->arg = arg; s->count = count; s->addr = addr; s->timeout = timeout;
  s->caller = (uint64_t)(uintptr_t)caller;
  __atomic_store_n(&s->since, tick() | 1, __ATOMIC_RELEASE);
  return s;
}
static inline void j_exit(JSlot *s) {
  if (!s) return;
  __atomic_store_n(&s->since, 0, __ATOMIC_RELEASE);
  s->calls++;
}

Result __real_svcSendSyncRequest(Handle session);
Result __real_svcSendSyncRequestWithUserBuffer(void *usrBuffer, u64 size, Handle session);
Result __real_svcWaitSynchronization(s32 *index, const Handle *handles, s32 handleCount, u64 timeout);
Result __real_svcArbitrateLock(u32 wait_tag, u32 *tag_location, u32 self_tag);
Result __real_svcWaitProcessWideKeyAtomic(u32 *key, u32 *tag_location, u32 self_tag, u64 timeout);
Result __real_svcWaitForAddress(void *address, u32 arb_type, s64 value, s64 timeout);

#define JWRAP(what, arg, count, addr, timeout, call) do { \
    JSlot *s_ = j_enter(what, arg, count, addr, timeout, __builtin_return_address(0)); \
    Result rc_ = call; j_exit(s_); return rc_; } while (0)

/* ---- IPC count per session ---------------------------------------------------
 * The ~400 s online freeze takes the network of the whole console down and
 * lets go when our process exits (02/11), yet our bsd traffic is steady and a
 * soak with the same traffic does not freeze: another service we talk to while
 * connected may be the link. Every request is counted per session handle,
 * with the last caller (names the clones of fs/bsd sessions and services the
 * autopsy has no getter for); the heartbeat shows the busiest every 10 s. */
#define IPC_SLOTS 256
static struct { volatile uint32_t h; volatile uint64_t n, caller; uint64_t last; } g_ipc[IPC_SLOTS];

static inline void ipc_count(uint32_t h, void *caller) {
  uint32_t i = (h ^ (h >> 15)) & (IPC_SLOTS - 1);
  for (int k = 0; k < IPC_SLOTS; k++, i = (i + 1) & (IPC_SLOTS - 1)) {
    uint32_t cur = g_ipc[i].h;
    if (cur == 0) {
      uint32_t expect = 0;
      if (!__atomic_compare_exchange_n(&g_ipc[i].h, &expect, h, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) &&
          expect != h)
        continue;
      cur = h;
    }
    if (cur == h) {
      __atomic_add_fetch(&g_ipc[i].n, 1, __ATOMIC_RELAXED);
      g_ipc[i].caller = (uint64_t)(uintptr_t)caller;
      return;
    }
  }
}

Result __wrap_svcSendSyncRequest(Handle session) {
  ipc_count(session, __builtin_return_address(0));
  JWRAP(J_IPC, session, 0, 0, 0, __real_svcSendSyncRequest(session));
}
Result __wrap_svcSendSyncRequestWithUserBuffer(void *usrBuffer, u64 size, Handle session) {
  ipc_count(session, __builtin_return_address(0));
  JWRAP(J_IPCBUF, session, 0, 0, 0, __real_svcSendSyncRequestWithUserBuffer(usrBuffer, size, session));
}
Result __wrap_svcWaitSynchronization(s32 *index, const Handle *handles, s32 handleCount, u64 timeout) {
  JWRAP(J_WAITSYNC, handleCount > 0 && handles ? handles[0] : 0, handleCount, 0, timeout,
        __real_svcWaitSynchronization(index, handles, handleCount, timeout));
}
Result __wrap_svcArbitrateLock(u32 wait_tag, u32 *tag_location, u32 self_tag) {
  JWRAP(J_LOCK, wait_tag, 0, (uint64_t)(uintptr_t)tag_location, 0,
        __real_svcArbitrateLock(wait_tag, tag_location, self_tag));
}
Result __wrap_svcWaitProcessWideKeyAtomic(u32 *key, u32 *tag_location, u32 self_tag, u64 timeout) {
  JWRAP(J_CONDVAR, 0, 0, (uint64_t)(uintptr_t)key, timeout,
        __real_svcWaitProcessWideKeyAtomic(key, tag_location, self_tag, timeout));
}
Result __wrap_svcWaitForAddress(void *address, u32 arb_type, s64 value, s64 timeout) {
  JWRAP(J_ADDR, arb_type, 0, (uint64_t)(uintptr_t)address, (uint64_t)timeout,
        __real_svcWaitForAddress(address, arb_type, value, timeout));
}

void nx_autopsy_name_self(const char *name) {
  JSlot *s = j_self();
  if (s) s->name = name;
}

/* ---- names ----------------------------------------------------------------- */
#define MAX_SESS 40
static uint32_t    g_sess_h[MAX_SESS];
static const char *g_sess_n[MAX_SESS];
static int         g_nsess;

static void sess_add(Service *s, const char *name) {
  if (s && serviceIsActive(s) && g_nsess < MAX_SESS) { g_sess_h[g_nsess] = s->session; g_sess_n[g_nsess++] = name; }
}
/* Getters only return the address of libnx's static Service: no IPC. fs and
 * bsd send most requests on clones of these sessions (libnx SessionMgr), which
 * cannot be named here: the caller address in the report names those. */
static void sess_refresh(void) {
  g_nsess = 0;
  sess_add(fsGetServiceSession(), "fs");
  sess_add(bsdGetServiceSession(), "bsd");
  sess_add(nvGetServiceSession(), "nvdrv");
  sess_add(hidGetServiceSession(), "hid");
  sess_add(timeGetServiceSession(), "time");
  sess_add(tsGetServiceSession(), "ts");
  sess_add(nifmGetServiceSession_StaticService(), "nifm");
  sess_add(nifmGetServiceSession_GeneralService(), "nifm-general");
  sess_add(audoutGetServiceSession(), "audout");
  sess_add(audoutGetServiceSession_AudioOut(), "audout-out");
  sess_add(audrenGetServiceSession_AudioRenderer(), "audren");
  sess_add(appletGetServiceSession_Proxy(), "am-proxy");
  sess_add(appletGetServiceSession_CommonStateGetter(), "am-commonstate");
  sess_add(appletGetServiceSession_SelfController(), "am-self");
  sess_add(appletGetServiceSession_WindowController(), "am-window");
  sess_add(appletGetServiceSession_AudioController(), "am-audio");
  sess_add(appletGetServiceSession_DisplayController(), "am-display");
  sess_add(appletGetServiceSession_Functions(), "am-functions");
  sess_add(viGetSession_IApplicationDisplayService(), "vi-appdisplay");
  sess_add(viGetSession_IHOSBinderDriverRelay(), "vi-binder-relay");
  sess_add(viGetSession_IHOSBinderDriverIndirect(), "vi-binder-indirect");
  sess_add(viGetSession_ISystemDisplayService(), "vi-sysdisplay");
  sess_add(viGetSession_IManagerDisplayService(), "vi-mgrdisplay");
}
static const char *sess_name(uint32_t h) {
  for (int i = 0; i < g_nsess; i++) if (g_sess_h[i] == h) return g_sess_n[i];
  return "?";
}

/* " | ipc/10s: N in S sessions: <name> <count> ..." -- the 6 busiest sessions
 * since the previous call (the heartbeat calls it every 10 s). A session with
 * no name shows its handle and last caller (resolve with the build's .elf). */
int nx_autopsy_ipc_beacon(char *buf, unsigned len) {
  enum { TOP = 6 };
  int idx[TOP];
  uint64_t d[TOP], total = 0;
  int nt = 0;
  unsigned live = 0, o = 0;
  sess_refresh();
  for (int i = 0; i < IPC_SLOTS; i++) {
    if (!g_ipc[i].h) continue;
    live++;
    uint64_t n = __atomic_load_n(&g_ipc[i].n, __ATOMIC_RELAXED), dl = n - g_ipc[i].last;
    g_ipc[i].last = n;
    total += dl;
    if (!dl) continue;
    if (nt < TOP) { idx[nt] = i; d[nt] = dl; nt++; }
    else if (dl > d[TOP - 1]) { idx[TOP - 1] = i; d[TOP - 1] = dl; }
    else continue;
    for (int k = nt - 1; k > 0 && d[k] > d[k - 1]; k--) {
      uint64_t td = d[k]; d[k] = d[k - 1]; d[k - 1] = td;
      int ti = idx[k]; idx[k] = idx[k - 1]; idx[k - 1] = ti;
    }
  }
  ADD(" | ipc/10s: %llu in %u sessions:", (unsigned long long)total, live);
  for (int k = 0; k < nt; k++) {
    uint32_t h = g_ipc[idx[k]].h;
    const char *nm = sess_name(h);
    if (nm[0] != '?') { ADD(" %s %llu", nm, (unsigned long long)d[k]); continue; }
    char c[48];
    diag_bfmt_addr(c, sizeof c, g_ipc[idx[k]].caller);
    ADD(" 0x%x(%s) %llu", h, c, (unsigned long long)d[k]);
  }
  return (int)clamp(o, len);
}
static const char *thread_name(uint32_t h) {
  const char *n = diag_name_by_handle(h);
  if (n) return n;
  JSlot *s = j_find(h);
  return s && s->name ? s->name : "?";
}

static unsigned bfmt_timeout(char *buf, unsigned len, uint64_t t) {
  unsigned o = 0;
  if (t == UINT64_MAX || (int64_t)t < 0) ADD("none");
  else ADD("%llu ms", (unsigned long long)(t / 1000000ull));
  return clamp(o, len);
}

int nx_autopsy_journal_bfmt(Handle h, char *buf, unsigned len, uint64_t now) {
  unsigned o = 0;
  JSlot *s = j_find(h);
  if (!s) return 0;
  uint64_t since = __atomic_load_n(&s->since, __ATOMIC_ACQUIRE);
  if (!since) {
    ADD(" | no system call in progress (%llu so far)", (unsigned long long)s->calls);
    return (int)clamp(o, len);
  }
  uint32_t what = s->what < J_N ? s->what : J_NONE;
  uint64_t ms = now > since ? tick_ms(now - since) : 0;
  char t[32];
  ADD(" | in %s", j_name[what]);
  switch (what) {
    case J_IPC: case J_IPCBUF: ADD(" session 0x%x (%s)", s->arg, sess_name(s->arg)); break;
    case J_LOCK:     ADD(" held by 0x%x (%s), lock 0x%llx", s->arg, thread_name(s->arg), (unsigned long long)s->addr); break;
    case J_WAITSYNC: bfmt_timeout(t, sizeof t, s->timeout);
                     ADD(" %d handle(s), first 0x%x (%s), timeout %s", s->count, s->arg, sess_name(s->arg), t); break;
    case J_CONDVAR:  bfmt_timeout(t, sizeof t, s->timeout); ADD(" key 0x%llx, timeout %s", (unsigned long long)s->addr, t); break;
    case J_ADDR:     bfmt_timeout(t, sizeof t, s->timeout); ADD(" address 0x%llx, timeout %s", (unsigned long long)s->addr, t); break;
    default: break;
  }
  char c[64];
  diag_bfmt_addr(c, sizeof c, s->caller);
  ADD(" for %llu.%llu s, caller %s", (unsigned long long)(ms / 1000), (unsigned long long)((ms % 1000) / 100), c);
  return (int)clamp(o, len);
}

/* ---- heartbeat ring -------------------------------------------------------- */
#define BEATS 12
static char g_beat[BEATS][1400];
static volatile uint32_t g_beat_n;

void nx_autopsy_note_beat(const char *line, unsigned len) {
  uint32_t i = g_beat_n % BEATS;
  if (len >= sizeof g_beat[0]) len = sizeof g_beat[0] - 1;
  memcpy(g_beat[i], line, len);
  g_beat[i][len] = 0;
  __atomic_add_fetch(&g_beat_n, 1, __ATOMIC_RELEASE);
}

/* ---- probes ---------------------------------------------------------------- */
typedef struct Probe Probe;
struct Probe {
  const char *name;
  Result (*fn)(Probe *);
  volatile int      ready;
  volatile uint32_t go, done;    /* generation fired / finished */
  volatile uint64_t t0, t1;
  volatile Result   rc;
  Thread thr;
  uint8_t tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
};

static FsFile    g_file;
static int       g_file_ok;
static int64_t   g_file_end;
static int       g_sock = -1;
static struct sockaddr_in g_dst;
static TsSession g_ts;
static int       g_ts_ok;
static u32       g_nvfd;
static int       g_nv_ok;

static char      g_mark[160];
static unsigned  g_mark_len;
static int64_t   g_mark_off;
static char      g_rep[96 * 1024];
static unsigned  g_rep_len;
static int64_t   g_rep_off;

static Result p_sd(Probe *p)    { (void)p; return fsFileWrite(&g_file, g_mark_off, g_mark, g_mark_len, FsWriteOption_Flush); }
static Result p_udp(Probe *p)   { (void)p; return sendto(g_sock, g_mark, g_mark_len, 0, (struct sockaddr *)&g_dst, sizeof g_dst) < 0 ? 1 : 0; }
static Result p_time(Probe *p)  { (void)p; u64 t; return timeGetCurrentTime(TimeType_UserSystemClock, &t); }
static Result p_nifm(Probe *p)  {
  (void)p; NifmInternetConnectionType ty; u32 s; NifmInternetConnectionStatus st;
  return nifmGetInternetConnectionStatus(&ty, &s, &st);
}
static Result p_am(Probe *p)    { (void)p; u8 f = 0; return serviceDispatchOut(appletGetServiceSession_CommonStateGetter(), 9, f); }
static Result p_nv(Probe *p)    { (void)p; u32 sz; return nvioctlNvhostCtrlGpu_ZCullGetCtxSize(g_nvfd, &sz); }
static Result p_ts(Probe *p)    { (void)p; float c; return tsSessionGetTemperature(&g_ts, &c); }
static Result p_sddump(Probe *p) { (void)p; return fsFileWrite(&g_file, g_rep_off, g_rep, g_rep_len, FsWriteOption_Flush); }
static Result p_udpdump(Probe *p) {
  (void)p;
  /* whole lines, ~1.2 KB per datagram, each tagged for grep on the PC */
  char d[1300];
  unsigned i = 0, part = 0;
  Result rc = 0;
  while (i < g_rep_len) {
    unsigned n = 0, last_nl = 0;
    while (i + n < g_rep_len && n < 1200) { if (g_rep[i + n] == '\n') last_nl = n + 1; n++; }
    if (i + n < g_rep_len && last_nl) n = last_nl;
    unsigned h = (unsigned)nx_bfmt(d, sizeof d, "[autopsy %u]\n", part++);
    if (h >= sizeof d) h = sizeof d - 1;
    memcpy(d + h, g_rep + i, n);
    if (sendto(g_sock, d, h + n, 0, (struct sockaddr *)&g_dst, sizeof g_dst) < 0) rc = 1;
    i += n;
    svcSleepThread(3000000ull);
  }
  return rc;
}

enum { P_SD, P_UDP, P_TIME, P_NIFM, P_AM, P_NV, P_TS, P_QUICK, P_SDDUMP = P_QUICK, P_UDPDUMP, P_N };
static Probe g_p[P_N] = {
  [P_SD]      = { "probe sd-card write", p_sd },
  [P_UDP]     = { "probe udp send", p_udp },
  [P_TIME]    = { "probe time", p_time },
  [P_NIFM]    = { "probe nifm", p_nifm },
  [P_AM]      = { "probe am", p_am },
  [P_NV]      = { "probe nvdrv", p_nv },
  [P_TS]      = { "probe ts", p_ts },
  [P_SDDUMP]  = { "autopsy sd writer", p_sddump },
  [P_UDPDUMP] = { "autopsy udp writer", p_udpdump },
};

static void probe_main(void *arg) {
  Probe *p = arg;
  install_bionic_tls(p->tls);
  nx_autopsy_name_self(p->name);
  for (;;) {
    uint32_t g = p->go;
    if (g == p->done) { svcSleepThread(100000000ull); continue; }
    p->t1 = 0;
    p->t0 = tick();
    Result rc = p->fn(p);
    p->rc = rc;
    p->t1 = tick();
    __atomic_store_n(&p->done, g, __ATOMIC_RELEASE);
  }
}
static int fire(Probe *p) {
  if (!p->ready || p->go != p->done) return 0;   /* off, or still stuck in the last call */
  p->t0 = 0; p->t1 = 0;                           /* the thread is idle: safe to reset */
  __atomic_add_fetch(&p->go, 1, __ATOMIC_RELEASE);
  return 1;
}
static int finished(Probe *p) { return p->ready && p->go == __atomic_load_n(&p->done, __ATOMIC_ACQUIRE) && p->t1; }

/* ---- arm ------------------------------------------------------------------- */
static int g_armed, g_episode;

void nx_autopsy_arm(void) {
  if (g_armed || nxcfg_int("debug.autopsy", 1) <= 0) return;
  g_armed = 1;

  /* report file, opened now: at the freeze, newlib and fsdev may be stuck */
  FsFileSystem *sd = fsdevGetDeviceFileSystem("sdmc");
  char path[FS_MAX_PATH];
  const char *root = cp_game_root();
  if (!strncmp(root, "sdmc:", 5)) root += 5;
  nx_bfmt(path, sizeof path, "%s/autopsy.log", root);
  if (sd) {
    fsFsCreateFile(sd, path, 0, 0);   /* fails harmlessly if it exists */
    if (R_SUCCEEDED(fsFsOpenFile(sd, path, FsOpenMode_Write | FsOpenMode_Append, &g_file))) {
      s64 size = 0;
      fsFileGetSize(&g_file, &size);
      if (size > 512 * 1024) { fsFileSetSize(&g_file, 0); size = 0; }
      g_file_end = size;
      g_file_ok = 1;
    }
  }
  if (nx_netbeat_dst(&g_dst)) g_sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (R_SUCCEEDED(tsInitialize()) && R_SUCCEEDED(tsOpenSession(&g_ts, TsDeviceCode_LocationExternal))) g_ts_ok = 1;
  if (R_SUCCEEDED(nvOpen(&g_nvfd, "/dev/nvhost-ctrl-gpu"))) g_nv_ok = 1;

  int want[P_N] = {
    [P_SD] = g_file_ok, [P_UDP] = g_sock >= 0, [P_TIME] = serviceIsActive(timeGetServiceSession()),
    [P_NIFM] = serviceIsActive(nifmGetServiceSession_GeneralService()),
    [P_AM] = serviceIsActive(appletGetServiceSession_CommonStateGetter()), [P_NV] = g_nv_ok, [P_TS] = g_ts_ok,
    [P_SDDUMP] = g_file_ok, [P_UDPDUMP] = g_sock >= 0,
  };
  int started = 0;
  for (int i = 0; i < P_N; i++) {
    if (!want[i]) continue;
    if (R_SUCCEEDED(threadCreate(&g_p[i].thr, probe_main, &g_p[i], NULL, 0x4000, 0x2C, -2)) &&
        R_SUCCEEDED(threadStart(&g_p[i].thr))) {
      g_p[i].ready = 1;
      started++;
    }
  }

  /* a first line now, while everything works: proves the file path */
  if (g_file_ok) {
    unsigned n = (unsigned)nx_bfmt(g_mark, sizeof g_mark, "--- mvlo_nx %s: autopsy armed at frame %d, %d probe threads ---\n",
                                   MVLO_NX_VERSION, diag_frame_no(), started);
    if (n >= sizeof g_mark) n = sizeof g_mark - 1;
    if (R_SUCCEEDED(fsFileWrite(&g_file, g_file_end, g_mark, n, FsWriteOption_Flush))) g_file_end += n;
  }
  debugPrintf("[wd] autopsy armed: report file %s, udp %s, %d probe threads (ts %d, nv %d)\n",
              g_file_ok ? path : "UNAVAILABLE", g_sock >= 0 ? "yes" : "no", started, g_ts_ok, g_nv_ok);
}

/* ---- report ---------------------------------------------------------------- */
static unsigned probes_bfmt(char *buf, unsigned len, uint64_t now) {
  unsigned o = 0;
  for (int i = 0; i < P_QUICK; i++) {
    Probe *p = &g_p[i];
    ADD("  %-22s ", p->name);
    if (!p->ready) ADD("not available\n");
    else if (finished(p)) ADD("answered in %llu ms, rc 0x%x\n", (unsigned long long)tick_ms(p->t1 - p->t0), p->rc);
    else if (p->t0) ADD("NO ANSWER after %llu ms\n", (unsigned long long)tick_ms(now - p->t0));
    else ADD("NOT STARTED (thread not scheduled)\n");
  }
  return clamp(o, len);
}

static unsigned journal_others(char *buf, unsigned len, uint64_t now) {
  unsigned o = 0;
  for (int i = 0; i < J_SLOTS; i++) {
    JSlot *s = &g_j[i];
    uint32_t h = s->handle;
    if (!h || diag_name_by_handle(h)) continue;
    if (!s->name && !__atomic_load_n(&s->since, __ATOMIC_ACQUIRE)) continue;   /* idle or gone */
    ADD("  %-24s h 0x%-6x", s->name ? s->name : "(unnamed thread)", h);
    if (o < len) o += (unsigned)nx_autopsy_journal_bfmt(h, buf + o, len - o, now);
    ADD("\n");
    if (o < len) o += diag_bfmt_context(h, buf + o, len - o);
  }
  return clamp(o, len);
}

int nx_autopsy_run(uint64_t stalled_ns) {
  if (!g_armed || nxcfg_int("debug.autopsy", 1) <= 0) return -1;
  g_episode++;
  char *buf = g_rep;
  unsigned len = sizeof g_rep, o = 0;
  uint64_t t_fire = tick();

  /* 1. quick probes, in parallel; the SD / UDP ones write a marker */
  g_mark_len = (unsigned)nx_bfmt(g_mark, sizeof g_mark, "[autopsy] #%d started, frame %d, stuck %llu s\n", g_episode,
                                 diag_frame_no(), (unsigned long long)(stalled_ns / 1000000000ull));
  if (g_mark_len >= sizeof g_mark) g_mark_len = sizeof g_mark - 1;
  if (g_p[P_SD].ready && g_p[P_SD].go == g_p[P_SD].done) { g_mark_off = g_file_end; g_file_end += g_mark_len; }
  for (int i = 0; i < P_QUICK; i++) fire(&g_p[i]);

  /* 2. threads, while the probes run */
  sess_refresh();
  uint64_t up = tick_ms(t_fire);
  ADD("\n===== mvlo_nx %s AUTOPSY #%d: no frame for %llu.%llu s, last frame %d, uptime %llu.%03llu s =====\n",
      MVLO_NX_VERSION, g_episode, (unsigned long long)(stalled_ns / 1000000000ull),
      (unsigned long long)((stalled_ns / 100000000ull) % 10), diag_frame_no(), (unsigned long long)(up / 1000),
      (unsigned long long)(up % 1000));
  ADD("threads (diag registry; journal of the blocking system call; context and frame-pointer backtrace):\n");
  if (o < len) o += diag_autopsy_threads(buf + o, len - o);
  ADD("other threads seen by the journal%s:\n", g_j_full ? " (JOURNAL FULL: some threads untracked)" : "");
  if (o < len) o += journal_others(buf + o, len - o, tick());

  /* 3. probe results, 3 s after firing */
  while (tick_ms(tick() - t_fire) < 3000) svcSleepThread(100000000ull);
  ADD("probes, fired %llu ms ago (one call per service, each in its own thread):\n",
      (unsigned long long)tick_ms(tick() - t_fire));
  if (o < len) o += probes_bfmt(buf + o, len - o, tick());
  ADD("sessions:");
  for (int i = 0; i < g_nsess; i++) ADD(" %s=0x%x", g_sess_n[i], g_sess_h[i]);
  ADD("\nIPC requests per session since boot (handle, name or last caller, count):");
  for (int i = 0; i < IPC_SLOTS; i++) {
    if (!g_ipc[i].h) continue;
    const char *nm = sess_name(g_ipc[i].h);
    char c[48];
    if (nm[0] == '?') diag_bfmt_addr(c, sizeof c, g_ipc[i].caller);
    ADD(" 0x%x(%s) %llu", g_ipc[i].h, nm[0] == '?' ? c : nm, (unsigned long long)g_ipc[i].n);
  }
  ADD("\nbeacons:");
  if (o < len) o += clamp((unsigned)diag_netbeat_line(buf + o, len - o), len - o);
  if (o < len) { nx_net_beacon(buf + o, len - o); while (o < len - 1 && buf[o]) o++; }
  if (o < len) o += clamp((unsigned)nx_sig_beacon(buf + o, len - o), len - o);
  if (o < len) o += clamp((unsigned)nx_kmon_beacon(buf + o, len - o), len - o);
  if (o < len) o += clamp((unsigned)nx_kmon_rl_beacon(buf + o, len - o), len - o);
  if (o < len) o += clamp((unsigned)nx_bsdmon_beacon(buf + o, len - o), len - o);
  ADD("\n");
  if (o < len) o += nx_bsdmon_report(buf + o, len - o, 128);
  if (o < len) o += nx_net_photon_report(buf + o, len - o, 30000);
  ADD("\nlast heartbeats:\n");
  uint32_t nb = __atomic_load_n(&g_beat_n, __ATOMIC_ACQUIRE);
  for (uint32_t k = nb > BEATS ? nb - BEATS : 0; k < nb; k++) ADD("  %s\n", g_beat[k % BEATS]);
  ADD("===== end of autopsy #%d =====\n", g_episode);
  g_rep_len = clamp(o, len);

  /* 4. the report itself: SD (pre-opened file) and UDP, each in its own thread */
  int sd_fired = 0;
  if (g_p[P_SDDUMP].ready && g_p[P_SDDUMP].go == g_p[P_SDDUMP].done) {
    g_rep_off = g_file_end; g_file_end += g_rep_len;
    sd_fired = fire(&g_p[P_SDDUMP]);
  }
  fire(&g_p[P_UDPDUMP]);
  uint64_t t_dump = tick();
  while (sd_fired && !finished(&g_p[P_SDDUMP]) && tick_ms(tick() - t_dump) < 4000) svcSleepThread(100000000ull);
  return sd_fired && finished(&g_p[P_SDDUMP]) && R_SUCCEEDED(g_p[P_SDDUMP].rc);
}
