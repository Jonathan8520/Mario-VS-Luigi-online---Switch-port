/* nx_bsdmon.c -- every request this process makes to the bsd (sockets) service,
 * counted and the last ones kept, for the heartbeat and the freeze autopsy.
 *
 * WHY
 * Console, 29/10: the whole-console freeze comes ~390-420 s into online play,
 * after 157-162 thousand network calls; at the freeze the bsd, fs and nifm
 * services stop answering while small requests to other services still pass.
 * One lead is the size of the requests: libnx sends a buffer by copy through
 * the service's pointer buffer when it fits (cmifRequestIn/OutAutoBuffer,
 * size <= server_pointer_size), and otherwise has the kernel MAP that part of
 * our memory into the service ("MapAlias"). This module says, per bsd command,
 * how many requests there were, how many were mapped, the bytes moved, the
 * failures and the time spent, and keeps the last 128 operations (thread,
 * command, socket, peer, size, flags, result, errno, duration).
 *
 * Every libnx bsd* call reaches us through ld --wrap (Makefile): the newlib
 * socket layer (libnx socket.c) and nx_net.c alike. Plain counters and a ring
 * written with relaxed atomics: no lock, no allocation, no log.
 *
 * MIT -- part of mvlo_nx. */
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <switch.h>
#include <switch/services/bsd.h>

#include "nx_bfmt.h"

static inline uint64_t tick(void) { return armGetSystemTick(); }
static inline uint64_t tick_us(uint64_t t) { return armTicksToNs(t) / 1000ull; }

/* ---- per command ---------------------------------------------------------- */
enum { B_SOCKET, B_CLOSE, B_CONNECT, B_BIND, B_SEND, B_SENDTO, B_RECV, B_RECVFROM, B_POLL, B_SELECT,
       B_IOCTL, B_FCNTL, B_SETOPT, B_GETOPT, B_SHUTDOWN, B_GETNAME, B_PEERNAME, B_ACCEPT, B_LISTEN,
       B_WRITE, B_READ, B_N };
static const char *const b_name[B_N] = {
  "socket", "close", "connect", "bind", "send", "sendto", "recv", "recvfrom", "poll", "select",
  "ioctl", "fcntl", "setsockopt", "getsockopt", "shutdown", "getsockname", "getpeername", "accept", "listen",
  "write", "read",
};
typedef struct { uint64_t calls, failed, mapped, bytes, us, max_us; } BStat;
static BStat g_b[B_N];
static volatile uint64_t g_tx, g_rx;
static volatile uint32_t g_inflight;

/* ---- errno histogram (raw bsd-service errno, g_bsdErrno) -----------------
 * To spot a resource exhaustion rising before the freeze: ENOBUFS ("No buffer
 * space available") is what hung sys-ftpd (DECISIONS 61). libnx states the bsd
 * service uses the LINUX errno definitions (socket.c _convert_errno), so
 * ENOBUFS = 105, EAGAIN = 11. The port's own bsd_errno_to_linux() instead
 * treats SO_ERROR as FreeBSD-numbered, so to be safe we ALSO watch the FreeBSD
 * ENOBUFS (55): whichever the console really uses, a rise is visible. The raw
 * value is in the per-op ring too. EAGAIN = normal back-pressure (not a
 * warning). */
static const struct { int e; const char *n; uint8_t beacon; } k_err[] = {
  { 11, "EAGAIN", 0 },      { 105, "ENOBUFS", 1 },      { 104, "ECONNRESET", 1 },
  { 111, "ECONNREFUSED", 1},{ 110, "ETIMEDOUT", 1 },    { 90,  "EMSGSIZE", 1 },
  { 99, "EADDRNOTAVAIL", 1},{ 101, "ENETUNREACH", 1 },  { 113, "EHOSTUNREACH", 1 },
  { 98, "EADDRINUSE", 1 },  { 22,  "EINVAL", 1 },       { 9,   "EBADF", 1 },
  { 35, "EAGAIN_bsd", 0 },  { 55,  "ENOBUFS_bsd", 1 },  { 61,  "ECONNREFUSED_bsd", 1 },
};
#define NERR ((int)(sizeof k_err / sizeof k_err[0]))
static volatile uint64_t g_err[NERR + 1];   /* last bucket = any other errno */

