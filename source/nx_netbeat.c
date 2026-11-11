/* nx_netbeat.c -- live state sent to a PC over UDP, twice a second.
 *
 * WHY
 * The whole-console freezes seen on hardware stop the render loop, the
 * watchdog and every write to the SD card at once (HOME dead, music still
 * playing). debug.log and stall.log are written to the SD card, so they end a
 * second or two before the freeze and say nothing about it. A UDP datagram
 * does not touch the SD card or the GPU: if the datagrams keep coming during
 * a freeze, the process is alive and they show what it is stuck on (frame
 * counter, FS op in flight, GPU fence wait, threads parked); if they stop at
 * the same instant, the console itself stopped.
 *
 * [debug] netlog = <PC IP>[:port] (default port 9999), empty = off.
 * PC side: tools/switch/netlog_listen.py.
 *
 * MIT -- part of mvlo_nx.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <switch.h>

#include "nx_autopsy.h"
#include "nx_bfmt.h"
#include "nx_config.h"
#include "nx_signals.h"
#include "util.h"

extern int diag_netbeat_line(char *buf, unsigned len);
extern void nx_net_beacon(char *buf, unsigned len);
extern volatile int g_cpu_boost_on;
extern int nx_kmon_beacon(char *buf, unsigned len);
extern int nx_kmon_rl_beacon(char *buf, unsigned len);
extern int nx_bsdmon_beacon(char *buf, unsigned len);

static Thread             g_thr;
static int                g_running;
static volatile int       g_stop;
static int                g_sock = -1;
static struct sockaddr_in g_dst;

/* Memory blocks of this process (one svcQueryMemory per block). A count that
 * keeps growing until a freeze would point at kernel bookkeeping. */
static unsigned count_blocks(void) {
  unsigned n = 0;
  uint64_t a = 0;
  for (;;) {
    MemoryInfo mi; u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, a))) break;
    n++;
    uint64_t e = mi.addr + mi.size;
    if (e <= a || n > 200000) break;
    a = e;
  }
  return n;
}

/* sendto is an IPC to the system's bsd service: a stuck bsd would stop the
 * datagrams exactly like a frozen console. Slow and failed sends are counted
 * and reported in the next datagram that does get out. */
static uint32_t g_send_fail, g_send_slow;
static uint64_t g_send_max_ns;

static void netbeat_main(void *arg) {
  (void)arg;
  /* Same reason as the watchdog: some code reached from here may carry a
   * stack-protector prologue reading TPIDR_EL0. */
  static uint8_t tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  install_bionic_tls(tls);
  nx_autopsy_name_self("heartbeat");
  /* SoC temperature (TMP451 external sensor), [10.0.0+] session API */
  TsSession ts;
  int have_ts = R_SUCCEEDED(tsInitialize()) && R_SUCCEEDED(tsOpenSession(&ts, TsDeviceCode_LocationExternal));
  /* Everything below formats with nx_bfmt: no malloc lock, no newlib state
   * (newlib's %f allocates). */
  char line[1400], net[160];
  char slow[300] = "";
  for (unsigned long long seq = 0; !g_stop; seq++) {
    if ((seq % 4) == 0) {                 /* every 2 s: the costlier probes */
      float temp = -1.f;
      if (have_ts) tsSessionGetTemperature(&ts, &temp);
      int t10 = (int)(temp * 10.f);
      u64 sr_tot = 0, sr_used = 0;
      svcGetInfo(&sr_tot, InfoType_SystemResourceSizeTotal, CUR_PROCESS_HANDLE, 0);
      svcGetInfo(&sr_used, InfoType_SystemResourceSizeUsed, CUR_PROCESS_HANDLE, 0);
      unsigned o = (unsigned)nx_bfmt(slow, sizeof slow, " | soc=%d.%dC blocks=%u sysres=%lluK/%lluK boost=%d", t10 / 10,
                                     (t10 < 0 ? -t10 : t10) % 10, count_blocks(), (unsigned long long)(sr_used >> 10),
                                     (unsigned long long)(sr_tot >> 10), g_cpu_boost_on);
      /* Physical memory pools of the whole console, used/total in KB. Freezing
       * our process releases the system services stuck since ~160k bsd calls
       * (29/10): a pool that fills with them would show here. */
      static const char *const pool_name[4] = { "app", "applet", "system", "sysunsafe" };
      if (o < sizeof slow) o += (unsigned)nx_bfmt(slow + o, sizeof slow - o, " | pools");
      for (int p = 0; p < 4 && o < sizeof slow; p++) {
        u64 used = 0, tot = 0;
        Result r1 = svcGetSystemInfo(&used, SystemInfoType_UsedPhysicalMemorySize, INVALID_HANDLE, p);
        Result r2 = svcGetSystemInfo(&tot, SystemInfoType_TotalPhysicalMemorySize, INVALID_HANDLE, p);
        if (R_FAILED(r1) || R_FAILED(r2))
          o += (unsigned)nx_bfmt(slow + o, sizeof slow - o, " %s=rc%x", pool_name[p], R_FAILED(r1) ? r1 : r2);
        else
          o += (unsigned)nx_bfmt(slow + o, sizeof slow - o, " %s=%lluK/%lluK", pool_name[p],
                                 (unsigned long long)(used >> 10), (unsigned long long)(tot >> 10));
      }
    }
    uint64_t up_ms = armTicksToNs(armGetSystemTick()) / 1000000ull;
    unsigned n = (unsigned)nx_bfmt(line, sizeof line, "#%llu up=%llu.%03u ", seq, up_ms / 1000ull,
                                   (unsigned)(up_ms % 1000ull));
    if (n < sizeof line) n += (unsigned)diag_netbeat_line(line + n, sizeof line - n);
    nx_net_beacon(net, sizeof net);
    if (n < sizeof line) n += (unsigned)nx_bfmt(line + n, sizeof line - n, "%s", net);
    if (n < sizeof line) n += (unsigned)nx_sig_beacon(line + n, sizeof line - n);
    if (n < sizeof line) n += (unsigned)nx_kmon_beacon(line + n, sizeof line - n);
    if (n < sizeof line) n += (unsigned)nx_bsdmon_beacon(line + n, sizeof line - n);
    if ((seq % 4) == 0 && n < sizeof line) n += (unsigned)nx_kmon_rl_beacon(line + n, sizeof line - n);
    if ((seq % 20) == 0 && n < sizeof line) n += (unsigned)nx_autopsy_ipc_beacon(line + n, sizeof line - n);
    if (n < sizeof line && (g_send_fail || g_send_slow))
      n += (unsigned)nx_bfmt(line + n, sizeof line - n, " | send: %u failed, %u slow, max %llums", g_send_fail,
                             g_send_slow, (unsigned long long)(g_send_max_ns / 1000000ull));
    if (n < sizeof line) nx_bfmt(line + n, sizeof line - n, "%s", slow);
    unsigned len = 0;
    while (line[len]) len++;
    nx_autopsy_note_beat(line, len);
    uint64_t t0 = armTicksToNs(armGetSystemTick());
    ssize_t r = sendto(g_sock, line, len, 0, (struct sockaddr *)&g_dst, sizeof g_dst);
    uint64_t dt = armTicksToNs(armGetSystemTick()) - t0;
    if (r < 0) g_send_fail++;
    if (dt > 50000000ull) g_send_slow++;
    if (dt > g_send_max_ns) g_send_max_ns = dt;
    svcSleepThread(500000000ull);
  }
  if (have_ts) { tsSessionClose(&ts); tsExit(); }
}

