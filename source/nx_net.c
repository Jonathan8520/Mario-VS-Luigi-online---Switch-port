/* nx_net.c -- Android/bionic (Linux ABI) sockets on top of libnx BSD sockets.
 *
 * The game's networking is Photon Quantum over UDP: Photon Realtime uses
 * System.Net.Sockets, which IL2CPP implements with plain libc calls
 * (socket/connect/sendto/recvfrom/poll/getaddrinfo/...). libunity and
 * libil2cpp were linked against bionic, so every constant, flag, struct and
 * errno they hand us is Linux-encoded, while libnx speaks FreeBSD:
 *
 *   sockaddr      Linux {u16 family}            BSD {u8 len, u8 family}
 *   SOL_SOCKET    1                             0xffff
 *   SO_*          2,4,6,7,8,9,13,20,21...       4,0x1007,0x20,0x1001,...
 *   MSG_DONTWAIT  0x40                          0x80
 *   O_NONBLOCK    0x800                         0x4000
 *   FIONBIO/READ  0x5421 / 0x541b               _IOW('f',126) / _IOR('f',127)
 *   errno         EINPROGRESS=115, ETIMEDOUT=110 ...   newlib 119, 116 ...
 *   AF_INET6      10                            28 (and the Switch has no IPv6)
 *
 * Every entry point below translates in and out. On top of that, blocking is
 * EMULATED: each socket is put in non-blocking mode at the libnx level and a
 * blocking call becomes a loop of short poll() slices. That gives us
 *   - SO_RCVTIMEO / SO_SNDTIMEO semantics without trusting the sysmodule, and
 *   - a GC safepoint every slice (nx_signals.c): a thread parked in recv() can
 *     still be stopped by IL2CPP's garbage collector.
 *
 * DNS goes through libnx getaddrinfo (sfdnsres); results are re-packed into
 * bionic-layout addrinfo lists that only freeaddrinfo_fake may free.
 *
 * MIT -- part of mvlo_nx.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <alloca.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/filio.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <switch.h>

#include "nx_autopsy.h"
#include "nx_bfmt.h"
#include "nx_config.h"
#include "nx_net.h"
#include "nx_signals.h"
#include "util.h"
#include "android_native_unity.h"   /* fakefd_is_fake */

/* ------------------------------------------------------------ Linux ABI */
#define L_AF_UNSPEC 0
#define L_AF_UNIX   1
#define L_AF_INET   2
#define L_AF_INET6  10
#define L_SOCK_NONBLOCK 0x800
#define L_SOCK_CLOEXEC  0x80000
#define L_SOL_SOCKET 1
#define L_IPPROTO_IP 0
#define L_IPPROTO_TCP 6
#define L_IPPROTO_UDP 17
#define L_IPPROTO_IPV6 41
#define L_O_NONBLOCK 0x800
#define L_F_GETFD 1
#define L_F_SETFD 2
#define L_F_GETFL 3
#define L_F_SETFL 4
#define L_FIONREAD 0x541b
#define L_FIONBIO  0x5421
#define L_MSG_OOB      0x1
#define L_MSG_PEEK     0x2
#define L_MSG_DONTROUTE 0x4
#define L_MSG_TRUNC    0x20
#define L_MSG_DONTWAIT 0x40
#define L_MSG_WAITALL  0x100
#define L_MSG_NOSIGNAL 0x4000

/* Linux errno numbers (what bionic callers compare against). */
enum {
  LE_EPERM = 1, LE_ENOENT = 2, LE_EINTR = 4, LE_EIO = 5, LE_EBADF = 9, LE_EAGAIN = 11,
  LE_ENOMEM = 12, LE_EACCES = 13, LE_EFAULT = 14, LE_EBUSY = 16, LE_EEXIST = 17,
  LE_EINVAL = 22, LE_ENFILE = 23, LE_EMFILE = 24, LE_EPIPE = 32, LE_ENOSYS = 38,
  LE_ENOTSOCK = 88, LE_EDESTADDRREQ = 89, LE_EMSGSIZE = 90, LE_EPROTOTYPE = 91,
  LE_ENOPROTOOPT = 92, LE_EPROTONOSUPPORT = 93, LE_EOPNOTSUPP = 95,
  LE_EAFNOSUPPORT = 97, LE_EADDRINUSE = 98, LE_EADDRNOTAVAIL = 99, LE_ENETDOWN = 100,
  LE_ENETUNREACH = 101, LE_ENETRESET = 102, LE_ECONNABORTED = 103, LE_ECONNRESET = 104,
  LE_ENOBUFS = 105, LE_EISCONN = 106, LE_ENOTCONN = 107, LE_ESHUTDOWN = 108,
  LE_ETIMEDOUT = 110, LE_ECONNREFUSED = 111, LE_EHOSTDOWN = 112, LE_EHOSTUNREACH = 113,
  LE_EALREADY = 114, LE_EINPROGRESS = 115,
};

int nx_errno_to_linux(int e) {
  switch (e) {
    case 0: return 0;
    case EPERM: return LE_EPERM;           case ENOENT: return LE_ENOENT;
    case EINTR: return LE_EINTR;           case EIO: return LE_EIO;
    case EBADF: return LE_EBADF;           case EAGAIN: return LE_EAGAIN;
    case ENOMEM: return LE_ENOMEM;         case EACCES: return LE_EACCES;
    case EFAULT: return LE_EFAULT;         case EBUSY: return LE_EBUSY;
    case EEXIST: return LE_EEXIST;         case EINVAL: return LE_EINVAL;
    case ENFILE: return LE_ENFILE;         case EMFILE: return LE_EMFILE;
    case EPIPE: return LE_EPIPE;           case ENOSYS: return LE_ENOSYS;
    case ENOTSOCK: return LE_ENOTSOCK;     case EDESTADDRREQ: return LE_EDESTADDRREQ;
    case EMSGSIZE: return LE_EMSGSIZE;     case EPROTOTYPE: return LE_EPROTOTYPE;
    case ENOPROTOOPT: return LE_ENOPROTOOPT; case EPROTONOSUPPORT: return LE_EPROTONOSUPPORT;
    case EOPNOTSUPP: return LE_EOPNOTSUPP; case EAFNOSUPPORT: return LE_EAFNOSUPPORT;
    case EADDRINUSE: return LE_EADDRINUSE; case EADDRNOTAVAIL: return LE_EADDRNOTAVAIL;
    case ENETDOWN: return LE_ENETDOWN;     case ENETUNREACH: return LE_ENETUNREACH;
    case ENETRESET: return LE_ENETRESET;   case ECONNABORTED: return LE_ECONNABORTED;
    case ECONNRESET: return LE_ECONNRESET; case ENOBUFS: return LE_ENOBUFS;
    case EISCONN: return LE_EISCONN;       case ENOTCONN: return LE_ENOTCONN;
#ifdef ESHUTDOWN
    case ESHUTDOWN: return LE_ESHUTDOWN;
#endif
    case ETIMEDOUT: return LE_ETIMEDOUT;
    case ECONNREFUSED: return LE_ECONNREFUSED;
#ifdef EHOSTDOWN
    case EHOSTDOWN: return LE_EHOSTDOWN;
#endif
    case EHOSTUNREACH: return LE_EHOSTUNREACH; case EALREADY: return LE_EALREADY;
    case EINPROGRESS: return LE_EINPROGRESS;
    default: return e;
  }
}
/* SO_ERROR comes straight from the bsd sysmodule: a FreeBSD errno, not a
 * newlib one. */
static int bsd_errno_to_linux(int e) {
  switch (e) {
    case 35: return LE_EAGAIN;        case 36: return LE_EINPROGRESS;   case 37: return LE_EALREADY;
    case 38: return LE_ENOTSOCK;      case 39: return LE_EDESTADDRREQ;  case 40: return LE_EMSGSIZE;
    case 41: return LE_EPROTOTYPE;    case 42: return LE_ENOPROTOOPT;   case 43: return LE_EPROTONOSUPPORT;
    case 45: return LE_EOPNOTSUPP;    case 47: return LE_EAFNOSUPPORT;  case 48: return LE_EADDRINUSE;
    case 49: return LE_EADDRNOTAVAIL; case 50: return LE_ENETDOWN;      case 51: return LE_ENETUNREACH;
    case 52: return LE_ENETRESET;     case 53: return LE_ECONNABORTED;  case 54: return LE_ECONNRESET;
    case 55: return LE_ENOBUFS;       case 56: return LE_EISCONN;       case 57: return LE_ENOTCONN;
    case 58: return LE_ESHUTDOWN;     case 60: return LE_ETIMEDOUT;     case 61: return LE_ECONNREFUSED;
    case 64: return LE_EHOSTDOWN;     case 65: return LE_EHOSTUNREACH;
    default: return e;                /* 1..34 agree with Linux */
  }
}
#define FAIL_LINUX(e) do { errno = (e); return -1; } while (0)
#define FAIL_NX()     do { errno = nx_errno_to_linux(errno); return -1; } while (0)

/* --------------------------------------------------------- per-fd state */
#define NFD 1024
typedef struct {
  uint8_t  is_sock;
  uint8_t  user_nonblock;   /* what the GAME asked for; libnx side is always NB */
  uint8_t  type;            /* SOCK_STREAM / SOCK_DGRAM */
  uint8_t  unconnected;     /* UDP socket the game "connected" but we left un-connected ([net] udp_unconnected) */
  uint8_t  has_peer;        /* peer below is set (recorded at connect) */
  int16_t  redirect;        /* [net] photon_rebind: the bsd fd to use instead (>0 = active) */
  int32_t  rcvtimeo_ms;     /* 0 = forever */
  int32_t  sndtimeo_ms;
  struct sockaddr_in peer;  /* BSD peer recorded at connect (udp_unconnected send target / rebind) */
} SockInfo;
static SockInfo g_fd[NFD];

/* [net] udp_unconnected: on a UDP socket, don't call bsd connect(); keep the
 * peer and sendto/recvfrom-filter it (tests whether a *connected* UDP socket in
 * bsdsocket is what rots after ~400 s). [net] photon_rebind_s: after N s,
 * swap a fresh UDP socket under the same app fd (new source port). Both off. */
