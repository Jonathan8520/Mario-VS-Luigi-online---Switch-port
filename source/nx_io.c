/* nx_io.c -- block cache for read-only game files on the SD card.
 *
 * Why: on Android, Unity's tiny reads (2-8 byte field reads while
 * deserializing, FMOD pulling sounds out of .resource files at scattered
 * offsets) hit the kernel page cache. Here every read() is an fsdev IPC to the
 * SD card. The previous cache (libc_shim.c ra_*) kept ONE 1 MB window per big
 * file under ONE global lock: a seek outside the window re-read a whole MB, and
 * every other thread's reads waited behind it -- the 6-9 s stalls at match
 * start (FMOD loading sounds while UnityMain waits) look exactly like that.
 *
 * What: a shared cache of BLOCK-sized blocks (default 64 KB, 32 MB in total),
 * LRU, keyed by (path, block index) so a file reopened later keeps its blocks.
 *   - sequential access grows a read-ahead run (up to io.readahead_kb);
 *   - a large request (>= 4 blocks) bypasses the cache and reads straight into
 *     the caller's buffer;
 *   - pread() uses the same cache (it used to bypass the old one entirely);
 *   - the cache lock is never held across an SD access: blocks being filled
 *     are marked LOADING and other readers wait for them.
 * The fd's file position is virtual (the real one is only scratch), exactly as
 * the old cache did, so read()/lseek()/pread() stay consistent.
 *
 * Per-file statistics (reads, SD time, hit rate) go to debug.log every 30 s
 * while they change, plus a line for any single read slower than 100 ms.
 *
 * [io] cache = 0 turns all of this off (the old 1 MB window is used again).
 *
 * MIT -- part of mvlo_nx. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <switch.h>

#include "nx_io.h"
#include "nx_config.h"
#include "nx_signals.h"
#include "util.h"
#include "nx_splash.h"

#define NXIO_MAX_FD   1024
#define NXIO_BUCKETS  4096
#define NXIO_FILES    2048          /* stats records (the last one collects any overflow) */
#define ST_EMPTY   0
#define ST_READY   1
#define ST_LOADING 2

typedef struct Blk {
  uint64_t fid;
  uint32_t idx;
  uint32_t len;                     /* valid bytes */
  uint8_t  state;
  int32_t  hnext;                   /* hash chain */
  int32_t  prev, next;              /* LRU list (ready blocks only) */
} Blk;

typedef struct FileStat {
  uint64_t fid;
  char     name[48];
  int64_t  size;
  uint32_t opens, reads, preads, direct;
  uint64_t bytes, hits, misses, sd_reads, sd_bytes, sd_ns, max_ns;
  uint64_t dumped_sd_ns;
} FileStat;

typedef struct FdEnt {
  uint8_t  used;
  uint64_t fid;
  int64_t  size, pos;
  int64_t  last_end;                /* end offset of the previous request */
  uint32_t ra_blocks;               /* current read-ahead run length */
  FileStat *st;
} FdEnt;

static int       g_on = -1;
static uint32_t  g_bs, g_nblk, g_ra_max;
static uint8_t  *g_pool;
static Blk      *g_blk;
static int32_t   g_bucket[NXIO_BUCKETS];
static int32_t   g_lru_head = -1, g_lru_tail = -1;   /* head = most recent */
static int32_t   g_free = -1;                        /* free list via next */
static Mutex     g_lock;
static FdEnt     g_fd[NXIO_MAX_FD];
static FileStat  g_files[NXIO_FILES];
static int       g_nfiles;
static uint64_t  g_tot_reads, g_tot_hits, g_tot_miss, g_tot_sd_ns, g_tot_sd_bytes;

static uint64_t now_ns(void) { return armTicksToNs(armGetSystemTick()); }

/* Straight from newlib: memalign is --wrap'ed and would route page-aligned
 * 64 KB..64 MB requests into the GPU arena (cloverpit_gpuarena.c). */
extern void *__real_memalign(size_t align, size_t size);

