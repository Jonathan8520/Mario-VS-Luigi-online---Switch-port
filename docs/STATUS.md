# Project status

Legend: ✅ verified · 🟡 implemented, not verified · ❌ blocked / not done

## Deliverables for the morning
| Deliverable | Status |
|---|---|
| `.nro` built by CI, downloadable (workflow **nro**, artifact `mvlo_nx-nro`) | ✅ |
| Game data for the SD card (workflow **android-build**, artifact `mvlo-sd-data-v2.2.0.1`, 94 MB zipped / 244 MB on the SD) | ✅ |
| `docs/TEST.md` (SD, launch, Photon AppId, logs) | ✅ |

## What was actually run
- ✅ The `.nro` compiles without errors (local + CI, image `devkitpro/devkita64`).
- ✅ **Eden emulator** (yuzu lineage, `linuxserver/eden`, lavapipe software
  rendering) in the container: the `.nro` starts, creates `mvlo.ini`, writes
  `debug.log`, initializes BSD sockets (local IP, DNS available).
- ✅ **Smoke test** (`[debug] smoke_test = 1`) with the *real* Unity player
  6000.3.10f1 (libmain + libunity, without the game) under Eden:
  BuildID recognized, **6 engine patches + 21/21 memory granularity sites
  applied**, 484 libunity constructors run, `JNI_OnLoad` →
  62 UnityPlayer natives registered, the 11 driven natives found at the
  expected RVAs, `SetFrameTimeNanos` vsync stream at 60 Hz (30 calls/0.5 s).
- ✅ **smoke_test = 2**: the Unity player enters its lifecycle
  (initJni, RecreateGfxState, SetRunning, 1st frame): Dynamic Heap allocator,
  system info and `ApplicationInfo 'com.ipodtouch0218.NSMBMarioVsLuigi' 2.2.0.1`
  logged, up to the loading of IL2CPP (absent by design →
  Unity error dialog intercepted and logged).
- ✅ **smoke_test = 3**: self-test of the signal emulation with bdwgc's
  exact protocol through the real import table: 5/5
  stop-the-world cycles, 4/4 stack pointers registered, stop ≈ 10 ms.
- ✅ **smoke_test = 4** (in CI): DNS 3/3 (`ns.photonengine.io` resolved),
  UDP round trip (NTP) succeeded, correct Linux TCP errno.
- ✅ **Full game under Eden** (workflow **emu-test**): 506 imports resolved,
  IL2CPP + GC active, NxPort C# bridge connected, controller 1 detected, FMOD
  OpenSL playing (24 kHz stereo), **title screen rendered continuously for
  10 min**, no Unity exception or error, no crash. ~7.7 fps in software
  rendering (lavapipe, runner CPU): this figure says nothing about the Switch.
- ✅ **Input**: scripted presses (`[debug] autopress`) on controller 1
  get past the title screen and then into the online menu.
- ✅ **HTTPS**: the game downloads its news
  (`mariovsluigi.azurewebsites.net`, "Found 3 news post(s)") through the
  port's DNS and sockets.
- ✅ **Photon**: the connection is attempted and fails cleanly with the message
  "No Photon Quantum AppId" (no AppId in the test, as expected). The
  real connection remains to be tested with your AppId.
- ✅ **Real Switch (v0.1.1, fw 22.1.0, handheld)**: title screen, menus,
  controller, sound, network (HTTPS news), **stable 30 fps**, ~28 s of
  loading, no crash. *Play Game* stays on "Connecting…" for lack of a
  Photon AppId in `mvlo.ini` (expected).
- 🟡 v0.1.2: 60 fps by default (the game leaves "Max FPS" unlimited, which
  Unity Android translates to 30). Under Eden the setting applies
  (`targetFrameRate never set by the game -> 60`) and the title screen goes
  from ~7.7 to ~13 fps in software rendering; to be confirmed on console.
- ✅ Real Photon connection (real Switch, Oct 26 evening): authentication
  (anonymous with a personal AppId, or the official auth `use_official_auth = 1`
  which also works: 200 response + token), region `eu`, room creation,
  Quantum Runner, AddPlayer, **match played for more than 5 min with the GC
  active** (v0.1.5), clean shutdown.

## On console, Oct 26 evening (v0.1.3 → v0.1.7)
Fixed and verified on the real Switch:
- ✅ **Freeze after "Creating a game"** (frozen screen, music keeps playing):
  GC stop-the-world stuck. Photon's network thread loops on
  `poll()` with short timeouts and `poll_fake` exited through the timeout
  check **before** its safe point: the signal was never delivered. Safe
  point now at the entry of `poll/recvfrom/sendto/accept4`. Confirmed: with
  `gc = disable` the match ran (then the console froze for lack of
  memory); with the fix it runs with the GC active.
- ✅ **Crash `[oc] svcMapMemory FAIL rc=0xd401`**: the kernel places the
  threads' TLS pages (type 0xc, 8 threads/page, page unmapped when it
  empties) at random in the stack region, hence in the overcommit window,
  in space reserved by Unity but not yet committed. At startup,
  64 helper threads force the creation of ~9 pages and one thread per page
  stays alive (`[tls] … pinned`); later threads reuse those slots
  (`debug.tls_anchor`, 64). Verified: no 0xc page appears any more after
  arming. (The 1st version used priority 0x3F, reserved for core 3:
  no thread created.)
- ✅ `stall.log` now shows the emulated-signal state of each thread
  (`[sig] tid … pending … blocked …`): a stuck stop-the-world names the
  offending thread.

- ✅ **PlayerPrefs, strings and integers** (v0.1.8, verified on console:
  key bindings kept after relaunch, `prefs.kv` with 53 entries including the
  nickname, `Controls_Bindings`, `Graphics_MaxFPS`, `id`/`token` from the
  official auth). Nothing was kept before, not even within the session:
  Unity 6000.3 loads its Java classes through `Class.forName`, for which the
  fake JNI returns `java/lang/Object`, so neither the handlers in
  `unity_jni.c` nor `Uri.encode/decode` nor `String(byte[], String)` (key
  encoding) were recognized. Dispatch by **receiver** (unity_jni handles)
  and by **signature** (DECISIONS 28).
- ❌ **PlayerPrefs, floats** (HUD Scale, volumes: revert to 0):
  `getFloat` is not routed to `unity_jni.c` (`dispatch_float` in
  `jni_fake.c` only knows boxes) and `putFloat` receives **0** even though
  the key arrives correctly (`[prefs] putFloat 'Audio_MasterVolume'=0` while the
  sound is playing). Reproducible under Eden.
