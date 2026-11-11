/* nx_input.c -- Switch HID for the managed input bridge.
 *
 * Pads 1-4 (pad 1 also merges the handheld Joy-Cons), the touchscreen and HD
 * rumble. Read on demand from nxbridge_poll_* (called by NxInput.cs inside
 * InputSystem.onBeforeUpdate, i.e. on the engine main thread), so there is no
 * per-frame pump in the render loop and no cross-thread state.
 *
 * MIT -- part of mvlo_nx.
 */
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "nx_input.h"
#include "nx_config.h"
#include "util.h"

#define NPADS 4
static PadState g_pad[NPADS];
static HidVibrationDeviceHandle g_vib[NPADS][2];
static int g_vib_ok[NPADS];
static int g_inited;
static u64 g_t0_ns;
/* Last rising edge of a "press" (face/shoulder/system button, stick click or
 * a new touch): nx_kbd only opens swkbd right after one, so a game that keeps
 * re-activating its input field every frame cannot loop the keyboard. */
static u64 g_prev_btn[NPADS];
static int g_prev_touch;
static volatile u64 g_action_ns, g_select_ns;   /* any press / A or new touch */
static volatile int g_action_touch, g_touching; /* last action was a touch / a finger is down */
#define ACTION_MASK (HidNpadButton_A | HidNpadButton_B | HidNpadButton_X | HidNpadButton_Y | \
                     HidNpadButton_L | HidNpadButton_R | HidNpadButton_ZL | HidNpadButton_ZR | \
                     HidNpadButton_Plus | HidNpadButton_Minus | \
                     HidNpadButton_StickL | HidNpadButton_StickR)

/* [debug] autopress = A@70,A@130,DOWN@190 -- test-only: hold a button on pad 1
 * for 250 ms at the given second since input init (headless emulator runs). */
#define MAX_AUTO 32
static struct { u64 at_ns; u64 mask; int fired; } g_auto[MAX_AUTO];
static int g_nauto;
static u64 auto_button(const char *n, size_t len) {
  static const struct { const char *n; u64 m; } tab[] = {
    {"A", HidNpadButton_A}, {"B", HidNpadButton_B}, {"X", HidNpadButton_X}, {"Y", HidNpadButton_Y},
    {"L", HidNpadButton_L}, {"R", HidNpadButton_R}, {"ZL", HidNpadButton_ZL}, {"ZR", HidNpadButton_ZR},
    {"PLUS", HidNpadButton_Plus}, {"MINUS", HidNpadButton_Minus},
    {"UP", HidNpadButton_Up}, {"DOWN", HidNpadButton_Down}, {"LEFT", HidNpadButton_Left}, {"RIGHT", HidNpadButton_Right},
  };
  for (unsigned i = 0; i < sizeof tab / sizeof *tab; i++)
    if (strlen(tab[i].n) == len && !strncmp(tab[i].n, n, len)) return tab[i].m;
  return 0;
}
static void auto_parse(void) {
  const char *s = nxcfg_get("debug.autopress", "");
  while (*s && g_nauto < MAX_AUTO) {
    while (*s == ' ' || *s == ',') s++;
    const char *at = strchr(s, '@');
    if (!at) break;
    u64 m = auto_button(s, (size_t)(at - s));
    int sec = atoi(at + 1);
    if (m) { g_auto[g_nauto].at_ns = (u64)sec * 1000000000ull; g_auto[g_nauto].mask = m; g_nauto++; }
    s = at + 1;
    while (*s && *s != ',') s++;
  }
  if (g_nauto) debugPrintf("[input] autopress: %d scripted presses (test mode)\n", g_nauto);
}
static u64 auto_buttons(void) {
  u64 now = armTicksToNs(armGetSystemTick()) - g_t0_ns, m = 0;
  for (int i = 0; i < g_nauto; i++) {
    if (now < g_auto[i].at_ns || now >= g_auto[i].at_ns + 250000000ull) continue;
    if (!g_auto[i].fired) { g_auto[i].fired = 1; debugPrintf("[input] autopress #%d mask=0x%llx at %.1fs\n", i, (unsigned long long)g_auto[i].mask, now / 1e9); }
    m |= g_auto[i].mask;
  }
  return m;
}

void android_native_input_init(void) {
  if (g_inited) return;
  g_inited = 1;
  padConfigureInput(NPADS, HidNpadStyleSet_NpadStandard);
  hidSetNpadJoyHoldType(HidNpadJoyHoldType_Horizontal);
  padInitializeDefault(&g_pad[0]);                       /* No1 + Handheld */
  padInitialize(&g_pad[1], HidNpadIdType_No2);
  padInitialize(&g_pad[2], HidNpadIdType_No3);
  padInitialize(&g_pad[3], HidNpadIdType_No4);
  hidInitializeTouchScreen();
  g_t0_ns = armTicksToNs(armGetSystemTick());
  auto_parse();
  debugPrintf("[input] HID ready: %d pads (No1+Handheld, No2..No4), touchscreen\n", NPADS);
}

