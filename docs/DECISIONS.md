# Decision log

Each entry: the decision, why, and how to revert it.

1. **Tag v2.2.0.1** (= HEAD of the official repo as of Oct 25, 2026).
   Configurable (`game_ref` of the android-build workflow).

2. **The game's code is never copied here.** CI does a `checkout` of the
   official tag and applies `unity/prepare_project.py`: 5 lines changed in 3
   C# files + the AppId emptied in `PhotonServerSettings.asset`. The script
   fails if a pattern does not match exactly once.

3. **Photon AppId**: the official AppId is also hard-coded in
   `NetworkHandler.cs`. Replaced by `NxPort.NxConfig.AppIdQuantum`, read from
   `sdmc:/switch/mvlo/mvlo.ini` (`[photon] appid_quantum`). The script checks
   that the official AppId no longer appears anywhere in `Assets/`.

4. **Official authentication bypassed** (`photon.use_official_auth = 0`):
   it goes through a custom auth tied to the official AppId (note:
   `UnityWebRequest` actually works, the game's news is downloaded fine
   under Eden). With your own AppId, Photon's anonymous auth is enough; a
   persistent UserId is generated (`PlayerPrefs nxport_userid`). The update
   checker (GitHub API) is also disabled by default.

5. **Android build: IL2CPP, ARM64 only, GLES3 only, Activity, no
   multithreaded rendering, no Swappy, ETC2 textures, Public symbols.**
   Everything is set in `unity/Editor/NxPortBuild.cs` (not in the game's
   ProjectSettings). ETC2 rather than ASTC: mandatory in GLES3, always
   decodable by mesa.

6. **APK kept in the Actions cache** (not as an artifact): the cache does not
   count toward the storage quota of private repos (500 MB on the free plan).
   Only the final SD folder is published as an artifact (30 days). ⚠ a cache
   unused for 7 days is deleted: rerun `android-build` (≈ 2 min if the cache
   exists, ≈ 1 h otherwise).

7. **libunity offsets derived from symbols**, not from fingerprints:
   `tools/re/derive_unity.py` on the Unity player extracted from the Docker
   image `unityci/editor:ubuntu-6000.3.10f1-android-3` (same BuildID as the
   APK). The wrapper checks the BuildID, then each guard word, before
   writing. The CI packaging also checks the BuildID of the APK's libunity.

8. **Memory granularity patch 256→64 MB**: 21 sites found in the same 11
   functions as cloverpit/pvz_fusion (transformation rules reproduced; the
   script requires exactly 21 sites).

9. **Time/vsync**: instead of only incrementing the vsync counter
   (cloverpit), a 60 Hz thread calls the engine function
   `SetFrameTimeNanos(t)` — exactly what the Android Choreographer does.
   This unblocks `WaitVSync` **and** advances the timestamp from which Unity
   derives `Time`. The wall-clock detour of `TimeManager::Update` is kept as
   well (can be disabled: `time_hook = 0`). cloverpit's il2cpp hooks for
   `Time`/`Screen` (addresses specific to their game) are removed.

10. **GC: POSIX signal emulation** (nx_signals.c) instead of cloverpit's
    "ack without handler" bridge, which leaves `stop_info.stack_ptr` empty
    (Unity's bdwgc: `ABORT("GC_push_all_stacks: sp not set!")`). The real
    bdwgc handler runs on the target thread at the next safe point (waits
    are already split into slices of ≤ 16 ms). Fallback: `gc = disable`.

11. **Input through a C# bridge** (DllImport `nxbridge` → `nx_bridge.c`)
    rather than `nativeInjectEvent` (which would require complete fake Java
    MotionEvents). Controllers 1–4 → virtual Input System `Gamepad`s, touch
    screen → virtual `Touchscreen`, HD rumble. **"nintendo" A/B layout by
    default** (A = confirm); `ab_layout = xbox` for the position-based layout.

12. **Sockets**: every socket is non-blocking on the libnx side; blocking is
    emulated in 10 ms slices (honors `SO_RCVTIMEO`, and lets the GC
    through). IPv6 cleanly refused (the Switch has none) → Photon uses IPv4.
    Enlarged UDP buffers (Quantum sends bursts).

13. **Resolution**: `auto` = 1080p docked, 720p handheld (cloverpit forced
    1080p everywhere).

14. **Logs always on** (cloverpit compiled with `DEBUG_LOG 0` → no logs at
    all). Timestamps, levels, `debug.prev.log` kept, `log_sync = 1` for the
    "write everything immediately" mode. `fatal_error()` now also writes its
    message to the log.

15. **No process handle** (launched outside hbloader, e.g. an emulator):
    fall back to the `CUR_PROCESS_HANDLE` pseudo-handle instead of a fatal
    error.

16. **Emulation test in CI** (`emu-test.yml`, Eden + lavapipe): the artifacts
    cannot be downloaded from my environment, and the game data must not
    leave it; the job prints `debug.log`.

17. **Removed from cloverpit**: Rewired/UnityEngine.Input hooks, ffmpeg video
    player (MvLO has no VideoPlayer), Firebase, Panik PlatformAPI, DLC,
    cursor/gyro layer (nx_pointer). Added: nx_compat.c (4 libunity 6000.3
    imports missing from the 6000.2 tables).

18. **Icon**: generated (text "MvL ONLINE NX"), no Nintendo assets.

19. **Commit author**: `Jonathan8520`, no Co-Authored-By line (explicit
    instruction from the user, taking priority over the default setting).

20. **Android build #1 lost** ("hosted runner lost communication", starved
    runner): the log stops during Quantum's automatic *bake* of the
    CustomBeach scene, after I had the runner's swap removed. Rerun with
    12 GB of swap, Unity container capped at 13 GB (clean failure rather than
    a lost runner), memory/disk log, and `AutoBuildOnBuild: 0` (the map data
    is already baked and committed upstream; it is also baked on every scene
    save). `NXPORT_QUANTUM_BAKE=1` restores the bake.

21. **Android build #6 succeeded** with 4 GB of swap (12 GB filled the disk)
    and a container capped at 7 GB. 506 imports resolved, 0 missing,
    libunity BuildID identical to the patch table.

22. **No `data.unity3d`**: Unity 6000.3 without LZ4 compression writes
    separate serialized files (`globalgamemanagers`, `level0`,
    `sharedassets*`…). `check_data()` accepts either one instead of requiring
    `data.unity3d` (seen on the first full launch under Eden).

23. **First full launch under Eden**: loads without errors, title screen
    rendered for 10 min straight (~7.7 fps with lavapipe software rendering,
    so meaningless for the real Switch), C# bridge connected, controller
    detected, FMOD/OpenSL started. To test the menus without a screen or
    keyboard, `[debug] autopress = A@65,DOWN@185,...` injects presses on
    controller 1 (test option only, empty by default).

24. **First console test (v0.1.0)**: still screen for 105 s, then intro with
    sound, then a crash after ~25 frames. Two causes found in
    `debug.log`/`stall.log`:
    - the 105 s = Unity copying `assets/bin/Data/Managed/` to
      `<filesDir>/il2cpp/` ("Extracting il2cpp resources", once per build,
      controlled by `il2cpp/unity.ver` = `unity_app_guid`). Now done by
      `tools/package_sd.sh`; CI checks that the game rewrites nothing.
    - the crash = running out of address space: Unity reserves ~1.8 GB
      (uses only ~80 MB of it). On console the stack region is split into 5
      holes and only the largest (512 MB) served as the "overcommit" window
      (under Eden: a single 1.5 GB hole, hence no problem there). The window
      now covers the whole stack region, the already-occupied pieces are
      marked and skipped, a reservation can stop right before an obstacle,
      and each page knows its reservation (a `munmap` never frees the pages
      of a neighboring one).
      The last 256 MB of the region stay outside the window: libnx places
      thread stacks there (first version without that margin: Eden asserts
      `virt_addr != 0`, thread creation stuck, black screen).

