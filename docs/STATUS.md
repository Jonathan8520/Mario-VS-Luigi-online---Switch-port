# État du projet (mis à jour en continu pendant la nuit)

Légende : ✅ vérifié · 🟡 implémenté, non vérifié · ❌ bloqué / non fait

## Livrables du réveil
| Livrable | État |
|---|---|
| `.nro` compilé par la CI, téléchargeable (workflow **nro**, artifact `mvlo_nx-nro`) | ✅ |
| Données du jeu pour la SD (workflow **android-build**, artifact `mvlo-sd-data-v2.2.0.1`, 94 Mo zippé / 244 Mo sur la SD) | ✅ |
| `docs/TEST.md` (SD, lancement, AppId Photon, logs) | ✅ |

## Ce qui a été réellement exécuté
- ✅ Le `.nro` compile sans erreur (local + CI, image `devkitpro/devkita64`).
- ✅ **Émulateur Eden** (yuzu lineage, `linuxserver/eden`, rendu logiciel
  lavapipe) dans le conteneur : le `.nro` démarre, crée `mvlo.ini`, écrit
  `debug.log`, initialise les sockets BSD (IP locale, DNS dispo).
- ✅ **Smoke test** (`[debug] smoke_test = 1`) avec le *vrai* player Unity
  6000.3.10f1 (libmain + libunity, sans le jeu) sous Eden :
  BuildID reconnu, **6 patches moteur + 21/21 sites de granularité mémoire
  appliqués**, 484 constructeurs de libunity exécutés, `JNI_OnLoad` →
  62 natives UnityPlayer enregistrées, les 11 natives pilotées trouvées aux
  RVA attendues, flux vsync `SetFrameTimeNanos` à 60 Hz (30 appels/0,5 s).
- ✅ **smoke_test = 2** : le player Unity entre dans son cycle de vie
  (initJni, RecreateGfxState, SetRunning, 1re frame) : allocateur Dynamic Heap,
  infos système et `ApplicationInfo 'com.ipodtouch0218.NSMBMarioVsLuigi' 2.2.0.1`
  journalisés, jusqu'au chargement d'IL2CPP (absent par construction →
  dialogue d'erreur Unity intercepté et journalisé).
- ✅ **smoke_test = 3** : auto-test de l'émulation de signaux avec le protocole
  exact de bdwgc à travers la vraie table d'imports : 5/5 cycles
  stop-the-world, 4/4 pointeurs de pile enregistrés, arrêt ≈ 10 ms.
- ✅ **smoke_test = 4** (dans la CI) : DNS 3/3 (`ns.photonengine.io` résolu),
  aller-retour UDP (NTP) réussi, errno TCP Linux correct.
- ✅ **Jeu complet sous Eden** (workflow **emu-test**) : 506 imports résolus,
  IL2CPP + GC actifs, pont C# NxPort connecté, manette 1 détectée, FMOD
  OpenSL en lecture (24 kHz stéréo), **écran titre rendu en continu pendant
  10 min**, aucune exception ni erreur Unity, aucun plantage. ~7,7 i/s en rendu
  logiciel (lavapipe, CPU du runner) : ce chiffre ne dit rien de la Switch.
- ✅ **Entrées** : des appuis scriptés (`[debug] autopress`) sur la manette 1
  font passer l'écran titre puis entrer dans le menu en ligne.
- ✅ **HTTPS** : le jeu télécharge ses actualités
  (`mariovsluigi.azurewebsites.net`, « Found 3 news post(s) ») via le DNS et
  les sockets du port.
- ✅ **Photon** : la connexion est tentée et échoue proprement avec le message
  « No Photon Quantum AppId » (aucun AppId dans le test, comme prévu). La
  connexion réelle reste à tester avec votre AppId.
- ✅ **Vraie Switch (v0.1.1, fw 22.1.0, portable)** : écran titre, menus,
  manette, son, réseau (actualités HTTPS), **30 i/s stables**, ~28 s de
  chargement, aucun plantage. *Play Game* reste sur « Connecting… » faute
  d'AppId Photon dans `mvlo.ini` (attendu).
- 🟡 v0.1.2 : 60 i/s par défaut (le jeu laisse « Max FPS » illimité, ce
  qu'Unity Android traduit en 30). Sous Eden le réglage s'applique
  (`targetFrameRate never set by the game -> 60`) et l'écran titre passe de
  ~7,7 à ~13 i/s en rendu logiciel ; à confirmer sur console.
- ✅ Connexion Photon réelle (vraie Switch, 26/10 au soir) : authentification
  (anonyme avec un AppId perso, ou l'auth officielle `use_official_auth = 1`
  qui marche aussi : réponse 200 + jeton), région `eu`, création de salle,
  Quantum Runner, AddPlayer, **partie jouée plus de 5 min avec le GC actif**
  (v0.1.5), fermeture propre.

## Sur console, 26/10 au soir (v0.1.3 → v0.1.7)
Corrigé et vérifié sur la vraie Switch :
- ✅ **Gel après « Creating a game »** (écran figé, musique qui continue) :
  stop-the-world du GC bloqué. Le thread réseau de Photon boucle sur
  `poll()` avec des délais courts et `poll_fake` sortait par le test de
  délai **avant** son point sûr : le signal n'était jamais livré. Point sûr
  désormais à l'entrée de `poll/recvfrom/sendto/accept4`. Confirmé : avec
  `gc = disable` la partie tournait (puis la console se figeait faute de
  mémoire), avec la correction elle tourne GC actif.
- ✅ **Plantage `[oc] svcMapMemory FAIL rc=0xd401`** : le noyau place les
  pages de TLS des threads (type 0xc, 8 threads/page, page démappée quand elle
  se vide) au hasard dans la zone de pile, donc dans la fenêtre d'overcommit,
  dans de l'espace réservé par Unity mais pas encore engagé. Au démarrage,
  64 threads auxiliaires forcent la création de ~9 pages et un thread par page
  reste vivant (`[tls] … pinned`), les threads suivants réutilisent ces places
  (`debug.tls_anchor`, 64). Vérifié : plus aucune page 0xc n'apparaît après
  l'armement. (La 1re version utilisait la priorité 0x3F, réservée au cœur 3 :
  aucun thread créé.)
- ✅ `stall.log` affiche maintenant l'état des signaux émulés de chaque thread
  (`[sig] tid … pending … blocked …`) : un stop-the-world bloqué nomme le
  thread fautif.

- ✅ **PlayerPrefs, chaînes et entiers** (v0.1.8, vérifié sur console :
  touches conservées après relance, `prefs.kv` de 53 entrées avec pseudo,
  `Controls_Bindings`, `Graphics_MaxFPS`, `id`/`token` de l'auth officielle).
  Rien n'était gardé avant, même dans la session : Unity 6000.3 charge ses
  classes Java par `Class.forName`, que le faux JNI rend `java/lang/Object`,
  donc ni les handlers de `unity_jni.c` ni `Uri.encode/decode` ni
  `String(byte[], String)` (encodage des clés) n'étaient reconnus. Aiguillage
  par **receveur** (handles de unity_jni) et par **signature** (DECISIONS 28).
- ❌ **PlayerPrefs, réels** (HUD Scale, volumes : reviennent à 0) :
  `getFloat` n'est pas routé vers `unity_jni.c` (`dispatch_float` de
  `jni_fake.c` ne connaît que les boxes) et `putFloat` reçoit **0** alors que
  la clé arrive juste (`[prefs] putFloat 'Audio_MasterVolume'=0` alors que le
  son joue). Reproductible sous Eden.
- ⚠ Cause racine commune : `Class.forName` rend toujours `java/lang/Object`.
  Le corriger globalement réglerait tout ça d'un coup, mais change
  l'aiguillage de nombreux appels qui marchent aujourd'hui « par accident » :
  à faire prudemment, avec un test émulateur complet.

Corrigé, à vérifier :
- 🟡 **X/Y inversés à l'affichage** en disposition `nintendo` (A/B étaient
  mappés par étiquette, X/Y par position) : X → West (« x »), Y → North (« y »)
  dans `NxInput.cs`. Demande la recompilation Unity.
