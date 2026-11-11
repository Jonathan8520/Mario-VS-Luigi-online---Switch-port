/* libc_shim.c -- bionic-compatible libc wrappers for libcrx.so + libc++_shared
 *
 * The Android engine and its C++ runtime are linked against bionic. Where the
 * bionic and newlib ABIs differ (struct layouts, flag values, missing
 * functions) we provide converting wrappers here; everything that matches is
 * passed straight through from imports.c. Online/IPC functionality (sockets,
 * fork/exec, dlopen of system libs) is dead on Switch and stubbed to fail
 * cleanly so the engine falls back to offline behaviour.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <malloc.h>
#include <wchar.h>
#include <wctype.h>
#include <time.h>
#include <sys/stat.h>
#include <switch.h>
#include <EGL/egl.h>     /* eglGetProcAddress: resolve the full GLES API for dlsym */

#include "nx_bfmt.h"
#include "config.h"
#include "cloverpit_root.h"
#include "util.h"
#include "nx_config.h"
#include "nx_io.h"
#include "nx_splash.h"
#include "error.h"
#include "imports.h"
#include "nx_signals.h"
#include "nx_net.h"
#include "nx_bridge.h"   /* dynlib_find_export (dlsym shim lookup) */
#include "nx_statcache.h"
#include "so_util.h"
#include "libc_shim.h"
#include "android_native_unity.h"
#include "diag.h"

// ---------------------------------------------------------------------------
// fortify (_chk) wrappers: ignore the object-size argument
// ---------------------------------------------------------------------------

void *__memcpy_chk_fake(void *dst, const void *src, size_t n, size_t dstlen) { (void)dstlen; return memcpy(dst, src, n); }
void *__memmove_chk_fake(void *dst, const void *src, size_t n, size_t dstlen) { (void)dstlen; return memmove(dst, src, n); }
void *__memset_chk_fake(void *dst, int c, size_t n, size_t dstlen) { (void)dstlen; return memset(dst, c, n); }
char *__strcat_chk_fake(char *dst, const char *src, size_t dstlen) { (void)dstlen; return strcat(dst, src); }
char *__strchr_chk_fake(const char *s, int c, size_t slen) { (void)slen; return strchr(s, c); }
char *__strcpy_chk_fake(char *dst, const char *src, size_t dstlen) { (void)dstlen; return strcpy(dst, src); }
size_t __strlen_chk_fake(const char *s, size_t slen) { (void)slen; return strlen(s); }
char *__strncat_chk_fake(char *dst, const char *src, size_t n, size_t dstlen) { (void)dstlen; return strncat(dst, src, n); }
char *__strncpy_chk_fake(char *dst, const char *src, size_t n, size_t dstlen) { (void)dstlen; return strncpy(dst, src, n); }
char *__strncpy_chk2_fake(char *dst, const char *src, size_t n, size_t dstlen, size_t srclen) { (void)dstlen; (void)srclen; return strncpy(dst, src, n); }
char *__strrchr_chk_fake(const char *s, int c, size_t slen) { (void)slen; return strrchr(s, c); }
int __vsnprintf_chk_fake(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, va_list va) { (void)flag; (void)slen; return vsnprintf(s, maxlen, fmt, va); }
int __vsprintf_chk_fake(char *s, int flag, size_t slen, const char *fmt, va_list va) { (void)flag; (void)slen; return vsprintf(s, fmt, va); }

int __snprintf_chk_fake(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, ...) {
  (void)flag; (void)slen;
  va_list va; va_start(va, fmt);
  int r = vsnprintf(s, maxlen, fmt, va);
  va_end(va);
  return r;
}
int __sprintf_chk_fake(char *s, int flag, size_t slen, const char *fmt, ...) {
  (void)flag; (void)slen;
  va_list va; va_start(va, fmt);
  int r = vsprintf(s, fmt, va);
  va_end(va);
  return r;
}

// fortified read helpers ignore the buffer-size guard
int   __open_2_fake(const char *path, int flags) { return open_fake(path, flags); }
/* Both go through the same paths as read()/pread(): a file in the block cache
 * (nx_io.c) has a virtual position, so a raw newlib read() would read from the
 * wrong offset. */
long  read_fake(int fd, void *buf, size_t count);

/* ---- phantom fds ([io] phantom_fds, default 1) -----------------------------
 * A read-only open of a data file the preload keeps in memory (nx_io.c RAM
 * files) gets a descriptor of our own, 3072..4095, with no FS request at all:
 * the open and close round trips were what was left of the first Room
 * Settings after the preload (~250 opens, ~0.55 s; 02/11). read, lseek,
 * pread, fstat (the per-fd stat cache), mmap, dup and close all know them. */
#define PH_BASE 3072
#define PH_N    1024
typedef struct { const uint8_t *data; int64_t size, pos; int refs; } Phantom;
static Phantom g_ph[PH_N];
static Mutex g_ph_lock;
static Phantom *ph_get(int fd) {
  return (fd >= PH_BASE && fd < PH_BASE + PH_N && g_ph[fd - PH_BASE].refs > 0) ? &g_ph[fd - PH_BASE] : NULL;
}
static int ph_open(const uint8_t *data, int64_t size) {
  mutexLock(&g_ph_lock);
  for (int i = 0; i < PH_N; i++)
    if (g_ph[i].refs == 0) {
      g_ph[i].data = data; g_ph[i].size = size; g_ph[i].pos = 0; g_ph[i].refs = 1;
      mutexUnlock(&g_ph_lock);
      return PH_BASE + i;
    }
  mutexUnlock(&g_ph_lock);
  return -1;
}
static long ph_pread(Phantom *p, void *buf, size_t n, int64_t off) {
  if (off < 0) { errno = EINVAL; return -1; }
  if (off >= p->size || !n) return 0;
  if ((int64_t)n > p->size - off) n = (size_t)(p->size - off);
  memcpy(buf, p->data + off, n);
  return (long)n;
}
/* imports.c (pread) and unity_imports.c (dup) */
int  nx_phantom_is(int fd) { return ph_get(fd) != NULL; }
long nx_phantom_pread(int fd, void *buf, size_t n, long off) {
  Phantom *p = ph_get(fd);
  return p ? ph_pread(p, buf, n, off) : -1;
}
int nx_phantom_dup(int fd) {
  mutexLock(&g_ph_lock);
  Phantom *p = ph_get(fd);
  if (p) p->refs++;
  mutexUnlock(&g_ph_lock);
  return p ? fd : -1;
}
/* 1 = it was a phantom (its last reference may be gone now). */
static int ph_close(int fd) {
  mutexLock(&g_ph_lock);
  Phantom *p = ph_get(fd);
  if (p && --p->refs == 0) p->data = NULL;
  mutexUnlock(&g_ph_lock);
  return p != NULL;
}

long  __read_chk_fake(int fd, void *buf, size_t count, size_t buflen) { (void)buflen; return read_fake(fd, buf, count); }
long  __pread_chk_fake(int fd, void *buf, size_t count, long off, size_t buflen) {
  (void)buflen;
  { Phantom *p = ph_get(fd); if (p) return ph_pread(p, buf, count, off); }
  if (nxio_owns(fd)) return nxio_pread(fd, buf, count, off);
  long cur = lseek(fd, 0, SEEK_CUR);
  if (cur < 0 || lseek(fd, off, SEEK_SET) < 0) return -1;
  long r = read(fd, buf, count);
  lseek(fd, cur, SEEK_SET);
  return r;
}
void  __FD_SET_chk_fake(int fd, void *set, size_t setlen) { (void)setlen; if (set && fd >= 0 && fd < 1024) ((unsigned long *)set)[fd / (8 * sizeof(long))] |= (1ul << (fd % (8 * sizeof(long)))); }
int   __FD_ISSET_chk_fake(int fd, const void *set, size_t setlen) { (void)setlen; if (set && fd >= 0 && fd < 1024) return (((const unsigned long *)set)[fd / (8 * sizeof(long))] >> (fd % (8 * sizeof(long)))) & 1; return 0; }

// ---------------------------------------------------------------------------
// misc bionic functions
// ---------------------------------------------------------------------------

// Native twin of the android.os.Build.* JNI fields. libunity calls this to
// detect API level / ABI / device; returning "" (the old stub) made it
// mis-detect the platform. Hand back Switch-sane values for the keys engines
// actually query; everything else stays empty (= property unset, the normal
// Android case). Return value is the value length, per bionic contract.
int __system_property_get_fake(const char *name, char *value) {
  if (!value) return 0;
  const char *v = "";
  if (name) {
    if      (!strcmp(name, "ro.build.version.sdk"))        v = "33";
    else if (!strcmp(name, "ro.build.version.release"))    v = "13";
    else if (!strcmp(name, "ro.build.version.codename"))   v = "REL";
    else if (!strcmp(name, "ro.product.cpu.abi"))          v = "arm64-v8a";
    else if (!strcmp(name, "ro.product.cpu.abilist"))      v = "arm64-v8a";
    else if (!strcmp(name, "ro.product.cpu.abilist64"))    v = "arm64-v8a";
    else if (!strcmp(name, "ro.product.cpu.abi2"))         v = "";
    else if (!strcmp(name, "ro.product.model"))            v = "Switch";
    else if (!strcmp(name, "ro.product.manufacturer"))     v = "Nintendo";
    else if (!strcmp(name, "ro.product.brand"))            v = "Nintendo";
    else if (!strcmp(name, "ro.product.name"))             v = "Switch";
    else if (!strcmp(name, "ro.product.device"))           v = "Switch";
    else if (!strcmp(name, "ro.product.board"))            v = "nx";
    else if (!strcmp(name, "ro.hardware"))                 v = "nx";
    else if (!strcmp(name, "ro.board.platform"))           v = "nx";
    else if (!strcmp(name, "ro.build.fingerprint"))        v = "Nintendo/Switch/Switch:13/REL/10007:user/release-keys";
    else if (!strcmp(name, "ro.build.characteristics"))    v = "default";
    else if (!strcmp(name, "ro.build.type"))               v = "user";
    else if (!strcmp(name, "ro.build.tags"))               v = "release-keys";
    else if (!strcmp(name, "ro.debuggable"))               v = "0";
    else if (!strcmp(name, "ro.secure"))                   v = "1";
    else if (!strcmp(name, "ro.kernel.qemu"))              v = "0";
    else if (!strcmp(name, "ro.opengles.version"))         v = "196610"; /* GLES 3.2 */
    else if (!strcmp(name, "dalvik.vm.heapsize"))          v = "512m";
    else if (!strcmp(name, "persist.sys.timezone"))        v = "UTC";
  }
  size_t n = strlen(v);
  if (n > 91) n = 91;            /* PROP_VALUE_MAX-1 */
  memcpy(value, v, n); value[n] = '\0';
  return (int)n;
}
unsigned long getauxval_fake(unsigned long type) { (void)type; return 0; }

int gettid_fake(void) {
  u64 tid = 1;
  if (R_SUCCEEDED(svcGetThreadId(&tid, CUR_THREAD_HANDLE)) && tid)
    return (int)(tid & 0x7fffffff);
  return 1;
}

#define ARM64_SYS_GETTID            178
#define ARM64_SYS_FUTEX             98
#define ARM64_SYS_SCHED_SETAFFINITY 122
#define ARM64_SYS_PROCESS_VM_READV  270
#define ARM64_SYS_PROCESS_VM_WRITEV 271

// futex(2) emulation over libnx mutex+condvar. The il2cpp runtime synchronizes
// its GC, thread pool and locks with raw futex; returning ENOSYS made every
// waiter spin forever (the syscall(98) -> ENOSYS flood) and threading never made
// progress. Wait queues are hashed by uaddr into a bucket array; FUTEX_WAKE wakes
// the whole bucket (waiters re-check *uaddr, so over-broad wakes are harmless).
// The bucket mutex serializes compare-and-sleep against wakers so no wake is lost.
// Infinite waits are capped at 16ms and return as if woken: under load a wake can
// be missed (the Unity Job System / GC otherwise deadlock), and a bounded re-poll
// recovers it safely since the waiter re-checks *uaddr before proceeding.
#define FUTEX_WAIT        0
#define FUTEX_WAKE        1
#define FUTEX_WAIT_BITSET 9
#define FUTEX_WAKE_BITSET 10
#define FUTEX_CMD_MASK    0x7f  // strip FUTEX_PRIVATE_FLAG(128)/CLOCK_REALTIME(256)
#define FUTEX_BUCKETS     256

static Mutex   futex_lock[FUTEX_BUCKETS];   // libnx Mutex/CondVar are u32; 0 == ready
static CondVar futex_cond[FUTEX_BUCKETS];

static long futex_impl(volatile int32_t *uaddr, int op, int val, const struct timespec *to) {
  const int cmd = op & FUTEX_CMD_MASK;
  const unsigned h = (unsigned)(((uintptr_t)uaddr >> 4) & (FUTEX_BUCKETS - 1));
  if (cmd == FUTEX_WAIT || cmd == FUTEX_WAIT_BITSET) {
    long ret = 0;
    nx_sig_safepoint();   /* GC signal delivery point (nx_signals.c) */
    nx_splash_tick();     /* loading screen: Unity's GL thread idles here during frame 1 */
    { extern unsigned long g_perf_futex_waits; g_perf_futex_waits++; }
    mutexLock(&futex_lock[h]);
    if (*uaddr != val) {
      errno = 11 /* Linux EAGAIN */; ret = -1;
    } else if (to) {
      /* Timed waits are capped at 16 ms too (a capped wait returns as a
       * spurious wake-up, which callers handle by re-checking the word and
       * their own deadline): a thread parked for a whole second could not be
       * stopped by the GC in the meantime. */
      u64 ns = (u64)to->tv_sec * 1000000000ULL + (u64)to->tv_nsec;
      const int capped = ns > 16000000ULL;
      if (capped) ns = 16000000ULL;
      if (R_FAILED(condvarWaitTimeout(&futex_cond[h], &futex_lock[h], ns)) && !capped) {
        errno = 110 /* Linux ETIMEDOUT */; ret = -1;
      }
    } else {
      diag_futex_spin(uaddr); // beacon: alive-but-spinning if value never flips
      condvarWaitTimeout(&futex_cond[h], &futex_lock[h], 16000000ULL); // capped infinite wait
    }
    mutexUnlock(&futex_lock[h]);
    return ret;
  }
  if (cmd == FUTEX_WAKE || cmd == FUTEX_WAKE_BITSET) {
    mutexLock(&futex_lock[h]);
    condvarWakeAll(&futex_cond[h]);
    mutexUnlock(&futex_lock[h]);
    return val > 0 ? val : 0; // approximate count woken
  }
  errno = ENOSYS;
  return -1;
}

/* newlib has no <sys/uio.h>; the kernel iovec layout is just {ptr, len}. */
struct nx_iovec { void *iov_base; size_t iov_len; };

/* Validate that [addr, addr+len) is mapped and readable via svcQueryMemory, so a
 * self process_vm_readv can copy safely instead of risking a fault. */
static int nx_addr_readable(uintptr_t addr, size_t len) {
  uintptr_t a = addr, end = addr + len;
  while (a < end) {
    MemoryInfo mi; u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, a))) return 0;
    if (mi.type == 0) return 0;                 /* MemType_Unmapped */
    if ((mi.perm & Perm_R) == 0) return 0;      /* not readable */
    uintptr_t be = (uintptr_t)mi.addr + mi.size;
    if (be <= a) return 0;
    a = be;
  }
  return 1;
}

long syscall_fake(long number, ...) {
  switch (number) {
    case ARM64_SYS_GETTID: return gettid_fake();
    case ARM64_SYS_FUTEX: {
      va_list va; va_start(va, number);
      volatile int32_t *uaddr = va_arg(va, volatile int32_t *);
      const int op  = va_arg(va, int);
      const int val = va_arg(va, int);
      const struct timespec *to = va_arg(va, const struct timespec *);
      va_end(va);
      return futex_impl(uaddr, op, val, to);
    }
    case ARM64_SYS_SCHED_SETAFFINITY:
      return 0; // affinity hints are advisory; pretend success
    case ARM64_SYS_PROCESS_VM_READV:
    case ARM64_SYS_PROCESS_VM_WRITEV: {
      /* Self memory copy used as a fault-safe read/write probe. Stubbing it to
       * ENOSYS made the caller spin once per frame (a process_vm_readv flood),
       * wedging the boot path. Implement it for the own-process case: validate
       * each remote range with svcQueryMemory, then copy the readable parts. */
      va_list va; va_start(va, number);
      long pid                   = va_arg(va, long); (void)pid;
      const struct nx_iovec *liov   = va_arg(va, const struct nx_iovec *);
      unsigned long lcnt         = va_arg(va, unsigned long);
      const struct nx_iovec *riov   = va_arg(va, const struct nx_iovec *);
      unsigned long rcnt         = va_arg(va, unsigned long);
      va_end(va);
      int writing = (number == ARM64_SYS_PROCESS_VM_WRITEV);
      static int dbg = 0;
      if (dbg < 5) {
        dbg++;
        debugPrintf("[sys%ld] %s lcnt=%lu rcnt=%lu remote0=%p rlen0=%zu caller=%p\n",
                    number, writing ? "vm_writev" : "vm_readv", lcnt, rcnt,
                    rcnt ? riov[0].iov_base : NULL, rcnt ? riov[0].iov_len : 0,
                    __builtin_return_address(0));
      }
      ssize_t total = 0;
      unsigned long li = 0, ri = 0; size_t lo = 0, ro = 0;
      while (li < lcnt && ri < rcnt) {
        char *lp = (char *)liov[li].iov_base + lo;
        char *rp = (char *)riov[ri].iov_base + ro;
        size_t lrem = liov[li].iov_len - lo, rrem = riov[ri].iov_len - ro;
        size_t n = lrem < rrem ? lrem : rrem;
        char *probe = writing ? lp : rp;   /* the side being read-from must be readable */
        if (!nx_addr_readable((uintptr_t)probe, n)) {
          if (total == 0) { errno = EFAULT; return -1; }
          return total;
        }
        if (writing) memcpy(rp, lp, n); else memcpy(lp, rp, n);
        total += (ssize_t)n; lo += n; ro += n;
        if (lo == liov[li].iov_len) { li++; lo = 0; }
        if (ro == riov[ri].iov_len) { ri++; ro = 0; }
      }
      return total;
    }
  }
  debugPrintf("libc: syscall(%ld) -> ENOSYS\n", number);
  errno = ENOSYS;
  return -1;
}

void sincosf_fake(float x, float *s, float *c) { *s = sinf(x); *c = cosf(x); }
int sched_get_priority_max_fake(int policy) { (void)policy; return 0; }
int sched_get_priority_min_fake(int policy) { (void)policy; return 0; }
void android_set_abort_message_fake(const char *msg) { debugPrintf("abort message: %s\n", msg ? msg : "(null)"); }
size_t __ctype_get_mb_cur_max_fake(void) { return 1; }
int __register_atfork_fake(void) { return 0; }
int __cxa_thread_atexit_impl_fake(void (*fn)(void *), void *arg, void *dso) { (void)fn; (void)arg; (void)dso; return 0; }

#define BIONIC_SC_PAGESIZE 39
#define BIONIC_SC_PAGE_SIZE 40
#define BIONIC_SC_NPROCESSORS_CONF 96
#define BIONIC_SC_NPROCESSORS_ONLN 97
#define BIONIC_SC_PHYS_PAGES 98

long sysconf_fake(int name) {
  switch (name) {
    case BIONIC_SC_PAGESIZE:
    case BIONIC_SC_PAGE_SIZE: return 0x1000;
    case BIONIC_SC_NPROCESSORS_CONF:
    case BIONIC_SC_NPROCESSORS_ONLN: return 3;
    // Report 512 MB (matches synthetic /proc/meminfo MemTotal) to make Unity's
    // DynamicHeap reserve fewer 256MB regions; real backing (arena/OC) holds more.
    case BIONIC_SC_PHYS_PAGES: return (512ll * 1024 * 1024) / 0x1000;
    default: return -1;
  }
}
long pathconf_fake(const char *path, int name) { (void)path; (void)name; return -1; }

// ---------------------------------------------------------------------------
// open() flag translation (bionic/linux -> newlib)
// ---------------------------------------------------------------------------

#define LINUX_O_CREAT  0100
#define LINUX_O_EXCL   0200
#define LINUX_O_TRUNC  01000
#define LINUX_O_APPEND 02000