static int g_udp_unconnected;
static int g_rebind_s;
static volatile int g_photon_fd = -1;     /* rebind target: last connected UDP socket */
static volatile uint64_t g_photon_since_ms;
/* The bsd fd an operation should really use: the rebind target, or fd itself. */
static inline int sock_real(int fd) {
  int r = (fd >= 0 && fd < NFD) ? g_fd[fd].redirect : 0;
  return r > 0 ? r : fd;
}
static int g_net_up;
static unsigned g_stat_sock, g_stat_dns, g_stat_dns_fail;

int nx_net_is_socket(int fd) { return fd >= 0 && fd < NFD && g_fd[fd].is_sock; }

static uint64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

/* Calls into the BSD/DNS services in flight, for the netlog heartbeat
 * (nx_netbeat.c): the whole-console freezes stop every thread at once, so the
 * last heartbeat has to say whether a network call had been stuck. */
static volatile int         g_net_busy;
static volatile uint64_t    g_net_busy_since;
static const char *volatile g_net_busy_op;
static volatile uint64_t    g_net_calls;
static inline uint64_t net_begin(const char *op) {
  uint64_t t = now_ms();
  if (__atomic_fetch_add(&g_net_busy, 1, __ATOMIC_ACQ_REL) == 0) { g_net_busy_op = op; g_net_busy_since = t; }
  return t;
}
static inline void net_end(void) {
  __atomic_add_fetch(&g_net_calls, 1, __ATOMIC_RELAXED);
  if (__atomic_sub_fetch(&g_net_busy, 1, __ATOMIC_ACQ_REL) == 0) g_net_busy_since = 0;
}
void nx_net_beacon(char *buf, unsigned len) {
  uint64_t s = g_net_busy_since;
  int n = g_net_busy;
  if (s && n)
    nx_bfmt(buf, len, " net: %d in flight, oldest %llums (%s), %llu calls", n,
             (unsigned long long)(now_ms() - s), g_net_busy_op ? g_net_busy_op : "?",
             (unsigned long long)g_net_calls);
  else
    nx_bfmt(buf, len, " net: idle, %llu calls", (unsigned long long)g_net_calls);
}

/* ---- Photon datagram ring ([debug] photon_ring) --------------------------
 * The last ~30 s of UDP datagrams this process sent and received, captured at
 * the application boundary (the real Photon payload, before any ipc_cap), with
 * a ms timestamp, the direction and the first 16 bytes (Photon command
 * header). The freeze (29/10) comes ~400 s into an open Photon connection with
 * the network almost silent; this says exactly what crossed the wire in the
 * seconds before it. Printed in the freeze autopsy and, every 1800 frames, in
 * debug.log. Off by default: a static ring, relaxed atomics, no lock, no
 * allocation, no log on the hot path. */
#define PR_RING 256
typedef struct {
  uint64_t t_ms;
  uint16_t len;
  int16_t  fd;
  uint8_t  dir;      /* 1 = tx, 0 = rx */
  uint8_t  n;        /* bytes captured (<= 16) */
  uint8_t  head[16];
} PRec;
static PRec g_pr[PR_RING];
static volatile uint32_t g_pr_n;
static int g_pr_on;

static void pr_note(int dir, int fd, const void *buf, size_t len) {
  if (!g_pr_on || !buf || fd < 0 || fd >= NFD || g_fd[fd].type != SOCK_DGRAM) return;
  uint32_t k = __atomic_fetch_add(&g_pr_n, 1, __ATOMIC_RELAXED) % PR_RING;
  PRec *r = &g_pr[k];
  size_t n = len < 16 ? len : 16;
  r->t_ms = now_ms(); r->fd = (int16_t)fd; r->dir = (uint8_t)dir;
  r->len = len > 0xFFFF ? 0xFFFF : (uint16_t)len; r->n = (uint8_t)n;
  if (n) memcpy(r->head, buf, n);
}

static unsigned pr_clamp(int n, unsigned room) { return n < 0 ? 0 : (unsigned)n < room ? (unsigned)n : (room ? room - 1 : 0); }

/* Age-sorted dump of the datagrams from the last window_ms ms (<=0 = all). */
unsigned nx_net_photon_report(char *buf, unsigned len, int window_ms) {
  if (!g_pr_on || !len) return 0;
  uint64_t now = now_ms();
  uint32_t n = __atomic_load_n(&g_pr_n, __ATOMIC_ACQUIRE);
  uint32_t k0 = n > PR_RING ? n - PR_RING : 0;
  unsigned o = pr_clamp(nx_bfmt(buf, len, "last Photon datagrams (age ms, dir, fd, len, first 16 bytes), %u seen:\n", n), len);
  for (uint32_t k = k0; k < n && o < len; k++) {
    PRec *r = &g_pr[k % PR_RING];
    uint64_t age = now - r->t_ms;
    if (window_ms > 0 && age > (uint64_t)window_ms) continue;
    o += pr_clamp(nx_bfmt(buf + o, len - o, "  -%llu %s fd %d len %u :", (unsigned long long)age,
                          r->dir ? "tx" : "rx", r->fd, r->len), len - o);
    for (unsigned i = 0; i < r->n && o < len; i++)
      o += pr_clamp(nx_bfmt(buf + o, len - o, " %02x", r->head[i]), len - o);
    if (o < len) o += pr_clamp(nx_bfmt(buf + o, len - o, "\n"), len - o);
  }
  return o;
}

/* ------------------------------------------------------------ sockaddr */
typedef struct { uint16_t family; uint16_t port; uint32_t addr; uint8_t zero[8]; } l_sockaddr_in;

/* Linux -> BSD. Returns BSD length, or -1 (errno set, Linux value). */
static int addr_in(const void *la, unsigned llen, struct sockaddr_in *out) {
  if (!la || llen < 2) { errno = LE_EINVAL; return -1; }
  uint16_t fam = *(const uint16_t *)la;
  if (fam == L_AF_INET && llen >= 8) {
    const l_sockaddr_in *s = la;
    memset(out, 0, sizeof *out);
    out->sin_len = sizeof *out;
    out->sin_family = AF_INET;
    out->sin_port = s->port;
    out->sin_addr.s_addr = s->addr;
    return sizeof *out;
  }
  if (fam == L_AF_UNSPEC) {           /* connect(AF_UNSPEC) == dissolve association */
    memset(out, 0, sizeof *out);
    out->sin_len = sizeof *out;
    out->sin_family = AF_UNSPEC;
    return sizeof *out;
  }
  errno = LE_EAFNOSUPPORT;            /* no IPv6 on Switch, no AF_UNIX */
  return -1;
}

/* BSD -> Linux into a caller buffer of *llen bytes (truncating like the kernel). */
static void addr_out(const struct sockaddr *ba, void *la, unsigned *llen) {
  if (!la || !llen) return;
  l_sockaddr_in s;
  memset(&s, 0, sizeof s);
  if (ba && ba->sa_family == AF_INET) {
    const struct sockaddr_in *b = (const struct sockaddr_in *)ba;
    s.family = L_AF_INET;
    s.port = b->sin_port;
    s.addr = b->sin_addr.s_addr;
  } else {
    s.family = L_AF_UNSPEC;
  }
  unsigned n = *llen < sizeof s ? *llen : (unsigned)sizeof s;
  memcpy(la, &s, n);
  *llen = sizeof s;
}

/* ------------------------------------------------------------- options */
static int map_sockopt(int level, int opt, int *blevel, int *bopt) {
  if (level == L_SOL_SOCKET) {
    *blevel = SOL_SOCKET;
    switch (opt) {
      case 1:  *bopt = SO_DEBUG; return 0;
      case 2:  *bopt = SO_REUSEADDR; return 0;
      case 3:  *bopt = SO_TYPE; return 0;
      case 4:  *bopt = SO_ERROR; return 0;
      case 5:  *bopt = SO_DONTROUTE; return 0;
      case 6:  *bopt = SO_BROADCAST; return 0;
      case 7:  *bopt = SO_SNDBUF; return 0;
      case 8:  *bopt = SO_RCVBUF; return 0;
      case 9:  *bopt = SO_KEEPALIVE; return 0;
      case 10: *bopt = SO_OOBINLINE; return 0;
      case 13: *bopt = SO_LINGER; return 0;
      case 15: *bopt = SO_REUSEPORT; return 0;
      case 18: *bopt = SO_RCVLOWAT; return 0;
      case 19: *bopt = SO_SNDLOWAT; return 0;
      case 20: *bopt = SO_RCVTIMEO; return 0;
      case 21: *bopt = SO_SNDTIMEO; return 0;
      case 30: *bopt = SO_ACCEPTCONN; return 0;
    }
    return -1;
  }
  if (level == L_IPPROTO_IP) {
    *blevel = IPPROTO_IP;
    switch (opt) {
      case 1:  *bopt = IP_TOS; return 0;
      case 2:  *bopt = IP_TTL; return 0;
      case 3:  *bopt = IP_HDRINCL; return 0;
      case 4:  *bopt = IP_OPTIONS; return 0;
      case 32: *bopt = IP_MULTICAST_IF; return 0;
      case 33: *bopt = IP_MULTICAST_TTL; return 0;
      case 34: *bopt = IP_MULTICAST_LOOP; return 0;
      case 35: *bopt = IP_ADD_MEMBERSHIP; return 0;
      case 36: *bopt = IP_DROP_MEMBERSHIP; return 0;
    }
    return -1;   /* IP_MTU_DISCOVER, IP_PKTINFO...: Linux-only */
  }
  if (level == L_IPPROTO_TCP) {
    *blevel = IPPROTO_TCP;
    if (opt == 1) { *bopt = TCP_NODELAY; return 0; }
    return -1;
  }
  return -1;
}