- ⚠ Common root cause: `Class.forName` always returns `java/lang/Object`.
  Fixing it globally would solve all of this at once, but it changes
  the dispatch of many calls that work today "by accident":
  to be done carefully, with a full emulator test.

Fixed, to be verified:
- 🟡 **X/Y swapped on display** in the `nintendo` layout (A/B were
  mapped by label, X/Y by position): X → West ("x"), Y → North ("y")
  in `NxInput.cs`. Requires a Unity rebuild.
- 🟡 Option `[photon] build_identifier` (suffix of the Photon AppVersion;
  the public sources give `vanilla`, the official builds `official`).
  Empty by default. Requires a Unity rebuild.

Remaining (seen on console):
- ❌ **No keyboard**: impossible to change the nickname or to type in the
  chat (the Switch software keyboard, swkbd, has to be hooked up to Unity's
  `TouchScreenKeyboard` / `showSoftInput`).
- ❌ **Fixed black screen for ~25 s at startup** (frame 1 ≈ 23 s): no
  loading screen.
- ❌ **Long stalls when a match starts** (6 to 9 s, then 0.3-1 s): not the
  GC (no pending signal); FMOD reads its banks from the SD in small
  chunks (`fsdev_read <- read_fake`) while UnityMain waits.
- ❌ `ArgumentOutOfRangeException` in the `UI/Navigate` callbacks with the
  controller (no visible effect).
- ❌ In 720p handheld, the "NoItem" circle (bottom right) and the player list
  (top left) are too small; *HUD Scale* at maximum suits
  the user (a larger default value on Switch would be good, and the
  PlayerPrefs floats first need to be kept).
- Free physical memory at startup: 3 MB (newlib heap 2 480 MB); without GC
  the console ends up frozen (seen with `gc = disable`).

## Night of Oct 26-27 (v0.1.9) — to be verified on console
Verified under Eden (CI, two successive boots on the same SD):
- ✅ **Float settings kept**: first launch `F Graphics_HudScale 8`,
  volumes 0.75 / 0.5 / 0.75, `Graphics_MaxFPS 60`; relaunch:
  `getFloat 'Graphics_HudScale' -> 8`, etc. Actual cause: empty resolution
  list → the game's default values were never set (DECISIONS 31).
  The tiny HUD came from there (game default = 8 = slider maximum).
- ✅ **Loading screen** during frames 0-1: `[load] loading
  screen: 217 frames over 18601 ms`, visible in the screenshots from 9 to 25 s
  (title, wheel of squares, bar).
- 🟡 **Picture loading screen** (v0.1.10): Infenek logo, 5 screenshots
  cross-fading with a slow zoom, game logo, rounded bar. Rendering verified
  on PC (`tools/splash_preview`) and under Eden (`5/5 picture(s) shown`, menus
  reached afterwards, loading time unchanged); images downloaded by the
  CI, never committed. Under Eden the first shader (with `return`
  statements) crashed the emulator: replaced by a branchless shader.
  To be checked on console.
- ✅ **SD read cache** active (`[io] block cache ON`), per-file statistics
  in the log; no slow reads under Eden (the emulator's SD
  is an SSD: the real gain will be measured on console).
- 🟡 **Switch keyboard**: hooked up (`showSoftInput` → swkbd), but not
  triggered by the CI's scripted presses: to be tried on console
  (nickname, chat). It only opens right after a press or a touch, so
  that the game's keyboard (which requests the keyboard again every frame)
  does not reopen it in a loop.
- 🟡 `Class.forName` returns the real class, `Call*MethodA` receive their
  arguments: no regression seen under Eden (menus, network, audio
  initialized), to be confirmed on console.
- ✅ No speed regression: the gap seen under Eden (163 ms then 90 ms
  per frame) came from the GitHub machines; A/B test done (SD cache alone
  turned off: 120 ms; everything on: 70 ms, the fastest). The log now
  shows a `[perf]` line every 2 s (time spent in `nativeRender`, in
  display, JNI calls, reads and waits per frame): on console,
  it will tell where the 30 fps cap comes from.
- 🟡 `ArgumentOutOfRangeException` in `UI/Navigate`: probable cause
  found in the sources — the *Resolution* list in the options was empty
  (same cause as the settings at 0, see DECISIONS 31) and left/right
  on it threw the exception. Fixed with `getSupportedModes`; if it
  comes back, I need the exact lines from `debug.log` and the screen where
  it happened.

## On console, Oct 27-28 (v0.1.10 → v0.1.12)
Verified on the real Switch with v0.1.10 (android-build data 255db44):
- ✅ **Picture loading screen**: `5/5 picture(s) shown`, ~25 s.
  The first ~4 seconds stay black: the screen waits for Unity's GL context
  (`[load] loading screen on` at 4.3 s).
- ✅ **Switch keyboard**: nickname changed, chat message sent
  (`[kbd] confirmed`).
- ✅ **~58 fps** in a match (`target_fps` applied), float settings kept.
- ❌ `NullReferenceException` in the `canceled` callbacks of `UI/Navigate`
  at the moment of `Game started!` (2nd match).

**Crash no. 1 — out of memory when loading a level (fixed in
v0.1.12).** The `[mem]` line (v0.1.11, every 2 s: newlib heap used /
free in the arena / never touched, overcommit committed / pool) showed:
```
65.344 [mem] newlib: in use 2275 MB, free in arena 34 MB, never touched 170 MB | oc: committed 203 MB, pool 203/896 MB
77.565 [gpua] memalign FAILED size=131068 KB (arena live=142 MB peak=143 MB)
77.878 Unity: Could not allocate memory: System out of memory! ... MemoryLabel: FMODSample
```
Two pools taken from the heap at startup stayed largely
empty: the overcommit commit pool (896 MB, 203 MB used) and
the GPU arena (512 MB, peak of 142 to 212 MB). v0.1.12: `[debug] oc_pool_mb`
(640) and `gpu_arena_mb` (320). On the next match: `in use 1962 MB, free
in arena 77 MB, never touched 440 MB | oc 241/640`, GPU arena 212/320. No
more out-of-memory on the game side. Note: Unity now reserves ~2.09 GB
("reserved: 2087566672B"), more than the overcommit window (1.28 to
1.66 GB depending on the layout); the excess takes real memory.

