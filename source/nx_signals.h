/* nx_signals.h -- signal emulation at safepoints (see nx_signals.c).
 * MIT -- part of mvlo_nx. */
#ifndef NX_SIGNALS_H
#define NX_SIGNALS_H
#include "so_util.h"

void nx_sig_init(void);                       /* main thread, before engine code */
void nx_sig_thread_register(const char *name);/* every engine-visible thread */
void nx_sig_thread_unregister(void);
void nx_sig_safepoint(void);                  /* deliver pending signals (cheap if none) */
void nx_sig_sleep_ms(int ms);                 /* sliced sleep with safepoints */
void nx_sig_report_stuck(void);               /* watchdog: threads that never ack */
void nx_sig_dump(void);                       /* stall dump: every thread's signal state */
#include <pthread.h>
pthread_t nx_sig_pthread_self(void);          /* "pthread_self" import: a proxy answers for the paused thread */
void nx_sig_force_allow(uintptr_t lo, uintptr_t hi); /* selftest: extra "game code" range */
int  nx_sig_beacon(char *buf, unsigned len);  /* heartbeat: GC stop-the-world state */
void nx_sig_force_stats(uint32_t *forced, uint32_t *skipped, uint32_t *failed);
int  nx_sig_sample_lock(uint32_t h, void **cookie); /* nx_prof.c: 0 = skip, a delivery is running */
void nx_sig_sample_unlock(void *cookie);

extern DynLibFunction nx_signal_exports[];
extern const int nx_signal_exports_num;
#endif