/* ---- per socket: type and peer -------------------------------------------- */
#define NFD 128
static struct { uint8_t type; uint32_t ip; uint16_t port; } g_fd[NFD];

/* ---- the last operations -------------------------------------------------- */
#define RING 128
typedef struct {
  uint64_t t;            /* start, system ticks */
  uint32_t thread;       /* thread handle (names in the autopsy's thread list) */
  uint8_t  op, mapped;
  int16_t  fd;
  uint32_t len, flags;
  int32_t  ret, err;
  uint32_t us;
} BOp;
static BOp g_ring[RING];
static volatile uint32_t g_ring_n;

/* Pointer buffer of the bsd service: requests up to this size are copied,
 * bigger ones are mapped. 0 until known. */
static uint32_t ptr_size(void) {
  static uint32_t s;
  if (!s) { Service *srv = bsdGetServiceSession(); if (srv && srv->session) s = srv->pointer_buffer_size; }
  return s;
}
uint32_t nx_bsdmon_pointer_size(void) { return ptr_size(); }

static inline uint64_t b_begin(void) {
  __atomic_add_fetch(&g_inflight, 1, __ATOMIC_RELAXED);
  return tick();
}
/* size: the biggest buffer of the request (decides copy vs map). */
static void b_end(int op, uint64_t t0, int fd, size_t size, uint32_t flags, long ret) {
  uint64_t us = tick_us(tick() - t0);
  BStat *b = &g_b[op];
  __atomic_add_fetch(&b->calls, 1, __ATOMIC_RELAXED);
  __atomic_add_fetch(&b->us, us, __ATOMIC_RELAXED);
  if (us > b->max_us) b->max_us = us;
  uint32_t ps = ptr_size();
  int mapped = ps && size > ps;
  if (mapped) __atomic_add_fetch(&b->mapped, 1, __ATOMIC_RELAXED);
  int err = 0;
  if (ret < 0) {
    __atomic_add_fetch(&b->failed, 1, __ATOMIC_RELAXED); err = g_bsdErrno;
    int ei = NERR;
    for (int i = 0; i < NERR; i++) if (k_err[i].e == err) { ei = i; break; }
    __atomic_add_fetch(&g_err[ei], 1, __ATOMIC_RELAXED);
  }
  else if (op == B_SEND || op == B_SENDTO || op == B_WRITE) { __atomic_add_fetch(&b->bytes, (uint64_t)ret, __ATOMIC_RELAXED); __atomic_add_fetch(&g_tx, (uint64_t)ret, __ATOMIC_RELAXED); }
  else if (op == B_RECV || op == B_RECVFROM || op == B_READ) { __atomic_add_fetch(&b->bytes, (uint64_t)ret, __ATOMIC_RELAXED); __atomic_add_fetch(&g_rx, (uint64_t)ret, __ATOMIC_RELAXED); }
  uint32_t k = __atomic_fetch_add(&g_ring_n, 1, __ATOMIC_RELAXED) % RING;
  BOp *r = &g_ring[k];
  r->t = t0; r->thread = threadGetCurHandle(); r->op = (uint8_t)op; r->mapped = (uint8_t)mapped;
  r->fd = (int16_t)fd; r->len = (uint32_t)size; r->flags = flags; r->ret = (int32_t)ret; r->err = err;
  r->us = us > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)us;
  __atomic_sub_fetch(&g_inflight, 1, __ATOMIC_RELAXED);
}
static void note_peer(int fd, const struct sockaddr *a, socklen_t l) {
  if (fd < 0 || fd >= NFD || !a || l < sizeof(struct sockaddr_in) || a->sa_family != AF_INET) return;
  const struct sockaddr_in *s = (const struct sockaddr_in *)a;
  g_fd[fd].ip = s->sin_addr.s_addr; g_fd[fd].port = ntohs(s->sin_port);
}