**Crash no. 2 — full console freeze, NOT solved.** v0.1.12: one
full match (74 s → 301 s), 2nd match started at 312 s, freeze at 442.4 s
right after `[gfx] swap 20700 ok`. The watchdog stops at the same moment
(last `alive` at frame 20653), HOME and volume no longer respond, only the
sound continues, no Atmosphère report. The newlib heap had ~520 MB free:
it is not the game running out of memory. Already seen twice at the start
of a 2nd
match (v0.1.10). Unverified leads: system GPU service (libnx gives
**8 MB** of transfer memory to nvservices by default,
`__nx_nv_transfermem_size = 0x800000`, never redefined by the port), physical
memory of the application pool (3 MB free at startup: everything is taken by
the heap), EGL context of the loading screen, 60 fps, SD cache.

**Stutters — NOT solved, unbearable for the user.** When a match
starts: `SLOW nativeRender: 6445 ms`, then `7637 ms`; then 300 to
800 ms the first time each object appears (first mushroom, first
block hit…). During the 6.4 s stall, the SD only read ~8 MB in ~0.6 s
(`[io] cache: … SD 46 MB in 2202 ms total`): it is **computation**, not
reads. Typical profile of shader compilation on first
use (mesa/nouveau on the A57) or of texture uploads. To be measured
then addressed (GL program cache `GL_OES_get_program_binary` / mesa disk
cache, shader prewarming by Unity, etc.).

## Night of Oct 27-28 (v0.1.13) — to be verified on console
- 🟡 **Console freeze**: not reproducible under Eden (it emulates neither
  nvservices nor nouveau's code segment). Two plausible causes
  neutralized (DECISIONS 39): nouveau's shader code segment raised
  to 16 MB on the first overflow instead of 4 successive evictions/reallocations
  (`[video] shader_code_mb`), nvservices transfer memory
  raised to 32 MB (`[debug] nv_tmem_mb`, automatic 8 MB fallback). And the
  means to see it coming: `[gpu]`/`[nv]` lines (GPU objects, channels,
  failures, fence waits with the channel error), GPU state in `[wd] alive`.
- 🟡 **Stutters**: now measured (`[hitch]` per slow frame). Under Eden,
  in the menu: GLSL compilation ≈ 50 %, linking ≈ 30 %, nouveau code
  generation ≈ 20 % of the compile time. devkitPro's mesa is
  built **without a shader cache** on Switch (checked in its sources):
  neither disk cache nor `glProgramBinary`. Done: CPU at 1785 MHz during
  compilations (`[video] compile_boost`), adjustable nouveau optimization
  level. Proposed: rebuild mesa with a cache (DECISIONS 40).
- 🟡 **Black screen during the first 4 seconds**: the 1st attempt (logo on
  the libnx framebuffer) blocked Unity's 1st frame under Eden, abandoned.
  Done instead (DECISIONS 41): we create the single EGL surface early, with
  the config recorded at the previous boot (`.eglconfig`), and give it to
  Unity. Under Eden on relaunch: loading screen at 0.49 s, handed over to
  Unity at 2.7 s, normal match afterwards. The 1st boot after installation
  stays black. To be validated on console (`[video] early_splash = 0` to
  turn it off).
- 🟡 **NullReferenceException in `UI/Navigate`**: cause found (the game's
  keyboard left open after the Switch keyboard, subscribed to Navigate after
  its destruction); fixed in the game (`prepare_project.py`) and in `NxInput`
  (game keyboard closed after swkbd). Requires the new SD data.
- ✅ Under Eden: boot, menus, settings kept, no nv error
  (`148 maps live, 1 channel, 0 failed`), 32 MB transfer memory accepted.

## On console, Oct 28 evening (v0.1.13 → v0.1.19)
Verified on the real Switch with v0.1.13: loading screen from ~0.5 s
(from the 2nd launch on), Switch keyboard, X/Y, float settings kept (HUD,
volumes), language list.

**Stutters: it wasn't the shaders, it was `stat()` (largely fixed,
v0.1.17).** The v0.1.13 `[hitch]` only saw ~0.1 s of
GL in a 24 s frame (`other 24064 ms`); the watchdog's stack (CI
symbols) showed UnityMain in a file-service IPC under
`File::Open`/`IsDir`. Measured in v0.1.14: *Room Settings* = **1728 `stat()`
in 14.8 s**, match start ~650 in ~6 s, boot 3077. libnx's
`fsdev_stat` makes 5 FS round trips per file (GetEntryType, OpenFile,
GetSize, Close, GetFileTimeStampRaw), each one a lookup in `Data/`
(1570 files): ~8.6 ms. `nx_statcache.c` lists each directory of
`assets/` and `il2cpp/` once and answers from memory (including "absent",
4207 out of 6947 in one session); `/`, `/switch`, `/switch/mvlo` answer
"directory".
Console results: *Room Settings* 20 s → **3-5 s**, boot ~25 s →
**8 s** (without loading screen). Remaining: ~260 opens per menu (~4.9 ms
each: `fsdev_open` = OpenFile + GetFileTimeStampRaw), with Unity reopening
the same files over and over (one file opened 310 times). `[hitch]` now
separates `file open` / `stat` / `fstat` / `access`.

**Hang no. 1 — the game freezes, the console lives on (cause found, NOT
fixed).** v0.1.19, at the start of a match: UDP heartbeat alive, `frame`
frozen, network calls stopped, HOME works. `stall.log`:
`[sig] tid 657 main pending 0x20000000 blocked 0x0` = the GC stop-the-world
is waiting for UnityMain, which goes through no safe point. Stack (CI symbols):
`SessionRunner.Service → DeterministicSession.UpdateSimulation →
QuantumGame.OnSimulate → TaskContext.EndFrame +0x64c`, a managed-code wait
loop (interface call on every iteration, IL2CPP method lookup
`ldur/cmp/b.ne`, no system call) that waits for the Quantum task workers…
parked by that same GC. Deadlock. Under Linux the signal
would interrupt the loop. Expected fix: let the signal emulation
forcibly stop a thread that does not reach a safe point (kernel suspension
+ context), or any other safe way to deliver the signal to a thread
running managed code.

