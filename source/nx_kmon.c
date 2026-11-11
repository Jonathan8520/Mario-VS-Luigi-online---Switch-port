/* nx_kmon.c -- what this process asks the kernel and the GPU service, for the
 * UDP heartbeat: memory-map system calls and nv ioctls, counted, with the one
 * in flight (and for how long) and the slowest / last one.
 *
 * WHY
 * The whole-console freeze stops the heartbeat with the last datagram
 * looking healthy. If the process is inside a memory-map system call (OC
 * window, memory pool, thread stacks, code patches) or an nv ioctl when it
 * happens, the datagrams that do get out say so; if the counters keep
 * growing right up to the freeze, that is a lead too.
 *
 * Every call reaches us through ld --wrap (Makefile), libnx's own internal
 * callers included (virtmem, thread stacks, the nv ioctl helpers). The
 * wrappers only touch plain counters: no lock, no allocation, no log.
 *
 * MIT -- part of mvlo_nx. */
#include <stdint.h>
#include <switch.h>

#include "nx_bfmt.h"

static inline uint64_t tick_ns(void) { return armTicksToNs(armGetSystemTick()); }

/* ---- memory-map system calls ---------------------------------------------- */
enum { K_HEAP, K_PERM, K_MAP, K_UNMAP, K_PHYS, K_UNPHYS, K_PPERM, K_PMAP, K_PUNMAP, K_PCODE, K_PUNCODE, K_N };
static const char *const k_name[K_N] = {
  "SetHeapSize", "SetMemoryPermission", "MapMemory", "UnmapMemory", "MapPhysicalMemory",
  "UnmapPhysicalMemory", "SetProcessMemoryPermission", "MapProcessMemory", "UnmapProcessMemory",
  "MapProcessCodeMemory", "UnmapProcessCodeMemory",
};
static volatile uint32_t k_calls, k_failed, k_inflight;
static volatile uint64_t k_since_ns, k_max_ns;
static volatile int k_what = -1, k_last = -1, k_max_what = -1;

static uint64_t k_begin(int what) {
  uint64_t t = tick_ns();
  if (__atomic_add_fetch(&k_inflight, 1, __ATOMIC_RELAXED) == 1) k_since_ns = t;
  k_what = what;
  return t;
}
static Result k_end(int what, uint64_t t0, Result rc) {
  uint64_t dt = tick_ns() - t0;
  if (dt > k_max_ns) { k_max_ns = dt; k_max_what = what; }
  k_last = what;
  __atomic_add_fetch(&k_calls, 1, __ATOMIC_RELAXED);
  if (R_FAILED(rc)) __atomic_add_fetch(&k_failed, 1, __ATOMIC_RELAXED);
  if (__atomic_sub_fetch(&k_inflight, 1, __ATOMIC_RELAXED) == 0) k_since_ns = 0;
  return rc;
}
#define KWRAP(id, call) do { uint64_t t0 = k_begin(id); Result rc = call; return k_end(id, t0, rc); } while (0)

Result __real_svcSetHeapSize(void **out, u64 size);
Result __real_svcSetMemoryPermission(void *addr, u64 size, u32 perm);
Result __real_svcMapMemory(void *dst, void *src, u64 size);
Result __real_svcUnmapMemory(void *dst, void *src, u64 size);
Result __real_svcMapPhysicalMemory(void *addr, u64 size);
Result __real_svcUnmapPhysicalMemory(void *addr, u64 size);
Result __real_svcSetProcessMemoryPermission(Handle proc, u64 addr, u64 size, u32 perm);
Result __real_svcMapProcessMemory(void *dst, Handle proc, u64 src, u64 size);
Result __real_svcUnmapProcessMemory(void *dst, Handle proc, u64 src, u64 size);
Result __real_svcMapProcessCodeMemory(Handle proc, u64 dst, u64 src, u64 size);
Result __real_svcUnmapProcessCodeMemory(Handle proc, u64 dst, u64 src, u64 size);

