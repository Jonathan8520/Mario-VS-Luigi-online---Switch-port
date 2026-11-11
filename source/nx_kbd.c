/* nx_kbd.c -- Switch software keyboard (swkbd) for Unity's TouchScreenKeyboard.
 *
 * On Android, TMP_InputField / TouchScreenKeyboard.Open() reaches Java
 * UnityPlayer.showSoftInput(text, type, correction, multiline, secure, alert,
 * placeholder, characterLimit, hideInput, consumeOutsideTouches) and the Java
 * dialog reports back through natives Unity registered at JNI_OnLoad:
 * nativeSetInputString(String), nativeSetInputSelection(int,int),
 * nativeSoftInputClosed(), nativeSoftInputCanceled(),
 * nativeSetKeyboardIsVisible(boolean). Without a JVM nothing answered, so the
 * nickname and chat fields could not be edited.
 *
 * Here showSoftInput starts libnx's swkbd on its own thread (swkbdShow blocks
 * until the user is done; the game keeps rendering and the network keeps
 * running underneath), and the render loop delivers the result to Unity's
 * natives between two frames, on the engine's main thread, like Android's UI
 * thread would.
 *
 * The game's own gamepad keyboard (OnScreenKeyboard) re-activates its input
 * field every frame while it is open, and TMP_InputField answers a closed soft
 * keyboard by deselecting: on its own that would reopen swkbd forever. So in
 * the default mode swkbd only opens when a button press or a new touch came
 * within the last second, and not one seen right after the previous swkbd
 * closed. Other showSoftInput calls are only remembered: Unity then believes
 * its keyboard is up (TMP keeps the field focused and never asks again), so
 * the next A press or touch -- within a minute, and only if the field still
 * has the focus 250 ms later (a tap elsewhere makes Unity hide its keyboard)
 * -- opens swkbd for it (e.g. after moving onto a field with the D-pad, or
 * pressing A on the game's keyboard after swkbd closed). After a minute the
 * field is released (soft input cancelled), so tapping it asks again.
 *
 * [input] keyboard = 0 turns it off, 2 opens it on every showSoftInput.
 *
 * MIT -- part of mvlo_nx. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "nx_kbd.h"
#include "nx_config.h"
#include "nx_input.h"
#include "util.h"

extern void *fake_env, *fake_unityplayer_thiz;
extern void *jni_lookup_unity_native(const char *name);
extern void *jni_make_string(const char *utf);

#define KBD_MAX 512

static Thread         g_thr;
static volatile int   g_state;          /* 0 idle, 1 open, 2 result ready, 3 chat submit closing */
static int            g_submit_pumps;   /* render-loop passes since the chat text went in */
static int            g_ok;             /* 1 = confirmed, 0 = cancelled */
static int            g_visible_sent;
static char           g_init[KBD_MAX], g_guide[128], g_out[KBD_MAX];
static int            g_limit, g_numeric, g_secure, g_multiline;
static uint64_t       g_closed_ns;      /* when the last result was delivered */
static volatile int   g_closed_event;   /* for the managed side: close the game's keyboard */
static unsigned       g_skipped;
static uint64_t       g_pending_ns;     /* ignored showSoftInput, still "up" for Unity */
static uint64_t       g_pending_sel_ns; /* A / touch seen while pending, not acted on yet */
static int            g_pending_type, g_pending_ml, g_pending_sec, g_pending_lim;
static char           g_pending_ph[128];

#define PRESS_WINDOW_NS   1000000000ull /* press -> showSoftInput */
#define CLOSE_GRACE_NS     250000000ull /* presses seen on return from swkbd */
#define PENDING_MAX_NS   60000000000ull /* deferred open gives up after this */
/* A deferred keyboard used to open on the next A or touch ANYWHERE: after a
 * match the game focuses the chat field by itself, and tapping "Envoyer" then
 * opened swkbd instead of sending (08-09/11). A tap elsewhere moves the focus
 * away and Unity hides its keyboard (hideSoftInput drops the pending one), so
 * wait this long after the press and open only if the field kept the focus. */
#define CONFIRM_NS         250000000ull

static int mode(void) {
  static int v = -1;
  if (v < 0) v = nxcfg_int("input.keyboard", 1);
  return v;
}

static uint64_t now_ns(void) { return armTicksToNs(armGetSystemTick()); }

