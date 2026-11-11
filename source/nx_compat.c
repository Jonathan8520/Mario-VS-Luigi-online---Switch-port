/* nx_compat.c -- libc/liblog imports that Unity 6000.3 added over the
 * cloverpit_nx (6000.2) tables. Found with tools/re/check_imports.py.
 * MIT -- part of mvlo_nx. */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#include "so_util.h"
#include "util.h"

extern int  fseek_fake(FILE *f, long off, int whence);   /* libc_shim.c */
extern long ftell_fake(FILE *f);
extern long write_fake(int fd, const void *buf, size_t count);

static int android_log_buf_write(int bufid, int prio, const char *tag, const char *text) {
  (void)bufid; (void)prio;
  debugPrintf("%s: %s\n", tag ? tag : "?", text ? text : "");
  return 1;
}
static int fseeko64_fake(FILE *f, int64_t off, int whence) { return fseek_fake(f, (long)off, whence); }
static int64_t ftello64_fake(FILE *f) { return (int64_t)ftell_fake(f); }

typedef struct { const void *base; size_t len; } l_iovec;
static long writev_fake(int fd, const l_iovec *iov, int cnt) {
  long total = 0;
  for (int i = 0; i < cnt; i++) {
    if (!iov[i].len) continue;
    long r = write_fake(fd, iov[i].base, iov[i].len);
    if (r < 0) return total ? total : -1;
    total += r;
    if ((size_t)r < iov[i].len) break;
  }
  return total;
}

DynLibFunction nx_compat_exports[] = {
  { "__android_log_buf_write", (uintptr_t)&android_log_buf_write },
  { "fseeko64", (uintptr_t)&fseeko64_fake },
  { "ftello64", (uintptr_t)&ftello64_fake },
  { "writev", (uintptr_t)&writev_fake },
};
const int nx_compat_exports_num = sizeof(nx_compat_exports) / sizeof(nx_compat_exports[0]);
