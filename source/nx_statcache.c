/* nx_statcache.c -- stat() of the game's read-only data, answered from memory.
 *
 * WHY
 * On hardware the long freezes in menus and at level start were stat() calls:
 * the [hitch] breakdown of a 17.6 s "Room Settings" frame showed 1728 stats
 * for 14.8 s, a level start 650 stats for ~6 s. Unity checks the same asset
 * files again and again (LocalFileSystemAndroid::Open -> IsDir), and libnx's
 * fsdev_stat() costs five FS service round trips per file (GetEntryType,
 * OpenFile, GetSize, Close, GetFileTimeStampRaw), each one a lookup in a
 * 1570-entry FAT directory: ~8.6 ms a call.
 *
 * WHAT
 * The first lookup in a directory under <root>/assets or <root>/il2cpp reads
 * the whole directory once (fsFsOpenDirectory + fsDirRead: names, types and
 * sizes in bulk). Every later stat() in it -- including "no such file" -- is
 * answered from that listing with the fields fsdev_stat() would have filled.
 * Nothing in those trees is written by the game; a write-open there drops the
 * directory anyway. [io] stat_cache = 0 turns it off.
 *
 * MIT -- part of mvlo_nx.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <switch.h>

#include "nx_statcache.h"
#include "nx_config.h"
#include "cloverpit_root.h"
#include "util.h"

typedef struct { char *name; uint32_t hash; int8_t type; int64_t size; int next; } Ent;
typedef struct {
  char    *path;          /* FS path of the directory, no trailing slash */
  uint32_t hash;
  Ent     *ent;
  int      n;
  int     *bucket;        /* nb heads into ent[], -1 = empty */
  int      nb;
  time_t   ctime, mtime, atime;
} Dir;

#define MAXDIRS 128
static Dir           g_dirs[MAXDIRS];
static int           g_ndirs;
static Mutex         g_lock;
static int           g_on = -1;
static FsFileSystem *g_fs;
static char          g_scope[2][FS_MAX_PATH];
static size_t        g_scope_len[2];
static char          g_root[FS_MAX_PATH];   /* FS path of the game folder */
static size_t        g_root_len;
static uint64_t      g_hits, g_absent;

/* Why a lookup was not answered (shown by nx_statcache_report with a few
 * sample paths): the first build answered 20 stats out of thousands and said
 * nothing about the rest. */
enum { MISS_TRANSLATE, MISS_SCOPE, MISS_LIST, MISS_N };
static const char *const k_miss_name[MISS_N] = { "untranslatable", "outside assets/ and il2cpp/", "listing failed" };
static uint64_t g_miss[MISS_N];
static char     g_miss_sample[MISS_N][4][160];
static void note_miss(int why, const char *path) {
  uint64_t k = g_miss[why]++;
  if (k < 4) snprintf(g_miss_sample[why][k], sizeof g_miss_sample[why][k], "%s", path);
}

/* Collapse "//", drop "/./", resolve "/../" in place (FS paths are absolute). */
static void normalize(char *p) {
  char *out = p, *in = p;
  while (*in) {
    if (in[0] == '/' && in[1] == '/') { in++; continue; }
    if (in[0] == '/' && in[1] == '.' && (in[2] == '/' || in[2] == 0)) { in += 2; continue; }
    if (in[0] == '/' && in[1] == '.' && in[2] == '.' && (in[3] == '/' || in[3] == 0)) {
      while (out > p && *--out != '/') {}
      in += 3;
      continue;
    }
    *out++ = *in++;
  }
  if (out == p) *out++ = '/';
  *out = 0;
}

static uint32_t hash_ci(const char *s, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c >= 'A' && c <= 'Z') c += 32;      /* FAT is case-insensitive */
    h = (h ^ c) * 16777619u;
  }
  return h;
}

static void init_once(void) {
  if (g_on >= 0) return;
  mutexInit(&g_lock);
  g_on = nxcfg_int("io.stat_cache", 1);
  if (!g_on) { debugPrintf("[io] stat cache OFF ([io] stat_cache = 0)\n"); return; }
  char root[FS_MAX_PATH];
  FsFileSystem *fs = NULL;
  if (fsdevTranslatePath(cp_game_root(), &fs, root) == -1 || !fs) {
    debugPrintf("[io] stat cache OFF: cannot translate %s\n", cp_game_root());
    g_on = 0;
    return;
  }
  size_t rl = strlen(root);
  while (rl > 1 && root[rl - 1] == '/') root[--rl] = 0;
  g_fs = fs;
  snprintf(g_root, FS_MAX_PATH, "%s", root);
  g_root_len = strlen(g_root);
  snprintf(g_scope[0], FS_MAX_PATH, "%s/assets", root);
  snprintf(g_scope[1], FS_MAX_PATH, "%s/il2cpp", root);
  g_scope_len[0] = strlen(g_scope[0]);
  g_scope_len[1] = strlen(g_scope[1]);
  debugPrintf("[io] stat cache ON for %s and %s\n", g_scope[0], g_scope[1]);
}