static void kbd_thread(void *arg) {
  (void)arg;
  SwkbdConfig kbd;
  Result rc = swkbdCreate(&kbd, 0);
  g_ok = 0;
  if (R_SUCCEEDED(rc)) {
    swkbdConfigMakePresetDefault(&kbd);
    if (g_numeric) swkbdConfigSetType(&kbd, SwkbdType_NumPad);
    if (g_secure)  swkbdConfigSetPasswordFlag(&kbd, 1);
    swkbdConfigSetStringLenMax(&kbd, (u32)(g_limit > 0 && g_limit < KBD_MAX - 1 ? g_limit : 100));
    if (g_multiline) swkbdConfigSetTextDrawType(&kbd, SwkbdTextDrawType_Box);
    if (g_init[0])  swkbdConfigSetInitialText(&kbd, g_init);
    if (g_guide[0]) swkbdConfigSetGuideText(&kbd, g_guide);
    swkbdConfigSetInitialCursorPos(&kbd, 1);     /* cursor at the end */
    memset(g_out, 0, sizeof g_out);
    rc = swkbdShow(&kbd, g_out, sizeof g_out);
    g_ok = R_SUCCEEDED(rc);
    swkbdClose(&kbd);
  }
  if (R_FAILED(rc) && R_VALUE(rc) != MAKERESULT(Module_Libnx, LibnxError_LibAppletBadExit))
    debugPrintf("[kbd] swkbd failed 0x%x\n", rc);
  __atomic_store_n(&g_state, 2, __ATOMIC_RELEASE);
}

static void kbd_open(const char *text, int type, int multiline, int secure,
                     const char *placeholder, int limit);

/* UnityPlayer.showSoftInput(...) -- from the JNI dispatch. */
void nx_kbd_show(const char *text, int type, int multiline, int secure,
                 const char *placeholder, int limit) {
  if (mode() == 0) { debugPrintf("[kbd] showSoftInput ignored ([input] keyboard = 0)\n"); return; }
  if (g_state != 0) { debugPrintf("[kbd] showSoftInput while the keyboard is up -- ignored\n"); return; }
  if (mode() == 1) {
    uint64_t press = nx_input_last_action_ns(), now = now_ns();
    /* A tap on a field asks for the keyboard while the finger is still down
     * (TMP selects on pointer down). If the finger is already up, the tap was
     * on something else and the game focused the field by itself: the chat
     * box refocuses after "Envoyer", which reopened swkbd at every send. */
    const int lifted = nx_input_touch_lifted();
    if (!press || now - press > PRESS_WINDOW_NS || press < g_closed_ns + CLOSE_GRACE_NS || lifted) {
      if (g_skipped++ < 16)
        debugPrintf("[kbd] showSoftInput without a fresh button press -- waiting for A or a touch\n");
      snprintf(g_init, sizeof g_init, "%s", text ? text : "");
      snprintf(g_pending_ph, sizeof g_pending_ph, "%s", placeholder ? placeholder : "");
      g_pending_type = type; g_pending_ml = multiline; g_pending_sec = secure; g_pending_lim = limit;
      g_pending_ns = now ? now : 1;
      g_pending_sel_ns = 0;
      return;
    }
  }
  kbd_open(text, type, multiline, secure, placeholder, limit);
}

static void kbd_open(const char *text, int type, int multiline, int secure,
                     const char *placeholder, int limit) {
  g_pending_ns = 0;
  g_pending_sel_ns = 0;
  if (text != g_init) snprintf(g_init, sizeof g_init, "%s", text ? text : "");
  snprintf(g_guide, sizeof g_guide, "%s", placeholder ? placeholder : "");
  g_limit = limit;
  /* TouchScreenKeyboardType: 4 NumberPad, 5 PhonePad, 10 DecimalPad */
  g_numeric = (type == 4 || type == 5 || type == 10);
  g_secure = secure;
  g_multiline = multiline;
  g_visible_sent = 0;
  debugPrintf("[kbd] showSoftInput type=%d limit=%d multiline=%d secure=%d (%zu chars)\n",
              type, limit, multiline, secure, strlen(g_init));
  g_state = 1;
  if (R_FAILED(threadCreate(&g_thr, kbd_thread, NULL, NULL, 0x10000, 0x2C, -2)) ||
      R_FAILED(threadStart(&g_thr))) {
    debugPrintf("[kbd] could not start the keyboard thread\n");
    g_ok = 0;
    g_state = 2;
  }
}

void nx_kbd_hide(void) {
  /* swkbd cannot be dismissed from the outside; the user closes it. */
  static unsigned n;
  if (n++ < 16 || g_pending_sel_ns)
    debugPrintf("[kbd] hideSoftInput%s\n", g_state ? " (swkbd stays up)" :
                g_pending_sel_ns ? " (the press was not on the waiting field: no swkbd)" : "");
  g_pending_ns = 0;
  g_pending_sel_ns = 0;
}

/* setSoftInputStr: the field's text changed while Unity's keyboard is "up"
 * (the game's keyboard typing); a deferred swkbd starts from it. */
void nx_kbd_set_text(const char *text) {
  if (g_pending_ns && g_state == 0) snprintf(g_init, sizeof g_init, "%s", text ? text : "");
}

int nx_kbd_is_open(void) { return g_state != 0; }

/* NxInput.cs, once per input update: 1 once after each swkbd result, so the
 * game's own on-screen keyboard (still open behind swkbd) gets closed. */
int nx_kbd_take_closed(void) { return __atomic_exchange_n(&g_closed_event, 0, __ATOMIC_ACQ_REL); }