Result __wrap_svcSetHeapSize(void **out, u64 size) { KWRAP(K_HEAP, __real_svcSetHeapSize(out, size)); }
Result __wrap_svcSetMemoryPermission(void *addr, u64 size, u32 perm) { KWRAP(K_PERM, __real_svcSetMemoryPermission(addr, size, perm)); }
Result __wrap_svcMapMemory(void *dst, void *src, u64 size) { KWRAP(K_MAP, __real_svcMapMemory(dst, src, size)); }
Result __wrap_svcUnmapMemory(void *dst, void *src, u64 size) { KWRAP(K_UNMAP, __real_svcUnmapMemory(dst, src, size)); }
Result __wrap_svcMapPhysicalMemory(void *addr, u64 size) { KWRAP(K_PHYS, __real_svcMapPhysicalMemory(addr, size)); }
Result __wrap_svcUnmapPhysicalMemory(void *addr, u64 size) { KWRAP(K_UNPHYS, __real_svcUnmapPhysicalMemory(addr, size)); }
Result __wrap_svcSetProcessMemoryPermission(Handle proc, u64 addr, u64 size, u32 perm) { KWRAP(K_PPERM, __real_svcSetProcessMemoryPermission(proc, addr, size, perm)); }
Result __wrap_svcMapProcessMemory(void *dst, Handle proc, u64 src, u64 size) { KWRAP(K_PMAP, __real_svcMapProcessMemory(dst, proc, src, size)); }
Result __wrap_svcUnmapProcessMemory(void *dst, Handle proc, u64 src, u64 size) { KWRAP(K_PUNMAP, __real_svcUnmapProcessMemory(dst, proc, src, size)); }
Result __wrap_svcMapProcessCodeMemory(Handle proc, u64 dst, u64 src, u64 size) { KWRAP(K_PCODE, __real_svcMapProcessCodeMemory(proc, dst, src, size)); }
Result __wrap_svcUnmapProcessCodeMemory(Handle proc, u64 dst, u64 src, u64 size) { KWRAP(K_PUNCODE, __real_svcUnmapProcessCodeMemory(proc, dst, src, size)); }

/* ---- nv ioctls ------------------------------------------------------------ */
static volatile uint32_t n_calls, n_failed, n_timeouts, n_inflight;
static volatile uint32_t n_req, n_last_req, n_max_req, n_fail_req, n_fail_rc;
static volatile uint64_t n_since_ns, n_last_ns, n_max_ns;

static uint64_t n_begin(u32 req) {
  uint64_t t = tick_ns();
  if (__atomic_add_fetch(&n_inflight, 1, __ATOMIC_RELAXED) == 1) n_since_ns = t;
  n_req = req;
  return t;
}
static Result n_end(u32 req, uint64_t t0, Result rc) {
  uint64_t dt = tick_ns() - t0;
  n_last_req = req; n_last_ns = dt;
  if (dt > n_max_ns) { n_max_ns = dt; n_max_req = req; }
  __atomic_add_fetch(&n_calls, 1, __ATOMIC_RELAXED);
  /* Timeout is the normal answer of a fence / event wait that is not due yet. */
  if (rc == MAKERESULT(Module_LibnxNvidia, LibnxNvidiaError_Timeout)) __atomic_add_fetch(&n_timeouts, 1, __ATOMIC_RELAXED);
  else if (R_FAILED(rc)) { __atomic_add_fetch(&n_failed, 1, __ATOMIC_RELAXED); n_fail_req = req; n_fail_rc = rc; }
  if (__atomic_sub_fetch(&n_inflight, 1, __ATOMIC_RELAXED) == 0) n_since_ns = 0;
  return rc;
}
#define NWRAP(req, call) do { uint64_t t0 = n_begin(req); Result rc = call; return n_end(req, t0, rc); } while (0)

Result __real_nvIoctl(u32 fd, u32 request, void *argp);
Result __real_nvIoctl2(u32 fd, u32 request, void *argp, const void *inbuf, size_t inbuf_size);
Result __real_nvIoctl3(u32 fd, u32 request, void *argp, void *outbuf, size_t outbuf_size);
Result __wrap_nvIoctl(u32 fd, u32 request, void *argp) { NWRAP(request, __real_nvIoctl(fd, request, argp)); }
Result __wrap_nvIoctl2(u32 fd, u32 request, void *argp, const void *inbuf, size_t inbuf_size) {
  NWRAP(request, __real_nvIoctl2(fd, request, argp, inbuf, inbuf_size));
}
Result __wrap_nvIoctl3(u32 fd, u32 request, void *argp, void *outbuf, size_t outbuf_size) {
  NWRAP(request, __real_nvIoctl3(fd, request, argp, outbuf, outbuf_size));
}