/* ---- staging buffers for multi-block fills ------------------------------- */
#define NSTAGE 4
static uint8_t *g_stage[NSTAGE];
static Mutex    g_stage_lock[NSTAGE];
static uint8_t *stage_get(int *slot, size_t need) {
  for (int i = 0; i < NSTAGE; i++)
    if (mutexTryLock(&g_stage_lock[i])) {
      if (!g_stage[i]) g_stage[i] = __real_memalign(0x1000, (size_t)g_ra_max * g_bs);
      if (g_stage[i] && need <= (size_t)g_ra_max * g_bs) { *slot = i; return g_stage[i]; }
      mutexUnlock(&g_stage_lock[i]);
    }
  *slot = -1;
  return malloc(need);
}
static void stage_put(int slot, uint8_t *p) {
  if (slot >= 0) mutexUnlock(&g_stage_lock[slot]);
  else free(p);
}

/* ---- init ---------------------------------------------------------------- */
int nxio_enabled(void) {
  if (g_on >= 0) return g_on;
  g_on = 0;
  if (!nxcfg_int("io.cache", 1)) { debugPrintf("[io] block cache OFF ([io] cache = 0): legacy 1 MB window\n"); return 0; }
  uint32_t kb = (uint32_t)nxcfg_int("io.block_kb", 64);
  if (kb < 16) kb = 16;
  if (kb > 1024) kb = 1024;
  kb = 1u << (31 - __builtin_clz(kb));        /* power of two */
  uint32_t mb = (uint32_t)nxcfg_int("io.cache_mb", 32);
  if (mb < 4) mb = 4;
  if (mb > 256) mb = 256;
  uint32_t ra = (uint32_t)nxcfg_int("io.readahead_kb", 1024);
  g_bs = kb * 1024;
  g_nblk = (mb * 1024u) / kb;
  g_ra_max = ra / kb; if (g_ra_max < 1) g_ra_max = 1; if (g_ra_max > 64) g_ra_max = 64;
  g_pool = __real_memalign(0x1000, (size_t)g_nblk * g_bs);
  g_blk = calloc(g_nblk, sizeof *g_blk);
  if (!g_pool || !g_blk) {
    free(g_pool); free(g_blk); g_pool = NULL; g_blk = NULL;
    debugPrintf("[io] block cache: allocation of %u MB FAILED -- legacy path\n", mb);
    return 0;
  }
  for (int i = 0; i < NXIO_BUCKETS; i++) g_bucket[i] = -1;
  for (uint32_t i = 0; i < g_nblk; i++) { g_blk[i].hnext = -1; g_blk[i].prev = -1; g_blk[i].next = (int32_t)i + 1; }
  g_blk[g_nblk - 1].next = -1;
  g_free = 0;
  g_on = 1;
  debugPrintf("[io] block cache ON: %u x %u KB = %u MB, read-ahead up to %u KB\n",
              g_nblk, kb, mb, g_ra_max * kb);
  return 1;
}

/* ---- hash / LRU (g_lock held) -------------------------------------------- */
static inline uint32_t hb(uint64_t fid, uint32_t idx) {
  uint64_t h = fid ^ ((uint64_t)idx * 0x9E3779B97F4A7C15ull);
  return (uint32_t)(h ^ (h >> 29)) & (NXIO_BUCKETS - 1);
}
static int32_t h_find(uint64_t fid, uint32_t idx) {
  for (int32_t i = g_bucket[hb(fid, idx)]; i >= 0; i = g_blk[i].hnext)
    if (g_blk[i].fid == fid && g_blk[i].idx == idx && g_blk[i].state != ST_EMPTY) return i;
  return -1;
}
static void h_insert(int32_t i) { uint32_t b = hb(g_blk[i].fid, g_blk[i].idx); g_blk[i].hnext = g_bucket[b]; g_bucket[b] = i; }
static void h_remove(int32_t i) {
  int32_t *pp = &g_bucket[hb(g_blk[i].fid, g_blk[i].idx)];
  while (*pp >= 0) { if (*pp == i) { *pp = g_blk[i].hnext; break; } pp = &g_blk[*pp].hnext; }
  g_blk[i].hnext = -1;
}
static void lru_unlink(int32_t i) {
  Blk *b = &g_blk[i];
  if (b->prev >= 0) g_blk[b->prev].next = b->next; else g_lru_head = b->next;
  if (b->next >= 0) g_blk[b->next].prev = b->prev; else g_lru_tail = b->prev;
  b->prev = b->next = -1;
}
static void lru_push_front(int32_t i) {
  Blk *b = &g_blk[i];
  b->prev = -1; b->next = g_lru_head;
  if (g_lru_head >= 0) g_blk[g_lru_head].prev = i;
  g_lru_head = i;
  if (g_lru_tail < 0) g_lru_tail = i;
}
/* A block to fill: a free one, else the least recently used ready one. */
static int32_t blk_alloc(void) {
  int32_t i = g_free;
  if (i >= 0) { g_free = g_blk[i].next; g_blk[i].next = -1; return i; }
  i = g_lru_tail;
  if (i < 0) return -1;                       /* everything is LOADING */
  lru_unlink(i);
  h_remove(i);
  g_blk[i].state = ST_EMPTY;
  return i;
}
static void blk_release(int32_t i) {          /* back to the free list */
  g_blk[i].state = ST_EMPTY; g_blk[i].fid = 0;
  g_blk[i].next = g_free; g_free = i;
}