25. **60 fps**: the game sets `Application.targetFrameRate = -1` ("Max FPS"
    unlimited by default) and Unity's Android player turns that into 30.
    Rather than patching the C# (a 1 h Unity rebuild), `dlsym_fake` wraps
    `il2cpp_add_internal_call` and replaces only the setter
    `UnityEngine.Application::set_targetFrameRate`: a value ≤ 0 becomes
    `[video] target_fps` (60; 0 = Unity's behavior), a value chosen in the
    game's options is respected.

26. **Safe points at the entry of network shims** (v0.1.4). GC signals are
    only delivered at safe points. A thread calling `poll()` in a loop with a
    timeout ≤ 10 ms left `poll_fake` through the timeout check, before the
    end-of-loop safe point: stop-the-world stuck forever after
    "Creating a game". Rule: every shim that a thread may call in a loop
    without blocking (non-blocking, `EAGAIN`, short timeout) starts with
    `nx_sig_safepoint()`. `pthread_exit` also unregisters the thread
    (signals + diag), and `nx_sig_thread_register` resets an entry inherited
    from a recycled `pthread_t`.

27. **TLS pages pinned at startup** (v0.1.5). The kernel maps a
    `ThreadLocal` page (8 slots of 0x200) at a random free address in the
    stack region when all of them are full, and unmaps it when it becomes
    empty (mesosphère `KProcess::CreateThreadLocalRegion` /
    `KThreadLocalPage::Finalize`). The overcommit window covers most of that
    region: a page placed inside a Unity reservation that is not committed
    yet makes `svcMapMemory` (0xd401) fail when it gets committed. Before
    arming the window, `tls_anchor_setup()` starts `debug.tls_anchor` (64)
    libnx threads and keeps one thread alive per page created: later threads
    take the free slots of those pages instead of causing new ones to be
    created. Priority = that of the calling thread (0x3F is only valid on
    core 3, see libnx `thread.h`).

28. **PlayerPrefs dispatched by receiver** (v0.1.7). Unity 6000.3 loads
    `android/content/SharedPreferences` & co. through a ClassLoader: the fake
    JNI returned `java/lang/Object` for these classes, so the method IDs did
    not carry the name expected by `unity_jni.c`. `jni_fake.c` now routes to
    `unity_jni.c` when the receiver is one of its handles
    (`unity_handle_class`), `GetObjectClass` returns the real class of those
    handles, and `getSharedPreferences` (receiver = the context) is routed by
    name to a `UJ_PREFS` handle. Storage unchanged: `prefs.kv` on the SD.
    v0.1.8: keys go through `String(byte[], String)` then
    `Uri.encode/decode`, also loaded via `Class.forName`: they are now
    recognized by their signature (`([BLjava/lang/String;)V`,
    `(Ljava/lang/String;)Ljava/lang/String;` + name `encode`/`decode`).
    Without this, every key except `__UNITY_PLAYERPREFS_VERSION__` arrived
    empty and was ignored.

29. **X/Y by label** (`nintendo` layout). The game names the buttons the Xbox
    way (`buttonWest` = "x", `buttonNorth` = "y", Input System
    `Gamepad.cs`): Switch X → West, Switch Y → North, just like A → South and
    B → East. The `xbox` layout (by position) is unchanged.

30. **`[photon] build_identifier`**. Photon only puts together players with
    the same AppId **and** the same AppVersion =
    `GameVersion.ToStringIgnoreHotfix()` + `-` + `BuildIdentifier.asset`
    (`vanilla` in the public sources, `official` in the official releases).
    A `prepare_project.py` patch routes the suffix through
    `NxConfig.BuildIdentifier()`: empty = the game's own.

31. **Actual settings at 0: the real cause was an empty
    `Screen.resolutions`** (v0.1.9). `Settings.LoadFromVersion0` (the game's
    default values) starts with `Screen.resolutions[^1]`; Unity Android fills
    this array with `Display.getSupportedModes()`, to which the fake JNI
    answered nothing. The empty array throws an exception, swallowed by the
    `catch {}` of `LoadSettings`, **before** HUD Scale (8), the volumes
    (0.75/0.5/0.75) and Max FPS are set: they stayed at 0 and were saved
    as 0. `getSupportedModes` now returns one mode (render size, 60 Hz) and
    `getRefreshRate` 60. In addition: `getFloat` is routed to `prefs.kv` (it
    always returned 0), and zeros already saved for these 4 keys are erased
    on load (0 is outside every slider of the game).
    The console's tiny HUD came from there: the game's default (8) is
    already the slider's maximum. `[game] hud_scale` allows going beyond it.
    Same cause, most likely, for the `ArgumentOutOfRangeException`s in
    `UI/Navigate`: the *Resolution* option (Graphics tab) is an empty list;
    left/right moves its value to -1 then 0, and
    `ResolutionOptionLoader.OnValueChanged` reads `resolutions[spo.value]`
    in the `Navigate` callback. With one mode, the list has 1 entry, the
    value no longer moves and the callback is no longer called.

32. **`Class.forName` returns the requested class** (`[debug] jni_forname`,
    default 1). Unity 6000.3 loads all its Java classes through
    `Class.forName(name, false, loader)`; the fake JNI always returned
    `java/lang/Object`. The static-field handlers written for the real names
    (`AudioManager.PROPERTY_*`, `Context.*_SERVICE`,
    `Environment.MEDIA_MOUNTED`…) are finally used. As a precaution,
    dispatching **by class name** to `unity_jni.c` is limited to the storage
    classes (SharedPreferences, Map/Set/Iterator, AssetManager,
    InputStream); Display, Context, Resources and UnityPlayer keep the
    previous dispatch (`act_*` handlers).

33. **`Call*MethodA` receive their arguments.** These variants (`jvalue[]`
    array) called the dispatch without any arguments: `va_arg` read leftover
    register contents. They now build a real AArch64 `va_list` from the
    signature (GP/FP save areas + stack, floats promoted to double).
    Verified under qemu-aarch64 against real variadic calls (all types,
    overflow > 8 registers).
    `[debug] jni_avariants = 0` restores the old behavior.

34. **Block cache for SD reads** (`nx_io.c`, `[io] cache`, default 1). The
    old cache kept a 1 MB window per large file (≥ 4 MB), under a global lock
    held during the SD read: every jump outside the window re-read 1 MB, and
    all other threads waited. The new one: shared 64 KB blocks (32 MB, LRU)
    indexed by path (a reopened file keeps its blocks), read-ahead that grows
    when access is sequential (up to 1 MB), direct reads for large requests,
    `pread`/`__read_chk`/`__pread_chk` also go through the cache, no lock
    held during SD accesses. Per-file statistics in `debug.log`
    (`[io] cache:` / `[io]   <file> …`) and a `[io] slow SD read` line
    beyond 100 ms. Tested off-console with an 8-thread load test under
    AddressSanitizer against the reference file.
    Settings: `[io] cache_mb`, `block_kb`, `readahead_kb`.

35. **Loading screen** (`nx_splash.c`, `[video] loading_screen`, default 1).
    During frame 1 (~23 s on console), the thread that holds Unity's GL
    context waits in our futex shim. There, we switch it to **our own** EGL
    context (same surface, same thread), draw, present, and restore Unity's
    context: Unity's GL state is never touched, and only between the creation
    of its surface and the end of frame 1, before any Unity frame. Original
    content (gradient, title in a pixel font, a wheel of 8 squares in the
    style of the game's "Connecting…", a progress bar calibrated on the
    amount of data read during the previous launch, `.loadstats`).

36. **Switch keyboard** (`nx_kbd.c`, `[input] keyboard`, default 1).
    `UnityPlayer.showSoftInput(...)` (called by `TouchScreenKeyboard.Open`,
    hence by TMP_InputField) opens swkbd in a separate thread; the render
    loop hands the text back to Unity through `nativeSetInputString` then
    `nativeSoftInputClosed` (or `nativeSoftInputCanceled`). Without a reply,
    Unity's "virtual" keyboard stayed open and TMP copied its (empty) text
    into the field every frame, which also canceled typing on the game's
    on-screen keyboard.
    Safeguard: the game's on-screen keyboard (`OnScreenKeyboard.Update`)
    calls `ActivateInputField()` again every frame as long as the field does
    not have focus, and TMP gives the focus back as soon as the soft keyboard
    closes; swkbd would therefore have reopened endlessly. In mode 1
    (default), swkbd only opens if a button (other than the D-pad) or a new
    touch is less than one second old and was not seen within the 250 ms
    following the closing of the previous one. Other calls are put on hold:
    Unity then believes its keyboard is open (TMP keeps the focus and stops
    asking), so the next A or touch (less than a minute later, unless
    `hideSoftInput` comes in between) opens swkbd for that field, with the
    text kept up to date by `setSoftInputStr` (cases: field reached with the
    D-pad via `TMP_InputFieldFixer`, or A on the game's keyboard after
    swkbd). Mode 2: on every call; 0: never. `setSoftInputStr`/`hideSoftInput`
    are logged (first 16) for diagnosis.

37. **Loading screen with images** (v0.1.10). The drawing code moved to
    `nx_splash_draw.c`, pure GLES2, no libnx: the same code runs on a PC
    (`tools/splash_preview/`, software Mesa) to check the rendering frame by
    frame without a console. A single shader: color, tinted texture,
    antialiased rounded rectangle (signed distance). Everything is a function
    of time, never of the frame count: the screen only gets a frame when
    Unity's GL thread is waiting, hence at irregular intervals.
    The images (studio logo, game logo, 5 screenshots, .nro icon) are
    **downloaded at build time** by `tools/fetch_splash.py` (list in
    `tools/splash_sources.txt`) and are never in the repo: same status as the
    game data (private artifacts). Conversion: screenshots at 1280x720
    (cropped), baseline JPEG (stb_image does not read progressive JPEG); SVG
    logo rendered by `rsvg-convert`; 256x256 icon. On console, a low-priority
    thread decodes them (stb_image, NEON) and the loop uploads at most one
    per frame to the GPU: the screen shows up immediately and the images
    arrive afterwards.
    At the end of loading: the thread is stopped, then `eglDestroyContext` on
    our context, which frees the textures (~15 MB). Guard: `done` is set
    before waiting on `busy`, and a tick sets `busy` before reading `done`
    (sequential ordering), so no tick can use the destroyed context.
    Pixel-art logos (Infenek, font): integer scale in screen pixels, to stay
    crisp at 720p as well as 1080p.

38. **GPU / shader measurement** (`nx_gpu.c`, v0.1.13). Unity resolves all
    its GL functions through our `dlsym`: an `nx_gpu_exports` table (checked
    first) times `glCompileShader` (mesa's GLSL), `glLinkProgram` (linking +
    GLSL→TGSI), draws, textures, buffers, `glMapBufferRange` and
    `glClientWaitSync`. Inside mesa and libdrm (statically linked), calls
    between object files go through `ld --wrap`: `nvc0_program_translate`
    (nouveau code generation), `nvc0_program_upload`,
    `nvc0_screen_resize_text_area`, and on the libnx side
    `nvMapCreate/Close`, `nvAddressSpaceMap/Unmap`, `nvGpuChannelCreate/Close`,
    `nvioctlChannel_KickoffPb`, `nvFenceWait`, `nvInitialize`. Nested time is
    counted only once (per thread). A frame slower than `[debug] hitch_ms`
    (100) produces a detailed `[hitch]` line; every 2 s, a `[gpu]` line.
    libdrm_nouveau's infinite fence waits are split into 500 ms slices (same
    result): beyond one second, the log names the fence and reads the error
    notifier of each GPU channel (MMU fault, idle timeout, PBDMA error…). The
    watchdog heartbeat shows the GPU state (nvmap objects, channels, wait in
    progress).