/* --------------------------------------------------------------- core */
static int set_nx_nonblock(int fd) {
  int fl = fcntl(fd, F_GETFL, 0);
  if (fl < 0) fl = 0;
  return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

int socket_fake(int domain, int type, int proto) {
  int base = type & ~(L_SOCK_NONBLOCK | L_SOCK_CLOEXEC);
  if (domain == L_AF_INET6 || domain == L_AF_UNIX) {
    static int logged;
    if (!logged++) debugPrintf("[net] socket(domain=%d) refused: IPv4 only on Switch\n", domain);
    FAIL_LINUX(LE_EAFNOSUPPORT);
  }
  if (domain != L_AF_INET) FAIL_LINUX(LE_EAFNOSUPPORT);
  if (!g_net_up) FAIL_LINUX(LE_ENETDOWN);
  int fd = socket(AF_INET, base, proto);
  if (fd < 0) {
    debugPrintf("[net] socket(type=%d,proto=%d) failed errno=%d\n", base, proto, errno);
    FAIL_NX();
  }
  if (fd >= NFD) { close(fd); FAIL_LINUX(LE_EMFILE); }
  set_nx_nonblock(fd);
  memset(&g_fd[fd], 0, sizeof g_fd[fd]);
  g_fd[fd].is_sock = 1;
  g_fd[fd].type = (uint8_t)base;
  g_fd[fd].user_nonblock = (type & L_SOCK_NONBLOCK) ? 1 : 0;
  g_stat_sock++;
  if (g_stat_sock <= 32)
    debugPrintf("[net] socket #%u -> fd %d (%s)\n", g_stat_sock, fd,
                base == SOCK_DGRAM ? "udp" : base == SOCK_STREAM ? "tcp" : "other");
  return fd;
}

static int g_async;                        /* [net] async_send (see sendto_fake) */
static void async_drain(void);

void nx_net_on_close(int fd) {
  if (fd < 0 || fd >= NFD || !g_fd[fd].is_sock) return;
  if (g_async && g_fd[fd].type == SOCK_DGRAM) async_drain();   /* its datagrams may still be queued */
  int rb = g_fd[fd].redirect;              /* [net] photon_rebind: close the spare socket too */
  memset(&g_fd[fd], 0, sizeof g_fd[fd]);
  if (rb > 0 && rb < NFD) { memset(&g_fd[rb], 0, sizeof g_fd[rb]); close(rb); }
}

/* Length of one bsd poll inside a longer wait ([net] poll_slice_ms). Short
 * slices keep the GC stop-the-world latency low, but every slice is one IPC to
 * the bsd service: 10 ms = ~380 calls a second while online. The freeze ~7 min
 * into online play (29/10) always came after 157-161k of them; a longer slice
 * tells a per-call cause (the freeze moves later) from a timer (it does not). */
static int g_slice_ms = 10;            /* 0 = no slicing: one bsd poll per wait (1 s cap) */

/* [net] small_ipc: keep every bsd request inside the service's pointer buffer
 * (copied) instead of having our memory mapped into the service (DECISIONS
 * 54): TCP sends/receives are cut to that size (partial results are legal on
 * stream sockets), UDP receives are capped to it (Photon datagrams are ~1.2
 * KB, the buffer 32 KB under Eden), and the FIONREAD fallback peeks that much
 * instead of 64 KB. */
static int g_small_ipc;
extern uint32_t nx_bsdmon_pointer_size(void);
static size_t ipc_cap(int fd, size_t len, int is_send) {
  if (!g_small_ipc) return len;
  uint32_t ps = nx_bsdmon_pointer_size();
  if (!ps || len <= ps) return len;
  if (is_send && g_fd[fd].type != SOCK_STREAM) return len;   /* a datagram cannot be split */
  return ps;
}
static int slice_for(int left_ms) {      /* left_ms < 0: no deadline */
  int s = g_slice_ms ? g_slice_ms : 1000;
  if (left_ms >= 0 && left_ms < s) s = left_ms;
  return s;
}

/* [net] local_wait: never let a bsd poll wait inside the service. The freeze
 * (29/10) comes after ~396 s of Photon connection summed over the session,
 * which is also the time Photon's receive thread spent waiting in bsd polls
 * (5 ms each, ~200 a second); 400k polls of 0 ms did nothing. With 1, a
 * timed poll becomes 0 ms polls with 1 ms sleeps here in between. */
static int g_local_wait;
static int bsd_poll(struct pollfd *p, nfds_t n, int ms) {
  if (!g_local_wait || ms == 0) return poll(p, n, ms);
  uint64_t end = ms < 0 ? UINT64_MAX : now_ms() + (uint64_t)ms;
  for (;;) {
    int r = poll(p, n, 0);
    if (r != 0 || now_ms() >= end) return r;
    nx_sig_sleep_ms(1);
  }
}

/* Wait for `ev` on fd, in GC-friendly slices. timeout_ms 0 = forever.
 * Returns 1 ready, 0 timeout, -1 error (Linux errno). */
static int wait_fd(int fd, short ev, int timeout_ms) {
  uint64_t start = now_ms();
  for (;;) {
    nx_sig_safepoint();
    struct pollfd p = { .fd = fd, .events = ev, .revents = 0 };
    net_begin("wait poll");
    int left = -1;
    if (timeout_ms > 0) { uint64_t el = now_ms() - start; left = el >= (uint64_t)timeout_ms ? 0 : (int)(timeout_ms - el); }
    int r = bsd_poll(&p, 1, slice_for(left));
    net_end();
    if (r > 0) return 1;
    if (r < 0 && errno != EINTR) { errno = nx_errno_to_linux(errno); return -1; }
    if (timeout_ms > 0 && now_ms() - start >= (uint64_t)timeout_ms) return 0;
  }
}

static int sock_blocking(int fd, int flags) {
  return !(g_fd[fd].user_nonblock || (flags & L_MSG_DONTWAIT));
}

static int map_msg_flags(int f) {
  int b = 0;
  if (f & L_MSG_OOB) b |= MSG_OOB;
  if (f & L_MSG_PEEK) b |= MSG_PEEK;
  if (f & L_MSG_DONTROUTE) b |= MSG_DONTROUTE;
  if (f & L_MSG_WAITALL) b |= MSG_WAITALL;
  return b | MSG_DONTWAIT;   /* the libnx side never blocks; see header */
}

int connect_fake(int fd, const void *la, unsigned llen) {
  if (!nx_net_is_socket(fd)) FAIL_LINUX(LE_ENOTSOCK);
  struct sockaddr_in a;
  int alen = addr_in(la, llen, &a);
  if (alen < 0) return -1;
  /* Record the peer of a UDP socket (for udp_unconnected sends and rebind). */
  if (g_fd[fd].type == SOCK_DGRAM && a.sin_family == AF_INET) {
    g_fd[fd].peer = a; g_fd[fd].has_peer = 1;
    if (g_rebind_s > 0) { g_photon_fd = fd; g_photon_since_ms = now_ms(); }   /* rebind target: last connected UDP socket */
    /* [net] udp_unconnected: don't actually connect the bsd socket; emulate a
     * connected UDP socket at our layer (send -> sendto(peer), recv filtered
     * by peer). The game (Photon) sees a normal connected socket. */
    if (g_udp_unconnected) {
      g_fd[fd].unconnected = 1;
      debugPrintf("[net] udp_unconnected: fd %d kept un-connected, peer %s:%u\n",
                  fd, inet_ntoa(a.sin_addr), ntohs(a.sin_port));
      return 0;
    }
  }
  int r = connect(fd, (struct sockaddr *)&a, alen);
  if (r == 0) return 0;
  if (errno != EINPROGRESS && errno != EALREADY) {
    int e = errno;
    debugPrintf("[net] connect fd %d -> %s:%u failed errno=%d\n", fd, inet_ntoa(a.sin_addr), ntohs(a.sin_port), e);
    errno = nx_errno_to_linux(e);
    return -1;
  }
  if (g_fd[fd].user_nonblock) FAIL_LINUX(LE_EINPROGRESS);
  int w = wait_fd(fd, POLLOUT, g_fd[fd].sndtimeo_ms > 0 ? g_fd[fd].sndtimeo_ms : 30000);
  if (w <= 0) { if (w == 0) errno = LE_ETIMEDOUT; return -1; }
  int soerr = 0; socklen_t sl = sizeof soerr;
  getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
  if (soerr) FAIL_LINUX(bsd_errno_to_linux(soerr));
  return 0;
}

int bind_fake(int fd, const void *la, unsigned llen) {
  if (!nx_net_is_socket(fd)) FAIL_LINUX(LE_ENOTSOCK);
  struct sockaddr_in a;
  int alen = addr_in(la, llen, &a);
  if (alen < 0) return -1;
  if (bind(fd, (struct sockaddr *)&a, alen) < 0) FAIL_NX();
  return 0;
}

int listen_fake(int fd, int backlog) {
  if (!nx_net_is_socket(fd)) FAIL_LINUX(LE_ENOTSOCK);
  if (listen(fd, backlog) < 0) FAIL_NX();
  return 0;
}

int accept4_fake(int fd, void *la, unsigned *llen, int flags) {
  nx_sig_safepoint();   /* non-blocking callers loop on EAGAIN without reaching wait_fd() */
  if (!nx_net_is_socket(fd)) FAIL_LINUX(LE_ENOTSOCK);
  for (;;) {
    struct sockaddr_in a; socklen_t al = sizeof a;
    int c = accept(fd, (struct sockaddr *)&a, &al);
    if (c >= 0) {
      if (c >= NFD) { close(c); FAIL_LINUX(LE_EMFILE); }
      set_nx_nonblock(c);
      memset(&g_fd[c], 0, sizeof g_fd[c]);
      g_fd[c].is_sock = 1;
      g_fd[c].type = SOCK_STREAM;
      g_fd[c].user_nonblock = (flags & L_SOCK_NONBLOCK) ? 1 : 0;
      addr_out((struct sockaddr *)&a, la, llen);
      return c;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK) FAIL_NX();
    if (g_fd[fd].user_nonblock) FAIL_LINUX(LE_EAGAIN);
    int w = wait_fd(fd, POLLIN, g_fd[fd].rcvtimeo_ms);
    if (w < 0) return -1;
    if (w == 0) FAIL_LINUX(LE_EAGAIN);
  }
}
int accept_fake(int fd, void *la, unsigned *llen) { return accept4_fake(fd, la, llen, 0); }

/* The send itself. `block` = wait for room even on a non-blocking socket (the
 * async sender: its datagram was already reported as sent). */
static long sendto_sync(int fd, const void *buf, size_t len, int flags, const struct sockaddr_in *a, int alen,
                        int block) {
  int bf = map_msg_flags(flags);
  int rfd = sock_real(fd);
  for (int tries = 0;; tries++) {
    net_begin("send");
    size_t n = ipc_cap(fd, len, 1);
    ssize_t r = alen ? sendto(rfd, buf, n, bf, (const struct sockaddr *)a, alen) : send(rfd, buf, n, bf);
    net_end();
    if (r >= 0) return r;
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      static unsigned nerr;
      if (nerr++ < 16) debugPrintf("[net] send fd %d len %zu failed errno=%d\n", fd, len, errno);
      FAIL_NX();
    }
    if (block ? tries >= 3 : !sock_blocking(fd, flags)) FAIL_LINUX(LE_EAGAIN);
    int w = wait_fd(rfd, POLLOUT, block ? 100 : g_fd[fd].sndtimeo_ms);
    if (w < 0) return -1;
    if (w == 0 && !block) FAIL_LINUX(LE_EAGAIN);
  }
}