/* ---- stats --------------------------------------------------------------- */
static FileStat *stat_for(uint64_t fid, const char *path, int64_t size) {
  for (int i = 0; i < g_nfiles; i++) if (g_files[i].fid == fid) return &g_files[i];
  if (g_nfiles >= NXIO_FILES) return &g_files[NXIO_FILES - 1];
  FileStat *s = &g_files[g_nfiles++];
  memset(s, 0, sizeof *s);
  s->fid = fid; s->size = size;
  const char *b = path ? strrchr(path, '/') : NULL;
  snprintf(s->name, sizeof s->name, "%s", b ? b + 1 : (path ? path : "?"));
  return s;
}

/* ---- public: open / close / dup ------------------------------------------ */
void nxio_open(int fd, uint64_t fid, const char *path, int64_t size) {
  if (fd < 0 || fd >= NXIO_MAX_FD || !nxio_enabled()) return;
  mutexLock(&g_lock);
  FdEnt *e = &g_fd[fd];
  memset(e, 0, sizeof *e);
  e->used = 1; e->fid = fid; e->size = size; e->ra_blocks = 1; e->last_end = -1;
  e->st = stat_for(fid, path, size);
  e->st->opens++; e->st->size = size;
  mutexUnlock(&g_lock);
}
void nxio_close(int fd) {
  if (fd < 0 || fd >= NXIO_MAX_FD || g_on != 1) return;
  mutexLock(&g_lock);
  g_fd[fd].used = 0;
  mutexUnlock(&g_lock);
}
int nxio_owns(int fd) { return g_on == 1 && fd >= 0 && fd < NXIO_MAX_FD && g_fd[fd].used; }

static void ramf_kill(uint64_t fid);   /* g_lock held */

/* A path opened for writing / renamed / removed: drop its cached blocks. */
void nxio_invalidate(uint64_t fid) {
  if (g_on != 1 || !fid) return;
  mutexLock(&g_lock);
  ramf_kill(fid);
  for (uint32_t i = 0; i < g_nblk; i++)
    if (g_blk[i].state == ST_READY && g_blk[i].fid == fid) { lru_unlink((int32_t)i); h_remove((int32_t)i); blk_release((int32_t)i); }
  mutexUnlock(&g_lock);
}

/* ---- the read path -------------------------------------------------------- */
static long sd_pread(int fd, void *buf, size_t n, int64_t off, FileStat *st) {
  uint64_t t0 = now_ns();
  long total = 0;
  if (lseek(fd, off, SEEK_SET) >= 0)
    while ((size_t)total < n) { long r = read(fd, (char *)buf + total, n - total); if (r <= 0) break; total += r; }
  uint64_t dt = now_ns() - t0;
  mutexLock(&g_lock);
  if (st) { st->sd_reads++; st->sd_bytes += (uint64_t)(total > 0 ? total : 0); st->sd_ns += dt; if (dt > st->max_ns) st->max_ns = dt; }
  g_tot_sd_ns += dt; g_tot_sd_bytes += (uint64_t)(total > 0 ? total : 0);
  mutexUnlock(&g_lock);
  if (dt > 100000000ull) {
    static int nslow;
    if (nslow < 200) { nslow++; debugPrintf("[io] slow SD read: %s off=%lld len=%zu -> %ld in %llu ms\n",
                                            st ? st->name : "?", (long long)off, n, total, (unsigned long long)(dt / 1000000)); }
  }
  return total;
}

