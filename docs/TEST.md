# Testing mvlo_nx on your Switch

> Requirements: a Switch with CFW (Atmosphère) and hbmenu. The steps below
> only touch the `sdmc:/switch/mvlo/` folder.

## 1. Get the files (GitHub → *Actions* tab)

Two artifacts, both from the `main` branch:

| Workflow | Artifact | Contents |
|---|---|---|
| **nro** (latest green run) | `mvlo_nx-nro` | `switch/mvlo/mvlo_nx.nro` + `mvlo.ini.example` |
| **android-build** (latest green run) | `mvlo-sd-data-v2.2.0.1` | **encrypted** `mvlo-sd-data-v2.2.0.1.zip`: `switch/mvlo/` with the game's `.so` files, `assets/`, `BUILDINFO.txt` |

The repository is public but the game must not be: its data is in an
AES-256 zip whose password is the repository's `SD_ARCHIVE_PASSWORD` secret
(without this secret, the workflow publishes nothing). Windows Explorer cannot
open this encryption: use **7-Zip**.

Unzip both **into the same folder**: they complement each other.

## 2. Copy to the SD card

```
sdmc:/switch/mvlo/
├── mvlo_nx.nro
├── mvlo.ini                    ← created automatically on first launch (see §4)
├── libmain.so
├── libunity.so
├── libil2cpp.so
├── lib_burst_generated.so      ← only if it is present in the artifact
├── BUILDINFO.txt  IMPORTS_CHECK.txt  IL2CPP_INFO.txt   (diagnostics, small)
├── il2cpp/                     ← pre-extracted copy of Managed/ (+ unity.ver)
└── assets/
    └── bin/Data/
        ├── boot.config
        ├── globalgamemanagers, level0…, sharedassets…, resources.assets
        │   (or a single data.unity3d, depending on the build's compression)
        ├── Managed/Metadata/global-metadata.dat
        └── ... (everything else in the assets/ folder)
```

The folder name is not mandatory (the .nro looks for its files next to
itself), but `sdmc:/switch/mvlo/` is the one documented everywhere.

## 3. Launch

**Required: *title takeover* mode** (full memory). Launch any installed
game **while holding R**: hbmenu opens instead of the game, then pick
*Mario vs Luigi Online*.

- Do **not** launch from the Album (applet mode: only ~400 MB, the game
  won't have enough memory; the log reports it: `APPLET MODE`).
- Do not use an NRO/NSP forwarder: on the PvZ Fusion port from the same
  lineage, they crash for lack of memory-mapping permissions.
- Startup: expect ~30–40 s of a still screen (loading from the SD card)
  before the intro. Without the `il2cpp/` folder (old artifacts, before
  v0.1.1), Unity recreates it itself on first launch: ~1 min 45 s more, only
  once.
- If you update the game data, copy the **whole** folder again
  (including `il2cpp/`).

Controls: regular controllers (Joy-Con, Pro Controller, up to 4).
A = confirm, B = back ("nintendo" layout; `ab_layout = xbox` for the
opposite). Touch works in the menus. Jump = A/B, run = X/Y,
power-up = R/ZR, reserve = L/ZL, + = pause, − = scores.

## 4. Configure `mvlo.ini` (and the Photon AppId)

On first launch, `sdmc:/switch/mvlo/mvlo.ini` is created with the default
values (a copy of `mvlo.ini.example`). To play **online**, you need
**your own Photon Quantum AppId** — no AppId is built into the port.

### Create your own Photon AppId (free, 5 min)

1. Create an account at <https://dashboard.photonengine.com> (free plan:
   20 concurrent players, more than enough).
2. *Create a new app* → **Application Type: Quantum** → **Photon SDK:
   Quantum 3** (the game uses Quantum 3) → give it a name → *Create*.
3. In the list of applications, copy the **App ID** (a GUID like
   `xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx`).
4. Edit `sdmc:/switch/mvlo/mvlo.ini`:
   ```ini
   [photon]
   appid_quantum = xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
   ```
5. Optional: `fixed_region = eu` to force Europe.