**Hang no. 2 — full console freeze (unknown cause).** HOME and
volume dead, the music loops. The UDP heartbeat (`[debug]
netlog`, v0.1.18) **stops at the same moment**, even though its thread only
sleeps, formats and sends (priority 0x28, core 2, neither SD nor GPU):
it is the whole console that stops. Last beat always healthy
(`idle` 5-30 ms, `gpu busy=0 failed=0`, `fs: idle, 16 open`). Happens after
~7 to 13 min of play, in the 1st as well as the 3rd match, also with
`gc = disable` (Oct 26). **Ruled out by measurement**: game out of memory
(`[mem]` stable, >500 MB free), file operations in progress, 60 fps, loading
screen, `shader_code_mb`, `nv_tmem_mb` (A/B test Oct 28: all off + 30 fps →
freeze at ~9 min). Open leads: deadlock in a system service
(network, audio…), emuMMC on the same SD, thread priorities / cores,
CPU boost, other. v0.1.19 adds to the heartbeat the SoC temperature
(~37 °C), the process's memory blocks (~900), the system resource (0: the
process uses the applications' shared one) and the network call in progress.

Tools: `tools/switch/` (FTP with SHA-256 verification and resume, MTP,
artifact diff, `resolve_stall.py` to name the addresses in dumps
with the `mvlo-symbols` artifact, heartbeat listener
`netlog_listen.py`/`.php`).

## Nov 12 — v0.1.47: applet mode stopped at launch

- ✅ Launched from the Album (applet mode, a few hundred MB), the game used
  to start anyway and die later, out of memory, during the load. It now
  checks the applet type first thing and shows a screen in the system font
  over the game's artwork: "hold R while starting any game", why, and
  A/B Exit, which returns to hbmenu (DECISIONS 84). Verified on the
  console: applet mode shows the screen, A and B both return to hbmenu;
  title mode boots normally to the title screen.

## Nov 11 — v0.1.46: Switch defaults for a new player

