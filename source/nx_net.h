/* nx_net.h -- bionic socket ABI over libnx (see nx_net.c). MIT -- mvlo_nx. */
#ifndef NX_NET_H
#define NX_NET_H
#include <stddef.h>

void nx_net_init(void);
void nx_net_exit(void);
void nx_net_report(void);
int  nx_net_is_socket(int fd);
void nx_net_on_close(int fd);
long nx_net_read(int fd, void *buf, size_t n);
long nx_net_write(int fd, const void *buf, size_t n);
/* Last Photon datagrams ([debug] photon_ring), newest-windowed; 0 if off. */
unsigned nx_net_photon_report(char *buf, unsigned len, int window_ms);
int  nx_errno_to_linux(int e);
#include "so_util.h"
extern DynLibFunction nx_net_exports[];
extern const int nx_net_exports_num;

#endif