static int convert_open_flags(int flags) {
  int out = flags & 3;
  if (flags & LINUX_O_CREAT)  out |= O_CREAT;
  if (flags & LINUX_O_EXCL)   out |= O_EXCL;
  if (flags & LINUX_O_TRUNC)  out |= O_TRUNC;
  if (flags & LINUX_O_APPEND) out |= O_APPEND;
  return out;
}

// The engine addresses asset packs as "<packdir>/<file>.mvgl" but we ship the
// data flat in the game dir. If a read path with a subdirectory is missing,
// fall back to just its basename in the cwd (the game dir). Reads only -- never
// redirect a write -- and only when the basename actually exists.
//
// Every stat()/open() the listing cache answers "absent" ends up here, and
// that is most of them (~60 %): a real stat() of the basename in the game
// folder for each one -- the 0.3-0.46 ms per "cached" stat seen on hardware.
// Mostly DIFFERENT names, so remembering the absent ones was not enough
// (v0.1.21 first try: half still real). The game folder itself is small:
// it is listed once (names hashed case-insensitively, fixed table, no
// allocation) and a basename not in it is absent without any FS call. The
// listing is redone after anything is created right in that folder
// (bn_forget_for: write-open, fopen for writing, rename, mkdir).
#define BN_SLOTS 1024                      /* power of two; 0 = empty slot */
static uint32_t g_bn_names[BN_SLOTS];
static int      g_bn_n, g_bn_valid;        /* valid: listing up to date */
static Mutex    g_bn_m;
static uint64_t g_bn_hits, g_bn_real, g_bn_lists;
static uint32_t bn_hash_ci(const char *s) {
  uint32_t h = 2166136261u;
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (c >= 'A' && c <= 'Z') c += 32;     /* FAT is case-insensitive */
    h = (h ^ c) * 16777619u;
  }
  return h ? h : 1;
}
static void bn_add(uint32_t h) {
  unsigned i = h & (BN_SLOTS - 1);
  while (g_bn_names[i] && g_bn_names[i] != h) i = (i + 1) & (BN_SLOTS - 1);
  if (!g_bn_names[i]) { g_bn_names[i] = h; g_bn_n++; }
}
/* Under g_bn_m. 0 = could not list (caller falls back to a real stat). */
static int bn_list(void) {
  char fsp[FS_MAX_PATH];
  FsFileSystem *fs = NULL;
  if (fsdevTranslatePath(cp_game_root(), &fs, fsp) == -1 || !fs) return 0;
  FsDir d;
  if (R_FAILED(fsFsOpenDirectory(fs, fsp, FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, &d))) return 0;
  memset(g_bn_names, 0, sizeof g_bn_names);
  g_bn_n = 0;
  static FsDirectoryEntry ent[32];
  int ok = 1;
  for (;;) {
    s64 got = 0;
    if (R_FAILED(fsDirRead(&d, &got, 32, ent))) { ok = 0; break; }
    if (got <= 0) break;
    for (s64 k = 0; k < got; k++) {
      if (g_bn_n >= BN_SLOTS / 2) { ok = 0; break; }   /* unexpectedly big: don't trust it */
      ent[k].name[sizeof ent[k].name - 1] = 0;
      bn_add(bn_hash_ci(ent[k].name));
    }
    if (!ok) break;
  }
  fsDirClose(&d);
  g_bn_lists++;
  g_bn_valid = ok;
  return ok;
}
/* 1 = the basename is surely absent from the game folder. */
static int bn_known_absent(const char *base) {
  int absent = 0;
  mutexLock(&g_bn_m);
  if (g_bn_valid || bn_list()) {
    uint32_t h = bn_hash_ci(base);
    unsigned i = h & (BN_SLOTS - 1);
    absent = 1;
    for (unsigned k = 0; k < BN_SLOTS && g_bn_names[i]; k++, i = (i + 1) & (BN_SLOTS - 1))
      if (g_bn_names[i] == h) { absent = 0; break; }
    if (absent) g_bn_hits++;
  }
  mutexUnlock(&g_bn_m);
  return absent;
}
static void bn_forget(void) {
  mutexLock(&g_bn_m);
  g_bn_valid = 0;
  mutexUnlock(&g_bn_m);
}
/* The fallback only looks in the game folder (the cwd): only something created
 * right there can turn an "absent" basename into a present one. Saves and
 * Unity's own files live in subfolders. */
static void bn_forget_for(const char *path) {
  if (!path) return;
  const char *slash = strrchr(path, '/');
  if (!slash) { bn_forget(); return; }          /* relative to the cwd */
  size_t dl = (size_t)(slash - path);
  const char *root = cp_game_root();            /* "sdmc:/switch/mvlo" */
  const char *rnd = strchr(root, ':');
  rnd = rnd ? rnd + 1 : root;                   /* "/switch/mvlo" */
  const char *pnd = strchr(path, ':');
  const char *p = pnd && pnd < slash ? pnd + 1 : path;
  size_t pl = (size_t)(slash - p);
  if ((dl == strlen(root) && !strncasecmp(path, root, dl)) ||
      (pl == strlen(rnd) && !strncasecmp(p, rnd, pl)))
    bn_forget();
}
void nx_bn_report(void) {
  if (g_bn_hits || g_bn_real)
    debugPrintf("[io] basename fallback: %llu answered from the game folder listing (%d names, listed %llu times), "
                "%llu real stats\n", (unsigned long long)g_bn_hits, g_bn_n, (unsigned long long)g_bn_lists,
                (unsigned long long)g_bn_real);
}
static int basename_fallback(const char *path, char *out, size_t outsz) {
  const char *slash = strrchr(path, '/');
  if (!slash || !slash[1]) return 0;   // no subdir component to strip
  struct stat st;
  snprintf(out, outsz, "%s", slash + 1); // basename, resolved against the cwd
  if (bn_known_absent(out)) { errno = ENOENT; return 0; }
  g_bn_real++;
  return stat(out, &st) == 0;
}

// Create one directory, skipping paths newlib's mkdir() can't handle safely.
// A bare "device:" path (e.g. "sdmc:") makes newlib resolve to a device root
// with an empty in-device path and dereference a NULL devoptab -- a Data Abort
// reading devoptab->mkdir_r at +0x68. Refuse those (and null/empty).
static int safe_mkdir(const char *p) {
  if (!p || !*p) { errno = EINVAL; return -1; }
  const char *colon = strchr(p, ':');
  if (colon) {                       // has a "device:" prefix
    const char *in = colon + 1;      // the path inside the device
    while (*in == '/') in++;
    if (!*in) { errno = EEXIST; return 0; }  // "sdmc:" / "sdmc:/" -> root, skip
    // A single top-level component ("sdmc:/switch") also null-derefs newlib's
    // devoptab. Such dirs (the homebrew mount point) always pre-exist already.
    if (!strchr(in, '/')) { errno = EEXIST; return 0; }
  }
  return mkdir(p, 0777);
}

// mkdir -p: create `dir` and every missing parent. Save data lives in subdirs
// the engine only mkdir()s one level at a time, so a deeper missing parent left
// the whole chain (and the save write) failing.
//
// We must NOT try to create the game root or any ancestor of it ("sdmc:",
// "sdmc:/switch", "sdmc:/switch/zookeeper"): they already exist, they aren't
// ours, and newlib's mkdir() of a *top-level* path (one component under the
// device, e.g. "sdmc:/switch") null-derefs its devoptab -> Data Abort at the
// mkdir_r slot (+0x68). So begin the parent walk *after* GAME_HOME.
static void mkdir_p_dir(const char *dir) {
  if (!dir || !*dir) return;
  char tmp[512];
  if (snprintf(tmp, sizeof(tmp), "%s", dir) <= 0) return;
  size_t skip;
  const char *root = cp_game_root();
  const size_t glen = strlen(root);
  if (strncmp(tmp, root, glen) == 0 && (tmp[glen] == '/' || tmp[glen] == '\0')) {
    skip = glen;                                  // only create *under* the game root
  } else {
    const char *colon = strchr(tmp, ':');         // unknown base: at least skip "device:"
    skip = colon ? (size_t)(colon + 1 - tmp) : 0;
  }
  for (char *p = tmp + skip + 1; *p; p++)
    if (*p == '/') { *p = '\0'; safe_mkdir(tmp); *p = '/'; }
  if (tmp[skip]) safe_mkdir(tmp);
}
// create the parent directory chain of a file path
static void mkdir_parents(const char *filepath) {
  char tmp[512];
  snprintf(tmp, sizeof(tmp), "%s", filepath);
  char *last = strrchr(tmp, '/');
  if (!last || last == tmp) return;
  *last = '\0';
  mkdir_p_dir(tmp);
}

// mkdir wrapper: create the full chain and treat "already exists" as success
int mkdir_fake(const char *path, unsigned mode) {
  (void)mode;
  if (!path || !*path) { errno = EINVAL; return -1; }
  bn_forget_for(path);
  mkdir_p_dir(path);
  int r = safe_mkdir(path);
  if (r != 0 && errno == EEXIST) r = 0;
  return r;
}

int g_watch_fd = -1;   /* data.unity3d fd: trace its reads/seeks to debug header load */
void watch_dump(const char *tag, int fd, long a, long b, const void *buf, long got) {
  if (fd != g_watch_fd) return;
  char h[64]; int n = (got > 16 ? 16 : (got < 0 ? 0 : (int)got));
  int p = 0; for (int i = 0; i < n; i++) p += snprintf(h + p, sizeof(h) - p, "%02x ", ((const unsigned char *)buf)[i]);
  h[p] = 0;
  if (TRACE_IO) debugPrintf("[io] %s fd=%d a=%ld b=%ld -> %ld  [%s]\n", tag, fd, a, b, got, h);
}

/* ---- read-ahead cache for big archive files (data.unity3d, sharedassets) ----
 * Unity deserializes archives with THOUSANDS of tiny read()s (2-8 bytes each,
 * field by field). On Android the archive sits in the OS page cache so these are
 * memory-fast; on Switch there is no page cache, so every tiny read is a direct
 * SD access (~ms) and boot crawls (8000+ reads just for the header). We buffer
 * each big read-only fd through a 1MB window filled by one large read, and
 * virtualize the logical file position -- turning ~250k tiny SD reads per MB into
 * a single one. Keyed by fd; the real fd position is used only as our scratch. */
#define RA_SLOTS 8
#define RA_WIN   (1u << 20)     /* 1 MB read-ahead window */
static struct RaCache {
  int  fd;           /* -1 == free */
  long pos;          /* virtual file position (what read/lseek observe) */
  long size;         /* file size (for SEEK_END) */
  long base;         /* file offset of buf[0] */
  long len;          /* valid bytes currently in buf */
  unsigned char *buf;
} g_ra[RA_SLOTS];
static Mutex g_ra_lock;
static struct RaCache *ra_find(int fd) {
  if (fd < 0) return NULL;
  for (int i = 0; i < RA_SLOTS; i++) if (g_ra[i].fd == fd) return &g_ra[i];
  return NULL;
}
void ra_attach(int fd, long size) {
  mutexLock(&g_ra_lock);
  for (int i = 0; i < RA_SLOTS; i++) if (g_ra[i].fd < 0) {
    if (!g_ra[i].buf) g_ra[i].buf = malloc(RA_WIN);
    if (g_ra[i].buf) { g_ra[i].fd = fd; g_ra[i].pos = 0; g_ra[i].size = size; g_ra[i].base = 0; g_ra[i].len = 0; }
    break;
  }
  mutexUnlock(&g_ra_lock);
}
void ra_detach(int fd) {
  mutexLock(&g_ra_lock);
  struct RaCache *c = ra_find(fd);
  if (c) c->fd = -1;   /* keep buf allocated for reuse */
  mutexUnlock(&g_ra_lock);
}
static long ra_read(struct RaCache *c, int fd, void *buf, size_t count) {
  size_t done = 0;
  mutexLock(&g_ra_lock);
  while (done < count) {
    if (c->len == 0 || c->pos < c->base || c->pos >= c->base + c->len) {
      if (lseek(fd, c->pos, SEEK_SET) < 0) break;
      long r = 0;
      while (r < (long)RA_WIN) { long k = read(fd, c->buf + r, RA_WIN - r); if (k <= 0) break; r += k; }
      if (r <= 0) break;
      c->base = c->pos; c->len = r;
    }
    long avail = (c->base + c->len) - c->pos;
    if (avail <= 0) break;
    size_t n = (count - done < (size_t)avail) ? count - done : (size_t)avail;
    memcpy((char *)buf + done, c->buf + (c->pos - c->base), n);
    c->pos += n; done += n;
  }
  mutexUnlock(&g_ra_lock);
  return (long)done;
}

/* lseek for arm64: off_t is already 64-bit, so this also services lseek64.
 * lseek64 was previously stubbed to return 0 (no seek) -- that made libunity's
 * archive reader see data.unity3d as empty/mis-positioned ("Unable to read
 * header from archive file"), since it lseek64(SEEK_END)s to size the file. */
static int wb_lseek(int fd, long off, int whence, long *pos);   /* write-behind, below */
long z_lseek(int fd, long off, int whence) {
  {
    Phantom *p = ph_get(fd);
    if (p) {
      int64_t np = whence == SEEK_SET ? off : whence == SEEK_CUR ? p->pos + off : p->size + off;
      if (np < 0) { errno = EINVAL; return -1; }
      p->pos = np;
      return (long)np;
    }
  }
  if (nxio_owns(fd)) return (long)nxio_lseek(fd, off, whence);
  { long pos; if (wb_lseek(fd, off, whence, &pos)) return pos; }
  struct RaCache *c = ra_find(fd);
  if (c) {   /* virtualized position -- don't touch the real fd here */
    mutexLock(&g_ra_lock);
    long np = (whence == SEEK_SET) ? off : (whence == SEEK_CUR) ? c->pos + off : c->size + off;
    c->pos = np;
    mutexUnlock(&g_ra_lock);
    return np;
  }
  return lseek(fd, off, whence);
}

static const char *synthetic_proc(const char *path);  /* defined below */

