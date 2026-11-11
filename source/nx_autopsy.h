/* nx_autopsy.h -- what each thread waits on when the game freezes (nx_autopsy.c).
 * MIT -- part of mvlo_nx. */
#ifndef NX_AUTOPSY_H
#define NX_AUTOPSY_H
#include <stdint.h>
#include <switch.h>

/* Name the calling thread in the system-call journal (libnx threads that are
 * not in the diag registry: watchdog, heartbeat, rumble, probes...). */
void nx_autopsy_name_self(const char *name);

/* Open the report file, the probe sessions and the probe threads. Called once
 * from the rumble thread when the game is running (frame >= 300). */
void nx_autopsy_arm(void);

/* Write the report. Called from the rumble thread when the game has been stuck
 * for [debug] stall_rumble_s. Returns 1 if the report reached the SD card,
 * 0 if not, -1 if the autopsy is off or not armed. Takes ~3-7 s. */
int nx_autopsy_run(uint64_t stalled_ns);

/* " | in IPC session 0x.. (fs) for 12.3 s, caller NRO+0x..." for the thread
 * whose handle is `h`, or "" if it is not in a recorded system call. */
int nx_autopsy_journal_bfmt(Handle h, char *buf, unsigned len, uint64_t now_tick);

/* Keep the last heartbeat lines for the report (nx_netbeat.c). */
void nx_autopsy_note_beat(const char *line, unsigned len);

/* " | ipc/10s: ..." -- the busiest IPC sessions since the previous call. */
int nx_autopsy_ipc_beacon(char *buf, unsigned len);

#endif
