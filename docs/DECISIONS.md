# Journal des décisions (mode nuit)

Chaque entrée : la décision, pourquoi, et comment revenir en arrière.

1. **Tag v2.2.0.1** (= HEAD du repo officiel au 25/10/2026). Paramétrable
   (`game_ref` du workflow android-build).

2. **Le code du jeu n'est jamais copié ici.** La CI fait `checkout` du tag
   officiel et applique `unity/prepare_project.py` : 5 lignes modifiées dans 3
   fichiers C# + l'AppId vidé dans `PhotonServerSettings.asset`. Le script
   échoue si un motif ne correspond pas exactement une fois.

3. **AppId Photon** : l'AppId officiel est aussi en dur dans
   `NetworkHandler.cs`. Remplacé par `NxPort.NxConfig.AppIdQuantum`, lu dans
   `sdmc:/switch/mvlo/mvlo.ini` (`[photon] appid_quantum`). Le script vérifie
   qu'il ne reste plus l'AppId officiel nulle part dans `Assets/`.

4. **Authentification officielle contournée** (`photon.use_official_auth = 0`) :
   elle passe par une auth custom liée à l'AppId officiel (note : `UnityWebRequest`
   fonctionne en fait, les news du jeu sont bien téléchargées sous Eden). Avec votre propre AppId,
   l'auth anonyme Photon suffit ; un UserId persistant est généré
   (`PlayerPrefs nxport_userid`). Le vérificateur de mises à jour (GitHub API)
   est aussi désactivé par défaut.

5. **Build Android : IL2CPP, ARM64 seul, GLES3 seul, Activity, pas de rendu
   multithread, pas de Swappy, textures ETC2, symboles Public.** Tout est fixé
   dans `unity/Editor/NxPortBuild.cs` (pas dans les ProjectSettings du jeu).
   ETC2 plutôt qu'ASTC : obligatoire en GLES3, toujours décodable par mesa.

6. **APK gardé dans le cache Actions** (pas en artifact) : le cache ne compte
   pas dans le quota de stockage des dépôts privés (500 Mo en gratuit). Seul le
   dossier SD final est publié en artifact (30 jours). ⚠ un cache non utilisé
   7 jours est effacé : relancer `android-build` (≈ 2 min si le cache existe,
   ≈ 1 h sinon).