// Serve /proc and /sys reads that arrive through raw open() (e.g.
// /proc/self/maps, which the engine opens to enumerate memory mappings).
// newlib's open() can't be memory-backed, so materialize the synthetic content
// into a small file under the game dir and hand back a real fd. Returns an fd,
// or -1 if `path` isn't a node we synthesize (caller proceeds normally).
static int synth_proc_open(const char *path) {
  if (!path) return -1;
  if (strncmp(path, "/proc/", 6) && strncmp(path, "/sys/", 5)) return -1;
  static char buf[16384];
  int len;
  if (!strcmp(path, "/proc/self/maps") || !strcmp(path, "/proc/self/smaps")) {
    len = so_dump_maps(buf, sizeof buf);
  } else {
    const char *s = synthetic_proc(path);
    if (!s) return -1;                                   // not /proc or /sys
    len = (int)strlen(s);
    if (len > (int)sizeof buf) len = (int)sizeof buf;
    memcpy(buf, s, (size_t)len);
  }
  char safe[160]; size_t j = 0;
  for (const char *p = path; *p && j < sizeof safe - 1; p++) safe[j++] = (*p == '/') ? '_' : *p;
  safe[j] = '\0';
  char tf[256];
  snprintf(tf, sizeof tf, "%s/.synth%s", cp_game_root(), safe);
  int wfd = open(tf, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (wfd >= 0) { if (write(wfd, buf, (size_t)len) < 0) { /* best effort */ } close(wfd); }
  return open(tf, O_RDONLY);
}

// ---------------------------------------------------------------------------
// Synthetic inode numbers. libnx's fsdev returns st_ino==0 for EVERY file, but
// il2cpp's System.IO share layer keys its open-file table on (st_dev,st_ino):
// with every file colliding on inode 0 it treats UNRELATED files as the same
// one, so Easy Save's concurrent SaveData.es3.tmp (write) + SaveData.es3 (read)
// looks like an incompatible double-open of one file and throws "IOException:
// Sharing violation on path SaveData.es3" every frame on the menu -- and each of
// those exception backtraces mmaps libunity/libil2cpp, leaking us to OOM/exit.
// Give each distinct path a stable, non-zero inode (only when the real one is 0,
// so a genuine inode from a future fsdev is preserved). fstat() has no path, so a
// small fd->inode map is filled at open() time and mirrors stat(path)'s value.
#define FD_INO_MAX 4096
static uint64_t g_fd_ino[FD_INO_MAX];
static uint64_t path_ino(const char *path) {
  // One key per file whatever the spelling: "sdmc:/x" and "/x" are the same
  // file, and the game opens its data RELATIVE to the cwd (the game root,
  // main.c chdir) as "assets/bin/Data/...". The preload's absolute paths never
  // matched (02/11, v0.1.32-35: 0 preloaded reads served).
  char abs[512];
  if (!strncmp(path, "sdmc:", 5)) path += 5;
  else if (path[0] != '/' && !strchr(path, ':')) {
    const char *root = cp_game_root();
    const char *colon = strchr(root, ':');
    if (path[0] == '.' && path[1] == '/') path += 2;
    snprintf(abs, sizeof abs, "%s/%s", colon ? colon + 1 : root, path);
    path = abs;
  }
  uint64_t h = 1469598103934665603ULL;               // FNV-1a 64 offset basis
  for (const unsigned char *p = (const unsigned char *)path; *p; p++) { h ^= *p; h *= 1099511628211ULL; }
  return h ? h : 1;                                   // 0 means "no inode" -- avoid it
}
static void fd_ino_set(int fd, const char *path) { if (fd >= 0 && fd < FD_INO_MAX) g_fd_ino[fd] = path_ino(path); }
/* fds opened on /dev/urandom or /dev/random: read_fake serves them from the
 * CSPRNG, never from the 64 KB file behind them (see open_fake). */
static uint8_t g_fd_rand[FD_INO_MAX];
static void fd_ino_clear(int fd) { if (fd >= 0 && fd < FD_INO_MAX) g_fd_ino[fd] = 0; }

/* Close every fd we know to be open on `path`, returning the LOWEST one we closed (or -1).
 * Used by rename_fake: Horizon refuses to rename, delete, or even open-for-write a file that
 * has a live handle, and il2cpp's ReplaceFile holds one on the very file we must replace.
 * g_fd_ino is a 64-bit FNV hash of the path, maintained by open_fake/close_fake, so a false
 * match would need a 64-bit hash collision against a path opened in this same process. */
int close_fake(int fd);          /* fwd: clears g_fd_ino and commits the SD card */
static int park_drop(uint64_t ino);   /* fwd: kept-open read-only files, below */
static int fd_close_by_path(const char *path) {
  const uint64_t ino = path_ino(path);
  int first = -1;
  for (int fd = 0; fd < FD_INO_MAX; fd++) {
    if (g_fd_ino[fd] != ino) continue;
    if (TRACE_IO) debugPrintf("[io] rename: closing caller's open handle fd=%d on %s\n", fd, path);
    close_fake(fd);
    if (first < 0) first = fd;
  }
  park_drop(ino);              /* close_fake may have parked them: really close */
  return first;
}

// libnx buffers sdmc: writes -- they reach the physical card only on fsdevCommitDevice() or a
// clean unmount. Exiting via HOME kills the process (title override), so that commit never runs
// and saves written this session vanish on reboot. Track fds opened for writing and commit the
// card when they close, so a save persists the moment the game finishes writing it.
#define WFD_MAX 4096
static unsigned char g_write_fd[WFD_MAX];
int nx_sd_dirty = 0;   /* a file has been opened for writing since the last commit */
static void mark_write_fd(int fd)   { nx_sd_dirty = 1; if (fd >= 0 && fd < WFD_MAX) g_write_fd[fd] = 1; }
static void commit_write_fd(int fd) { if (fd >= 0 && fd < WFD_MAX && g_write_fd[fd]) {
                                        g_write_fd[fd] = 0; nx_sd_dirty = 0; fsdevCommitDevice("sdmc"); } }

/* Commit any pending writes to the physical card. Called periodically from the render loop and
 * after the game's save (nx_save_prefs), so the save survives a HOME-kill / reboot. */
void nx_sd_flush(void) {
  debug_log_kick();    /* our own buffered log: the log thread writes it */
  if (!nx_sd_dirty) return;
  nx_sd_dirty = 0;
  fsdevCommitDevice("sdmc");
}

/* ---- stat cache for the replays folder ---------------------------------------
 * When a match ends the game looks at every temporary replay (size and dates,
 * to pick the oldest to delete): 25 stats of 4-5 FS round trips each, ~50 ms
 * of that freeze (05/11). A replay never changes once written, so answers for
 * paths under /replays/ (and the folders themselves) are kept until this
 * process writes, renames or deletes that path. A folder's own answer is kept
 * when files come and go in it: the game only asks whether it exists
 * (Directory.CreateDirectory, FileStream's folder check), never its dates.
 * 4-way sets: with one slot per key, two replays that shared it pushed each
 * other out on every match end (05/11: still 28-36 ms of stats per save). */
#define RSC_SETS 64
#define RSC_WAYS 4
typedef struct { uint64_t ino; struct stat st; } RscEnt;
static RscEnt g_rsc[RSC_SETS][RSC_WAYS];
static Mutex  g_rsc_lock;
static unsigned g_rsc_hits, g_rsc_misses;       /* since the last replay save */
static uint64_t g_rsc_miss_ns;
static char     g_rsc_miss_eg[200];
static int rsc_on(const char *path) {
  if (!path) return 0;
  if (strstr(path, "/replays/")) return 1;
  size_t n = strlen(path);
  return n >= 8 && !strcmp(path + n - 8, "/replays");
}
static int rsc_get(const char *path, struct stat *out) {
  if (!rsc_on(path)) return 0;
  uint64_t ino = path_ino(path);
  int hit = 0;
  mutexLock(&g_rsc_lock);
  RscEnt *set = g_rsc[ino % RSC_SETS];
  for (int w = 0; w < RSC_WAYS; w++)
    if (set[w].ino == ino) { *out = set[w].st; hit = 1; break; }
  if (hit) g_rsc_hits++;
  mutexUnlock(&g_rsc_lock);
  return hit;
}
static void rsc_put(const char *path, const struct stat *st) {
  if (!rsc_on(path)) return;
  uint64_t ino = path_ino(path);
  mutexLock(&g_rsc_lock);
  RscEnt *set = g_rsc[ino % RSC_SETS];
  int w = 0;
  while (w < RSC_WAYS && set[w].ino && set[w].ino != ino) w++;
  if (w == RSC_WAYS) w = (int)((ino >> 32) % RSC_WAYS);   /* set full: replace one */
  set[w] = (RscEnt){ ino, *st };
  mutexUnlock(&g_rsc_lock);
}
static void rsc_forget(const char *path) {
  if (!rsc_on(path)) return;
  uint64_t ino = path_ino(path);
  mutexLock(&g_rsc_lock);
  RscEnt *set = g_rsc[ino % RSC_SETS];
  for (int w = 0; w < RSC_WAYS; w++)
    if (set[w].ino == ino) set[w].ino = 0;
  mutexUnlock(&g_rsc_lock);
}
/* a /replays/ stat that went to the card: counted for rsc_report */
static void rsc_note_miss(const char *path, uint64_t ns, int absent) {
  mutexLock(&g_rsc_lock);
  g_rsc_misses++;
  g_rsc_miss_ns += ns;
  size_t used = strlen(g_rsc_miss_eg);
  if (g_rsc_misses <= 4 && used < sizeof g_rsc_miss_eg - 8) {
    const char *r = strstr(path, "/replays");
    snprintf(g_rsc_miss_eg + used, sizeof g_rsc_miss_eg - used, "%s%s%s",
             used ? ", " : "", r ? r : path, absent ? " (absent)" : "");
  }
  mutexUnlock(&g_rsc_lock);
}
/* One line per replay save: what the stats since the previous one cost. */
static void rsc_report(void) {
  mutexLock(&g_rsc_lock);
  unsigned h = g_rsc_hits, m = g_rsc_misses;
  uint64_t ms = g_rsc_miss_ns / 1000000ull;
  char eg[sizeof g_rsc_miss_eg];
  memcpy(eg, g_rsc_miss_eg, sizeof eg);
  g_rsc_hits = g_rsc_misses = 0; g_rsc_miss_ns = 0; g_rsc_miss_eg[0] = 0;
  mutexUnlock(&g_rsc_lock);
  debugPrintf("[io] replays stats since the last save: %u from the cache, %u from the card in %llu ms%s%s\n",
              h, m, (unsigned long long)ms, m ? ", e.g. " : "", m ? eg : "");
}

/* ---- write-behind for files the game creates ([io] write_behind, default 1) --
 * The replay saved when a match ends went to the card one write() at a time:
 * each a fsFileWrite that grows the file, then close and commit -- about half
 * of the ~350 ms freeze (04/11 profile: 41 % fsFileWrite, 12 % open/close). A
 * file opened with O_CREAT|O_TRUNC now fills a buffer in memory; close() hands
 * it to the "write-behind" thread, which writes it in one go, closes the
 * descriptor and commits the card. Anything else done on the descriptor
 * (read, seek, dup, mmap, truncate, sync) first writes the buffer out and
 * stops buffering it; an open, stat, rename or unlink of a file still being
 * written waits for it, and so does the process exit. */
#define WB_LIMIT (16u << 20)            /* past this, a file is written as it comes */
#define WB_QUEUE 16
typedef struct { uint8_t *data; size_t len, cap; int64_t start; unsigned writes; } WBuf;
typedef struct { int fd; WBuf *wb; uint64_t ino; char *path; } WbJob;
static WBuf *g_wb[WFD_MAX];
static WbJob g_wbq[WB_QUEUE];
static volatile int g_wbq_head, g_wbq_tail;   /* [head, tail): queued or being written */
static Mutex   g_wbq_lock;
static CondVar g_wbq_cv, g_wbq_done_cv;
static int     g_wb_state;                    /* 0 unknown, 1 on, -1 off */
static Thread  g_wb_thread;
static unsigned g_wb_files;
static uint64_t g_wb_bytes, g_wb_max_ms;

static void wb_free(WBuf *w) { if (w) { free(w->data); free(w); } }

/* write() the buffer to the descriptor's current position; 0 or -1. */
static int wb_write_out(int fd, WBuf *w) {
  size_t off = 0;
  while (off < w->len) {
    long r = write(fd, w->data + off, w->len - off);
    if (r <= 0) return -1;
    off += (size_t)r;
  }
  return 0;
}

static void wb_main(void *arg) {
  (void)arg;
  for (;;) {
    mutexLock(&g_wbq_lock);
    while (g_wbq_head == g_wbq_tail) condvarWait(&g_wbq_cv, &g_wbq_lock);
    WbJob j = g_wbq[g_wbq_head % WB_QUEUE];
    mutexUnlock(&g_wbq_lock);

    uint64_t t0 = armTicksToNs(armGetSystemTick());
    if (!j.wb) {                       /* a deferred unlink (nx_unlink) */
      int r = unlink(j.path);
      fsdevCommitDevice("sdmc");
      rsc_forget(j.path);
      static int nlog;
      if (r != 0 || nlog++ < 3)
        debugPrintf("[io] write-behind: unlink %s %s in %llu ms\n", j.path, r == 0 ? "done" : "FAILED",
                    (unsigned long long)((armTicksToNs(armGetSystemTick()) - t0) / 1000000ull));
    } else {
      int ok = wb_write_out(j.fd, j.wb) == 0;
      close(j.fd);
      fsdevCommitDevice("sdmc");
      if (j.path) rsc_forget(j.path);
      if (j.path && strstr(j.path, "/replays/")) rsc_report();
      uint64_t ms = (armTicksToNs(armGetSystemTick()) - t0) / 1000000ull;
      if (!ok || j.wb->len >= 64 * 1024 || g_wb_files < 3)
        debugPrintf("[io] write-behind: %s, %zu KB from %u writes, written+closed+committed in %llu ms%s\n",
                    j.path ? j.path : "?", j.wb->len >> 10, j.wb->writes, (unsigned long long)ms,
                    ok ? "" : " -- WRITE FAILED");
      g_wb_files++;
      g_wb_bytes += j.wb->len;
      if (ms > g_wb_max_ms) g_wb_max_ms = ms;
      wb_free(j.wb);
    }
    free(j.path);

    mutexLock(&g_wbq_lock);
    g_wbq_head++;
    condvarWakeAll(&g_wbq_done_cv);
    mutexUnlock(&g_wbq_lock);
  }
}

static int wb_enabled(void) {
  if (g_wb_state) return g_wb_state > 0;
  mutexLock(&g_wbq_lock);
  if (!g_wb_state) {
    g_wb_state = -1;
    if (nxcfg_int("io.write_behind", 1) == 1) {
      condvarInit(&g_wbq_cv);
      condvarInit(&g_wbq_done_cv);
      if (R_SUCCEEDED(threadCreate(&g_wb_thread, wb_main, NULL, NULL, 0x8000, 0x3B, -2)) &&
          R_SUCCEEDED(threadStart(&g_wb_thread))) {
        g_wb_state = 1;
        debugPrintf("[io] write-behind on: files the game creates are written off its thread\n");
      }
    }
  }
  mutexUnlock(&g_wbq_lock);
  return g_wb_state > 0;
}

/* open_fake: buffer a file just created or truncated for writing. */
static int wb_attach(int fd, int flags) {
  if (fd < 0 || fd >= WFD_MAX || (flags & 3) == 0 || !(flags & LINUX_O_CREAT) ||
      !(flags & LINUX_O_TRUNC) || (flags & LINUX_O_APPEND) || !wb_enabled())
    return 0;
  WBuf *w = calloc(1, sizeof *w);
  if (!w) return 0;
  g_wb[fd] = w;
  return 1;
}

/* Write out what is buffered for fd and stop buffering it. */
int nx_wb_settle(int fd) {
  if (fd < 0 || fd >= WFD_MAX || !g_wb[fd]) return 0;
  WBuf *w = g_wb[fd];
  g_wb[fd] = NULL;
  static int nlog;
  if (nlog < 12) {
    nlog++;
    uintptr_t ra = (uintptr_t)__builtin_return_address(0);
    MemoryInfo mi; u32 pi;
    uintptr_t base = R_SUCCEEDED(svcQueryMemory(&mi, &pi, ra)) ? (uintptr_t)mi.addr : 0;
    debugPrintf("[io] write-behind: fd %d written out early (%zu KB, caller NRO+0x%lx)\n", fd, w->len >> 10,
                (unsigned long)(ra - base));
  }
  int r = wb_write_out(fd, w);
  wb_free(w);
  return r;
}

/* z_lseek: a seek that lands where the next write goes -- "where am I", or
 * Mono's FileStream seeking to its own position before every write -- needs
 * no write (1, *pos set). The file was truncated when the buffer started, so
 * its end is that same spot. Any other seek writes the buffer out first (0). */
static int wb_lseek(int fd, long off, int whence, long *pos) {
  if (fd < 0 || fd >= WFD_MAX || !g_wb[fd]) return 0;
  const int64_t here = g_wb[fd]->start + (int64_t)g_wb[fd]->len;
  if ((whence == SEEK_CUR && off == 0) || (whence == SEEK_SET && off == here) ||
      (whence == SEEK_END && off == 0 && g_wb[fd]->start == 0)) {
    *pos = (long)here;
    return 1;
  }
  nx_wb_settle(fd);
  return 0;
}

static long wb_write(int fd, WBuf *w, const void *buf, size_t n) {
  if (w->len + n > WB_LIMIT) { nx_wb_settle(fd); return write(fd, buf, n); }
  if (w->len + n > w->cap) {
    size_t nc = w->cap ? w->cap : 64 * 1024;
    while (nc < w->len + n) nc *= 2;
    uint8_t *p = realloc(w->data, nc);
    if (!p) { nx_wb_settle(fd); return write(fd, buf, n); }
    w->data = p;
    w->cap = nc;
  }
  memcpy(w->data + w->len, buf, n);
  w->len += n;
  w->writes++;
  return (long)n;
}

/* close_fake: queue the buffered file (path: malloc'd, taken over on success);
 * 0 = queue full, close it there. */
static int wb_enqueue(int fd, WBuf *w, uint64_t ino, char *path) {
  mutexLock(&g_wbq_lock);
  int ok = g_wbq_tail - g_wbq_head < WB_QUEUE;
  if (ok) {
    g_wbq[g_wbq_tail % WB_QUEUE] = (WbJob){ fd, w, ino, path };
    g_wbq_tail++;
    condvarWakeOne(&g_wbq_cv);
  }
  mutexUnlock(&g_wbq_lock);
  return ok;
}

/* Before touching a path: wait while a file with that name is being written. */
static void wb_wait_ino(uint64_t ino) {
  if (g_wbq_head == g_wbq_tail) return;
  mutexLock(&g_wbq_lock);
  for (;;) {
    int busy = 0;
    for (int i = g_wbq_head; i != g_wbq_tail; i++)
      if (g_wbq[i % WB_QUEUE].ino == ino) busy = 1;
    if (!busy) break;
    condvarWait(&g_wbq_done_cv, &g_wbq_lock);
  }
  mutexUnlock(&g_wbq_lock);
}
static void wb_wait_path(const char *path) { if (path && g_wbq_head != g_wbq_tail) wb_wait_ino(path_ino(path)); }

/* unlink/remove: Horizon refuses while the write-behind thread holds the file.
 * An old replay is deleted by that thread too (~20 ms of FS calls when a match
 * ends: the game drops it from its list first and never waits on it); a later
 * open or stat of that path waits for the delete. 0 = not queued. */
static int wb_unlink_behind(const char *path) {
  if (!path || !strstr(path, "/replays/") || !wb_enabled()) return 0;
  char *p = strdup(path);
  if (!p) return 0;
  if (wb_enqueue(-1, NULL, path_ino(path), p)) return 1;
  free(p);
  return 0;
}
int nx_unlink(const char *path) {
  wb_wait_path(path);
  rsc_forget(path);
  return wb_unlink_behind(path) ? 0 : unlink(path);
}
int nx_remove(const char *path) {   /* may be a directory */
  wb_wait_path(path);
  rsc_forget(path);
  return remove(path);
}

/* Process exit (main.c): every queued file reaches the card. */
void nx_wb_drain(void) {
  if (g_wbq_head == g_wbq_tail) return;
  mutexLock(&g_wbq_lock);
  while (g_wbq_head != g_wbq_tail) condvarWait(&g_wbq_done_cv, &g_wbq_lock);
  mutexUnlock(&g_wbq_lock);
}

/* Unity probes filesystem case-sensitivity on every boot: it creates
 * CASESENSITIVETEST<guid> (O_CREAT|O_EXCL) in the data root, re-opens it under a
 * different case, and never cleans up -- a fresh GUID name each launch, so junk
 * accumulates on the SD card. Redirect every such name (any case, any guid) onto ONE
 * fixed hidden scratch file. Different-case probes then hit the same file, which is
 * exactly the case-INSENSITIVE answer FAT/exFAT gives anyway, and main.c sweeps the
 * scratch (plus any strays from older builds) at boot. (Copied from zookeeperdx_nx.) */
static const char *casetest_redirect(const char *path) {
  if (!path) return path;
  const char *b = strrchr(path, '/');
  b = b ? b + 1 : path;
  if (strncasecmp(b, "CASESENSITIVETEST", 17) == 0) {
    /* Was a compile-time literal; now built from the runtime root, so it needs
     * storage that outlives the call. One probe path, one buffer. */
    static char casetest[560];
    if (!casetest[0])
      snprintf(casetest, sizeof casetest, "%s/.casetest", cp_game_root());
    return casetest;
  }
  return path;
}

/* Android APK asset root -> our asset dir on the SD card.
 *
 * The game builds ABSOLUTE "/assets/..." paths (on Android that's the APK's
 * asset root). On Switch a leading "/" makes newlib resolve against the DEFAULT
 * DEVICE ROOT, i.e. sdmc:/assets/... -- the root of the SD card -- which does not
 * exist. Our assets live under GAME_HOME/assets/. Result: every absolute asset
 * open returned -1 (AssetBundles/*.unity3d, Data/*.json) even though the files
 * were present, because they were being looked for in the wrong place.
 *
 * Relative "assets/..." paths already work (they resolve against the cwd, which
 * main.c chdir()s to GAME_HOME), so ONLY the absolute form needs rewriting.
 * basename_fallback() can't cover this: it drops the subdirectory, so
 * "/assets/AssetBundles/Episode_1_Levels.unity3d" would degrade to looking for
 * "Episode_1_Levels.unity3d" in the game root. Keep the full subpath instead. */
static const char *asset_redirect(const char *path, char *buf, size_t bufsz) {
  if (!path) return path;
  if (!strncmp(path, "/assets/", 8)) {
    snprintf(buf, bufsz, "%s/assets/%s", cp_game_root(), path + 8);
    return buf;
  }
  if (!strcmp(path, "/assets")) {
    snprintf(buf, bufsz, "%s/assets", cp_game_root());
    return buf;
  }
  return path;
}

/* ---- file-system timing ---------------------------------------------------
 * On hardware a 24 s frame was 99.6 % "other" in the [hitch] breakdown while
 * the watchdog caught UnityMain inside an FS IPC under File::Open, and the
 * whole-console freezes keep the sound going while every file write stops.
 * So every game open/stat is timed: [hitch] gets "file open" / "file stat",
 * any op over 100 ms is logged with its path, and the watchdog's alive line
 * says which op has been in flight the longest. */
#include "nx_gpu.h"
static volatile uint64_t g_fs_busy_since;     /* start of the op that made g_fs_busy 1 */
static volatile int      g_fs_busy;           /* FS ops in flight, all threads */
static char              g_fs_busy_what[112];
static volatile int      g_files_open;        /* opened by open_fake, not closed yet */
static uint8_t           g_open_mark[FD_INO_MAX];
static inline uint64_t fs_now(void) { return armTicksToNs(armGetSystemTick()); }
static uint64_t fs_begin(const char *op, const char *path) {
  uint64_t t = fs_now();
  if (__atomic_fetch_add(&g_fs_busy, 1, __ATOMIC_ACQ_REL) == 0) {
    snprintf(g_fs_busy_what, sizeof g_fs_busy_what, "%s %s", op, path ? path : "-");
    g_fs_busy_since = t;
  }
  return t;
}
static void fs_end(uint64_t t0, int kind, const char *op, const char *path) {
  uint64_t dt = fs_now() - t0;
  if (__atomic_sub_fetch(&g_fs_busy, 1, __ATOMIC_ACQ_REL) == 0) g_fs_busy_since = 0;
  nx_gpu_account_fs(kind, dt);
  if (dt >= 100000000ull)
    debugPrintf("[io] slow %s %llu ms: %s (%d files open)\n", op,
                (unsigned long long)(dt / 1000000ull), path ? path : "-", g_files_open);
}
/* diag.c watchdog beacon: no locks, no allocation */
void nx_fs_beacon(char *buf, unsigned len) {
  uint64_t s = g_fs_busy_since;
  int n = g_fs_busy;
  if (s && n)
    nx_bfmt(buf, len, " fs: %d in flight, oldest %llums (%s), %d open", n,
             (unsigned long long)((fs_now() - s) / 1000000ull), g_fs_busy_what, g_files_open);
  else
    nx_bfmt(buf, len, " fs: idle, %d open", g_files_open);
}
int nx_files_open(void) { return g_files_open; }

/* fstat() of read-only data opened with the listing cache: answered from what
 * the listing said at open time instead of an FS GetSize round trip. */
typedef struct { int64_t size; time_t ctime, mtime, atime; uint32_t mode; uint8_t valid; } FdStat;
static FdStat *g_fd_st;            /* FD_INO_MAX entries, allocated on first use */
static void fd_stat_remember(int fd, const struct stat *st) {
  if (fd < 0 || fd >= FD_INO_MAX) return;
  if (!g_fd_st && !(g_fd_st = calloc(FD_INO_MAX, sizeof *g_fd_st))) return;
  FdStat *e = &g_fd_st[fd];
  e->size = st->st_size; e->mode = st->st_mode;
  e->ctime = st->st_ctime; e->mtime = st->st_mtime; e->atime = st->st_atime;
  e->valid = 1;
}
static int fd_stat_lookup(int fd, struct stat *st) {
  if (!g_fd_st || fd < 0 || fd >= FD_INO_MAX || !g_fd_st[fd].valid) return 0;
  const FdStat *e = &g_fd_st[fd];
  memset(st, 0, sizeof *st);
  st->st_nlink = 1; st->st_mode = e->mode; st->st_size = (off_t)e->size;
  st->st_ctime = e->ctime; st->st_mtime = e->mtime; st->st_atime = e->atime;
  return 1;
}
static void fd_stat_forget(int fd) { if (g_fd_st && fd >= 0 && fd < FD_INO_MAX) g_fd_st[fd].valid = 0; }

/* Diagnostics: path behind an fd opened by open_fake, so the first fstat()s
 * the listing cache could not answer say which files they were. */
static char **g_fd_path;
static void fd_path_set(int fd, const char *path) {
  if (fd < 0 || fd >= FD_INO_MAX || !path) return;
  if (!g_fd_path && !(g_fd_path = calloc(FD_INO_MAX, sizeof *g_fd_path))) return;
  free(g_fd_path[fd]);
  g_fd_path[fd] = strdup(path);
}
static void fd_path_clear(int fd) {
  if (g_fd_path && fd >= 0 && fd < FD_INO_MAX) { free(g_fd_path[fd]); g_fd_path[fd] = NULL; }
}
static const char *fd_path(int fd) {
  return (g_fd_path && fd >= 0 && fd < FD_INO_MAX && g_fd_path[fd]) ? g_fd_path[fd] : "(not opened by open_fake)";
}

/* ---- kept-open read-only data ([io] keep_open) -----------------------------
 * Unity closes and reopens the same data files all the time (one file 310
 * times in a session) and each open is two FS round trips in libnx's fsdev
 * (OpenFile + GetFileTimeStampRaw, ~4.9 ms on hardware; the timestamps only
 * feed fstat, which the listing cache answers for these files). A close of a
 * read-only file under assets/ or il2cpp/ (listing cache domain: data that
 * never changes during a session) parks the descriptor instead; the next open
 * of the same path gets it back, rewound -- no FS call at all. At most
 * keep_open parked (oldest really closed first, libnx allows 1024
 * descriptors); any failed open releases them all, and a write, rename or
 * unlink of the path drops its entries. */
typedef struct { int fd; uint64_t ino; uint64_t used; uint32_t opens; } Parked;
static Parked  *g_park;
static int      g_park_cap = -1, g_park_n;
static uint64_t g_park_clock, g_park_hits, g_park_parks;
static Mutex    g_park_m;
static uint8_t  g_keep_ok[FD_INO_MAX];          /* fd eligible for parking on close */
static uint64_t g_keep_ino[FD_INO_MAX];

static int park_enabled(void) {
  if (__atomic_load_n(&g_park_cap, __ATOMIC_ACQUIRE) < 0) {
    mutexLock(&g_park_m);
    if (g_park_cap < 0) {
      int cap = nxcfg_int("io.keep_open", 64);
      if (cap < 0) cap = 0;
      if (cap > 256) cap = 256;
      if (cap && !(g_park = calloc((size_t)cap, sizeof *g_park))) cap = 0;
      __atomic_store_n(&g_park_cap, cap, __ATOMIC_RELEASE);
    }
    mutexUnlock(&g_park_m);
  }
  return g_park_cap > 0;
}
/* Parked fd for this path, or -1. */
static int park_take(uint64_t ino) {
  if (!park_enabled()) return -1;
  int fd = -1;
  mutexLock(&g_park_m);
  for (int i = 0; i < g_park_n; i++)
    if (g_park[i].ino == ino) { fd = g_park[i].fd; g_park[i] = g_park[--g_park_n]; g_park_hits++; break; }
  mutexUnlock(&g_park_m);
  return fd;
}
/* How many times each eligible file was opened (fixed table, no allocation),
 * for the report. Eviction stays least-recently-parked: under Eden it served
 * 216 reopens at boot, against 112 for "fewest opens first" and 38 for
 * "park only files opened twice" (the first reopen of every file missed). */
#define SEEN_SLOTS 8192
static struct { uint64_t ino; uint32_t n; } g_seen[SEEN_SLOTS];
static int g_seen_used;
static uint32_t seen_bump(uint64_t ino) {      /* under g_park_m */
  unsigned i = (unsigned)ino & (SEEN_SLOTS - 1);
  for (unsigned k = 0; k < SEEN_SLOTS; k++, i = (i + 1) & (SEEN_SLOTS - 1)) {
    if (g_seen[i].ino == ino) return ++g_seen[i].n;
    if (!g_seen[i].ino) {
      if (g_seen_used >= SEEN_SLOTS / 2) return 2;   /* table full: park anyway */
      g_seen[i].ino = ino; g_seen[i].n = 1; g_seen_used++;
      return 1;
    }
  }
  return 2;
}
static uint64_t g_park_once;
static void park_note_open(uint64_t ino) {
  if (!park_enabled()) return;
  mutexLock(&g_park_m);
  seen_bump(ino);
  mutexUnlock(&g_park_m);
}
static uint32_t park_opens(uint64_t ino) {     /* under g_park_m */
  unsigned i = (unsigned)ino & (SEEN_SLOTS - 1);
  for (unsigned k = 0; k < SEEN_SLOTS && g_seen[i].ino; k++, i = (i + 1) & (SEEN_SLOTS - 1))
    if (g_seen[i].ino == ino) return g_seen[i].n;
  return 1;
}

/* 1 = parked (caller must not close it). */
static int park_put(int fd, uint64_t ino) {
  if (!park_enabled()) return 0;
  int victim = -1;
  mutexLock(&g_park_m);
  uint32_t opens = park_opens(ino);
  if (opens < 2) g_park_once++;
  if (g_park_n >= g_park_cap) {          /* the cap can shrink below n */
    int o = 0;                           /* least recently parked */
    for (int i = 1; i < g_park_n; i++) if (g_park[i].used < g_park[o].used) o = i;
    victim = g_park[o].fd;
    g_park[o] = g_park[--g_park_n];
  }
  g_park[g_park_n++] = (Parked){ fd, ino, ++g_park_clock, opens };
  g_park_parks++;
  mutexUnlock(&g_park_m);
  if (victim >= 0) close(victim);
  return 1;
}
/* Really close the parked fds of one path (ino != 0) or of all (ino == 0). */
static int park_drop(uint64_t ino) {
  if (g_park_cap <= 0) return 0;
  int fds[256], n = 0;
  mutexLock(&g_park_m);
  for (int i = 0; i < g_park_n;) {
    if (!ino || g_park[i].ino == ino) { fds[n++] = g_park[i].fd; g_park[i] = g_park[--g_park_n]; }
    else i++;
  }
  mutexUnlock(&g_park_m);
  for (int i = 0; i < n; i++) close(fds[i]);
  return n;
}
void nx_park_report(void) {
  if (g_park_cap > 0)
    debugPrintf("[io] kept-open files: %d parked of %d, %llu reopens served without the SD card, %llu parks, "
                "%llu closes of files opened only once so far\n", g_park_n, g_park_cap, (unsigned long long)g_park_hits,
                (unsigned long long)g_park_parks, (unsigned long long)g_park_once);
}

static int open_fake_impl(const char *path, int flags, int mode) {
  char _rbuf[512];
  path = casetest_redirect(path);
  path = asset_redirect(path, _rbuf, sizeof _rbuf);
  wb_wait_path(path);
  const int cvt = convert_open_flags(flags);
  const int writing = (flags & 3) != 0 || (flags & LINUX_O_CREAT);
  if (!writing) {
    // /dev/urandom + /dev/random: Switch has no /dev node, but Mono/.NET (RNG
    // seeds, Guid.NewGuid, hashtable randomization) and asset crypto open these.
    // A failing open (-1) leaves those paths without entropy and can stall the
    // scene/asset load. Materialize a buffer of real CSPRNG bytes (libnx
    // randomGet) into a file and hand back a real fd, so fstat/lseek work.
    // Reads never touch that file: read_fake serves the fd from randomGet.
    // Reading the file itself ran out after 64 KB -- ~400 s of online play,
    // as Photon draws random bytes -- and il2cpp's FillBufferWithRandomBytes
    // then loops on read() == 0 forever: thousands of fs requests a second,
    // which starved the system services on core 3 (network included) and
    // froze the game (the "~400 s online freeze", 29/10-02/11).
    if (!strcmp(path, "/dev/urandom") || !strcmp(path, "/dev/random")) {
      static char rbuf[65536];
      randomGet(rbuf, sizeof rbuf);
      char tf[256];
      snprintf(tf, sizeof tf, "%s/.synth_dev_random", cp_game_root());
      int wfd = open(tf, O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (wfd >= 0) { if (write(wfd, rbuf, sizeof rbuf) < 0) { /* best effort */ } close(wfd); }
      int rfd = open(tf, O_RDONLY);
      fd_ino_set(rfd, path);
      if (rfd >= 0 && rfd < FD_INO_MAX) g_fd_rand[rfd] = 1;
      if (TRACE_IO) debugPrintf("[io] open(%s,0x%x) -> %d [urandom]\n", path, flags, rfd);
      return rfd;
    }
    // synthetic /proc, /sys (incl. self/maps)
    int sfd = synth_proc_open(path);
    if (sfd >= 0) { fd_ino_set(sfd, path); if (TRACE_IO) debugPrintf("[io] open(%s,0x%x) -> %d [synthetic]\n", path, flags, sfd); return sfd; }
    static int phantom_on = -1;
    if (phantom_on < 0) phantom_on = nxcfg_int("io.phantom_fds", 1) == 1;
    const uint8_t *rdata; int64_t rsize;
    if (phantom_on && !(flags & LINUX_O_CREAT) && strstr(path, "assets/bin/Data/") &&
        nxio_ramfile_get(path_ino(path), &rdata, &rsize)) {
      int pfd = ph_open(rdata, rsize);
      if (pfd >= 0) {
        struct stat st;
        memset(&st, 0, sizeof st);
        st.st_mode = S_IFREG | 0444; st.st_nlink = 1; st.st_size = rsize;
        st.st_blksize = 4096; st.st_blocks = (rsize + 511) / 512;
        fd_ino_set(pfd, path);
        fd_stat_forget(pfd);
        fd_stat_remember(pfd, &st);
        static unsigned long nph;
        if (!nph++) debugPrintf("[io] phantom fds: first one for %s (fd %d)\n", path, pfd);
        return pfd;
      }
    }
  }
  /* Read-only data: the listing cache already knows whether the file exists
   * and its size (nx_statcache.c), which saves the FS round trips of a failed
   * open and of the fstat below. A write drops the directory's listing. */
  struct stat cst;
  int cached = -1;
  if (writing) { nx_statcache_invalidate(path); park_drop(path_ino(path)); bn_forget_for(path); rsc_forget(path); }
  else cached = nx_statcache_stat(path, &cst);
  const int keep = !writing && cached == 1 && S_ISREG(cst.st_mode) && !(flags & LINUX_O_CREAT);
  int fd = keep ? park_take(path_ino(path)) : -1;
  if (fd >= 0 && lseek(fd, 0, SEEK_SET) != 0) { close(fd); fd = -1; }
  if (fd < 0) fd = cached == 0 ? (errno = ENOENT, -1) : open(path, cvt, mode);
  if (fd < 0 && cached == 1) {                 /* out of descriptors / FS open limit? */
    int dropped = park_drop(0);
    if (dropped > 0) {
      fd = open(path, cvt, mode);
      if (fd >= 0 && g_park_cap > 8) {         /* it was the pool: keep it smaller from now on */
        g_park_cap = dropped > 16 ? dropped - 8 : 8;
        debugPrintf("[io] kept-open files: an open failed until %d were released -- pool now %d\n", dropped, g_park_cap);
      }
    }
  }
  if (fd < 0 && writing) {
    // save files: the target subdir may not exist yet -- create it and retry
    mkdir_parents(path);
    fd = open(path, cvt, mode);
  }
  if (fd < 0 && (flags & 3) == 0 && !(flags & LINUX_O_CREAT)) {
    char alt[320];
    if (basename_fallback(path, alt, sizeof(alt))) {
      fd = open(alt, cvt, mode);
      cached = -1;               /* cst described `path`, not `alt` */
    }
  }
  if (fd >= 0) {
    fd_ino_set(fd, path);
    if (fd < FD_INO_MAX) { g_keep_ok[fd] = keep && cached == 1; g_keep_ino[fd] = path_ino(path); }
    if (keep && cached == 1) park_note_open(path_ino(path));
    fd_stat_forget(fd);          /* a reused fd number must not inherit a size */
    if (writing) {
      if (wb_attach(fd, flags)) g_write_fd[fd] = 1;   /* the write-behind thread commits it */
      else mark_write_fd(fd);
    }
    struct stat _st;
    int have = 0;
    if (cached == 1 && S_ISREG(cst.st_mode)) { _st = cst; have = 1; fd_stat_remember(fd, &cst); }
    else have = fstat(fd, &_st) == 0;
    if (have) {
      if (TRACE_IO) debugPrintf("[io] open(%s,0x%x) -> %d size=%lld\n", path, flags, fd, (long long)_st.st_size);
      /* Read-only game files go through the block cache (nx_io.c); a path
       * opened for writing drops whatever was cached for it. With
       * [io] cache = 0, big files get the legacy 1 MB read-ahead window. */
      if (writing)
        nxio_invalidate(path_ino(path));
      else if (nxio_enabled()) {
        if (S_ISREG(_st.st_mode) && _st.st_size > 0) nxio_open(fd, path_ino(path), path, (int64_t)_st.st_size);
      } else if (_st.st_size >= (4 << 20))
        ra_attach(fd, (long)_st.st_size);
    } else {
      if (TRACE_IO) debugPrintf("[io] open(%s,0x%x) -> %d size=?\n", path, flags, fd);
    }
  } else {
    if (TRACE_IO) debugPrintf("[io] open(%s,0x%x) -> %d\n", path, flags, fd);
  }
  return fd;
}
/* libnx's fsdev_open asks the FS for the file's timestamps after every open,
 * only to answer a later fstat's st_mtime: one more SD round trip per open,
 * 27 % of the 2.1 s first Room Settings (02/11 profile). Game data never needs
 * them: answered "no timestamp" without asking. [io] open_timestamps = 1
 * restores the request. */
Result __real_fsFsGetFileTimeStampRaw(FsFileSystem *fs, const char *path, FsTimeStampRaw *out);
Result __wrap_fsFsGetFileTimeStampRaw(FsFileSystem *fs, const char *path, FsTimeStampRaw *out) {
  if (path && out && (strstr(path, "/assets/bin/Data/") || strstr(path, "/il2cpp/")) &&
      nxcfg_int("io.open_timestamps", 0) == 0) {
    memset(out, 0, sizeof *out);
    return 0;
  }
  return __real_fsFsGetFileTimeStampRaw(fs, path, out);
}

/* [io] preload_kb = N (default 1024, 0 = off): from the main menu on, a
 * low-priority thread reads every assets/bin/Data file up to N KB into memory
 * (nx_io.c RAM files, ~40 MB at 1024), smallest first. The first Room Settings, match start
 * and power-ups otherwise open and read hundreds of those files one by one
 * on the game's threads (78 % of the 2.1 s Room Settings frame was FS time). */
extern int diag_frame_no(void);
typedef struct { int64_t size; char name[64]; } PreEnt;
static int pre_cmp(const void *a, const void *b) {
  int64_t x = ((const PreEnt *)a)->size, y = ((const PreEnt *)b)->size;
  return x < y ? -1 : x > y;
}
static void preload_main(void *arg) {
  (void)arg;
  static uint8_t tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  install_bionic_tls(tls);
  const int64_t limit = (int64_t)nxcfg_int("io.preload_kb", 1024) * 1024;
  while (diag_frame_no() < 300) svcSleepThread(500000000ull);
  svcSleepThread(2000000000ull);              /* let the main menu settle */
  const char *root = cp_game_root();          /* "sdmc:/switch/mvlo" */
  const char *fsroot = strchr(root, ':') ? strchr(root, ':') + 1 : root;
  char dirp[FS_MAX_PATH];
  snprintf(dirp, sizeof dirp, "%s/assets/bin/Data", fsroot);
  FsFileSystem *sd = fsdevGetDeviceFileSystem("sdmc");
  FsDir dir;
  if (!sd || R_FAILED(fsFsOpenDirectory(sd, dirp, FsDirOpenMode_ReadFiles, &dir))) {
    debugPrintf("[io] preload: cannot list %s\n", dirp);
    return;
  }
  enum { MAXE = 4096 };
  FsDirectoryEntry *de = malloc(256 * sizeof *de);
  PreEnt *pe = malloc(MAXE * sizeof *pe);
  int n = 0;
  s64 got = 0;
  while (de && pe && R_SUCCEEDED(fsDirRead(&dir, &got, 256, de)) && got > 0)
    for (s64 i = 0; i < got && n < MAXE; i++)
      if (de[i].type == FsDirEntryType_File && de[i].file_size > 0 && de[i].file_size <= limit &&
          strlen(de[i].name) < sizeof pe[n].name) {
        pe[n].size = de[i].file_size;
        snprintf(pe[n].name, sizeof pe[n].name, "%s", de[i].name);
        n++;
      }
  fsDirClose(&dir);
  free(de);
  if (!pe) return;
  qsort(pe, (size_t)n, sizeof *pe, pre_cmp);
  int64_t want = 0;
  for (int i = 0; i < n; i++) want += pe[i].size;
  debugPrintf("[io] preload: %d data files up to %lld KB (%.1f MB) into memory\n", n,
              (long long)(limit >> 10), want / 1048576.0);
  uint64_t t0 = armTicksToNs(armGetSystemTick());
  int64_t sd_bytes = 0;
  int done = 0;
  for (int i = 0; i < n; i++) {
    char path[320], fsp[FS_MAX_PATH + 80];
    snprintf(path, sizeof path, "%s/assets/bin/Data/%s", root, pe[i].name);
    snprintf(fsp, sizeof fsp, "%s/%s", dirp, pe[i].name);
    uint8_t *buf = malloc((size_t)pe[i].size);
    FsFile f;
    if (!buf || R_FAILED(fsFsOpenFile(sd, fsp, FsOpenMode_Read, &f))) { free(buf); continue; }
    u64 got = 0;
    Result rc = fsFileRead(&f, 0, buf, (u64)pe[i].size, FsReadOption_None, &got);
    fsFileClose(&f);
    if (R_FAILED(rc) || got != (u64)pe[i].size || !nxio_ramfile_add(path_ino(path), buf, pe[i].size)) {
      free(buf);
      continue;
    }
    sd_bytes += pe[i].size;
    done++;
  }
  free(pe);
  debugPrintf("[io] preload: %d files, %.1f MB read in %.1f s\n", done, sd_bytes / 1048576.0,
              (armTicksToNs(armGetSystemTick()) - t0) / 1e9);
}
void nx_preload_start(void) {
  if (nxcfg_int("io.preload_kb", 1024) <= 0 || !nxio_enabled()) return;
  static Thread th;
  if (R_SUCCEEDED(threadCreate(&th, preload_main, NULL, NULL, 0x8000, 0x3B, -2)) && R_SUCCEEDED(threadStart(&th)))
    debugPrintf("[io] preload armed: Data files up to %d KB, from the main menu\n", nxcfg_int("io.preload_kb", 1024));
}

int open_fake(const char *path, int flags, ...) {
  int mode = 0666;
  if (flags & LINUX_O_CREAT) { va_list va; va_start(va, flags); mode = va_arg(va, int); va_end(va); }
  uint64_t t0 = fs_begin("open", path);
  int fd = open_fake_impl(path, flags, mode);
  fs_end(t0, 0, "open", path);
  if (fd >= 0) fd_path_set(fd, path);
  if (fd >= 0 && fd < FD_INO_MAX && !g_open_mark[fd]) { g_open_mark[fd] = 1; __atomic_add_fetch(&g_files_open, 1, __ATOMIC_RELAXED); }
  return fd;
}
int openat_fake(int dirfd, const char *path, int flags, ...) {
  (void)dirfd;
  int mode = 0666;
  if (flags & LINUX_O_CREAT) { va_list va; va_start(va, flags); mode = va_arg(va, int); va_end(va); }
  // Delegate to open_fake so /dev/urandom, synthetic /proc + /sys, save-dir
  // creation and basename fallback all apply (some libc paths route open->openat).
  return open_fake(path, flags, mode);
}
int unlinkat_fake(int dirfd, const char *path, int flags) { (void)dirfd; (void)flags; return nx_unlink(path); }

// ---------------------------------------------------------------------------
// struct stat conversion (bionic aarch64 layout)
// ---------------------------------------------------------------------------

struct bionic_timespec { int64_t tv_sec; int64_t tv_nsec; };
struct bionic_stat {
  uint64_t st_dev; uint64_t st_ino; uint32_t st_mode; uint32_t st_nlink;
  uint32_t st_uid; uint32_t st_gid; uint64_t st_rdev; uint64_t __pad1;
  int64_t st_size; int32_t st_blksize; int32_t __pad2; int64_t st_blocks;
  struct bionic_timespec st_atim; struct bionic_timespec st_mtim; struct bionic_timespec st_ctim;
  uint32_t __unused4; uint32_t __unused5;
};

static void convert_stat(const struct stat *in, struct bionic_stat *out) {
  memset(out, 0, sizeof(*out));
  out->st_dev = in->st_dev; out->st_ino = in->st_ino; out->st_mode = in->st_mode;
  out->st_nlink = in->st_nlink; out->st_uid = in->st_uid; out->st_gid = in->st_gid;
  out->st_rdev = in->st_rdev; out->st_size = in->st_size; out->st_blksize = in->st_blksize;
  out->st_blocks = in->st_blocks;
  out->st_atim.tv_sec = in->st_atime; out->st_mtim.tv_sec = in->st_mtime; out->st_ctim.tv_sec = in->st_ctime;
}

/* POSIX rename() REPLACES an existing destination. Horizon's fsFsRenameFile does NOT -- it
 * fails if the destination exists, and libnx's fsdev passes that straight through. Worse,
 * Horizon will not rename OR delete a file that currently has an open handle.
 *
 * Bad Piggies saves via SettingsData.TransactionalFileWrite:
 *     write Progress.dat.tmp
 *     if (File.Exists(Progress.dat)) File.Replace(tmp, Progress.dat, Progress.dat.bak);
 *     else                           File.Move   (tmp, Progress.dat);
 * and il2cpp's native ReplaceFile does:
 *     fd = open(backup, O_RDONLY);          <-- keeps .bak OPEN across the next call
 *     rename(dest -> backup);
 *     rename(source -> dest);
 *     close(fd);
 *
 * So on Switch that first rename hits a destination which (a) already exists and (b) is held
 * open by the caller itself. It cannot be renamed away and it cannot be deleted -- which is
 * why every save silently jammed, leaving the real save stranded in Progress.dat.tmp while
 * the game kept loading a stale Progress.dat. (The game try/catches its save, hence no
 * managed exception in the log.) Settings.xml goes down the identical path.
 *
 * The fix therefore must never touch the destination's DIRECTORY ENTRY -- only its CONTENTS.
 * We try, in order:
 *   1. plain rename                    (destination absent: nothing to do)
 *   2. park the destination aside, rename, drop the parked copy, roll back on failure
 *      (correct and near-atomic; works whenever the destination is not held open)
 *   3. copy the source's BYTES over the destination and unlink the source
 *      (the only thing that works when the destination has an open handle -- writing to an
 *       open file is allowed, renaming/deleting it is not)
 * Step 2 is kept ahead of step 3 because it is atomic and never leaves a half-written file;
 * step 3 is the fallback that actually rescues this game's save. We deliberately read the
 * source fully BEFORE truncating the destination, so a failure to read cannot destroy it. */
int rename_fake(const char *oldp, const char *newp) {
  char rb1[512], rb2[512];
  oldp = asset_redirect(oldp, rb1, sizeof rb1);
  newp = asset_redirect(newp, rb2, sizeof rb2);
  wb_wait_path(oldp);
  wb_wait_path(newp);
  rsc_forget(oldp);
  rsc_forget(newp);
  nxio_invalidate(path_ino(oldp));
  nxio_invalidate(path_ino(newp));
  bn_forget_for(newp);

  if (rename(oldp, newp) == 0) return 0;          /* 1. destination absent: plain rename */

  struct stat st;
  if (stat(newp, &st) != 0) {                     /* dest really is absent -> genuine error */
    if (TRACE_IO) debugPrintf("[io] rename(%s -> %s) FAILED, dest absent (errno %d)\n", oldp, newp, errno);
    return -1;
  }

  /* 2. park the destination aside */
  char aside[576];
  snprintf(aside, sizeof aside, "%s.rnold", newp);
  remove(aside);                                  /* stale one from an interrupted swap */
  if (rename(newp, aside) == 0) {
    if (rename(oldp, newp) == 0) {
      remove(aside);
      if (TRACE_IO) debugPrintf("[io] rename(%s -> %s) ok (displaced existing)\n", oldp, newp);
      return 0;
    }
    rename(aside, newp);                          /* roll back: restore the old file */
    if (TRACE_IO) debugPrintf("[io] rename(%s -> %s) FAILED, original restored (errno %d)\n", oldp, newp, errno);
    return -1;
  }

  /* 2b. The destination is held OPEN by the caller, and Horizon will not rename it, delete it,
   *     or even open it for writing (EIO) while a handle is live. il2cpp's native ReplaceFile is
   *     literally holding the file we have to replace:
   *         fd = open(backup, O_RDONLY);   rename(dest -> backup);   ... ;   close(fd);
   *     Nothing can be done to that file until the handle is gone -- so close it ourselves.
   *
   *     This is safe: the handle is a pure existence probe. ReplaceFile never reads a byte from
   *     it; its only other use is the close() afterwards, which on a stale fd returns EBADF and
   *     is ignored. To stop that fd NUMBER being handed to another thread in the gap, we re-open
   *     the destination right after the swap -- fds are allocated lowest-free, so we normally get
   *     the same number straight back and the caller's close() then closes our handle, which is
   *     exactly what it meant to do. */
  int victim = fd_close_by_path(newp);
  wb_wait_path(newp);                           /* if that close queued a write-behind */
  if (victim >= 0) {
    int ok = 0;
    if (rename(newp, aside) == 0) {
      if (rename(oldp, newp) == 0) { remove(aside); ok = 1; }
      else rename(aside, newp);                 /* roll back */
    }
    int re = open(newp, O_RDONLY);              /* re-occupy the fd number for the caller */
    if (re >= 0) {
      if (re == victim) fd_ino_set(re, newp);   /* caller's close() will close this */
      else close(re);
    }
    if (ok) {
      if (TRACE_IO) debugPrintf("[io] rename(%s -> %s) ok (closed caller's handle, then displaced)\n", oldp, newp);
      return 0;
    }
  }

  /* 3. still stuck -> overwrite the destination's contents instead of its directory entry.
   *    Read the source completely first; only then truncate the destination. */
  int fsrc = open(oldp, O_RDONLY);
  if (fsrc < 0) {
    if (TRACE_IO) debugPrintf("[io] rename(%s -> %s) FAILED, cannot read source (errno %d)\n", oldp, newp, errno);
    return -1;
  }
  struct stat ss;
  if (fstat(fsrc, &ss) != 0 || ss.st_size < 0) { close(fsrc); return -1; }
  size_t n = (size_t)ss.st_size;
  unsigned char *buf = (unsigned char *)malloc(n ? n : 1);
  if (!buf) { close(fsrc); return -1; }
  size_t got = 0;
  while (got < n) {
    ssize_t r = read(fsrc, buf + got, n - got);
    if (r <= 0) break;
    got += (size_t)r;
  }
  close(fsrc);
  if (got != n) {
    free(buf);
    if (TRACE_IO) debugPrintf("[io] rename(%s -> %s) FAILED, short read of source (%zu/%zu)\n", oldp, newp, got, n);
    return -1;                                    /* destination untouched */
  }

  int fdst = open(newp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fdst < 0) {
    free(buf);
    if (TRACE_IO) debugPrintf("[io] rename(%s -> %s) FAILED, cannot write dest (errno %d)\n", oldp, newp, errno);
    return -1;
  }
  size_t put = 0;
  while (put < n) {
    ssize_t w = write(fdst, buf + put, n - put);
    if (w <= 0) break;
    put += (size_t)w;
  }
  fsync(fdst);
  close(fdst);
  free(buf);
  if (put != n) {
    if (TRACE_IO) debugPrintf("[io] rename(%s -> %s) FAILED, short write of dest (%zu/%zu)\n", oldp, newp, put, n);
    return -1;
  }

  /* The destination now holds the source's bytes; rename() also means the source is gone. */
  if (remove(oldp) != 0)
    if (TRACE_IO) debugPrintf("[io] rename: warning, could not unlink source %s (errno %d)\n", oldp, errno);
  if (TRACE_IO) debugPrintf("[io] rename(%s -> %s) ok (%zu bytes copied over open dest)\n", oldp, newp, n);
  return 0;
}

static int stat_fake_impl(const char *path, struct bionic_stat *st);
int stat_fake(const char *path, struct bionic_stat *st) {
  wb_wait_path(path);
  uint64_t t0 = fs_begin("stat", path);
  int r = stat_fake_impl(path, st);
  fs_end(t0, 1, "stat", path);
  return r;
}
/* stat() through the directory listing cache when it covers the path
 * (nx_statcache.c), the real 5-round-trip fsdev stat otherwise. */
static int stat_cached(const char *path, struct stat *real) {
  int c = nx_statcache_stat(path, real);
  if (c < 0) return stat(path, real);
  if (c == 0) { errno = ENOENT; return -1; }
  return 0;
}
static int stat_fake_impl(const char *path, struct bionic_stat *st) {
  char _rbuf[512];
  path = asset_redirect(path, _rbuf, sizeof _rbuf);
  struct stat real;
  int r;
  if (rsc_get(path, &real)) r = 0;                 /* replays folder, already asked */
  else {
    uint64_t t = fs_now();
    if ((r = stat_cached(path, &real)) == 0) rsc_put(path, &real);
    if (rsc_on(path)) rsc_note_miss(path, fs_now() - t, r != 0);
  }
  if (r != 0) {
    char alt[320];
    if (basename_fallback(path, alt, sizeof(alt))) r = stat_cached(alt, &real);
  }
  if (r == 0) {
    convert_stat(&real, st);
    if (st->st_ino == 0) st->st_ino = path_ino(path);   // fsdev gives 0 -> synth
  }
  return r;
}
int fstat_fake(int fd, struct bionic_stat *st) {
  uint64_t t0 = fs_begin("fstat", NULL);
  struct stat real;
  int r;
  if (fd >= 0 && fd < WFD_MAX && g_wb[fd]) {      /* write-behind: a new file, all in the buffer */
    memset(&real, 0, sizeof real);
    real.st_mode = S_IFREG | 0666; real.st_nlink = 1; real.st_blksize = 4096;
    real.st_size = g_wb[fd]->start + (int64_t)g_wb[fd]->len;
    real.st_blocks = (real.st_size + 511) / 512;
    real.st_mtime = real.st_ctime = real.st_atime = time(NULL);
    r = 0;
  } else if (fd_stat_lookup(fd, &real)) r = 0;
  else {
    r = fstat(fd, &real);
    static int nlog;
    if (nlog < 40) { nlog++; debugPrintf("[io] fstat not cached: fd %d = %s\n", fd, fd_path(fd)); }
  }
  fs_end(t0, 2, "fstat", NULL);
  if (r == 0) {
    convert_stat(&real, st);
    if (st->st_ino == 0) {                               // mirror stat(path)'s inode
      uint64_t ino = (fd >= 0 && fd < FD_INO_MAX) ? g_fd_ino[fd] : 0;
      st->st_ino = ino ? ino : ((uint64_t)(fd + 1) * 2654435761ULL) | 1;
    }
  }
  return r;
}
int lstat_fake(const char *path, struct bionic_stat *st) { return stat_fake(path, st); }

// ---------------------------------------------------------------------------
// dirent conversion (bionic dirent64 layout)
// ---------------------------------------------------------------------------

struct bionic_dirent {
  uint64_t d_ino; int64_t d_off; uint16_t d_reclen; uint8_t d_type; char d_name[256];
};

void *readdir_fake(void *dirp) {
  static struct bionic_dirent out; // not thread-safe (matches bionic readdir)
  struct dirent *e = readdir((DIR *)dirp);
  if (!e) return NULL;
  memset(&out, 0, sizeof(out));
  out.d_ino = e->d_ino;
  out.d_reclen = sizeof(out);
  out.d_type = e->d_type;
  snprintf(out.d_name, sizeof(out.d_name), "%s", e->d_name);
  return &out;
}

// ---------------------------------------------------------------------------
// locale: ignore the locale argument and use the C-locale versions
// ---------------------------------------------------------------------------

void *newlocale_fake(int mask, const char *locale, void *base) { (void)mask; (void)locale; (void)base; return (void *)1; }
void freelocale_fake(void *loc) { (void)loc; }
void *uselocale_fake(void *loc) { (void)loc; return (void *)1; }

#define WRAP_ISW_L(fn) int fn##_l_fake(int wc, void *loc) { (void)loc; return fn(wc); }
WRAP_ISW_L(iswalpha) WRAP_ISW_L(iswblank) WRAP_ISW_L(iswcntrl) WRAP_ISW_L(iswdigit)
WRAP_ISW_L(iswlower) WRAP_ISW_L(iswprint) WRAP_ISW_L(iswpunct) WRAP_ISW_L(iswspace)
WRAP_ISW_L(iswupper) WRAP_ISW_L(iswxdigit) WRAP_ISW_L(towlower) WRAP_ISW_L(towupper)

int strcoll_l_fake(const char *a, const char *b, void *loc) { (void)loc; return strcoll(a, b); }
size_t strxfrm_l_fake(char *dst, const char *src, size_t n, void *loc) { (void)loc; return strxfrm(dst, src, n); }
size_t strftime_l_fake(char *s, size_t max, const char *fmt, const void *tm, void *loc) { (void)loc; return strftime(s, max, fmt, (const struct tm *)tm); }
long double strtold_l_fake(const char *s, char **end, void *loc) { (void)loc; return strtold(s, end); }
long long strtoll_l_fake(const char *s, char **end, int base, void *loc) { (void)loc; return strtoll(s, end, base); }
unsigned long long strtoull_l_fake(const char *s, char **end, int base, void *loc) { (void)loc; return strtoull(s, end, base); }
int wcscoll_l_fake(const wchar_t *a, const wchar_t *b, void *loc) { (void)loc; return wcscoll(a, b); }
size_t wcsxfrm_l_fake(wchar_t *dst, const wchar_t *src, size_t n, void *loc) { (void)loc; return wcsxfrm(dst, src, n); }

size_t mbsnrtowcs_fake(wchar_t *dst, const char **src, size_t nms, size_t len, void *ps) {
  (void)ps;
  size_t i = 0; const char *s = *src;
  while (i < nms && s[i] && (!dst || i < len)) { if (dst) dst[i] = (unsigned char)s[i]; i++; }
  if (dst && i < len) { dst[i] = 0; *src = NULL; }
  return i;
}
size_t wcsnrtombs_fake(char *dst, const wchar_t **src, size_t nwc, size_t len, void *ps) {
  (void)ps;
  size_t i = 0; const wchar_t *s = *src;
  while (i < nwc && s[i] && (!dst || i < len)) { if (dst) dst[i] = (char)s[i]; i++; }
  if (dst && i < len) { dst[i] = 0; *src = NULL; }
  return i;
}

// ---------------------------------------------------------------------------
// memory
// ---------------------------------------------------------------------------

int posix_memalign_fake(void **out, size_t align, size_t size) {
  void *p = memalign(align, size);
  if (!p) return ENOMEM;
  *out = p;
  return 0;
}

// --- anonymous mmap arena (page-granular; supports sub-range munmap) ----------
//
// Switch has no mmap. Unity reserves big *256MB-aligned* pools by over-mmapping a
// larger region then munmapping the unaligned head/tail to keep an aligned middle.
// A plain malloc/free-per-mmap frees the WHOLE block when the head is trimmed (the
// trim's addr == the registered base) and the kept aligned middle is then reused
// out from under the engine -> the TLSF allocator's free block reads back zeroed
// (next_free == NULL) and faults. So we manage a dedicated arena (carved 256MB-
// aligned in __libnx_initheap) with a per-page used-bitmap: mmap = find a free run
// of pages and mark them; munmap = clear exactly the pages of the sub-range. Big
// requests are handed back 256MB-aligned so Unity only ever trims the tail.
// File-backed maps (Unity streams BGM/SE this way) are served from the same arena.
// ------------------------------------------------------------------------------
extern void  *g_mmap_arena_base;   // set by __libnx_initheap (main.c)
extern size_t g_mmap_arena_size;
extern int    g_overcommit;        // 1 = alias-region on-demand commit
extern u64    g_alias_base, g_alias_size;

#define BIONIC_MAP_ANONYMOUS 0x20
#define MMAP_PAGE       0x1000u
#define MMAP_BIG_ALIGN  MMAP_ARENA_ALIGN
#define MMAP_BIG_THRESH ((size_t)64 * 1024 * 1024)
#define BIONIC_PROT_NONE 0x0
#define BIONIC_PROT_WRITE 0x2
#define BIONIC_MADV_DONTNEED 4

static uint8_t *mmap_arena;    // 256MB-aligned usable base (published last)
static size_t   mmap_usable;   // usable bytes
static size_t   mmap_pages;    // usable / page
static uint8_t *mmap_used;     // 1 byte/page bitmap: reserved (address space)
static uint8_t *mmap_committed;// 1 byte/page bitmap: physically committed (overcommit only)
static size_t   g_committed_pages;   // running count of committed pages
static size_t   g_commit_peak;       // high-water mark (pages)
static Mutex    g_mmap_lock;   // zero-init == valid unlocked libnx mutex

// --- overcommit commit/decommit (caller holds g_mmap_lock) -------------------
// svcMapPhysicalMemory zero-fills and draws from the freed physical limit; it
// FAILS on already-mapped pages, so we only ever commit pages we track as
// uncommitted, in contiguous runs. Out-of-physical is logged, not fatal.
static void arena_commit_locked(size_t first, size_t cnt) {
  size_t i = 0;
  while (i < cnt) {
    if (mmap_committed[first + i]) { i++; continue; }
    size_t run = 0;
    while (i + run < cnt && !mmap_committed[first + i + run]) run++;
    u64 a = (u64)(uintptr_t)(mmap_arena + (first + i) * MMAP_PAGE);
    Result rc = svcMapPhysicalMemory((void *)a, (u64)run * MMAP_PAGE);
    if (R_SUCCEEDED(rc)) {
      for (size_t k = 0; k < run; k++) mmap_committed[first + i + k] = 1;
      g_committed_pages += run;
      if (g_committed_pages > g_commit_peak) {
        size_t prev = g_commit_peak;
        g_commit_peak = g_committed_pages;
        if ((g_commit_peak >> 16) != (prev >> 16))   // new 256MB high-water mark
          if (TRACE_MMAP) debugPrintf("[mmap] committed peak %u MB (live %u MB)\n",
                      (unsigned)((g_commit_peak * MMAP_PAGE) >> 20),
                      (unsigned)((g_committed_pages * MMAP_PAGE) >> 20));
      }
    } else {
      if (TRACE_MMAP) debugPrintf("[mmap] COMMIT FAIL %u KB @ 0x%lx rc=0x%x (committed %u MB peak %u MB)\n",
                  (unsigned)((run * MMAP_PAGE) >> 10), (unsigned long)a, rc,
                  (unsigned)((g_committed_pages * MMAP_PAGE) >> 20),
                  (unsigned)((g_commit_peak * MMAP_PAGE) >> 20));
    }
    i += run ? run : 1;
  }
}

static void arena_decommit_locked(size_t first, size_t cnt) {
  size_t i = 0;
  while (i < cnt) {
    if (!mmap_committed[first + i]) { i++; continue; }
    size_t run = 0;
    while (i + run < cnt && mmap_committed[first + i + run]) run++;
    u64 a = (u64)(uintptr_t)(mmap_arena + (first + i) * MMAP_PAGE);
    if (R_SUCCEEDED(svcUnmapPhysicalMemory((void *)a, (u64)run * MMAP_PAGE))) {
      for (size_t k = 0; k < run; k++) mmap_committed[first + i + k] = 0;
      g_committed_pages -= run;
    }
    i += run ? run : 1;
  }
}

// translate [addr,addr+len) to a clamped page range within the arena; returns 0 if
// outside the arena (e.g. a newlib-fallback pointer), else 1 with *first/*cnt set.
static int arena_page_range(void *addr, size_t len, size_t *first, size_t *cnt) {
  if (!mmap_arena || (uint8_t *)addr < mmap_arena) return 0;
  size_t off = (uint8_t *)addr - mmap_arena;
  if (off >= mmap_usable) return 0;
  size_t f = off / MMAP_PAGE;
  size_t c = (len + MMAP_PAGE - 1) / MMAP_PAGE;
  if (f + c > mmap_pages) c = mmap_pages - f;
  *first = f; *cnt = c;
  return 1;
}

// commit [addr,len) on demand (mprotect RW / anon mmap). no-op if not overcommit.
static void arena_commit_range(void *addr, size_t len) {
  if (!g_overcommit) return;
  size_t first, cnt;
  mutexLock(&g_mmap_lock);
  if (arena_page_range(addr, len, &first, &cnt)) arena_commit_locked(first, cnt);
  mutexUnlock(&g_mmap_lock);
}

// decommit [addr,len) (mprotect PROT_NONE / munmap). reclaims physical. Safe
// because re-use of a decommitted page goes through mprotect(RW) -> recommit.
static void arena_decommit_range(void *addr, size_t len) {
  if (!g_overcommit) return;
  size_t first, cnt;
  mutexLock(&g_mmap_lock);
  if (arena_page_range(addr, len, &first, &cnt)) arena_decommit_locked(first, cnt);
  mutexUnlock(&g_mmap_lock);
}

// madvise(MADV_DONTNEED): zero the committed pages but KEEP them committed. The
// Switch has no fault handler, so decommitting here would crash if the engine
// re-touches without an intervening mprotect(RW) (allowed on Linux). Zeroing
// preserves the "reads back as zero after DONTNEED" contract safely.
static void arena_dontneed_range(void *addr, size_t len) {
  if (!g_overcommit) return;
  size_t first, cnt;
  mutexLock(&g_mmap_lock);
  if (arena_page_range(addr, len, &first, &cnt)) {
    for (size_t i = 0; i < cnt; ) {
      if (!mmap_committed[first + i]) { i++; continue; }
      size_t run = 0;
      while (i + run < cnt && mmap_committed[first + i + run]) run++;
      memset(mmap_arena + (first + i) * MMAP_PAGE, 0, run * MMAP_PAGE);
      i += run;
    }
  }
  mutexUnlock(&g_mmap_lock);
}

// ===========================================================================
// Stack-region overcommit (OC) arena.
// Boot probe established: svcMapMemory can alias heap pages into the STACK
// region (the alias region is rejected, kernel err 0xdc01), and Unity reserves
// ~2.8GB of PROT_NONE blocks while committing only ~80MB via mprotect(RW) with
// ZERO decommits. So we satisfy the big PROT_NONE reservations from a cheap
// stack-region address window and alias a small bump-allocated heap commit-pool
// in on mprotect(RW). Tried BEFORE the heap-backed arena for big anon PROT_NONE
// maps; anything else (and overflow when the window fills) falls through to the
// heap-backed arena, so if OC setup fails the engine runs exactly as before.
// Because decommits are never observed, the pool is a no-reclaim bump allocator.
// ===========================================================================
static uint8_t *oc_base;        // stack-region window base (256MB-aligned)
static size_t   oc_pages;       // window size in pages (0 => OC disabled)
static uint8_t *oc_used;        // 1/page: address space reserved by an mmap
static uint8_t *oc_committed;   // 1/page: physically backed via svcMapMemory
static uint8_t *oc_pool;        // commit-pool base (heap, page-aligned)
static size_t   oc_pool_pages;  // pool capacity in pages
static size_t   oc_pool_bump;   // next free pool page (bump; no reclaim)
static size_t   oc_pool_spent;  // pages of earlier pool chunks, all used up
static size_t   oc_live_pages;  // committed pages (diagnostic)
// [mem] heartbeat (cloverpit_boot.c): committed / pool used / pool size, in MB
void oc_stats_mb(unsigned *committed, unsigned *pool_used, unsigned *pool) {
  *committed = (unsigned)((oc_live_pages * 0x1000) >> 20);
  *pool_used = (unsigned)(((oc_pool_spent + oc_pool_bump) * 0x1000) >> 20);
  *pool      = (unsigned)(((oc_pool_spent + oc_pool_pages) * 0x1000) >> 20);
}
// 1/page: start page + 1 of the reservation that owns it (OC_FOREIGN = a span
// mapped by someone else). munmap only releases pages of the reservation it
// starts in, so a trim that runs past a short reservation (see oc_alloc_locked)
// can never release a neighbour's or a foreign span's pages.
static uint32_t *oc_owner;
#define OC_FOREIGN 0xffffffffu

// Called once from main() after the newlib heap exists. window = a reserved
// stack-region range; pool = a heap buffer. Returns 1 if OC is armed.
int oc_arena_init(void *window, size_t window_bytes, void *pool, size_t pool_bytes) {
  if (!window || !pool || !window_bytes || !pool_bytes) return 0;
  size_t wp = window_bytes / MMAP_PAGE;
  uint8_t *u = (uint8_t *)calloc(wp, 1);
  uint8_t *c = (uint8_t *)calloc(wp, 1);
  uint32_t *o = (uint32_t *)calloc(wp, sizeof *o);
  if (!u || !c || !o) { free(u); free(c); free(o); return 0; }
  mutexLock(&g_mmap_lock);
  oc_base = (uint8_t *)window; oc_pages = wp; oc_used = u; oc_committed = c; oc_owner = o;
  oc_pool = (uint8_t *)pool; oc_pool_pages = pool_bytes / MMAP_PAGE;
  oc_pool_bump = 0; oc_pool_spent = 0; oc_live_pages = 0;
  mutexUnlock(&g_mmap_lock);
  return 1;
}

static int oc_range_occupied(size_t i, size_t need);
// The window spans every hole of the stack region, so it also covers what is
// already mapped between them (module images, thread stacks): mark those spans
// foreign up front. Returns the free MB left in the window.
unsigned oc_arena_mark_foreign(void) {
  mutexLock(&g_mmap_lock);
  oc_range_occupied(0, oc_pages);
  size_t freep = 0;
  for (size_t k = 0; k < oc_pages; k++) freep += !oc_used[k];
  mutexUnlock(&g_mmap_lock);
  return (unsigned)((freep * MMAP_PAGE) >> 20);
}

static int oc_contains(void *addr) {
  return oc_pages && (uint8_t *)addr >= oc_base &&
         (uint8_t *)addr < oc_base + oc_pages * MMAP_PAGE;
}

// A thread stack (e.g. an audio worker's) can get mapped by libnx INSIDE the OC
// window after arm time -- the boot hole-scan can't see stacks created later, and
// the virtmem reservation on the window doesn't reliably deflect them. Reserving
// such a range and later svcMapMemory'ing over it fails (0xd401 InvalidCurrentMemory)
// and leaves Unity's allocation unbacked -> the engine faults. So before handing out
// a candidate, confirm every page is still Unmapped; if a mapped span is found, mark
// its OC pages used so the scan permanently routes around it. caller holds g_mmap_lock.
static int oc_range_occupied(size_t i, size_t need) {
  uint64_t a   = (uint64_t)(uintptr_t)(oc_base + i * MMAP_PAGE);
  uint64_t end = a + (uint64_t)need * MMAP_PAGE;
  int occ = 0;
  while (a < end) {
    MemoryInfo mi; u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, a))) { occ = 1; break; }
    uint64_t span_end = mi.addr + mi.size;
    if (span_end <= a) { occ = 1; break; }
    if (mi.type != MemType_Unmapped) {
      occ = 1;
      uint64_t s = mi.addr > (uint64_t)(uintptr_t)oc_base ? mi.addr
                                                          : (uint64_t)(uintptr_t)oc_base;
      size_t p0 = (size_t)((s - (uint64_t)(uintptr_t)oc_base) / MMAP_PAGE);
      size_t p1 = (size_t)((span_end - (uint64_t)(uintptr_t)oc_base + MMAP_PAGE - 1) / MMAP_PAGE);
      for (size_t k = p0; k < p1 && k < oc_pages; k++) { oc_used[k] = 1; oc_owner[k] = OC_FOREIGN; }
      debugPrintf("[oc] window range 0x%llx has mapped span 0x%llx..0x%llx (type=0x%x) -> routing around\n",
                  (unsigned long long)s, (unsigned long long)mi.addr,
                  (unsigned long long)span_end, mi.type);
    }
    a = span_end;
  }
  return occ;
}