- ✅ A player who has never saved a setting starts with **Max FPS 30**
  (the game's "unlimited" gives an uneven 40-50 fps on the console),
  **HUD Scale 4** (the game's 8 is large on the handheld screen) and
  **Discord integration off** (no Discord on a Switch). An existing
  player's saved settings are not touched; the options menu changes these
  as usual (DECISIONS 83). Key bindings stay the game's (A/B jump, X/Y run,
  R/ZR power-up, L/ZL reserve item).
- ✅ Verified: emu-test on a blank SD seeds the three keys on the first
  boot, the game saves them (`Graphics_MaxFPS 30`, `Graphics_HudScale 4`,
  `General_DiscordIntegration 0` in prefs.kv) and the second boot reads
  them back without seeding again. On the console, with existing settings:
  nothing seeded, settings unchanged, clean quit.

## Nov 11 — public repository, squashed history

- ✅ History rewritten into a few commits on `main` (author Jonathan8520);
  the old work branch is deleted. The commit hashes cited below
  (before Nov 11) refer to the old history, kept outside the repository.
- ✅ Repository made public (free Actions minutes). The game can never be
  downloaded from it: android-build's `mvlo-sd-data-*` artifact is an
  AES-256 zip (secret `SD_ARCHIVE_PASSWORD`), see `docs/TEST.md`.
- Credits: LICENSE and the author shown in hbmenu = Jonathan8520 (the game
  stays credited to ipodtouch0218).

## On console, Nov 9 evening (v0.1.45) — room chat by touch

- ✅ **Chat**: after a match, impossible to reply "gg" (reported by the
  player). Three defects (DECISIONS 81): OK on the Switch keyboard filled
  the box without sending; touching "Send" sent and then reopened the
  keyboard (the game refocuses the box); a "pending" keyboard opened
  on the next touch anywhere, or left the box dead after a
  minute. Verified in a private room, before and after a match: OK sends,
  "Send" no longer opens anything, touching the box opens the keyboard.
- 🟡 The chat box remains unreachable with the controller: the game's scene
  links it to nothing (`BtnSendMessage` has no left neighbor), on PC too.
  Possible fix on the Unity side (`prepare_project.py`), with a new
  copy of the data.

## On console, Nov 9 (v0.1.44) — no more threads after ~1 h online

- ✅ **v0.1.43 online (Nov 8)**: two sessions of 74 min (14 matches) and
  44 min (10 matches), 30 fps, no game crash. Replay cache:
  22 out of 25 from memory, 2-23 ms. The only crash report of the
  day is from the HB App Store app, not from the game.
- ✅ **"Thread creation failed" after 74 min**: when reconnecting to Photon,
  the game could not create its connection thread and stayed offline.
  The process has 96 threads; the engine's detached threads were
  never closed (libnx has no `pthread_detach`) and kept their slot
  and their 2 MB stack. Fixed in v0.1.44 (DECISIONS 80).
- ✅ Verified on console (v0.1.44, 4 reconnections, a private room, a
  match, back to the room): 58 threads created, 17 closed by the reaper, 7
  joined, 67/96 in use and stable (v0.1.43 would be at 84); clean
  exit.

## On console, Nov 8 (v0.1.43) — launch crash fixed

- ✅ **v0.1.42 crashed once at launch** (Nov 8, before "Play",
  Atmosphère report `01794139211_…`). It was not the game: the watchdog
  (`diag.c`) was describing all threads because frame 1 of loading
  lasts 3.6-4.4 s (3 s threshold, report on 82 launches out of 84, ~8 s
  of writes to the card), and it read the stack of a short thread that had
  just ended (stack unmapped). Fixed in v0.1.43 (DECISIONS 79): threshold
  of 20 s during frames 0-1, stack read while the thread is
  paused, and a crash of the watchdog now stops only the watchdog.
- ✅ Verified on console: a test build that triggers the report and then
  a fault in the watchdog keeps going to the menu and exits cleanly;
  v0.1.43 boots without a watchdog report, main menu, clean exit.

## On console, Nov 5 (v0.1.40) — validated online, exit without error

- ✅ **52 min online** (Nov 4→5, Jungle included): no crash, stable 30 fps
  in matches (the 60 fps setting gives 40-48 fps in matches: CPU
  limit, the player stays at 30). Only drops: match start/end (~0.4 s)
  and a ~0.2 s hitch in the middle of a match. OC pool at its highest
  253/448 MB, GPU arena 177 MB, 753 MB of the heap never touched.
- ✅ **"Quit" showed "The software was closed because an error
  occurred"**: three causes, all fixed (DECISIONS 77). Verified
  on console, launched from the game icon and from hbmenu: back to the HOME
  Menu without a message, no crash report, complete `debug.log`.
- ✅ **v0.1.41 — match end**: the "mid-match" hitch at 01:13:45
  was the replay save. Writing, closing, committing and deleting
  the old replay move to a thread; the `stat`s of the replays folder are
  kept in memory (DECISIONS 78). Back to the room: 261 →
  177 ms the first time, 164 → 92 ms afterwards. Replays played back without
  error in the "Replays" menu.
- ✅ **v0.1.41 validated online** (Nov 5 16:41-17:12, 31 min, "it was
  great"): 8 match ends, replays of 11 to 196 KB written off the game thread
  in 37-71 ms; end-of-match freeze 125-202 ms; clean exit (`exit rc=0`).
  The player notices neither the end nor the start of a match (315-443 ms,
  every match, during level loading).
- ✅ **v0.1.42 — replay `stat` cache repaired**: v0.1.41 only served
  about half of them (28-36 ms remained at each match end).
  4-way sets, directories kept (DECISIONS 78). In a private room: 22
  out of 25 served from memory from the 2nd match end on, freeze 80-84
  ms. `stall.log` and `prof.log` (3.8 and 4.6 MB) roll over to `*.prev.log`
  beyond 1 MB. `unity/prepare_project.py` no longer contains the official
  AppId (only its SHA-256).
- 🟡 Remaining at match end: the 25 `stat`s of the first save of a
  session (~50 ms), the compression and the game's code.

## On console, Nov 4 evening (v0.1.40) — in-match stutters

Analysis of the `prof.log` from the 27 min session of Nov 3 (6 matches), then
tests in a private room, console driven by sys-botbase, `.nro` sent through
hbmenu's NetLoader (`nxlink`).

| Moment | v0.1.38 (Nov 3) | v0.1.40 (Nov 4) | Measured cause |
|---|---|---|---|
| During play | 150-375 ms, ~once every 1-2 min | **no frame ≥ 100 ms in 6.6 min** (59.8 fps) | our `debug.log`: `fflush` → `fsFileWrite` on UnityMain, the SD card sometimes takes 150-375 ms |
| Match end (replay) | 300-700 ms (550-660 ms for 176-255 KB) | **354 ms** for 210 KB | gzip level 6 compression on UnityMain (97 % in `deflate_slow`), then SD writes |
| Match start | 0.4-0.6 s | unchanged | `FindObjectsByType` + the game's sorting, first shaders/textures |

- ✅ **Asynchronous log**: `debugPrintf` writes to an in-memory queue, and a
  "log" thread writes it to the card (DECISIONS 74). The periodic
  `[loop]`/`[gpua]`/`[gfx]` lines from rendering forced an SD write on
  the game thread.
- ✅ **`[debug] zlib_level = 1`**: IL2CPP's gzip level is hard-coded
  (6); patched to 1 at load time (DECISIONS 73). On a real replay:
  3.7× faster, 12 % larger.
- ✅ **Default memory** `oc_pool_mb = 448`, `gpu_arena_mb = 256` (previously
  640/320, only in `mvlo.ini` since Nov 3); the OC pool
  **grows** instead of letting Unity write to an unallocated page
  (DECISIONS 72). Verified with a 128 MB pool: 128 → 160 → 192 →
  224 MB, no crash.
- 🟡 Remaining: match end (41 % SD writes, 16 % compression),
  match start, room creation (0.7 + 0.9 s, unimportant according to
  the player). Docked mode (1080p) never tested; the GPU pool rises to
  219 MB out of 256 in handheld.

## On console, Nov 3 → Nov 4 night (v0.1.38 + Unity data 61a2f02 → 44ab3d5)

- ✅ **Joins/leaves**: `Regex.CacheSize = 128` (DECISIONS 70), 26
  joins/leaves in 27 min, no stutter.
- ✅ **"Jungle" crash online** (2 times): `[gpua] memalign FAILED
  size=131068 KB`, newlib short of contiguous memory (DECISIONS 71).
  Worked around with `oc_pool_mb = 448` / `gpu_arena_mb = 256` in `mvlo.ini`:
  ~630 MB never touched instead of 136-168, 5 matches without failure.
- ❌ v0.1.39 (allocate only 64 MB of these 128 MB reservations): crash
  at boot, withdrawn (DECISIONS 71).
- ❌ IL2CPP prewarming on the menu (e4c6472, 44ab3d5): no effect on
  room creation, 0.74 + 0.90 s (DECISIONS 75).
- ✅ *Room Settings* ~0.35 s, match launch ~0.55 s. At 60 fps a match
  frame costs 20-23 ms of CPU: irregular 40-50 fps; 30 recommended.

## On console, Nov 2 evening (v0.1.31 → v0.1.37) — first-time stutters

Measured with a sampling profiler (`[debug] prof_ms`, `nx_prof.c`,
`tools/switch/prof_report.py`) in a private room, console driven by
sys-botbase; the match played with the controller.

| Moment | v0.1.30 | v0.1.37 | Measured cause |
|---|---|---|---|
| 1st *Room Settings* | 2.1 s | **1.1 s** | 78 % waiting on the fs service: timestamps on every open (27 %), opens (23 %), reads (21 %) |
| Launch of the 1st match | ~1.9 s | **~1.15 s** (3 shorter frames) | same opens/reads, on UnityMain and the loaders |
| In a match (objects, etc.) | 190-310 ms, 3 times in 5 min | **none ≥ 150 ms in 21 min** | UnityMain blocked in Photon's UDP send (slow `bsdSend`) |
| Room creation | 0.7 + 1.0 s | unchanged | IL2CPP initializes its generics on first use (CPU, no files) |

- ✅ `[io] open_timestamps = 0`: `fsFsGetFileTimeStampRaw` (libnx requests it
  after each `open`) is no longer sent for the game data.
- ✅ `[io] preload_kb = 1024`: from the main menu on, the 1 540 files of
  `Data/` ≤ 1 MB (38.7 MB, ~7 s in the background) are kept **in memory**
  (`nx_io.c`, "RAM files"); their reads no longer touch the SD.
- ✅ `[net] async_send = 1`: UDP datagrams go out through a queue and a
  send thread; the game no longer waits for the network service. 21 min of
  match, no disconnection.
- ✅ The online freeze remains fixed: this 21 min match went far past 400 s.
- ✅ **v0.1.38, `[io] phantom_fds = 1`**: a preloaded file opens without
  any fs request (descriptor 3072-4095 served from memory). *Room
  Settings* **1.1 s → 0.49 s** (202 opens = 29 ms), match launch
  ~1.15 s → ~0.9 s.
- ✅ Played in public in the official group (`[photon] build_identifier =
  official`, 6-player match): it runs. Players joining/leaving in the middle
  of a match: stutters of 127-245 ms (see DECISIONS 69).
- 🟡 Remaining: room creation (0.7 + 1.0 s, IL2CPP); joins/leaves.

## On console, Nov 2 (v0.1.27 → v0.1.30) — online freeze FIXED

Console driven from the PC by **sys-botbase** (`tools/switch/sbb.py`:
buttons, screenshots), tests monitored by `tools/switch/freeze_watch.sh`
(heartbeat, autopsy, sys-botbase response). fw 22.1.0, AMS 1.11.1.

| Test | Version, setting | Result |
|---|---|---|
| 1. `udp_soak` → PC on the local network (raw libnx, 20 packets/s), menu | 0.1.27 | no freeze in 24 min |
| 2. "Play" + `udp_unconnected = 1` | 0.1.27 | **freeze** at ~400 s cumulative (US 51 s then EU) |
| 3. "Play" + `chiaki_cfg = 1` (16 bsd sessions accepted by the console) | 0.1.27 | **freeze** at ~400 s |
| 4. `udp_soak` through our socket layer → DNS 1.1.1.1 (Internet), 5/s, menu | 0.1.28 | no freeze in 10 min |
| 5. "Play", IPC request counter per service | 0.1.29 | **freeze** at ~400 s: cause found |
| 6. "Play", `/dev/urandom` served by the CSPRNG | **0.1.30** | ✅ **no freeze in 11 min 40** |

- ✅ **Cause**: `/dev/urandom` was emulated by a **64 KB file** on the
  SD (`.synth_dev_random`, `open_fake`), read through the fs service. IL2CPP
  keeps the fd open; Photon draws ~160 bytes/s from it once connected; around
  **~400 s** the file reaches its end and `il2cpp::os::Cryptography::FillBufferWithRandomBytes`
  loops on `read() == 0`: **~7 000 fs reads/s** (autopsy: 129 520
  fs requests versus ~23 000 expected). fs (core 3) saturates and **starves
  the other services on core 3**: networking for **the whole console**
  (sys-botbase refuses connections, ping fails), nifm; `UnityMain` stays
  blocked in an fs write → frozen frame. Everything resumes when our process
  dies. This explains everything: count accumulated per process (the fd stays
  open), nothing while offline (the menu draws no randomness), reset at
  every launch, the soak that never freezes.
- ✅ **Fix (v0.1.30)**: `read_fake` serves the fds of `/dev/urandom` and
  `/dev/random` with `randomGet()` (endless, no SD access). In "Play",
  fs requests drop from 46/s to 7.5/s (the menu's level).
- ❌ The three fixes of v0.1.27 (`udp_unconnected`, `chiaki_cfg`,
  `photon_rebind_s` not tried) changed nothing: left in, off by default.
- 🟡 Diagnostics added (v0.1.28-29): `[debug] udp_soak_layer/_dns/_hz/_reuse`,
  logging of `setsockopt` (Photon only sets `SO_REUSEADDR`), **IPC request
  counter per session** (`ipc/10s:` in the heartbeat, totals in
  the autopsy). `udp_echo.php` finally shows its throughput under continuous
  traffic.
- 📋 To be confirmed: a real match of more than 7 min.

## On console, Oct 29, late (v0.1.23 → v0.1.25) — the freeze narrowed down

Seven tests on console (fw 22.1.0, room created alone, region eu). Freeze =
last frame; "connection" = `[Network] Successfully connected to
region` (entering "Play", not the main menu, which does not
connect).

| Test | Version, setting | Connected until the freeze | bsd requests at the freeze |
|---|---|---|---|
| Reference (30-star match) | 0.1.23, default | 394 s | 162 504 |
| B | 0.1.23, `socket_config = 1` | ~394 s (frame 11 798 versus 11 759) | 162 849 |
| Flood on the menu, without Photon | 0.1.24, `bsd_flood = 2000` (0 ms polls) | no freeze (4 min, 400 000 requests) | — |
| "Play" screen alone, no room | 0.1.24 | 396 s | **85 013** |
| Connection 6 s then main menu 8 min | 0.1.24 | no freeze | — |
| Same launch: 109 s connected, B 5 s, reconnected | 0.1.24 | freeze 285 s after the reconnection = **400 s cumulative** (399 s since the 2nd connection) | — |
| `local_wait = 1`, "Play" screen | 0.1.25 | 409 s | 442 000 (0 ms polls) |

Established:
- **The freeze comes after ~391-409 s of open Photon connection**, with or
  without room, match, Quantum or replays (earlier sessions also froze
  with several short matches whose replays are saved).
  A 5 s cut does not reset the count; a 6 s connection
  followed by 8 min disconnected triggers nothing.
- **Not the number of requests** (85 000 at the freeze in "Play", 162 000 in
  a match, 400 000 without a freeze in flood), **not the wait time in bsd**
  (`local_wait`: no more waiting in the service, freeze anyway),
  **not the mapped requests** (bsd *pointer buffer* **0x1000** on console,
  17 mapped requests out of 162 000, none near the freeze), **not the
  buffers** (B), **no kernel object leak** (`rl:` flat until the freeze:
  threads 65/96, tmem 3/32, sessions 1/1). No `FIONREAD refused`.
- At the freeze, **bsd stops first**: the Photon receive thread stays
  13.2 s in a **0 ms** poll (with `local_wait`), the heartbeat 13.2 s
  in `sendto`, UnityMain 12.4 s in `send`; then **fs** (watchdog 12.3
  s, an IL2CPP worker 10 s, UnityMain 13.7 s in the `debug.log` write
  in another test) and **nifm**; `am`, `time`, `ts`, `nvdrv` respond
  in 0-1 ms. Once ("Play" screen), the services came back after
  ~12 s, the game rendered one frame and then blocked again.
- The last 128 bsd operations before the freeze are normal (small
  Photon UDP packets, network almost silent in "Play"). Once, 7
  Photon packets of 24 bytes arrived together ~2 s before.

Open lead (not proven): **a system service spins in a loop** because of
our connected UDP socket after ~400 s (bsd itself?) and starves of CPU
the lower-priority services of the system core (bsd, fs, nifm,
then hid); our exit closes the socket and everything resumes. Or an event
from the Photon server around ~400 s that the console's network stack
handles badly.

Added: `[debug] bsd_flood` / `bsd_flood_op` (DECISIONS 56),
`[net] local_wait` (DECISIONS 57), version 0.1.25 (local build).

## Night of Oct 30 (v0.1.27, cloud session) — candidate fixes

Continuation of the research (DECISIONS 61, sources) and **three fixes to
test** tomorrow, all behind a `[net]` switch, default off; clean build,
Eden self-test OK (chiaki config applied, 16 sessions accepted under Eden,
`udp_unconnected` active, no crash; defaults off = zero change).

- 🔎 **sys-ftpd #30**: the ENOBUFS ("hangs the system") comes from **active
  transfers** with buffers that are too small, not from an idle socket. The
  sources describe **no** mechanism by which **a single** drained connected
  UDP socket rots with time alone (bounded receive then drop; ICMP = a single
  `so_error`; poll retains nothing; fixed tmem). An ENOBUFS freeze at ~400 s
  would rather come from a **socket/fd leak** or from **system RAM
  starvation**.
- 🔎 **chiaki-ng** (real-time UDP analogue): udp 256 KB, `sb_efficiency 8`,
  **16 sessions** (libnx max; the console has 15), without nifm.
- 🟡 **Delivered (v0.1.27)**:
  - `[net] chiaki_cfg`: chiaki socket config (large UDP buffers, 16 sessions,
    fallback if refused);
  - `[net] udp_unconnected`: keep the UDP socket **unconnected** at the bsd
    level (filtered sendto/recvfrom) — the most likely fix if it is the
    connected socket that rots;
  - `[net] photon_rebind_s = N`: fresh socket under the same fd at N s (new
    source port; **risk** of Photon disconnection);
  - **bsd errno per command** (`err: ENOBUFS=…` in the heartbeat, table in
    the autopsy) and **`N sock open`** in the heartbeat (fd leak = a count
    that climbs).
- 📋 Console tests: TEST.md "4 octies" (measurements 1-2 then fixes a-c,
  each ≤ 8 min, "Play" only). The "applet mode" test is withdrawn
  (launch always in title takeover).

## Night of Oct 30 (v0.1.26, cloud session) — research + instrumentation

In-depth research on the ~400 s freeze (4 sub-agents, sources in DECISIONS 58:
libnx, Mesosphère, the game's Photon sources, switchbrew, issues). Results:

- 🔎 **Photon has no event at ~400 s**: all its timers are ≤ 60 s
  (service 10 ms, ping 1 s, LobbyStats 60 s, DisconnectTimeout 10 s) or = 1 h
  (token). Quantum 3.0.13 / Realtime 5.1.15. The trigger is therefore **not**
  a Photon application event → it is very likely the **console's network
  stack** facing our long-lived connected UDP socket.
- 🔎 **Why bsd first, then nifm**: the `bsdsocket` process hosts bsd
  **and DNS (sfdnsres)**; nifm (separate) runs its connection test through
  bsdsocket (libcurl+socket/ssl). bsd goes down → nifm blocks. **fs**
  (separate, core 3, no network), which freezes too, is not explained by the
  network.
- 🔎 **Strictly priority-based scheduler per core, with no time sharing
  between priorities**: starvation is possible, **but would be immediate,
  not at 400 s**; no kernel object/session/thread leak found for a single
  socket. FS is on core 3, we are on 0-2. **No simple mechanism alone
  explains the 400 s delay** (honest conclusion, DECISIONS 58f).
- 🔎 **nifm downgraded**: `socketInitializeDefault` does not call nifm and
  almost no homebrew submits a nifm request (the norm); the nifm fix is
  therefore not coded. Closest analogue: chiaki-ng (16 bsd sessions, large
  UDP buffers). Since 18.0.0, **15 bsd:u sessions for the whole console**.
  RetroArch Switch freezes "after 10-15 min" in **applet mode**, not in
  application mode → free test added.
- 🟡 **Delivered (v0.1.26, defaults off, clean build, Eden self-test OK)**:
  - `[debug] photon_ring`: ring of the last 30 s of Photon datagrams
    (direction, size, first 16 bytes) in the autopsy and `debug.log`;
  - `[debug] udp_soak = ip:port` (+ `udp_soak_connect`): mimics Photon
    traffic **without Photon** towards a PC echo — the decisive
    network stack / Photon experiment;
  - `tools/switch/udp_echo.php`: the PC-side UDP echo (like netlog, in PHP).
- 📋 **Spec** (not coded, safety filter, for the local session): measure at
  the freeze whether bsd/fs/nifm are *runnable but starved* or *blocked in
  IPC* (svcDebugActiveProcess via pmdmnt), and candidate fixes (poll_yield,
  socket renewal) — DECISIONS 59.
- 📋 **Console tests** in TEST.md "4 octies": 1) udp_soak on the menu,
  2) applet vs application, 3) photon_ring at the freeze, 4) bsd_sessions A/B.