7. **Offsets libunity dérivés par symboles**, pas par empreintes :
   `tools/re/derive_unity.py` sur le player Unity extrait de l'image Docker
   `unityci/editor:ubuntu-6000.3.10f1-android-3` (même BuildID que l'APK).
   Le wrapper vérifie le BuildID puis chaque mot de garde avant d'écrire.
   Le packaging CI vérifie aussi le BuildID de la libunity de l'APK.

8. **Patch de granularité mémoire 256→64 Mo** : 21 sites trouvés dans les
   mêmes 11 fonctions que cloverpit/pvz_fusion (règles de transformation
   reproduites ; le script exige exactement 21 sites).

9. **Temps/vsync** : au lieu d'incrémenter seulement le compteur vsync
   (cloverpit), un thread 60 Hz appelle la fonction du moteur
   `SetFrameTimeNanos(t)` — exactement ce que fait le Choreographer Android.
   Ça débloque `WaitVSync` **et** fait avancer l'horodatage d'où Unity tire
   `Time`. Le détour wall-clock de `TimeManager::Update` est gardé en plus
   (désactivable : `time_hook = 0`). Les hooks il2cpp de `Time`/`Screen` de
   cloverpit (adresses propres à leur jeu) sont supprimés.

10. **GC : émulation de signaux POSIX** (nx_signals.c) au lieu du pont
    « ack sans handler » de cloverpit, qui laisse `stop_info.stack_ptr` vide
    (bdwgc d'Unity : `ABORT("GC_push_all_stacks: sp not set!")`). Le vrai
    handler bdwgc tourne sur le thread cible au prochain point sûr (attentes
    déjà découpées en tranches ≤ 16 ms). Secours : `gc = disable`.

11. **Entrées par pont C#** (DllImport `nxbridge` → `nx_bridge.c`) plutôt que
    `nativeInjectEvent` (qui exigerait des MotionEvent Java factices complets).
    Manettes 1–4 → `Gamepad` virtuels de l'Input System, écran tactile →
    `Touchscreen` virtuel, vibrations HD. **Disposition A/B « nintendo » par
    défaut** (A = valider) ; `ab_layout = xbox` pour la disposition par position.

12. **Sockets** : chaque socket est non bloquant côté libnx ; le blocage est
    émulé par tranches de 10 ms (respecte `SO_RCVTIMEO`, et laisse passer le GC).
    IPv6 refusé proprement (la Switch n'en a pas) → Photon utilise IPv4.
    Tampons UDP agrandis (Quantum envoie des rafales).

13. **Résolution** : `auto` = 1080p en dock, 720p en portable (cloverpit
    forçait 1080p partout).

14. **Logs toujours actifs** (cloverpit compilait `DEBUG_LOG 0` → aucun log).
    Horodatage, niveaux, `debug.prev.log` conservé, `log_sync = 1` pour le
    mode « tout écrire immédiatement ». `fatal_error()` écrit désormais aussi
    son message dans le log.

15. **Pas de handle de processus** (lancement hors hbloader, ex. émulateur) :
    repli sur le pseudo-handle `CUR_PROCESS_HANDLE` au lieu d'une erreur fatale.

16. **Test en émulation dans la CI** (`emu-test.yml`, Eden + lavapipe) : les
    artifacts ne sont pas téléchargeables depuis mon environnement, et les
    données du jeu ne doivent pas en sortir ; le job imprime `debug.log`.

17. **Supprimé de cloverpit** : hooks Rewired/UnityEngine.Input, lecteur vidéo
    ffmpeg (MvLO n'a pas de VideoPlayer), Firebase, PlatformAPI Panik, DLC,
    couche curseur/gyro (nx_pointer). Ajouté : nx_compat.c (4 imports
    libunity 6000.3 absents des tables 6000.2).

18. **Icône** : générée (texte « MvL ONLINE NX »), aucun asset Nintendo.

19. **Auteur des commits** : `Jonathan8520`, sans ligne Co-Authored-By
    (consigne explicite de l'utilisateur, prioritaire sur le réglage par défaut).

20. **Build Android n°1 perdu** (« hosted runner lost communication », runner
    affamé) : le log s'arrête pendant le *bake* automatique Quantum de la scène
    CustomBeach, après que j'ai fait supprimer le swap du runner. Relance avec
    12 Go de swap, conteneur Unity limité à 13 Go (échec propre plutôt que
    runner perdu), journal mémoire/disque, et `AutoBuildOnBuild: 0` (les
    données de map sont déjà bakées et commitées en amont ; elles le sont aussi
    à chaque sauvegarde de scène). `NXPORT_QUANTUM_BAKE=1` rétablit le bake.

21. **Build Android n°6 réussi** avec 4 Go de swap (12 Go remplissaient le
    disque) et un conteneur limité à 7 Go. 506 imports résolus, 0 manquant,
    BuildID libunity identique à la table de patches.

22. **Pas de `data.unity3d`** : Unity 6000.3 sans compression LZ4 écrit des
    fichiers sérialisés séparés (`globalgamemanagers`, `level0`,
    `sharedassets*`…). `check_data()` accepte l'un ou l'autre au lieu d'exiger
    `data.unity3d` (vu au premier lancement complet sous Eden).

23. **Premier lancement complet sous Eden** : chargement sans erreur, écran
    titre rendu 10 min d'affilée (~7,7 i/s en rendu logiciel lavapipe, donc
    sans valeur pour la Switch réelle), pont C# connecté, manette détectée,
    FMOD/OpenSL démarré. Pour tester les menus sans écran ni clavier,
    `[debug] autopress = A@65,DOWN@185,...` injecte des appuis sur la manette 1
    (option de test uniquement, vide par défaut).

24. **Premier essai console (v0.1.0)** : écran fixe 105 s puis intro avec
    son, puis plantage au bout de ~25 images. Deux causes trouvées dans
    `debug.log`/`stall.log` :
    - les 105 s = Unity qui recopie `assets/bin/Data/Managed/` vers
      `<filesDir>/il2cpp/` (« Extracting il2cpp resources », une fois par
      build, contrôlé par `il2cpp/unity.ver` = `unity_app_guid`). Désormais
      fait par `tools/package_sd.sh` ; la CI vérifie que le jeu ne réécrit rien.
    - le plantage = manque d'espace d'adressage : Unity réserve ~1,8 Go
      (n'en utilise que ~80 Mo). Sur console la zone de pile est découpée en
      5 trous et seul le plus grand (512 Mo) servait de fenêtre
      d'« overcommit » (sous Eden : un seul trou de 1,5 Go, d'où l'absence du
      problème). La fenêtre couvre maintenant toute la zone de pile, les
      morceaux déjà occupés sont marqués et contournés, une réservation peut
      s'arrêter juste avant un obstacle, et chaque page connaît sa
      réservation (un `munmap` ne libère jamais celles d'une voisine).
      Les 256 derniers Mo de la zone restent hors fenêtre : libnx y place les
      piles des threads (première version sans marge : Eden assert
      `virt_addr != 0`, création de thread bloquée, écran noir).

25. **60 i/s** : le jeu met `Application.targetFrameRate = -1` (« Max FPS »
    illimité par défaut) et le player Android d'Unity en fait 30. Plutôt que de
    patcher le C# (rebuild Unity d'1 h), `dlsym_fake` enveloppe
    `il2cpp_add_internal_call` et remplace uniquement le setter
    `UnityEngine.Application::set_targetFrameRate` : une valeur ≤ 0 devient
    `[video] target_fps` (60 ; 0 = comportement d'Unity), une valeur choisie
    dans les options du jeu est respectée.

26. **Points sûrs à l'entrée des shims réseau** (v0.1.4). Les signaux du GC
    ne sont livrés qu'aux points sûrs. Un thread qui appelle `poll()` en boucle
    avec un délai ≤ 10 ms sortait de `poll_fake` par le test de délai, avant le
    point sûr de fin de boucle : stop-the-world bloqué pour toujours après
    « Creating a game ». Règle : tout shim qu'un thread peut appeler en boucle
    sans bloquer (non bloquant, `EAGAIN`, délai court) commence par
    `nx_sig_safepoint()`. `pthread_exit` désenregistre aussi le thread
    (signaux + diag), et `nx_sig_thread_register` remet à zéro une entrée
    héritée d'un `pthread_t` recyclé.

27. **Pages de TLS épinglées au démarrage** (v0.1.5). Le noyau mappe une page
    `ThreadLocal` (8 emplacements de 0x200) à une adresse libre au hasard de la
    zone de pile quand toutes sont pleines, et la démappe quand elle se vide
    (mesosphère `KProcess::CreateThreadLocalRegion` /
    `KThreadLocalPage::Finalize`). La fenêtre d'overcommit couvre l'essentiel
    de cette zone : une page posée dans une réservation d'Unity pas encore
    engagée fait échouer `svcMapMemory` (0xd401) au moment de l'engager. Avant
    d'armer la fenêtre, `tls_anchor_setup()` lance `debug.tls_anchor` (64)
    threads libnx et garde un thread vivant par page créée : les threads
    suivants prennent les emplacements libres de ces pages au lieu d'en faire
    créer de nouvelles. Priorité = celle du thread appelant (0x3F n'est valide
    que sur le cœur 3, cf. libnx `thread.h`).

28. **PlayerPrefs aiguillés par le receveur** (v0.1.7). Unity 6000.3 charge
    `android/content/SharedPreferences` & co. par un ClassLoader : le faux JNI
    rend `java/lang/Object` pour ces classes, donc les method IDs ne portaient
    pas le nom attendu par `unity_jni.c`. `jni_fake.c` route désormais vers
    `unity_jni.c` quand le receveur est l'un de ses handles
    (`unity_handle_class`), `GetObjectClass` rend la vraie classe de ces
    handles, et `getSharedPreferences` (receveur = le contexte) est routé par
    nom vers un handle `UJ_PREFS`. Stockage inchangé : `prefs.kv` sur la SD.
    v0.1.8 : les clés passent par `String(byte[], String)` puis
    `Uri.encode/decode`, eux aussi chargés par `Class.forName` : reconnus
    désormais par leur signature (`([BLjava/lang/String;)V`,
    `(Ljava/lang/String;)Ljava/lang/String;` + nom `encode`/`decode`).
    Sans ça, toutes les clés sauf `__UNITY_PLAYERPREFS_VERSION__` arrivaient
    vides et étaient ignorées.

29. **X/Y par étiquette** (disposition `nintendo`). Le jeu nomme les boutons à
    la Xbox (`buttonWest` = « x », `buttonNorth` = « y », Input System
    `Gamepad.cs`) : Switch X → West, Switch Y → North, comme A → South et
    B → East. La disposition `xbox` (par position) ne change pas.

30. **`[photon] build_identifier`**. Photon ne met ensemble que les joueurs de
    même AppId **et** même AppVersion =
    `GameVersion.ToStringIgnoreHotfix()` + `-` + `BuildIdentifier.asset`
    (`vanilla` dans les sources publiques, `official` dans les versions
    officielles). Un patch de `prepare_project.py` fait passer le suffixe par
    `NxConfig.BuildIdentifier()` : vide = celui du jeu.

31. **Réglages réels à 0 : la vraie cause était `Screen.resolutions` vide**
    (v0.1.9). `Settings.LoadFromVersion0` (valeurs par défaut du jeu) commence
    par `Screen.resolutions[^1]` ; Unity Android remplit ce tableau avec
    `Display.getSupportedModes()`, auquel le faux JNI ne répondait rien. Le
    tableau vide lève une exception, avalée par le `catch {}` de
    `LoadSettings`, **avant** que HUD Scale (8), les volumes (0,75/0,5/0,75)
    et Max FPS soient posés : ils restaient à 0 et étaient sauvegardés à 0.
    `getSupportedModes` rend maintenant un mode (taille de rendu, 60 Hz) et
    `getRefreshRate` 60. En plus : `getFloat` est routé vers `prefs.kv`
    (il rendait toujours 0), et les zéros déjà sauvegardés pour ces 4 clés
    sont effacés au chargement (0 est hors de chaque curseur du jeu).
    Le HUD minuscule de la console venait de là : le défaut du jeu (8) est
    déjà le maximum du curseur. `[game] hud_scale` permet d'aller au-delà.
    Même cause, très probablement, pour les `ArgumentOutOfRangeException`
    dans `UI/Navigate` : l'option *Résolution* (onglet Graphismes) est une
    liste vide ; gauche/droite fait passer sa valeur à -1 puis 0, et
    `ResolutionOptionLoader.OnValueChanged` lit `resolutions[spo.value]`
    dans le rappel `Navigate`. Avec un mode, la liste a 1 entrée, la valeur
    ne bouge plus et le rappel n'est plus appelé.

32. **`Class.forName` rend la classe demandée** (`[debug] jni_forname`,
    défaut 1). Unity 6000.3 charge toutes ses classes Java par
    `Class.forName(nom, false, loader)` ; le faux JNI rendait toujours
    `java/lang/Object`. Les gestionnaires de champs statiques écrits pour les
    vrais noms (`AudioManager.PROPERTY_*`, `Context.*_SERVICE`,
    `Environment.MEDIA_MOUNTED`…) servent enfin. Par prudence, l'aiguillage
    **par nom de classe** vers `unity_jni.c` est limité aux classes du
    stockage (SharedPreferences, Map/Set/Iterator, AssetManager,
    InputStream) ; Display, Context, Resources et UnityPlayer gardent
    l'aiguillage d'avant (handlers `act_*`).

33. **`Call*MethodA` reçoivent leurs arguments.** Ces variantes (tableau
    `jvalue[]`) appelaient le dispatch sans aucun argument : `va_arg` lisait
    le contenu résiduel des registres. Elles construisent maintenant un vrai
    `va_list` AArch64 depuis la signature (zones de sauvegarde GP/FP + pile,
    réels promus en double). Vérifié sous qemu-aarch64 contre de vrais appels
    variadiques (tous types, débordement > 8 registres).
    `[debug] jni_avariants = 0` revient à l'ancien comportement.

34. **Cache de blocs pour les lectures SD** (`nx_io.c`, `[io] cache`,
    défaut 1). L'ancien cache gardait une fenêtre de 1 Mo par gros fichier
    (≥ 4 Mo), sous un verrou global tenu pendant la lecture SD : chaque saut
    hors fenêtre relisait 1 Mo, et tous les autres threads attendaient. Le
    nouveau : blocs de 64 Ko partagés (32 Mo, LRU) indexés par chemin (un
    fichier rouvert garde ses blocs), lecture anticipée qui grandit quand
    l'accès est séquentiel (jusqu'à 1 Mo), lecture directe pour les grosses
    requêtes, `pread`/`__read_chk`/`__pread_chk` passent aussi par le cache,
    aucun verrou pendant les accès SD. Statistiques par fichier dans
    `debug.log` (`[io] cache:` / `[io]   <fichier> …`) et ligne
    `[io] slow SD read` au-delà de 100 ms. Testé hors console par un test de
    charge à 8 threads sous AddressSanitizer contre le fichier de référence.
    Réglages : `[io] cache_mb`, `block_kb`, `readahead_kb`.

35. **Écran de chargement** (`nx_splash.c`, `[video] loading_screen`,
    défaut 1). Pendant la frame 1 (~23 s sur console), le thread qui porte le
    contexte GL d'Unity attend dans notre shim futex. Là, on le bascule sur
    **notre propre** contexte EGL (même surface, même thread), on dessine,
    on présente, et on remet le contexte d'Unity : l'état GL d'Unity n'est
    jamais touché, et seulement entre la création de sa surface et la fin de
    la frame 1, avant toute image d'Unity. Contenu original (dégradé, titre
    en police pixel, roue de 8 carrés façon « Connecting… » du jeu, barre de
    progression calée sur le volume lu au lancement précédent,
    `.loadstats`).

36. **Clavier Switch** (`nx_kbd.c`, `[input] keyboard`, défaut 1).
    `UnityPlayer.showSoftInput(...)` (appelé par `TouchScreenKeyboard.Open`,
    donc par TMP_InputField) ouvre swkbd dans un thread à part ; la boucle de
    rendu rend le texte à Unity par `nativeSetInputString` puis
    `nativeSoftInputClosed` (ou `nativeSoftInputCanceled`). Sans réponse, le
    clavier « virtuel » d'Unity restait ouvert et TMP recopiait son texte
    (vide) dans le champ à chaque image, ce qui annulait aussi la saisie au
    clavier à l'écran du jeu.
    Garde-fou : le clavier à l'écran du jeu (`OnScreenKeyboard.Update`)
    rappelle `ActivateInputField()` à chaque image tant que le champ n'a pas
    le focus, et TMP rend le focus dès que le clavier logiciel se ferme ;
    swkbd se serait donc rouvert sans fin. En mode 1 (défaut), swkbd ne
    s'ouvre que si un bouton (hors croix) ou un nouveau toucher date de moins
    d'une seconde et n'a pas été vu dans les 250 ms qui suivent la fermeture
    du précédent. Les autres appels sont mis en attente : Unity croit alors
    son clavier ouvert (TMP garde le focus et ne redemande plus), donc le
    prochain A ou toucher (moins d'une minute après, sauf `hideSoftInput`
    entre-temps) ouvre swkbd pour ce champ, avec le texte tenu à jour par
    `setSoftInputStr` (cas : champ atteint à la croix via
    `TMP_InputFieldFixer`, ou A sur le clavier du jeu après swkbd). Mode 2 :
    à chaque appel ; 0 : jamais. `setSoftInputStr`/`hideSoftInput` sont
    journalisés (16 premiers) pour le diagnostic.

37. **Écran de chargement en images** (v0.1.10). Le dessin est sorti dans
    `nx_splash_draw.c`, du GLES2 pur, sans libnx : le même code tourne sur un
    PC (`tools/splash_preview/`, Mesa en logiciel) pour vérifier le rendu
    image par image sans console. Un seul shader : couleur, texture teintée,
    rectangle arrondi anticrénelé (distance signée). Tout est fonction du
    temps, jamais du nombre d'images : l'écran n'a une image que quand le
    thread GL d'Unity attend, donc à intervalles irréguliers.
    Les images (logo du studio, logo du jeu, 5 captures, icône du .nro) sont
    **téléchargées au moment de la compilation** par
    `tools/fetch_splash.py` (liste dans `tools/splash_sources.txt`) et ne
    sont jamais dans le dépôt : même statut que les données du jeu (artefacts
    privés). Conversion : captures en 1280x720 (recadrage), JPEG baseline
    (stb_image ne lit pas le JPEG progressif) ; logo SVG rendu par
    `rsvg-convert` ; icône 256x256. Sur console, un thread de priorité basse
    les décode (stb_image, NEON) et la boucle en envoie au plus une par image
    au GPU : l'écran s'affiche tout de suite et les images arrivent ensuite.
    À la fin du chargement : arrêt du thread puis `eglDestroyContext` de notre
    contexte, ce qui libère les textures (~15 Mo). Garde : `done` est levé
    avant d'attendre `busy`, et un tick lève `busy` avant de lire `done`
    (ordre séquentiel), donc aucun tick ne peut utiliser le contexte détruit.
    Logos pixel art (Infenek, police) : échelle entière en pixels d'écran,
    pour rester nets en 720p comme en 1080p.

38. **Mesure GPU / shaders** (`nx_gpu.c`, v0.1.13). Unity résout toutes ses
    fonctions GL par notre `dlsym` : une table `nx_gpu_exports` (en tête)
    chronomètre `glCompileShader` (GLSL de mesa), `glLinkProgram` (édition
    de liens + GLSL→TGSI), les dessins, textures, tampons, `glMapBufferRange`
    et `glClientWaitSync`. Dans mesa et libdrm (liés statiquement), les
    appels entre fichiers objets passent par `ld --wrap` :
    `nvc0_program_translate` (génération de code nouveau),
    `nvc0_program_upload`, `nvc0_screen_resize_text_area`, et côté libnx
    `nvMapCreate/Close`, `nvAddressSpaceMap/Unmap`, `nvGpuChannelCreate/Close`,
    `nvioctlChannel_KickoffPb`, `nvFenceWait`, `nvInitialize`. Temps imbriqué
    compté une seule fois (par thread). Une image plus lente que
    `[debug] hitch_ms` (100) donne une ligne `[hitch]` détaillée ; toutes les
    2 s une ligne `[gpu]`. Les attentes de fence infinies de libdrm_nouveau
    sont découpées en tranches de 500 ms (même résultat) : au-delà d'une
    seconde, le log nomme la fence et lit la notification d'erreur de chaque
    canal GPU (faute MMU, délai d'inactivité, erreur PBDMA…). Le battement
    du watchdog affiche l'état GPU (objets nvmap, canaux, attente en cours).

39. **Gel de la console : deux causes plausibles neutralisées, le reste
    instrumenté.**
    - *Segment de code des shaders de nouveau* : mesa 20.1 le crée à 512 Ko
      et, à chaque débordement, évince tous les shaders, réalloue le double
      (1, 2, 4, 8 Mo) et réenvoie les shaders liés ; à 8 Mo il évince tout à
      chaque débordement. Le code (`nvc0_program.c`) porte un commentaire sur
      des fautes de page GPU en fin de segment, et des jeux plantent sur
      « out of code space, evicting all shaders » (bugzilla nouveau). La 2e
      partie charge de nouveaux shaders : moment suspect. `[video]
      shader_code_mb` (16) : au premier débordement, saut direct à 16 Mo
      (l'appel interne qui crée le segment à 512 Ko n'est pas interceptable).
      Chaque redimensionnement est journalisé.
    - *Transfer memory de nvservices* : libnx donne 8 Mo (fincs l'a montée
      de 3 à 8 Mo en 2018 pour mesa) ; nvservices y range ses objets pour ce
      processus. `[debug] nv_tmem_mb` (32), retour automatique à 8 Mo si
      nvservices refuse. Prise dans le tas (≈520 Mo libres).
    - Non retenu sans preuve : 60 i/s, cache SD, écran de chargement (son
      contexte est détruit à la fin du chargement, journalisé).

40. **À-coups de compilation : pas de cache possible avec le mesa actuel.**
    Vérifié dans les sources devkitPro (`switch-20.1.0-rc3`) : `meson.build`
    désactive le cache de shaders pour `horizon`, et `st_extensions.c`
    n'expose `GL_OES_get_program_binary` que si le cache disque existe —
    donc ni cache mesa, ni cache de binaires de programmes d'Unity. Ce qui
    est fait : mesure (38), CPU à 1785 MHz pendant les compilations
    (`[video] compile_boost`), niveau d'optimisation nouveau réglable
    (`[video] nv_shader_opt`). Ce qui supprimerait vraiment les à-coups :
    reconstruire mesa avec le cache activé et un stockage simple sur la SD
    (proposé, pas fait : à valider).

41. **Écran de chargement dès la 1re seconde** (`nx_splash.c`,
    `[video] early_splash`). Avant qu'Unity crée sa surface EGL (~4,3 s sur
    console), personne n'utilise la fenêtre : écran noir.
    *1er essai, abandonné* : logo dessiné au framebuffer libnx, puis
    `framebufferClose()` juste avant la surface d'Unity. Sous Eden la première
    image d'Unity ne revenait jamais (A/B net) : deux producteurs successifs
    sur l'unique `NWindow` cassent sa file de tampons — déjà vu sur console
    pour une 2e surface EGL, d'où la surface singleton d'`imports.c`.
    *Version retenue* : un seul producteur. Au 1er démarrage, le wrapper de
    `eglCreateWindowSurface` note l'`EGL_CONFIG_ID` choisi par Unity dans
    `mvlo/.eglconfig`. Aux démarrages suivants, juste après l'init vidéo,
    `nx_splash_early_start` crée LA surface de fenêtre avec cette config
    (mesa exige la même config pour contexte et surface), notre contexte
    GLES2, et un thread dessine l'écran de chargement. Quand Unity demande sa
    surface, le thread s'arrête, lâche le contexte, et Unity reçoit la même
    surface ; les ticks habituels continuent ensuite avec le même contexte,
    les mêmes textures et la même horloge (aucun saut visible). Si Unity
    demande une autre config (ne devrait arriver qu'après une mise à jour du
    jeu), la surface est détruite puis recréée — chemin à risque (2e
    surface), mais `.eglconfig` est effacé et le démarrage suivant redevient
    normal. Le tout 1er démarrage après installation reste noir 4 s.

42. **`NullReferenceException` dans `UI/Navigate` (`canceled`) au début de
    la 2e partie.** `OnScreenKeyboard` (clavier du jeu) s'abonne à Navigate
    quand il s'ouvre et ne se désabonne que dans `Close()`. Laissé ouvert
    au lancement d'une partie — fréquent depuis le clavier Switch, qui le
    laisse derrière lui (chat du salon) — l'objet détruit continue de
    recevoir `canceled`. Corrigé deux fois : `prepare_project.py` ajoute le
    désabonnement dans `OnScreenKeyboard.OnDisable` (et dans
    `PauseMenuManager.OnDestroy`, même schéma), et `NxInput` ferme le
    clavier du jeu dès que swkbd rend son texte (`nxbridge_kbd_closed`).
43. **`stat()`/`access()` répondus depuis la liste des dossiers
    (`nx_statcache.c`, v0.1.15-0.1.17).** libnx `fsdev_stat` coûte 5
    allers-retours au service de fichiers (~8,6 ms dans `Data/`, 1570
    fichiers) et Unity en fait ~1700 en ouvrant *Room Settings*, dont plus de
    la moitié sur des fichiers absents. Les données sous `assets/` et
    `il2cpp/` ne changent pas pendant une session : chaque dossier est lu une
    fois (`fsDirRead`), puis taille, type, dates et « absent » viennent de la
    mémoire. Chemins normalisés avant la recherche (`//`, `/./`, `/../` : la
    v0.1.15 ratait tout à cause d'eux), noms comparés sans la casse, tout
    échec de lecture journalisé, le reste (sauvegardes, `files/`, journaux)
    passe par libnx comme avant. Une écriture/suppression invalide le dossier.
    `[io] stat_cache = 0` coupe tout. Rejeté : un index construit au
    démarrage (lecture de 1570 entrées pour rien si le menu n'est jamais
    ouvert) et le cache de la v0.1.13 (`nxio`, données lues, pas les métadonnées).
44. **Heartbeat UDP (`[debug] netlog`, v0.1.18-0.1.19).** Les gels de toute
    la console arrêtent en même temps la boucle de rendu, le watchdog et
    les écritures sur la SD : `debug.log`/`stall.log` finissent avant le gel.
    Un thread libnx (priorité 0x28-0x2C, cœur 2) envoie deux fois par
    seconde à un PC un datagramme : image, temps d'inactivité, attente GPU,
    opération de fichier et appel réseau en cours, threads bloqués ; toutes
    les 2 s température SoC (`ts`), blocs mémoire, ressource système, mode
    CPU boost. Si les datagrammes continuent pendant un gel, le processus
    vit et ils disent où il est bloqué ; s'ils s'arrêtent, c'est la console.
    Écoute PC : `tools/switch/netlog_listen.py`, ou `.php` sous `php-cgi`
    quand le pare-feu Windows bloque python.exe en entrée. Vide = coupé.
45. **Signaux du GC livrés à la place d'un thread bloqué dans le code du jeu
    (`nx_signals.c`, `[debug] gc_force_ms`, v0.1.20).** Le stop-the-world
    de bdwgc attend que chaque thread exécute son gestionnaire de SIGPWR ;
    chez nous il ne s'exécute qu'aux points sûrs de nos shims. Un thread qui
    tourne en code managé sans appel système (Quantum `TaskContext.EndFrame`,
    attente active de ses workers… garés par ce même GC) bloquait tout.
    Vérifié dans Mesosphère : `svcSetThreadActivity(Paused)` est permis sur un
    autre thread du processus et ne rend la main qu'une fois le thread hors de
    tout cœur (`KThread::SetActivity`) ; `svcGetThreadContext3` exige cette
    pause ; aucun « SetThreadContext » hors débogage. Donc, après
    `gc_force_ms` (30 ms) : un thread « forcer » réserve l'enregistrement du
    thread (CAS, il ne peut plus livrer lui-même), le met en pause et lit son
    contexte. Si le PC n'est pas dans un module du jeu (il est dans nos shims,
    libnx, newlib : il peut tenir un de NOS verrous, et atteindra de toute
    façon un point sûr), il est relâché tel quel. Sinon un thread proxy
    recopie x0-x30, PC et q0-q31 sous le SP du thread en pause, bascule sa
    propre pile juste en dessous (`nx_run_on_stack`), prend son registre TLS
    (bloc bionic) et se fait passer pour lui (`pthread_self` importé →
    `nx_sig_pthread_self`), puis exécute **le vrai gestionnaire de bdwgc**.
    Pour bdwgc c'est exactement un cadre de signal : `GC_approx_sp()` tombe
    sur la pile du thread, sous ses registres, et la collection la scanne
    (sources de Unity-Technologies/bdwgc, `pthread_stop_world.c` : sur
    aarch64 le gestionnaire compte sur le cadre de signal pour les registres,
    il n'utilise pas `context`). Le gestionnaire attend le signal de reprise
    dans `sigsuspend` (sur le proxy), revient, et le thread est relancé là où
    il était. Rejeté : (B) accrocher les méthodes de la boucle Quantum —
    ne règle que ce cas ; (C) simulation Quantum sur un seul thread — coût CPU
    et android-build, gardé en réserve. Test : `smoke_test = 3` a maintenant
    un 5e thread qui boucle en assembleur sans aucun appel (5/5 tours sous
    Eden avec la correction, ack manquant et attente de 3 s sans) ;
    `gc_spin_test = N` fait un vrai `il2cpp_gc_collect` à l'image N pendant
    qu'un thread attaché à IL2CPP boucle.
46. **Battement indépendant et témoins de vie (`nx_bfmt.c`, `nx_kmon.c`,
    `nx_alive.c`, v0.1.20).** Le battement n'était pas autonome : le `%.3f`
    de newlib passe par dtoa → Balloc → `_calloc_r` (verrou de malloc), et
    `sendto` est une IPC au module bsd. Tout le battement (et les beacons
    qu'il imprime) passe par `nx_bfmt`, un formateur entier sans allocation ;
    les `sendto` lents (> 50 ms) ou en échec sont comptés dans le datagramme
    suivant. Ajouts : état du GC (threads garés, depuis quand, qui n'a pas
    acquitté, livraisons forcées), appels noyau de mappage mémoire
    (`svcMapMemory`, `svcMapPhysicalMemory`, `svcSetMemoryPermission`,
    `svcSetHeapSize`, …) et ioctls nv (dernier, plus lent, en cours) via
    `ld --wrap` — y compris ceux de libnx elle-même. Sans réseau ni SD :
    `[debug] stall_rumble_s` (12 s) — jeu figé après le démarrage → la
    manette vibre deux fois toutes les 5 s tant que le processus tourne
    (thread libnx qui ne lit que deux compteurs et parle au service HID).
47. **Fichiers de données gardés ouverts (`[io] keep_open`, v0.1.20).**
    libnx `fsdev_open` = `OpenFile` + `GetFileTimeStampRaw` (vérifié dans
    `fs_dev.c` : les dates ne servent qu'à `fstat`, que le cache de listes
    sert déjà pour ces fichiers). Unity ferme et rouvre sans cesse les mêmes
    fichiers. La fermeture d'un fichier en lecture seule sous `assets/` ou
    `il2cpp/` (domaine du cache de listes) garde le descripteur ; la
    prochaine ouverture du même chemin le reprend, rembobiné (`lseek` SET :
    pas d'IPC). Plafond 64 (le plus ancien est vraiment fermé ; newlib en
    permet 1024 : table `handles` de 0x2000 octets dans libsysbase) ; une
    ouverture qui échoue libère tout et réessaie ; une écriture, un
    renommage ou une suppression du chemin ferme ses descripteurs gardés.
48. **Repli « nom de base » servi par une liste (`libc_shim.c`, v0.1.21).**
    Un stat ou une ouverture que le cache des listes déclare « absent »
    (≈ 60 % des stats) passait ensuite par `basename_fallback`, qui faisait
    un vrai `stat()` libnx du nom de base dans le dossier du jeu : au moins un
    aller-retour FS par stat « absent », d'où 0,3-0,46 ms par stat « servi
    par le cache » sur console. Mémoriser les noms absents ne suffisait pas
    (Eden : ~1400 vrais stats au démarrage, surtout des noms différents). Le
    dossier du jeu est petit : il est listé une fois (hachages des noms sans
    la casse, table fixe) et un nom de base qui n'y figure pas est absent
    sans appel FS. La liste est refaite après une création dans ce dossier
    même (ouverture ou `fopen` en écriture, renommage, `mkdir`) ; les
    sauvegardes et fichiers d'Unity dans des sous-dossiers n'y touchent pas.
49. **`keep_open` : politique mesurée (v0.1.21).** Sur console la réserve
    de 64 reste pleine (1016 mises de côté, 217 réutilisations). Deux
    politiques essayées sous Eden, au démarrage : ne garder que les fichiers
    déjà ouverts deux fois → 38 réutilisations (la 1re réouverture de chaque
    fichier rate) ; fermer d'abord le moins souvent ouvert → 112. La simple
    LRU de la v0.1.20 faisait 216 : gardée. La limite d'ouvertures du
    service FS n'est documentée nulle part (le code `OpenCountLimit` existe,
    pas sa valeur) : 64 par défaut, et si une ouverture échoue jusqu'à ce que
    la réserve soit libérée, la réserve rétrécit d'elle-même (journalisé).
    Le rapport compte aussi les fermetures de fichiers ouverts une seule
    fois, pour juger d'une plus grande réserve sur console.
50. **Autopsie du gel depuis le thread de vibration : non faite.** Le code
    (mise en pause de chaque thread, lecture du contexte, sondes de
    services) a été interrompu deux fois par le filtre de sécurité de la
    session cloud ; rien n'en a été gardé. À faire dans la session locale
    si on la veut toujours. Même raison pour le profileur par
    échantillonnage.
51. **Autopsie du gel (`nx_autopsy.c`, v0.1.22, session locale).** Faite
    finalement en local. (a) Un journal des appels système bloquants, une
    case par thread, rempli par `ld --wrap` (`svcSendSyncRequest`,
    `svcWaitSynchronization`, `svcArbitrateLock`,
    `svcWaitProcessWideKeyAtomic`, `svcWaitForAddress`) : appel, session ou
    propriétaire du verrou, depuis quand, appelant. Nécessaire : pour un
    thread arrêté dans un appel système, `svcGetThreadContext3` ne rend ni
    x0 ni les registres volatils, et son PC est l'instruction `svc` elle-même
    (vu sur console). (b) Au gel, le thread de vibration (seul vivant)
    écrit pour chaque thread : journal, contexte, pile ; puis les sessions de
    services du processus, les balises, les 12 derniers battements, et le
    résultat de sondes — un petit appel par service (SD, bsd, time, nifm,
    am, nvdrv, ts), chacune dans un thread créé d'avance. (c) Sortie dans
    `autopsy.log` par un `FsFile` ouvert d'avance (sans newlib) et en UDP
    vers `netlog` ; vibration longue = écrit sur la SD, trois courtes = non.
    Aucun verrou ni allocation une fois armé (`nx_bfmt`). Testé par
    `fake_stall_frame` puis sur le vrai gel. Rejeté : un dump depuis le
    watchdog (il est lui-même bloqué dans le service de fichiers).
52. **Fermeture automatique après un gel (`stall_exit_s`, 90 s).** Thread
    à part qui ne fait que dormir et lire le compteur d'images (le thread de
    vibration finit lui-même bloqué dans le service hid). Le temps passé
    suspendu (menu HOME, veille) ne compte pas : un sommeil de 1 s qui en a
    pris plus de 2,5 remet le compteur à zéro. `svcExitProcess` direct, sans
    nettoyage (newlib peut être bloqué). Sur console : écran « Vous avez
    quitté le logiciel car une erreur est survenue », puis console normale
    tout de suite — c'est ce qui prouve que le gel vient de notre processus.
53. **Pools mémoire de la console dans le battement, `[net] poll_slice_ms`.**
    `svcGetSystemInfo` (app, applet, system, sysunsafe) toutes les 2 s : rien
    ne bouge jusqu'au gel. `poll_slice_ms` (10 par défaut, 1-100) règle la
    tranche des attentes sur les sockets ; à 40 le débit d'appels bsd n'a
    pas baissé (le trafic du jeu domine), donc il ne départage pas encore
    minuterie et nombre d'appels. Laissé à 10.
54. **Requêtes bsd mesurées et interrupteurs réseau (v0.1.23).**
    `nx_bsdmon.c` enveloppe (`ld --wrap`) toutes les fonctions `bsd*` de
    libnx (la couche newlib de libnx et `nx_net.c` y passent) : par
    commande, nombre, échecs, octets, temps, et combien de requêtes ont été
    **mappées** ; plus un anneau des 128 dernières opérations (thread,
    commande, socket, type, pair, taille, drapeaux, résultat/errno, durée).
    Vérifié dans libnx (`cmif.h`, `cmifRequestIn/OutAutoBuffer`) : un tampon
    « AutoSelect » est copié s'il tient dans le *pointer buffer* du service,
    sinon le noyau mappe notre mémoire dans le service ; le *pointer buffer*
    de bsd est journalisé (0x8000 sous Eden). Notre repli `FIONREAD`
    (`.NET Socket.Available`) lisait 64 Ko en `MSG_PEEK` : toujours mappé.
    `[net] small_ipc = 1` garde chaque requête sous cette taille (TCP coupé,
    réception UDP plafonnée, repli `FIONREAD` réduit) ; `socket_config = 1`
    (tampons libnx par défaut, `sb_efficiency` 4 au lieu de 8 : mémoire de
    transfert bsd ≈ 2,2 Mo au lieu de 4,4) ; `bsd_sessions` (1-8, défaut 3 ;
    le wiki switchbrew donne 15 sessions `bsd:u` pour toute la console
    depuis 18.0.0) ; `poll_slice_ms = 0` (une requête par attente, 1 s au
    plus pour le GC). Tous à l'ancien comportement par défaut.
    Limites de ressources du processus dans le battement (`rl:`, toutes les
    2 s) : Mesosphère `GetInfo(InfoType_ResourceLimit)` donne un handle, lu
    par `svcGetResourceLimitCurrent/LimitValue` (threads, événements,
    transfer memory, sessions, mémoire). Un compteur qui monte avec le temps
    en ligne désignerait une fuite d'objets noyau.
    Lu dans Mesosphère (`kern_k_process.cpp`, `kern_kernel.cpp/.hpp`) : un
    processus d'application sans ressource système propre (`sysres=0`, notre
    cas) prend ses blocs mémoire dans une réserve partagée par les
    applications (`ApplicationMemoryBlockSlabHeapSize` = 20 000) et ses
    tables de pages dans **le même tas que les processus système**
    (`s_page_table_heap`), sans quota. Piste, pas preuve : rien ne montre
    encore qu'on l'épuise.
55. **Regrouper les petits fichiers de `Data/` : reporté.** ~250 ouvertures
    de 4,5 ms par Room Settings, de fichiers ouverts une seule fois (donc
    hors de portée de `keep_open`). Piste : un pack construit par
    `package_sd.sh` (fichiers < 64 Ko, index + données) et des descripteurs
    « fichier en mémoire » servis par `fakefd.c`. Mais plusieurs chemins
    lisent un descripteur directement avec newlib (`mmap` de fichier,
    `pread`, `lseek`, `fstat`, le cache de blocs `nx_io.c`) : il faut les
    brancher tous, et l'essai exige de nouvelles données SD. Pas fait dans
    la session du gel en ligne (v0.1.23).
56. **`[debug] bsd_flood` : le nombre de requêtes n'est pas la cause (v0.1.24,
    session locale).** Un thread (`nx_net.c`, `flood_main`) envoie N requêtes
    bsd par seconde à partir de l'image 300, sur une socket UDP à lui :
    `bsd_flood_op = poll` (poll 0 ms), `sockopt` (`getsockopt`) ou
    `pollwait` (polls de 5 ms à la suite, comme le fil de réception Photon ;
    débit ignoré). Sur console, 2000/s de polls 0 ms sur le menu principal
    (sans Photon) : 400 000 requêtes en 4 min, aucun gel, alors que le gel
    en ligne venait à ~162 000. Puis gel dans « Jouer » à 85 000 requêtes.
    `pollwait` n'a pas encore été essayé sur console.
57. **`[net] local_wait` : ne corrige pas le gel (v0.1.25, session locale).**
    Avec 1, `bsd_poll()` remplace chaque poll bsd avec délai (attentes de
    `poll_fake` et `wait_fd`) par des polls 0 ms et des pauses de 1 ms chez
    nous (`nx_sig_sleep_ms`) : plus aucune attente dans le service. But :
    tester si le temps d'attente cumulé dans bsd (≈ temps connecté, le fil
    Photon attend 5 ms en boucle) était la cause. Gel quand même après 409 s
    connecté, le fil Photon bloqué 13,2 s sur un poll de 0 ms. Gardé, 0 par
    défaut (~1000 requêtes/s en attente, sans effet utile).
58. **Recherche du gel ~400 s en ligne (30/10, session cloud, 4 sous-agents).**
    Sources lues : code libnx (`socket.c`, `bsd.c`, `nifm.c/.h`), code
    Atmosphère/Mesosphère (clone au commit `6b0c077b`), sources Photon du jeu
    (clone `ipodtouch0218/nsmb-mariovsluigi` : Quantum 3.0.13, Realtime 5.1.15),
    switchbrew (via recherche ; l'hôte est bloqué par le proxy), issues GitHub,
    GBAtemp. **switchbrew.org, forum/doc.photonengine.com, gamebrew.org sont
    bloqués en lecture directe dans la session** (à autoriser si on veut les
    pages mot à mot).
    (a) **Photon n'a aucun événement à ~400 s.** Tous ses timers : service
    10 ms, fil de repli 50 ms, ping RTT 1 s, AppStats ~5 s, LobbyStats 60 s,
    keepalive en arrière-plan 60 s (`ClientServiceInactivity`), DisconnectTimeout
    10 s, jeton d'auth **1 h**. `ChecksumInterval = 0` (désactivé) et checksums
    Quantum off. `NetworkHandler.cs` reconstruit l'`AppSettings` à la connexion :
    **UDP pur, `AuthMode=Auth` (par serveur, pas WSS), LobbyStats ON** — il
    écrase `PhotonServerSettings.asset`. Donc un événement LobbyStats entrant
    ~toutes les 60 s et un ping sortant ~1 s (~400 `sendto` sur la socket
    connectée en 400 s), mais **rien à 6 min 40**. Un rapport forum décrit un
    gel d'image sur `socket.Close()` en IL2CPP (explique « tout repart à la
    fermeture de la socket », pas l'origine à 400 s). Conclusion : le
    déclencheur n'est pas un événement applicatif Photon.
    Sources : doc.photonengine.com/quantum/v3/manual/game-session/reconnecting,
    /realtime/.../appandlobbystats, /server/v5/operations/debugging ;
    `Assets/Photon/PhotonRealtime/Code/{ConnectionHandler,AsyncExtensions,
    Enums,RealtimeClientVersion}.cs`, `Assets/Scripts/Networking/NetworkHandler.cs`.
    (b) **Pourquoi bsd tombe en premier, puis nifm.** Le processus `bsdsocket`
    (TID 0100000000000012) héberge bsd:u/s/a, bsdcfg, nsd **et sfdnsres (le
    résolveur DNS)**. **nifm (TID …0F) est un processus séparé, mais son test de
    connexion et ses opérations internet passent par bsdsocket** (libcurl +
    socket/ssl + DNS). Donc quand bsdsocket cesse de répondre, toute opération
    nifm qui l'appelle se bloque à son tour. **fs (processus séparé, built-in,
    sans réseau) qui gèle aussi n'est PAS expliqué par le réseau** → cause
    transversale (cœur CPU, ressource noyau), ou artefact de notre propre
    session fsdev partagée saturée par nos écritures de `debug.log` (à
    confirmer : la sonde fs de l'autopsie ouvre-t-elle sa propre session ?).
    Sources : switchbrew « Sockets services », « Network Interface services »,
    « Boot2 » ; libnx `nifm.h`.
    (c) **`socketInitializeDefault` n'appelle PAS nifm** (vérifié dans
    `socket.c` : seulement `bsdInitialize`). Quasi aucun homebrew ne crée/soumet
    de requête nifm (`nifmCreateRequest`/`nifmRequestSubmit`) : c'est la norme,
    y compris pour les apps UDP temps-réel les plus exigeantes. Donc « ne pas
    faire de requête nifm » n'est probablement **pas** la cause → piste du
    correctif nifm **déclassée**, non codée. L'analogue le plus proche,
    **chiaki-ng** (PS Remote Play, UDP longue durée), utilise udp_tx/rx
    0x40000, `sb_efficiency 8`, **`num_bsd_sessions 16`**, sans nifm ; nous :
    udp_rx 0xA500, 3 sessions. **sys-ftpd #30** (l'analogue documenté le plus
    proche d'un « réseau → hang système ») : épuisement de buffers/sockets
    (`ENOBUFS`, sockets en TIME_WAIT) → « promptly hangs the system ».
    **RetroArch Switch** gèle « après 10-15 min » en **mode applet** (lancé
    depuis l'album) et plus en **mode application** (title-takeover) → test
    gratuit ajouté (TEST 4 octies, essai 2). Sources :
    github.com/chiaki-ng/chiaki-ng switch/src/main.cpp, cathery/sys-ftpd#30,
    gbatemp.net/threads/retroarch-switch.492920.
    (d) **Limites.** Depuis 18.0.0, bsd:u = **15 sessions pour toute la
    console** (0x40→0x24 en 16.0.0→0x0F en 18.0.0) ; bsd:u a 22 threads IPC.
    Les timers FreeBSD `slowtimo`/`fasttimo` sont **TCP** : une socket UDP n'y
    est pas soumise (fuite de callouts TCP peu probable pour de l'UDP pur).
    (e) **Ordonnanceur Mesosphère (confirmé dans le code).** Strictement
    prioritaire **par cœur**, **aucun partage de temps entre priorités
    différentes** : un thread de priorité plus haute qui ne dort jamais affame
    *indéfiniment* un thread plus bas **sur le même cœur** (round-robin
    seulement entre priorités égales, aux niveaux 59 cœurs 0-2 / 63 cœur 3, via
    le « DPC preemption thread » du cœur 3 toutes les 10 ms). `svcSendSyncRequest`
    **n'a pas de timeout** : un client attend pour toujours si le serveur ne
    répond pas. **FS : `default_cpu_id = 3`, priorité 45** (`stratosphere/fs/fs.json`).
    **nx-hbloader : priorité 44, cœurs 0-2, priorités 28-59, mode applet
    (`application_type=2`)** ; il **mappe le NRO dans son propre processus** →
    on hérite du masque de cœurs, de la resource limit et du system resource de
    l'hôte (hbloader, ou l'applet Album en mode applet). Un processus `sysres=0`
    prend ses KMemoryBlock dans un pool **application** (20000) distinct du pool
    **système** (10000) ; l'épuisement d'un slab rend `ResultOutOfResource`
    **sur la SVC** (pas un gel silencieux), et une seule socket + poll() **ne
    fragmente pas** la mémoire. Sources : Atmosphere `kern_k_scheduler.cpp`,
    `kern_k_dpc_manager.cpp`, `kern_kernel.cpp/.hpp`, `kern_k_process.cpp`,
    `kern_k_client_session.cpp`, `kern_k_server_session.cpp`, `stratosphere/fs/fs.json`,
    `switchbrew/nx-hbloader/hbl.json`.
    (f) **Conclusion honnête.** Une famine CPU pure serait **immédiate**, pas à
    400 s ; aucune fuite d'objet noyau, de session ni de thread n'est trouvée
    pour une seule socket. **Aucun mécanisme simple n'explique à lui seul le
    délai de ~400 s.** Il faut mesurer au gel si bsd/fs/nifm sont *runnable mais
    non ordonnancés* (famine) ou *bloqués en IPC* (DECISIONS 59), et trancher
    pile-réseau / Photon / mode applet par expérience (TEST 4 octies). FS étant
    sur le cœur 3 et nous sur 0-2, une famine CPU directe de FS par nous est
    peu probable : l'hypothèse « un service système s'emballe » reste à prouver.

59. **Spec du diagnostic de charge système au gel + correctifs candidats
    (pour la session LOCALE ; non codé en cloud, filtre de sécurité).** Comme
    en DECISIONS 50, le code qui met en pause/lit le contexte d'un AUTRE
    processus a de bonnes chances d'être coupé par le filtre de la session
    cloud ; écrit ici en spec, à implémenter en local.
    **But :** au gel, dire si bsdsocket/nifm/fs sont *runnable mais affamés*
    (famine CPU) ou *bloqués en attente* (IPC/inversion). C'est LE départage
    que la recherche laisse ouvert (DECISIONS 58f).
    **Méthode proposée :**
    - obtenir le pid des services via **pmdmnt** (`pmdmntGetProcessId` sur les
      TID 0100000000000012 bsdsocket, 010000000000000F nifm, 0100000000000000
      fs ; nvdrv 0100000000000019 et am 0100000000000023 en témoins) ;
    - `svcDebugActiveProcess(pid)` puis, en boucle ~5× pendant le gel,
      `svcGetDebugThreadParam`/`svcGetThreadContext3` pour **lister, par thread,
      le PC, la priorité, le cœur courant (`svcGetThreadCoreMask` / le champ du
      contexte) et l'état** (runnable vs waiting). Un PC qui bouge = le service
      tourne (famine ailleurs) ; un PC figé dans un `svc` d'attente = bloqué.
    - sortie dans `autopsy.log` + UDP, format identique à l'autopsie existante,
      **sans toucher** la partie pause/contexte de `diag.c` ni le journal
      `--wrap` de `nx_autopsy.c`.
    - **Le NPDM de hbloader autorise-t-il `svcDebugActiveProcess` ?** À vérifier
      (capability `DebugFlags`/`debug=1`). Sinon, repli : `svcGetInfo`
      (`InfoType` temps CPU par cœur si exposé) pour voir si le temps CPU
      système monte (famine) ou stagne (blocage).
    **Correctifs candidats (specs, non codés ; chacun derrière un interrupteur,
    défaut sûr) :**
    - `[net] poll_yield_ms` : un `svcSleepThread` forcé dans la boucle de poll
      du fil de réception Photon (teste la famine CPU : si le gel disparaît,
      c'était notre boucle qui affamait un cœur) ;
    - abaisser la priorité des threads que NOUS créons est déjà fait (59, la
      plus basse permise) ; la priorité du fil Photon est fixée par IL2CPP (on
      pourrait la baisser via `svcSetThreadPriority` sur ce thread connu) ;
    - `[net] socket_renew_s` (dernier recours, risqué) : substituer une
      nouvelle socket bsd au même fd avant ~6 min. **Le port source change** →
      le serveur Photon voit un nouveau flux : à ne tenter qu'avec la capture
      `photon_ring` active pour voir comment le serveur réagit. Probablement
      inutile si l'essai `udp_soak` montre que Photon n'est pas en cause.

60. **Instrumentation livrée (v0.1.26, session cloud) : anneau Photon +
    `udp_soak` + écho PC.** Tous les deux sûrs (pas de pause/lecture de
    contexte), défauts à off, build propre, auto-test Eden OK (boot, 57 clés,
    BSD up, selftest).
    - **`[debug] photon_ring`** (`nx_net.c`) : anneau des 256 derniers
      datagrammes UDP, capturés au niveau applicatif (vrai payload Photon, avant
      `ipc_cap`), des deux sens, avec heure à la ms, taille et 16 premiers
      octets. `nx_net_photon_report()` est appelé dans l'autopsie (fenêtre 30 s)
      et toutes les 1800 images dans `debug.log`. Statique, atomics relâchés,
      pas de verrou/alloc. But : voir exactement ce qui passe sur le fil dans
      les secondes avant le gel (rafale ? type de commande ? silence ?).
    - **`[debug] udp_soak = ip:port` / `udp_soak_connect`** (`nx_net.c`) : deux
      threads imitent le trafic Photon SANS Photon — socket raw libnx connectée
      (ou `sendto` si `=0`), ~20 datagrammes/s + un fil de réception qui poll
      5 ms — vers un écho UDP sur le PC, depuis l'image 300 (menu principal).
      **L'expérience décisive** : un gel sur le menu principal après ~400 s de
      ce trafic = c'est la pile réseau de la console, pas Photon.
    - **`tools/switch/udp_echo.php`** : écho UDP côté PC en PHP (le pare-feu
      Windows bloque python.exe en entrée mais laisse php-cgi), modelé sur
      `netlog_listen.php` ; renvoie chaque datagramme et journalise le débit et
      les silences (l'heure PC de la dernière ligne date le gel).

61. **Recherche des correctifs (30/10 nuit 2, session cloud) : sys-ftpd #30 et
    chiaki-ng.** Sources lues en direct (raw.githubusercontent.com ; GitHub MCP
    et switchbrew bloqués par le proxy).
    (a) **sys-ftpd #30 : ENOBUFS lié à l'ACTIVITÉ, pas au temps.** Le gel
    (« socket: 105 No buffer space available » → « hangs the system », RAM
    système 8→3 Mo) survient pendant des **transferts TCP actifs** avec des
    tampons trop petits, pas sur une socket oisive. Correctif du fork jakibaki :
    `SOCK_BUFFERSIZE 16384`, tampons TCP 8×/16× la base, **`sb_efficiency = 8`**
    (d'où ~2× la RAM : la tension du fil est « agrandir les tampons » vs « ne
    pas affamer la RAM système des autres sysmodules »). La **transfer memory
    est par client, fixe, allouée une fois** : `tmem = sb_efficiency ×
    pageround(tcp_tx_max+tcp_rx_max+udp_tx+udp_rx)` (libnx `bsd.c`), et côté
    service `sb_mbmax = taille × sb_efficiency` par socket (FreeBSD
    `uipc_sockbuf.c`). ENOBUFS = échec d'allocation d'un mbuf à l'émission
    (`M_PREPEND … M_NOWAIT` → `ENOBUFS`, `udp_usrreq.c`). **libnx dit que le
    service bsd emploie les errno LINUX** (`socket.c _convert_errno` :
    « Nintendo actually used the Linux errno definitions »), donc 105 = ENOBUFS.
    Sources : github.com/cathery/sys-ftpd/issues/30, /pull/29,
    github.com/jakibaki/sys-ftpd/blob/master/source/ftp.c,
    switchbrew/libnx `services/bsd.c`, `runtime/devices/socket.c`,
    freebsd-src `sys/netinet/udp_usrreq.c`, `sys/kern/uipc_sockbuf.c`.
    (b) **Verdict sur l'hypothèse « une socket UDP connectée oisive pourrit en
    400 s » : non étayé par les sources.** Réception bornée à `SO_RCVBUF` puis
    **drop** (`sbappendaddr_locked==0 → m_freem`, compteur `udps_fullsock`) ;
    erreur ICMP = **un seul `so_error`** (pas de file de mbufs comme le
    `MSG_ERRQUEUE` de Linux) ; `poll()` ne retient aucun mbuf ; tmem fixe. Un
    ENOBUFS-puis-gel à ~400 s viendrait plutôt d'une **fuite de sockets/fds ou
    de TIME_WAIT** (compte qui grimpe → cumul de réservations `sb_mbmax`) ou
    d'une **famine de RAM système** (mécanisme du gel global de #30). D'où
    l'instrumentation ajoutée (DECISIONS 62) : errno par commande **et compte
    de sockets ouvertes-non-fermées** dans le battement (teste la fuite).
    (c) **chiaki-ng (analogue UDP temps-réel longue durée)** : config socket
    exacte = udp_tx/rx **0x40000**, tcp 0x8000/0x10000 (max 0x40000),
    **`sb_efficiency 8`**, **`num_bsd_sessions 16`**, bsd:u, **aucun nifm**
    (`switch/src/main.cpp`). `num_bsd_sessions` max libnx = **16**
    (`NX_SESSION_MGR_MAX_SESSIONS`, `sessionmgrCreate` rejette au-delà) ; la
    console n'a que **15 sessions bsd:u** (fw 18.0.0+) → 16 peut échouer à
    l'init (chiaki ne teste pas le retour ; nous oui, avec repli 16→15→8→3).
    magicTupper (remote-play) : sockets UDP **indépendants**, non bloquants,
    `num_bsd_sessions 6`, « drop plutôt que bloquer/affamer ». Moonlight :
    demande gros `SO_RCVBUF` mais **tolère le refus** (step-down). Sources :
    github.com/chiaki-ng/chiaki-ng, nemerpus/magicTupper,
    moonlight-stream/moonlight-common-c `src/PlatformSockets.c`.

62. **Correctifs candidats + instrumentation errno (v0.1.27, session cloud).**
    Tous derrière un interrupteur `[net]`/`[debug]`, défaut sûr ; build propre,
    auto-test Eden OK (chiaki config appliquée, 16 sessions acceptées sous Eden,
    `udp_unconnected` actif, aucun plantage). Défaut off = `redirect` 0 partout
    = zéro changement de comportement.
    - **`[net] chiaki_cfg`** : config socket de chiaki-ng (udp 256 Ko,
      `sb_efficiency 8`, 16 sessions forcées, repli progressif si la console
      refuse — le log `[net] BSD sockets up … N bsd sessions` donne N réel).
      Teste le manque de tampons/sessions.
    - **`[net] udp_unconnected`** : sur une socket UDP, on **ne fait pas**
      `connect()` au niveau bsd ; on garde le pair (`connect_fake`) et on fait
      `sendto(peer)` / `recvfrom` filtré par le pair (le jeu voit une socket
      connectée normale : `getpeername` rend le pair gardé). Teste si une
      socket UDP *connectée* de longue durée est ce qui pourrit dans bsdsocket.
      **Le correctif le plus probable** si la piste « socket connectée » tient.
    - **`[net] photon_rebind_s = N`** : à N s de connexion, un fil substitue
      une socket bsd fraîche sous le même fd applicatif via une indirection
      interne (`SockInfo.redirect`, traduite dans send/recv/poll/sockopt/…),
      nouveau port source, et `shutdown` de l'ancienne. **Risque** : Photon
      suit le pair par IP:port source → il verra sûrement un nouveau flux et se
      déconnectera (~10 s). Pas de `dup2` (le devoptab socket de libnx n'a pas
      de `dup`) : d'où l'indirection. Teste seulement si une socket neuve
      remet à zéro l'horloge des 400 s.
    - **errno bsd par commande + sockets ouvertes** (`nx_bsdmon.c`) :
      histogramme des errno du service (ENOBUFS, ECONNRESET… ; numérotés Linux
      d'après libnx, avec garde-fou FreeBSD pour ENOBUFS) dans le battement
      (`err: ENOBUFS=N …`, seulement si non nul) et l'autopsie (table
      complète) ; plus `N sock open` (sockets - close + accept) dans le
      battement — un compte qui grimpe = fuite de fd/socket (piste #30).
63. **Le gel en ligne de ~400 s : `/dev/urandom` à fond de fichier (02/11,
    session locale, v0.1.29 → corrigé en v0.1.30).** `open_fake` émulait
    `/dev/urandom` (et `/dev/random`) par un fichier de 64 Ko d'octets
    aléatoires écrit sur la SD, puis rendait un vrai fd sur ce fichier. IL2CPP
    garde ce fd ; Photon y puise ~160 octets/s pendant la connexion ; vers
    ~400 s le fichier est épuisé, `read()` rend 0 et
    `il2cpp::os::Cryptography::FillBufferWithRandomBytes` reboucle sans fin :
    ~7 000 `fsFileRead`/s. fs (cœur 3, priorité 45) sature et affame les
    autres services du cœur 3 : bsd (le réseau de **toute** la console —
    sys-botbase, autre processus, n'accepte plus de connexion, le ping
    échoue), nifm ; `UnityMain` attend une écriture fs 12,8 s → image figée.
    Tout repart quand notre processus meurt. Comment on l'a trouvé : un
    compteur de requêtes IPC par session (`nx_autopsy.c`, battement
    `ipc/10s:`) montrait des débits plats jusqu'au gel, mais 129 520 requêtes
    fs à l'autopsie contre ~23 000 attendues ; le thread qui les envoyait
    (« IL2CPP Threadpool worker ») remontait `read_fake` → `fsFileRead` ←
    `libil2cpp.so+0x2901ccc` = `FillBufferWithRandomBytes` (symboles de
    l'artifact CI `mvlo-symbols`). **Correctif** : `read_fake` sert les fd
    marqués `g_fd_rand[]` avec `randomGet()` (le fichier reste pour
    `fstat`/`lseek`). Vérifié : « Jouer » 11 min 40 sans gel, requêtes fs de
    46/s à 7,5/s. Ce que ça invalide : les pistes pile réseau / socket
    connectée / tampons (DECISIONS 58, 61, 62) ; les interrupteurs v0.1.27
    restent, off par défaut, sans effet sur ce gel.
64. **Outils de la session du 02/11.** `sys-botbase` v2.5 sur la console
    (`atmosphere/contents/430000000000000B`, port 6000) et
    `tools/switch/sbb.py` : boutons, captures, `ping` — sert aussi de témoin
    (un autre processus qui utilise le réseau). `tools/switch/freeze_watch.sh`
    : surveille un essai « Jouer » pour un suivi ligne à ligne
    (progression, arrêt du battement, autopsie, sys-botbase). `udp_soak`
    gagne `udp_soak_layer` (par notre couche socket), `udp_soak_dns` (requêtes
    DNS, cible Internet 1.1.1.1:53), `udp_soak_hz`, `udp_soak_reuse`.
    `udp_echo.php` imprime son débit même sous trafic continu (avant : rien
    tant que `stream_select` ne tombait pas à vide).
65. **Profileur par échantillonnage (`[debug] prof_ms`, v0.1.31-33, session
    locale).** Un thread (`nx_prof.c`) regarde toutes les 4 ms la durée de
    l'image en cours ; au-delà de `prof_ms`, il met en pause UnityMain,
    `Loading.AsyncRead` et `Loading.PreloadManager` le temps de lire PC, LR et
    la chaîne de frame pointers, puis écrit les échantillons dans `prof.log`
    (`tools/switch/prof_report.py` les nomme avec l'`.elf` et les symboles CI).
    L'enregistrement du thread dans la couche signaux est tenu pendant la
    lecture (`nx_sig_sample_lock`, `in_delivery = 3`) pour ne jamais croiser
    une livraison forcée du GC. Deux plantages en v0.1.31-32 venaient d'un
    dépassement d'entier dans la borne `fp + 16 <= hi` (FP = -16 la passait) ;
    corrigé en `fp <= hi - 16`, aussi dans `diag.c`. Off par défaut.
66. **À-coups des premières fois : ce qui coûtait (02/11, profils console).**
    *Room Settings* (2,1 s) : 78 % d'IPC fs sur UnityMain, dont
    `fsFsGetFileTimeStampRaw` que `fsdev_open` envoie après chaque ouverture
    (27 %), l'ouverture (23 %), les lectures (21 %). Lancement du match :
    UnityMain attend les chargeurs, qui font les mêmes accès. Création du
    salon : CPU IL2CPP (tables de méthodes génériques, GC). En match : les
    à-coups de 190-310 ms sont UnityMain dans `SocketImpl::Send` → `bsdSend`.
    **Correctifs** : (a) `--wrap fsFsGetFileTimeStampRaw`, réponse « pas
    d'horodatage » pour `assets/bin/Data/` et `il2cpp/` (`[io]
    open_timestamps = 1` le rétablit) ; (b) préchargement des fichiers de
    `Data/` ≤ `[io] preload_kb` en **fichiers en mémoire** dans `nx_io.c`
    (listes à insertion seule, lecture sans verrou, invalidés à l'écriture).
    Deux pièges : dans le cache de blocs de 64 Ko, le préchargement se
    chassait lui-même ; et le jeu ouvre ses données en **chemin relatif**
    (`assets/bin/Data/x`, cwd = racine du jeu) : `path_ino` canonise
    maintenant (retire `sdmc:`, préfixe la racine). Résultat : *Room
    Settings* 1,1 s, lancement ~1,15 s.
67. **Envois UDP asynchrones (`[net] async_send`, v0.1.37, défaut 1).**
    `sendto_fake` copie un datagramme ≤ 1 536 octets dans une file de 256 et
    rend « envoyé » ; un thread d'envoi le passe au service bsd (jusqu'à 3
    essais si EAGAIN). Un seul thread garde l'ordre ; file pleine ou gros
    datagramme = envoi direct comme avant ; `close` d'une socket UDP attend
    au plus 200 ms que la file se vide. Vérifié : 21 min de match, aucune
    image ≥ 150 ms, aucune déconnexion.
68. **Descripteurs fantômes (`[io] phantom_fds`, v0.1.38, défaut 1).** Après
    le préchargement, *Room Settings* passait encore ~0,55 s dans les
    ouvertures/fermetures fs (~250 fichiers). Une ouverture en lecture d'un
    fichier de `assets/bin/Data/` présent en mémoire (`nxio_ramfile_get`)
    rend un descripteur à nous, 3072-4095, sans requête fs ; `read_fake`,
    `z_lseek`, `pread_impl`, `__pread_chk_fake`, `fstat_fake` (cache de stat
    par fd), `mmap_fake` (map anonyme + copie), `z_dup` (même fd, une
    référence de plus) et `close_fake` les servent. Mesuré : *Room Settings*
    1,1 s → 0,49 s (202 ouvertures = 29 ms), lancement du match ~1,15 s →
    ~0,9 s.
69. **Arrivées/départs en pleine partie (02/11, partie publique à 6).**
    À-coups de 127-245 ms sur UnityMain à chaque « X a rejoint / a quitté ».
    Profil : code managé du jeu, surtout la **construction d'expressions
    régulières** (`RegexParser`, `RegexWriter`… : un `new Regex(...)` à
    chaque arrivée/départ, sans doute le filtre du chat ou des pseudos,
    `ChatFilter.cs` / `StringExtensions.cs` en amont), plus une collecte du
    GC et des `FindObjectsByType` de l'interface. Correctif possible côté C#
    (`unity/`) : garder ces `Regex` en `static readonly` construits une fois
    (au chargement), ce qui demande le build Android de la CI (~1 h) et de
    nouvelles données SD. Pas fait.
70. **`Regex.CacheSize = 128` (unity/Runtime/NxPort/NxPerf.cs, 03/11).** Cause
    exacte des à-coups de DECISIONS 69 : `ChatFilter.FilterString` (chaque
    ligne de chat, dont « X a rejoint / a quitté ») passe 53 motifs dans le
    `Regex.Match` statique, dont le cache garde 15 motifs compilés ; chaque
    appel en recompilait ~40. 128 les garde tous après leur premier usage
    (la ligne « Jojo a rejoint » de l'hôte). Demande le build Android de la
    CI puis la mise à jour des données SD (libil2cpp, metadata).
    Vérifié le 03/11 : 26 arrivées/départs en 27 min, aucun à-coup.
71. **Crash « Jungle » en ligne (03/11) : réserves mémoire trop grosses.**
    Unity réserve ses régions de 64 Mo en demandant 131 068 Ko (bloc +
    alignement). Quand la fenêtre OC (1,47-1,66 Go selon l'ASLR) est pleine,
    `mmap_fake` passe par `memalign(64 Mo, 131 068 Ko)` de newlib, qui engage
    les 128 Mo et cherche ~192 Mo contigus. En ligne, newlib n'avait plus que
    136-168 Mo jamais touchés : `[gpua] memalign FAILED size=131068 KB`, puis
    crash. La réserve OC (640 Mo, pic 188-257) et l'arène GPU (320 Mo) sont
    de la mémoire prise à newlib au démarrage : 448/256 laissent ~630 Mo
    jamais touchés. **Ne pas** n'allouer que 64 Mo de ces 131 068 Ko : la
    v0.1.39 l'a fait et a planté au démarrage, Unity écrivant juste après
    les 64 Mo (le passage 2 « kept » d'`oc_alloc_locked` suppose la même
    chose : à suspecter s'il sert un jour).
72. **La réserve OC s'agrandit (v0.1.40).** Elle est découpée au démarrage
    (`oc_pool_mb`, 448 par défaut) et ne rend jamais rien. Avant, une fois
    pleine, `oc_commit_locked` journalisait « EXHAUSTED » et rendait la main :
    `mprotect` avait déjà dit oui à Unity, sa première écriture plantait.
    Maintenant elle prend un bloc de 32 Mo à newlib (`__real_memalign`, pour
    ne pas tomber dans l'arène GPU) et finit d'abord le reste de l'ancien.
    Vérifié avec 128 Mo : trois agrandissements, aucun plantage. À 448 Mo
    rien ne bouge (193-251 Mo utilisés). L'arène GPU, elle, déborde déjà sans
    danger sur newlib ; son vrai pic est 219 Mo (03/11, portable), le
    compteur « maps live » du `[gpu]` (143-165 Mo) n'est pas l'arène.
73. **Niveau gzip d'IL2CPP (`[debug] zlib_level`, v0.1.40, défaut 1).** En fin
    de partie le jeu compresse son replay sur UnityMain
    (`ActiveReplayManager.SaveReplay` → `Quantum.Compression` → `GZipStream`) :
    97 % de `deflate_slow`, 300-700 ms. Le côté natif de Mono
    (`CreateZStream`, exporté par libil2cpp) appelle
    `deflateInit2(niveau -1)` en dur : `CompressionLevel.Fastest` côté C#
    serait ignoré. Le `.nro` cherche `mov w1,#-1 ; mov w2,#8` dans
    `CreateZStream` et met le niveau voulu, sinon ne touche à rien. Mesuré
    sur un replay de 573 Ko bruts : 3,7× plus rapide pour 12 % d'octets en
    plus, toujours du gzip standard. Sur console la compression est passée de
    97 % à 16 % de l'image du replay. Quitter une partie sauve aussi un replay
    (`OnGameDestroyed`), ce qui permet de mesurer seul en salon privé.
74. **Journal asynchrone (v0.1.40).** Cause des à-coups pendant le jeu
    (profil du 03/11 : 150-375 ms, 74-100 % des échantillons dans
    `debugPrintf` ← `fflush` ← `fsFileWrite`, appelé par `nx_gpu_report` et
    la boucle de rendu) : les lignes « importantes » `[loop]`, `[gpua]`,
    `[gfx]` du rendu, toutes les ~2 s, forçaient une écriture sur la SD
    depuis UnityMain, et la carte prend parfois des centaines de ms.
    `debugPrintf` remplit une file de 256 Ko (deux tampons) et un thread
    « log » (priorité 0x3B) l'écrit au plus tard toutes les 2 s, tout de
    suite après une ligne importante. File pleine = lignes perdues et
    comptées (`[log] N bytes of log lost`) plutôt que d'attendre la carte.
    `debug_log_flush()` reste synchrone (erreur fatale, sortie,
    `OnExitRequest`) ; le gestionnaire de plantage appelle
    `debug_log_emergency()`, qui n'écrit que si aucun verrou n'est pris ;
    `[debug] log_sync = 1` garde l'ancien comportement. Mesuré : 6,6 min de
    jeu sans aucune image ≥ 100 ms.
75. **Préchauffage IL2CPP pour la création du salon : sans effet (03/11).**
    v1 (e4c6472) : attributs, `Assembly.GetType` ; v2 (44ab3d5) : plus
    `Statics.RegisterSimulationTypes` sur un `TypeRegistry` jetable, 15 s
    après la première scène (~100 ms, en arrière-plan). La première création
    reste à 0,74 + 0,90 s (la deuxième 0,45 s) : le coût est dans la
    construction de `QuantumGame`/`Frame` elle-même (inflation de méthodes
    génériques ~25 %, table `char*` → `Il2CppClass` ~13 %, malloc/free
    newlib ~25-30 %). Inoffensif, laissé (`[game] warmup = 0` le coupe).
76. **Transferts vers la console (03-04/11).** Le serveur FTP de DBI ne finit
    plus les `STOR` (tous les octets reçus, fin de flux jamais vue, un seul
    thread : il reste bloqué, même sur un fichier neuf après `DELE`) ; les
    lectures (`RETR`, `SIZE`) marchent. Donc : données Unity par MTP
    (`mtp_push_verify.ps1 -NoVerify`, qui répond aux dialogues cachés de
    l'Explorateur) puis vérification par FTP (`ftp_push_verify.py --check`) ;
    `.nro` sans câble par le NetLoader de hbmenu (Y, puis
    `nxlink -a <ip> -p mvlo/mvlo_nx.nro mvlo_nx.nro` : hbmenu l'écrit sous
    `sdmc:/switch/` + ce chemin et le lance).
77. **Sortie sans écran d'erreur (v0.1.40, 05/11).** « Quitter » finissait
    sur « Vous avez quitté le logiciel car une erreur est survenue ». Les
    rapports Atmosphère sont rangés sous l'identifiant du titre qui a lancé
    le `.nro` (icône du jeu : `0587ac03cde1c000`, hbmenu/nxlink :
    `03db12780bd84000`). Trois causes, l'une derrière l'autre :
    1. le thread « log » (DECISIONS 74) écrivait dans `debug.log` après le
       démontage de la carte → `userAppExit()` (util.c, appelé par
       `__appExit` avant `fsdevUnmountAll`) l'arrête, vide la file et ferme
       le fichier ;
    2. le battement `[debug] netlog` envoyait après `socketExit` →
       `nx_netbeat_stop()` avant `nx_net_exit()`, qui vide aussi la file des
       envois UDP asynchrones ;
    3. `__libnx_exit` rend la main à hbloader, qui démappe le `.nro` alors
       qu'environ 25 threads du moteur tournent encore : le premier qui se
       réveille saute dans du code libéré (rapport avec le seul module
       « hbl »). `nx_exit_process()` (main.c, aussi pour `_exit`) met
       `__nx_applet_exit_mode = 1` avant `__libnx_exit(0)` : `appletExit`
       remplace alors la fonction de sortie par `_appletExitProcess` de
       libnx (`ISelfController::Exit`, puis attente que le système termine
       le processus), le `.nro` reste en place. Un `svcExitProcess` seul
       (avec ou sans ce mode) affichait encore l'écran d'erreur, sans
       rapport de plantage.
    Pendant cette sortie, le gestionnaire de plantage termine
    (`svcExitThread`) tout autre thread qui fauterait sur un service déjà
    fermé.
78. **Fin de partie : écriture différée (v0.1.41, `[io] write_behind`,
    défaut 1).** Profil complet de l'enregistrement du replay
    (`ActiveReplayManager.SaveReplay`, sur UnityMain) : écritures + fermeture
    + commit de la carte, `File.Delete` de l'ancien replay temporaire (~20 ms),
    25 `lstat` des replays temporaires (~35-55 ms : taille et dates, pour
    choisir lequel supprimer), création du fichier (~8 ms), compression, et du
    code IL2CPP exécuté pour la première fois.
    - Un fichier ouvert avec `O_CREAT|O_TRUNC` remplit un tampon en mémoire ;
      `close()` le confie au thread « write-behind », qui l'écrit d'un coup,
      ferme le descripteur et fait le commit. Toute autre opération sur le
      descripteur (`read`, `pread`/`pwrite`, `dup`, `mmap`, `ftruncate`,
      `fsync`, `fdopen`, seek ailleurs) vide d'abord le tampon. Piège : le
      `FileStream` de Mono fait `lseek(fd, position, SEEK_SET)` avant chaque
      écriture ; un seek vers la position courante (ou `SEEK_CUR 0`) est donc
      répondu sans écrire, sinon tout repassait par le chemin direct.
      `fstat` sur un tel descripteur est synthétisé (fichier neuf, taille =
      tampon).
    - `unlink` d'un fichier sous `/replays/` passe par le même thread.
    - Un `open`, `stat`, `rename` ou `unlink` d'un chemin encore en cours
      d'écriture ou de suppression attend la fin ; la sortie
      (`nx_exit_process`) et `OnExitRequest` vident la file.
    - Les `stat` des chemins sous `/replays/` sont gardés en mémoire (un
      replay ne change plus une fois écrit), oubliés à chaque écriture,
      fermeture, renommage ou suppression du chemin ou d'un fichier de son
      dossier.
    Mesuré sur console (replays de ~25 Ko, salon privé) : 1er enregistrement
    d'une session 261 → 177 ms, suivants 164 → 92 ms. Sur un vrai replay en
    ligne (100-250 Ko), l'écriture seule coûtait jusqu'à ~145 ms.
    - v0.1.42 : en ligne, les fins de partie suivantes passaient encore
      28-36 ms dans 25 `stat`. Deux défauts du cache : une seule case par clé
      (deux replays qui la partageaient se chassaient à chaque fois) et le
      dossier oublié à chaque écriture, alors que le jeu le teste plusieurs
      fois (`Directory.CreateDirectory`, vérification du dossier par
      `FileStream`) et ne lit jamais ses dates. Cache en 64 ensembles de 4
      voies ; un dossier n'est oublié que s'il est lui-même renommé ou
      supprimé ; `/switch/mvlo/replays` (sans `/` final) est couvert. Une
      ligne `[io] replays stats since the last save: N from the cache, M from
      the card in T ms` par enregistrement. Restent 3 `stat` sur la carte :
      le replay précédent (oublié après son écriture) et deux fois le nom du
      nouveau, encore absent. En salon privé : gel 80-84 ms à partir de la
      2e fin de partie.

79. **Le watchdog a planté le jeu au lancement (v0.1.43, 08/11).** Rapport
    Atmosphère : « User Break » dans `__libnx_exception_handler` ; le vrai
    défaut est dans `stall.log` (`[crash] esr=92000006 far=0x77296498`,
    lecture dans `dump_thread_context`, thread du watchdog). La frame 1 du
    chargement dure 3,6-4,4 s : avec un seuil de 3 s, le watchdog décrivait
    tous les threads à presque chaque lancement (82 sur 84), ~270 lignes
    écrites une par une sur la carte pendant ~8 s. Il mettait chaque thread
    en pause, lisait son contexte, le relançait, puis parcourait sa pile.
    Entre-temps, un thread court du jeu (« T18532 ») s'était terminé et
    libnx avait démappé sa pile (`threadClose`) ; `svcQueryMemory` réussit
    aussi sur une adresse non mappée (type `Unmapped`), donc la borne ne
    protégeait rien. Trois corrections :
    - seuil de 20 s tant que la frame 0 ou 1 n'est pas finie (un vrai blocage
      au chargement est encore décrit ; l'arrêt automatique reste à 30 s) ;
    - la pile (chaîne FP et balayage des adresses de retour) est copiée
      pendant la pause, avec seulement des lectures et
      `so_find_module_by_addr` (sans verrou), puis affichée après la reprise ;
      un bloc non mappé ou illisible est ignoré. Même chose pour l'autopsie
      (`diag_bfmt_context`). `nx_prof.c` le faisait déjà ;
    - `__libnx_exception_handler` : une faute dans le thread du watchdog
      (`diag_thread_is_watchdog`) écrit le dump puis `svcExitThread()` au
      lieu de `svcBreak` : le diagnostic s'arrête, le jeu continue. L'arrêt
      automatique et la vibration ont leurs propres threads (`nx_alive.c`).
    Vérifié sur console avec une version de test (seuil de 3 s et faute
    volontaire à l'adresse 0x8 après le rapport) : rapport complet au même
    format, `[crash] fault inside the watchdog: the watchdog stops, the game
    carries on`, puis écran titre, menu et sortie propre.

80. **Threads jamais fermés : « Thread creation failed » (v0.1.44, 09/11).**
    Session en ligne de 74 min en v0.1.43 : à une reconnexion Photon,
    `[Network] Failed to connect with thrown exception: Thread creation
    failed` (le socket UDP de Photon a besoin d'un thread), le jeu reste hors
    ligne. Le processus dispose de 96 threads (`rl: thr` du heartbeat) ; 70
    étaient pris au menu principal et le compte montait avec les connexions.
    Cause : libnx 4.12 n'implémente pas `__syscall_thread_detach`
    (« Unsupported » dans `newlib.c`), donc le `pthread_detach` de newlib
    renvoie `ENOSYS` sans rien faire, et `pthread_create_fake` ignorait
    l'attribut `PTHREAD_CREATE_DETACHED`. Un thread détaché qui se termine
    n'est jamais `threadClose` : le thread noyau garde sa place parmi les 96
    et sa pile de 2 Mo (allouée par `pthread_create_fake`) reste mappée. Les
    threads joints perdaient aussi leur pile (libnx ne libère pas une pile
    fournie par l'appelant). Correction dans `imports.c` :
    - une fiche par thread (pile, `pthread_t` posé par le créateur et par le
      thread lui-même, détaché ou non, fini ou non) ;
    - `pthread_detach_fake` et l'attribut marquent le thread détaché ; à la fin
      d'un thread détaché (trampoline ou `pthread_exit_fake`), sa fiche part
      vers un thread « ramasseur » (libnx, 16 Ko) qui fait le vrai
      `pthread_join` (attente de la sortie, `threadClose`), puis libère la
      pile et la fiche (le bloc TLS bionic n'est libéré qu'après la sortie) ;
    - `pthread_join_fake` retire la fiche avant le join (la valeur
      `pthread_t` peut resservir ensuite) puis libère la pile ;
    - la ligne `[mem]` donne `threads: créés, ramassés, joints, échecs` et
      `rl:` ; un échec écrit `[thr] pthread_create failed` avec les compteurs.
    Vérifié sur console : après 4 reconnexions, un salon privé et une partie,
    58 créés, 17 ramassés, 7 joints, 67/96 stables (84 sans la correction).

81. **Chat du salon au clavier Switch (v0.1.45, 09/11).** Le joueur n'a pas
    pu répondre « gg » après une partie. Analyse (`[kbd]` du `debug.log`) et
    scène du jeu (`MainMenu.unity`, `MainMenuChat.cs`,
    `TextSubmitValidator.cs`) :
    - la case du chat est la seule multiligne (`LineType` 2) ; son
      validateur envoie le message sur `'\n'`. TMP n'applique les
      validateurs qu'au texte reçu pendant que le clavier est « visible » ;
      le texte laissé par un clavier qui se ferme est copié tel quel. OK
      ne faisait donc que remplir la case. Désormais, pour un champ
      multiligne, `nx_kbd_pump` donne le texte + `'\n'` clavier encore
      visible (le message part à la frame suivante), puis ferme le clavier,
      vide, deux passages plus tard (état 3) ;
    - `SendChat` refocalise la case (`SelectTextboxNextFrame`) : la touche
      sur « Envoyer », de moins d'une seconde, passait pour une demande et
      rouvrait swkbd. Une touche déjà relâchée quand le champ demande son
      clavier ne compte plus (`nx_input_touch_lifted` ; TMP sélectionne au
      contact, doigt encore posé) ;
    - un clavier « en attente » (demande sans geste frais) s'ouvrait à la
      touche suivante n'importe où : il attend 250 ms et ne s'ouvre que si
      le champ a gardé le focus (une touche ailleurs fait `hideSoftInput`) ;
      expiré au bout d'une minute, il rend le champ (`nativeSoftInputCanceled`)
      au lieu de le laisser « actif » et sourd aux touches.
    Vérifié en salon privé, avant et après une partie : OK envoie (« gg »,
    « re », « gg »), « Envoyer » champ vide n'ouvre rien, toucher la case
    ouvre le clavier ; aucune exception, sortie propre.
    Reste : la case est inaccessible à la manette dans la scène du jeu
    (`TextField` n'est la cible d'aucune navigation, `BtnSendMessage` n'a
    pas de voisin à gauche), sur PC aussi.

82. **Dépôt public, jeu chiffré (11/11).** Les minutes Actions du compte
    étaient épuisées (dépôts privés) ; un dépôt public a des minutes
    gratuites. Mais sur un dépôt public, tout compte GitHub peut télécharger
    les artifacts : celui d'android-build (les `.so` du jeu + ses assets, dont
    des assets Nintendo) aurait redistribué le jeu. Il n'est donc plus publié
    qu'en zip AES-256 (7-Zip) dont le mot de passe est le secret
    `SD_ARCHIVE_PASSWORD` ; sans le secret, rien n'est publié. Le `.nro`, les
    logs et les captures de l'émulateur restent publics (notre code, des
    images d'écran). L'AppId Quantum officiel, présent dans l'ancien
    historique jusqu'à la v0.1.42, est de toute façon en clair dans les
    sources publiques du jeu ; l'historique a quand même été regroupé, et les
    pull requests de forks attendent une approbation avant de lancer la CI
    (elles pourraient sinon lire le cache de la build du jeu).
