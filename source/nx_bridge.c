/* nx_bridge.c -- native side of the game <-> wrapper bridge.
 *
 * unity/Runtime/NxPort/NxBridge.cs declares [DllImport("nxbridge")]. IL2CPP
 * resolves that with dlopen("libnxbridge.so") + dlsym("nxbridge_*"); our
 * dlopen shim accepts the name and dlsym finds these functions in the import
 * table (nx_extra_imports[], imports.c). On real Android the library does not
 * exist and the managed side switches every Switch feature off.
 *
 * Everything here runs on the calling (engine main) thread and is cheap.
 * MIT -- part of mvlo_nx.
 */
#include <stdint.h>
#include <string.h>

#include "nx_bridge.h"
#include "nx_config.h"
#include "nx_input.h"
#include "nx_kbd.h"
#include "util.h"

#define NXBRIDGE_VERSION 1

static int nxbridge_version(void) {
  static int logged;
  if (!logged) { logged = 1; debugPrintf("[nxport] managed bridge connected (v%d)\n", NXBRIDGE_VERSION); }
  return NXBRIDGE_VERSION;
}

static int nxbridge_get_config(char *buf, int cap) {
  const char *t = nxcfg_text();
  int n = (int)strlen(t);
  if (!buf || cap <= 0) return n;
  if (n > cap) n = cap;
  memcpy(buf, t, (size_t)n);
  return n;
}

static void nxbridge_log(const char *utf8) {
  if (utf8) debugPrintf("[nxport] %s\n", utf8);
}

static int nxbridge_poll_pads(NxPadState *pads, int max) {
  static int logged_conn[4];
  int n = nx_input_poll_pads(pads, max);
  for (int i = 0; i < n && i < 4; i++) {
    if (pads[i].connected != logged_conn[i]) {
      logged_conn[i] = pads[i].connected;
      debugPrintf("[input] pad %d %s (style 0x%x)\n", i + 1,
                  pads[i].connected ? "connected" : "disconnected", (unsigned)pads[i].style);
    }
  }
  return n;
}

static int nxbridge_poll_touch(NxTouch *t, int max) { return nx_input_poll_touch(t, max); }
static void nxbridge_rumble(int pad, float lo, float hi) { nx_input_rumble(pad, lo, hi); }
static int nxbridge_kbd_closed(void) { return nx_kbd_take_closed(); }

DynLibFunction nx_bridge_exports[] = {
  { "nxbridge_version",    (uintptr_t)&nxbridge_version },
  { "nxbridge_get_config", (uintptr_t)&nxbridge_get_config },
  { "nxbridge_log",        (uintptr_t)&nxbridge_log },
  { "nxbridge_poll_pads",  (uintptr_t)&nxbridge_poll_pads },
  { "nxbridge_poll_touch", (uintptr_t)&nxbridge_poll_touch },
  { "nxbridge_rumble",     (uintptr_t)&nxbridge_rumble },
  { "nxbridge_kbd_closed", (uintptr_t)&nxbridge_kbd_closed },
};
const int nx_bridge_exports_num = sizeof(nx_bridge_exports) / sizeof(nx_bridge_exports[0]);

int nx_bridge_is_lib(const char *name) {
  return name && strstr(name, "nxbridge") != NULL;
}
