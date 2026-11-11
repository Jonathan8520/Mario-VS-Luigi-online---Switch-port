/* mvlo_patches.c -- libunity.so (Unity 6000.3.10f1) engine patches.
 *
 * Ported from cloverpit_nx's cloverpit_patches.c (MIT). The PATCHES are the
 * same ideas; what changed is how their addresses are known:
 *
 *   cloverpit_nx fingerprinted a stripped game binary. We build the game
 *   ourselves, so libunity.so is Unity's stock Android player, byte-identical
 *   to PlaybackEngines/AndroidPlayer/Variations/il2cpp/Release/Libs/arm64-v8a.
 *   tools/re/derive_unity.py finds every target BY NAME in Unity's own
 *   libunity.sym.so and generates mvlo_unity_offsets.h. At runtime we check
 *   the GNU BuildID first, then every guard word, and only then write.
 *
 * Patches (each one switchable in mvlo.ini [debug], default on):
 *   [R]  region granularity 256 MB -> 64 MB (21 sites, all-or-nothing)
 *   [C]  ChoreographerBase::Get() -> NULL      (choreographer_patch)
 *   [A]  AndroidAudio::GetAndroidAudioOutputType -> 2 = OpenSL (audio_opensl_patch)
 *   [F]  FMOD OpenSL buffer geometry / period    (fmod_patch)
 *   [T]  TimeManager::Update wall-clock detour   (time_hook)
 *   [V]  vsync feed: a 60 Hz thread calls the engine's own
 *        SetFrameTimeNanos(t), exactly what Choreographer.doFrame does on a
 *        phone. This advances the counter WaitVSync() blocks on AND the frame
 *        timestamp GetVSyncInfo() derives Time from. (cloverpit_nx only
 *        bumped the counter, which unblocked the wait but left the timestamp
 *        frozen -- the likely root cause of its "TimeManager never ticks".)
 *   [Z]  libil2cpp CreateZStream: deflate level 6 -> 1 (zlib_level)
 *
 * MIT -- part of mvlo_nx.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <switch.h>
#include <elf.h>

#include "so_util.h"
#include "util.h"
#include "config.h"
#include "nx_config.h"
#include "mvlo_unity_offsets.h"
#include "nx_autopsy.h"

#define A64_RET 0xd65f03c0u

static int g_buildid_ok = 0;

/* ------------------------------------------------------------ BuildID */
static int read_buildid(so_module *m, char *out, size_t cap) {
  /* Walk PT_NOTE in the loaded image. */
  out[0] = 0;
  for (int i = 0; i < m->phnum; i++) {
    Elf64_Phdr *p = &m->phdr[i];
    if (p->p_type != PT_NOTE) continue;
    uint8_t *n = so_readable((uintptr_t)m->load_virtbase + p->p_vaddr, p->p_memsz), *end = n + p->p_memsz;
    while (n + 12 <= end) {
      uint32_t namesz = *(uint32_t *)n, descsz = *(uint32_t *)(n + 4), type = *(uint32_t *)(n + 8);
      uint8_t *name = n + 12, *desc = name + ((namesz + 3) & ~3u);
      if (type == 3 /* NT_GNU_BUILD_ID */ && namesz == 4 && !memcmp(name, "GNU", 4)) {
        size_t o = 0;
        for (uint32_t k = 0; k < descsz && o + 3 < cap; k++)
          o += (size_t)snprintf(out + o, cap - o, "%02x", desc[k]);
        return 0;
      }
      n = desc + ((descsz + 3) & ~3u);
    }
  }
  return -1;
}

int mvlo_check_buildid(so_module *unity) {
  char id[96];
  if (read_buildid(unity, id, sizeof id) < 0) {
    debugPrintf("[patch] libunity has no readable BuildID -- trusting guard words only\n");
    g_buildid_ok = 1;
    return 1;
  }
  g_buildid_ok = !strcmp(id, MVLO_UNITY_BUILDID);
  debugPrintf("[patch] libunity BuildID %s %s\n", id,
              g_buildid_ok ? "(matches the patch table)" :
              "!= " MVLO_UNITY_BUILDID " -- WRONG Unity version: engine patches DISABLED");
  return g_buildid_ok;
}