/* dir (FS path) is one of the scope roots or below one */
static int in_scope(const char *dir, size_t len) {
  for (int i = 0; i < 2; i++) {
    size_t sl = g_scope_len[i];
    if (len >= sl && !strncasecmp(dir, g_scope[i], sl) && (len == sl || dir[sl] == '/')) return 1;
  }
  return 0;
}

static Dir *find_dir(const char *dir, size_t len, uint32_t h) {
  for (int i = 0; i < g_ndirs; i++)
    if (g_dirs[i].hash == h && strlen(g_dirs[i].path) == len && !strncasecmp(g_dirs[i].path, dir, len))
      return &g_dirs[i];
  return NULL;
}

static void free_dir(Dir *d) {
  for (int i = 0; i < d->n; i++) free(d->ent[i].name);
  free(d->ent); free(d->bucket); free(d->path);
  memset(d, 0, sizeof *d);
}

/* Read a whole directory. Returns NULL (caller falls back to a real stat) if
 * anything fails: a listing we are not sure of must never answer "absent". */
static Dir *list_dir(const char *dir, size_t len, uint32_t h) {
  if (g_ndirs >= MAXDIRS) return NULL;
  char path[FS_MAX_PATH];
  if (len >= sizeof path) return NULL;
  memcpy(path, dir, len); path[len] = 0;

  uint64_t t0 = armTicksToNs(armGetSystemTick());
  FsDir fd;
  Result orc = fsFsOpenDirectory(g_fs, path, FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, &fd);
  if (R_FAILED(orc)) {
    static int nfail;
    if (nfail++ < 16) debugPrintf("[io] stat cache: cannot list %s (rc=0x%x)\n", path, orc);
    return NULL;
  }
  enum { CHUNK = 64 };
  FsDirectoryEntry *buf = malloc(CHUNK * sizeof *buf);
  Ent *ent = NULL;
  int n = 0, cap = 0, ok = buf != NULL;
  while (ok) {
    s64 got = 0;
    if (R_FAILED(fsDirRead(&fd, &got, CHUNK, buf))) { ok = 0; break; }
    if (got <= 0) break;
    for (s64 i = 0; i < got && ok; i++) {
      if (n == cap) {
        cap = cap ? cap * 2 : 128;
        Ent *ne = realloc(ent, (size_t)cap * sizeof *ent);
        if (!ne) { ok = 0; break; }
        ent = ne;
      }
      size_t nl = strnlen(buf[i].name, sizeof buf[i].name);
      ent[n].name = malloc(nl + 1);
      if (!ent[n].name) { ok = 0; break; }
      memcpy(ent[n].name, buf[i].name, nl); ent[n].name[nl] = 0;
      ent[n].hash = hash_ci(ent[n].name, nl);
      ent[n].type = buf[i].type;
      ent[n].size = buf[i].file_size;
      n++;
    }
  }
  fsDirClose(&fd);
  free(buf);

  Dir *d = &g_dirs[g_ndirs];
  memset(d, 0, sizeof *d);
  d->ent = ent; d->n = n;
  d->nb = n < 16 ? 16 : n;
  d->bucket = malloc((size_t)d->nb * sizeof(int));
  d->path = malloc(len + 1);
  if (!ok || !d->bucket || !d->path) { free_dir(d); return NULL; }
  memcpy(d->path, dir, len); d->path[len] = 0;
  d->hash = h;
  for (int i = 0; i < d->nb; i++) d->bucket[i] = -1;
  for (int i = 0; i < n; i++) {
    int b = (int)(ent[i].hash % (uint32_t)d->nb);
    ent[i].next = d->bucket[b]; d->bucket[b] = i;
  }
  /* fsdev_stat() reports each file's own timestamps; one read per directory
   * (from its first file) keeps them plausible without 5 round trips a file. */
  for (int i = 0; i < n; i++) {
    if (ent[i].type != FsDirEntryType_File) continue;
    char fp[FS_MAX_PATH];
    snprintf(fp, sizeof fp, "%s/%s", d->path, ent[i].name);
    FsTimeStampRaw ts = {0};
    if (R_SUCCEEDED(fsFsGetFileTimeStampRaw(g_fs, fp, &ts)) && ts.is_valid) {
      d->ctime = (time_t)ts.created; d->mtime = (time_t)ts.modified; d->atime = (time_t)ts.accessed;
    }
    break;
  }
  g_ndirs++;
  debugPrintf("[io] stat cache: listed %s (%d entries) in %llu ms\n", d->path, n,
              (unsigned long long)((armTicksToNs(armGetSystemTick()) - t0) / 1000000ull));
  return d;
}