/* ---- RAM files (the preload, libc_shim.c) ---------------------------------
 * Whole small data files kept in memory at their exact size and never
 * evicted: their reads never reach the SD card. (The first try put them in
 * the block cache: each file took at least one 64 KB block, the preload
 * overflowed the cache and evicted itself, and the game's own reads evicted
 * the rest before Room Settings -- 02/11, v0.1.32-34.) Insert-only lists
 * published with release stores, so readers take no lock; a file opened for
 * writing is tombstoned (size -1). */
#define RAMF_BUCKETS 4096
typedef struct RamFile { uint64_t fid; volatile int64_t size; uint8_t *data; struct RamFile *next; } RamFile;
static RamFile *volatile g_ramf[RAMF_BUCKETS];
static uint64_t g_ramf_bytes, g_ramf_hits;
static uint32_t g_ramf_n;

static RamFile *ramf_find(uint64_t fid) {
  for (RamFile *r = __atomic_load_n(&g_ramf[fid & (RAMF_BUCKETS - 1)], __ATOMIC_ACQUIRE); r; r = r->next)
    if (r->fid == fid) return r->size >= 0 ? r : NULL;
  return NULL;
}

/* The bytes of a file kept in memory (phantom fds, libc_shim.c). The data is
 * never freed, so the pointer stays valid even if the file is invalidated. */
int nxio_ramfile_get(uint64_t fid, const uint8_t **data, int64_t *size) {
  if (g_on != 1 || !fid) return 0;
  RamFile *r = ramf_find(fid);
  if (!r) return 0;
  *data = r->data; *size = r->size;
  return 1;
}

/* Keep a whole file in memory; takes ownership of `data` (malloc'd). */
int nxio_ramfile_add(uint64_t fid, uint8_t *data, int64_t size) {
  if (!fid || !data || size < 0 || !nxio_enabled()) return 0;
  RamFile *r = malloc(sizeof *r);
  if (!r) return 0;
  r->fid = fid; r->size = size; r->data = data;
  mutexLock(&g_lock);
  RamFile *head = g_ramf[fid & (RAMF_BUCKETS - 1)];
  for (RamFile *q = head; q; q = q->next)
    if (q->fid == fid) { mutexUnlock(&g_lock); free(r); return 0; }
  r->next = head;
  __atomic_store_n(&g_ramf[fid & (RAMF_BUCKETS - 1)], r, __ATOMIC_RELEASE);
  g_ramf_bytes += (uint64_t)size; g_ramf_n++;
  mutexUnlock(&g_lock);
  return 1;
}

static void ramf_kill(uint64_t fid) {
  for (RamFile *r = g_ramf[fid & (RAMF_BUCKETS - 1)]; r; r = r->next)
    if (r->fid == fid && r->size >= 0) { g_ramf_bytes -= (uint64_t)r->size; g_ramf_n--; r->size = -1; }
}

