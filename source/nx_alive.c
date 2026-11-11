/* nx_alive.c -- "is the process still alive?" without network or SD card.
 *
 * When the render loop has not advanced for [debug] stall_rumble_s seconds
 * (after boot), the first controller vibrates twice every 5 s for as long as
 * the stall lasts. During a freeze:
 *   - vibrations   -> this process still runs (the game is stuck, not the
 *                     console); debug.log / stall.log / netlog say where;
 *   - nothing      -> the process is stopped too (or the HID service is).
 *
 * The thread only reads two lock-free counters (diag.c) and talks to the HID
 * service: no SD card, no socket, no malloc, no log -- except nx_autopsy_arm()
 * once while the game still runs. At the start of each freeze it writes the
 * autopsy report (nx_autopsy.c, lock-free) and says on the controller whether
 * the SD card took it.
 *
 * MIT -- part of mvlo_nx. */
#include <stdint.h>
#include <switch.h>

#include "nx_autopsy.h"
#include "nx_config.h"
#include "nx_input.h"
#include "util.h"

extern int diag_frame_no(void);
extern uint64_t diag_idle_ns(void);

static Thread g_thr;
static uint64_t g_limit_ns;

/* The value is re-sent every 50 ms: the 1 s pulse sent once was not felt as
 * long on hardware (29/10), so do not rely on a value lasting by itself. */
static void pulse_ms(uint64_t ms) {
  for (uint64_t t = 0; t < ms; t += 50) {
    nx_input_rumble(0, 0.6f, 0.5f);
    svcSleepThread(50000000ull);
  }
  nx_input_rumble(0, 0.f, 0.f);
}
static void pulse(void) { pulse_ms(120); }

/* Once per freeze: the autopsy report (nx_autopsy.c), then its outcome on the
 * controller -- one long vibration: written to autopsy.log on the SD card;
 * three short ones: the SD card did not take it (UDP may still have). */
static void autopsy(void) {
  int ok = nx_autopsy_run(diag_idle_ns());
  if (ok < 0) return;
  if (ok) pulse_ms(1000);
  else for (int i = 0; i < 3; i++) { pulse_ms(150); svcSleepThread(150000000ull); }
  svcSleepThread(1500000000ull);
}

static void alive_main(void *arg) {
  (void)arg;
  static uint8_t tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  install_bionic_tls(tls);
  nx_autopsy_name_self("stall-rumble");
  int armed = 0, reported = 0;
  for (;;) {
    svcSleepThread(1000000000ull);
    if (diag_frame_no() < 300) continue;
    if (!armed) { nx_autopsy_arm(); armed = 1; }
    if (diag_idle_ns() < g_limit_ns) { reported = 0; continue; }
    if (!reported) { autopsy(); reported = 1; }
    pulse();
    svcSleepThread(200000000ull);
    pulse();
    svcSleepThread(3500000000ull);
  }
}

/* [debug] stall_exit_s: end the process after that many seconds without a new
 * frame, so a freeze no longer needs POWER held down -- and if the console
 * comes back as soon as we are gone, our requests were what kept the system
 * services stuck. Its own thread, which only sleeps and reads a counter: by
 * then the rumble thread may be stuck in the HID service itself (29/10: the
 * vibrations stopped a minute into the freeze). Time spent suspended (HOME
 * menu, sleep) does not count: a 1 s sleep that took much longer means the
 * process was not running. */
static Thread g_exit_thr;
static int    g_exit_s;

static void exit_main(void *arg) {
  (void)arg;
  static uint8_t tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  install_bionic_tls(tls);
  nx_autopsy_name_self("stall-exit");
  int last = -1, still = 0;
  for (;;) {
    uint64_t t0 = armGetSystemTick();
    svcSleepThread(1000000000ull);
    uint64_t dt_ms = armTicksToNs(armGetSystemTick() - t0) / 1000000ull;
    int f = diag_frame_no();
    if (f < 300 || f != last || dt_ms > 2500) { last = f; still = 0; continue; }
    if (++still >= g_exit_s) svcExitProcess();
  }
}

static void exit_start(void) {
  g_exit_s = nxcfg_int("debug.stall_exit_s", 90);
  if (g_exit_s <= 0) return;
  if (R_SUCCEEDED(threadCreate(&g_exit_thr, exit_main, NULL, NULL, 0x4000, 0x2C, -2)) &&
      R_SUCCEEDED(threadStart(&g_exit_thr)))
    debugPrintf("[wd] stall exit armed: the game closes itself after %d s without a new frame\n", g_exit_s);
  else
    debugPrintf("[wd] stall exit: thread failed\n");
}

void nx_alive_start(void) {
  exit_start();
  int s = nxcfg_int("debug.stall_rumble_s", 12);
  if (s <= 0) return;
  g_limit_ns = (uint64_t)s * 1000000000ull;
  static const int PRIOS[] = { 0x2A, 0x2B, 0x2C };
  Result rc = 0xe001;
  for (unsigned i = 0; i < sizeof PRIOS / sizeof PRIOS[0] && R_FAILED(rc); i++) {
    rc = threadCreate(&g_thr, alive_main, NULL, NULL, 0x8000, PRIOS[i], 2);
    if (R_FAILED(rc)) rc = threadCreate(&g_thr, alive_main, NULL, NULL, 0x8000, PRIOS[i], -2);
  }
  if (R_FAILED(rc) || R_FAILED(threadStart(&g_thr))) {
    debugPrintf("[wd] stall rumble: thread failed rc=0x%x\n", rc);
    return;
  }
  debugPrintf("[wd] stall rumble armed: the controller vibrates twice every 5 s while the game is stuck "
              "for more than %d s (the process is alive)\n", s);
}