/* ---- asynchronous UDP sends ([net] async_send, default 1) ------------------
 * In a match UnityMain sends Photon's datagrams itself (SocketImpl::Send),
 * and now and then the bsd service takes 150-300 ms to take one: the game
 * stood still that long (02/11 profiles: bsdSend under SocketImpl::Send). A
 * UDP send needs no answer, so the datagram is copied into a FIFO and a
 * sender thread hands it to the bsd service; the game gets "sent" at once.
 * One queue and one sender keep the order. A datagram over ASYNC_MAX, or a
 * full queue, goes out synchronously as before; closing a UDP socket waits
 * (at most 200 ms) for the queue to drain. */
#define ASYNC_SLOTS 256
#define ASYNC_MAX   1536
typedef struct {
  int fd, flags, alen;
  struct sockaddr_in a;
  uint32_t len;
  uint8_t data[ASYNC_MAX];
} AsyncPkt;
static AsyncPkt g_aq[ASYNC_SLOTS];
static uint32_t g_aq_head, g_aq_tail;      /* head = next to send, tail = next free (g_aq_lock) */
static int g_aq_busy;                      /* the sender is in the middle of g_aq[head] */
static Mutex g_aq_lock;
static CondVar g_aq_cv;
static uint64_t g_aq_queued, g_aq_sync, g_aq_max_ms;

static void async_main(void *arg) {
  (void)arg;
  static uint8_t tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  install_bionic_tls(tls);
  nx_autopsy_name_self("udp-sender");
  for (;;) {
    mutexLock(&g_aq_lock);
    while (g_aq_head == g_aq_tail) condvarWait(&g_aq_cv, &g_aq_lock);
    AsyncPkt *p = &g_aq[g_aq_head % ASYNC_SLOTS];
    g_aq_busy = 1;
    mutexUnlock(&g_aq_lock);
    uint64_t t0 = now_ms();
    sendto_sync(p->fd, p->data, p->len, p->flags, &p->a, p->alen, 1);
    uint64_t dt = now_ms() - t0;
    mutexLock(&g_aq_lock);
    if (dt > g_aq_max_ms) g_aq_max_ms = dt;
    g_aq_head++;
    g_aq_busy = 0;
    condvarWakeAll(&g_aq_cv);
    mutexUnlock(&g_aq_lock);
  }
}

/* nx_net_on_close: the queue may still hold datagrams for this socket. */
static void async_drain(void) {
  uint64_t until = now_ms() + 200;
  mutexLock(&g_aq_lock);
  while ((g_aq_head != g_aq_tail || g_aq_busy) && now_ms() < until)
    condvarWaitTimeout(&g_aq_cv, &g_aq_lock, 10000000ull);
  mutexUnlock(&g_aq_lock);
}

long sendto_fake(int fd, const void *buf, size_t len, int flags, const void *la, unsigned llen) {
  nx_sig_safepoint();   /* non-blocking callers loop on EAGAIN without reaching wait_fd() */
  if (!nx_net_is_socket(fd)) FAIL_LINUX(LE_ENOTSOCK);
  struct sockaddr_in a; int alen = 0;
  if (la && llen) { alen = addr_in(la, llen, &a); if (alen < 0) return -1; }
  else if (g_fd[fd].unconnected && g_fd[fd].has_peer) { a = g_fd[fd].peer; alen = sizeof a; }
  if (g_async && g_fd[fd].type == SOCK_DGRAM && len <= ASYNC_MAX) {
    mutexLock(&g_aq_lock);
    if (g_aq_tail - g_aq_head < ASYNC_SLOTS) {
      AsyncPkt *p = &g_aq[g_aq_tail % ASYNC_SLOTS];
      p->fd = fd; p->flags = flags; p->alen = alen; p->len = (uint32_t)len;
      if (alen) p->a = a;
      memcpy(p->data, buf, len);
      g_aq_tail++;
      g_aq_queued++;
      condvarWakeAll(&g_aq_cv);
      mutexUnlock(&g_aq_lock);
      pr_note(1, fd, buf, len);
      return (long)len;
    }
    g_aq_sync++;
    mutexUnlock(&g_aq_lock);
  }
  long r = sendto_sync(fd, buf, len, flags, &a, alen, 0);
  if (r >= 0) pr_note(1, fd, buf, len);
  return r;
}
long send_fake(int fd, const void *buf, size_t len, int flags) { return sendto_fake(fd, buf, len, flags, NULL, 0); }

long recvfrom_fake(int fd, void *buf, size_t len, int flags, void *la, unsigned *llen) {
  nx_sig_safepoint();   /* non-blocking callers loop on EAGAIN without reaching wait_fd() */
  if (!nx_net_is_socket(fd)) FAIL_LINUX(LE_ENOTSOCK);
  int bf = map_msg_flags(flags);
  int rfd = sock_real(fd);
  for (;;) {
    struct sockaddr_in a; socklen_t al = sizeof a;
    memset(&a, 0, sizeof a);
    net_begin("recv");
    ssize_t r = recvfrom(rfd, buf, ipc_cap(fd, len, 0), bf, (struct sockaddr *)&a, &al);
    net_end();
    if (r >= 0) {
      /* udp_unconnected: a real connected UDP socket only delivers datagrams
       * from its peer; emulate that by dropping any other source. */
      if (g_fd[fd].unconnected && g_fd[fd].has_peer && !(flags & L_MSG_PEEK) &&
          (a.sin_addr.s_addr != g_fd[fd].peer.sin_addr.s_addr || a.sin_port != g_fd[fd].peer.sin_port))
        continue;
      if (la && llen) addr_out((struct sockaddr *)&a, la, llen);
      pr_note(0, fd, buf, (size_t)r);
      return r;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      static unsigned nerr;
      if (nerr++ < 16) debugPrintf("[net] recv fd %d failed errno=%d\n", fd, errno);
      FAIL_NX();
    }
    if (!sock_blocking(fd, flags)) FAIL_LINUX(LE_EAGAIN);
    int w = wait_fd(rfd, POLLIN, g_fd[fd].rcvtimeo_ms);
    if (w < 0) return -1;
    if (w == 0) FAIL_LINUX(LE_EAGAIN);   /* SO_RCVTIMEO expiry == EAGAIN on Linux */
  }
}
long recv_fake(int fd, void *buf, size_t len, int flags) { return recvfrom_fake(fd, buf, len, flags, NULL, NULL); }

/* Linux struct msghdr / iovec (LP64). */
typedef struct { void *base; size_t len; } l_iovec;
typedef struct {
  void *name; uint32_t namelen; uint32_t _pad;
  l_iovec *iov; size_t iovlen;
  void *control; size_t controllen;
  int flags;
} l_msghdr;

long sendmsg_fake(int fd, const l_msghdr *m, int flags) {
  if (!m) FAIL_LINUX(LE_EFAULT);
  size_t total = 0;
  for (size_t i = 0; i < m->iovlen; i++) total += m->iov[i].len;
  uint8_t stackbuf[2048];
  uint8_t *b = total <= sizeof stackbuf ? stackbuf : malloc(total);
  if (!b) FAIL_LINUX(LE_ENOMEM);
  size_t o = 0;
  for (size_t i = 0; i < m->iovlen; i++) { memcpy(b + o, m->iov[i].base, m->iov[i].len); o += m->iov[i].len; }
  long r = sendto_fake(fd, b, total, flags, m->name, m->name ? m->namelen : 0);
  if (b != stackbuf) free(b);
  return r;
}

long recvmsg_fake(int fd, l_msghdr *m, int flags) {
  if (!m) FAIL_LINUX(LE_EFAULT);
  size_t total = 0;
  for (size_t i = 0; i < m->iovlen; i++) total += m->iov[i].len;
  uint8_t *b = malloc(total ? total : 1);
  if (!b) FAIL_LINUX(LE_ENOMEM);
  unsigned nl = m->namelen;
  long r = recvfrom_fake(fd, b, total, flags, m->name, m->name ? &nl : NULL);
  if (r >= 0) {
    size_t o = 0;
    for (size_t i = 0; i < m->iovlen && o < (size_t)r; i++) {
      size_t c = m->iov[i].len < (size_t)r - o ? m->iov[i].len : (size_t)r - o;
      memcpy(m->iov[i].base, b + o, c); o += c;
    }
    if (m->name) m->namelen = nl;
    m->controllen = 0;
    m->flags = 0;
  }
  free(b);
  return r;
}

int shutdown_fake(int fd, int how) {
  if (!nx_net_is_socket(fd)) FAIL_LINUX(LE_ENOTSOCK);
  if (shutdown(sock_real(fd), how) < 0) FAIL_NX();
  return 0;
}

int setsockopt_fake(int fd, int level, int opt, const void *val, unsigned len) {
  if (!nx_net_is_socket(fd)) FAIL_LINUX(LE_ENOTSOCK);
  {  /* what Photon/IL2CPP set on their sockets: the soak does not set any (v0.1.28) */
    static int n;
    if (n++ < 48)
      debugPrintf("[net] setsockopt fd %d (%s) level %d opt %d len %u val %d\n", fd,
                  g_fd[fd].type == SOCK_DGRAM ? "udp" : "tcp", level, opt, len,
                  (val && len >= 4) ? *(const int *)val : -1);
  }
  if (level == L_SOL_SOCKET && (opt == 20 || opt == 21) && val && len >= 16) {
    const int64_t *tv = val;   /* struct timeval {long sec; long usec;} */
    int ms = (int)(tv[0] * 1000 + tv[1] / 1000);
    if (opt == 20) g_fd[fd].rcvtimeo_ms = ms; else g_fd[fd].sndtimeo_ms = ms;
    return 0;                  /* emulated; the libnx socket stays non-blocking */
  }
  if (level == L_IPPROTO_IPV6) FAIL_LINUX(LE_ENOPROTOOPT);
  int bl, bo;
  if (map_sockopt(level, opt, &bl, &bo) < 0) {
    static int warned;
    if (warned++ < 8) debugPrintf("[net] setsockopt(level=%d,opt=%d) not mapped -- ignored\n", level, opt);
    return 0;                  /* Linux-only tuning knobs: pretend success */
  }
  if (setsockopt(sock_real(fd), bl, bo, val, len) < 0) {
    int e = errno;
    if (bo == SO_REUSEPORT || bo == IP_TOS) return 0;   /* advisory */
    debugPrintf("[net] setsockopt(%d,%d) failed errno=%d\n", bl, bo, e);
    errno = nx_errno_to_linux(e);
    return -1;
  }
  return 0;
}