## Evening of Oct 29 (v0.1.23, cloud session) — freeze measurements and A/B tests

- 📏 **bsd requests measured** (`nx_bsdmon.c`, DECISIONS 54): per command
  (count, failures, bytes, time, **mapped** or copied), ring of the last 128
  operations; in the heartbeat (`bsd:`), the autopsy and
  `debug.log` (`[net] bsd requests per command`, every 1800 frames).
  Process resource limits in the heartbeat (`rl:` threads,
  events, transfer memory, sessions, memory).
- 🔀 **`[net]` switches** for the A/B tests, all set to the old
  behavior by default: `small_ipc`, `socket_config`, `bsd_sessions`,
  `poll_slice_ms = 0`.
- 🔎 Read: bsd *pointer buffer* 0x8000 under Eden; our `FIONREAD` fallback
  read 64 KB (always mapped); a `sysres=0` process takes its page tables
  from the heap shared with the system processes (Mesosphère).
  Leads, no proven cause.
- ✅ Under Eden (CI): 10 min of play + relaunch with `small_ipc = 1`,
  self-tests and forced GC `PASSED`, 1172 heartbeats with `bsd:` (1429
  requests, 0 mapped: without an AppId, little traffic under Eden).
- ⏸ Grouping of the small files of `Data/`: postponed (DECISIONS 55).

