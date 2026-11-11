/* nx_bfmt.c -- snprintf for the paths that must keep working when the rest of
 * the process is stuck: the UDP heartbeat and the beacons it prints.
 *
 * newlib's printf is not lock- or allocation-free for every conversion: %f
 * goes through dtoa -> Balloc -> _calloc_r, i.e. the malloc lock. A thread
 * blocked while holding that lock would silently stop the heartbeat, and a
 * stopped heartbeat reads as "the whole console froze". This formatter only
 * touches the caller's buffer: %s %c %d %i %u %x %p %%, the l / ll / z
 * length modifiers, a width and the '-' and '0' flags. No floating point:
 * print fixed point (tenths, milliseconds) with integers.
 *
 * MIT -- part of mvlo_nx. */
#include <stdarg.h>
#include <stdint.h>
#include "nx_bfmt.h"

typedef struct { char *b; unsigned n, o; } Out;

static void put(Out *w, char c) {
  if (w->o + 1 < w->n) w->b[w->o] = c;
  w->o++;
}

static void pad(Out *w, int count, char c) { while (count-- > 0) put(w, c); }

static void field(Out *w, const char *s, int len, int width, int left, char padc) {
  if (!left) pad(w, width - len, padc);
  for (int i = 0; i < len; i++) put(w, s[i]);
  if (left) pad(w, width - len, ' ');
}

int nx_vbfmt(char *buf, unsigned size, const char *fmt, va_list ap) {
  Out w = { buf, size, 0 };
  for (const char *p = fmt; *p; p++) {
    if (*p != '%') { put(&w, *p); continue; }
    p++;
    int left = 0, width = 0, lng = 0;
    char padc = ' ';
    for (;; p++) {
      if (*p == '-') left = 1;
      else if (*p == '0') padc = '0';
      else break;
    }
    while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
    while (*p == 'l' || *p == 'z') { lng++; p++; }
    char tmp[24];
    int len = 0;
    switch (*p) {
      case '%': put(&w, '%'); break;
      case 'c': tmp[0] = (char)va_arg(ap, int); field(&w, tmp, 1, width, left, ' '); break;
      case 's': {
        const char *s = va_arg(ap, const char *);
        if (!s) s = "(null)";
        while (s[len]) len++;
        field(&w, s, len, width, left, ' ');
        break;
      }
      case 'd': case 'i': case 'u': case 'x': case 'p': {
        uint64_t v;
        int neg = 0, hex = *p == 'x' || *p == 'p';
        if (*p == 'p') v = (uintptr_t)va_arg(ap, void *);
        else if (*p == 'd' || *p == 'i') {
          int64_t s = lng ? va_arg(ap, int64_t) : (int64_t)va_arg(ap, int);
          if (s < 0) { neg = 1; v = (uint64_t)0 - (uint64_t)s; } else v = (uint64_t)s;
        } else v = lng ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, unsigned);
        /* least significant first, then reversed into out */
        do {
          unsigned d = (unsigned)(hex ? v & 15 : v % 10);
          tmp[len++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
          v = hex ? v >> 4 : v / 10;
        } while (v && len < (int)sizeof tmp - 1);
        if (neg) tmp[len++] = '-';
        if (*p == 'p') { tmp[len++] = 'x'; tmp[len++] = '0'; }
        char out[24];
        for (int i = 0; i < len; i++) out[i] = tmp[len - 1 - i];
        if (neg && padc == '0' && !left) {   /* "-0042", not "00-42" */
          put(&w, '-');
          field(&w, out + 1, len - 1, width - 1, 0, '0');
        } else {
          field(&w, out, len, width, left, left ? ' ' : padc);
        }
        break;
      }
      case 0: p--; break;
      default: put(&w, '%'); put(&w, *p); break;
    }
  }
  if (w.n) buf[w.o < w.n ? w.o : w.n - 1] = 0;
  return (int)w.o;
}

int nx_bfmt(char *buf, unsigned size, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = nx_vbfmt(buf, size, fmt, ap);
  va_end(ap);
  return r;
}