/* ---- beacon --------------------------------------------------------------- */
static const char *kname(int i) { return i >= 0 && i < K_N ? k_name[i] : "-"; }

/* " | kmem: 1234 calls (0 failed), max 3ms MapPhysicalMemory[, IN FLIGHT Xms Y]
 *   | nv: 5678 ioctls (0 failed), last 0xc0104701 120us, max 4ms 0x...[, IN FLIGHT ...]" */
/* The process's resource limit (Mesosphere kern_svc_info.cpp: GetInfo
 * InfoType_ResourceLimit with an invalid handle adds a handle to it). Taken
 * once; its current values say whether threads, events, transfer memories or
 * sessions pile up until the freeze. */
static Handle rl_handle(void) {
  static volatile Handle h;
  static volatile int state;            /* 0 not tried, 1 trying, 2 done */
  int z = 0;
  if (__atomic_compare_exchange_n(&state, &z, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    u64 v = 0;
    if (R_SUCCEEDED(svcGetInfo(&v, InfoType_ResourceLimit, INVALID_HANDLE, 0))) h = (Handle)v;
    __atomic_store_n(&state, 2, __ATOMIC_RELEASE);
  }
  return __atomic_load_n(&state, __ATOMIC_ACQUIRE) == 2 ? h : 0;
}

/* " | rl: thr 37/96 ev 160/700 tmem 4/200 sess 120/933 mem 3100/3200M" */
int nx_kmon_rl_beacon(char *buf, unsigned len) {
  Handle h = rl_handle();
  if (!h) return nx_bfmt(buf, len, " | rl: none");
  static const LimitableResource r[] = { LimitableResource_Threads, LimitableResource_Events,
                                         LimitableResource_TransferMemories, LimitableResource_Sessions };
  static const char *const nm[] = { "thr", "ev", "tmem", "sess" };
  int o = nx_bfmt(buf, len, " | rl:");
  for (int i = 0; i < 4 && o > 0 && (unsigned)o < len; i++) {
    s64 cur = -1, lim = -1;
    svcGetResourceLimitCurrentValue(&cur, h, r[i]);
    svcGetResourceLimitLimitValue(&lim, h, r[i]);
    o += nx_bfmt(buf + o, len - o, " %s %lld/%lld", nm[i], (long long)cur, (long long)lim);
  }
  s64 mc = -1, ml = -1;
  svcGetResourceLimitCurrentValue(&mc, h, LimitableResource_Memory);
  svcGetResourceLimitLimitValue(&ml, h, LimitableResource_Memory);
  if (o > 0 && (unsigned)o < len)
    o += nx_bfmt(buf + o, len - o, " mem %lld/%lldM", (long long)(mc >> 20), (long long)(ml >> 20));
  return o;
}

int nx_kmon_beacon(char *buf, unsigned len) {
  uint64_t now = tick_ns();
  int o = nx_bfmt(buf, len, " | kmem: %u calls (%u failed), max %llums %s", k_calls, k_failed,
                  (unsigned long long)(k_max_ns / 1000000ull), kname(k_max_what));
  uint64_t ks = k_since_ns;
  if (k_inflight && ks && o > 0 && (unsigned)o < len)
    o += nx_bfmt(buf + o, len - o, ", IN FLIGHT %llums %s", (unsigned long long)((now - ks) / 1000000ull), kname(k_what));
  if (o > 0 && (unsigned)o < len)
    o += nx_bfmt(buf + o, len - o, " | nv: %u ioctls (%u failed, %u timeouts), last 0x%x %lluus, max %llums 0x%x", n_calls, n_failed, n_timeouts,
                 n_last_req, (unsigned long long)(n_last_ns / 1000ull), (unsigned long long)(n_max_ns / 1000000ull),
                 n_max_req);
  if (n_failed && o > 0 && (unsigned)o < len)
    o += nx_bfmt(buf + o, len - o, ", last failed 0x%x rc 0x%x", n_fail_req, n_fail_rc);
  uint64_t ns = n_since_ns;
  if (n_inflight && ns && o > 0 && (unsigned)o < len)
    o += nx_bfmt(buf + o, len - o, ", IN FLIGHT %llums 0x%x", (unsigned long long)((now - ns) / 1000000ull), n_req);
  return o;
}