// Reserve address space in the OC window. Unity over-maps (block + alignment)
// and trims the unaligned tail, so each reservation really keeps one
// MMAP_BIG_ALIGN block. Pass 1 wants the whole over-map free; pass 2 accepts a
// shorter free run that still holds the kept block (before a foreign span, a
// neighbour, or the window end): the trim past it is a no-op thanks to
// oc_owner. caller holds g_mmap_lock.
static void *oc_take_locked(size_t i, size_t n, size_t *got) {
  for (size_t k = 0; k < n; k++) { oc_used[i + k] = 1; oc_owner[i + k] = (uint32_t)i + 1; }
  *got = n * MMAP_PAGE; return oc_base + i * MMAP_PAGE;
}
static void *oc_alloc_locked(size_t len, size_t *got) {
  *got = 0;
  if (!oc_pages) return NULL;
  size_t need = (len + MMAP_PAGE - 1) / MMAP_PAGE; if (!need) need = 1;
  const size_t step = MMAP_BIG_ALIGN / MMAP_PAGE;
  // over-map = block + align - 1 page (131068 KB for a 64 MB block)
  size_t kept = need > step ? need - step + 1 : need;
  for (int pass = 0; pass < 2; pass++)
    for (size_t i = 0; i + kept <= oc_pages; i += step) {
      size_t lim = need < oc_pages - i ? need : oc_pages - i;
      size_t run = 0; while (run < lim && !oc_used[i + run]) run++;
      if (pass == 0 ? run < need : run < kept) continue;
      if (oc_range_occupied(i, run)) continue;   // a thread stack landed here -> skip
      return oc_take_locked(i, run, got);
    }
  return NULL;
}