int nx_statcache_stat(const char *path, struct stat *st) {
  init_once();
  if (!g_on || !path || !*path) return -1;
  char fp[FS_MAX_PATH];
  if (fsdevTranslatePath(path, NULL, fp) == -1) { note_miss(MISS_TRANSLATE, path); return -1; }
  normalize(fp);
  size_t len = strlen(fp);
  while (len > 1 && fp[len - 1] == '/') fp[--len] = 0;
  /* The game folder and its parents ("/", "/switch", "/switch/mvlo") exist:
   * the .nro was started from there. Unity re-checks them on every path it
   * builds (~190 real stats at 8 ms each when opening a menu). */
  if (len <= g_root_len && !strncasecmp(g_root, fp, len) &&
      (len == g_root_len || g_root[len] == '/' || (len == 1 && fp[0] == '/'))) {
    memset(st, 0, sizeof *st);
    st->st_nlink = 1;
    st->st_mode = S_IFDIR | S_IRWXU | S_IRWXG | S_IRWXO;
    g_hits++;
    return 1;
  }
  const char *slash = strrchr(fp, '/');
  if (!slash || slash == fp) { note_miss(MISS_SCOPE, fp); return -1; }
  size_t dlen = (size_t)(slash - fp);
  const char *name = slash + 1;
  size_t nlen = len - dlen - 1;
  if (!nlen || !in_scope(fp, dlen)) { note_miss(MISS_SCOPE, fp); return -1; }

  int r = -1;
  mutexLock(&g_lock);
  uint32_t dh = hash_ci(fp, dlen);
  Dir *d = find_dir(fp, dlen, dh);
  if (!d) d = list_dir(fp, dlen, dh);
  if (d) {
    uint32_t h = hash_ci(name, nlen);
    int i = d->bucket[h % (uint32_t)d->nb];
    while (i >= 0 && !(d->ent[i].hash == h && strlen(d->ent[i].name) == nlen && !strncasecmp(d->ent[i].name, name, nlen)))
      i = d->ent[i].next;
    if (i < 0) {
      r = 0; g_absent++;
    } else {
      memset(st, 0, sizeof *st);
      st->st_nlink = 1;
      if (d->ent[i].type == FsDirEntryType_Dir) {
        st->st_mode = S_IFDIR | S_IRWXU | S_IRWXG | S_IRWXO;
      } else {
        st->st_mode = S_IFREG | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;
        st->st_size = (off_t)d->ent[i].size;
      }
      st->st_ctime = d->ctime; st->st_mtime = d->mtime; st->st_atime = d->atime;
      r = 1; g_hits++;
    }
  } else {
    note_miss(MISS_LIST, fp);
  }
  mutexUnlock(&g_lock);
  return r;
}

void nx_statcache_invalidate(const char *path) {
  if (g_on <= 0 || !path) return;
  char fp[FS_MAX_PATH];
  if (fsdevTranslatePath(path, NULL, fp) == -1) return;
  const char *slash = strrchr(fp, '/');
  if (!slash) return;
  size_t dlen = (size_t)(slash - fp);
  mutexLock(&g_lock);
  Dir *d = find_dir(fp, dlen, hash_ci(fp, dlen));
  if (d) {
    debugPrintf("[io] stat cache: %s written, listing dropped\n", d->path);
    free_dir(d);
    *d = g_dirs[--g_ndirs];
    memset(&g_dirs[g_ndirs], 0, sizeof g_dirs[g_ndirs]);
  }
  mutexUnlock(&g_lock);
}

void nx_statcache_report(void) {
  if (g_on <= 0) return;
  debugPrintf("[io] stat cache: %d dir(s) listed, %llu stat(s) answered, %llu of them \"absent\"\n",
              g_ndirs, (unsigned long long)(g_hits + g_absent), (unsigned long long)g_absent);
  for (int w = 0; w < MISS_N; w++) {
    if (!g_miss[w]) continue;
    debugPrintf("[io] stat cache: %llu not answered (%s), e.g. %s | %s | %s | %s\n",
                (unsigned long long)g_miss[w], k_miss_name[w], g_miss_sample[w][0],
                g_miss_sample[w][1], g_miss_sample[w][2], g_miss_sample[w][3]);
  }
}