/* Copy [off, off+n) out of the cache, filling it as needed. g_lock NOT held. */
static long cached_read(int fd, FdEnt *e, void *buf, size_t n, int64_t off) {
  RamFile *rf = ramf_find(e->fid);
  if (rf) {
    if (off >= rf->size || n == 0) return 0;
    if ((int64_t)n > rf->size - off) n = (size_t)(rf->size - off);
    memcpy(buf, rf->data + off, n);
    __atomic_add_fetch(&g_ramf_hits, 1, __ATOMIC_RELAXED);
    return (long)n;
  }
  if (off >= e->size || n == 0) return 0;
  if ((int64_t)n > e->size - off) n = (size_t)(e->size - off);
  FileStat *st = e->st;
  const uint64_t fid = e->fid;
  /* Sequential detection on this fd: grow the run, else start small. */
  int seq = (off == e->last_end);
  e->last_end = off + (int64_t)n;
  if (seq) { if (e->ra_blocks < g_ra_max) e->ra_blocks = e->ra_blocks ? e->ra_blocks * 2 : 1; if (e->ra_blocks > g_ra_max) e->ra_blocks = g_ra_max; }
  else e->ra_blocks = 1;

  /* Large request: straight from the SD into the caller's buffer. */
  if (n >= 4 * (size_t)g_bs) {
    mutexLock(&g_lock); if (st) st->direct++; mutexUnlock(&g_lock);
    return sd_pread(fd, buf, n, off, st);
  }

  size_t done = 0;
  while (done < n) {
    int64_t p = off + (int64_t)done;
    uint32_t idx = (uint32_t)(p / g_bs);
    uint32_t in = (uint32_t)(p % g_bs);
    mutexLock(&g_lock);
    int32_t b = h_find(fid, idx);
    if (b >= 0 && g_blk[b].state == ST_READY) {
      uint32_t avail = g_blk[b].len > in ? g_blk[b].len - in : 0;
      if (!avail) { mutexUnlock(&g_lock); break; }               /* past EOF */
      size_t k = n - done < avail ? n - done : avail;
      memcpy((char *)buf + done, g_pool + (size_t)b * g_bs + in, k);
      lru_unlink(b); lru_push_front(b);
      if (st) st->hits++;
      g_tot_hits++;
      mutexUnlock(&g_lock);
      done += k;
      continue;
    }
    if (b >= 0) {                                               /* LOADING by another thread */
      mutexUnlock(&g_lock);
      nx_sig_safepoint();
      svcSleepThread(100000);
      continue;
    }
    /* Miss: claim a run of missing blocks starting at idx. The run covers at
     * least the rest of this request and, when sequential, the read-ahead. */
    uint32_t last_blk = (uint32_t)((e->size - 1) / g_bs);
    uint32_t need = (uint32_t)((off + (int64_t)n - 1) / g_bs) - idx + 1;
    uint32_t run = need > e->ra_blocks ? need : e->ra_blocks;
    if (run > g_ra_max) run = g_ra_max;
    if (idx + run - 1 > last_blk) run = last_blk - idx + 1;
    int32_t ids[64]; uint32_t got = 0;
    for (; got < run; got++) {
      if (got && h_find(fid, idx + got) >= 0) break;           /* already cached further on */
      int32_t nb = blk_alloc();
      if (nb < 0) break;
      g_blk[nb].fid = fid; g_blk[nb].idx = idx + got; g_blk[nb].len = 0; g_blk[nb].state = ST_LOADING;
      h_insert(nb);
      ids[got] = nb;
    }
    if (st) st->misses++;
    g_tot_miss++;
    mutexUnlock(&g_lock);
    if (!got) {                                                 /* cache exhausted: read directly */
      long r = sd_pread(fd, (char *)buf + done, n - done, p, st);
      if (r > 0) done += (size_t)r;
      break;
    }
    size_t want = (size_t)got * g_bs;
    int64_t base = (int64_t)idx * g_bs;
    if ((int64_t)want > e->size - base) want = (size_t)(e->size - base);
    int slot; uint8_t *tmp = stage_get(&slot, want);
    long r = tmp ? sd_pread(fd, tmp, want, base, st) : -1;
    mutexLock(&g_lock);
    for (uint32_t k = 0; k < got; k++) {
      int32_t nb = ids[k];
      long boff = (long)k * g_bs;
      long blen = r > boff ? r - boff : 0;
      if (blen > (long)g_bs) blen = g_bs;
      if (blen <= 0) { h_remove(nb); blk_release(nb); continue; }   /* short read: drop */
      memcpy(g_pool + (size_t)nb * g_bs, tmp + boff, (size_t)blen);
      g_blk[nb].len = (uint32_t)blen;
      g_blk[nb].state = ST_READY;
      lru_push_front(nb);
    }
    mutexUnlock(&g_lock);
    if (tmp) stage_put(slot, tmp);
    if (r <= 0) break;                                          /* I/O error or EOF */
  }
  return (long)done;
}

