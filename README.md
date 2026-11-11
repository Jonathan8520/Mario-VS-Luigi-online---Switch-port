# mvlo_nx — NSMB Mario vs Luigi Online sur Nintendo Switch (homebrew)

Portage non officiel de [NSMB Mario vs Luigi Online](https://github.com/ipodtouch0218/NSMB-MarioVsLuigi)
(ipodtouch0218 et contributeurs) sur Switch, sous forme de `.nro`.

Le jeu (Unity 6000.3.10f1) est compilé **depuis ses sources officielles** en
Android ARM64/IL2CPP par la CI, puis ses bibliothèques natives sont chargées
sur Switch par un wrapper libnx de la lignée *so-loader*, adapté de
[cloverpit_nx](https://github.com/ChanseyIsTheBest/cloverpit_nx).

> ⚠ Ce dépôt ne contient **ni code ni asset du jeu** : le jeu n'a pas de
> licence publique et contient des assets Nintendo. La CI le compile depuis
> ses sources officielles ; ses données ne sont publiées que dans une archive
> chiffrée (mot de passe connu du seul propriétaire du dépôt).

## Installation (carte SD)

Tout va dans **`sdmc:/switch/mvlo/`** :

```
sdmc:/switch/mvlo/
├── mvlo_nx.nro                     ← artifact « mvlo_nx-nro » (workflow nro)
├── libmain.so libunity.so libil2cpp.so [lib_burst_generated.so]
├── assets/…                         ← artifact « mvlo-sd-data-v2.2.0.1 » (workflow android-build, zip chiffré)
└── mvlo.ini                         ← créé au 1er lancement ; y mettre son AppId Photon
```

Guide pas à pas, AppId Photon et logs à remonter : **[docs/TEST.md](docs/TEST.md)**.

## Lancement

hbmenu en **title takeover** : maintenir **R** en lançant n'importe quel jeu,
puis choisir *Mario vs Luigi Online*. (Pas depuis l'Album : mémoire insuffisante.)

## Build

### Le .nro (wrapper)
```sh
docker run --rm -v "$PWD":/src -w /src devkitpro/devkita64 make -j4
```
ou, avec devkitPro installé : `make` (`switch-mesa switch-libdrm_nouveau switch-sdl2 switch-zlib`).
La CI `nro.yml` fait la même chose à chaque push et publie l'artifact `mvlo_nx-nro`.

### Les données du jeu
Workflow **android-build** (`.github/workflows/android.yml`, déclenchement
manuel ou push sur `unity/`) : checkout du tag officiel, injection du pont
`unity/` + patch de 5 lignes (`unity/prepare_project.py`), build Unity via
game-ci (secrets `UNITY_LICENSE`, `UNITY_EMAIL`, `UNITY_PASSWORD`). L'APK est
gardé en cache Actions ; l'artifact publié est le dossier SD prêt à copier.

### Test en émulation
Workflow **emu-test** : lance le `.nro` + les données en cache dans l'émulateur
Eden (rendu logiciel) sur le runner et imprime `debug.log` dans les logs du job.

### Nouvelle version d'Unity / du jeu
`tools/re/derive_unity.py <libunity.so> <libunity.sym.so> -o source/mvlo_unity_offsets.h`
avec le player Android de la nouvelle version (image `unityci/editor:ubuntu-<version>-android-3`,
`PlaybackEngines/AndroidPlayer/Variations/il2cpp/Release/{Libs,Symbols}/arm64-v8a`).
`tools/re/check_imports.py` liste les imports non couverts par les shims.

## Arborescence

| Chemin | Rôle |
|---|---|
| `source/` | wrapper C (substrat cloverpit_nx + modules `nx_*` / `mvlo_*`) |
| `unity/` | code injecté dans le build Android (pont C#, script de build, patch) |
| `tools/` | packaging SD, outils de rétro-ingénierie (offsets, imports, symboles) |
| `dist/mvlo.ini.example` | configuration par défaut |
| `docs/` | PLAN, DECISIONS, STATUS (journal), TEST (installation) |

## Crédits

- Portage Switch (mvlo_nx) : **Jonathan8520**.
- Jeu : **ipodtouch0218** et les contributeurs de NSMB-MarioVsLuigi. Photon Quantum : Exit Games.
- Wrapper : **cloverpit_nx** (ChanseyIsTheBest, MIT) et sa lignée SoLoader :
  badpiggies_nx, colorsheep_nx, laytonbmr_nx, vln_nx, pvz_fusion_nx ;
  so-loader de TheOfficialFloW (Andy Nguyen), fgsfds, Rinnegatamante (MIT).
- Outils : devkitPro / libnx / switch-mesa, game-ci, Eden (tests en émulation).

Code du wrapper sous licence MIT (voir `LICENSE` et `LICENSE.cloverpit_nx`).