/* ---- wrappers ------------------------------------------------------------- */
int __real_bsdSocket(int domain, int type, int protocol);
int __real_bsdClose(int fd);
int __real_bsdConnect(int fd, const struct sockaddr *a, socklen_t l);
int __real_bsdBind(int fd, const struct sockaddr *a, socklen_t l);
ssize_t __real_bsdSend(int fd, const void *buf, size_t len, int flags);
ssize_t __real_bsdSendTo(int fd, const void *buf, size_t len, int flags, const struct sockaddr *a, socklen_t l);
ssize_t __real_bsdRecv(int fd, void *buf, size_t len, int flags);
ssize_t __real_bsdRecvFrom(int fd, void *buf, size_t len, int flags, struct sockaddr *a, socklen_t *l);
int __real_bsdPoll(struct pollfd *fds, nfds_t n, int timeout);
int __real_bsdSelect(int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *t);
int __real_bsdIoctl(int fd, int request, void *data);
int __real_bsdFcntl(int fd, int cmd, int flags);
int __real_bsdSetSockOpt(int fd, int level, int name, const void *v, socklen_t l);
int __real_bsdGetSockOpt(int fd, int level, int name, void *v, socklen_t *l);
int __real_bsdShutdown(int fd, int how);
int __real_bsdGetSockName(int fd, struct sockaddr *a, socklen_t *l);
int __real_bsdGetPeerName(int fd, struct sockaddr *a, socklen_t *l);
int __real_bsdAccept(int fd, struct sockaddr *a, socklen_t *l);
int __real_bsdListen(int fd, int backlog);
ssize_t __real_bsdWrite(int fd, const void *buf, size_t len);
ssize_t __real_bsdRead(int fd, void *buf, size_t len);

int __wrap_bsdSocket(int domain, int type, int protocol) {
  uint64_t t = b_begin();
  int r = __real_bsdSocket(domain, type, protocol);
  if (r >= 0 && r < NFD) { memset(&g_fd[r], 0, sizeof g_fd[r]); g_fd[r].type = (uint8_t)type; }
  b_end(B_SOCKET, t, r, 0, (uint32_t)type, r);
  return r;
}
int __wrap_bsdClose(int fd) {
  uint64_t t = b_begin(); int r = __real_bsdClose(fd); b_end(B_CLOSE, t, fd, 0, 0, r); return r;
}
int __wrap_bsdConnect(int fd, const struct sockaddr *a, socklen_t l) {
  note_peer(fd, a, l);
  uint64_t t = b_begin(); int r = __real_bsdConnect(fd, a, l); b_end(B_CONNECT, t, fd, l, 0, r); return r;
}
int __wrap_bsdBind(int fd, const struct sockaddr *a, socklen_t l) {
  uint64_t t = b_begin(); int r = __real_bsdBind(fd, a, l); b_end(B_BIND, t, fd, l, 0, r); return r;
}
ssize_t __wrap_bsdSend(int fd, const void *buf, size_t len, int flags) {
  uint64_t t = b_begin(); ssize_t r = __real_bsdSend(fd, buf, len, flags); b_end(B_SEND, t, fd, len, (uint32_t)flags, r); return r;
}
ssize_t __wrap_bsdSendTo(int fd, const void *buf, size_t len, int flags, const struct sockaddr *a, socklen_t l) {
  note_peer(fd, a, l);
  uint64_t t = b_begin(); ssize_t r = __real_bsdSendTo(fd, buf, len, flags, a, l); b_end(B_SENDTO, t, fd, len, (uint32_t)flags, r); return r;
}
ssize_t __wrap_bsdRecv(int fd, void *buf, size_t len, int flags) {
  uint64_t t = b_begin(); ssize_t r = __real_bsdRecv(fd, buf, len, flags); b_end(B_RECV, t, fd, len, (uint32_t)flags, r); return r;
}
ssize_t __wrap_bsdRecvFrom(int fd, void *buf, size_t len, int flags, struct sockaddr *a, socklen_t *l) {
  uint64_t t = b_begin(); ssize_t r = __real_bsdRecvFrom(fd, buf, len, flags, a, l); b_end(B_RECVFROM, t, fd, len, (uint32_t)flags, r); return r;
}
int __wrap_bsdPoll(struct pollfd *fds, nfds_t n, int timeout) {
  uint64_t t = b_begin(); int r = __real_bsdPoll(fds, n, timeout);
  b_end(B_POLL, t, n ? fds[0].fd : -1, n * sizeof *fds, (uint32_t)timeout, r); return r;
}
int __wrap_bsdSelect(int n, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *tv) {
  uint64_t t = b_begin(); int r = __real_bsdSelect(n, rd, wr, ex, tv); b_end(B_SELECT, t, n, sizeof(fd_set), 0, r); return r;
}
int __wrap_bsdIoctl(int fd, int request, void *data) {
  uint64_t t = b_begin(); int r = __real_bsdIoctl(fd, request, data); b_end(B_IOCTL, t, fd, 0, (uint32_t)request, r); return r;
}
int __wrap_bsdFcntl(int fd, int cmd, int flags) {
  uint64_t t = b_begin(); int r = __real_bsdFcntl(fd, cmd, flags); b_end(B_FCNTL, t, fd, 0, (uint32_t)cmd, r); return r;
}
int __wrap_bsdSetSockOpt(int fd, int level, int name, const void *v, socklen_t l) {
  uint64_t t = b_begin(); int r = __real_bsdSetSockOpt(fd, level, name, v, l); b_end(B_SETOPT, t, fd, l, (uint32_t)name, r); return r;
}
int __wrap_bsdGetSockOpt(int fd, int level, int name, void *v, socklen_t *l) {
  uint64_t t = b_begin(); int r = __real_bsdGetSockOpt(fd, level, name, v, l); b_end(B_GETOPT, t, fd, l ? *l : 0, (uint32_t)name, r); return r;
}
int __wrap_bsdShutdown(int fd, int how) {
  uint64_t t = b_begin(); int r = __real_bsdShutdown(fd, how); b_end(B_SHUTDOWN, t, fd, 0, (uint32_t)how, r); return r;
}
int __wrap_bsdGetSockName(int fd, struct sockaddr *a, socklen_t *l) {
  uint64_t t = b_begin(); int r = __real_bsdGetSockName(fd, a, l); b_end(B_GETNAME, t, fd, l ? *l : 0, 0, r); return r;
}
int __wrap_bsdGetPeerName(int fd, struct sockaddr *a, socklen_t *l) {
  uint64_t t = b_begin(); int r = __real_bsdGetPeerName(fd, a, l); b_end(B_PEERNAME, t, fd, l ? *l : 0, 0, r); return r;
}
int __wrap_bsdAccept(int fd, struct sockaddr *a, socklen_t *l) {
  uint64_t t = b_begin(); int r = __real_bsdAccept(fd, a, l); b_end(B_ACCEPT, t, fd, l ? *l : 0, 0, r); return r;
}
int __wrap_bsdListen(int fd, int backlog) {
  uint64_t t = b_begin(); int r = __real_bsdListen(fd, backlog); b_end(B_LISTEN, t, fd, 0, 0, r); return r;
}
ssize_t __wrap_bsdWrite(int fd, const void *buf, size_t len) {
  uint64_t t = b_begin(); ssize_t r = __real_bsdWrite(fd, buf, len); b_end(B_WRITE, t, fd, len, 0, r); return r;
}
ssize_t __wrap_bsdRead(int fd, void *buf, size_t len) {
  uint64_t t = b_begin(); ssize_t r = __real_bsdRead(fd, buf, len); b_end(B_READ, t, fd, len, 0, r); return r;
}