int getsockopt_fake(int fd, int level, int opt, void *val, unsigned *len) {
  if (!nx_net_is_socket(fd)) FAIL_LINUX(LE_ENOTSOCK);
  if (level == L_SOL_SOCKET && (opt == 20 || opt == 21) && val && len && *len >= 16) {
    int ms = opt == 20 ? g_fd[fd].rcvtimeo_ms : g_fd[fd].sndtimeo_ms;
    int64_t *tv = val; tv[0] = ms / 1000; tv[1] = (ms % 1000) * 1000; *len = 16;
    return 0;
  }
  int bl, bo;
  if (map_sockopt(level, opt, &bl, &bo) < 0) FAIL_LINUX(LE_ENOPROTOOPT);
  socklen_t l = len ? *len : 0;
  if (getsockopt(sock_real(fd), bl, bo, val, &l) < 0) FAIL_NX();
  if (len) *len = l;
  if (bo == SO_ERROR && val) *(int *)val = bsd_errno_to_linux(*(int *)val);
  if (bo == SO_TYPE && val) { /* SOCK_* values agree */ }
  return 0;
}

int getsockname_fake(int fd, void *la, unsigned *llen) {
  if (!nx_net_is_socket(fd)) FAIL_LINUX(LE_ENOTSOCK);
  struct sockaddr_in a; socklen_t al = sizeof a;
  if (getsockname(sock_real(fd), (struct sockaddr *)&a, &al) < 0) FAIL_NX();
  addr_out((struct sockaddr *)&a, la, llen);
  return 0;
}

int getpeername_fake(int fd, void *la, unsigned *llen) {
  if (!nx_net_is_socket(fd)) FAIL_LINUX(LE_ENOTSOCK);
  /* un-connected UDP socket: the bsd socket has no peer; return the one we kept. */
  if (g_fd[fd].unconnected && g_fd[fd].has_peer) {
    addr_out((struct sockaddr *)&g_fd[fd].peer, la, llen);
    return 0;
  }
  struct sockaddr_in a; socklen_t al = sizeof a;
  if (getpeername(sock_real(fd), (struct sockaddr *)&a, &al) < 0) FAIL_NX();
  addr_out((struct sockaddr *)&a, la, llen);
  return 0;
}

int socketpair_fake(int d, int t, int p, int sv[2]) { (void)d; (void)t; (void)p; (void)sv; FAIL_LINUX(LE_EOPNOTSUPP); }

/* read/write/close routed here by libc_shim.c for socket fds. */
long nx_net_read(int fd, void *buf, size_t n) { return recvfrom_fake(fd, buf, n, 0, NULL, NULL); }
long nx_net_write(int fd, const void *buf, size_t n) { return sendto_fake(fd, buf, n, 0, NULL, 0); }

/* ------------------------------------------------------- fcntl / ioctl */
int fcntl_fake(int fd, int cmd, ...) {
  va_list va; va_start(va, cmd);
  long arg = va_arg(va, long);
  va_end(va);
  if (nx_net_is_socket(fd)) {
    switch (cmd) {
      case L_F_GETFL: return 2 /* O_RDWR */ | (g_fd[fd].user_nonblock ? L_O_NONBLOCK : 0);
      case L_F_SETFL: g_fd[fd].user_nonblock = (arg & L_O_NONBLOCK) ? 1 : 0; return 0;
      case L_F_GETFD: return 0;
      case L_F_SETFD: return 0;
    }
    return 0;
  }
  if (cmd == L_F_GETFL) return 2;
  return 0;   /* files / fake pipes: flags are not meaningful here */
}

int ioctl_fake(int fd, unsigned long req, ...) {
  va_list va; va_start(va, req);
  void *argp = va_arg(va, void *);
  va_end(va);
  if (!nx_net_is_socket(fd)) FAIL_LINUX(LE_ENOTSOCK);
  if (req == L_FIONBIO) {
    g_fd[fd].user_nonblock = (argp && *(int *)argp) ? 1 : 0;
    return 0;
  }
  if (req == L_FIONREAD) {
    int n = 0;
    int rfd = sock_real(fd);
    if (ioctl(rfd, FIONREAD, &n) < 0) {
      /* fallback: size of the next datagram via a non-blocking peek */
      static uint8_t peek[65536];
      static int logged;
      if (!logged++) debugPrintf("[net] FIONREAD refused by bsd (errno %d): peeking %u bytes instead\n", errno,
                                 (unsigned)ipc_cap(fd, sizeof peek, 0));
      ssize_t r = recv(rfd, peek, ipc_cap(fd, sizeof peek, 0), MSG_PEEK | MSG_DONTWAIT);
      n = r > 0 ? (int)r : 0;
    }
    if (argp) *(int *)argp = n;
    return 0;
  }
  FAIL_LINUX(LE_EINVAL);
}

/* ------------------------------------------------------- poll / select */
typedef struct { int fd; short events; short revents; } l_pollfd;   /* same as BSD */

int poll_fake(l_pollfd *fds, unsigned long nfds, int timeout) {
  /* Safepoint on entry, not only between slices: a receive thread that polls
   * with a 0..10 ms timeout in a loop returns through the timeout check below
   * every time, and a GC stop-the-world waited forever for its ack. */
  nx_sig_safepoint();
  if (nfds == 0) {
    if (timeout > 0) { nx_sig_sleep_ms(timeout); }
    return 0;
  }
  struct pollfd *bp = alloca(nfds * sizeof *bp);
  uint64_t start = now_ms();
  for (;;) {
    int nsock = 0, ready = 0;
    for (unsigned long i = 0; i < nfds; i++) {
      fds[i].revents = 0;
      int fd = fds[i].fd;
      if (fd < 0) continue;
      if (nx_net_is_socket(fd)) {
        bp[nsock].fd = sock_real(fd); bp[nsock].events = fds[i].events; bp[nsock].revents = 0; nsock++;
      } else if (fakefd_is_fake(fd)) {
        int r = fakefd_poll(fd, fds[i].events);
        if (r) { fds[i].revents = (short)r; ready++; }
      } else {
        fds[i].revents = fds[i].events & (POLLIN | POLLOUT);   /* regular file: always ready */
        if (fds[i].revents) ready++;
      }
    }
    int slice = 0;
    if (!ready) {
      int64_t left = -1;
      if (timeout >= 0) { left = (int64_t)timeout - (int64_t)(now_ms() - start); if (left < 0) left = 0; }
      slice = slice_for(left > 0x7fffffff ? 0x7fffffff : (int)left);
    }
    if (nsock) {
      net_begin("poll");
      int r = bsd_poll(bp, nsock, nsock && !ready ? slice : 0);
      net_end();
      if (r < 0 && errno != EINTR) FAIL_NX();
      int k = 0;
      for (unsigned long i = 0; i < nfds; i++) {
        if (fds[i].fd >= 0 && nx_net_is_socket(fds[i].fd)) {
          if (bp[k].revents) { fds[i].revents = bp[k].revents; ready++; }
          k++;
        }
      }
    } else if (!ready && slice > 0) {
      nx_sig_sleep_ms(slice > 2 ? 2 : slice);
    }
    if (ready) return ready;
    if (timeout >= 0 && now_ms() - start >= (uint64_t)timeout) return 0;
    nx_sig_safepoint();
  }
}

/* Linux fd_set: 1024 bits. struct timeval {long sec; long usec}. */
int select_fake(int n, uint64_t *rd, uint64_t *wr, uint64_t *ex, int64_t *tv) {
  l_pollfd pf[64];
  int np = 0;
  if (n > 1024) n = 1024;
  for (int fd = 0; fd < n && np < 64; fd++) {
    short ev = 0;
    if (rd && (rd[fd / 64] >> (fd % 64) & 1)) ev |= POLLIN;
    if (wr && (wr[fd / 64] >> (fd % 64) & 1)) ev |= POLLOUT;
    if (ex && (ex[fd / 64] >> (fd % 64) & 1)) ev |= POLLPRI;
    if (ev) { pf[np].fd = fd; pf[np].events = ev; pf[np].revents = 0; np++; }
  }
  int to = tv ? (int)(tv[0] * 1000 + tv[1] / 1000) : -1;
  int r = poll_fake(pf, (unsigned long)np, to);
  if (r < 0) return -1;
  if (rd) memset(rd, 0, (size_t)((n + 63) / 64) * 8);
  if (wr) memset(wr, 0, (size_t)((n + 63) / 64) * 8);
  if (ex) memset(ex, 0, (size_t)((n + 63) / 64) * 8);
  int cnt = 0;
  for (int i = 0; i < np; i++) {
    int fd = pf[i].fd;
    if (rd && (pf[i].revents & (POLLIN | POLLHUP | POLLERR))) { rd[fd / 64] |= 1ull << (fd % 64); cnt++; }
    if (wr && (pf[i].revents & (POLLOUT | POLLERR))) { wr[fd / 64] |= 1ull << (fd % 64); cnt++; }
    if (ex && (pf[i].revents & POLLPRI)) { ex[fd / 64] |= 1ull << (fd % 64); cnt++; }
  }
  return cnt;
}

/* ----------------------------------------------------------------- DNS */
typedef struct l_addrinfo {
  int ai_flags, ai_family, ai_socktype, ai_protocol;
  uint32_t ai_addrlen;
  char *ai_canonname;
  void *ai_addr;
  struct l_addrinfo *ai_next;
} l_addrinfo;

#define L_AI_PASSIVE     0x1
#define L_AI_CANONNAME   0x2
#define L_AI_NUMERICHOST 0x4
#define L_AI_NUMERICSERV 0x400
#define L_EAI_FAMILY 5
#define L_EAI_MEMORY 6
#define L_EAI_NONAME 8