static int want(const char *key) { return nxcfg_int(key, 1) != 0; }

static uintptr_t at(so_module *u, uint32_t rva) { return (uintptr_t)u->load_virtbase + rva; }

static int guard(const char *what, uintptr_t a, uint32_t expect) {
  uint32_t got = so_read32(a);
  if (got != expect) {
    debugPrintf("[patch] %-30s SKIP (guard %08x, found %08x)\n", what, expect, got);
    return 0;
  }
  return 1;
}

static int write_words(const char *what, uintptr_t a, const uint32_t *w, size_t n) {
  if (so_patch_code((void *)a, w, n * 4) < 0) {
    debugPrintf("[patch] %-30s FAILED (so_patch_code)\n", what);
    return 0;
  }
  debugPrintf("[patch] %-30s applied @ %p\n", what, (void *)a);
  return 1;
}

/* ----------------------------------------------------------- [R] region */
int cp_install_region_patch(so_module *unity) {
  if (!g_buildid_ok) return 0;
  if (!want("debug.region_patch")) {
    debugPrintf("[region] disabled in mvlo.ini -- Unity keeps 256 MB regions (likely OOM on 4 GB consoles)\n");
    return 0;
  }
  uintptr_t base = (uintptr_t)unity->load_virtbase;
  for (size_t i = 0; i < MVLO_REGION_WORDS_N; i++) {
    const NxPatchWord *p = &MVLO_REGION_WORDS[i];
    uint32_t got = so_read32(base + p->off);
    if (got != p->from) {
      debugPrintf("[region] ABORT: site %u @+0x%07x expected %08x found %08x -- NOTHING patched\n",
                  (unsigned)i, p->off, p->from, got);
      return 0;
    }
  }
  for (size_t i = 0; i < MVLO_REGION_WORDS_N; i++) {
    const NxPatchWord *p = &MVLO_REGION_WORDS[i];
    uint32_t w = p->to;
    if (so_patch_code((void *)(base + p->off), &w, 4) < 0) {
      debugPrintf("[region] FAILED writing site %u -- allocator state undefined\n", (unsigned)i);
      return 0;
    }
  }
  debugPrintf("[region] %u/%u sites patched: region granularity 256MB -> %dMB\n",
              (unsigned)MVLO_REGION_WORDS_N, (unsigned)MVLO_REGION_WORDS_N, MVLO_REGION_GRANULARITY_MB);
  return 1;
}

/* --------------------------------------------------- clock / vsync feed */
static uint64_t nx_now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

typedef void (*fn_tm_body)(void *tm, double newTime);
typedef void (*fn_set_frame_time)(long ns);
static fn_tm_body g_tm_body;
static fn_set_frame_time g_set_frame_time;
static volatile int64_t *g_vsync_counter;
static void *g_tm;
static uint64_t g_clk_base_ns;
static double g_last_engine_time;
static uint64_t g_vsync_ticks;
static volatile int g_clock_run;
static Thread g_clock_thread;
static uint8_t g_clock_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));

void  *cp_clock_tm(void) { return g_tm; }
double cp_clock_last_time(void) { return g_last_engine_time; }
uint64_t mvlo_vsync_ticks(void) { return g_vsync_ticks; }

static void cp_tm_update_hook(void *tm, double newTime) {
  (void)newTime;
  uint64_t *fc = (uint64_t *)((uintptr_t)tm + TM_FIELD_FRAMECOUNT);
  uint32_t *rc = (uint32_t *)((uintptr_t)tm + TM_FIELD_RENDERCOUNT);
  uint8_t  *pz = (uint8_t *)((uintptr_t)tm + TM_FIELD_PAUSE);
  (*fc)++;
  (*rc)++;
  if (*pz) return;
  g_tm = tm;
  uint64_t now = nx_now_ns();
  if (!g_clk_base_ns) g_clk_base_ns = now;
  double t = (double)(now - g_clk_base_ns) / 1e9;
  g_last_engine_time = t;
  g_tm_body(tm, t);
}

#define VSYNC_PERIOD_NS 16666667ull

