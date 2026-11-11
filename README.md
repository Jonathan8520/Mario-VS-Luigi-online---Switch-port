# mvlo_nx — NSMB Mario vs Luigi Online on Nintendo Switch (homebrew)

Unofficial port of [NSMB Mario vs Luigi Online](https://github.com/ipodtouch0218/NSMB-MarioVsLuigi)
(by ipodtouch0218 and contributors) to the Switch, as a `.nro`.

The game (Unity 6000.3.10f1) is built **from its official sources** for
Android ARM64/IL2CPP by the CI; its native libraries are then loaded on the
Switch by a libnx wrapper from the *so-loader* lineage, adapted from
[cloverpit_nx](https://github.com/ChanseyIsTheBest/cloverpit_nx).

> ⚠ This repository contains **no game code and no game assets**: the game has
> no public license and contains Nintendo assets. The CI builds it from its
> official sources; its data is only published inside an encrypted archive
> (password known only to the repository owner).

## Installation (SD card)

Everything goes into **`sdmc:/switch/mvlo/`**:

```
sdmc:/switch/mvlo/
├── mvlo_nx.nro                     ← artifact "mvlo_nx-nro" (nro workflow)
├── libmain.so libunity.so libil2cpp.so [lib_burst_generated.so]
├── assets/…                         ← artifact "mvlo-sd-data-v2.2.0.1" (android-build workflow, encrypted zip)
└── mvlo.ini                         ← created on first launch; put your Photon AppId in it
```

Step-by-step guide, Photon AppId and which logs to send back: **[docs/TEST.md](docs/TEST.md)**.

## Launching

From hbmenu in **title takeover** mode: hold **R** while starting any game,
then pick *Mario vs Luigi Online*. (Not from the Album: not enough memory.)

## Build

### The .nro (wrapper)
```sh
docker run --rm -v "$PWD":/src -w /src devkitpro/devkita64 make -j4
```
or, with devkitPro installed: `make` (`switch-mesa switch-libdrm_nouveau switch-sdl2 switch-zlib`).
The `nro.yml` CI does the same on every push and publishes the `mvlo_nx-nro` artifact.

### The game data
**android-build** workflow (`.github/workflows/android.yml`, manual trigger or
push on `unity/`): checks out the official tag, injects the `unity/` bridge
and a few text patches (`unity/prepare_project.py`), builds with Unity through
game-ci (secrets `UNITY_LICENSE`, `UNITY_EMAIL`, `UNITY_PASSWORD`). The APK is
kept in the Actions cache; the published artifact is the ready-to-copy SD
folder, as an AES-256 zip (secret `SD_ARCHIVE_PASSWORD`, open it with 7-Zip).

### Emulator test
**emu-test** workflow: runs the `.nro` + the cached game data in the Eden
emulator (software rendering) on the runner and prints `debug.log` in the job log.

### New Unity / game version
`tools/re/derive_unity.py <libunity.so> <libunity.sym.so> -o source/mvlo_unity_offsets.h`
with the Android player of the new version (image `unityci/editor:ubuntu-<version>-android-3`,
`PlaybackEngines/AndroidPlayer/Variations/il2cpp/Release/{Libs,Symbols}/arm64-v8a`).
`tools/re/check_imports.py` lists the imports not covered by the shims.

## Layout

| Path | Role |
|---|---|
| `source/` | C wrapper (cloverpit_nx base + `nx_*` / `mvlo_*` modules) |
| `unity/` | code injected into the Android build (C# bridge, build script, patch) |
| `tools/` | SD packaging, reverse-engineering tools (offsets, imports, symbols), console scripts |
| `dist/mvlo.ini.example` | default configuration |
| `docs/` | PLAN, DECISIONS, STATUS (journal), TEST (installation) |

## Credits

- Switch port (mvlo_nx): **Jonathan8520**.
- Game: **ipodtouch0218** and the NSMB-MarioVsLuigi contributors. Photon Quantum: Exit Games.
- Wrapper: **cloverpit_nx** (ChanseyIsTheBest, MIT) and its SoLoader lineage:
  badpiggies_nx, colorsheep_nx, laytonbmr_nx, vln_nx, pvz_fusion_nx;
  so-loader by TheOfficialFloW (Andy Nguyen), fgsfds, Rinnegatamante (MIT).
- Tools: devkitPro / libnx / switch-mesa, game-ci, Eden (emulator tests).

The wrapper code is MIT-licensed (see `LICENSE` and `LICENSE.cloverpit_nx`).