typedef void (*fn_str)(void *, void *, void *);
typedef void (*fn_v)(void *, void *);
typedef void (*fn_z)(void *, void *, uint8_t);
typedef void (*fn_ii)(void *, void *, int, int);

/* Render loop, between nativeRender calls. */
void nx_kbd_pump(void) {
  if (g_pending_ns && g_state == 0) {
    uint64_t sel = nx_input_last_select_ns(), now = now_ns();
    if (now - g_pending_ns > PENDING_MAX_NS) {
      /* Give the field back. Unity still believes its keyboard is up, so the
       * field keeps the focus and a later tap on it asked for nothing: the
       * chat box looked dead until something else was tapped first. */
      g_pending_ns = 0;
      g_pending_sel_ns = 0;
      fn_v canc = (fn_v)jni_lookup_unity_native("nativeSoftInputCanceled");
      fn_z vis  = (fn_z)jni_lookup_unity_native("nativeSetKeyboardIsVisible");
      if (canc) canc(fake_env, fake_unityplayer_thiz);
      if (vis) vis(fake_env, fake_unityplayer_thiz, 0);
      debugPrintf("[kbd] waiting keyboard expired -- field released\n");
    } else if (sel > g_pending_ns && sel >= g_closed_ns + CLOSE_GRACE_NS) {
      if (!g_pending_sel_ns) {
        g_pending_sel_ns = sel;
      } else if (now - g_pending_sel_ns >= CONFIRM_NS) {
        debugPrintf("[kbd] A / touch on a field still waiting for its keyboard -- opening swkbd\n");
        kbd_open(g_init, g_pending_type, g_pending_ml, g_pending_sec, g_pending_ph, g_pending_lim);
      }
    }
  }
  if (g_state == 1 && !g_visible_sent) {
    g_visible_sent = 1;
    fn_z vis = (fn_z)jni_lookup_unity_native("nativeSetKeyboardIsVisible");
    if (vis) vis(fake_env, fake_unityplayer_thiz, 1);
  }
  const int st = __atomic_load_n(&g_state, __ATOMIC_ACQUIRE);
  if (st != 2 && st != 3) return;
  fn_str set  = (fn_str)jni_lookup_unity_native("nativeSetInputString");
  fn_ii  sel  = (fn_ii)jni_lookup_unity_native("nativeSetInputSelection");
  fn_v   done = (fn_v)jni_lookup_unity_native("nativeSoftInputClosed");
  fn_v   canc = (fn_v)jni_lookup_unity_native("nativeSoftInputCanceled");
  fn_z   vis  = (fn_z)jni_lookup_unity_native("nativeSetKeyboardIsVisible");
  /* Chat submit, second step: the text went in while the keyboard was still
   * "visible" (see below); close it now, empty, so the box the game just
   * cleared is not refilled from the keyboard's copy. */
  if (st == 3) {
    if (++g_submit_pumps < 3) return;
    if (set) set(fake_env, fake_unityplayer_thiz, jni_make_string(""));
    if (done) done(fake_env, fake_unityplayer_thiz);
    if (vis) vis(fake_env, fake_unityplayer_thiz, 0);
    g_closed_ns = now_ns();
    g_closed_event = 1;
    g_state = 0;
    return;
  }
  threadWaitForExit(&g_thr);
  threadClose(&g_thr);
  debugPrintf("[kbd] %s (%zu chars); natives set=%p sel=%p closed=%p canceled=%p\n",
              g_ok ? "confirmed" : "cancelled", strlen(g_out), set, sel, done, canc);
  if (g_ok && g_multiline && set) {
    /* The only multi-line field is the room chat. Its validator
     * (TextSubmitValidator) sends the message on '\n', like Enter on a PC or
     * on an Android keyboard -- but TMP runs validators only on text that
     * arrives while the keyboard is visible; the text a closing keyboard
     * leaves is copied as is. swkbd's OK therefore only filled the box, and
     * the message waited for "Envoyer". So: deliver the text plus '\n' now,
     * keyboard still visible (TMP validates it char by char next frame and
     * the chat sends), then close two frames later (state 3). */
    int n = (int)strlen(g_out);
    if (n < KBD_MAX - 1 && (n == 0 || g_out[n - 1] != '\n')) { g_out[n] = '\n'; g_out[n + 1] = 0; }
    set(fake_env, fake_unityplayer_thiz, jni_make_string(g_out));
    g_submit_pumps = 0;
    g_state = 3;
    return;
  }
  if (g_ok) {
    if (set) set(fake_env, fake_unityplayer_thiz, jni_make_string(g_out));
    if (sel) { int n = (int)strlen(g_out); sel(fake_env, fake_unityplayer_thiz, n, 0); }
    if (done) done(fake_env, fake_unityplayer_thiz);
  } else {
    if (canc) canc(fake_env, fake_unityplayer_thiz);
    else if (done) done(fake_env, fake_unityplayer_thiz);
  }
  if (vis) vis(fake_env, fake_unityplayer_thiz, 0);
  g_closed_ns = now_ns();
  g_closed_event = 1;
  g_state = 0;
}