- 🟡 Option `[photon] build_identifier` (suffixe de l'AppVersion Photon ;
  les sources publiques donnent `vanilla`, les builds officiels `official`).
  Vide par défaut. Demande la recompilation Unity.

Restant (vu sur console) :
- ❌ **Pas de clavier** : impossible de changer le pseudo ou d'écrire dans le
  chat (il faut brancher le clavier logiciel Switch, swkbd, sur le
  `TouchScreenKeyboard` d'Unity / `showSoftInput`).
- ❌ **Écran noir fixe ~25 s au démarrage** (frame 1 ≈ 23 s) : pas d'écran de
  chargement.
- ❌ **Gros arrêts au lancement d'une partie** (6 à 9 s, puis 0,3-1 s) : pas le
  GC (aucun signal en attente) ; FMOD lit ses banques depuis la SD par petits
  morceaux (`fsdev_read <- read_fake`) pendant que UnityMain attend.
- ❌ `ArgumentOutOfRangeException` dans les callbacks `UI/Navigate` à la
  manette (sans effet visible).
- ❌ En 720p portable, le rond « NoItem » (bas droite) et la liste des joueurs
  (haut gauche) sont trop petits ; *HUD Scale* au maximum convient à
  l'utilisateur (une valeur par défaut plus grande sur Switch serait bien, et
  il faut d'abord que les réels des PlayerPrefs soient gardés).
- Mémoire physique libre au démarrage : 3 Mo (tas newlib 2 480 Mo) ; sans GC
  la console finit figée (vu avec `gc = disable`).

## Nuit du 26 au 27/10 (v0.1.9) — à vérifier sur console
Vérifié sous Eden (CI, deux démarrages successifs sur la même SD) :
- ✅ **Réglages réels gardés** : premier lancement `F Graphics_HudScale 8`,
  volumes 0,75 / 0,5 / 0,75, `Graphics_MaxFPS 60` ; relancement :
  `getFloat 'Graphics_HudScale' -> 8`, etc. Cause réelle : liste des
  résolutions vide → valeurs par défaut du jeu jamais posées (DECISIONS 31).
  Le HUD minuscule venait de là (défaut du jeu = 8 = maximum du curseur).
- ✅ **Écran de chargement** pendant les frames 0-1 : `[load] loading
  screen: 217 frames over 18601 ms`, visible sur les captures de 9 à 25 s
  (titre, roue de carrés, barre).
- 🟡 **Écran de chargement en images** (v0.1.10) : logo Infenek, 5 captures
  en fondu enchaîné avec zoom lent, logo du jeu, barre arrondie. Rendu vérifié
  sur PC (`tools/splash_preview`) et sous Eden (`5/5 picture(s) shown`, menus
  atteints ensuite, temps de chargement inchangé) ; images téléchargées par la
  CI, jamais commitées. Sous Eden le premier shader (avec des `return`)
  faisait planter l'émulateur : remplacé par un shader sans branchement.
  À voir sur console.
- ✅ **Cache de lecture SD** actif (`[io] block cache ON`), statistiques par
  fichier dans le log ; aucune lecture lente sous Eden (la SD de l'émulateur
  est un SSD : le gain réel se mesurera sur console).
- 🟡 **Clavier Switch** : branché (`showSoftInput` → swkbd), mais pas
  déclenché par les appuis scriptés de la CI : à essayer sur console
  (pseudo, chat). Il ne s'ouvre que juste après un appui ou un toucher, pour
  que le clavier du jeu (qui redemande le clavier à chaque image) ne le
  rouvre pas en boucle.
- 🟡 `Class.forName` rend la vraie classe, `Call*MethodA` reçoivent leurs
  arguments : aucune régression vue sous Eden (menus, réseau, audio
  initialisé), à confirmer sur console.
- ✅ Pas de régression de vitesse : l'écart vu sous Eden (163 ms puis 90 ms
  par image) venait des machines GitHub ; test A/B fait (cache SD seul
  coupé : 120 ms ; tout actif : 70 ms, le plus rapide). Le log affiche
  maintenant toutes les 2 s une ligne `[perf]` (temps de `nativeRender`, de
  l'affichage, appels JNI, lectures et attentes par image) : sur console,
  elle dira d'où vient le plafond à 30 i/s.
- 🟡 `ArgumentOutOfRangeException` dans `UI/Navigate` : cause probable
  trouvée dans les sources — la liste *Résolution* des options était vide
  (même cause que les réglages à 0, voir DECISIONS 31) et gauche/droite
  dessus levait l'exception. Corrigé avec `getSupportedModes` ; si elle
  revient, il me faut les lignes exactes du `debug.log` et l'écran où
  c'était.

## Sur console, 27-28/10 (v0.1.10 → v0.1.12)
Vérifié sur la vraie Switch avec la v0.1.10 (données android-build 255db44) :
- ✅ **Écran de chargement en images** : `5/5 picture(s) shown`, ~25 s.
  Les ~4 premières secondes restent noires : l'écran attend le contexte GL
  d'Unity (`[load] loading screen on` à 4,3 s).
- ✅ **Clavier Switch** : pseudo changé, message de chat envoyé
  (`[kbd] confirmed`).
- ✅ **~58 i/s** en partie (`target_fps` appliqué), réglages réels gardés.
- ❌ `NullReferenceException` dans les callbacks `canceled` de `UI/Navigate`
  au moment de `Game started!` (2e partie).

**Plantage n° 1 — manque de mémoire au chargement d'un niveau (corrigé en
v0.1.12).** La ligne `[mem]` (v0.1.11, toutes les 2 s : tas newlib utilisé /
libre dans l'arène / jamais touché, overcommit engagé / réserve) a montré :
```
65.344 [mem] newlib: in use 2275 MB, free in arena 34 MB, never touched 170 MB | oc: committed 203 MB, pool 203/896 MB
77.565 [gpua] memalign FAILED size=131068 KB (arena live=142 MB peak=143 MB)
77.878 Unity: Could not allocate memory: System out of memory! ... MemoryLabel: FMODSample
```
Deux réserves prises au démarrage dans le tas restaient en grande partie
vides : la réserve d'engagement de l'overcommit (896 Mo, 203 Mo utilisés) et
l'arène GPU (512 Mo, pic de 142 à 212 Mo). v0.1.12 : `[debug] oc_pool_mb`
(640) et `gpu_arena_mb` (320). Sur la partie suivante : `in use 1962 MB, free
in arena 77 MB, never touched 440 MB | oc 241/640`, arène GPU 212/320. Plus
de manque de mémoire côté jeu. Note : Unity réserve maintenant ~2,09 Go
(« reserved: 2087566672B »), plus que la fenêtre d'overcommit (1,28 à
1,66 Go selon le découpage) ; le surplus prend de la vraie mémoire.

