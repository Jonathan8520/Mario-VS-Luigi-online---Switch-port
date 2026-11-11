/* nx_statcache.h -- stat() of the game's read-only data from a directory
 * listing kept in memory (nx_statcache.c). MIT -- part of mvlo_nx. */
#ifndef NX_STATCACHE_H
#define NX_STATCACHE_H
#include <sys/stat.h>

/* 1 = exists (*st filled like libnx's fsdev_stat), 0 = does not exist,
 * -1 = not covered (outside <root>/assets and <root>/il2cpp, cache off, or
 * the directory could not be listed): do the real stat(). */
int  nx_statcache_stat(const char *path, struct stat *st);
/* A file at `path` is about to be written: forget its directory's listing. */
void nx_statcache_invalidate(const char *path);
/* One [io] line: directories listed, stats answered. */
void nx_statcache_report(void);

#endif