// The pool is sized once from mvlo.ini. Past it, mprotect has already told Unity
// yes, so its first write to an unbacked page would fault: take another chunk
// from newlib instead (straight from newlib -- the wrapped memalign would put a
// 32 MB page-aligned block in the GPU arena). The rest of the old chunk is used
// up first, so nothing is lost. caller holds g_mmap_lock.
#define OC_POOL_GROW_PAGES ((size_t)32 * 1024 * 1024 / MMAP_PAGE)
static int oc_pool_grow_locked(size_t want) {
  extern void *__real_memalign(size_t, size_t);
  size_t pages = want > OC_POOL_GROW_PAGES ? want : OC_POOL_GROW_PAGES;
  uint8_t *p = (uint8_t *)__real_memalign(MMAP_PAGE, pages * MMAP_PAGE);
  if (!p && pages > want) { pages = want; p = (uint8_t *)__real_memalign(MMAP_PAGE, pages * MMAP_PAGE); }
  if (!p) return 0;
  oc_pool_spent += oc_pool_bump;
  oc_pool = p; oc_pool_pages = pages; oc_pool_bump = 0;
  debugPrintf("[oc] commit-pool grown by %zu MB from newlib (now %zu MB, live %zu MB)\n",
              (pages * MMAP_PAGE) >> 20, ((oc_pool_spent + pages) * MMAP_PAGE) >> 20,
              (oc_live_pages * MMAP_PAGE) >> 20);
  return 1;
}