int getaddrinfo_fake(const char *node, const char *svc, const l_addrinfo *hints, l_addrinfo **res) {
  struct addrinfo h, *r = NULL;
  memset(&h, 0, sizeof h);
  h.ai_family = AF_INET;
  if (hints) {
    if (hints->ai_family == L_AF_INET6) {
      debugPrintf("[net] getaddrinfo(%s): AF_INET6 requested -- no IPv6 on Switch\n", node ? node : "(null)");
      return L_EAI_FAMILY;
    }
    h.ai_socktype = hints->ai_socktype;
    h.ai_protocol = hints->ai_protocol;
    if (hints->ai_flags & L_AI_PASSIVE) h.ai_flags |= AI_PASSIVE;
    if (hints->ai_flags & L_AI_CANONNAME) h.ai_flags |= AI_CANONNAME;
    if (hints->ai_flags & L_AI_NUMERICHOST) h.ai_flags |= AI_NUMERICHOST;
    if (hints->ai_flags & L_AI_NUMERICSERV) h.ai_flags |= AI_NUMERICSERV;
  }
  if (!g_net_up) return L_EAI_NONAME;
  uint64_t t0 = now_ms();
  net_begin("dns");
  int rc = getaddrinfo(node, svc, &h, &r);
  net_end();
  g_stat_dns++;
  if (rc != 0 || !r) {
    g_stat_dns_fail++;
    debugPrintf("[net] getaddrinfo(%s, %s) FAILED rc=%d (%s) after %llu ms\n", node ? node : "(null)",
                svc ? svc : "(null)", rc, gai_strerror(rc), (unsigned long long)(now_ms() - t0));
    if (r) freeaddrinfo(r);
    return rc ? rc : L_EAI_NONAME;
  }
  l_addrinfo *head = NULL, **tail = &head;
  int n = 0;
  for (struct addrinfo *p = r; p; p = p->ai_next) {
    if (p->ai_family != AF_INET || !p->ai_addr) continue;
    l_addrinfo *o = calloc(1, sizeof *o + sizeof(l_sockaddr_in));
    if (!o) break;
    l_sockaddr_in *sa = (l_sockaddr_in *)(o + 1);
    unsigned sl = sizeof *sa;
    addr_out(p->ai_addr, sa, &sl);
    o->ai_flags = hints ? hints->ai_flags : 0;
    o->ai_family = L_AF_INET;
    o->ai_socktype = p->ai_socktype ? p->ai_socktype : (hints ? hints->ai_socktype : 0);
    o->ai_protocol = p->ai_protocol ? p->ai_protocol : (hints ? hints->ai_protocol : 0);
    o->ai_addrlen = sizeof *sa;
    o->ai_addr = sa;
    if (p->ai_canonname) o->ai_canonname = strdup(p->ai_canonname);
    *tail = o; tail = &o->ai_next; n++;
    if (g_stat_dns <= 64 && n <= 4) {
      struct in_addr ia; ia.s_addr = sa->addr;
      debugPrintf("[net] getaddrinfo(%s) -> %s (%llu ms)\n", node ? node : "(null)", inet_ntoa(ia),
                  (unsigned long long)(now_ms() - t0));
    }
  }
  freeaddrinfo(r);
  if (!head) return L_EAI_NONAME;
  *res = head;
  return 0;
}

void freeaddrinfo_fake(l_addrinfo *a) {
  while (a) {
    l_addrinfo *n = a->ai_next;
    free(a->ai_canonname);
    free(a);
    a = n;
  }
}

const char *gai_strerror_fake(int e) { return gai_strerror(e); }

int getnameinfo_fake(const void *la, unsigned llen, char *host, unsigned hl, char *serv, unsigned sl, int flags) {
  (void)flags;
  struct sockaddr_in a;
  if (addr_in(la, llen, &a) < 0) return L_EAI_FAMILY;
  if (host && hl) inet_ntop(AF_INET, &a.sin_addr, host, hl);
  if (serv && sl) snprintf(serv, sl, "%u", ntohs(a.sin_port));
  return 0;
}

/* bionic struct hostent is the same shape as newlib's on LP64:
 * { char *h_name; char **h_aliases; int h_addrtype; int h_length; char **h_addr_list; } */
void *gethostbyname_fake(const char *name) {
  static struct { char *h_name; char **h_aliases; int h_addrtype; int h_length; char **h_addr_list; } he;
  static char namebuf[256];
  static uint32_t addrs[8];
  static char *addrp[9];
  static char *aliases[1];
  l_addrinfo hints = {0}, *res = NULL;
  hints.ai_family = L_AF_INET;
  if (getaddrinfo_fake(name, NULL, &hints, &res) != 0) return NULL;
  int n = 0;
  for (l_addrinfo *p = res; p && n < 8; p = p->ai_next) addrs[n++] = ((l_sockaddr_in *)p->ai_addr)->addr;
  freeaddrinfo_fake(res);
  for (int i = 0; i < n; i++) addrp[i] = (char *)&addrs[i];
  addrp[n] = NULL;
  snprintf(namebuf, sizeof namebuf, "%s", name ? name : "");
  he.h_name = namebuf; he.h_aliases = aliases; he.h_addrtype = L_AF_INET; he.h_length = 4; he.h_addr_list = addrp;
  return &he;
}

int gethostname_fake(char *name, size_t len) {
  if (name && len) snprintf(name, len, "nintendo-switch");
  return 0;
}

/* inet_* are pure and ABI-identical (in_addr is a u32 in network order). */
int inet_pton_fake(int af, const char *src, void *dst) {
  if (af == L_AF_INET) return inet_pton(AF_INET, src, dst);
  if (af == L_AF_INET6) { errno = LE_EAFNOSUPPORT; return -1; }
  errno = LE_EAFNOSUPPORT; return -1;
}
const char *inet_ntop_fake(int af, const void *src, char *dst, unsigned size) {
  if (af == L_AF_INET) return inet_ntop(AF_INET, src, dst, size);
  errno = LE_EAFNOSUPPORT;
  return NULL;
}
int inet_aton_fake(const char *cp, void *inp) { return inet_pton(AF_INET, cp, inp) == 1; }
uint32_t inet_addr_fake(const char *cp) { struct in_addr a; return inet_pton(AF_INET, cp, &a) == 1 ? a.s_addr : 0xffffffffu; }
char *inet_ntoa_fake(uint32_t in) { struct in_addr a; a.s_addr = in; return inet_ntoa(a); }

/* NetworkInterface enumeration: not available -- callers fall back. */
int getifaddrs_fake(void **ifap) { if (ifap) *ifap = NULL; errno = LE_EOPNOTSUPP; return -1; }
void freeifaddrs_fake(void *ifa) { (void)ifa; }
unsigned if_nametoindex_fake(const char *n) { (void)n; return 0; }
char *if_indextoname_fake(unsigned i, char *buf) { (void)i; if (buf) buf[0] = 0; return NULL; }
void *getservbyname_fake(const char *n, const char *p) { (void)n; (void)p; return NULL; }

/* --------------------------------------------------------------- init */
static void nifm_report_thread(void *arg) {
  (void)arg;
  if (R_FAILED(nifmInitialize(NifmServiceType_User))) {
    debugPrintf("[net] nifm unavailable -- cannot report the connection state\n");
    return;
  }
  NifmInternetConnectionType t; u32 strength = 0; NifmInternetConnectionStatus st;
  if (R_SUCCEEDED(nifmGetInternetConnectionStatus(&t, &strength, &st)))
    debugPrintf("[net] internet: type=%d (1=wifi 2=ethernet) strength=%u status=%d (4=connected)\n",
                (int)t, strength, (int)st);
  else
    debugPrintf("[net] internet: NOT CONNECTED (nifm reports no connection) -- online play will fail\n");
  u32 ip = 0;
  if (R_SUCCEEDED(nifmGetCurrentIpAddress(&ip))) {
    struct in_addr a; a.s_addr = ip;
    debugPrintf("[net] local IP %s\n", inet_ntoa(a));
  }
  /* nifmExit() deliberately not called: the session is tiny and exiting it
   * from a side thread raced the main thread's IPC under Eden. */
}

/* [debug] bsd_flood = N: N extra bsd requests a second from a side thread,
 * from frame 300 (main menu) on, with no game traffic. The freeze ~7 min into
 * online play always came after ~160k bsd requests at ~400 a second (29/10):
 * if the request count is the cause, N = 2000 brings it to ~80 s on the main
 * menu; if it is time online, nothing happens. [debug] bsd_flood_op = poll
 * (default: a 0 ms poll, like Photon's 5 ms receive poll, 61% of the game's
 * requests), sockopt (getsockopt, the smallest request) or pollwait (5 ms
 * polls back to back, like Photon's receive thread; the rate is ignored).
 * 29/10: 400k polls of 0 ms did not freeze; pollwait tells whether the time
 * spent waiting inside bsd is what counts (freeze after ~396 s of it). */
extern int diag_frame_no(void);
static int g_flood_rate, g_flood_op;     /* 0 poll, 1 sockopt, 2 pollwait */

static void flood_main(void *arg) {
  (void)arg;
  static uint8_t tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  install_bionic_tls(tls);
  nx_autopsy_name_self("bsd-flood");
  while (diag_frame_no() < 300) svcSleepThread(500000000ull);
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) { debugPrintf("[net] bsd flood: socket failed errno=%d\n", errno); return; }
  static const char *const OPS[] = { "poll", "getsockopt", "5 ms poll" };
  debugPrintf("[net] bsd flood: %d %s requests a second from frame %d on (heartbeat bsd: counts them)\n",
              g_flood_op == 2 ? 200 : g_flood_rate, OPS[g_flood_op], diag_frame_no());
  if (g_flood_op == 2) {                 /* a thread always waiting in bsd, as Photon's receive thread */
    for (;;) {
      struct pollfd p = { .fd = fd, .events = POLLIN, .revents = 0 };
      poll(&p, 1, 5);
    }
  }
  const int per_tick = g_flood_rate >= 100 ? g_flood_rate / 100 : 1;   /* 10 ms ticks */
  for (;;) {
    for (int i = 0; i < per_tick; i++) {
      if (g_flood_op == 1) {
        int v = 0; socklen_t l = sizeof v;
        getsockopt(fd, SOL_SOCKET, SO_TYPE, &v, &l);
      } else {
        struct pollfd p = { .fd = fd, .events = POLLIN, .revents = 0 };
        poll(&p, 1, 0);
      }
    }
    svcSleepThread(10000000ull);
  }
}