⚠ A limit to know about: you only see players who use **the same AppId and
the same version of the game**. The official PC/Web versions use the
official AppId: no cross-play with them (that would take a PC version built
with your AppId). Between two Switches with this port and your AppId: yes.
**Without an AppId, you cannot play**: the game has no offline mode, even a
solo game goes through a Photon room. The *Play Game* menu then stays stuck
on "Connecting…" (B to go back) and the log shows
`photon.appid_quantum missing`.

Don't share your `mvlo.ini` publicly (it contains your AppId);
`debug.log` masks it (`(set, hidden)`).

## 4 sexdecies. v0.1.45: room chat (Nov 9)

> Touching the "Enter text…" box opens the Switch keyboard; **OK sends the
> message** (no need to touch "Send" anymore). "Send" is still useful if
> some text is waiting in the box, and no longer opens the keyboard. With a
> controller, the box still can't be reached (the game's scene).

## 4 quindecies. v0.1.44: long online sessions (Nov 9)

> Nothing to set. The `[mem]` line in `debug.log` ends with `threads: N
> created, M reaped, J joined, F failed | rl: thr X/96 …`. `X` should stay
> stable during a long session (around 65-70); `failed` should stay at 0.
> If a creation fails, a `[thr] pthread_create failed` line records it.

## 4 quaterdecies. v0.1.43: crash at launch (Nov 8)

> Nothing to set. `stall.log` no longer contains a "STALL #1" at every
> launch (loading frames 0-1 is allowed 20 s). If the watchdog ever
> crashes, `stall.log` ends with `[crash] fault inside the watchdog:
> the watchdog stops, the game carries on` and the game continues.

## 4 terdecies. v0.1.42: replay cache, capped logs (Nov 5)