39. **Console freeze: two plausible causes neutralized, the rest
    instrumented.**
    - *nouveau's shader code segment*: mesa 20.1 creates it at 512 KB and, on
      each overflow, evicts all shaders, reallocates twice the size (1, 2, 4,
      8 MB) and re-uploads the bound shaders; at 8 MB it evicts everything on
      every overflow. The code (`nvc0_program.c`) carries a comment about GPU
      page faults at the end of the segment, and games crash on
      "out of code space, evicting all shaders" (nouveau bugzilla). The 2nd
      match loads new shaders: a suspicious moment. `[video]
      shader_code_mb` (16): on the first overflow, jump straight to 16 MB
      (the internal call that creates the 512 KB segment cannot be
      intercepted). Each resize is logged.
    - *nvservices transfer memory*: libnx gives 8 MB (fincs raised it from 3
      to 8 MB in 2018 for mesa); nvservices stores its objects for this
      process there. `[debug] nv_tmem_mb` (32), automatic fallback to 8 MB if
      nvservices refuses. Taken from the heap (≈520 MB free).
    - Not kept without proof: 60 fps, SD cache, loading screen (its context
      is destroyed at the end of loading, logged).

40. **Compilation hitches: no cache possible with the current mesa.**
    Checked in the devkitPro sources (`switch-20.1.0-rc3`): `meson.build`
    disables the shader cache for `horizon`, and `st_extensions.c` only
    exposes `GL_OES_get_program_binary` if the disk cache exists — so neither
    the mesa cache nor Unity's program binary cache. What was done:
    measurement (38), CPU at 1785 MHz during compilations
    (`[video] compile_boost`), adjustable nouveau optimization level
    (`[video] nv_shader_opt`). What would really remove the hitches:
    rebuilding mesa with the cache enabled and simple storage on the SD
    (proposed, not done: to be validated).

41. **Loading screen from the 1st second** (`nx_splash.c`,
    `[video] early_splash`). Before Unity creates its EGL surface (~4.3 s on
    console), nobody uses the window: black screen.
    *1st attempt, abandoned*: logo drawn into the libnx framebuffer, then
    `framebufferClose()` just before Unity's surface. Under Eden, Unity's
    first frame never came back (clean A/B): two successive producers on the
    single `NWindow` break its buffer queue — already seen on console with a
    2nd EGL surface, hence the singleton surface in `imports.c`.
    *Chosen version*: a single producer. On the 1st boot, the
    `eglCreateWindowSurface` wrapper records the `EGL_CONFIG_ID` chosen by
    Unity in `mvlo/.eglconfig`. On later boots, right after video init,
    `nx_splash_early_start` creates THE window surface with that config (mesa
    requires the same config for context and surface), our GLES2 context, and
    a thread draws the loading screen. When Unity asks for its surface, the
    thread stops, releases the context, and Unity gets the same surface; the
    usual ticks then continue with the same context, the same textures and
    the same clock (no visible jump). If Unity asks for another config
    (should only happen after a game update), the surface is destroyed then
    recreated — a risky path (2nd surface), but `.eglconfig` is erased and
    the next boot is back to normal. The very 1st boot after installation
    stays black for 4 s.