// Commit [addr,len): alias contiguous bump-pool runs into the reserved OC range
// via svcMapMemory (which remaps the pool source away -- we only access via the
// OC address). Already-committed pages are skipped. caller holds g_mmap_lock.
static void oc_commit_locked(void *addr, size_t len) {
  if ((uint8_t *)addr < oc_base) return;
  size_t first = ((uint8_t *)addr - oc_base) / MMAP_PAGE;
  size_t cnt   = (len + MMAP_PAGE - 1) / MMAP_PAGE;
  if (first >= oc_pages) return;
  if (first + cnt > oc_pages) cnt = oc_pages - first;
  size_t i = 0;
  while (i < cnt) {
    if (oc_committed[first + i]) { i++; continue; }
    size_t run = 0;
    while (i + run < cnt && !oc_committed[first + i + run]) run++;
    if (oc_pool_bump == oc_pool_pages && !oc_pool_grow_locked(run)) {
      debugPrintf("[oc] commit-pool EXHAUSTED: need %zu pages, newlib has no more (live %zu MB)\n",
                  run, (oc_live_pages * MMAP_PAGE) >> 20);
      return;   // can't back; touching it will fault
    }
    if (run > oc_pool_pages - oc_pool_bump) run = oc_pool_pages - oc_pool_bump;
    void *dst = oc_base + (first + i) * MMAP_PAGE;
    void *src = oc_pool + oc_pool_bump * MMAP_PAGE;
    Result rc = svcMapMemory(dst, src, (u64)run * MMAP_PAGE);
    if (R_FAILED(rc)) {
      debugPrintf("[oc] svcMapMemory FAIL dst=%p run=%zu rc=0x%x\n", dst, run, rc);
      /* name what got mapped into the reserved range behind our back */
      uint64_t q = (uint64_t)(uintptr_t)dst, qend = q + (uint64_t)run * MMAP_PAGE;
      while (q < qend) {
        MemoryInfo mi; u32 pi;
        if (R_FAILED(svcQueryMemory(&mi, &pi, q)) || mi.addr + mi.size <= q) break;
        if (mi.type != MemType_Unmapped)
          debugPrintf("[oc]   occupied: 0x%llx..0x%llx type=0x%x\n", (unsigned long long)mi.addr,
                      (unsigned long long)(mi.addr + mi.size), mi.type);
        q = mi.addr + mi.size;
      }
      return;
    }
    memset(dst, 0, run * MMAP_PAGE);   // freshly committed anon must read as zero
    for (size_t k = 0; k < run; k++) oc_committed[first + i + k] = 1;
    oc_pool_bump += run; oc_live_pages += run;
    if (((oc_live_pages * MMAP_PAGE) >> 24) != (((oc_live_pages - run) * MMAP_PAGE) >> 24))
      if (TRACE_MMAP) debugPrintf("[oc] committed %zu MB (pool %zu/%zu MB)\n",
                  (oc_live_pages * MMAP_PAGE) >> 20, ((oc_pool_spent + oc_pool_bump) * MMAP_PAGE) >> 20,
                  ((oc_pool_spent + oc_pool_pages) * MMAP_PAGE) >> 20);
    i += run;
  }
}

// munmap of an OC range: reclaim only UNCOMMITTED pages (the tail-overflow slack
// Unity trims after each over-map). Committed pages stay reserved+mapped (a small
// bounded leak) so a later reservation can't collide with a live alias.
static void oc_free_locked(void *addr, size_t len) {
  if ((uint8_t *)addr < oc_base) return;
  size_t first = ((uint8_t *)addr - oc_base) / MMAP_PAGE;
  size_t cnt   = (len + MMAP_PAGE - 1) / MMAP_PAGE;
  if (first >= oc_pages) return;
  if (first + cnt > oc_pages) cnt = oc_pages - first;
  const uint32_t own = oc_owner[first];
  if (!own || own == OC_FOREIGN) return;
  for (size_t i = 0; i < cnt; i++) {
    if (oc_owner[first + i] != own) break;
    if (!oc_committed[first + i]) { oc_used[first + i] = 0; oc_owner[first + i] = 0; }
  }
}

// caller holds g_mmap_lock
static void mmap_arena_init_locked(void) {
  if (mmap_arena) return;
  uint8_t *base; size_t usable;
  if (g_mmap_arena_base) {
    base   = (uint8_t *)g_mmap_arena_base;   // dedicated, already 256MB-aligned
    usable = g_mmap_arena_size;
  } else {
    // fallback (small heap / applet): memalign a modest arena (< 2GB newlib limit)
    const size_t want = (size_t)768 * 1024 * 1024 + MMAP_BIG_ALIGN;
    uint8_t *raw = memalign(MMAP_PAGE, want);
    if (!raw) fatal_error("mmap arena alloc (%u MB) failed", (unsigned)(want >> 20));
    base   = (uint8_t *)(((uintptr_t)raw + (MMAP_BIG_ALIGN - 1)) & ~(MMAP_BIG_ALIGN - 1));
    usable = (size_t)768 * 1024 * 1024;
  }
  size_t pages  = usable / MMAP_PAGE;
  uint8_t *used = (uint8_t *)calloc(pages, 1);
  if (!used) fatal_error("mmap bitmap alloc failed");
  if (g_overcommit) {
    mmap_committed = (uint8_t *)calloc(pages, 1);
    if (!mmap_committed) fatal_error("mmap commit-bitmap alloc failed");
  }
  mmap_usable = usable; mmap_pages = pages; mmap_used = used;
  mmap_arena  = base;   // publish last (alloc/free key off this)
  if (TRACE_MMAP) debugPrintf("[mmap] arena: %u MB %s at %p\n", (unsigned)(usable >> 20),
              g_overcommit ? "virtual (alias, on-demand commit)" : "256MB-aligned heap-backed",
              base);
}

// caller holds g_mmap_lock.
// Returns the mapped base and writes the number of bytes ACTUALLY reserved
// (in-arena) to *got. For big alignment over-maps (Unity reserves block+align,
// then munmaps the unaligned head/tail), the request is much larger than the
// ~256MB block Unity actually keeps. Normally we reserve the whole over-map and
// let the tail-munmap give it back. But for the LAST 256MB slot the full over-map
// runs past the arena end, so a plain "need contiguous pages" search fails even
// though the kept block fits. In that case we reserve only [slot, arena_end) -- the
// kept block lives there; Unity's tail-munmap targets addresses beyond our arena
// and is a harmless no-op. This removes the transient peak so each block costs
// exactly its 256MB slot (floor(arena/256MB) blocks fit, no 2x headroom needed).
static void *mmap_arena_alloc_locked(size_t len, size_t *got) {
  size_t need = (len + MMAP_PAGE - 1) / MMAP_PAGE;
  if (!need) need = 1;
  if (len >= MMAP_BIG_THRESH) {
    const size_t step = MMAP_BIG_ALIGN / MMAP_PAGE;   // 256MB in pages
    size_t kept = need > step ? need - step : need;   // pages Unity actually keeps
    // pass 1: full over-map fits within the arena (normal case for all but the last slot)
    for (size_t i = 0; i + need <= mmap_pages; i += step) {
      size_t run = 0;
      while (run < need && !mmap_used[i + run]) run++;
      if (run == need) {
        for (size_t k = 0; k < need; k++) mmap_used[i + k] = 1;
        *got = need * MMAP_PAGE;
        return mmap_arena + i * MMAP_PAGE;
      }
    }
    // pass 2: tail slot -- the over-map would spill past the arena end, but the kept
    // block fits in [slot, arena_end). Reserve only that; the spill is trimmed away.
    for (size_t i = 0; i < mmap_pages; i += step) {
      if (i + need <= mmap_pages) continue;          // handled by pass 1
      size_t avail = mmap_pages - i;
      if (avail < kept) continue;                    // kept block wouldn't fit
      size_t run = 0;
      while (run < avail && !mmap_used[i + run]) run++;
      if (run == avail) {
        for (size_t k = 0; k < avail; k++) mmap_used[i + k] = 1;
        *got = avail * MMAP_PAGE;                     // only the in-arena portion
        return mmap_arena + i * MMAP_PAGE;
      }
    }
  } else {
    for (size_t i = 0; i + need <= mmap_pages; ) {
      size_t run = 0;
      while (run < need && !mmap_used[i + run]) run++;
      if (run == need) {
        for (size_t k = 0; k < need; k++) mmap_used[i + k] = 1;
        *got = need * MMAP_PAGE;
        return mmap_arena + i * MMAP_PAGE;
      }
      i += run + 1;
    }
  }
  *got = 0;
  return NULL;
}

static void mmap_arena_free(void *addr, size_t len) {
  if (!mmap_arena || (uint8_t *)addr < mmap_arena) return;
  size_t off = (uint8_t *)addr - mmap_arena;
  if (off >= mmap_usable) return;
  size_t first = off / MMAP_PAGE;
  size_t cnt   = (len + MMAP_PAGE - 1) / MMAP_PAGE;
  mutexLock(&g_mmap_lock);
  for (size_t k = 0; k < cnt && first + k < mmap_pages; k++)
    mmap_used[first + k] = 0;
  mutexUnlock(&g_mmap_lock);
}