/* ---- output (nx_bfmt only: heartbeat and autopsy) ------------------------- */
static unsigned clampu(int n, unsigned room) { return n < 0 ? 0 : (unsigned)n < room ? (unsigned)n : (room ? room - 1 : 0); }

/* Heartbeat: " | bsd: 160012 req (321 mapped, 12 failed), tx 1234K rx 5678K, ptr 0x1000, 1 in flight" */
int nx_bsdmon_beacon(char *buf, unsigned len) {
  uint64_t calls = 0, mapped = 0, failed = 0;
  for (int i = 0; i < B_N; i++) { calls += g_b[i].calls; mapped += g_b[i].mapped; failed += g_b[i].failed; }
  /* sockets opened but not yet closed: a climbing count = an fd/socket leak,
   * the likeliest route to ENOBUFS over time (DECISIONS 61). */
  long open = (long)(g_b[B_SOCKET].calls + g_b[B_ACCEPT].calls) - (long)g_b[B_CLOSE].calls;
  unsigned o = clampu(nx_bfmt(buf, len, " | bsd: %llu req (%llu mapped, %llu failed), %ld sock open, tx %lluK rx %lluK, ptr 0x%x, %u in flight",
                 (unsigned long long)calls, (unsigned long long)mapped, (unsigned long long)failed, open,
                 (unsigned long long)(g_tx >> 10), (unsigned long long)(g_rx >> 10), ptr_size(), g_inflight), len);
  /* flag the exhaustion-canary errnos (ENOBUFS...) inline when any is non-zero */
  int any = 0;
  for (int i = 0; i < NERR && o < len; i++) {
    if (!k_err[i].beacon || !g_err[i]) continue;
    if (!any && o < len) { o += clampu(nx_bfmt(buf + o, len - o, " err:"), len - o); any = 1; }
    o += clampu(nx_bfmt(buf + o, len - o, " %s=%llu", k_err[i].n, (unsigned long long)g_err[i]), len - o);
  }
  return (int)o;
}