long nxio_read(int fd, void *buf, size_t n) {
  nx_splash_tick();                 /* the main thread may own the GL context */
  FdEnt *e = &g_fd[fd];
  mutexLock(&g_lock);
  int64_t pos = e->pos;
  if (e->st) e->st->reads++, e->st->bytes += n;
  g_tot_reads++;
  mutexUnlock(&g_lock);
  long r = cached_read(fd, e, buf, n, pos);
  if (r > 0) { mutexLock(&g_lock); e->pos = pos + r; mutexUnlock(&g_lock); }
  return r;
}
long nxio_pread(int fd, void *buf, size_t n, int64_t off) {
  FdEnt *e = &g_fd[fd];
  mutexLock(&g_lock);
  if (e->st) e->st->preads++, e->st->bytes += n;
  g_tot_reads++;
  mutexUnlock(&g_lock);
  return cached_read(fd, e, buf, n, off);
}
int64_t nxio_lseek(int fd, int64_t off, int whence) {
  FdEnt *e = &g_fd[fd];
  mutexLock(&g_lock);
  int64_t np = whence == SEEK_SET ? off : whence == SEEK_CUR ? e->pos + off : e->size + off;
  if (np >= 0) e->pos = np;
  mutexUnlock(&g_lock);
  return np >= 0 ? np : -1;
}

uint64_t nxio_total_sd_bytes(void) { return g_tot_sd_bytes; }

/* ---- reporting ----------------------------------------------------------- */
void nxio_report(int force) {
  if (g_on != 1) return;
  static uint64_t last_sd_ns;
  if (!force && g_tot_sd_ns == last_sd_ns) return;
  last_sd_ns = g_tot_sd_ns;
  /* top 12 files by SD time since the last report */
  int order[12]; int no = 0;
  mutexLock(&g_lock);
  for (int k = 0; k < 12; k++) {
    int best = -1; uint64_t bv = 0;
    for (int i = 0; i < g_nfiles; i++) {
      uint64_t v = g_files[i].sd_ns - g_files[i].dumped_sd_ns;
      int taken = 0; for (int j = 0; j < no; j++) if (order[j] == i) taken = 1;
      if (!taken && v > bv) { bv = v; best = i; }
    }
    if (best < 0) break;
    order[no++] = best;
  }
  uint64_t reads = g_tot_reads, hits = g_tot_hits, miss = g_tot_miss, sdns = g_tot_sd_ns, sdb = g_tot_sd_bytes;
  FileStat snap[12];
  for (int k = 0; k < no; k++) { snap[k] = g_files[order[k]]; g_files[order[k]].dumped_sd_ns = g_files[order[k]].sd_ns; }
  mutexUnlock(&g_lock);
  debugPrintf("[io] cache: %llu reads, %llu block hits, %llu misses, SD %llu MB in %llu ms total\n",
              (unsigned long long)reads, (unsigned long long)hits, (unsigned long long)miss,
              (unsigned long long)(sdb >> 20), (unsigned long long)(sdns / 1000000));
  if (g_ramf_n)
    debugPrintf("[io] preloaded files in memory: %u (%.1f MB), %llu reads served\n", g_ramf_n,
                g_ramf_bytes / 1048576.0, (unsigned long long)g_ramf_hits);
  for (int k = 0; k < no; k++) {
    FileStat *s = &snap[k];
    debugPrintf("[io]   %-40s %7lld KB  opens=%u reads=%u preads=%u direct=%u hits=%llu miss=%llu  SD %llu KB/%llu ms (+%llu ms) max %llu ms\n",
                s->name, (long long)(s->size >> 10), s->opens, s->reads, s->preads, s->direct,
                (unsigned long long)s->hits, (unsigned long long)s->misses,
                (unsigned long long)(s->sd_bytes >> 10), (unsigned long long)(s->sd_ns / 1000000),
                (unsigned long long)((s->sd_ns - s->dumped_sd_ns) / 1000000),
                (unsigned long long)(s->max_ns / 1000000));
  }
}