// Stopgap: when the 256MB-block arena is exhausted by Unity's 9 pools, small
// (sub-threshold) il2cpp/GC mmaps still need to land somewhere. Serve them from
// newlib's free heap via memalign and track each so munmap can free it. This is
// not real overcommit (it consumes physical newlib heap), but it unblocks the
// sub-1MB il2cpp allocations that were failing and surfaces il2cpp's true mmap
// appetite in the log to size the proper fix.
#define MMAP_FALLBACK_MAX 4096
static struct { void *ptr; size_t len; } g_fb[MMAP_FALLBACK_MAX];
static int   g_fb_n = 0;
static size_t g_fb_bytes = 0;
static Mutex g_fb_lock;

static void *mmap_fallback(size_t length, int flags, int fd, long offset) {
  /* Big anonymous reservations are Unity Dynamic-Heap pools: its allocator masks
   * pointers down to a large-aligned pool base to derive block indices, so a
   * 4KB-aligned block makes that math walk off into unmapped memory (the
   * libunity+0xdce75c Data Abort). Hand big anon maps a MMAP_BIG_ALIGN-aligned
   * base so the index math lands correctly even out here in the newlib overflow. */
  size_t align = (length >= MMAP_BIG_THRESH && (flags & BIONIC_MAP_ANONYMOUS))
                   ? MMAP_BIG_ALIGN : MMAP_PAGE;
  void *q = memalign(align, length);
  if (!q) return NULL;
  long got = 0;
  if (flags & BIONIC_MAP_ANONYMOUS) {
    memset(q, 0, length);
  } else {
    if (fd >= 0) {
      long cur = lseek(fd, 0, SEEK_CUR);
      if (lseek(fd, offset, SEEK_SET) >= 0)
        while ((size_t)got < length) { long r = read(fd, (char *)q + got, length - got); if (r <= 0) break; got += r; }
      if (cur >= 0) lseek(fd, cur, SEEK_SET);
    }
    if ((size_t)got < length) memset((char *)q + got, 0, length - got);
  }
  mutexLock(&g_fb_lock);
  if (g_fb_n < MMAP_FALLBACK_MAX) { g_fb[g_fb_n].ptr = q; g_fb[g_fb_n].len = length; g_fb_n++; g_fb_bytes += length; }
  const size_t total = g_fb_bytes;
  mutexUnlock(&g_fb_lock);
  if (TRACE_MMAP) debugPrintf("[mmap] fallback %u KB -> %p  anon=%d fd=%d off=0x%lx got=%ld (total %u MB)\n",
              (unsigned)(length >> 10), q, !!(flags & BIONIC_MAP_ANONYMOUS), fd, offset, got,
              (unsigned)(total >> 20));
  return q;
}

// returns 1 and frees if addr was a fallback allocation
static int mmap_fallback_free(void *addr) {
  mutexLock(&g_fb_lock);
  for (int i = 0; i < g_fb_n; i++) {
    if (g_fb[i].ptr == addr) {
      free(addr);
      g_fb_bytes -= g_fb[i].len;
      g_fb[i] = g_fb[--g_fb_n];
      mutexUnlock(&g_fb_lock);
      return 1;
    }
  }
  mutexUnlock(&g_fb_lock);
  return 0;
}

// ---- read-only file-map dedup cache ---------------------------------------
// il2cpp builds a stack trace for every thrown managed exception (even caught
// ones -- GPG login retries, etc.), and the symbolizer mmaps libil2cpp.so
// (~40MB) + libunity.so (~25MB) read-only EACH TIME, never munmapping. Over a
// play session that is dozens of fresh copies (~1.4GB) that exhaust newlib ->
// the "arena FULL / out of RAM" self-exit. Dedup: hand back one shared, pinned
// buffer per (file inode, offset, length). Safe -- these maps are read-only.
#define MAPC_N 24
static struct { uint64_t ino; long off; size_t len; void *ptr; } g_mapc[MAPC_N];
static int g_mapc_n = 0;
static void *mapcache_get(uint64_t ino, long off, size_t len) {
  void *r = NULL;
  mutexLock(&g_fb_lock);
  for (int i = 0; i < g_mapc_n; i++)
    if (g_mapc[i].ino == ino && g_mapc[i].off == off && g_mapc[i].len == len) { r = g_mapc[i].ptr; break; }
  mutexUnlock(&g_fb_lock);
  return r;
}
static void mapcache_put(uint64_t ino, long off, size_t len, void *ptr) {
  mutexLock(&g_fb_lock);
  for (int i = 0; i < g_fb_n; i++)          // pin: drop from fallback free-list
    if (g_fb[i].ptr == ptr) { g_fb_bytes -= g_fb[i].len; g_fb[i] = g_fb[--g_fb_n]; break; }
  if (g_mapc_n < MAPC_N) { g_mapc[g_mapc_n].ino = ino; g_mapc[g_mapc_n].off = off;
                           g_mapc[g_mapc_n].len = len; g_mapc[g_mapc_n].ptr = ptr; g_mapc_n++; }
  mutexUnlock(&g_fb_lock);
}

void *mmap_fake(void *addr, size_t length, int prot, int flags, int fd, long offset) {
  (void)addr;
  if (length == 0) length = 1;
  if (!(flags & BIONIC_MAP_ANONYMOUS)) nx_wb_settle(fd);   /* the map copies the file's bytes */
  {
    /* phantom fd: an anonymous map (zero-filled) with the file's bytes copied in */
    Phantom *ph = !(flags & BIONIC_MAP_ANONYMOUS) ? ph_get(fd) : NULL;
    if (ph) {
      void *q = mmap_fake(addr, length, 0x1 | BIONIC_PROT_WRITE, flags | BIONIC_MAP_ANONYMOUS, -1, 0);
      if (q != (void *)-1 && q) ph_pread(ph, q, length, offset);
      return q;
    }
  }

  // Big anonymous PROT_NONE reservations -> stack-region OC arena: cheap address
  // space now, physical aliased in on the later mprotect(RW). On a full window we
  // fall through to the heap-backed arena below (no behaviour change there).
  if (oc_pages && length >= MMAP_BIG_THRESH &&
      (flags & BIONIC_MAP_ANONYMOUS) && prot == BIONIC_PROT_NONE) {
    size_t ocres = 0;
    mutexLock(&g_mmap_lock);
    void *op = oc_alloc_locked(length, &ocres);
    mutexUnlock(&g_mmap_lock);
    if (op) {
      if (TRACE_MMAP) debugPrintf("[mmap] %u MB (prot=0x0 anon=1) -> %p  [OC reserve %u MB]\n",
                  (unsigned)(length >> 20), op, (unsigned)(ocres >> 20));
      return op;   // reserved-only; committed lazily via mprotect_fake
    }
    if (TRACE_MMAP) debugPrintf("[mmap] OC window full for %u MB -> heap-backed arena\n",
                (unsigned)(length >> 20));
  }

  size_t reserved = 0;
  mutexLock(&g_mmap_lock);
  mmap_arena_init_locked();
  void *p = mmap_arena_alloc_locked(length, &reserved);
  mutexUnlock(&g_mmap_lock);
  if (length >= MMAP_BIG_THRESH)
    if (TRACE_MMAP) debugPrintf("[mmap] %u MB (prot=0x%x anon=%d) -> %p  [reserved %u MB]\n",
                (unsigned)(length >> 20), prot, !!(flags & BIONIC_MAP_ANONYMOUS), p,
                (unsigned)(reserved >> 20));
  /* A file-backed map must be contiguous and fully readable. When the arena can
   * only give a tail-overflow reservation (reserved < length) we'd read just
   * `fill` bytes and leave the tail unfilled -- silently truncating the file in
   * RAM. For global-metadata.dat that nulls out System.Object (Class::Init NULL).
   * Hand any short-reserved file map to newlib, which backs the whole length. */
  if (p && !(flags & BIONIC_MAP_ANONYMOUS) && fd >= 0 && reserved < length) {
    mutexLock(&g_mmap_lock);
    mmap_arena_free(p, length);
    mutexUnlock(&g_mmap_lock);
    if (TRACE_MMAP) debugPrintf("[mmap] file fd=%d len=%zu: arena tail-overflow (reserved=%zu) -> newlib\n",
                fd, length, reserved);
    p = NULL;
  }
  if (!p) {
    // Arena exhausted (Unity's pools fill it). Route the request to newlib's free
    // heap regardless of size -- il2cpp's resource-extraction maps can exceed 64MB,
    // and rejecting them is what NULL-derefs the engine. Only a genuinely huge map
    // (> newlib free) will fail, and we log that distinctly.
    // Read-only file maps get deduped (backtrace .so symbolication leak, see above).
    int ro_file = fd >= 0 && !(flags & BIONIC_MAP_ANONYMOUS) && !(prot & BIONIC_PROT_WRITE);
    uint64_t mino = (ro_file && fd < FD_INO_MAX) ? g_fd_ino[fd] : 0;
    if (mino) { void *hit = mapcache_get(mino, offset, length); if (hit) return hit; }
    void *q = mmap_fallback(length, flags, fd, offset);
    if (q) { if (mino) mapcache_put(mino, offset, length, q); return q; }
    if (TRACE_MMAP) debugPrintf("[mmap] arena FULL and newlib fallback FAILED for %u MB (out of RAM)\n",
                (unsigned)(length >> 20));
    errno = ENOMEM; return (void *)-1;
  }

  // Never touch beyond what we actually reserved in-arena (tail over-maps reserve
  // less than the requested length; the spill lives past the arena and is trimmed).
  size_t fill = length < reserved ? length : reserved;

  if (g_overcommit) {
    // PROT_NONE reservation: address space only, no physical -- the whole point.
    // The engine commits the sub-ranges it uses later via mprotect(RW).
    if (prot == BIONIC_PROT_NONE) return p;
    // Otherwise commit now (anon RW, file maps): svcMapPhysicalMemory zero-fills,
    // so anon needs no memset; file maps read their contents over the zeros.
    arena_commit_range(p, fill);
    if (!(flags & BIONIC_MAP_ANONYMOUS) && fd >= 0) {
      long got = 0, cur = lseek(fd, 0, SEEK_CUR);
      if (lseek(fd, offset, SEEK_SET) >= 0)
        while ((size_t)got < fill) { long r = read(fd, (char *)p + got, fill - got); if (r <= 0) break; got += r; }
      if (cur >= 0) lseek(fd, cur, SEEK_SET);
    }
    return p;
  }

  if (flags & BIONIC_MAP_ANONYMOUS) {
    memset(p, 0, fill);   // anonymous memory must read back as zero
  } else {
    // File-backed mapping: pull [offset, offset+fill) into RAM (no real mmap).
    long got = 0;
    if (fd >= 0) {
      long cur = lseek(fd, 0, SEEK_CUR);
      if (lseek(fd, offset, SEEK_SET) >= 0) {
        while ((size_t)got < fill) {
          long r = read(fd, (char *)p + got, fill - (size_t)got);
          if (r <= 0) break;
          got += r;
        }
      }
      if (cur >= 0) lseek(fd, cur, SEEK_SET);
    }
    if ((size_t)got < fill) memset((char *)p + got, 0, fill - (size_t)got);
    if (fd >= 0)
      if (TRACE_MMAP) debugPrintf("[mmap] file map fd=%d len=%zu reserved=%zu fill=%zu got=%ld%s\n",
                  fd, length, reserved, fill, got,
                  ((size_t)got < length) ? "  *** TRUNCATED ***" : "");
  }
  return p;
}

int munmap_fake(void *addr, size_t length) {
  if (mmap_fallback_free(addr)) return 0;   // newlib fallback allocation
  if (oc_contains(addr)) {                   // stack-region OC reservation
    mutexLock(&g_mmap_lock);
    oc_free_locked(addr, length);
    mutexUnlock(&g_mmap_lock);
    return 0;
  }
  arena_decommit_range(addr, length);       // reclaim physical (overcommit only)
  mmap_arena_free(addr, length);            // unreserve address space
  return 0;
}

// In overcommit mode mprotect drives commit/decommit: RW/R commits physical at
// the alias address, PROT_NONE decommits it (safe -- reuse re-mprotects to RW).
// In heap-backed mode the arena is always RW so this is a no-op.
int mprotect_fake(void *addr, size_t len, int prot) {
  /* Diagnostic (fires even heap-backed): measure Unity's commit pattern so we can
   * confirm it commits PROT_NONE reservations via mprotect(RW) and size the
   * overcommit commit-pool. Tracks cumulative RW-commit vs PROT_NONE-decommit
   * bytes that fall inside the mmap arena (= Unity's live committed footprint). */
  {
    static size_t rw_b = 0, none_b = 0;
    static unsigned rw_n = 0, none_n = 0, oth_n = 0;
    int in_arena = g_mmap_arena_base &&
                   (uint8_t *)addr >= (uint8_t *)g_mmap_arena_base &&
                   (uint8_t *)addr <  (uint8_t *)g_mmap_arena_base + g_mmap_arena_size;
    if (prot == BIONIC_PROT_NONE)       { none_n++; if (in_arena) none_b += len; }
    else if (prot & BIONIC_PROT_WRITE)  { rw_n++;   if (in_arena) rw_b   += len; }
    else                                  oth_n++;
    if (len >= 4u * 1024 * 1024 || ((rw_n + none_n) & 0x7F) == 0)
      if (TRACE_MMAP) debugPrintf("[mprot] addr=%p len=%zuKB prot=0x%x arena=%d | RW %u/%zuMB NONE %u/%zuMB oth %u  net=%zdMB\n",
                  addr, len >> 10, prot, in_arena, rw_n, rw_b >> 20, none_n, none_b >> 20, oth_n,
                  (ssize_t)(rw_b - none_b) >> 20);
  }
  if (oc_contains(addr)) {
    // OC reservation being committed/decommitted. Unity only ever commits (RW);
    // decommits aren't observed, so PROT_NONE here is left mapped (cheap + safe).
    if (prot != BIONIC_PROT_NONE) {
      mutexLock(&g_mmap_lock);
      oc_commit_locked(addr, len);
      mutexUnlock(&g_mmap_lock);
    }
    return 0;
  }
  if (!g_overcommit) return 0;
  if (prot == BIONIC_PROT_NONE) arena_decommit_range(addr, len);
  else                          arena_commit_range(addr, len);
  return 0;
}
// madvise(MADV_DONTNEED): overcommit zeroes-but-keeps (see arena_dontneed_range);
// heap-backed leaves pages as-is (always RW-backed).
int madvise_fake(void *addr, size_t len, int advice) {
  if (g_overcommit && advice == BIONIC_MADV_DONTNEED) arena_dontneed_range(addr, len);
  return 0;
}

// ---------------------------------------------------------------------------
// filesystem odds and ends
// ---------------------------------------------------------------------------

char *realpath_fake(const char *path, char *resolved) {
  if (!path) return NULL;          /* POSIX: realpath(NULL,..) is an error, not a crash */
  if (!resolved) resolved = malloc(0x1000);
  strcpy(resolved, path);
  return resolved;
}
int strerror_r_fake(int err, char *buf, size_t len) { snprintf(buf, len, "%s", strerror(err)); return 0; }
int statvfs_fake(const char *path, void *buf) { (void)path; memset(buf, 0, 0x70); return 0; }
int statfs_fake(const char *path, void *buf) { (void)path; memset(buf, 0, 0x78); return 0; }

// Synthetic /proc and /sys files. Unity reads /proc/meminfo (MemTotal) to size
// its allocator reservations and /proc/cpuinfo + /sys cpu range to count cores
// for the job system. We report ~1 GB (NOT the real ~3 GB) so the engine's big
// 256MB-block dynamic-heap reservations stay within our mmap arena -- the arena
// is the real backing and has headroom, but Unity must not try to reserve 3 GB
// of address space up front. 3 cores (homebrew gets 0-2).
static const char *synthetic_proc(const char *path) {
  if (!path) return NULL;
  if (!strcmp(path, "/proc/meminfo"))
    return "MemTotal:        524288 kB\n"
           "MemFree:         393216 kB\n"
           "MemAvailable:    393216 kB\n"
           "Buffers:              0 kB\n"
           "Cached:               0 kB\n"
           "SwapTotal:            0 kB\n"
           "SwapFree:             0 kB\n";
  if (!strcmp(path, "/proc/cpuinfo"))
    return "processor\t: 0\nprocessor\t: 1\nprocessor\t: 2\n"
           "Features\t: fp asimd aes pmull sha1 sha2 crc32\n"
           "CPU implementer\t: 0x41\nCPU architecture: 8\nCPU variant\t: 0x1\n"
           "CPU part\t: 0xd07\nCPU revision\t: 1\n";
  if (strstr(path, "cpu_capacity")) return "1024\n";
  if (strstr(path, "cpuinfo_max_freq") || strstr(path, "scaling_max_freq")) return "1785000\n";
  if (strstr(path, "cpuinfo_min_freq") || strstr(path, "scaling_min_freq")) return "1020000\n";
  if (strstr(path, "/cpu/possible") || strstr(path, "/cpu/present") || strstr(path, "/cpu/online"))
    return "0-2\n";
  if (!strncmp(path, "/proc/", 6) || !strncmp(path, "/sys/", 5)) return ""; // empty for the rest
  return NULL;
}

// a buffered fopen for the big .mvgl archives: the engine issues many small
// reads/seeks and the fsdev round-trips dominate without a large buffer.
static FILE *fopen_fake_impl(const char *path, const char *mode);
FILE *fopen_fake(const char *path, const char *mode) {
  uint64_t t0 = fs_begin("fopen", path);
  FILE *f = fopen_fake_impl(path, mode);
  fs_end(t0, 0, "fopen", path);
  if (f) fd_stat_forget(fileno(f));
  return f;
}
static FILE *fopen_fake_impl(const char *path, const char *mode) {
  char _rbuf[512];
  path = asset_redirect(path, _rbuf, sizeof _rbuf);
  const char *synth = synthetic_proc(path);
  if (synth) {
    size_t n = strlen(synth);
    return fmemopen((void *)strdup(synth), n ? n : 1, "r");
  }
  const int writing = strpbrk(mode, "wa+") != NULL;
  if (writing) bn_forget_for(path);
  FILE *f = fopen(path, mode);
  if (!f && writing) {            // save file: create the subdir and retry
    mkdir_parents(path);
    f = fopen(path, mode);
  }
  if (!f && !writing && strchr(mode, 'r')) {
    char alt[320];
    if (basename_fallback(path, alt, sizeof(alt)))
      f = fopen(alt, mode);
  }
  if (!f)
    return NULL;
  if (writing) mark_write_fd(fileno(f));
  if (TRACE_IO) debugPrintf("[io] fopen(%s,%s) -> %p\n", path, mode, (void *)f);
  if (strchr(mode, 'r')) {
    /* Large-sequential-read buffering.
     *
     * CHANGED FOR CLOVERPIT: was keyed on ".mvgl", which is a Chaos Rings 3
     * archive extension. CloverPit has no .mvgl files at all, so this branch
     * never fired and every big asset read went unbuffered off the SD card.
     *
     * CloverPit's large sequential reads are:
     *     assets/bin/Data/data.unity3d                 67.8 MB
     *     assets/bin/Data/*.resource                   streamed audio/texture
     *     assets/bin/Data/Managed/Metadata/global-metadata.dat   15.7 MB
     *
     * global-metadata.dat is read once at il2cpp init but it is read hard, and
     * data.unity3d is read throughout the session. */
    const char *ext = strrchr(path, '.');
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if ((ext && (strcasecmp(ext, ".unity3d")  == 0 ||
                 strcasecmp(ext, ".resource") == 0 ||
                 strcasecmp(ext, ".resS")     == 0)) ||
        strcasecmp(base, "global-metadata.dat") == 0)
      /* 1 MB, not 256 KB. data.unity3d is 67 MB and global-metadata.dat 15.7 MB,
       * read sequentially off an SD card during the load frames that measured
       * 1188 ms and 2987 ms on hardware. Larger buffers mean fewer fs IPC round
       * trips, which is the dominant cost on this path. */
      setvbuf(f, NULL, _IOFBF, 1024 * 1024);
  }
  return f;
}

// ---------------------------------------------------------------------------
// stdio over the fake bionic __sF (stdin/stdout/stderr). libc++_shared wires
// std::cout/cerr/cin to &__sF[1]/[2]/[0]; these wrappers absorb writes to those
// fake FILEs and forward everything else to newlib.
// ---------------------------------------------------------------------------

uint8_t fake_sF[3][0x100]; // referenced by imports.c (__sF / std{in,out,err})

static int is_fake_file(const void *f) {
  const uint8_t *p = f;
  const uint8_t *base = (const uint8_t *)fake_sF;
  return p >= base && p < base + sizeof(fake_sF);
}