/* Autopsy / debug.log: per command, then the last operations, oldest first. */
unsigned nx_bsdmon_report(char *buf, unsigned len, int last_ops) {
  unsigned o = 0;
  o += clampu(nx_bfmt(buf + o, len - o, "bsd requests per command (pointer buffer 0x%x: bigger requests are mapped):\n",
                      ptr_size()), len - o);
  for (int i = 0; i < B_N && o < len; i++) {
    BStat *b = &g_b[i];
    if (!b->calls) continue;
    o += clampu(nx_bfmt(buf + o, len - o, "  %-11s %8llu req, %6llu mapped, %6llu failed, %8lluK, %llu ms total, max %llu ms\n",
                        b_name[i], (unsigned long long)b->calls, (unsigned long long)b->mapped,
                        (unsigned long long)b->failed, (unsigned long long)(b->bytes >> 10),
                        (unsigned long long)(b->us / 1000ull), (unsigned long long)(b->max_us / 1000ull)), len - o);
  }
  /* errno breakdown (FreeBSD errno); ENOBUFS rising = buffer exhaustion. */
  uint64_t etot = 0;
  for (int i = 0; i <= NERR; i++) etot += g_err[i];
  if (etot && o < len) {
    o += clampu(nx_bfmt(buf + o, len - o, "bsd errno breakdown (FreeBSD errno, %llu failed total):", (unsigned long long)etot), len - o);
    for (int i = 0; i < NERR && o < len; i++)
      if (g_err[i]) o += clampu(nx_bfmt(buf + o, len - o, " %s=%llu", k_err[i].n, (unsigned long long)g_err[i]), len - o);
    if (g_err[NERR] && o < len) o += clampu(nx_bfmt(buf + o, len - o, " other=%llu", (unsigned long long)g_err[NERR]), len - o);
    if (o < len) o += clampu(nx_bfmt(buf + o, len - o, "\n"), len - o);
  }
  if (last_ops <= 0) return o;
  uint32_t n = __atomic_load_n(&g_ring_n, __ATOMIC_ACQUIRE);
  uint32_t k0 = n > (uint32_t)last_ops ? n - (uint32_t)last_ops : 0;
  if (n - k0 > RING) k0 = n - RING;
  uint64_t now = tick();
  o += clampu(nx_bfmt(buf + o, len - o, "last %u bsd operations (age ms, thread handle, op, fd type peer, size, flags, result/errno, us, M = mapped):\n",
                      n - k0), len - o);
  for (uint32_t k = k0; k < n && o < len; k++) {
    BOp *r = &g_ring[k % RING];
    int fd = r->fd;
    const char *ty = "?";
    uint32_t ip = 0; uint16_t port = 0;
    if (fd >= 0 && fd < NFD) {
      int t = g_fd[fd].type & 0xF;
      ty = t == SOCK_DGRAM ? "udp" : t == SOCK_STREAM ? "tcp" : "-";
      ip = g_fd[fd].ip; port = g_fd[fd].port;
    }
    const uint8_t *a = (const uint8_t *)&ip;
    o += clampu(nx_bfmt(buf + o, len - o, "  -%llu 0x%x %s fd %d %s %u.%u.%u.%u:%u len %u fl 0x%x -> %d/%d %u us%s\n",
                        (unsigned long long)(armTicksToNs(now - r->t) / 1000000ull), r->thread,
                        r->op < B_N ? b_name[r->op] : "?", fd, ty, a[0], a[1], a[2], a[3], port, r->len, r->flags,
                        r->ret, r->err, r->us, r->mapped ? " M" : ""), len - o);
  }
  return o;
}