static void flood_start(void) {
  g_flood_rate = nxcfg_int("debug.bsd_flood", 0);
  if (!g_net_up || g_flood_rate <= 0) return;
  if (g_flood_rate > 20000) g_flood_rate = 20000;
  const char *op = nxcfg_get("debug.bsd_flood_op", "poll");
  g_flood_op = strcmp(op, "sockopt") == 0 ? 1 : strcmp(op, "pollwait") == 0 ? 2 : 0;
  static Thread th;
  if (R_SUCCEEDED(threadCreate(&th, flood_main, NULL, NULL, 0x4000, 0x3B, -2)) && R_SUCCEEDED(threadStart(&th)))
    debugPrintf("[net] bsd flood armed: %d requests a second once the main menu is up\n", g_flood_rate);
  else
    debugPrintf("[net] bsd flood: thread failed\n");
}

/* [debug] udp_soak = <PC IP>:<port> ----------------------------------------
 * Mimic Photon's UDP traffic with NO Photon at all, to tell the console's
 * network stack apart from Photon as the cause of the ~400 s freeze. From the
 * main menu on, a socket -- connected by default, like Photon (connect +
 * send); udp_soak_connect = 0 uses sendto to an explicit address instead --
 * carries ~20 small datagrams a second to a UDP echo on a PC
 * (tools/switch/udp_echo.php), while a second thread polls the socket 5 ms at
 * a time and drains it, exactly like Photon's receive thread. Run it on the
 * MAIN MENU (no online play) for ~10 min: a freeze there after ~400 s means
 * the console's network stack, not Photon. Raw libnx sockets (counted by the
 * bsd monitor), not our translation layer. Empty = off.
 * 02/11 on console: 24 min to the PC on the LAN, no freeze, while Photon
 * froze at ~400 s. Two differences were left, hence (v0.1.28):
 *   udp_soak_layer = 1  go through our translation layer (socket_fake,
 *                       connect_fake, send_fake, poll_fake, recvfrom_fake),
 *                       the path Photon's IL2CPP sockets take;
 *   udp_soak_dns = 1    send DNS queries for example.com, so a resolver on
 *                       the Internet (udp_soak = 1.1.1.1:53) answers them:
 *                       traffic to the Internet through the gateway, like
 *                       Photon's, instead of to the LAN;
 *   udp_soak_hz = N     datagrams a second (default 20; Photon in "Jouer"
 *                       sends ~4). */
static struct { int connected, layer, dns, hz; struct sockaddr_in dst; } g_soak;
static int g_soak_fd = -1;
static volatile uint32_t g_soak_rx;

/* One soak datagram into p (>= 40 bytes): a DNS query (udp_soak_dns) or a
 * Photon-ish header, so the capture looks familiar. Returns its length. */
static size_t soak_packet(uint8_t *p, uint32_t seq) {
  if (g_soak.dns) {
    static const uint8_t q[] = { 1, 0, 0, 1, 0, 0, 0, 0, 0, 0,   /* recursion desired, 1 question */
                                 7, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 3, 'c', 'o', 'm', 0,
                                 0, 1, 0, 1 };                    /* type A, class IN */
    p[0] = (uint8_t)(seq >> 8); p[1] = (uint8_t)seq;             /* query id */
    memcpy(p + 2, q, sizeof q);
    return 2 + sizeof q;
  }
  memset(p, 0, 40);
  p[0] = 0xF3; p[1] = 0x04;
  p[2] = (uint8_t)(seq >> 8); p[3] = (uint8_t)seq;
  return 40;
}

static l_sockaddr_in soak_lsa(void) {
  l_sockaddr_in s;
  memset(&s, 0, sizeof s);
  s.family = L_AF_INET; s.port = g_soak.dst.sin_port; s.addr = g_soak.dst.sin_addr.s_addr;
  return s;
}

static int soak_parse(const char *s, struct sockaddr_in *out) {
  char ip[64]; const char *c = strchr(s, ':');
  if (!c || (size_t)(c - s) >= sizeof ip) return 0;
  memcpy(ip, s, (size_t)(c - s)); ip[c - s] = 0;
  struct in_addr a;
  if (inet_pton(AF_INET, ip, &a) != 1) return 0;
  int port = atoi(c + 1);
  if (port <= 0 || port > 65535) return 0;
  memset(out, 0, sizeof *out);
  out->sin_family = AF_INET; out->sin_addr = a; out->sin_port = htons((uint16_t)port);
  return 1;
}

static void soak_recv_main(void *arg) {
  (void)arg;
  static uint8_t tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  install_bionic_tls(tls);
  nx_autopsy_name_self("udp-soak-rx");
  uint8_t b[2048];
  for (;;) {
    int fd = g_soak_fd;
    if (fd < 0) { svcSleepThread(50000000ull); continue; }
    if (g_soak.layer) {
      l_pollfd p = { fd, POLLIN, 0 };
      poll_fake(&p, 1, 5);
      if ((p.revents & POLLIN) && recvfrom_fake(fd, b, sizeof b, L_MSG_DONTWAIT, NULL, NULL) > 0) g_soak_rx++;
      continue;
    }
    struct pollfd p = { .fd = fd, .events = POLLIN, .revents = 0 };
    poll(&p, 1, 5);
    if (p.revents & POLLIN) {
      ssize_t r;
      if (g_soak.connected) r = recv(fd, b, sizeof b, 0);
      else { struct sockaddr_in a; socklen_t al = sizeof a; r = recvfrom(fd, b, sizeof b, 0, (struct sockaddr *)&a, &al); }
      if (r > 0) g_soak_rx++;
    }
  }
}

static void soak_main(void *arg) {
  (void)arg;
  static uint8_t tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  install_bionic_tls(tls);
  nx_autopsy_name_self("udp-soak-tx");
  while (diag_frame_no() < 300) svcSleepThread(500000000ull);
  l_sockaddr_in lsa = soak_lsa();
  int fd = g_soak.layer ? socket_fake(L_AF_INET, SOCK_DGRAM, 0) : socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) { debugPrintf("[net] udp_soak: socket failed errno=%d\n", errno); return; }
  if (nxcfg_int("debug.udp_soak_reuse", 0) == 1) {   /* the one option Photon sets on its socket (02/11 log) */
    int one = 1;
    if (g_soak.layer) setsockopt_fake(fd, L_SOL_SOCKET, 2, &one, sizeof one);
    else setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  }
  if (g_soak.connected) {
    int r = g_soak.layer ? connect_fake(fd, &lsa, sizeof lsa)
                         : connect(fd, (struct sockaddr *)&g_soak.dst, sizeof g_soak.dst);
    if (r < 0) debugPrintf("[net] udp_soak: connect failed errno=%d (sending anyway)\n", errno);
  }
  g_soak_fd = fd;
  struct in_addr da = g_soak.dst.sin_addr;
  debugPrintf("[net] udp_soak armed: %d datagrams/s (%s) to %s:%u (%s, %s), 5 ms receive poll, from frame %d\n",
              g_soak.hz, g_soak.dns ? "DNS queries" : "40 bytes", inet_ntoa(da), ntohs(g_soak.dst.sin_port),
              g_soak.connected ? "connected" : "sendto", g_soak.layer ? "our socket layer" : "raw libnx",
              diag_frame_no());
  static Thread rx;
  if (R_SUCCEEDED(threadCreate(&rx, soak_recv_main, NULL, NULL, 0x4000, 0x3B, -2))) threadStart(&rx);
  uint8_t pkt[64];
  uint32_t seq = 0;
  uint64_t next_note = now_ms() + 60000;
  for (;;) {
    size_t n = soak_packet(pkt, seq++);
    if (g_soak.layer) {
      if (g_soak.connected) send_fake(fd, pkt, n, 0);
      else sendto_fake(fd, pkt, n, 0, &lsa, sizeof lsa);
    } else if (g_soak.connected) {
      send(fd, pkt, n, 0);
    } else {
      sendto(fd, pkt, n, 0, (struct sockaddr *)&g_soak.dst, sizeof g_soak.dst);
    }
    if (now_ms() >= next_note) {         /* one line a minute: are the answers coming back? */
      next_note += 60000;
      debugPrintf("[net] udp_soak: %u sent, %u received\n", seq, g_soak_rx);
    }
    svcSleepThread(1000000000ull / (uint64_t)g_soak.hz);
  }
}

static void soak_start(void) {
  const char *s = nxcfg_get("debug.udp_soak", "");
  if (!g_net_up || !s || !*s) return;
  if (!soak_parse(s, &g_soak.dst)) { debugPrintf("[net] udp_soak: bad address '%s' (want ip:port)\n", s); return; }
  g_soak.connected = nxcfg_int("debug.udp_soak_connect", 1) != 0;
  g_soak.layer = nxcfg_int("debug.udp_soak_layer", 0) == 1;
  g_soak.dns = nxcfg_int("debug.udp_soak_dns", 0) == 1;
  g_soak.hz = nxcfg_int("debug.udp_soak_hz", 20);
  if (g_soak.hz < 1) g_soak.hz = 1; else if (g_soak.hz > 200) g_soak.hz = 200;
  static Thread th;
  if (R_SUCCEEDED(threadCreate(&th, soak_main, NULL, NULL, 0x8000, 0x3B, -2)) && R_SUCCEEDED(threadStart(&th)))
    debugPrintf("[net] udp_soak: thread armed (%s)\n", g_soak.connected ? "connected" : "sendto");
  else
    debugPrintf("[net] udp_soak: thread failed\n");
}

/* [net] photon_rebind_s = N: N s after a UDP socket is connected to its peer,
 * swap a fresh bsd socket under the same app fd (via g_fd[].redirect): the
 * source port changes, the kernel socket is new. RISK: Photon tracks the peer
 * by source IP:port, so the server very likely sees a new flow and the session
 * times out (~10 s DisconnectTimeout) -- run with photon_ring on to see the
 * server's reaction. Tests only whether a FRESH bsd socket resets the ~400 s
 * clock (i.e. whether the rot is tied to the individual socket's lifetime).
 * Off by default. */
static int g_rebind_done;

