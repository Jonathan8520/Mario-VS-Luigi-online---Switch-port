# Plan — portage de NSMB Mario vs Luigi Online sur Switch (mvlo_nx)

## 0. Faits vérifiés (25/10/2026)

| Point | Vérification | Résultat |
|---|---|---|
| Dernier tag stable | `git ls-remote --tags` du repo officiel | **v2.2.0.1** = HEAD (`fb3da5b`) |
| Version Unity | `ProjectSettings/ProjectVersion.txt` du tag | **6000.3.10f1** (révision `e35f0c77bd8e`) |
| Licence du jeu | pas de fichier LICENSE dans le repo officiel | aucune licence → dépôt **privé**, rien du jeu n'est commité |
| Burst | `Packages/manifest.json` | `com.unity.burst` 1.8.28 → `lib_burst_generated.so` probable (chargé s'il existe) |
| Réseau | `PhotonServerSettings.asset` + `NetworkHandler.cs` | Photon **Quantum**, `Protocol: 0` (UDP), `AuthMode = Auth` (UDP, pas de WSS) |
| AppId | `NetworkHandler.cs` | l'AppId officiel est **codé en dur dans le C#** (pas seulement dans l'asset) → patché au build |
| Authentification | `AuthenticationHandler.cs` | appel HTTPS à `mariovsluigi.azurewebsites.net` via `UnityWebRequest` (Java sur Android) → contourné |
| Entrées | `Controls.cs`, InputSystem 1.18 | **nouvel Input System** (`<Gamepad>`, `<Touchscreen>`), pas Rewired |
| Graphismes Android | `ProjectSettings.asset` | Vulkan puis GLES3, ARMv7+ARM64 → forcé **GLES3 seul, ARM64 seul** au build |
| Point d'entrée Android | `androidApplicationEntry: 1` | `Activity` (natives JNI classiques), pas GameActivity |
| cloverpit_nx | `git clone` | MIT, archivé, Unity **6000.2.6f2** (≠ 6000.3.10f1) → offsets à refaire |
| pvz_fusion_en_nx | `git clone` | même auteur, même lignée SoLoader ; source de l'arène GPU / patch région |

## 1. Architecture retenue

```
 ┌──────────── CI GitHub Actions ────────────┐
 │ android.yml : tag officiel v2.2.0.1        │
 │   + unity/prepare_project.py (5 lignes)    │
 │   + unity/Runtime/NxPort (pont C#)         │──► APK (cache) ──► mvlo-sd-data (artifact)
 │   game-ci/unity-builder, IL2CPP ARM64 GLES3│
 │ nro.yml : devkitpro/devkita64 → mvlo_nx.nro│──► mvlo_nx-nro (artifact)
 │ emu-test.yml : Eden + Xvfb + lavapipe       │──► debug.log dans les logs CI
 └────────────────────────────────────────────┘

 Console : hbmenu (title takeover) ─► mvlo_nx.nro
   so_loader : charge libmain / libunity / libil2cpp / lib_burst_generated
   imports   : libc bionic → newlib/libnx, GLES/EGL → mesa, OpenSL → SDL2
   JNI factice : UnityPlayer piloté depuis la boucle de rendu (cloverpit_boot.c)
   patches libunity (mvlo_patches.c) : BuildID + mots de garde → écriture
   signaux POSIX émulés (nx_signals.c) : stop-the-world du GC Boehm
   sockets (nx_net.c) : ABI Linux/bionic → BSD libnx, DNS, blocage émulé
   pont C# ⇄ natif (nx_bridge.c) : DllImport("nxbridge") → manettes, tactile,
                                   vibrations, lecture de mvlo.ini (AppId)
```

### Pourquoi construire le jeu soi-même change tout
cloverpit_nx devait retrouver des fonctions dans un binaire *strippé* par
empreintes. Ici, `libunity.so` est le player Android **standard** d'Unity
(même BuildID `35ad22c1…` que `PlaybackEngines/AndroidPlayer/.../Release`), et
l'image Docker `unityci/editor:ubuntu-6000.3.10f1-android-3` contient son
fichier de symboles `libunity.sym.so`. `tools/re/derive_unity.py` trouve donc
chaque cible **par son nom** et génère `source/mvlo_unity_offsets.h`.
Et comme on compile le C#, on remplace les hooks il2cpp à adresses fixes de
cloverpit (Rewired, Time, Screen…) par un **vrai pont managé** compilé dans le
jeu (unity/Runtime/NxPort).

## 2. Risques (par ordre de gravité estimée)

1. **Démarrage du moteur** (JNI factice incomplet pour Unity 6000.3, chemins
   Java inattendus) → visible dans `debug.log` ; testé en émulation dans la CI.
2. **GC Boehm** : le pont « ack sans handler » de cloverpit peut faire
   `ABORT("sp not set")`. Remplacé par une émulation de signaux ; si un thread
   n'atteint jamais de point sûr, le GC attend (journalisé, `gc = disable` en secours).
3. **Mémoire** (4 Go) : patch de granularité 256→64 Mo dérivé (21 sites, comme
   cloverpit/pvz) ; lancement obligatoire en *title takeover*.
4. **Audio FMOD/OpenSL** : patches cloverpit re-dérivés (champ 0x410 au lieu de 0x3f8).
5. **Réseau** : Photon Quantum en UDP via nos sockets ; pas d'IPv6 sur Switch ;
   l'AppId de l'utilisateur ne joue qu'avec les clients qui utilisent le même AppId.
6. **Performance** : Tegra X1 + mesa/nouveau, 2D → probablement OK ; 720p en portable.

## 3. Phases

| Phase | Contenu | État |
|---|---|---|
| 0 | Recherche, PLAN, DECISIONS | fait |
| 1 | CI Android (cache + artifact SD) | voir STATUS.md |
| 2 | Wrapper, CI .nro | `.nro` compilé en CI |
| 3 | Entrées / audio / stabilité | pont C# + HID + patches audio |
| 4 | Réseau (sockets libnx, DNS, AppId depuis SD) | implémenté, non testé en ligne |
| 5 | README, TEST.md | voir docs/ |