> Nothing to set. At the end of each game, `debug.log` says where the
> replay info comes from: `[io] replays stats since the last save: 22 from the
> cache, 3 from the card in … ms` (many more "from the card" on the first
> game of the session, that's normal). At launch, a `stall.log` or
> `prof.log` over 1 MB becomes `stall.prev.log` / `prof.prev.log`.

## 4 duodecies. v0.1.41: end of game (Nov 5)

> Nothing to set. In `debug.log`, at the first end of a game: `[io]
> write-behind on`, then `[io] write-behind: …Replay-….mvlreplay, N KB from
> 3 writes, written+closed+committed in … ms`. To check in a real game:
> the small freeze when a game ends (especially the later ones), and that
> the replays open in the "Replays" menu. If in doubt:
> `[io] write_behind = 0` (previous behavior). See DECISIONS 78.

## 4 undecies. v0.1.40: in-game stutters and memory (Nov 4)

> Nothing to set. The `oc_pool_mb = 448` / `gpu_arena_mb = 256` lines
> added to `mvlo.ini` on Nov 3 are now the default values (keeping them
> changes nothing). In `debug.log` at startup: `[log] writer thread
> started` and `[patch] CreateZStream deflate level applied`. If the OC
> reserve grows: `[oc] commit-pool grown by 32 MB` (normal, no more crash).
> To check in a real public game: no more stutters during play, end of
> game (replay save) ~0.35 s instead of 0.5-0.7 s. To compare:
> `[debug] zlib_level = 0` (original level). See DECISIONS 72-74.
>
> **Result (Nov 4→5).** 52 min online, Jungle included: no crash, steady
> 30 fps. "Quit" now returns to the HOME menu without the "… an error
> occurred" message (DECISIONS 77); if it comes back, note the time and
> grab `atmosphere/crash_reports/` (reports named after the icon that
> launched the game).

## 4 decies. v0.1.37: first-time stutters (Nov 2, evening)

> Nothing to set: preloading (`[io] preload_kb = 1024`), phantom
> descriptors (`[io] phantom_fds = 1`, v0.1.38) and asynchronous UDP sends
> (`[net] async_send = 1`) are on by default. To play with the players from
> the website: `[photon] build_identifier = official`. On the main
> menu, give the preload ~10 s (`[io] preload: … read in 7 s`
> in debug.log) before going into "Play". To measure: `[debug]
> prof_ms = 150` then `tools/switch/prof_report.py` on `prof.log` (see
> DECISIONS 65-67). Create games as a **private room**.

## 4 nonies. v0.1.30: the online freeze is fixed (Nov 2)

> **Result.** The cause was `/dev/urandom` emulated by a 64 KB file that
> ran out after ~400 s of connection (DECISIONS 63). With v0.1.30,
> "Play" ran 11 min 40 s without a freeze. All that's left is to confirm on
> **a real match longer than 7 minutes**; no `[net]` setting is needed. The
> tests in section 4 octies below are obsolete.

## 4 octies. Testing v0.1.27: measure, then fix the online freeze

> **Oct 30, cloud session.** Research (DECISIONS 58, 61): **no Photon event
> at ~400 s** (its timers are ≤ 60 s or = 1 h) → the freeze most likely
> comes from the **console's network stack** on our long-lived connected
> UDP socket. `bsdsocket` hosts bsd **and DNS**; nifm depends on it →
> "bsd goes down first, then nifm". Documented analogue:
> **sys-ftpd #30**, buffer exhaustion (`ENOBUFS`) → "hangs the system".
> Since 18.0.0, **15 `bsd:u` sessions for the whole console**.
>
> v0.1.27 adds the **bsd errno breakdown** (heartbeat + autopsy:
> `err: ENOBUFS=N …`) — watch whether `ENOBUFS` climbs before the freeze —
> and three **candidate fixes** (below). **Still launching in title
> takeover (applet type 0)**: the "applet mode" test is moot, removed.

Copy only `mvlo_nx-nro` (no android-build). Keep `mvlo.ini` and
`[debug] netlog` with the PC listener. The added lines go under **`[debug]`**
or **`[net]`** as indicated; set them back as before after each test.
**The "Play" screen alone (room list, without playing) reproduces the freeze
in 6 min 40 s**: no need to start a match. Measurements first (1-2), then the
fixes (a-c), each ≤ 8 min.

**Test 1 — the most decisive: Photon-like traffic WITHOUT Photon (`udp_soak`).**
Tells whether it's the console's network stack or Photon.
- On the PC, start the UDP echo (Windows Firewall lets php-cgi through):
  `php tools/switch/udp_echo.php 9998 udp_soak.txt` (or
  `php-cgi -q -d max_execution_time=0 -f tools/switch/udp_echo.php 9998 udp_soak.txt`).
- In `mvlo.ini`, under `[debug]`: `udp_soak = <PC IP on the network>:9998`.
- Start the game and **stay on the MAIN MENU** (don't go into
  "Play"), controller put down, ~10 min. The PC should show ~20 packets/s.
- **Result:** a freeze on the main menu after ~400 s = **it's the console's
  network stack** (our connected UDP socket + 5 ms poll), not Photon.
  No freeze in 10 min = the socket pattern alone is not enough (a cause
  specific to Photon, or to the nifm state of a real connection).
- **Variant**, only if test 1 doesn't freeze: add
  `udp_soak_connect = 0` (unconnected socket, `sendto`) and run it again.
  Tells whether what matters is the socket being *connected* (`connect`+`send`).
- Send `debug.log` and `udp_soak.txt` (the PC time of the last line dates
  the freeze).

**Test 2 (measurement) — capture the wire at the freeze + watch for exhaustion
(`photon_ring`).**
- In `mvlo.ini`, under `[debug]`: `photon_ring = 1`.
- The "Play" screen alone until the freeze (~6 min 40 s), controller put down.
- **Result:** the autopsy contains "last Photon datagrams" (the ~30 s before
  the freeze: direction, size, first 16 bytes) and "bsd errno breakdown". In
  the heartbeats, watch for `err: ENOBUFS=…`: if it **climbs** before the
  freeze, it's buffer exhaustion (the sys-ftpd lead). Send `autopsy.log` +
  the PC listener's output.

### Candidate fixes (one per "Play"-only session, ≤ 8 min, under `[net]`)

Each one is `0`/absent by default. If a fix **removes or clearly delays**
the freeze, we have a lead; otherwise we rule it out. Always send back
`debug.log` (+ `autopsy.log` if it froze), noting the setting.

**a) `chiaki_cfg = 1` — chiaki-ng's socket config.** Large UDP buffers
(256 KB), `sb_efficiency 8`, up to 16 bsd sessions (automatic fallback if the
console refuses: look at the `[net] BSD sockets up … N bsd sessions` line).
- **Result:** no more freeze / freeze much later = not enough
  buffers/sessions (exhaustion). Same freeze at ~400 s = it's not the sizing.