/* Before socketExit: in 0.1.40 the next heartbeat went into the closed bsd
 * session and every exit ended on the Switch's error screen. */
void nx_netbeat_stop(void) {
  if (!g_running) return;
  g_stop = 1;
  if (R_SUCCEEDED(waitSingleHandle(g_thr.handle, 2000000000ull))) threadClose(&g_thr);
  g_running = 0;
  close(g_sock);
  g_sock = -1;
}

/* The PC that receives the heartbeat, for the autopsy report. 0 = none. */
int nx_netbeat_dst(struct sockaddr_in *out) {
  if (g_sock < 0) return 0;
  *out = g_dst;
  return 1;
}

void nx_netbeat_start(void) {
  const char *cfg = nxcfg_get("debug.netlog", "");
  if (!cfg || !*cfg) return;
  char host[64];
  snprintf(host, sizeof host, "%s", cfg);
  int port = 9999;
  char *colon = strchr(host, ':');
  if (colon) { *colon = 0; port = atoi(colon + 1); }
  memset(&g_dst, 0, sizeof g_dst);
  g_dst.sin_family = AF_INET;
  g_dst.sin_port = htons((uint16_t)port);
  if (!inet_aton(host, &g_dst.sin_addr)) { debugPrintf("[net] netlog: bad address '%s'\n", cfg); return; }
  g_sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (g_sock < 0) { debugPrintf("[net] netlog: socket() failed\n"); return; }
  /* Same priority walk as the watchdog: outrank the engine if allowed, on its
   * own core when possible, never fail closed. */
  static const int PRIOS[] = { 0x28, 0x2A, 0x2B, 0x2C };
  Result rc = 0xe001;
  for (unsigned i = 0; i < sizeof PRIOS / sizeof PRIOS[0] && R_FAILED(rc); i++) {
    rc = threadCreate(&g_thr, netbeat_main, NULL, NULL, 0x8000, PRIOS[i], 2);
    if (R_FAILED(rc)) rc = threadCreate(&g_thr, netbeat_main, NULL, NULL, 0x8000, PRIOS[i], -2);
  }
  if (R_FAILED(rc) || R_FAILED(threadStart(&g_thr))) {
    debugPrintf("[net] netlog: thread failed rc=0x%x\n", rc);
    return;
  }
  g_running = 1;
  debugPrintf("[net] netlog -> %s:%d every 0.5 s (system clock now %.3f s)\n", host, port,
              armTicksToNs(armGetSystemTick()) / 1e9);
}