static void clock_thread(void *arg) {
  (void)arg;
  install_bionic_tls(g_clock_tls);
  nx_autopsy_name_self("vsync-clock");
  uint64_t base = nx_now_ns(), next = base + VSYNC_PERIOD_NS;
  while (g_clock_run) {
    uint64_t now = nx_now_ns();
    if (now < next) { svcSleepThread(next - now); continue; }
    /* Strictly wall-clock paced; never catch up in a burst (flooding vi takes
     * the whole console down, per the Layton/CloverPit ports). */
    while (next <= now) next += VSYNC_PERIOD_NS;
    if (g_set_frame_time) g_set_frame_time((long)(now - base));
    else if (g_vsync_counter) __atomic_add_fetch(g_vsync_counter, 1, __ATOMIC_RELAXED);
    g_vsync_ticks++;
  }
}

/* android::ui::DisplayDialog(title, message, button1, button2): on Android a
 * modal Java dialog; here nobody can press its button and the engine waits
 * forever. Log it (this is how Unity reports fatal init errors such as
 * "Failed to load Il2CPP") and return as if dismissed. */
static void hk_display_dialog(const char *title, const char *msg, const char *b1, const char *b2) {
  debugPrintf("[unity] DIALOG \"%s\": %s  [%s|%s] -- auto-dismissed\n", title ? title : "",
              msg ? msg : "", b1 ? b1 : "", b2 ? b2 : "");
  debug_log_flush();
}

/* ------------------------------------------------------------- install */
int cp_install_patches(so_module *unity) {
  int applied = 0;
  if (!g_buildid_ok) {
    debugPrintf("[patch] BuildID mismatch: no engine patch applied. Rebuild the SD data with the "
                "android workflow of this exact port version.\n");
    return 0;
  }

  if (want("debug.choreographer_patch")) {
    static const uint32_t stub[] = { 0xd2800000u, A64_RET };   /* mov x0,#0 ; ret */
    uintptr_t a = at(unity, RVA_CHOREO_GET);
    if (guard("ChoreographerBase::Get", a, GUARD_CHOREO_GET))
      applied += write_words("ChoreographerBase::Get -> NULL", a, stub, 2);
  }

  if (want("debug.audio_opensl_patch")) {
    static const uint32_t stub[] = { 0x52800040u, A64_RET };   /* mov w0,#2 ; ret */
    uintptr_t a = at(unity, RVA_AUDIO_OUTTYPE);
    if (guard("GetAndroidAudioOutputType", a, GUARD_AUDIO_OUTTYPE))
      applied += write_words("GetAndroidAudioOutputType -> 2", a, stub, 2);
  }

  {
    uintptr_t a = at(unity, RVA_DISPLAY_DIALOG);
    if (guard("android::ui::DisplayDialog", a, GUARD_DISPLAY_DIALOG)) {
      hook_arm64(a, (uintptr_t)&hk_display_dialog);
      debugPrintf("[patch] %-30s hooked (log + auto-dismiss)\n", "android::ui::DisplayDialog");
      applied++;
    }
  }

#if CP_PATCH_FMOD_BUFFER_GEOMETRY
  if (want("debug.fmod_patch")) {
    uintptr_t run = at(unity, RVA_FMOD_BUFGEOM);
    uintptr_t br = run + 0x1c, ld = run - 8, cz = run - 4;
    if (guard("FMOD geometry run", run, 0x5100068au) && guard("FMOD b.ls", br, 0x54000089u) &&
        guard("FMOD period ldr", ld, GUARD_FMOD_PERIOD_LDR) && guard("FMOD period cbz", cz, GUARD_FMOD_PERIOD_CBZ)) {
      static const uint32_t b_uncond = 0x14000004u;            /* b +0x10 (same target) */
      applied += write_words("FMOD OpenSL buffer geometry", br, &b_uncond, 1);
      const uint32_t mv = 0x52800000u | ((CP_AUDIO_PERIOD_FRAMES & 0xffffu) << 5) | 9u;   /* mov w9,#period */
      const uint32_t st = PATCH_FMOD_PERIOD_STR;                                         /* str w9,[x19,#field] */
      applied += write_words("FMOD OpenSL period", ld, &mv, 1);
      write_words("FMOD OpenSL period store", cz, &st, 1);
    }
  }
#endif

  if (want("debug.time_hook")) {
    uintptr_t a = at(unity, RVA_TM_UPDATE);
    if (guard("TimeManager::Update", a, GUARD_TM_UPDATE)) {
      _Static_assert(TM_BODY_DELTA > 16, "detour would overwrite TimeManager::Update's body");
      g_tm_body = (fn_tm_body)(a + TM_BODY_DELTA);
      hook_arm64(a, (uintptr_t)&cp_tm_update_hook);
      debugPrintf("[patch] %-30s hooked (wall clock)\n", "TimeManager::Update");
      applied++;
    }
  }

  if (want("debug.vsync_feed")) {
    uintptr_t a = at(unity, RVA_SET_FRAME_TIME_NANOS);
    if (guard("SetFrameTimeNanos", a, GUARD_SET_FRAME_TIME_NANOS) &&
        guard("WaitVSync", at(unity, RVA_WAITVSYNC), GUARD_WAITVSYNC)) {
      g_set_frame_time = (fn_set_frame_time)a;
      g_vsync_counter = (volatile int64_t *)at(unity, RVA_VSYNC_COUNTER);
      debugPrintf("[vsync] feeding SetFrameTimeNanos @ libunity+0x%x at 60 Hz (counter @+0x%x)\n",
                  RVA_SET_FRAME_TIME_NANOS, RVA_VSYNC_COUNTER);
      applied++;
    }
  }
  debugPrintf("[patch] %d engine patches applied\n", applied);
  return applied;
}

