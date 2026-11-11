/* nx_io.h -- block cache for read-only game files (see nx_io.c).
 * MIT -- part of mvlo_nx. */
#ifndef NX_IO_H
#define NX_IO_H
#include <stddef.h>
#include <stdint.h>

int     nxio_enabled(void);                      /* [io] cache (default 1); inits on first call */
void    nxio_open(int fd, uint64_t fid, const char *path, int64_t size);
void    nxio_close(int fd);
int     nxio_owns(int fd);
void    nxio_invalidate(uint64_t fid);           /* path written / renamed */
long    nxio_read(int fd, void *buf, size_t n);  /* only for nxio_owns(fd) */
long    nxio_pread(int fd, void *buf, size_t n, int64_t off);
int64_t nxio_lseek(int fd, int64_t off, int whence);
void    nxio_report(int force);
uint64_t nxio_total_sd_bytes(void);                  /* per-file SD statistics to debug.log */
int     nxio_ramfile_add(uint64_t fid, uint8_t *data, int64_t size); /* preload: whole file in memory, owns data */
int     nxio_ramfile_get(uint64_t fid, const uint8_t **data, int64_t *size); /* phantom fds */
#endif