## On console, Oct 29 evening (v0.1.20 → v0.1.22)
v0.1.20 and v0.1.21 installed and tested: Room Settings ~3 s, stats
almost free (frame 484: 1904 stats in 15 ms), there remain ~250
opens of files opened only once (4.5 ms each) and Unity's
"other" on first displays. The forced GC suspension never
actually came into play (`forced 0` in every session).

**The "whole console freeze" comes from our process.** Five
reproductions:
- always **~390-420 s after the first connection to Photon** (0.1.18:
  397 s; 0.1.20: 391; 0.1.21: 395; 0.1.22: 402 and ~419), not after
  launch: 10 min on the main menu without network (133 calls) gave
  nothing, then a freeze 7 min after the match started; at that point
  **157 to 162 thousand network calls** (~380/s online);
- autopsy (DECISIONS 51) at the moment of the freeze: `bsd` (UnityMain in
  `bsdSend` 12.9 s, the network thread in `bsdPoll`, the heartbeat in
  `sendto`), `fs` (the watchdog in `fsFsCreateFile` 10.9 s, SD probe with no
  response) and `nifm` no longer respond; `am`, `time`, `ts`, `nvdrv`
  respond in 0-1 ms; `hid` (rumble) stops a little later; the
  services sometimes come back for ~1 s (HOME handled minutes later);
