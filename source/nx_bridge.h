/* nx_bridge.h -- see nx_bridge.c. MIT -- part of mvlo_nx. */
#ifndef NX_BRIDGE_H
#define NX_BRIDGE_H
#include "so_util.h"

extern DynLibFunction nx_bridge_exports[];
extern const int nx_bridge_exports_num;
int nx_bridge_is_lib(const char *name);   /* dlopen("libnxbridge.so") */

#endif