**Plantage n° 2 — gel complet de la console, NON résolu.** v0.1.12 : une
partie complète (74 s → 301 s), 2e partie lancée à 312 s, gel à 442,4 s
juste après `[gfx] swap 20700 ok`. Le watchdog s'arrête au même instant
(dernier `alive` à la frame 20653), HOME et volume ne répondent plus, seul le
son continue, aucun rapport Atmosphère. Le tas newlib avait ~520 Mo de libre :
ce n'est pas le manque de mémoire du jeu. Déjà vu deux fois en début de 2e
partie (v0.1.10). Pistes non vérifiées : service GPU système (libnx donne
**8 Mo** de transfer memory à nvservices par défaut,
`__nx_nv_transfermem_size = 0x800000`, jamais redéfini par le port), mémoire
physique du pool applicatif (3 Mo libres au démarrage : tout est pris par le
tas), contexte EGL de l'écran de chargement, 60 i/s, cache SD.

**À-coups — NON résolus, insupportables pour l'utilisateur.** Au lancement
d'une partie : `SLOW nativeRender: 6445 ms`, puis `7637 ms` ; ensuite 300 à
800 ms à la première apparition de chaque objet (premier champignon, premier
bloc tapé…). Pendant l'arrêt de 6,4 s, la SD n'a lu que ~8 Mo en ~0,6 s
(`[io] cache: … SD 46 MB in 2202 ms total`) : c'est du **calcul**, pas des
lectures. Profil typique de la compilation de shaders à la première
utilisation (mesa/nouveau sur l'A57) ou de l'upload de textures. À mesurer
puis traiter (cache de programmes GL `GL_OES_get_program_binary` / cache
disque mesa, préchauffage des shaders par Unity, etc.).

## Nuit du 27 au 28/10 (v0.1.13) — à vérifier sur console
- 🟡 **Gel de la console** : pas reproductible sous Eden (il n'émule ni
  nvservices ni le segment de code de nouveau). Deux causes plausibles
  neutralisées (DECISIONS 39) : segment de code des shaders de nouveau porté
  à 16 Mo au premier débordement au lieu de 4 évictions/réallocations
  successives (`[video] shader_code_mb`), transfer memory de nvservices
  portée à 32 Mo (`[debug] nv_tmem_mb`, repli 8 Mo automatique). Et de quoi
  le voir venir : lignes `[gpu]`/`[nv]` (objets GPU, canaux, échecs, attentes
  de fence avec l'erreur du canal), état GPU dans `[wd] alive`.
- 🟡 **À-coups** : mesurés désormais (`[hitch]` par image lente). Sous Eden,
  au menu : compilation GLSL ≈ 50 %, édition de liens ≈ 30 %, génération de
  code nouveau ≈ 20 % du temps de compilation. Le mesa de devkitPro est
  compilé **sans cache de shaders** sur Switch (vérifié dans ses sources) :
  ni cache disque, ni `glProgramBinary`. Fait : CPU à 1785 MHz pendant les
  compilations (`[video] compile_boost`), niveau d'optimisation nouveau
  réglable. Proposé : reconstruire mesa avec un cache (DECISIONS 40).
- 🟡 **Écran noir des 4 premières secondes** : le 1er essai (logo au
  framebuffer libnx) bloquait la 1re image d'Unity sous Eden, abandonné. Fait
  à la place (DECISIONS 41) : nous créons tôt l'unique surface EGL, avec la
  config notée au démarrage précédent (`.eglconfig`), et la donnons à Unity.
  Sous Eden au relancement : écran de chargement à 0,49 s, rendu à Unity à
  2,7 s, partie normale ensuite. Le 1er démarrage après installation reste
  noir. À valider sur console (`[video] early_splash = 0` pour couper).
- 🟡 **NullReferenceException `UI/Navigate`** : cause trouvée (clavier du jeu
  resté ouvert après le clavier Switch, abonné à Navigate après sa
  destruction) ; corrigée dans le jeu (`prepare_project.py`) et dans `NxInput`
  (clavier du jeu fermé après swkbd). Demande les nouvelles données SD.
- ✅ Sous Eden : démarrage, menus, réglages gardés, aucune erreur nv
  (`148 maps live, 1 channel, 0 failed`), transfer memory 32 Mo acceptée.

## Sur console, 28/10 au soir (v0.1.13 → v0.1.19)
Vérifié sur la vraie Switch avec la v0.1.13 : écran de chargement dès ~0,5 s
(dès le 2e lancement), clavier Switch, X/Y, réglages réels gardés (HUD,
volumes), liste des langues.

**À-coups : ce n'étaient pas les shaders, c'était `stat()` (corrigé en
grande partie, v0.1.17).** Le `[hitch]` de la v0.1.13 ne voyait que ~0,1 s de
GL dans une image de 24 s (`other 24064 ms`) ; la pile du watchdog (symboles
CI) montrait UnityMain dans une IPC du service de fichiers sous
`File::Open`/`IsDir`. Mesuré en v0.1.14 : *Room Settings* = **1728 `stat()`
pour 14,8 s**, début de partie ~650 pour ~6 s, démarrage 3077. libnx
`fsdev_stat` fait 5 allers-retours FS par fichier (GetEntryType, OpenFile,
GetSize, Close, GetFileTimeStampRaw), chacun une recherche dans `Data/`
(1570 fichiers) : ~8,6 ms. `nx_statcache.c` liste une fois chaque dossier de
`assets/` et `il2cpp/` et répond de mémoire (y compris « absent », 4207 sur
6947 dans une session) ; `/`, `/switch`, `/switch/mvlo` répondent « dossier ».
Résultats console : *Room Settings* 20 s → **3-5 s**, démarrage ~25 s →
**8 s** (sans écran de chargement). Reste : ~260 ouvertures par menu (~4,9 ms
chacune : `fsdev_open` = OpenFile + GetFileTimeStampRaw), Unity rouvrant sans
cesse les mêmes fichiers (un même fichier ouvert 310 fois). `[hitch]` sépare
maintenant `file open` / `stat` / `fstat` / `access`.

**Blocage n° 1 — le jeu se fige, la console vit (cause trouvée, NON
corrigé).** v0.1.19, au début d'une partie : heartbeat UDP vivant, `frame`
figé, appels réseau arrêtés, HOME fonctionne. `stall.log` :
`[sig] tid 657 main pending 0x20000000 blocked 0x0` = le stop-the-world du GC
attend UnityMain, qui ne passe par aucun point sûr. Pile (symboles CI) :
`SessionRunner.Service → DeterministicSession.UpdateSimulation →
QuantumGame.OnSimulate → TaskContext.EndFrame +0x64c`, une boucle d'attente
de code managé (appel d'interface à chaque tour, recherche de méthode IL2CPP
`ldur/cmp/b.ne`, aucun appel système) qui attend les workers de tâches
Quantum… parqués par ce même GC. Interblocage. Sous Linux le signal
interromprait la boucle. Correction attendue : que l'émulation de signaux
puisse arrêter de force un thread qui ne joint pas de point sûr (suspension
noyau + contexte), ou tout autre moyen sûr de livrer le signal à un thread
qui tourne en code managé.

**Blocage n° 2 — gel complet de la console (cause inconnue).** HOME et
volume morts, la musique tourne en boucle. Le heartbeat UDP (`[debug]
netlog`, v0.1.18) **s'arrête au même instant**, alors que son thread ne fait
que dormir, formater et envoyer (priorité 0x28, cœur 2, ni SD ni GPU) :
c'est toute la console qui s'arrête. Dernier battement toujours sain
(`idle` 5-30 ms, `gpu busy=0 failed=0`, `fs: idle, 16 open`). Survient après
~7 à 13 min de jeu, en 1re comme en 3e partie, aussi avec `gc = disable`
(26/10). **Écarté par mesure** : manque de mémoire du jeu (`[mem]` stable,
>500 Mo libres), opérations de fichiers en cours, 60 i/s, écran de chargement,
`shader_code_mb`, `nv_tmem_mb` (test A/B 28/10 : tous coupés + 30 i/s →
gel à ~9 min). Pistes ouvertes : interblocage dans un service système
(réseau, audio…), emuMMC sur la même SD, priorités / cœurs des threads,
CPU boost, autre. La v0.1.19 ajoute au heartbeat température SoC
(~37 °C), blocs mémoire du processus (~900), ressource système (0 : le
processus utilise celle, partagée, des applications) et appel réseau en cours.

Outils : `tools/switch/` (FTP avec vérification SHA-256 et reprise, MTP,
différence d'artifacts, `resolve_stall.py` pour nommer les adresses des
dumps avec l'artifact `mvlo-symbols`, écoute du heartbeat
`netlog_listen.py`/`.php`).

## 11/11 — dépôt public, historique regroupé

- ✅ Historique réécrit en quelques commits sur `main` (auteur Jonathan8520) ;
  l'ancienne branche de travail est supprimée. Les hashs de commit cités plus
  bas (avant le 11/11) désignent l'ancien historique, gardé hors du dépôt.
- ✅ Dépôt passé en public (minutes Actions gratuites). Le jeu n'y est
  jamais téléchargeable : l'artifact `mvlo-sd-data-*` d'android-build est un
  zip AES-256 (secret `SD_ARCHIVE_PASSWORD`), voir `docs/TEST.md`.
- Crédits : LICENSE et auteur affiché dans hbmenu = Jonathan8520 (le jeu reste
  crédité à ipodtouch0218).

## Sur console, 09/11 soir (v0.1.45) — chat du salon au tactile

- ✅ **Chat** : après une partie, impossible de répondre « gg » (signalé par le
  joueur). Trois défauts (DECISIONS 81) : OK du clavier Switch remplissait
  la case sans envoyer ; toucher « Envoyer » envoyait puis rouvrait le
  clavier (le jeu refocalise la case) ; un clavier « en attente » s'ouvrait
  à la touche suivante n'importe où, ou laissait la case morte au bout d'une
  minute. Vérifié en salon privé, avant et après une partie : OK envoie,
  « Envoyer » n'ouvre plus rien, toucher la case ouvre le clavier.
- 🟡 La case du chat reste inaccessible à la manette : la scène du jeu ne la
  relie à rien (`BtnSendMessage` n'a pas de voisin à gauche), sur PC aussi.
  Correction possible côté Unity (`prepare_project.py`), avec une nouvelle
  copie des données.

## Sur console, 09/11 (v0.1.44) — plus de threads après ~1 h en ligne

- ✅ **v0.1.43 en ligne (08/11)** : deux sessions de 74 min (14 parties) et
  44 min (10 parties), 30 i/s, aucun plantage du jeu. Cache des replays :
  22 sur 25 depuis la mémoire, 2-23 ms. Le seul rapport de plantage de la
  journée est celui de l'appli HB App Store, pas du jeu.
- ✅ **« Thread creation failed » après 74 min** : en se reconnectant à Photon,
  le jeu n'a pas pu créer le thread de sa connexion et est resté hors ligne.
  Le processus a 96 threads ; les threads détachés du moteur n'étaient
  jamais fermés (libnx n'a pas de `pthread_detach`) et gardaient leur place
  et leur pile de 2 Mo. Corrigé en v0.1.44 (DECISIONS 80).
- ✅ Vérifié sur console (v0.1.44, 4 reconnexions, un salon privé, une
  partie, retour au salon) : 58 threads créés, 17 fermés par le ramasseur, 7
  joints, 67/96 occupés et stables (la v0.1.43 en serait à 84) ; sortie
  propre.

## Sur console, 08/11 (v0.1.43) — plantage au lancement corrigé

- ✅ **v0.1.42 a planté une fois au lancement** (08/11, avant « Jouer »,
  rapport Atmosphère `01794139211_…`). Ce n'était pas le jeu : le watchdog
  (`diag.c`) décrivait tous les threads parce que la frame 1 du chargement
  dure 3,6-4,4 s (seuil 3 s, rapport à 82 lancements sur 84, ~8 s
  d'écritures sur la carte), et il a lu la pile d'un thread court qui venait
  de se terminer (pile démappée). Corrigé en v0.1.43 (DECISIONS 79) : seuil
  de 20 s pendant les frames 0-1, pile lue pendant que le thread est en
  pause, et un plantage du watchdog n'arrête plus que lui.
- ✅ Vérifié sur console : une version de test qui provoque le rapport puis
  une faute dans le watchdog continue jusqu'au menu et quitte proprement ;
  la v0.1.43 démarre sans rapport du watchdog, menu principal, sortie propre.

## Sur console, 05/11 (v0.1.40) — validée en ligne, sortie sans erreur

- ✅ **52 min en ligne** (04→05/11, Jungle comprise) : aucun plantage, 30 i/s
  stables en partie (le réglage 60 i/s donne 40-48 i/s en partie : limite
  CPU, le joueur reste à 30). Seules chutes : début/fin de partie (~0,4 s)
  et un accroc de ~0,2 s en pleine partie. Réserve OC au plus haut
  253/448 Mo, arène GPU 177 Mo, 753 Mo du tas jamais touchés.
- ✅ **« Quitter » affichait « Vous avez quitté le logiciel car une erreur
  est survenue »** : trois causes, toutes corrigées (DECISIONS 77). Vérifié
  sur console, lancé par l'icône du jeu et par hbmenu : retour à l'accueil
  sans message, aucun rapport de plantage, `debug.log` complet.
- ✅ **v0.1.41 — fin de partie** : l'accroc « en pleine partie » de 01:13:45
  était la sauvegarde du replay. Écriture, fermeture, commit et suppression
  de l'ancien replay passent sur un thread ; les `stat` du dossier des
  replays sont gardés en mémoire (DECISIONS 78). Retour au salon : 261 →
  177 ms la première fois, 164 → 92 ms ensuite. Replays relus sans erreur
  dans le menu « Replays ».
- ✅ **v0.1.41 validée en ligne** (05/11 16:41-17:12, 31 min, « c'était
  top ») : 8 fins de partie, replays de 11 à 196 Ko écrits hors du jeu en
  37-71 ms ; gel de fin de partie 125-202 ms ; sortie propre (`exit rc=0`).
  Le joueur ne remarque ni la fin ni le début de partie (315-443 ms, à
  chaque partie, pendant le chargement du niveau).
- ✅ **v0.1.42 — cache des `stat` des replays réparé** : la v0.1.41 n'en
  servait qu'environ la moitié (28-36 ms restaient à chaque fin de partie).
  Ensembles à 4 voies, dossiers gardés (DECISIONS 78). En salon privé : 22
  sur 25 servis depuis la mémoire à partir de la 2e fin de partie, gel 80-84
  ms. `stall.log` et `prof.log` (3,8 et 4,6 Mo) passent en `*.prev.log`
  au-delà de 1 Mo. `unity/prepare_project.py` ne contient plus l'AppId
  officiel (seulement son SHA-256).
- 🟡 Reste en fin de partie : les 25 `stat` de la première sauvegarde d'une
  session (~50 ms), la compression et le code du jeu.

## Sur console, 04/11 soir (v0.1.40) — à-coups en partie

Analyse du `prof.log` de la session de 27 min du 03/11 (6 parties), puis
essais en salon privé, console pilotée par sys-botbase, `.nro` envoyé par le
NetLoader de hbmenu (`nxlink`).

| Moment | v0.1.38 (03/11) | v0.1.40 (04/11) | Cause mesurée |
|---|---|---|---|
| Pendant le jeu | 150-375 ms, ~1 fois toutes les 1-2 min | **aucune image ≥ 100 ms en 6,6 min** (59,8 i/s) | notre `debug.log` : `fflush` → `fsFileWrite` sur UnityMain, la carte SD met parfois 150-375 ms |
| Fin de partie (replay) | 300-700 ms (550-660 ms pour 176-255 Ko) | **354 ms** pour 210 Ko | compression gzip niveau 6 sur UnityMain (97 % de `deflate_slow`), puis écritures SD |
| Début de partie | 0,4-0,6 s | inchangé | `FindObjectsByType` + tri du jeu, premiers shaders/textures |

- ✅ **Journal asynchrone** : `debugPrintf` écrit dans une file en mémoire,
  un thread « log » l'écrit sur la carte (DECISIONS 74). Les lignes
  périodiques `[loop]`/`[gpua]`/`[gfx]` du rendu forçaient une écriture SD sur
  le thread du jeu.
- ✅ **`[debug] zlib_level = 1`** : le niveau de gzip d'IL2CPP est codé en dur
  (6) ; patché à 1 au chargement (DECISIONS 73). Sur un vrai replay :
  3,7× plus rapide, 12 % plus gros.
- ✅ **Mémoire par défaut** `oc_pool_mb = 448`, `gpu_arena_mb = 256` (avant
  640/320, seulement dans le `mvlo.ini` depuis le 03/11) ; la réserve OC
  **s'agrandit** au lieu de laisser Unity écrire dans une page non allouée
  (DECISIONS 72). Vérifié avec une réserve de 128 Mo : 128 → 160 → 192 →
  224 Mo, aucun plantage.
- 🟡 Reste : fin de partie (41 % d'écritures SD, 16 % de compression),
  début de partie, création du salon (0,7 + 0,9 s, sans importance d'après
  le joueur). Mode station (1080p) jamais testé ; la réserve GPU monte à
  219 Mo sur 256 en portable.

## Sur console, 03/11 → 04/11 nuit (v0.1.38 + données Unity 61a2f02 → 44ab3d5)

- ✅ **Arrivées/départs** : `Regex.CacheSize = 128` (DECISIONS 70), 26
  arrivées/départs en 27 min, aucun à-coup.
- ✅ **Crash « Jungle » en ligne** (2 fois) : `[gpua] memalign FAILED
  size=131068 KB`, newlib à court de mémoire contiguë (DECISIONS 71).
  Contourné par `oc_pool_mb = 448` / `gpu_arena_mb = 256` dans `mvlo.ini` :
  ~630 Mo jamais touchés au lieu de 136-168, 5 parties sans échec.
- ❌ v0.1.39 (n'allouer que 64 Mo de ces réservations de 128 Mo) : plantage
  au démarrage, retirée (DECISIONS 71).
- ❌ Préchauffage IL2CPP sur le menu (e4c6472, 44ab3d5) : sans effet sur la
  création du salon, 0,74 + 0,90 s (DECISIONS 75).
- ✅ *Room Settings* ~0,35 s, lancement de partie ~0,55 s. À 60 i/s une image
  de match coûte 20-23 ms de CPU : 40-50 i/s irréguliers ; 30 conseillé.

## Sur console, 02/11 soir (v0.1.31 → v0.1.37) — à-coups des premières fois

Mesuré avec un profileur par échantillonnage (`[debug] prof_ms`, `nx_prof.c`,
`tools/switch/prof_report.py`) dans un salon privé, console pilotée par
sys-botbase ; le match joué à la manette.

| Moment | v0.1.30 | v0.1.37 | Cause mesurée |
|---|---|---|---|
| 1er *Room Settings* | 2,1 s | **1,1 s** | 78 % d'attente du service fs : horodatages à chaque ouverture (27 %), ouvertures (23 %), lectures (21 %) |
| Lancement du 1er match | ~1,9 s | **~1,15 s** (3 images plus courtes) | mêmes ouvertures/lectures, sur UnityMain et les chargeurs |
| En match (objets, etc.) | 190-310 ms, 3 fois en 5 min | **aucun ≥ 150 ms en 21 min** | UnityMain bloqué dans l'envoi UDP de Photon (`bsdSend` lent) |
| Création du salon | 0,7 + 1,0 s | inchangé | IL2CPP initialise ses génériques au premier usage (CPU, pas de fichiers) |

- ✅ `[io] open_timestamps = 0` : `fsFsGetFileTimeStampRaw` (libnx le demande
  après chaque `open`) n'est plus envoyé pour les données du jeu.
- ✅ `[io] preload_kb = 1024` : depuis le menu principal, les 1 540 fichiers de
  `Data/` ≤ 1 Mo (38,7 Mo, ~7 s en arrière-plan) sont gardés **en mémoire**
  (`nx_io.c`, « RAM files ») ; leurs lectures ne touchent plus la SD.
- ✅ `[net] async_send = 1` : les datagrammes UDP partent par une file et un
  thread d'envoi ; le jeu n'attend plus le service réseau. 21 min de match,
  aucune déconnexion.
- ✅ Le gel en ligne reste corrigé : ce match de 21 min a dépassé de loin les 400 s.
- ✅ **v0.1.38, `[io] phantom_fds = 1`** : un fichier préchargé s'ouvre sans
  aucune requête fs (descripteur 3072-4095 servi depuis la mémoire). *Room
  Settings* **1,1 s → 0,49 s** (202 ouvertures = 29 ms), lancement du match
  ~1,15 s → ~0,9 s.
- ✅ Joué en public dans le groupe officiel (`[photon] build_identifier =
  official`, partie à 6) : ça tourne. Arrivées/départs de joueurs en pleine
  partie : à-coups de 127-245 ms (voir DECISIONS 69).
- 🟡 Reste : création du salon (0,7 + 1,0 s, IL2CPP) ; arrivées/départs.

## Sur console, 02/11 (v0.1.27 → v0.1.30) — gel en ligne CORRIGÉ

Console pilotée depuis le PC par **sys-botbase** (`tools/switch/sbb.py` :
boutons, captures d'écran), essais surveillés par `tools/switch/freeze_watch.sh`
(battement, autopsie, réponse de sys-botbase). fw 22.1.0, AMS 1.11.1.

| Essai | Version, réglage | Résultat |
|---|---|---|
| 1. `udp_soak` → PC du réseau local (libnx brut, 20 paquets/s), menu | 0.1.27 | pas de gel en 24 min |
| 2. « Jouer » + `udp_unconnected = 1` | 0.1.27 | **gel** à ~400 s cumulées (US 51 s puis EU) |
| 3. « Jouer » + `chiaki_cfg = 1` (16 sessions bsd acceptées par la console) | 0.1.27 | **gel** à ~400 s |
| 4. `udp_soak` par notre couche socket → DNS 1.1.1.1 (Internet), 5/s, menu | 0.1.28 | pas de gel en 10 min |
| 5. « Jouer », compteur de requêtes IPC par service | 0.1.29 | **gel** à ~400 s : cause trouvée |
| 6. « Jouer », `/dev/urandom` servi par le CSPRNG | **0.1.30** | ✅ **pas de gel en 11 min 40** |

- ✅ **Cause** : `/dev/urandom` était émulé par un **fichier de 64 Ko** sur la
  SD (`.synth_dev_random`, `open_fake`), lu par le service fs. IL2CPP garde le
  fd ouvert ; Photon y puise ~160 octets/s une fois connecté ; vers **~400 s**
  le fichier arrive à la fin et `il2cpp::os::Cryptography::FillBufferWithRandomBytes`
  reboucle sur `read() == 0` : **~7 000 lectures fs/s** (autopsie : 129 520
  requêtes fs contre ~23 000 attendues). fs (cœur 3) sature et **affame les
  autres services du cœur 3** : réseau de **toute la console** (sys-botbase
  refuse les connexions, le ping échoue), nifm ; `UnityMain` reste bloqué dans
  une écriture fs → image figée. Tout repart quand notre processus meurt.
  Explique tout : compte cumulé par processus (le fd reste ouvert), rien hors
  connexion (le menu ne tire pas d'aléatoire), remise à zéro à chaque
  lancement, le soak qui ne gèle jamais.
- ✅ **Correctif (v0.1.30)** : `read_fake` sert les fd de `/dev/urandom` et
  `/dev/random` avec `randomGet()` (sans fin, sans accès SD). Dans « Jouer »,
  les requêtes fs retombent de 46/s à 7,5/s (le niveau du menu).
- ❌ Les trois correctifs de la v0.1.27 (`udp_unconnected`, `chiaki_cfg`,
  `photon_rebind_s` non essayé) ne changeaient rien : laissés, off par défaut.
- 🟡 Diagnostics ajoutés (v0.1.28-29) : `[debug] udp_soak_layer/_dns/_hz/_reuse`,
  log des `setsockopt` (Photon ne pose que `SO_REUSEADDR`), **compteur de
  requêtes IPC par session** (`ipc/10s:` dans le battement, totaux dans
  l'autopsie). `udp_echo.php` affiche enfin son débit sous trafic continu.
- 📋 À confirmer : un vrai match de plus de 7 min.

## Sur console, 29/10 tard (v0.1.23 → v0.1.25) — le gel cerné

Sept essais sur console (fw 22.1.0, salon créé seul, région eu). Gel =
dernière image ; « connexion » = `[Network] Successfully connected to
region` (entrée dans « Jouer », pas le menu principal, qui ne se connecte
pas).

| Essai | Version, réglage | Connecté jusqu'au gel | Requêtes bsd au gel |
|---|---|---|---|
| Référence (match 30 étoiles) | 0.1.23, défaut | 394 s | 162 504 |
| B | 0.1.23, `socket_config = 1` | ~394 s (image 11 798 contre 11 759) | 162 849 |
| Flood sur le menu, sans Photon | 0.1.24, `bsd_flood = 2000` (polls 0 ms) | pas de gel (4 min, 400 000 requêtes) | — |
| Écran « Jouer » seul, aucun salon | 0.1.24 | 396 s | **85 013** |
| Connexion 6 s puis menu principal 8 min | 0.1.24 | pas de gel | — |
| Même lancement : 109 s connecté, B 5 s, reconnecté | 0.1.24 | gel 285 s après la reconnexion = **400 s cumulées** (399 s depuis la 2e connexion) | — |
| `local_wait = 1`, écran « Jouer » | 0.1.25 | 409 s | 442 000 (polls 0 ms) |

Établi :
- **Le gel vient après ~391-409 s de connexion Photon ouverte**, avec ou
  sans salon, match, Quantum ou replays (les sessions d'avant gelaient
  aussi avec plusieurs petits matchs dont les replays sont enregistrés).
  Une coupure de 5 s ne remet pas le compte à zéro ; une connexion de 6 s
  suivie de 8 min déconnecté ne déclenche rien.
- **Pas le nombre de requêtes** (85 000 au gel dans « Jouer », 162 000 en
  match, 400 000 sans gel en flood), **pas le temps d'attente dans bsd**
  (`local_wait` : plus aucune attente dans le service, gel quand même),
  **pas les requêtes mappées** (*pointer buffer* bsd **0x1000** sur console,
  17 requêtes mappées sur 162 000, aucune près du gel), **pas les tampons**
  (B), **pas de fuite d'objets noyau** (`rl:` plat jusqu'au gel : threads
  65/96, tmem 3/32, sessions 1/1). Pas de `FIONREAD refused`.
- Au gel, **bsd s'arrête en premier** : le fil de réception Photon reste
  13,2 s dans un poll **de 0 ms** (avec `local_wait`), le battement 13,2 s
  dans `sendto`, UnityMain 12,4 s dans `send` ; puis **fs** (watchdog 12,3
  s, un worker IL2CPP 10 s, UnityMain 13,7 s dans l'écriture de `debug.log`
  dans un autre essai) et **nifm** ; `am`, `time`, `ts`, `nvdrv` répondent
  en 0-1 ms. Une fois (écran « Jouer »), les services sont repartis après
  ~12 s, le jeu a rendu une image puis s'est rebloqué.
- Les 128 dernières opérations bsd avant le gel sont normales (petits
  paquets UDP Photon, réseau presque muet dans « Jouer »). Une fois, 7
  paquets Photon de 24 octets sont arrivés ensemble ~2 s avant.

Piste ouverte (non prouvée) : **un service système tourne en boucle** à
cause de notre socket UDP connectée après ~400 s (bsd lui-même ?) et prive
de CPU les services de priorité plus basse du cœur système (bsd, fs, nifm,
puis hid) ; notre sortie ferme la socket et tout repart. Ou un événement
du serveur Photon vers ~400 s que la pile réseau de la console gère mal.

Ajouté : `[debug] bsd_flood` / `bsd_flood_op` (DECISIONS 56),
`[net] local_wait` (DECISIONS 57), version 0.1.25 (build local).

## Nuit du 30/10 (v0.1.27, session cloud) — correctifs candidats

Suite de la recherche (DECISIONS 61, sources) et **trois correctifs à tester**
demain, tous derrière un interrupteur `[net]`, défaut off ; build propre,
auto-test Eden OK (chiaki config appliquée, 16 sessions acceptées sous Eden,
`udp_unconnected` actif, pas de plantage ; défauts off = zéro changement).

- 🔎 **sys-ftpd #30** : l'ENOBUFS (« hangs the system ») vient de **transferts
  actifs** avec tampons trop petits, pas d'une socket oisive. Les sources ne
  décrivent **aucun** mécanisme par lequel **une** socket UDP connectée drainée
  pourrit avec le seul temps (réception bornée puis drop ; ICMP = un `so_error`
  unique ; poll ne retient rien ; tmem fixe). Un gel ENOBUFS à ~400 s viendrait
  plutôt d'une **fuite de sockets/fds** ou d'une **famine de RAM système**.
- 🔎 **chiaki-ng** (analogue UDP temps-réel) : udp 256 Ko, `sb_efficiency 8`,
  **16 sessions** (max libnx ; la console en a 15), sans nifm.
- 🟡 **Livré (v0.1.27)** :
  - `[net] chiaki_cfg` : config socket chiaki (gros tampons UDP, 16 sessions,
    repli si refus) ;
  - `[net] udp_unconnected` : garder la socket UDP **non connectée** au niveau
    bsd (sendto/recvfrom filtré) — le correctif le plus probable si c'est la
    socket connectée qui pourrit ;
  - `[net] photon_rebind_s = N` : socket fraîche sous le même fd à N s (nouveau
    port source ; **risque** de déconnexion Photon) ;
  - **errno bsd par commande** (`err: ENOBUFS=…` dans le battement, table dans
    l'autopsie) et **`N sock open`** dans le battement (fuite de fd = compte qui
    grimpe).
- 📋 Essais console : TEST.md « 4 octies » (mesures 1-2 puis correctifs a-c,
  chacun ≤ 8 min, « Jouer » seul). L'essai « mode applet » est retiré
  (lancement toujours en title takeover).

## Nuit du 30/10 (v0.1.26, session cloud) — recherche + instrumentation

Recherche approfondie du gel ~400 s (4 sous-agents, sources dans DECISIONS 58 :
libnx, Mesosphère, sources Photon du jeu, switchbrew, issues). Résultats :

- 🔎 **Photon n'a aucun événement à ~400 s** : tous ses timers sont ≤ 60 s
  (service 10 ms, ping 1 s, LobbyStats 60 s, DisconnectTimeout 10 s) ou = 1 h
  (jeton). Quantum 3.0.13 / Realtime 5.1.15. Le déclencheur n'est donc **pas**
  un événement applicatif Photon → c'est très probablement la **pile réseau de
  la console** face à notre socket UDP connectée de longue durée.
- 🔎 **Pourquoi bsd d'abord, puis nifm** : le processus `bsdsocket` héberge bsd
  **et le DNS (sfdnsres)** ; nifm (séparé) fait son test de connexion via
  bsdsocket (libcurl+socket/ssl). bsd tombe → nifm se bloque. **fs** (séparé,
  cœur 3, sans réseau) qui gèle aussi n'est pas expliqué par le réseau.
- 🔎 **Ordonnanceur strictement prioritaire par cœur, sans partage de temps
  entre priorités** : une famine est possible, **mais serait immédiate, pas à
  400 s** ; aucune fuite d'objet noyau/session/thread trouvée pour une seule
  socket. FS est sur le cœur 3, nous sur 0-2. **Aucun mécanisme simple
  n'explique seul le délai de 400 s** (conclusion honnête, DECISIONS 58f).
- 🔎 **nifm déclassé** : `socketInitializeDefault` n'appelle pas nifm et quasi
  aucun homebrew ne soumet de requête nifm (la norme) ; le correctif nifm n'est
  donc pas codé. Analogue le plus proche : chiaki-ng (16 sessions bsd, gros
  tampons UDP). Depuis 18.0.0, **15 sessions bsd:u pour toute la console**.
  RetroArch Switch gèle « après 10-15 min » en **mode applet**, pas en mode
  application → test gratuit ajouté.
- 🟡 **Livré (v0.1.26, défauts à off, build propre, auto-test Eden OK)** :
  - `[debug] photon_ring` : anneau des 30 dernières s de datagrammes Photon
    (sens, taille, 16 premiers octets) dans l'autopsie et `debug.log` ;
  - `[debug] udp_soak = ip:port` (+ `udp_soak_connect`) : imite le trafic
    Photon **sans Photon** vers un écho PC — l'expérience décisive
    pile-réseau / Photon ;
  - `tools/switch/udp_echo.php` : l'écho UDP côté PC (comme netlog, en PHP).
- 📋 **Spec** (non codée, filtre de sécurité, pour la session locale) : mesure
  au gel si bsd/fs/nifm sont *runnable mais affamés* ou *bloqués en IPC*
  (svcDebugActiveProcess via pmdmnt), et correctifs candidats (poll_yield,
  renouvellement de socket) — DECISIONS 59.
- 📋 **Essais console** dans TEST.md « 4 octies » : 1) udp_soak sur le menu,
  2) applet vs application, 3) photon_ring au gel, 4) bsd_sessions A/B.

## Soir du 29/10 (v0.1.23, session cloud) — mesures et essais A/B du gel

- 📏 **Requêtes bsd mesurées** (`nx_bsdmon.c`, DECISIONS 54) : par commande
  (nombre, échecs, octets, temps, **mappées** ou copiées), anneau des 128
  dernières opérations ; dans le battement (`bsd:`), l'autopsie et
  `debug.log` (`[net] bsd requests per command`, toutes les 1800 images).
  Limites de ressources du processus dans le battement (`rl:` threads,
  événements, transfer memory, sessions, mémoire).
- 🔀 **Interrupteurs `[net]`** pour les essais A/B, tous à l'ancien
  comportement par défaut : `small_ipc`, `socket_config`, `bsd_sessions`,
  `poll_slice_ms = 0`.
- 🔎 Lu : *pointer buffer* bsd 0x8000 sous Eden ; notre repli `FIONREAD`
  lisait 64 Ko (toujours mappé) ; un processus `sysres=0` prend ses tables
  de pages dans le tas partagé avec les processus système (Mesosphère).
  Pistes, pas de cause prouvée.
- ✅ Sous Eden (CI) : 10 min de jeu + relancement avec `small_ipc = 1`,
  auto-tests et GC forcé `PASSED`, 1172 battements avec `bsd:` (1429
  requêtes, 0 mappée : sans AppId, peu de trafic sous Eden).
- ⏸ Regroupement des petits fichiers de `Data/` : reporté (DECISIONS 55).

## Sur console, 29/10 au soir (v0.1.20 → v0.1.22)
v0.1.20 et v0.1.21 installées et testées : Room Settings ~3 s, stats
presque gratuits (frame 484 : 1904 stats en 15 ms), il reste ~250
ouvertures de fichiers ouverts une seule fois (4,5 ms chacune) et le
« other » d'Unity aux premiers affichages. La suspension forcée du GC n'a
jamais servi en vrai (`forced 0` sur toutes les sessions).

**Le « gel de toute la console » vient de notre processus.** Cinq
reproductions :
- toujours **~390-420 s après la première connexion à Photon** (0.1.18 :
  397 s ; 0.1.20 : 391 ; 0.1.21 : 395 ; 0.1.22 : 402 et ~419), pas après le
  lancement : 10 min sur le menu principal sans réseau (133 appels) n'ont
  rien donné, puis gel 7 min après le début de la partie ; à ce moment
  **157 à 162 milliers d'appels réseau** (~380/s en ligne) ;
- autopsie (DECISIONS 51) au moment du gel : `bsd` (UnityMain dans
  `bsdSend` 12,9 s, le thread réseau dans `bsdPoll`, le battement dans
  `sendto`), `fs` (le watchdog dans `fsFsCreateFile` 10,9 s, sonde SD sans
  réponse) et `nifm` ne répondent plus ; `am`, `time`, `ts`, `nvdrv`
  répondent en 0-1 ms ; `hid` (vibration) s'arrête un peu plus tard ; les
  services repartent parfois ~1 s (HOME traité des minutes après) ;
- **dès que notre processus se termine** (`stall_exit_s`), la console
  redevient normale tout de suite (FTP, HOME) ;
- écartés : pools mémoire de la console (stables au Ko près jusqu'au gel),
  fuite de sockets (20 en 17 min), cœur 3 (hbloader limite aux cœurs 0-2),
  `poll_slice_ms = 40` (le débit d'appels ne baisse pas).
Piste : ce qui bloque, ce sont les requêtes qui font mapper notre mémoire
au service (gros tampons : écriture fs, send/poll bsd) ; les petites
requêtes passent. Prochaine mesure : les ~100 dernières opérations réseau
dans l'autopsie, et le nombre de requêtes bsd par commande.

## Soir du 28/10 (v0.1.21) — à vérifier sur console

- 🟡 **Stats « servies par le cache » encore lentes : cause trouvée.**
  Chaque stat déclaré « absent » (≈ 60 %) refaisait un vrai stat libnx du
  nom de base dans le dossier du jeu (`basename_fallback`). Mémorisé
  maintenant servi par une liste du dossier du jeu, refaite seulement après
  une création dans ce dossier (DECISIONS 48).
- ✅ Sous Eden : 2807 replis « nom de base » servis par la liste, **0 vrai
  stat** ; image 1 : stats 116 ms pour 3087 (672-939 ms avant).
- 🟡 **`keep_open`** : la LRU de la v0.1.20 reste la meilleure mesurée
  (deux autres politiques essayées, moins bonnes) ; la réserve rétrécit
  maintenant d'elle-même si le service FS refuse une ouverture
  (DECISIONS 49).
- ❌ **Autopsie du gel** et **profileur par échantillonnage** : non faits
  dans la session cloud (interrompus par le filtre de sécurité, DECISIONS
  50). La vibration de la v0.1.20 a prouvé que notre processus vit pendant
  le gel complet.

## Nuit du 28 au 29/10 (v0.1.20) — à vérifier sur console

- 🟢 **Blocage n° 1 (GC / Quantum) : corrigé, validé sous Eden.** Un thread
  qui n'atteint pas de point sûr 30 ms après le signal du GC alors qu'il
  exécute du code du jeu est mis en pause (`svcSetThreadActivity`), ses
  registres sont recopiés sous son SP et un thread proxy exécute le vrai
  gestionnaire de bdwgc sur sa pile, à sa place (DECISIONS 45,
  `[debug] gc_force_ms`). Auto-test `smoke_test = 3` : 5e thread en boucle
  sans aucun appel → 5/5 tours (sans la correction : ack manquant, 3 s
  d'attente). Dans le jeu sous Eden (`gc_spin_test = 900`) :
  `il2cpp_gc_collect` revient en 90 ms avec 1 livraison forcée pendant
  qu'un thread attaché à IL2CPP boucle — `PASSED` — puis 10 min de jeu
  normales.
- 🟡 **Blocage n° 2 (gel de la console) : pas de cause, mais le battement
  ne dépend plus du verrou de malloc** (formateur entier `nx_bfmt`), compte
  les `sendto` lents/en échec, et donne l'état du GC, les appels noyau de
  mappage mémoire et les ioctls nv en cours (`nx_kmon.c`). Nouveau témoin
  sans réseau ni SD : la manette vibre deux fois toutes les 5 s si le jeu
  est figé depuis 12 s et que le processus vit (`[debug] stall_rumble_s`).
- 🟡 **Ouvertures de fichiers** : les fichiers de données en lecture seule
  fermés par Unity restent ouverts (64 au plus) et sont rendus à la
  réouverture, sans aller-retour FS (`[io] keep_open`, DECISIONS 47). Sous
  Eden : 213 réouvertures servies sans la SD au démarrage (sur ~960
  ouvertures), aucun fichier manquant, 10 min de jeu normales.
- ✅ Sous Eden (CI) : 1171 battements reçus par l'écoute du runner en 10 min
  (`gc:`, `kmem:`, `nv:` présents) ; les « failed » nv étaient des
  expirations normales d'attente d'événement GPU (`0xc010001e`), comptées à
  part depuis (`timeouts`).
- 🔧 CI : depuis l'ajout de `[io]` en fin de `mvlo.ini`, les lignes ajoutées
  par emu-test (`autopress`, `hitch_ms`…) tombaient dans `[io]` et étaient
  ignorées ; elles retombent dans `[debug]`. emu-test écoute aussi le
  battement (`netlog = 127.0.0.1:9999`, docker `--network host`).

## Bugs trouvés et corrigés grâce à l'émulation / l'analyse
1. `initJni` prend **5 arguments** dans Unity 6000.3 (cloverpit : 4).
2. Le GC Boehm de cloverpit (ack sans handler) pouvait faire `ABORT("sp not set")`
   → émulation de signaux POSIX ; puis interblocage possible sur un mutex
   contendu → toutes les attentes rendues interruptibles.
3. errno : les shims renvoyaient des valeurs newlib (ETIMEDOUT=116) à du code
   bionic qui attend Linux (110).
4. `fatal_error()` n'écrivait rien dans le log ; `DEBUG_LOG 0` désactivait tout.
5. Patches de code via alias `svcMapProcessMemory` → désormais écrits avant le
   mapping (fonctionne aussi sans vrai handle de processus).
6. 4 imports libunity 6000.3 manquants (`writev`, `fseeko64`, …).
7. Dialogues d'erreur Unity (modaux Java) → bloquaient sans rien dire ;
   désormais journalisés et fermés automatiquement.
8. Variables de condition newlib non interruptibles → interblocage GC
   reproduit par l'auto-test, réécrites.
9. Unity 6000.3 n'écrit pas `data.unity3d` (fichiers sérialisés séparés) :
   la vérification des données au démarrage l'exigeait à tort.

## Points ouverts / risques connus
- Performances réelles et audio jamais mesurés sur console.
- Réseau en ligne : implémenté (nx_net.c) mais jamais testé contre Photon.
- Audio : patches FMOD/OpenSL de cloverpit re-dérivés, non écoutés.
- Sans AppId Photon dans `mvlo.ini`, le mode en ligne ne peut pas se
  connecter (le jeu le signale : `photon.appid_quantum MISSING`). Voir TEST.md.