**b) `udp_unconnected = 1` — don't connect the UDP socket at the bsd level.**
We keep the peer and do a filtered `sendto`/`recvfrom`; Photon notices nothing.
- **Result:** no more freeze = it really is a long-lived **connected** UDP
  socket rotting in bsdsocket (the most likely lead). Same freeze =
  it's not about being connected. (If Photon no longer connects at all,
  tell me: the peer filtering would be too strict.)

**c) `photon_rebind_s = 350` — a new socket before ~400 s.** At 350 s, a fresh
UDP socket replaces the old one under the same fd (new source port). **Known
risk: Photon may see a new flow and disconnect (~10 s later).**
Run it with `photon_ring = 1`.
- **Result:** if the console **doesn't freeze** at ~400 s (even if Photon
  disconnects) = the freeze is tied to the lifetime of ONE bsd socket → a
  periodic rotation would be a workaround. If it freezes anyway = the
  socket's lifetime is not the factor. If Photon holds up after the port
  change, that's a bonus (rotation viable as a real fix).

**Extra setting — `bsd:u` sessions (A/B).** `bsd_sessions = 1` then
`bsd_sessions = 8`. If `1` delays the freeze and `8` makes it worse =
pressure on the 15 shared sessions.

Suggested order: **1 → 2 → a → b → c**. Test 1 settles network stack / Photon;
test 2 shows any exhaustion; **b** is the most likely fix.

## 4 septies. Testing v0.1.23: measurements and A/B tests of the online freeze

> **Done on Oct 29 (v0.1.23 → v0.1.25, local session), see STATUS "the
> freeze narrowed down".** Reference and B done (B had no effect); A and D
> dropped (17 mapped requests out of 162,000; the 5 ms polls come from
> Photon, not from the slicing). C not done. Instead: flood on the menu (no
> freeze), the "Play" screen alone (freeze at 396 s connected),
> disconnect/reconnect (cumulative count), `local_wait` (freeze at 409 s).

Copy only `mvlo_nx-nro` (no android-build). Keep `mvlo.ini` and
`[debug] netlog` with the PC listener.

1. **Reference game, default settings**, until the freeze (~7 min
   online). Send `autopsy.log`, `debug.log` and the PC listener's file.
   What I need:
   - in the heartbeats, `bsd: N req (M mapped ...)` and, every 2 s,
     `rl: thr .. ev .. tmem .. sess ..`: does any counter climb
     steadily until the freeze?
   - the `[net] BSD sockets up ... bsd pointer buffer 0x....` line and, if
     it appears, `[net] FIONREAD refused by bsd ...`;
   - in `autopsy.log`, the "bsd requests per command" table and the last
     128 operations.
2. **A/B tests**, one per game, each until the freeze or 15 min online
   without a freeze. Put the line under `[net]` in `mvlo.ini`, then set it
   back as before:
   - A: `small_ipc = 1` (no mapped network request);
   - B: `socket_config = 1` (default libnx buffers);
   - C: `bsd_sessions = 6`;
   - D: `poll_slice_ms = 0` (far fewer requests per second);
   For each test: freeze yes/no, after how many seconds online, and
   the `bsd: N req` counter at the freeze. If D clearly delays the freeze,
   it's the number of calls; if nothing changes, it's a duration.

## 4 sexies. Testing v0.1.22: during a freeze

1. Play online until the freeze (~7 min connected).
2. Don't touch anything. ~15 s after the freeze: **one long vibration** =
   report written to `autopsy.log`; **three short ones** = the SD card didn't
   take it (the `netlog` PC may have received it). After that, two short ones
   every 5 s as long as the process is running.
