# Plan — porting NSMB Mario vs Luigi Online to the Switch (mvlo_nx)

## 0. Verified facts (Oct 25, 2026)

| Item | Check | Result |
|---|---|---|
| Latest stable tag | `git ls-remote --tags` on the official repo | **v2.2.0.1** = HEAD (`fb3da5b`) |
| Unity version | the tag's `ProjectSettings/ProjectVersion.txt` | **6000.3.10f1** (revision `e35f0c77bd8e`) |
| Game license | no LICENSE file in the official repo | no license → **private** repository, nothing from the game is committed |
| Burst | `Packages/manifest.json` | `com.unity.burst` 1.8.28 → `lib_burst_generated.so` likely (loaded if it exists) |
| Network | `PhotonServerSettings.asset` + `NetworkHandler.cs` | Photon **Quantum**, `Protocol: 0` (UDP), `AuthMode = Auth` (UDP, no WSS) |
| AppId | `NetworkHandler.cs` | the official AppId is **hard-coded in the C#** (not only in the asset) → patched at build time |
| Authentication | `AuthenticationHandler.cs` | HTTPS call to `mariovsluigi.azurewebsites.net` via `UnityWebRequest` (Java on Android) → bypassed |
| Input | `Controls.cs`, InputSystem 1.18 | **new Input System** (`<Gamepad>`, `<Touchscreen>`), not Rewired |
| Android graphics | `ProjectSettings.asset` | Vulkan then GLES3, ARMv7+ARM64 → forced to **GLES3 only, ARM64 only** at build time |
| Android entry point | `androidApplicationEntry: 1` | `Activity` (classic JNI natives), not GameActivity |
| cloverpit_nx | `git clone` | MIT, archived, Unity **6000.2.6f2** (≠ 6000.3.10f1) → offsets to redo |
| pvz_fusion_en_nx | `git clone` | same author, same SoLoader lineage; source of the GPU arena / region patch |

## 1. Chosen architecture

```
 ┌──────────── CI GitHub Actions ────────────┐
 │ android.yml : official tag v2.2.0.1        │
 │   + unity/prepare_project.py (5 lines)     │
 │   + unity/Runtime/NxPort (C# bridge)       │──► APK (cache) ──► mvlo-sd-data (artifact)
 │   game-ci/unity-builder, IL2CPP ARM64 GLES3│
 │ nro.yml : devkitpro/devkita64 → mvlo_nx.nro│──► mvlo_nx-nro (artifact)
 │ emu-test.yml : Eden + Xvfb + lavapipe       │──► debug.log in the CI logs
 └────────────────────────────────────────────┘

 Console : hbmenu (title takeover) ─► mvlo_nx.nro
   so_loader : loads libmain / libunity / libil2cpp / lib_burst_generated
   imports   : libc bionic → newlib/libnx, GLES/EGL → mesa, OpenSL → SDL2
   fake JNI : UnityPlayer driven from the render loop (cloverpit_boot.c)
   libunity patches (mvlo_patches.c) : BuildID + guard words → write
   emulated POSIX signals (nx_signals.c) : Boehm GC stop-the-world
   sockets (nx_net.c) : Linux/bionic ABI → libnx BSD, DNS, emulated blocking
   C# ⇄ native bridge (nx_bridge.c) : DllImport("nxbridge") → controllers, touch,
                                      vibration, reading mvlo.ini (AppId)
```

### Why building the game ourselves changes everything
cloverpit_nx had to find functions in a *stripped* binary by
signatures. Here, `libunity.so` is Unity's **standard** Android player
(same BuildID `35ad22c1…` as `PlaybackEngines/AndroidPlayer/.../Release`), and
the Docker image `unityci/editor:ubuntu-6000.3.10f1-android-3` contains its
symbol file `libunity.sym.so`. So `tools/re/derive_unity.py` finds
each target **by its name** and generates `source/mvlo_unity_offsets.h`.
And since we compile the C#, we replace cloverpit's fixed-address il2cpp
hooks (Rewired, Time, Screen…) with a **real managed bridge** compiled into
the game (unity/Runtime/NxPort).

## 2. Risks (in order of estimated severity)

1. **Engine startup** (fake JNI incomplete for Unity 6000.3, unexpected Java
   paths) → visible in `debug.log`; tested under emulation in the CI.
2. **Boehm GC**: cloverpit's "ack without handler" bridge can cause
   `ABORT("sp not set")`. Replaced by signal emulation; if a thread
   never reaches a safe point, the GC waits (logged, `gc = disable` as a fallback).
3. **Memory** (4 GB): 256→64 MB granularity patch, derived (21 sites, like
   cloverpit/pvz); launching in *title takeover* is mandatory.
4. **FMOD/OpenSL audio**: cloverpit patches re-derived (field 0x410 instead of 0x3f8).
5. **Network**: Photon Quantum over UDP through our sockets; no IPv6 on Switch;
   the user's AppId only plays with clients that use the same AppId.
6. **Performance**: Tegra X1 + mesa/nouveau, 2D → probably OK; 720p in handheld mode.

## 3. Phases

| Phase | Contents | Status |
|---|---|---|
| 0 | Research, PLAN, DECISIONS | done |
| 1 | Android CI (cache + SD artifact) | see STATUS.md |
| 2 | Wrapper, .nro CI | `.nro` built in CI |
| 3 | Input / audio / stability | C# bridge + HID + audio patches |
| 4 | Network (libnx sockets, DNS, AppId from the SD card) | implemented, not tested online |
| 5 | README, TEST.md | see docs/ |