size_t fwrite_fake(const void *ptr, size_t size, size_t n, FILE *f) {
  if (is_fake_file(f)) {
#if DEBUG_LOG
    static char buf[0x400];
    const size_t total = size * n < sizeof(buf) - 1 ? size * n : sizeof(buf) - 1;
    memcpy(buf, ptr, total); buf[total] = '\0';
    debugPrintf("stdio: %s", buf);
#endif
    return n;
  }
  return fwrite(ptr, size, n, f);
}
size_t fread_fake(void *ptr, size_t size, size_t n, FILE *f) {
  if (is_fake_file(f)) return 0;
  return fread(ptr, size, n, f);
}
int fputc_fake(int c, FILE *f) { if (is_fake_file(f)) return c; return fputc(c, f); }
int fputs_fake(const char *s, FILE *f) { if (is_fake_file(f)) { debugPrintf("stdio: %s", s); return 0; } return fputs(s, f); }
int fflush_fake(FILE *f) { if (is_fake_file(f) || f == NULL) return 0; return fflush(f); }
int fclose_fake(FILE *f) { if (is_fake_file(f)) return 0;
  int fd = fileno(f); fd_ino_clear(fd);   /* keep g_fd_ino free of stale entries: fd_close_by_path
                                             (rename_fake) closes fds by that table */
  int r = fclose(f); commit_write_fd(fd); return r; }
int ferror_fake(FILE *f) { if (is_fake_file(f)) return 0; return ferror(f); }
int feof_fake(FILE *f) { if (is_fake_file(f)) return 1; return feof(f); }
int fileno_fake(FILE *f) { if (is_fake_file(f)) return ((const uint8_t *)f - &fake_sF[0][0]) / 0x100; return fileno(f); }
int fseek_fake(FILE *f, long off, int whence) { if (is_fake_file(f)) return -1; return fseek(f, off, whence); }
long ftell_fake(FILE *f) { if (is_fake_file(f)) return -1; return ftell(f); }
int getc_fake(FILE *f) { if (is_fake_file(f)) return -1; return getc(f); }
int fgetc_fake(FILE *f) { if (is_fake_file(f)) return -1; return fgetc(f); }
char *fgets_fake(char *s, int n, FILE *f) { if (is_fake_file(f)) return NULL; return fgets(s, n, f); }
int ungetc_fake(int c, FILE *f) { if (is_fake_file(f)) return -1; return ungetc(c, f); }
void setbuf_fake(FILE *f, char *buf) { if (is_fake_file(f)) return; setbuf(f, buf); }

int fprintf_fake(FILE *f, const char *fmt, ...) {
  va_list va; va_start(va, fmt);
  int ret;
  if (is_fake_file(f)) {
#if DEBUG_LOG
    static char buf[0x400];
    ret = vsnprintf(buf, sizeof(buf), fmt, va);
    debugPrintf("stdio: %s", buf);
#else
    ret = 0;
#endif
  } else {
    ret = vfprintf(f, fmt, va);
  }
  va_end(va);
  return ret;
}
int vfprintf_fake(FILE *f, const char *fmt, va_list va) {
  if (is_fake_file(f)) {
#if DEBUG_LOG
    static char buf[0x400];
    int ret = vsnprintf(buf, sizeof(buf), fmt, va);
    debugPrintf("stdio: %s", buf);
    return ret;
#else
    return 0;
#endif
  }
  return vfprintf(f, fmt, va);
}

// ---------------------------------------------------------------------------
// fd routing: the native_app_glue command pipe lives in the fake-fd layer
// (android_native.c). Real files (small fds from open()) pass through to newlib.
// ---------------------------------------------------------------------------

unsigned long g_perf_reads, g_perf_futex_waits;   /* [perf] heartbeat */
long read_fake(int fd, void *buf, size_t count) {
  g_perf_reads++;
  nx_wb_settle(fd);
  {
    Phantom *p = ph_get(fd);
    if (p) { long r = ph_pread(p, buf, count, p->pos); if (r > 0) p->pos += r; return r; }
  }
  if (fd >= 0 && fd < FD_INO_MAX && g_fd_rand[fd]) {   /* /dev/urandom: endless, no SD access */
    if (count) randomGet(buf, count);
    return (long)count;
  }
  if (fakefd_is_fake(fd)) return fakefd_read(fd, buf, count);
  if (nx_net_is_socket(fd)) return nx_net_read(fd, buf, count);
  if (nxio_owns(fd)) return nxio_read(fd, buf, count);
  { struct RaCache *c = ra_find(fd); if (c) return ra_read(c, fd, buf, count); }
  /* fsdev can return fewer bytes than requested for a large read; il2cpp's
   * global-metadata.dat loader (and others) assume a single read() fills the
   * buffer. Loop until `count` is satisfied or we hit EOF/error so the metadata
   * is never silently truncated (a short read leaves System.Object et al.
   * unresolvable -> Class::Init(NULL)). */
  size_t total = 0;
  while (total < count) {
    long r = read(fd, (char *)buf + total, count - total);
    if (r < 0) { if (total) break; return -1; }
    if (r == 0) break; /* EOF */
    total += (size_t)r;
  }
  if (count >= (1u << 20))
    if (TRACE_IO) debugPrintf("[io] read(fd=%d, %zu) -> %zu%s\n", fd, count, total,
                total < count ? "  *** SHORT READ ***" : "");
  watch_dump("read", fd, (long)count, 0, buf, (long)total);
  return (long)total;
}
long write_fake(int fd, const void *buf, size_t count) {
  if (fd >= 0 && fd < WFD_MAX && g_wb[fd]) return wb_write(fd, g_wb[fd], buf, count);
  if (fakefd_is_fake(fd)) return fakefd_write(fd, buf, count);
  if (nx_net_is_socket(fd)) return nx_net_write(fd, buf, count);
  return write(fd, buf, count);
}
int close_fake(int fd) {
  /* A file being created: the write-behind thread finishes it (queued last,
   * once this fd's bookkeeping is cleared: the thread's close frees the number). */
  WBuf *wb = (fd >= 0 && fd < WFD_MAX) ? g_wb[fd] : NULL;
  uint64_t wb_ino = 0;
  char *wb_path = NULL;
  if (fd >= 0 && fd < WFD_MAX && g_write_fd[fd] && g_fd_path && g_fd_path[fd])
    rsc_forget(g_fd_path[fd]);   /* a stat taken while it was being written is stale */
  if (wb) {
    g_wb[fd] = NULL;
    wb_ino = g_fd_ino[fd];
    if (g_fd_path && g_fd_path[fd]) wb_path = strdup(g_fd_path[fd]);
  }
  if (fd >= 0 && fd < FD_INO_MAX && g_open_mark[fd]) { g_open_mark[fd] = 0; __atomic_sub_fetch(&g_files_open, 1, __ATOMIC_RELAXED); }
  fd_stat_forget(fd);
  fd_path_clear(fd);
  nx_net_on_close(fd);
  ra_detach(fd);
  nxio_close(fd);
  fd_ino_clear(fd);
  if (fd >= 0 && fd < FD_INO_MAX) g_fd_rand[fd] = 0;
  if (ph_close(fd)) return 0;
  if (fakefd_is_fake(fd)) return fakefd_close(fd);
  if (fd >= 0 && fd < FD_INO_MAX && g_keep_ok[fd]) {
    g_keep_ok[fd] = 0;
    if (park_put(fd, g_keep_ino[fd])) return 0;
  }
  if (wb) {
    if (wb_enqueue(fd, wb, wb_ino, wb_path)) { g_write_fd[fd] = 0; return 0; }
    free(wb_path);             /* queue full: as before, on this thread */
    wb_write_out(fd, wb);
    wb_free(wb);
  }
  int r = close(fd);
  commit_write_fd(fd);   /* flush a just-written save to the physical SD */
  return r;
}
int pipe_fake(int fds[2]) { return fakefd_pipe(fds); }
/* poll/select and every socket entry point now live in nx_net.c. */

/* `environ`: libil2cpp imports the POSIX environment pointer (getenv-style reads). Give it an
 * empty, NULL-terminated vector -- reads see no variables, nothing faults. The resolver binds
 * the game's `environ` data symbol to &fake_environ (address of this char** variable). */
char  *g_env_empty[1] = { NULL };
char **fake_environ   = g_env_empty;
static volatile int g_h_errno = 0;
int *__get_h_errno_fake(void) { return (int *)&g_h_errno; }

// ---------------------------------------------------------------------------
// process control: fork/exec/etc. are unavailable; report failure.
// ---------------------------------------------------------------------------

int fork_fake(void) { errno = ENOSYS; return -1; }
int execvp_fake(const char *f, char *const argv[]) { (void)f; (void)argv; errno = ENOSYS; return -1; }
int waitpid_fake(int pid, int *status, int opts) { (void)pid; (void)opts; if (status) *status = 0; errno = ECHILD; return -1; }
int kill_fake(int pid, int sig) { (void)pid; (void)sig; return 0; }
int getpid_fake(void) { return 1; }
int sched_yield_fake(void) { svcSleepThread(0); return 0; }
// bionic struct passwd layout (pw_dir at +0x20, as the engine derefs).
struct bionic_passwd {
  char *pw_name;     /* 0x00 */
  char *pw_passwd;   /* 0x08 */
  uint32_t pw_uid;   /* 0x10 */
  uint32_t pw_gid;   /* 0x14 */
  char *pw_gecos;    /* 0x18 */
  char *pw_dir;      /* 0x20 */
  char *pw_shell;    /* 0x28 */
};
void *getpwuid_fake(int uid) {
  (void)uid;
  static struct bionic_passwd pw;
  static char nm[] = "switch", sh[] = "/bin/sh", empty[] = "";
  /* pw_dir was a compile-time array of GAME_HOME; the root is resolved at
   * runtime now, so fill it on first use. */
  static char dir[512];
  if (!dir[0]) snprintf(dir, sizeof dir, "%s", cp_game_root());
  pw.pw_name = nm; pw.pw_passwd = empty; pw.pw_uid = 0; pw.pw_gid = 0;
  pw.pw_gecos = empty; pw.pw_dir = dir; pw.pw_shell = sh;
  return &pw;
}

// Unity computes its home/cache dir via getenv("HOME") (then getpwuid fallback).
// Serve the writable game root for HOME/TMPDIR; delegate everything else to newlib.
const char *managed_path(const char *p) {
  if (!p) return p;
  const char *c = strchr(p, ':');
  return (c && c[1] == '/') ? c + 1 : p;     // "sdmc:/switch/.." -> "/switch/.."
}
char *getenv_fake(const char *name) {
  if (name) {
    /* Unity derives its home/cache dir from these. Must track the runtime root,
     * or it writes its cache next to a folder that does not exist. */
    if (!strcmp(name, "HOME") || !strcmp(name, "TMPDIR"))
      return (char *)managed_path(cp_game_root());
  }
  return getenv(name);
}
// Report a Unix-rooted cwd ("/switch/zookeeper", no "sdmc:") so managed Path
// APIs don't treat it as relative in Path.Combine. newlib's *internal* cwd is
// unchanged, so relative file resolution still works via the default device.
char *getcwd_fake(char *buf, size_t size) {
  char *r = getcwd(buf, size);
  if (!r) return r;
  const char *c = strchr(r, ':');
  if (c && c[1] == '/') memmove(r, c + 1, strlen(c + 1) + 1);  // drop "sdmc:"
  return r;
}
int getrusage_fake(int who, void *usage) { (void)who; if (usage) memset(usage, 0, 144); return 0; }

// ---------------------------------------------------------------------------
// dlopen/dlsym over the already-loaded modules (no real dynamic loading).
// dlsym lets the engine look up its own exports / our shims.
// ---------------------------------------------------------------------------

void *dlopen_fake(const char *name, int flags) {
  (void)flags; debugPrintf("dlopen(%s)\n", name ? name : "(self)");

  /* CLOVERPIT / Unity 6 GATING.
   *
   * libunity reaches these three through dlopen rather than DT_NEEDED, so
   * refusing them here is how the port selects the code paths it can actually
   * service. This is not an optimisation -- without it the engine takes paths
   * that have no backing on Switch and fails in ways that look like hangs.
   *
   *   libswappywrapper.so  Unity logs "InitSwappyWrapper() failed" and uses its
   *                        non-Swappy present path. Also avoids loading a fourth
   *                        module and binding 60 libc++ symbols, since the port
   *                        drives its own frame loop anyway.
   *   libvulkan.so         forces the GLES backend, which switch-mesa provides.
   *                        CloverPit ships GLES3 shader variants (91 GLSL
   *                        sources in data.unity3d), so this is safe.
   *   libaaudio.so         forces the FMOD/OpenSL output that opensles.c backs,
   *                        rather than an AAudio device that does not exist.
   *
   * Anything else keeps the old behaviour of succeeding with a token handle;
   * dlsym_fake ignores the handle and resolves against loaded modules + shims. */
  if (name) {
    static const char *const refuse[] = {
      "libswappywrapper.so", "libvulkan.so", "libaaudio.so",
    };
    for (size_t i = 0; i < sizeof(refuse) / sizeof(*refuse); i++) {
      if (strstr(name, refuse[i])) {
        debugPrintf("dlopen(%s) -> NULL [cloverpit: forced fallback path]\n", name);
        return NULL;
      }
    }
  }

  if (name && nx_bridge_is_lib(name)) {
    debugPrintf("dlopen(%s) -> mvlo_nx managed bridge\n", name);
    return (void *)0x2;
  }
  return (void *)0x1;
}
int dlclose_fake(void *h) { (void)h; return 0; }
const char *dlerror_fake(void) { return NULL; }

/* Application.targetFrameRate: MvLO leaves it at -1 ("Max FPS" unlimited) and
 * Unity's Android player turns -1 into 30 fps. Unity registers its C# internal
 * calls through il2cpp_add_internal_call, which it looks up with dlsym, so
 * wrap just that setter: <= 0 becomes [video] target_fps (60 by default, 0 =
 * leave Unity's default); a value picked in the game's options is kept. */
static void (*g_real_add_icall)(const char *, const void *);
static void (*g_real_set_tfr)(int);
static int g_tfr_set_by_game;
static void nx_set_target_frame_rate(int v) {
  g_tfr_set_by_game = 1;
  int def = nxcfg_int("video.target_fps", 60);
  int eff = (v <= 0 && def > 0) ? def : v;
  static int last = -2;
  if (eff != last) { last = eff; debugPrintf("[gfx] targetFrameRate %d -> %d\n", v, eff); }
  g_real_set_tfr(eff);
}
/* MvLO only calls the setter when a saved setting exists, so a fresh profile
 * keeps Unity's -1 (= 30 fps on Android). Called by the render loop once the
 * managed side is up: apply the default unless the game already chose. */
void nx_tfr_apply_default(void) {
  if (!g_real_set_tfr || g_tfr_set_by_game) return;
  int def = nxcfg_int("video.target_fps", 60);
  if (def <= 0) return;
  debugPrintf("[gfx] targetFrameRate never set by the game -> %d\n", def);
  g_real_set_tfr(def);
}
static void nx_add_internal_call(const char *name, const void *fn) {
  if (name && fn && !strcmp(name, "UnityEngine.Application::set_targetFrameRate")) {
    g_real_set_tfr = (void (*)(int))fn;
    fn = (const void *)&nx_set_target_frame_rate;
    debugPrintf("[gfx] wrapped %s (target_fps=%d)\n", name, nxcfg_int("video.target_fps", 60));
  }
  g_real_add_icall(name, fn);
}

void *dlsym_fake(void *handle, const char *symbol) {
  (void)handle;
  if (!symbol) return NULL;
  /* 1) a real export from a loaded module (il2cpp/unity/main) */
  void *p = so_resolve_external(symbol);
  if (p && !strcmp(symbol, "il2cpp_add_internal_call")) {
    g_real_add_icall = (void (*)(const char *, const void *))p;
    return (void *)&nx_add_internal_call;
  }
  if (p) return p;
  /* 2) one of our libc/GLES/EGL shims (the engine dlopen()s libGLESv2.so etc.
   *    and dlsym()s glGetString/glGetIntegerv, which are shims, not exports) */
  uintptr_t shim = dynlib_find_export(symbol);
  if (shim) { debugPrintf("dlsym(%s) -> %p [shim]\n", symbol, (void *)shim); return (void *)shim; }
  /* 3) the full GLES/EGL API (~150 entry points) lives in mesa, beyond our
   *    static table -- resolve any gl or egl symbol via eglGetProcAddress. */
  if (!strncmp(symbol, "gl", 2) || !strncmp(symbol, "egl", 3)) {
    p = (void *)eglGetProcAddress(symbol);
    if (p) { debugPrintf("dlsym(%s) -> %p [egl]\n", symbol, p); return p; }
  }
  debugPrintf("dlsym(%s) -> NULL\n", symbol);
  return NULL;
}

// ---------------------------------------------------------------------------
// pthread extras: rwlocks, semaphores, timed locks
// ---------------------------------------------------------------------------

typedef struct { RwLock lock; } FakeRwLock;

static FakeRwLock *get_rwlock(void **storage) {
  if (!*storage) { FakeRwLock *l = calloc(1, sizeof(*l)); rwlockInit(&l->lock); *storage = l; }
  return *storage;
}
/* Polled with backoff + GC safepoint for the same reason as the mutex shim. */
static void rw_backoff(unsigned spin) {
  nx_sig_safepoint();
  if (spin < 64) __asm__ volatile("yield"); else svcSleepThread(spin < 256 ? 20000ull : 200000ull);
}
int pthread_rwlock_rdlock_fake(void **rw) { RwLock *l=&get_rwlock(rw)->lock; if (rwlockTryReadLock(l)) return 0; diag_wait_enter(DIAG_W_RWLOCK,l); for (unsigned i=0; !rwlockTryReadLock(l); i++) rw_backoff(i); diag_wait_exit(); return 0; }
int pthread_rwlock_wrlock_fake(void **rw) { RwLock *l=&get_rwlock(rw)->lock; if (rwlockTryWriteLock(l)) return 0; diag_wait_enter(DIAG_W_RWLOCK,l); for (unsigned i=0; !rwlockTryWriteLock(l); i++) rw_backoff(i); diag_wait_exit(); return 0; }
int pthread_rwlock_unlock_fake(void **rw) {
  FakeRwLock *l = get_rwlock(rw);
  if (rwlockIsWriteLockHeldByCurrentThread(&l->lock)) rwlockWriteUnlock(&l->lock);
  else rwlockReadUnlock(&l->lock);
  return 0;
}

typedef struct { Semaphore sem; } FakeSem;
int sem_init_fake(void **s, int pshared, unsigned int value) { (void)pshared; FakeSem *fs = calloc(1, sizeof(*fs)); semaphoreInit(&fs->sem, value); *s = fs; return 0; }
int sem_destroy_fake(void **s) { if (s && *s) { free(*s); *s = NULL; } return 0; }
int sem_post_fake(void **s) { if (s && *s) semaphoreSignal(&((FakeSem *)*s)->sem); return 0; }
int sem_wait_fake(void **s) { if (s && *s) { Semaphore *sm=&((FakeSem *)*s)->sem; diag_wait_enter(DIAG_W_SEM,sm); semaphoreWait(sm); diag_wait_exit(); } return 0; }
int sem_trywait_fake(void **s) { if (s && *s && semaphoreTryWait(&((FakeSem *)*s)->sem)) return 0; errno = EAGAIN; return -1; }
int sem_getvalue_fake(void **s, int *val) { if (s && *s) *val = (int)((FakeSem *)*s)->sem.count; else *val = 0; return 0; }
// no native timed wait on libnx Semaphore; poll with a short backoff to the
// deadline. The engine uses it as a yield-with-timeout in its task scheduler.
int sem_timedwait_fake(void **s, const struct timespec *abs) {
  (void)abs;
  for (int i = 0; i < 1000; i++) {
    if (sem_trywait_fake(s) == 0) return 0;
    svcSleepThread(1000000ull); // 1 ms
  }
  errno = ETIMEDOUT;
  return -1;
}

/* cloverpit_nx's "ack without running the handler" GC bridge used to live
 * here. It is replaced by real signal emulation in nx_signals.c: bdwgc's own
 * handler runs on the target thread at a safepoint, so stop_info.stack_ptr is
 * recorded and GC_push_all_stacks scans the right range. g_il2cpp_base/size
 * are kept for the crash handler. */
uintptr_t g_il2cpp_base = 0;
size_t    g_il2cpp_size = 0;
