/* nx_gpu.h -- GPU / shader instrumentation and safety knobs (nx_gpu.c).
 * MIT -- part of mvlo_nx. */
#ifndef NX_GPU_H
#define NX_GPU_H
#include <stdint.h>

/* Before the first EGL call: environment for mesa (NV50_PROG_OPTIMIZE). */
void nx_gpu_early_init(void);
/* Render loop, once per nativeRender: hitch breakdown when the frame was slow,
 * compile-time CPU boost bookkeeping. */
void nx_gpu_frame_end(uint64_t frame_ns, uint64_t frame_no);
/* Heartbeat (every ~2 s): [gpu] counters since the previous call. */
void nx_gpu_report(void);
/* cloverpit_boot.c's load-time CPU boost on/off (compile boosts wait for it). */
void nx_gpu_base_boost(int on);
/* glShaderSource shim (imports.c): final source of a shader. */
void nx_gpu_note_shader_source(unsigned shader, const char *src);
/* One line for the watchdog beacon (no locks, no allocation). */
void nx_gpu_beacon(char *buf, unsigned len);
/* File-system time charged to the [hitch] breakdown:
 * kind 0 = open, 1 = stat, 2 = fstat, 3 = access. */
void nx_gpu_account_fs(int kind, uint64_t ns);

#endif