3. ~90 s after the freeze, the game closes by itself ("The software was
   closed because an error occurred"): no more need for POWER. Note whether
   HOME and everything else respond right away.
4. Send `autopsy.log`, `debug.log`, `stall.log`.

To test the tooling without waiting: `[debug] fake_stall_frame = 3600`
(stops the render loop at that frame), then set it back to `0`.

## 4 quinquies. Testing v0.1.21, in order

Copy only `mvlo_nx-nro` (no android-build). Keep `mvlo.ini`.

1. **Room Settings and game start**: compare with the v0.1.20 timings.
   Useful lines in `debug.log`:
   - `[io] basename fallback: N answered from the game folder listing (...),
     M real stats` — M should stay close to zero;
   - `[io] kept-open files: ... reopens served without the SD card ...` —
     more reuses than in v0.1.20 for the same run;
   - `[hitch]`: `stat` should cost clearly less than 0.3 ms per call,
     `file open` less too.
   If a file can't be found or a load fails: `[io] keep_open = 0`
   and tell me whether that fixes it.
2. Nothing else changes: the vibration during a freeze and everything else
   from v0.1.20 still apply.

## 4 quater. Testing v0.1.20 (night of Oct 28-29), in order

Copy only the `mvlo_nx-nro` artifact (the `.nro`; no new game data, no
android-build needed). Keep `prefs.kv` and `mvlo.ini`: the new options have
their default values. Keep `[debug] netlog` and the PC listener if
possible.

1. **Startup + Room Settings**: as in v0.1.19, a bit faster
   (files kept open). In `debug.log`, the line `[io] kept-open files:
   ... reopens served without the SD card`. If there's a file problem
   (missing text, failed load): `[io] keep_open = 0`.
2. **Hang #1 (the game freezes, the console is alive)**: start game after
   game, like when it used to happen. It should no longer freeze. In
   `debug.log`, `[sig] main stuck NN ms in game code (...):
   delivering its signals ... on its behalf` lines = the fix was used. If the
   game freezes anyway: `stall.log` + `debug.log` + the latest
   heartbeats (they now say `gc: N parked for ... ms, M not acked
   (who ...)`). To compare, one game with `[debug] gc_force_ms = 0`
   (old behavior).
3. **Hang #2 (the whole console freezes)**: play ~15 min with the PC
   listener. During a freeze:
   - **does the controller vibrate twice every 5 s?** (after 12 s of a
     frozen game). Yes = our process is still running, it's not the whole
     console; no = the process has stopped too;
   - send the last ~20 heartbeats: new parts `gc:`, `kmem:`
     (kernel memory calls, with `IN FLIGHT` if one is in progress),
     `nv:` (GPU ioctl, `IN FLIGHT`), `send:` (slow/failed UDP sends);
   - note whether the heartbeat stops **at the same time** as the vibration.
4. **Optional self-test** (≈ 1 s, on the menu): `[debug] gc_spin_test = 600`
   → a `[selftest] GC with a spinning thread: ... PASSED` line around frame
   600. Set it back to 0 afterwards.
5. **Stutters**: `[hitch]` (100 ms threshold) separates `file open` / `stat` /
   `fstat` / `access` / GL. Send me a few lines from an object's first
   appearance in a game.

Lines to send me first: `[sig]` (especially `stuck ... on its behalf`
and `cannot ...`), `[selftest]`, `[io] kept-open`, `[hitch]`, and for a
freeze: `stall.log`, the latest heartbeats, vibration yes/no.

## 4 ter. Testing v0.1.13 (night of Oct 27-28), in order

Copy **both** artifacts from the latest commit: `mvlo_nx-nro` (the `.nro` and
`splash/`) and `mvlo-sd-data-v2.2.0.1` (rebuilt game data: fixes for the
keyboard and `UI/Navigate`). Keep `prefs.kv` and your `mvlo.ini` (the new
options have default values, nothing to add).

1. **Startup**: the 1st launch after copying stays black ~4 s (it records
   Unity's EGL config in `mvlo/.eglconfig`). **Relaunch**: the loading
   screen should appear from ~0.5 s, then carry on without a jump when the
   game takes over. Lines: `[load] early loading screen on` then
   `handed to Unity after ... ms`. If the screen stays frozen or black after
   that: `[video] early_splash = 0` and send me `debug.log`.
2. **Keyboard**: nickname, then a room's chat. After confirming (+) on the
   Switch keyboard, the game's keyboard should **close by itself**.
3. **Two games in a row**, the 2nd one at least 5 minutes (that's when the
   console used to freeze). If it freezes:
   - note whether the **music carries on normally** or **loops a snippet of
     sound** (that tells whether the CPU is still running);
   - send `debug.log`, `stall.log`: the last `[gpu]`, `[nv]`,
     `[wd] alive` lines (they now carry the GPU state) tell what was
     growing or what the GPU was waiting for.
   Then try again once with `[video] shader_code_mb = 0` and once with
   `[debug] nv_tmem_mb = 8` to find out which of the two safeguards matters.
4. **Stutters**: during these games, every pause longer than 100 ms writes a
   `[hitch]` line that says where the time goes (GLSL compilation, linking,
   generating new code, textures, waiting for the GPU…). That's what I need
   to choose the next step. To compare: one game with
   `[video] compile_boost = 0`, and one with `[video] nv_shader_opt = 1`.
5. **NullReferenceException** at the start of the 2nd game: it should no
   longer appear; if it comes back, send the full lines (with the stack).

Lines to send me first: `[hitch]`, `[gpu]`, `[nv]`, `[load] early
loading screen ...`, `[gpu] shader code segment full ...` (with the time), and
`stall.log` in case of a freeze.

## 4 bis. Testing v0.1.9 (night of Oct 26-27), in order

Copy the new `.nro` (`mvlo_nx-nro` artifact from the latest *nro* run). The
SD data hasn't changed, unless the v0.1.8 Unity rebuild (X/Y,
`build_identifier`) has finished: then also take `mvlo-sd-data-v2.2.0.1` again
(the whole folder, `il2cpp/` included). Keep `prefs.kv`: the zeros left in it
for real-valued settings are cleared automatically when it loads.

1. **Startup**: after 2-4 s, the Infenek logo on a black background (2 s),
   then the game's images crossfading and slowly zooming, the game's logo
   at the top, and at the bottom "LOADING...", the percentage and the bar.
   The images are in `switch/mvlo/splash/` (`mvlo_nx-nro` or
   `mvlo-sd-data` artifact); without them, the plain screen is shown. The bar
   shows a percentage from the 2nd launch on. If it crashes or freezes at that
   point, or just as the game starts: `[video] loading_screen = 0`. If the
   animation stutters too much: `[video] loading_fps = 30`.
2. **Sound and HUD**: the music should play from the title screen on (default
   volumes 0.75 / 0.5 / 0.75), the in-game HUD should be large (HUD Scale 8).
   Change a volume in the options, quit, relaunch: it should be kept.
   Even bigger: `[game] hud_scale = 10` (or 12).
3. **Keyboard**: *Play Game* → *Nickname* field → A: the Switch keyboard
   should open; confirm (+) → the nickname changes. The game's keyboard may
   stay displayed behind it: B closes it, A reopens the Switch keyboard. Same
   in a room's chat, and when touching the field with your finger. If it
   keeps reopening in a loop: `[input] keyboard = 0` (and send me the `[kbd]`
   lines); if it never opens, or if it opens on an A press somewhere other
   than a text field: `[input] keyboard = 2`.
4. **Starting a game**: note how long the pauses last (before: 6 to 9 s).
   If something breaks or if data seems corrupted:
   `[io] cache = 0` (old path).
5. Play online for a few minutes as usual.

Logs to send me: `debug.log` (+ `debug.prev.log`), `stall.log` if it has
grown, and one sentence per item above (OK / not OK). New useful lines in
`debug.log`:
- `[load] ...` (loading screen), `[kbd] ...` (keyboard),
- `[io] cache: ...` and `[io]   <file> ...` (SD time per file, every
  ~15-30 s and after each frame longer than 1 s), `[io] slow SD read`,
- `[perf] per frame: nativeRender X ms ... present Y ms | limiter sleep Z ms`
  (tells what caps it at 30 fps),
- and, if the `ArgumentOutOfRangeException` from `UI/Navigate` comes back,
  **the full lines** that contain it (with the stack if it follows).

If a test goes wrong, turn off the new features one by one
(`[video] loading_screen`, `[input] keyboard`, `[io] cache`, then
`[debug] jni_forname = 0`, `[debug] jni_avariants = 0`).

## 5. Which logs to send me

After a test (whether it works or crashes), copy from the SD card:

1. `sdmc:/switch/mvlo/debug.log` — **the most important** (timestamped)
2. `sdmc:/switch/mvlo/debug.prev.log` — the previous launch
3. `sdmc:/switch/mvlo/stall.log` — if it exists: hangs (watchdog) and
   **crash dumps** (registers, backtrace `libxxx.so+0x…`)
4. recent `sdmc:/atmosphere/crash_reports/*.log` and recent
   `sdmc:/atmosphere/fatal_errors/*.bin`, if there are any
5. `sdmc:/switch/mvlo/BUILDINFO.txt` (identifies the data used)
6. one sentence: how far it gets (black screen? logo? menu? game?), docked
   or handheld, and which firmware/Atmosphère.

If the console **freezes** (nothing responds), set `log_sync = 1` in the
`[debug]` section of `mvlo.ini` and run the test again: each line is then
written immediately, so the last line of the log tells where it stopped.

## 6. Diagnostic switches (`[debug]` in mvlo.ini)

Only change them if I ask you to (all at `1` by default):

| Key | Effect if `0` |
|---|---|
| `region_patch` | Unity keeps 256 MB reservations (likely OOM on 4 GB) |
| `choreographer_patch` | the engine looks for an Android Choreographer |
| `audio_opensl_patch` / `fmod_patch` | audio probably silent |
| `time_hook` | time only comes from the vsync feed |
| `vsync_feed` | the engine waits for a vsync that never comes (freeze) |
| `cpu_boost` | no CPU boost while loading |
| `tls_anchor = 0` | thread pages not reserved at startup (possible `svcMapMemory FAIL` crash; 64 by default) |
| `gc = disable` | GC never runs (workaround if it freezes in the middle of a game) |
| `jni_forname = 0` | `Class.forName` returns `java/lang/Object` like before v0.1.9 |
| `jni_avariants = 0` | JNI `…MethodA` calls no longer receive their arguments (before v0.1.9) |
| `log_level = 2` | verbose log (slow) |
| `log_sync = 1` | each line written right away, on the thread that writes it (slow; for a full console freeze) |
| `zlib_level = 0` | the game's gzip at its original level (6): slower replay save at the end of a game |
| `[io] write_behind = 0` | files created by the game (replays) written on its thread, like before v0.1.41 |
| `oc_pool_mb` / `gpu_arena_mb` | memory reserved at startup (448 / 256 MB by default; more = possible "Jungle" crash) |
| `autopsy = 0` | no `autopsy.log` report during a freeze |
| `stall_exit_s = 0` | the game no longer closes by itself after a freeze (90 s by default) |
| `unity_args = ...` | command line passed to Unity (advanced) |

Online freeze diagnostics (all off by default): `[debug] photon_ring = 1`
(a ring of the last 30 s of Photon datagrams, in the autopsy and in
`debug.log`), `[debug] udp_soak = ip:port` (mimics Photon traffic without
Photon, see 4 octies; `udp_soak_connect = 0` for an unconnected socket),
`[net] bsd_sessions = 1..8` (bsd:u sessions, default 3), `socket_config = 1`
(libnx buffers), `small_ipc = 1`, `local_wait = 1`, `poll_slice_ms`.

`[video] resolution = 1280x720` forces 720p (smoother when docked),
`frame_limiter = 0` removes the wrapper's 60 Hz limiter.
`[video] loading_screen = 0` turns off the loading screen,
`[input] keyboard = 0` the Switch keyboard, `[io] cache = 0` the SD read
cache (`cache_mb`, `block_kb`, `readahead_kb` to tune it),
`[game] hud_scale = N` forces the HUD size.

## 7. What has been tested, honestly

See `docs/STATUS.md`: what has run under emulation (Eden, in the CI) and what
has never run on a real console. The first try on a console
is the real test.
