/* nx_input.h -- HID for the managed bridge. Layouts MUST match
 * unity/Runtime/NxPort/NxBridge.cs (NxPadState / NxTouch). MIT -- mvlo_nx. */
#ifndef NX_INPUT_H
#define NX_INPUT_H
#include <stdint.h>

typedef struct {
  uint32_t buttons;      /* HidNpadButton bits */
  int32_t  connected;
  float    lx, ly, rx, ry;
  int32_t  style;
  int32_t  reserved;
} NxPadState;            /* 32 bytes */

typedef struct {
  int32_t id;
  float   x, y;          /* 0..1, top-left origin */
  int32_t reserved;
} NxTouch;               /* 16 bytes */

_Static_assert(sizeof(NxPadState) == 32, "NxPadState layout");
_Static_assert(sizeof(NxTouch) == 16, "NxTouch layout");

void android_native_input_init(void);
int  nx_input_poll_pads(NxPadState *out, int max);
int  nx_input_poll_touch(NxTouch *out, int max);
void nx_input_rumble(int pad, float low, float high);
/* armTicksToNs() time of the last button press / new touch, 0 if none yet. */
uint64_t nx_input_last_action_ns(void);
/* Same, for A or a new touch only. */
uint64_t nx_input_last_select_ns(void);
/* 1 if the last action was a touch and no finger is on the screen any more. */
int nx_input_touch_lifted(void);

#endif