- **as soon as our process ends** (`stall_exit_s`), the console
  is back to normal right away (FTP, HOME);
- ruled out: the console's memory pools (stable to the KB until the freeze),
  socket leak (20 in 17 min), core 3 (hbloader limits to cores 0-2),
  `poll_slice_ms = 40` (the call rate does not drop).
Lead: what blocks are the requests that make the service map our memory
(large buffers: fs write, bsd send/poll); small
requests go through. Next measurement: the last ~100 network operations
in the autopsy, and the number of bsd requests per command.

## Evening of Oct 28 (v0.1.21) — to be verified on console

- 🟡 **Stats "served by the cache" still slow: cause found.**
  Each stat declared "absent" (≈ 60 %) redid a real libnx stat of the
  base name in the game folder (`basename_fallback`). Now memoized,
  served from a listing of the game folder, redone only after
  a creation in that folder (DECISIONS 48).
- ✅ Under Eden: 2807 "base name" fallbacks served from the listing, **0 real
  stats**; frame 1: stats 116 ms for 3087 (672-939 ms before).
- 🟡 **`keep_open`**: the v0.1.20 LRU remains the best one measured
  (two other policies tried, worse); the pool now shrinks
  by itself if the FS service refuses an open
  (DECISIONS 49).
- ❌ **Freeze autopsy** and **sampling profiler**: not done
  in the cloud session (interrupted by the safety filter, DECISIONS
  50). The v0.1.20 rumble proved that our process is alive during
  the full freeze.

## Night of Oct 28-29 (v0.1.20) — to be verified on console

- 🟢 **Hang no. 1 (GC / Quantum): fixed, validated under Eden.** A thread
  that has not reached a safe point 30 ms after the GC signal while it is
  executing game code is paused (`svcSetThreadActivity`), its
  registers are copied below its SP and a proxy thread runs bdwgc's real
  handler on its stack, in its place (DECISIONS 45,
  `[debug] gc_force_ms`). Self-test `smoke_test = 3`: a 5th thread looping
  without any call → 5/5 rounds (without the fix: missing ack, 3 s
  wait). In the game under Eden (`gc_spin_test = 900`):
  `il2cpp_gc_collect` returns in 90 ms with 1 forced delivery while
  a thread attached to IL2CPP loops — `PASSED` — then 10 min of normal
  play.
- 🟡 **Hang no. 2 (console freeze): no cause, but the heartbeat
  no longer depends on the malloc lock** (integer formatter `nx_bfmt`),
  counts slow/failed `sendto` calls, and gives the GC state, the kernel
  memory-mapping calls and the nv ioctls in progress (`nx_kmon.c`). New
  indicator with neither network nor SD: the controller rumbles twice every
  5 s if the game has been frozen for 12 s and the process is alive
  (`[debug] stall_rumble_s`).
- 🟡 **File opens**: read-only data files
  closed by Unity stay open (64 at most) and are handed back on
  reopening, without an FS round trip (`[io] keep_open`, DECISIONS 47).
  Under Eden: 213 reopens served without the SD at boot (out of ~960
  opens), no missing file, 10 min of normal play.
- ✅ Under Eden (CI): 1171 heartbeats received by the runner's listener in
  10 min (`gc:`, `kmem:`, `nv:` present); the nv "failed" were normal
  GPU event wait timeouts (`0xc010001e`), counted separately
  since then (`timeouts`).
- 🔧 CI: since `[io]` was added at the end of `mvlo.ini`, the lines added
  by emu-test (`autopress`, `hitch_ms`…) landed in `[io]` and were
  ignored; they land in `[debug]` again. emu-test also listens to the
  heartbeat (`netlog = 127.0.0.1:9999`, docker `--network host`).

## Bugs found and fixed thanks to emulation / analysis
1. `initJni` takes **5 arguments** in Unity 6000.3 (cloverpit: 4).
2. cloverpit's Boehm GC (ack without handler) could hit `ABORT("sp not set")`
   → POSIX signal emulation; then a possible deadlock on a contended
   mutex → all waits made interruptible.
3. errno: the shims returned newlib values (ETIMEDOUT=116) to bionic code
   that expects Linux values (110).
4. `fatal_error()` wrote nothing to the log; `DEBUG_LOG 0` disabled everything.
5. Code patches through an `svcMapProcessMemory` alias → now written before
   the mapping (also works without a real process handle).
6. 4 missing libunity 6000.3 imports (`writev`, `fseeko64`, …).
7. Unity error dialogs (Java modals) → blocked silently;
   now logged and closed automatically.
8. Non-interruptible newlib condition variables → GC deadlock
   reproduced by the self-test, rewritten.
9. Unity 6000.3 does not write `data.unity3d` (separate serialized files):
   the data check at startup wrongly required it.

## Open issues / known risks
- Real performance and audio never measured on console.
- Online network: implemented (nx_net.c) but never tested against Photon.
- Audio: cloverpit's FMOD/OpenSL patches re-derived, not listened to.
- Without a Photon AppId in `mvlo.ini`, online mode cannot
  connect (the game reports it: `photon.appid_quantum MISSING`). See TEST.md.