42. **`NullReferenceException` in `UI/Navigate` (`canceled`) at the start of
    the 2nd match.** `OnScreenKeyboard` (the game's keyboard) subscribes to
    Navigate when it opens and only unsubscribes in `Close()`. When it is left
    open as a match starts — common after using the Switch keyboard, which
    leaves it behind (room chat) — the destroyed object keeps receiving
    `canceled`. Fixed twice: `prepare_project.py` adds the unsubscription in
    `OnScreenKeyboard.OnDisable` (and in `PauseMenuManager.OnDestroy`, same
    pattern), and `NxInput` closes the game's keyboard as soon as swkbd
    returns its text (`nxbridge_kbd_closed`).
43. **`stat()`/`access()` answered from the directory listings
    (`nx_statcache.c`, v0.1.15-0.1.17).** libnx `fsdev_stat` costs 5 round
    trips to the file service (~8.6 ms in `Data/`, 1570 files) and Unity
    makes ~1700 of them when opening *Room Settings*, more than half of them
    on missing files. The data under `assets/` and `il2cpp/` does not change
    during a session: each directory is read once (`fsDirRead`), then size,
    type, dates and "missing" come from memory. Paths are normalized before
    the lookup (`//`, `/./`, `/../`: v0.1.15 missed everything because of
    them), names are compared case-insensitively, every read failure is
    logged, and the rest (saves, `files/`, logs) goes through libnx as
    before. A write/delete invalidates the directory.
    `[io] stat_cache = 0` turns it all off. Rejected: an index built at
    startup (reading 1570 entries for nothing if the menu is never opened)
    and the v0.1.13 cache (`nxio`, read data, not metadata).
44. **UDP heartbeat (`[debug] netlog`, v0.1.18-0.1.19).** Whole-console
    freezes stop the render loop, the watchdog and SD writes all at once:
    `debug.log`/`stall.log` end before the freeze.
    A libnx thread (priority 0x28-0x2C, core 2) sends a datagram to a PC
    twice per second: frame, idle time, GPU wait, file operation and network
    call in progress, blocked threads; every 2 s, SoC temperature (`ts`),
    memory blocks, system resource, CPU boost mode. If the datagrams keep
    coming during a freeze, the process is alive and they tell where it is
    stuck; if they stop, it is the console.
    PC listener: `tools/switch/netlog_listen.py`, or `.php` under `php-cgi`
    when the Windows firewall blocks incoming python.exe. Empty = off.
45. **GC signals delivered on behalf of a thread stuck in game code
    (`nx_signals.c`, `[debug] gc_force_ms`, v0.1.20).** bdwgc's
    stop-the-world waits for each thread to run its SIGPWR handler; here it
    only runs at our shims' safe points. A thread spinning in managed code
    without system calls (Quantum `TaskContext.EndFrame`, busy-waiting of its
    workers… parked by that same GC) blocked everything.
    Checked in Mesosphère: `svcSetThreadActivity(Paused)` is allowed on
    another thread of the process and only returns once the thread is off
    every core (`KThread::SetActivity`); `svcGetThreadContext3` requires that
    pause; there is no "SetThreadContext" outside debugging. So, after
    `gc_force_ms` (30 ms): a "forcer" thread claims the thread's record
    (CAS: it can no longer deliver on its own), pauses it and reads its
    context. If the PC is not inside a game module (it is in our shims,
    libnx, newlib: it may hold one of OUR locks, and will reach a safe point
    anyway), it is released as is. Otherwise a proxy thread copies x0-x30,
    PC and q0-q31 below the paused thread's SP, switches its own stack to
    just below them (`nx_run_on_stack`), takes over its TLS register (bionic
    block) and impersonates it (imported `pthread_self` →
    `nx_sig_pthread_self`), then runs **bdwgc's real handler**.
    For bdwgc this is exactly a signal frame: `GC_approx_sp()` lands on the
    thread's stack, below its registers, and the collection scans it
    (Unity-Technologies/bdwgc sources, `pthread_stop_world.c`: on aarch64
    the handler relies on the signal frame for the registers, it does not use
    `context`). The handler waits for the resume signal in `sigsuspend` (on
    the proxy), returns, and the thread is resumed where it was. Rejected:
    (B) hooking the Quantum loop methods — only fixes this case; (C)
    single-threaded Quantum simulation — CPU cost and android-build, kept in
    reserve. Test: `smoke_test = 3` now has a 5th thread that spins in
    assembly without any call (5/5 runs under Eden with the fix, missing ack
    and a 3 s wait without it); `gc_spin_test = N` does a real
    `il2cpp_gc_collect` at frame N while a thread attached to IL2CPP spins.
46. **Independent heartbeat and liveness markers (`nx_bfmt.c`, `nx_kmon.c`,
    `nx_alive.c`, v0.1.20).** The heartbeat was not self-contained: newlib's
    `%.3f` goes through dtoa → Balloc → `_calloc_r` (the malloc lock), and
    `sendto` is an IPC to the bsd module. The whole heartbeat (and the
    beacons it prints) goes through `nx_bfmt`, an allocation-free integer
    formatter; slow (> 50 ms) or failed `sendto` calls are counted in the
    next datagram. Additions: GC state (parked threads, since when, who has
    not acknowledged, forced deliveries), memory-mapping kernel calls
    (`svcMapMemory`, `svcMapPhysicalMemory`, `svcSetMemoryPermission`,
    `svcSetHeapSize`, …) and nv ioctls (last, slowest, in progress) via
    `ld --wrap` — including those made by libnx itself. Without network or
    SD: `[debug] stall_rumble_s` (12 s) — game frozen after startup → the
    controller rumbles twice every 5 s as long as the process is running (a
    libnx thread that only reads two counters and talks to the HID service).
47. **Data files kept open (`[io] keep_open`, v0.1.20).**
    libnx `fsdev_open` = `OpenFile` + `GetFileTimeStampRaw` (checked in
    `fs_dev.c`: the dates are only used by `fstat`, which the listing cache
    already answers for these files). Unity keeps closing and reopening the
    same files. Closing a read-only file under `assets/` or `il2cpp/` (the
    listing cache's domain) keeps the descriptor; the next open of the same
    path takes it back, rewound (`lseek` SET: no IPC). Cap of 64 (the oldest
    one is really closed; newlib allows 1024: 0x2000-byte `handles` table in
    libsysbase); an open that fails releases everything and retries; a
    write, a rename or a delete of the path closes its kept descriptors.
48. **"Basename" fallback served from a listing (`libc_shim.c`, v0.1.21).**
    A stat or open that the listing cache declares "missing" (≈ 60% of
    stats) then went through `basename_fallback`, which did a real libnx
    `stat()` of the basename in the game folder: at least one FS round trip
    per "missing" stat, hence 0.3-0.46 ms per stat "served from the cache"
    on console. Remembering missing names was not enough (Eden: ~1400 real
    stats at startup, mostly different names). The game folder is small: it
    is listed once (case-insensitive name hashes, fixed table) and a basename
    that is not in it is missing without an FS call. The list is rebuilt
    after a creation in that very folder (open or `fopen` for writing,
    rename, `mkdir`); Unity's saves and files in subfolders do not affect it.
49. **`keep_open`: measured policy (v0.1.21).** On console the pool of 64
    stays full (1016 set aside, 217 reuses). Two policies tried under Eden,
    at startup: keep only the files already opened twice → 38 reuses (the
    1st reopening of each file misses); close the least often opened first
    → 112. The plain LRU of v0.1.20 did 216: kept. The FS service's open
    limit is not documented anywhere (the `OpenCountLimit` code exists, not
    its value): 64 by default, and if an open fails until the pool is freed,
    the pool shrinks by itself (logged).
    The report also counts closes of files opened only once, to judge
    whether a larger pool is worth it on console.
50. **Freeze autopsy from the rumble thread: not done.** The code (pausing
    each thread, reading its context, service probes) was interrupted twice
    by the cloud session's safety filter; none of it was kept. To be done in
    the local session if it is still wanted. Same reason for the sampling
    profiler.
51. **Freeze autopsy (`nx_autopsy.c`, v0.1.22, local session).** Done
    locally in the end. (a) A log of blocking system calls, one slot per
    thread, filled by `ld --wrap` (`svcSendSyncRequest`,
    `svcWaitSynchronization`, `svcArbitrateLock`,
    `svcWaitProcessWideKeyAtomic`, `svcWaitForAddress`): call, session or
    lock owner, since when, caller. Necessary: for a thread stopped in a
    system call, `svcGetThreadContext3` returns neither x0 nor the volatile
    registers, and its PC is the `svc` instruction itself (seen on console).
    (b) At the freeze, the rumble thread (the only one alive) writes for each
    thread: log, context, stack; then the process's service sessions, the
    beacons, the last 12 heartbeats, and the result of probes — one small
    call per service (SD, bsd, time, nifm, am, nvdrv, ts), each in a thread
    created in advance. (c) Output to `autopsy.log` through an `FsFile`
    opened in advance (without newlib) and over UDP to `netlog`; a long
    rumble = written to the SD, three short ones = not. No lock or
    allocation once armed (`nx_bfmt`). Tested with `fake_stall_frame`, then
    on the real freeze. Rejected: a dump from the watchdog (it is itself
    stuck in the file service).
52. **Automatic exit after a freeze (`stall_exit_s`, 90 s).** A separate
    thread that only sleeps and reads the frame counter (the rumble thread
    itself ends up stuck in the hid service). Time spent suspended (HOME
    menu, sleep mode) does not count: a 1 s sleep that took more than 2.5
    resets the counter. Direct `svcExitProcess`, without cleanup (newlib may
    be stuck). On console: the "The software was closed because an error
    occurred" screen, then the console is back to normal right away — that
    is what proves the freeze comes from our process.
53. **Console memory pools in the heartbeat, `[net] poll_slice_ms`.**
    `svcGetSystemInfo` (app, applet, system, sysunsafe) every 2 s: nothing
    moves until the freeze. `poll_slice_ms` (10 by default, 1-100) sets the
    slice length of socket waits; at 40 the rate of bsd calls did not drop
    (game traffic dominates), so it does not yet tell timer and call count
    apart. Left at 10.
54. **bsd requests measured and network switches (v0.1.23).**
    `nx_bsdmon.c` wraps (`ld --wrap`) all the `bsd*` functions of libnx
    (libnx's newlib layer and `nx_net.c` go through them): per command,
    count, failures, bytes, time, and how many requests were **mapped**;
    plus a ring of the last 128 operations (thread, command, socket, type,
    peer, size, flags, result/errno, duration).
    Checked in libnx (`cmif.h`, `cmifRequestIn/OutAutoBuffer`): an
    "AutoSelect" buffer is copied if it fits in the service's *pointer
    buffer*, otherwise the kernel maps our memory into the service; bsd's
    *pointer buffer* is logged (0x8000 under Eden). Our `FIONREAD` fallback
    (`.NET Socket.Available`) read 64 KB with `MSG_PEEK`: always mapped.
    `[net] small_ipc = 1` keeps every request below that size (TCP off, UDP
    receive capped, `FIONREAD` fallback reduced); `socket_config = 1`
    (default libnx buffers, `sb_efficiency` 4 instead of 8: bsd transfer
    memory ≈ 2.2 MB instead of 4.4); `bsd_sessions` (1-8, default 3; the
    switchbrew wiki gives 15 `bsd:u` sessions for the whole console since
    18.0.0); `poll_slice_ms = 0` (one request per wait, at most 1 s for the
    GC). All set to the old behavior by default.
    Process resource limits in the heartbeat (`rl:`, every 2 s): Mesosphère
    `GetInfo(InfoType_ResourceLimit)` gives a handle, read with
    `svcGetResourceLimitCurrent/LimitValue` (threads, events, transfer
    memory, sessions, memory). A counter that rises with time spent online
    would point to a kernel-object leak.
    Read in Mesosphère (`kern_k_process.cpp`, `kern_kernel.cpp/.hpp`): an
    application process without its own system resource (`sysres=0`, our
    case) takes its memory blocks from a pool shared by the applications
    (`ApplicationMemoryBlockSlabHeapSize` = 20 000) and its page tables from
    **the same heap as the system processes** (`s_page_table_heap`), with no
    quota. A lead, not proof: nothing shows yet that we are exhausting it.
55. **Bundling the small files of `Data/`: postponed.** ~250 opens of
    4.5 ms each per Room Settings, of files opened only once (hence out of
    reach of `keep_open`). Lead: a pack built by `package_sd.sh` (files
    < 64 KB, index + data) and "in-memory file" descriptors served by
    `fakefd.c`. But several paths read a descriptor directly with newlib
    (file `mmap`, `pread`, `lseek`, `fstat`, the `nx_io.c` block cache): all
    of them must be hooked up, and testing it requires new SD data. Not done
    in the online-freeze session (v0.1.23).
56. **`[debug] bsd_flood`: the number of requests is not the cause (v0.1.24,
    local session).** A thread (`nx_net.c`, `flood_main`) sends N bsd
    requests per second from frame 300 on, on a UDP socket of its own:
    `bsd_flood_op = poll` (0 ms poll), `sockopt` (`getsockopt`) or
    `pollwait` (back-to-back 5 ms polls, like the Photon receive thread;
    rate ignored). On console, 2000/s 0 ms polls on the main menu (without
    Photon): 400 000 requests in 4 min, no freeze, whereas the online
    freeze came at ~162 000. Then a freeze in "Play" at 85 000 requests.
    `pollwait` has not been tried on console yet.
57. **`[net] local_wait`: does not fix the freeze (v0.1.25, local session).**
    With 1, `bsd_poll()` replaces every bsd poll with a timeout (waits in
    `poll_fake` and `wait_fd`) with 0 ms polls and 1 ms sleeps on our side
    (`nx_sig_sleep_ms`): no more waiting inside the service. Goal: test
    whether the cumulative wait time in bsd (≈ connected time, the Photon
    thread waits 5 ms in a loop) was the cause. Froze anyway after 409 s
    connected, the Photon thread stuck 13.2 s on a 0 ms poll. Kept, 0 by
    default (~1000 requests/s while waiting, no useful effect).
58. **Hunting the ~400 s online freeze (Oct 30, cloud session, 4 sub-agents).**
    Sources read: libnx code (`socket.c`, `bsd.c`, `nifm.c/.h`),
    Atmosphère/Mesosphère code (clone at commit `6b0c077b`), the game's
    Photon sources (clone `ipodtouch0218/nsmb-mariovsluigi`: Quantum 3.0.13,
    Realtime 5.1.15), switchbrew (via search; the host is blocked by the
    proxy), GitHub issues, GBAtemp. **switchbrew.org,
    forum/doc.photonengine.com and gamebrew.org cannot be read directly in
    the session** (to be allowed if we want the pages verbatim).
    (a) **Photon has no event at ~400 s.** All its timers: service 10 ms,
    fallback thread 50 ms, RTT ping 1 s, AppStats ~5 s, LobbyStats 60 s,
    background keepalive 60 s (`ClientServiceInactivity`), DisconnectTimeout
    10 s, auth token **1 h**. `ChecksumInterval = 0` (disabled) and Quantum
    checksums off. `NetworkHandler.cs` rebuilds the `AppSettings` on
    connect: **pure UDP, `AuthMode=Auth` (per server, not WSS), LobbyStats
    ON** — it overrides `PhotonServerSettings.asset`. So an incoming
    LobbyStats event ~every 60 s and an outgoing ping ~every 1 s (~400
    `sendto` on the connected socket in 400 s), but **nothing at 6 min 40**.
    A forum report describes a frame freeze on `socket.Close()` under IL2CPP
    (explains "everything resumes when the socket is closed", not the origin
    at 400 s). Conclusion: the trigger is not a Photon application event.
    Sources: doc.photonengine.com/quantum/v3/manual/game-session/reconnecting,
    /realtime/.../appandlobbystats, /server/v5/operations/debugging;
    `Assets/Photon/PhotonRealtime/Code/{ConnectionHandler,AsyncExtensions,
    Enums,RealtimeClientVersion}.cs`, `Assets/Scripts/Networking/NetworkHandler.cs`.
    (b) **Why bsd goes down first, then nifm.** The `bsdsocket` process
    (TID 0100000000000012) hosts bsd:u/s/a, bsdcfg, nsd **and sfdnsres (the
    DNS resolver)**. **nifm (TID …0F) is a separate process, but its
    connection test and its internet operations go through bsdsocket**
    (libcurl + socket/ssl + DNS). So when bsdsocket stops responding, every
    nifm operation that calls it blocks in turn. **fs (a separate process,
    built-in, no network) freezing too is NOT explained by the network** →
    a cross-cutting cause (CPU core, kernel resource), or an artifact of our
    own shared fsdev session saturated by our `debug.log` writes (to be
    confirmed: does the autopsy's fs probe open its own session?).
    Sources: switchbrew "Sockets services", "Network Interface services",
    "Boot2"; libnx `nifm.h`.
    (c) **`socketInitializeDefault` does NOT call nifm** (checked in
    `socket.c`: only `bsdInitialize`). Almost no homebrew creates/submits a
    nifm request (`nifmCreateRequest`/`nifmRequestSubmit`): that is the norm,
    including for the most demanding real-time UDP apps. So "not making a
    nifm request" is probably **not** the cause → the nifm fix lead is
    **downgraded**, not coded. The closest analogue, **chiaki-ng** (PS
    Remote Play, long-running UDP), uses udp_tx/rx 0x40000,
    `sb_efficiency 8`, **`num_bsd_sessions 16`**, no nifm; ours: udp_rx
    0xA500, 3 sessions. **sys-ftpd #30** (the closest documented analogue of
    a "network → system hang"): buffer/socket exhaustion (`ENOBUFS`, sockets
    in TIME_WAIT) → "promptly hangs the system". **RetroArch Switch**
    freezes "after 10-15 min" in **applet mode** (launched from the album)
    and not in **application mode** (title-takeover) → free test added
    (TEST 4 octies, run 2). Sources:
    github.com/chiaki-ng/chiaki-ng switch/src/main.cpp, cathery/sys-ftpd#30,
    gbatemp.net/threads/retroarch-switch.492920.
    (d) **Limits.** Since 18.0.0, bsd:u = **15 sessions for the whole
    console** (0x40→0x24 in 16.0.0→0x0F in 18.0.0); bsd:u has 22 IPC
    threads. The FreeBSD `slowtimo`/`fasttimo` timers are **TCP**: a UDP
    socket is not subject to them (a TCP callout leak is unlikely for pure
    UDP).
    (e) **Mesosphère scheduler (confirmed in the code).** Strictly
    priority-based **per core**, **no time-sharing between different
    priorities**: a higher-priority thread that never sleeps starves a lower
    thread **on the same core** *indefinitely* (round-robin only between
    equal priorities, at levels 59 on cores 0-2 / 63 on core 3, via core 3's
    "DPC preemption thread" every 10 ms). `svcSendSyncRequest` **has no
    timeout**: a client waits forever if the server does not respond.
    **FS: `default_cpu_id = 3`, priority 45** (`stratosphere/fs/fs.json`).
    **nx-hbloader: priority 44, cores 0-2, priorities 28-59, applet mode
    (`application_type=2`)**; it **maps the NRO into its own process** → we
    inherit the core mask, resource limit and system resource of the host
    (hbloader, or the Album applet in applet mode). A `sysres=0` process
    takes its KMemoryBlock objects from an **application** pool (20000)
    separate from the **system** pool (10000); exhausting a slab returns
    `ResultOutOfResource` **on the SVC** (not a silent freeze), and a single
    socket + poll() **does not fragment** memory. Sources: Atmosphere
    `kern_k_scheduler.cpp`, `kern_k_dpc_manager.cpp`, `kern_kernel.cpp/.hpp`,
    `kern_k_process.cpp`, `kern_k_client_session.cpp`,
    `kern_k_server_session.cpp`, `stratosphere/fs/fs.json`,
    `switchbrew/nx-hbloader/hbl.json`.
    (f) **Honest conclusion.** Pure CPU starvation would be **immediate**,
    not at 400 s; no kernel object, session or thread leak is found for a
    single socket. **No simple mechanism alone explains the ~400 s delay.**
    At the freeze, we need to measure whether bsd/fs/nifm are *runnable but
    not scheduled* (starvation) or *blocked in IPC* (DECISIONS 59), and
    decide between network stack / Photon / applet mode by experiment (TEST 4
    octies). With FS on core 3 and us on 0-2, direct CPU starvation of FS by
    us is unlikely: the "a system service runs away" hypothesis remains to
    be proven.

59. **Spec for the system-load diagnostic at the freeze + candidate fixes
    (for the LOCAL session; not coded in the cloud, safety filter).** As in
    DECISIONS 50, code that pauses/reads the context of ANOTHER process is
    likely to be cut off by the cloud session's filter; written here as a
    spec, to be implemented locally.
    **Goal:** at the freeze, tell whether bsdsocket/nifm/fs are *runnable
    but starved* (CPU starvation) or *blocked waiting* (IPC/inversion). This
    is THE tie-breaker the research leaves open (DECISIONS 58f).
    **Proposed method:**
    - get the services' pids via **pmdmnt** (`pmdmntGetProcessId` on the
      TIDs 0100000000000012 bsdsocket, 010000000000000F nifm, 0100000000000000
      fs; nvdrv 0100000000000019 and am 0100000000000023 as controls);
    - `svcDebugActiveProcess(pid)` then, in a loop ~5× during the freeze,
      `svcGetDebugThreadParam`/`svcGetThreadContext3` to **list, per thread,
      the PC, the priority, the current core (`svcGetThreadCoreMask` / the
      context field) and the state** (runnable vs waiting). A moving PC = the
      service is running (starvation elsewhere); a PC stuck in a waiting
      `svc` = blocked.
    - output to `autopsy.log` + UDP, in the same format as the existing
      autopsy, **without touching** the pause/context part of `diag.c` or
      the `--wrap` log of `nx_autopsy.c`.
    - **Does hbloader's NPDM allow `svcDebugActiveProcess`?** To be checked
      (capability `DebugFlags`/`debug=1`). Otherwise, fallback: `svcGetInfo`
      (`InfoType` per-core CPU time, if exposed) to see whether system CPU
      time rises (starvation) or stalls (blocking).
    **Candidate fixes (specs, not coded; each behind a toggle, safe
    default):**
    - `[net] poll_yield_ms`: a forced `svcSleepThread` in the poll loop of
      the Photon receive thread (tests CPU starvation: if the freeze goes
      away, it was our loop starving a core);
    - lowering the priority of the threads WE create is already done (59,
      the lowest allowed); the Photon thread's priority is set by IL2CPP (we
      could lower it via `svcSetThreadPriority` on that known thread);
    - `[net] socket_renew_s` (last resort, risky): substitute a new bsd
      socket under the same fd before ~6 min. **The source port changes** →
      the Photon server sees a new flow: only to be tried with the
      `photon_ring` capture active, to see how the server reacts. Probably
      useless if the `udp_soak` test shows that Photon is not at fault.

60. **Instrumentation shipped (v0.1.26, cloud session): Photon ring +
    `udp_soak` + PC echo.** Both safe (no pausing/reading of context), off
    by default, clean build, Eden self-test OK (boot, 57 keys, BSD up,
    selftest).
    - **`[debug] photon_ring`** (`nx_net.c`): ring of the last 256 UDP
      datagrams, captured at the application level (real Photon payload,
      before `ipc_cap`), in both directions, with a millisecond timestamp,
      size and first 16 bytes. `nx_net_photon_report()` is called in the
      autopsy (30 s window) and every 1800 frames in `debug.log`. Static,
      relaxed atomics, no lock/alloc. Goal: see exactly what goes over the
      wire in the seconds before the freeze (burst? command type? silence?).
    - **`[debug] udp_soak = ip:port` / `udp_soak_connect`** (`nx_net.c`): two
      threads mimic Photon traffic WITHOUT Photon — connected raw libnx
      socket (or `sendto` if `=0`), ~20 datagrams/s + a receive thread that
      polls 5 ms — towards a UDP echo on the PC, from frame 300 (main menu).
      **The decisive experiment**: a freeze on the main menu after ~400 s of
      this traffic = it is the console's network stack, not Photon.
    - **`tools/switch/udp_echo.php`**: PC-side UDP echo in PHP (the Windows
      firewall blocks inbound python.exe but lets php-cgi through), modeled
      on `netlog_listen.php`; sends back every datagram and logs throughput
      and silences (the PC time of the last line dates the freeze).

61. **Research on fixes (Oct 30, overnight session 2, cloud session):
    sys-ftpd #30 and chiaki-ng.** Sources read live
    (raw.githubusercontent.com; GitHub MCP and switchbrew blocked by the
    proxy).
    (a) **sys-ftpd #30: ENOBUFS tied to ACTIVITY, not to time.** The freeze
    ("socket: 105 No buffer space available" → "hangs the system", system
    RAM 8→3 MB) happens during **active TCP transfers** with buffers that
    are too small, not on an idle socket. Fix in the jakibaki fork:
    `SOCK_BUFFERSIZE 16384`, TCP buffers 8×/16× the base,
    **`sb_efficiency = 8`** (hence ~2× the RAM: the tension in the thread is
    "enlarge the buffers" vs "do not starve the other sysmodules of system
    RAM"). The **transfer memory is per client, fixed, allocated once**:
    `tmem = sb_efficiency ×
    pageround(tcp_tx_max+tcp_rx_max+udp_tx+udp_rx)` (libnx `bsd.c`), and on
    the service side `sb_mbmax = size × sb_efficiency` per socket (FreeBSD
    `uipc_sockbuf.c`). ENOBUFS = failure to allocate an mbuf on send
    (`M_PREPEND … M_NOWAIT` → `ENOBUFS`, `udp_usrreq.c`). **libnx says the
    bsd service uses LINUX errno values** (`socket.c _convert_errno`:
    "Nintendo actually used the Linux errno definitions"), so 105 = ENOBUFS.
    Sources: github.com/cathery/sys-ftpd/issues/30, /pull/29,
    github.com/jakibaki/sys-ftpd/blob/master/source/ftp.c,
    switchbrew/libnx `services/bsd.c`, `runtime/devices/socket.c`,
    freebsd-src `sys/netinet/udp_usrreq.c`, `sys/kern/uipc_sockbuf.c`.
    (b) **Verdict on the hypothesis "an idle connected UDP socket rots
    within 400 s": not supported by the sources.** Reception is capped at
    `SO_RCVBUF`, then **dropped** (`sbappendaddr_locked==0 → m_freem`,
    counter `udps_fullsock`); an ICMP error = **a single `so_error`** (no
    mbuf queue like Linux's `MSG_ERRQUEUE`); `poll()` holds on to no mbuf;
    tmem is fixed. An ENOBUFS-then-freeze at ~400 s would rather come from a
    **socket/fd or TIME_WAIT leak** (a climbing count → accumulated
    `sb_mbmax` reservations) or from **system RAM starvation** (the
    mechanism of #30's global freeze). Hence the instrumentation added
    (DECISIONS 62): errno per command **and the count of
    opened-but-not-closed sockets** in the heartbeat (tests the leak).
    (c) **chiaki-ng (long-running real-time UDP analogue)**: exact socket
    config = udp_tx/rx **0x40000**, tcp 0x8000/0x10000 (max 0x40000),
    **`sb_efficiency 8`**, **`num_bsd_sessions 16`**, bsd:u, **no nifm**
    (`switch/src/main.cpp`). The libnx maximum for `num_bsd_sessions` =
    **16** (`NX_SESSION_MGR_MAX_SESSIONS`, `sessionmgrCreate` rejects more);
    the console only has **15 bsd:u sessions** (fw 18.0.0+) → 16 may fail
    at init (chiaki does not check the return value; we do, with a fallback
    16→15→8→3). magicTupper (remote play): **independent** UDP sockets,
    non-blocking, `num_bsd_sessions 6`, "drop rather than block/starve".
    Moonlight: asks for a large `SO_RCVBUF` but **tolerates refusal**
    (step-down). Sources: github.com/chiaki-ng/chiaki-ng,
    nemerpus/magicTupper, moonlight-stream/moonlight-common-c
    `src/PlatformSockets.c`.

62. **Candidate fixes + errno instrumentation (v0.1.27, cloud session).**
    All behind a `[net]`/`[debug]` toggle, safe default; clean build, Eden
    self-test OK (chiaki config applied, 16 sessions accepted under Eden,
    `udp_unconnected` active, no crash). Default off = `redirect` 0
    everywhere = zero change in behavior.
    - **`[net] chiaki_cfg`**: chiaki-ng's socket config (udp 256 KB,
      `sb_efficiency 8`, 16 sessions forced, progressive fallback if the
      console refuses — the `[net] BSD sockets up … N bsd sessions` log
      gives the real N). Tests a lack of buffers/sessions.
    - **`[net] udp_unconnected`**: on a UDP socket, we **do not** do
      `connect()` at the bsd level; we keep the peer (`connect_fake`) and do
      `sendto(peer)` / `recvfrom` filtered by the peer (the game sees a
      normal connected socket: `getpeername` returns the kept peer). Tests
      whether a long-lived *connected* UDP socket is what rots in bsdsocket.
      **The most likely fix** if the "connected socket" lead holds.
    - **`[net] photon_rebind_s = N`**: after N s connected, a thread swaps a
      fresh bsd socket in under the same application fd through an internal
      indirection (`SockInfo.redirect`, translated in
      send/recv/poll/sockopt/…), with a new source port, and does a
      `shutdown` of the old one. **Risk**: Photon tracks the peer by source
      IP:port → it will most likely see a new flow and disconnect (~10 s).
      No `dup2` (libnx's socket devoptab has no `dup`): hence the
      indirection. Only tests whether a new socket resets the 400 s clock.
    - **bsd errno per command + open sockets** (`nx_bsdmon.c`): histogram of
      the service's errno values (ENOBUFS, ECONNRESET…; Linux numbering per
      libnx, with a FreeBSD safeguard for ENOBUFS) in the heartbeat
      (`err: ENOBUFS=N …`, only if non-zero) and in the autopsy (full
      table); plus `N sock open` (sockets - close + accept) in the
      heartbeat — a climbing count = an fd/socket leak (lead #30).
63. **The ~400 s online freeze: `/dev/urandom` running out of file (Nov 2,
    local session, v0.1.29 → fixed in v0.1.30).** `open_fake` emulated
    `/dev/urandom` (and `/dev/random`) with a 64 KB file of random bytes
    written to the SD card, then returned a real fd on that file. IL2CPP
    keeps that fd; Photon draws ~160 bytes/s from it while connected; at
    ~400 s the file is exhausted, `read()` returns 0 and
    `il2cpp::os::Cryptography::FillBufferWithRandomBytes` loops forever:
    ~7 000 `fsFileRead`/s. fs (core 3, priority 45) saturates and starves
    the other services on core 3: bsd (the network of the **whole** console
    — sys-botbase, another process, no longer accepts connections, ping
    fails), nifm; `UnityMain` waits 12.8 s for an fs write → frozen frame.
    Everything resumes when our process dies. How we found it: a
    per-session IPC request counter (`nx_autopsy.c`, heartbeat
    `ipc/10s:`) showed flat rates up to the freeze, but 129 520 fs requests
    at the autopsy versus ~23 000 expected; the backtrace of the thread
    sending them ("IL2CPP Threadpool worker") went `read_fake` →
    `fsFileRead` ← `libil2cpp.so+0x2901ccc` = `FillBufferWithRandomBytes`
    (symbols from the CI artifact `mvlo-symbols`). **Fix**: `read_fake`
    serves the fds flagged in `g_fd_rand[]` with `randomGet()` (the file
    stays for `fstat`/`lseek`). Verified: "Play" for 11 min 40 with no
    freeze, fs requests down from 46/s to 7.5/s. What this invalidates: the
    network stack / connected socket / buffers leads (DECISIONS 58, 61, 62);
    the v0.1.27 toggles remain, off by default, with no effect on this
    freeze.
64. **Tools from the Nov 2 session.** `sys-botbase` v2.5 on the console
    (`atmosphere/contents/430000000000000B`, port 6000) and
    `tools/switch/sbb.py`: buttons, screenshots, `ping` — it also serves as
    a control (another process that uses the network).
    `tools/switch/freeze_watch.sh`: watches a "Play" run for line-by-line
    tracking (progress, heartbeat stop, autopsy, sys-botbase). `udp_soak`
    gains `udp_soak_layer` (through our socket layer), `udp_soak_dns` (DNS
    requests, Internet target 1.1.1.1:53), `udp_soak_hz`, `udp_soak_reuse`.
    `udp_echo.php` prints its throughput even under continuous traffic
    (before: nothing as long as `stream_select` did not come back empty).
65. **Sampling profiler (`[debug] prof_ms`, v0.1.31-33, local
    session).** A thread (`nx_prof.c`) checks the duration of the current
    frame every 4 ms; beyond `prof_ms`, it pauses UnityMain,
    `Loading.AsyncRead` and `Loading.PreloadManager` long enough to read PC,
    LR and the frame-pointer chain, then writes the samples to `prof.log`
    (`tools/switch/prof_report.py` names them using the `.elf` and the CI
    symbols). The thread's registration in the signal layer is held during
    the read (`nx_sig_sample_lock`, `in_delivery = 3`) so that it never
    crosses a forced GC delivery. Two crashes in v0.1.31-32 came from an
    integer overflow in the bound `fp + 16 <= hi` (FP = -16 passed it);
    fixed as `fp <= hi - 16`, in `diag.c` too. Off by default.
66. **First-time hitches: what was costly (Nov 2, console profiles).**
    *Room Settings* (2.1 s): 78% fs IPC on UnityMain, including
    `fsFsGetFileTimeStampRaw`, which `fsdev_open` sends after every open
    (27%), the open itself (23%), the reads (21%). Match start: UnityMain
    waits for the loaders, which do the same accesses. Room creation: IL2CPP
    CPU (generic method tables, GC). In a match: the 190-310 ms hitches are
    UnityMain in `SocketImpl::Send` → `bsdSend`.
    **Fixes**: (a) `--wrap fsFsGetFileTimeStampRaw`, answering "no
    timestamp" for `assets/bin/Data/` and `il2cpp/` (`[io]
    open_timestamps = 1` restores it); (b) preloading of the `Data/` files
    ≤ `[io] preload_kb` as **in-memory files** in `nx_io.c` (insert-only
    lists, lock-free reads, invalidated on write). Two traps: in the 64 KB
    block cache, the preload kept evicting itself; and the game opens its
    data by **relative path** (`assets/bin/Data/x`, cwd = game root):
    `path_ino` now canonicalizes (strips `sdmc:`, prefixes the root).
    Result: *Room Settings* 1.1 s, launch ~1.15 s.
67. **Asynchronous UDP sends (`[net] async_send`, v0.1.37, default 1).**
    `sendto_fake` copies a datagram ≤ 1 536 bytes into a 256-entry queue and
    returns "sent"; a send thread passes it to the bsd service (up to 3
    attempts on EAGAIN). A single thread keeps the order; a full queue or a
    large datagram = direct send as before; `close` of a UDP socket waits at
    most 200 ms for the queue to drain. Verified: 21 min of match, no frame
    ≥ 150 ms, no disconnection.
68. **Phantom descriptors (`[io] phantom_fds`, v0.1.38, default 1).** After
    preloading, *Room Settings* still spent ~0.55 s in fs opens/closes (~250
    files). A read-only open of a file in `assets/bin/Data/` that is present
    in memory (`nxio_ramfile_get`) returns a descriptor of our own,
    3072-4095, with no fs request; `read_fake`, `z_lseek`, `pread_impl`,
    `__pread_chk_fake`, `fstat_fake` (per-fd stat cache), `mmap_fake`
    (anonymous map + copy), `z_dup` (same fd, one more reference) and
    `close_fake` serve them. Measured: *Room Settings* 1.1 s → 0.49 s (202
    opens = 29 ms), match launch ~1.15 s → ~0.9 s.
69. **Joins/leaves in the middle of a match (Nov 2, public match with 6
    players).** Hitches of 127-245 ms on UnityMain on every "X joined /
    left". Profile: the game's managed code, mostly the **construction of
    regular expressions** (`RegexParser`, `RegexWriter`…: a `new Regex(...)`
    on every join/leave, probably the chat or nickname filter,
    `ChatFilter.cs` / `StringExtensions.cs` upstream), plus a GC collection
    and some `FindObjectsByType` calls from the UI. Possible fix on the C#
    side (`unity/`): keep these `Regex` as `static readonly`, built once (at
    load time), which requires the CI Android build (~1 h) and new SD data.
    Not done.
70. **`Regex.CacheSize = 128` (unity/Runtime/NxPort/NxPerf.cs, Nov 3).**
    Exact cause of the hitches in DECISIONS 69: `ChatFilter.FilterString`
    (every chat line, including "X joined / left") runs 53 patterns through
    the static `Regex.Match`, whose cache holds 15 compiled patterns; each
    call recompiled ~40 of them. 128 keeps them all after their first use
    (the host's "Jojo joined" line). Requires the CI Android build, then an
    update of the SD data (libil2cpp, metadata).
    Verified on Nov 3: 26 joins/leaves in 27 min, no hitch.
71. **"Jungle" crash online (Nov 3): memory reservations too large.**
    Unity reserves its 64 MB regions by asking for 131 068 KB (block +
    alignment). When the OC window (1.47-1.66 GB depending on ASLR) is full,
    `mmap_fake` goes through newlib's `memalign(64 MB, 131 068 KB)`, which
    commits the 128 MB and looks for ~192 MB of contiguous space. Online,
    newlib had only 136-168 MB of never-touched memory left:
    `[gpua] memalign FAILED size=131068 KB`, then a crash. The OC reserve
    (640 MB, peak 188-257) and the GPU arena (320 MB) are memory taken from
    newlib at startup: 448/256 leave ~630 MB never touched. **Do not**
    allocate only 64 MB of these 131 068 KB: v0.1.39 did that and crashed at
    startup, as Unity writes just past the 64 MB (pass 2 "kept" of
    `oc_alloc_locked` assumes the same thing: suspect it if it is ever
    used).
72. **The OC reserve grows (v0.1.40).** It is carved out at startup
    (`oc_pool_mb`, 448 by default) and never gives anything back. Before,
    once it was full, `oc_commit_locked` logged "EXHAUSTED" and returned:
    `mprotect` had already said yes to Unity, and its first write crashed.
    Now it takes a 32 MB block from newlib (`__real_memalign`, so as not to
    land in the GPU arena) and first uses up the rest of the old one.
    Verified with 128 MB: three expansions, no crash. At 448 MB nothing
    changes (193-251 MB used). The GPU arena, for its part, already
    overflows safely onto newlib; its real peak is 219 MB (Nov 3, handheld),
    the "maps live" counter of `[gpu]` (143-165 MB) is not the arena.
73. **IL2CPP gzip level (`[debug] zlib_level`, v0.1.40, default 1).** At the
    end of a match the game compresses its replay on UnityMain
    (`ActiveReplayManager.SaveReplay` → `Quantum.Compression` →
    `GZipStream`): 97% in `deflate_slow`, 300-700 ms. Mono's native side
    (`CreateZStream`, exported by libil2cpp) calls `deflateInit2(level -1)`
    with a hard-coded level: `CompressionLevel.Fastest` on the C# side would
    be ignored. The `.nro` looks for `mov w1,#-1 ; mov w2,#8` in
    `CreateZStream` and sets the desired level, otherwise it touches
    nothing. Measured on a replay of 573 KB uncompressed: 3.7× faster for
    12% more bytes, still standard gzip. On console, compression went from
    97% to 16% of the replay frame. Quitting a match also saves a replay
    (`OnGameDestroyed`), which makes it possible to measure alone in a
    private room.
74. **Asynchronous log (v0.1.40).** Cause of the hitches during gameplay
    (profile of Nov 3: 150-375 ms, 74-100% of the samples in `debugPrintf`
    ← `fflush` ← `fsFileWrite`, called by `nx_gpu_report` and the render
    loop): the "important" `[loop]`, `[gpua]`, `[gfx]` lines from rendering,
    every ~2 s, forced a write to the SD card from UnityMain, and the card
    sometimes takes hundreds of ms. `debugPrintf` fills a 256 KB queue (two
    buffers) and a "log" thread (priority 0x3B) writes it out at least every
    2 s, and right away after an important line. A full queue = lines lost
    and counted (`[log] N bytes of log lost`) rather than waiting for the
    card. `debug_log_flush()` stays synchronous (fatal error, exit,
    `OnExitRequest`); the crash handler calls `debug_log_emergency()`, which
    only writes if no lock is held; `[debug] log_sync = 1` keeps the old
    behavior. Measured: 6.6 min of play without a single frame ≥ 100 ms.
75. **IL2CPP warm-up for room creation: no effect (Nov 3).**
    v1 (e4c6472): attributes, `Assembly.GetType`; v2 (44ab3d5): plus
    `Statics.RegisterSimulationTypes` on a throwaway `TypeRegistry`, 15 s
    after the first scene (~100 ms, in the background). The first creation
    still takes 0.74 + 0.90 s (the second 0.45 s): the cost is in building
    `QuantumGame`/`Frame` itself (generic method inflation ~25%, `char*` →
    `Il2CppClass` table ~13%, newlib malloc/free ~25-30%). Harmless, left in
    (`[game] warmup = 0` turns it off).
76. **Transfers to the console (Nov 3-4).** DBI's FTP server no longer
    completes `STOR` (all bytes received, end of stream never seen, a single
    thread: it stays stuck, even on a new file after `DELE`); reads (`RETR`,
    `SIZE`) work. So: Unity data over MTP
    (`mtp_push_verify.ps1 -NoVerify`, which answers Explorer's hidden
    dialogs), then verification over FTP (`ftp_push_verify.py --check`);
    `.nro` without a cable through the hbmenu NetLoader (Y, then
    `nxlink -a <ip> -p mvlo/mvlo_nx.nro mvlo_nx.nro`: hbmenu writes it under
    `sdmc:/switch/` + that path and launches it).
77. **Exit without an error screen (v0.1.40, Nov 5).** "Quit" ended on "The
    software was closed because an error occurred". Atmosphère reports are
    filed under the ID of the title that launched the `.nro` (game icon:
    `0587ac03cde1c000`, hbmenu/nxlink: `03db12780bd84000`). Three causes,
    one behind the other:
    1. the "log" thread (DECISIONS 74) was writing to `debug.log` after the
       card was unmounted → `userAppExit()` (util.c, called by `__appExit`
       before `fsdevUnmountAll`) stops it, drains the queue and closes the
       file;
    2. the `[debug] netlog` heartbeat was sending after `socketExit` →
       `nx_netbeat_stop()` before `nx_net_exit()`, which also drains the
       asynchronous UDP send queue;
    3. `__libnx_exit` returns to hbloader, which unmaps the `.nro` while
       about 25 engine threads are still running: the first one that wakes
       up jumps into freed code (a report with "hbl" as the only module).
       `nx_exit_process()` (main.c, also used for `_exit`) sets
       `__nx_applet_exit_mode = 1` before `__libnx_exit(0)`: `appletExit`
       then replaces the exit function with libnx's `_appletExitProcess`
       (`ISelfController::Exit`, then waiting for the system to terminate
       the process), and the `.nro` stays in place. A `svcExitProcess` alone
       (with or without this mode) still showed the error screen, without a
       crash report.
    During this exit, the crash handler terminates (`svcExitThread`) any
    other thread that faults on an already closed service.
78. **End of match: write-behind (v0.1.41, `[io] write_behind`,
    default 1).** Full profile of the replay save
    (`ActiveReplayManager.SaveReplay`, on UnityMain): writes + close + card
    commit, `File.Delete` of the old temporary replay (~20 ms), 25 `lstat`
    calls on the temporary replays (~35-55 ms: size and dates, to choose
    which one to delete), file creation (~8 ms), compression, and IL2CPP
    code run for the first time.
    - A file opened with `O_CREAT|O_TRUNC` fills an in-memory buffer;
      `close()` hands it to the "write-behind" thread, which writes it in one
      go, closes the descriptor and does the commit. Any other operation on
      the descriptor (`read`, `pread`/`pwrite`, `dup`, `mmap`, `ftruncate`,
      `fsync`, `fdopen`, a seek elsewhere) flushes the buffer first. Trap:
      Mono's `FileStream` does `lseek(fd, position, SEEK_SET)` before every
      write; a seek to the current position (or `SEEK_CUR 0`) is therefore
      answered without writing, otherwise everything went back through the
      direct path. `fstat` on such a descriptor is synthesized (new file,
      size = buffer).
    - `unlink` of a file under `/replays/` goes through the same thread.
    - An `open`, `stat`, `rename` or `unlink` of a path that is still being
      written or deleted waits for completion; exit (`nx_exit_process`) and
      `OnExitRequest` drain the queue.
    - The `stat` results of paths under `/replays/` are kept in memory (a
      replay never changes once written), and forgotten on every write,
      close, rename or deletion of the path or of a file in its folder.
    Measured on console (~25 KB replays, private room): 1st save of a
    session 261 → 177 ms, subsequent ones 164 → 92 ms. On a real online
    replay (100-250 KB), the write alone cost up to ~145 ms.
    - v0.1.42: online, the following match ends still spent 28-36 ms in 25
      `stat` calls. Two flaws in the cache: a single slot per key (two
      replays that shared it evicted each other every time) and the folder
      forgotten on every write, whereas the game checks it several times
      (`Directory.CreateDirectory`, folder check by `FileStream`) and never
      reads its dates. Cache in 64 sets of 4 ways; a folder is only
      forgotten if it is itself renamed or deleted; `/switch/mvlo/replays`
      (without a trailing `/`) is covered. One line
      `[io] replays stats since the last save: N from the cache, M from
      the card in T ms` per save. 3 `stat` calls on the card remain: the
      previous replay (forgotten after being written) and twice the name of
      the new one, still absent. In a private room: an 80-84 ms freeze from
      the 2nd end of match on.

79. **The watchdog crashed the game at launch (v0.1.43, Nov 8).** Atmosphère
    report: "User Break" in `__libnx_exception_handler`; the real fault is in
    `stall.log` (`[crash] esr=92000006 far=0x77296498`, a read in
    `dump_thread_context`, on the watchdog thread). Frame 1 of loading takes
    3.6-4.4 s: with a 3 s threshold, the watchdog described every thread on
    almost every launch (82 out of 84), ~270 lines written one by one to the
    card over ~8 s. It paused each thread, read its context, resumed it,
    then walked its stack. Meanwhile, a short-lived game thread ("T18532")
    had exited and libnx had unmapped its stack (`threadClose`);
    `svcQueryMemory` also succeeds on an unmapped address (type `Unmapped`),
    so the bound protected nothing. Three fixes:
    - a 20 s threshold as long as frame 0 or 1 is not finished (a real hang
      during loading is still described; the automatic stop stays at 30 s);
    - the stack (FP chain and scan for return addresses) is copied during
      the pause, using only reads and `so_find_module_by_addr` (lock-free),
      then printed after resuming; an unmapped or unreadable block is
      skipped. Same for the autopsy (`diag_bfmt_context`). `nx_prof.c`
      already did this;
    - `__libnx_exception_handler`: a fault in the watchdog thread
      (`diag_thread_is_watchdog`) writes the dump, then `svcExitThread()`
      instead of `svcBreak`: the diagnostic stops, the game carries on. The
      automatic stop and the vibration have their own threads (`nx_alive.c`).
    Verified on console with a test build (3 s threshold and a deliberate
    fault at address 0x8 after the report): full report in the same
    format, `[crash] fault inside the watchdog: the watchdog stops, the game
    carries on`, then title screen, menu and clean exit.

80. **Threads never closed: "Thread creation failed" (v0.1.44, Nov 9).**
    A 74 min online session on v0.1.43: on a Photon reconnection,
    `[Network] Failed to connect with thrown exception: Thread creation
    failed` (Photon's UDP socket needs a thread), and the game stays
    offline. The process has 96 threads available (`rl: thr` in the
    heartbeat); 70 were in use at the main menu and the count rose with
    each connection. Cause: libnx 4.12 does not implement
    `__syscall_thread_detach` ("Unsupported" in `newlib.c`), so newlib's
    `pthread_detach` returns `ENOSYS` without doing anything, and
    `pthread_create_fake` ignored the `PTHREAD_CREATE_DETACHED` attribute. A
    detached thread that exits is never passed to `threadClose`: the kernel
    thread keeps its slot among the 96 and its 2 MB stack (allocated by
    `pthread_create_fake`) stays mapped. Joined threads leaked their stack
    too (libnx does not free a stack provided by the caller). Fix in
    `imports.c`:
    - one record per thread (stack, `pthread_t` set by the creator and by
      the thread itself, detached or not, finished or not);
    - `pthread_detach_fake` and the attribute mark the thread as detached;
      when a detached thread ends (trampoline or `pthread_exit_fake`), its
      record is handed to a "reaper" thread (libnx, 16 KB) that does the
      real `pthread_join` (waits for the exit, `threadClose`), then frees the
      stack and the record (the bionic TLS block is only freed after the
      exit);
    - `pthread_join_fake` removes the record before the join (the
      `pthread_t` value may be reused afterwards), then frees the stack;
    - the `[mem]` line gives `threads: created, reaped, joined, failed` and
      `rl:`; a failure writes `[thr] pthread_create failed` with the
      counters.
    Verified on console: after 4 reconnections, a private room and a match,
    58 created, 17 reaped, 7 joined, 67/96 stable (84 without the fix).

81. **Room chat with the Switch keyboard (v0.1.45, Nov 9).** The player
    could not reply "gg" after a match. Analysis (`[kbd]` lines of
    `debug.log`) and the game's scene (`MainMenu.unity`, `MainMenuChat.cs`,
    `TextSubmitValidator.cs`):
    - the chat box is the only multiline one (`LineType` 2); its validator
      sends the message on `'\n'`. TMP only applies validators to text
      received while the keyboard is "visible"; text left behind by a
      closing keyboard is copied as is. So OK only filled the box. Now, for
      a multiline field, `nx_kbd_pump` delivers the text + `'\n'` while the
      keyboard is still visible (the message goes out on the next frame),
      then closes the keyboard, empty, two passes later (state 3);
    - `SendChat` refocuses the box (`SelectTextboxNextFrame`): the tap on
      "Send", shorter than a second, was taken for a request and reopened
      swkbd. A tap that is already lifted when the field requests its
      keyboard no longer counts (`nx_input_touch_lifted`; TMP selects on
      contact, with the finger still down);
    - a "pending" keyboard (a request without a fresh gesture) opened on the
      next tap anywhere: it now waits 250 ms and only opens if the field has
      kept the focus (a tap elsewhere does `hideSoftInput`); once it expires
      after one minute, it releases the field (`nativeSoftInputCanceled`)
      instead of leaving it "active" and deaf to taps.
    Verified in a private room, before and after a match: OK sends ("gg",
    "re", "gg"), "Send" with an empty field opens nothing, touching the box
    opens the keyboard; no exception, clean exit.
    Remaining: the box cannot be reached with the controller in the game's
    scene (`TextField` is not the target of any navigation, `BtnSendMessage`
    has no left neighbor), on PC as well.

82. **Public repo, game encrypted (Nov 11).** The account's Actions minutes
    were used up (private repos); a public repo gets free minutes. But on a
    public repo, any GitHub account can download the artifacts: the
    android-build one (the game's `.so` files + its assets, including
    Nintendo assets) would have redistributed the game. So it is now only
    published as an AES-256 zip (7-Zip) whose password is the
    `SD_ARCHIVE_PASSWORD` secret; without the secret, nothing is published.
    The `.nro`, the logs and the emulator screenshots stay public (our code,
    screen images). The official Quantum AppId, present in the old history
    up to v0.1.42, is in plain text in the game's public sources anyway; the
    history was squashed nonetheless, and pull requests from forks wait for
    approval before running the CI (otherwise they could read the cache of
    the game build).
83. **Switch defaults for a new player (v0.1.46, Nov 11).** The game's
    `Settings.LoadSettings()` runs `LoadFromVersion0` (hard-coded defaults:
    Max FPS 0 = unlimited, HUD Scale 8, Discord integration on), then
    `LoadFromVersion1`, which replaces every setting whose PlayerPrefs key
    exists. The wrapper's prefs store (`unity_jni.c`) therefore seeds, when
    they are missing, `Graphics_MaxFPS = 30`, `Graphics_HudScale = 4` and
    `General_DiscordIntegration = 0`: no game code is patched, the game
    saves these values on its first `SaveSettings`, and the options menu
    changes them as usual. Keys already saved are never touched, so an
    existing player keeps their settings. Why: unlimited fps runs at an
    uneven 40-50 on the console (30 is steady, cooler and the simulation
    speed does not depend on it); 8 is the slider's maximum and large on the
    handheld screen (the tester plays at 4); there is no Discord on a Switch.
    The key bindings stay the game's: they already suit a Switch controller
    (A/B jump, X/Y run, R/ZR power-up, L/ZL reserve item).