static void rebind_main(void *arg) {
  (void)arg;
  static uint8_t tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  install_bionic_tls(tls);
  nx_autopsy_name_self("udp-rebind");
  for (;;) {
    svcSleepThread(500000000ull);
    if (g_rebind_done) continue;
    int fd = g_photon_fd;
    if (fd < 0 || fd >= NFD || !g_fd[fd].is_sock || !g_fd[fd].has_peer || g_fd[fd].redirect) continue;
    uint64_t since = g_photon_since_ms;
    if (!since || now_ms() - since < (uint64_t)g_rebind_s * 1000ull) continue;
    int nb = socket(AF_INET, SOCK_DGRAM, 0);
    if (nb < 0) { g_rebind_done = 1; continue; }
    if (nb >= NFD) { close(nb); g_rebind_done = 1; continue; }
    set_nx_nonblock(nb);
    if (!g_fd[fd].unconnected)
      connect(nb, (struct sockaddr *)&g_fd[fd].peer, sizeof g_fd[fd].peer);
    memset(&g_fd[nb], 0, sizeof g_fd[nb]);
    g_fd[nb].is_sock = 1; g_fd[nb].type = SOCK_DGRAM;
    __atomic_store_n(&g_fd[fd].redirect, (int16_t)nb, __ATOMIC_RELEASE);
    shutdown(fd, SHUT_RDWR);        /* stop the old kernel socket from buffering */
    g_rebind_done = 1;
    debugPrintf("[net] photon_rebind: fd %d now rides a fresh socket (bsd fd %d, new source port) after %d s\n",
                fd, nb, g_rebind_s);
  }
}

static void rebind_start(void) {
  if (!g_net_up || g_rebind_s <= 0) return;
  static Thread th;
  if (R_SUCCEEDED(threadCreate(&th, rebind_main, NULL, NULL, 0x4000, 0x3B, -2)) && R_SUCCEEDED(threadStart(&th)))
    debugPrintf("[net] photon_rebind armed: fresh UDP socket after %d s connected (RISK: may disconnect Photon)\n", g_rebind_s);
}

void nx_net_init(void) {
  /* Larger UDP buffers than the libnx default: Quantum sends bursts of input
   * and snapshot packets; the default 8 KB receive window drops them. */
  SocketInitConfig cfg = *socketGetDefaultInitConfig();
  const int libnx_cfg = nxcfg_int("net.socket_config", 0) == 1;   /* 1 = libnx's own defaults */
  const int chiaki = nxcfg_int("net.chiaki_cfg", 0) == 1;         /* chiaki-ng's real-time UDP config */
  if (chiaki) {
    cfg.udp_tx_buf_size = 0x40000;
    cfg.udp_rx_buf_size = 0x40000;
    cfg.tcp_tx_buf_size = 0x8000;
    cfg.tcp_rx_buf_size = 0x10000;
    cfg.sb_efficiency = 8;
  } else if (!libnx_cfg) {
    cfg.udp_tx_buf_size = 0x2400;
    cfg.udp_rx_buf_size = 0xA500;
    cfg.tcp_tx_buf_size = 0x8000;
    cfg.tcp_rx_buf_size = 0x10000;
    cfg.sb_efficiency = 8;
  }
  /* libnx allows up to 16 sessions; the console shares only 15 bsd:u sessions
   * (fw 18.0.0+), so a high count can fail -- step down instead of losing the
   * buffer config to socketInitializeDefault. */
  /* chiaki_cfg forces chiaki's 16 sessions (its config includes them), even
   * if the template still carries bsd_sessions = 3; the step-down below copes
   * with the console's 15-session limit. Otherwise honour bsd_sessions. */
  int ns = chiaki ? 16 : nxcfg_int("net.bsd_sessions", 3);
  if (ns < 1) ns = 1; else if (ns > 16) ns = 16;
  Result rc = 1;
  for (int try_ns = ns; ; ) {
    cfg.num_bsd_sessions = (u32)try_ns;
    rc = socketInitialize(&cfg);
    if (R_SUCCEEDED(rc) || try_ns <= 3) break;
    debugPrintf("[net] socketInitialize(%d bsd sessions) failed 0x%x -- fewer sessions\n", try_ns, rc);
    try_ns = try_ns > 15 ? 15 : try_ns > 8 ? 8 : 3;   /* console shares 15 bsd:u */
  }
  if (R_FAILED(rc)) {
    debugPrintf("[net] socketInitialize(custom) failed 0x%x -- retrying with defaults\n", rc);
    rc = socketInitializeDefault();
    cfg.num_bsd_sessions = 3;
  }
  g_net_up = R_SUCCEEDED(rc);
  g_slice_ms = nxcfg_int("net.poll_slice_ms", 10);
  if (g_slice_ms < 0) g_slice_ms = 10;
  if (g_slice_ms > 1000) g_slice_ms = 1000;
  g_small_ipc = nxcfg_int("net.small_ipc", 0) == 1;
  g_local_wait = nxcfg_int("net.local_wait", 0) == 1;
  g_pr_on = nxcfg_int("debug.photon_ring", 0) == 1;
  g_udp_unconnected = nxcfg_int("net.udp_unconnected", 0) == 1;
  g_rebind_s = nxcfg_int("net.photon_rebind_s", 0);
  if (g_rebind_s < 0) g_rebind_s = 0;
  debugPrintf("[net] BSD sockets %s (rc=0x%x): %s config (sb_efficiency %u, udp %u/%u K, tcp %u/%u K), %u bsd sessions, "
              "poll slice %s%d ms%s, small_ipc %d, local_wait %d, udp_unconnected %d, rebind %ds, bsd pointer buffer 0x%x\n",
              g_net_up ? "up" : "DOWN", rc, chiaki ? "chiaki" : libnx_cfg ? "libnx default" : "game",
              cfg.sb_efficiency, cfg.udp_tx_buf_size >> 10, cfg.udp_rx_buf_size >> 10, cfg.tcp_tx_buf_size >> 10,
              cfg.tcp_rx_buf_size >> 10, cfg.num_bsd_sessions, g_slice_ms ? "" : "none (cap ",
              g_slice_ms ? g_slice_ms : 1000, g_slice_ms ? "" : ")", g_small_ipc, g_local_wait,
              g_udp_unconnected, g_rebind_s, nx_bsdmon_pointer_size());
  flood_start();
  soak_start();
  rebind_start();
  if (g_net_up && nxcfg_int("net.async_send", 1) == 1) {
    mutexInit(&g_aq_lock);
    condvarInit(&g_aq_cv);
    static Thread th;
    if (R_SUCCEEDED(threadCreate(&th, async_main, NULL, NULL, 0x8000, 0x2C, -2)) && R_SUCCEEDED(threadStart(&th))) {
      g_async = 1;
      debugPrintf("[net] async UDP sends: queue of %d datagrams up to %d bytes, sender thread\n", ASYNC_SLOTS, ASYNC_MAX);
    }
  }

  /* Connection diagnostics on a side thread: nifm IPC must never be able to
   * stall boot (it did under the Eden emulator). */
  static Thread th;
  if (R_SUCCEEDED(threadCreate(&th, nifm_report_thread, NULL, NULL, 0x8000, 0x3B, -2)))
    threadStart(&th);
}

void nx_net_report(void) {
  debugPrintf("[net] stats: %u sockets created, %u DNS lookups (%u failed)\n",
              g_stat_sock, g_stat_dns, g_stat_dns_fail);
  if (g_async)
    debugPrintf("[net] async sends: %llu queued, %llu sent synchronously (queue full), slowest bsd send %llu ms\n",
                (unsigned long long)g_aq_queued, (unsigned long long)g_aq_sync, (unsigned long long)g_aq_max_ms);
  extern unsigned nx_bsdmon_report(char *buf, unsigned len, int last_ops);
  static char rep[2048];
  nx_bsdmon_report(rep, sizeof rep, 0);
  debugPrintf("[net] %s", rep);
  if (g_pr_on) {
    rep[0] = 0;
    nx_net_photon_report(rep, sizeof rep, 30000);
    if (rep[0]) debugPrintf("[net] %s", rep);
  }
}

void nx_net_exit(void) {
  if (g_async) async_drain();   /* the sender thread must not be inside bsd at socketExit */
  if (g_net_up) socketExit();
  g_net_up = 0;
}

/* ------------------------------------------------------- import table */
DynLibFunction nx_net_exports[] = {
  { "socket", (uintptr_t)&socket_fake },
  { "connect", (uintptr_t)&connect_fake },
  { "bind", (uintptr_t)&bind_fake },
  { "listen", (uintptr_t)&listen_fake },
  { "accept", (uintptr_t)&accept_fake },
  { "accept4", (uintptr_t)&accept4_fake },
  { "send", (uintptr_t)&send_fake },
  { "recv", (uintptr_t)&recv_fake },
  { "sendto", (uintptr_t)&sendto_fake },
  { "recvfrom", (uintptr_t)&recvfrom_fake },
  { "sendmsg", (uintptr_t)&sendmsg_fake },
  { "recvmsg", (uintptr_t)&recvmsg_fake },
  { "shutdown", (uintptr_t)&shutdown_fake },
  { "setsockopt", (uintptr_t)&setsockopt_fake },
  { "getsockopt", (uintptr_t)&getsockopt_fake },
  { "getsockname", (uintptr_t)&getsockname_fake },
  { "getpeername", (uintptr_t)&getpeername_fake },
  { "socketpair", (uintptr_t)&socketpair_fake },
  { "fcntl", (uintptr_t)&fcntl_fake },
  { "ioctl", (uintptr_t)&ioctl_fake },
  { "poll", (uintptr_t)&poll_fake },
  { "select", (uintptr_t)&select_fake },
  { "getaddrinfo", (uintptr_t)&getaddrinfo_fake },
  { "freeaddrinfo", (uintptr_t)&freeaddrinfo_fake },
  { "gai_strerror", (uintptr_t)&gai_strerror_fake },
  { "getnameinfo", (uintptr_t)&getnameinfo_fake },
  { "gethostbyname", (uintptr_t)&gethostbyname_fake },
  { "gethostname", (uintptr_t)&gethostname_fake },
  { "inet_pton", (uintptr_t)&inet_pton_fake },
  { "inet_ntop", (uintptr_t)&inet_ntop_fake },
  { "inet_aton", (uintptr_t)&inet_aton_fake },
  { "inet_addr", (uintptr_t)&inet_addr_fake },
  { "inet_ntoa", (uintptr_t)&inet_ntoa_fake },
  { "getifaddrs", (uintptr_t)&getifaddrs_fake },
  { "freeifaddrs", (uintptr_t)&freeifaddrs_fake },
  { "if_nametoindex", (uintptr_t)&if_nametoindex_fake },
  { "if_indextoname", (uintptr_t)&if_indextoname_fake },
  { "getservbyname", (uintptr_t)&getservbyname_fake },
};
const int nx_net_exports_num = sizeof(nx_net_exports) / sizeof(nx_net_exports[0]);
