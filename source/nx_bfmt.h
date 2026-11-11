/* nx_bfmt.h -- allocation- and lock-free snprintf subset (see nx_bfmt.c).
 * MIT -- part of mvlo_nx. */
#ifndef NX_BFMT_H
#define NX_BFMT_H
#include <stdarg.h>

/* snprintf semantics: returns the length the full output would have. */
int nx_bfmt(char *buf, unsigned size, const char *fmt, ...);
int nx_vbfmt(char *buf, unsigned size, const char *fmt, va_list ap);

#endif