/* ------------------------------------------------- [Z] libil2cpp zlib level */
/* System.IO.Compression's native side (CreateZStream, Mono's zlib-helper)
 * hard-codes deflateInit2(level -1 = zlib's default 6) and ignores the managed
 * CompressionLevel. The game gzips its replay on the main thread when a match
 * ends: 300-700 ms frozen on hardware, 97 % of it in deflate_slow. Level 1 is
 * 3.7x faster on a real 573 KB replay for 12 % more bytes, and still plain
 * gzip for every reader. [debug] zlib_level = 1..9, 0 = leave it alone. */
int mvlo_install_il2cpp_patches(so_module *il2cpp) {
  int lvl = nxcfg_int("debug.zlib_level", 1);
  if (lvl < 1 || lvl > 9) {
    debugPrintf("[patch] %-30s off in mvlo.ini (zlib default level)\n", "CreateZStream deflate level");
    return 0;
  }
  uintptr_t f = so_try_find_addr_rx(il2cpp, "CreateZStream");
  if (!f) {
    debugPrintf("[patch] %-30s SKIP (no CreateZStream export)\n", "CreateZStream deflate level");
    return 0;
  }
  /* deflateInit2_(z, level, Z_DEFLATED, bits, 8, ...): mov w1,#-1 ; mov w2,#8 */
  for (int i = 0; i < 64; i++) {
    uintptr_t a = f + 4 * (uintptr_t)i;
    if (so_read32(a) == 0x12800001u && so_read32(a + 4) == 0x52800102u) {
      const uint32_t mv = 0x52800001u | ((uint32_t)lvl << 5);   /* mov w1,#lvl */
      return write_words("CreateZStream deflate level", a, &mv, 1);
    }
  }
  debugPrintf("[patch] %-30s SKIP (deflateInit2 call not recognised)\n", "CreateZStream deflate level");
  return 0;
}

/* Started once the engine is mapped and constructed (the thread calls
 * straight into libunity). */
void mvlo_start_clock(void) {
  if (!(g_set_frame_time || g_vsync_counter) || g_clock_run) return;
  g_clock_run = 1;
  if (R_SUCCEEDED(threadCreate(&g_clock_thread, clock_thread, NULL, NULL, 0x4000, 0x2C, -2))) {
    threadStart(&g_clock_thread);
    debugPrintf("[vsync] clock thread started\n");
  } else {
    debugPrintf("[vsync] clock thread FAILED to start -- the engine will wait forever in WaitVSync\n");
  }
}

void cp_shutdown_patches(void) {
  if (!g_clock_run) return;
  g_clock_run = 0;
  threadWaitForExit(&g_clock_thread);
  threadClose(&g_clock_thread);
}