static void vib_init(int i) {
  if (g_vib_ok[i]) return;
  g_vib_ok[i] = -1;
  u32 style = padGetStyleSet(&g_pad[i]);
  HidNpadIdType id = (i == 0 && (style & HidNpadStyleTag_NpadHandheld)) ? HidNpadIdType_Handheld
                                                                       : (HidNpadIdType)(HidNpadIdType_No1 + i);
  HidNpadStyleTag tag = (style & HidNpadStyleTag_NpadHandheld) ? HidNpadStyleTag_NpadHandheld :
                        (style & HidNpadStyleTag_NpadFullKey)  ? HidNpadStyleTag_NpadFullKey :
                        (style & HidNpadStyleTag_NpadJoyDual)  ? HidNpadStyleTag_NpadJoyDual :
                        (style & HidNpadStyleTag_NpadJoyLeft)  ? HidNpadStyleTag_NpadJoyLeft :
                                                                 HidNpadStyleTag_NpadJoyRight;
  int n = (tag == HidNpadStyleTag_NpadJoyDual || tag == HidNpadStyleTag_NpadHandheld) ? 2 : 1;
  if (R_SUCCEEDED(hidInitializeVibrationDevices(g_vib[i], n, id, tag)))
    g_vib_ok[i] = n;
}

int nx_input_poll_pads(NxPadState *out, int max) {
  if (!g_inited) android_native_input_init();
  int n = max < NPADS ? max : NPADS;
  u64 scripted = g_nauto ? auto_buttons() : 0;
  for (int i = 0; i < n; i++) {
    padUpdate(&g_pad[i]);
    memset(&out[i], 0, sizeof out[i]);
    out[i].connected = padIsConnected(&g_pad[i]) ? 1 : 0;
    if (!out[i].connected) { g_vib_ok[i] = 0; continue; }
    u64 b = padGetButtons(&g_pad[i]) | (i == 0 ? scripted : 0);
    u64 down = b & ~g_prev_btn[i];
    if (down & ACTION_MASK) { g_action_ns = armTicksToNs(armGetSystemTick()); g_action_touch = 0; }
    if (down & HidNpadButton_A) g_select_ns = g_action_ns;
    g_prev_btn[i] = b;
    out[i].buttons = (uint32_t)b;
    HidAnalogStickState l = padGetStickPos(&g_pad[i], 0);
    HidAnalogStickState r = padGetStickPos(&g_pad[i], 1);
    out[i].lx = l.x / 32767.0f; out[i].ly = l.y / 32767.0f;
    out[i].rx = r.x / 32767.0f; out[i].ry = r.y / 32767.0f;
    out[i].style = (int32_t)padGetStyleSet(&g_pad[i]);
  }
  return n;
}

int nx_input_poll_touch(NxTouch *out, int max) {
  HidTouchScreenState st = {0};
  if (!hidGetTouchScreenStates(&st, 1)) return 0;
  if (st.count > g_prev_touch) { g_select_ns = g_action_ns = armTicksToNs(armGetSystemTick()); g_action_touch = 1; }
  g_prev_touch = st.count;
  g_touching = st.count > 0;
  int n = st.count < (s32)max ? st.count : max;
  for (int i = 0; i < n; i++) {
    out[i].id = (int32_t)st.touches[i].finger_id;
    out[i].x = st.touches[i].x / 1280.0f;
    out[i].y = st.touches[i].y / 720.0f;
    out[i].reserved = 0;
  }
  return n;
}

uint64_t nx_input_last_action_ns(void) { return g_action_ns; }
uint64_t nx_input_last_select_ns(void) { return g_select_ns; }
int nx_input_touch_lifted(void) { return g_action_touch && !g_touching; }

void nx_input_rumble(int pad, float low, float high) {
  if (pad < 0 || pad >= NPADS) return;
  vib_init(pad);
  if (g_vib_ok[pad] <= 0) return;
  if (low < 0) low = 0; if (low > 1) low = 1;
  if (high < 0) high = 0; if (high > 1) high = 1;
  HidVibrationValue v[2];
  for (int k = 0; k < 2; k++) {
    v[k].amp_low = low;   v[k].freq_low = 160.0f;
    v[k].amp_high = high; v[k].freq_high = 320.0f;
  }
  hidSendVibrationValues(g_vib[pad], v, g_vib_ok[pad]);
}
