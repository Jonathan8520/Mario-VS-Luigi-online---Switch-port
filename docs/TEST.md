# Tester mvlo_nx sur ta Switch

> Prérequis : Switch avec CFW (Atmosphère) et hbmenu. Les manipulations
> ci-dessous ne touchent qu'au dossier `sdmc:/switch/mvlo/`.

## 1. Récupérer les fichiers (GitHub → onglet *Actions*)

Deux artifacts, tous deux sur la branche `main` :

| Workflow | Artifact | Contenu |
|---|---|---|
| **nro** (dernier run vert) | `mvlo_nx-nro` | `switch/mvlo/mvlo_nx.nro` + `mvlo.ini.example` |
| **android-build** (dernier run vert) | `mvlo-sd-data-v2.2.0.1` | `mvlo-sd-data-v2.2.0.1.zip` **chiffré** : `switch/mvlo/` avec les `.so` du jeu, `assets/`, `BUILDINFO.txt` |

Le dépôt est public mais le jeu ne doit pas l'être : ses données sont dans un
zip AES-256 dont le mot de passe est le secret `SD_ARCHIVE_PASSWORD` du dépôt
(sans ce secret, le workflow ne publie rien). L'Explorateur Windows ne sait pas
ouvrir ce chiffrement : utiliser **7-Zip**.

Dézippe les deux **dans le même dossier** : ils se complètent.

## 2. Copier sur la carte SD

```
sdmc:/switch/mvlo/
├── mvlo_nx.nro
├── mvlo.ini                    ← créé automatiquement au 1er lancement (voir §4)
├── libmain.so
├── libunity.so
├── libil2cpp.so
├── lib_burst_generated.so      ← seulement s'il est présent dans l'artifact
├── BUILDINFO.txt  IMPORTS_CHECK.txt  IL2CPP_INFO.txt   (diagnostic, petits)
├── il2cpp/                     ← copie pré-extraite de Managed/ (+ unity.ver)
└── assets/
    └── bin/Data/
        ├── boot.config
        ├── globalgamemanagers, level0…, sharedassets…, resources.assets
        │   (ou un seul data.unity3d selon la compression du build)
        ├── Managed/Metadata/global-metadata.dat
        └── ... (tout le reste du dossier assets/)
```

Le nom du dossier n'est pas imposé (le .nro cherche ses fichiers à côté de
lui), mais `sdmc:/switch/mvlo/` est celui documenté partout.

## 3. Lancer

**Obligatoire : mode *title takeover*** (mémoire complète). Lance un jeu
installé quelconque **en maintenant R**, hbmenu s'ouvre à la place du jeu,
puis choisis *Mario vs Luigi Online*.

- Ne **pas** lancer depuis l'Album (mode applet : ~400 Mo seulement, le jeu
  n'aura pas assez de mémoire ; le log le signale : `APPLET MODE`).
- Ne pas utiliser de forwarder NRO/NSP : sur le port PvZ Fusion du même
  lignage, ils plantent faute de permissions de mapping mémoire.
- Démarrage : compter ~30–40 s d'écran fixe (chargement depuis la SD) avant
  l'intro. Sans le dossier `il2cpp/` (anciens artifacts, avant la v0.1.1),
  Unity le recrée lui-même au premier lancement : ~1 min 45 de plus, une
  seule fois.
- Si vous mettez à jour les données du jeu, recopiez **tout** le dossier
  (y compris `il2cpp/`).

Commandes : manette normale (Joy-Con, Pro Controller, jusqu'à 4).
A = valider, B = retour (disposition « nintendo » ; `ab_layout = xbox` pour
l'inverse). Le tactile fonctionne dans les menus. Saut = A/B, course = X/Y,
pouvoir = R/ZR, réserve = L/ZL, + = pause, − = scores.

## 4. Configurer `mvlo.ini` (et l'AppId Photon)

Au premier lancement, `sdmc:/switch/mvlo/mvlo.ini` est créé avec les valeurs
par défaut (copie de `mvlo.ini.example`). Pour jouer **en ligne**, il faut
**ton propre AppId Photon Quantum** — aucun AppId n'est intégré au port.

### Créer son AppId Photon (gratuit, 5 min)

1. Crée un compte sur <https://dashboard.photonengine.com> (offre gratuite :
   20 joueurs simultanés, largement suffisant).
2. *Create a new app* → **Application Type : Quantum** → **Photon SDK :
   Quantum 3** (le jeu utilise Quantum 3) → donne un nom → *Create*.
3. Dans la liste des applications, copie l'**App ID** (un GUID du type
   `xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx`).
4. Édite `sdmc:/switch/mvlo/mvlo.ini` :
   ```ini
   [photon]
   appid_quantum = xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
   ```
5. Facultatif : `fixed_region = eu` pour forcer l'Europe.

⚠ Limite à connaître : on ne voit que les joueurs qui utilisent **le même
AppId et la même version du jeu**. Les versions PC/Web officielles utilisent
l'AppId officiel : pas de jeu croisé avec elles (il faudrait une version PC
compilée avec ton AppId). Entre deux Switch avec ce port et ton AppId : oui.
**Sans AppId, on ne peut pas jouer** : le jeu n'a pas de mode hors-ligne,
même une partie seul passe par une salle Photon. Le menu *Play Game* reste
alors bloqué sur « Connecting… » (B pour revenir) et le log affiche
`photon.appid_quantum missing`.

Ne partage pas ton `mvlo.ini` publiquement (il contient ton AppId) ; le
`debug.log` le masque (`(set, hidden)`).

## 4 sexdecies. v0.1.45 : chat du salon (09/11)

> Toucher la case « Entrez du texte… » ouvre le clavier Switch ; **OK envoie
> le message** (plus besoin de toucher « Envoyer »). « Envoyer » reste utile
> si du texte attend dans la case, et n'ouvre plus le clavier. À la manette,
> la case n'est toujours pas atteignable (scène du jeu).

## 4 quindecies. v0.1.44 : longues sessions en ligne (09/11)

> Rien à régler. La ligne `[mem]` de `debug.log` finit par `threads: N
> created, M reaped, J joined, F failed | rl: thr X/96 …`. `X` doit rester
> stable pendant une longue session (vers 65-70) ; `failed` doit rester à 0.
> Si une création échoue, une ligne `[thr] pthread_create failed` l'écrit.

## 4 quaterdecies. v0.1.43 : plantage au lancement (08/11)

> Rien à régler. `stall.log` ne contient plus de « STALL #1 » à chaque
> lancement (le chargement des frames 0-1 a droit à 20 s). Si le watchdog
> plante un jour, `stall.log` finit par `[crash] fault inside the watchdog:
> the watchdog stops, the game carries on` et le jeu continue.

## 4 terdecies. v0.1.42 : cache des replays, logs plafonnés (05/11)

> Rien à régler. À chaque fin de partie, `debug.log` dit d'où viennent les
> infos des replays : `[io] replays stats since the last save: 22 from the
> cache, 3 from the card in … ms` (beaucoup plus « from the card » à la
> première de la session, c'est normal). Au lancement, `stall.log` et
> `prof.log` de plus de 1 Mo deviennent `stall.prev.log` / `prof.prev.log`.

## 4 duodecies. v0.1.41 : fin de partie (05/11)

> Rien à régler. Dans `debug.log`, à la première fin de partie : `[io]
> write-behind on`, puis `[io] write-behind: …Replay-….mvlreplay, N KB from
> 3 writes, written+closed+committed in … ms`. À vérifier en vraie partie :
> le petit gel quand une partie se termine (surtout les suivantes), et que
> les replays s'ouvrent dans le menu « Replays ». En cas de doute :
> `[io] write_behind = 0` (comportement d'avant). Voir DECISIONS 78.

## 4 undecies. v0.1.40 : à-coups en partie et mémoire (04/11)

> Rien à régler. Les lignes `oc_pool_mb = 448` / `gpu_arena_mb = 256`
> ajoutées au `mvlo.ini` le 03/11 sont désormais les valeurs par défaut (les
> garder ne change rien). Dans `debug.log` au démarrage : `[log] writer thread
> started` et `[patch] CreateZStream deflate level applied`. Si la réserve OC
> grandit : `[oc] commit-pool grown by 32 MB` (normal, plus de plantage).
> À vérifier en vraie partie publique : plus d'à-coup pendant le jeu, fin de
> partie (sauvegarde du replay) ~0,35 s au lieu de 0,5-0,7 s. Pour comparer :
> `[debug] zlib_level = 0` (niveau d'origine). Voir DECISIONS 72-74.
>
> **Résultat (04→05/11).** 52 min en ligne, Jungle comprise : aucun plantage,
> 30 i/s stables. « Quitter » revient maintenant à l'accueil sans le message
> « … une erreur est survenue » (DECISIONS 77) ; s'il revient, noter l'heure
> et récupérer `atmosphere/crash_reports/` (rapports au nom de l'icône qui a
> lancé le jeu).

## 4 decies. v0.1.37 : à-coups des premières fois (02/11 soir)

> Rien à régler : préchargement (`[io] preload_kb = 1024`), descripteurs
> fantômes (`[io] phantom_fds = 1`, v0.1.38) et envois UDP asynchrones
> (`[net] async_send = 1`) sont actifs par défaut. Jouer avec les joueurs du
> site : `[photon] build_identifier = official`. Sur le menu
> principal, laisser ~10 s au préchargement (`[io] preload: … read in 7 s`
> dans debug.log) avant d'entrer dans « Jouer ». Pour mesurer : `[debug]
> prof_ms = 150` puis `tools/switch/prof_report.py` sur `prof.log` (voir
> DECISIONS 65-67). Créer les parties en **salon privé**.

## 4 nonies. v0.1.30 : le gel en ligne est corrigé (02/11)

> **Résultat.** La cause était `/dev/urandom` émulé par un fichier de 64 Ko
> qui s'épuisait vers ~400 s de connexion (DECISIONS 63). Avec la v0.1.30,
> « Jouer » 11 min 40 sans gel. Il ne reste qu'à confirmer sur **un vrai match
> de plus de 7 minutes** ; aucun réglage `[net]` n'est nécessaire. Les essais
> de la section 4 octies ci-dessous sont caducs.

## 4 octies. Essai de la v0.1.27 : mesurer puis corriger le gel en ligne

> **30/10, session cloud.** Recherche (DECISIONS 58, 61) : **aucun événement
> Photon à ~400 s** (ses timers sont ≤ 60 s ou = 1 h) → le gel vient très
> probablement de la **pile réseau de la console** sur notre socket UDP
> connectée de longue durée. `bsdsocket` héberge bsd **et le DNS** ; nifm
> dépend de lui → « bsd tombe d'abord, puis nifm ». Analogue documenté :
> **sys-ftpd #30**, épuisement de tampons (`ENOBUFS`) → « hangs the system ».
> Depuis 18.0.0, **15 sessions `bsd:u` pour toute la console**.
>
> La v0.1.27 ajoute la **répartition des errno bsd** (battement + autopsie :
> `err: ENOBUFS=N …`) — surveiller si `ENOBUFS` monte avant le gel — et trois
> **correctifs candidats** (ci-dessous). **Lancement toujours en title
> takeover (applet type 0)** : l'essai « mode applet » est sans objet, retiré.

Copier seulement `mvlo_nx-nro` (pas d'android-build). Garder `mvlo.ini` et
`[debug] netlog` avec l'écoute PC. Les lignes ajoutées vont sous **`[debug]`**
ou **`[net]`** comme indiqué ; les remettre comme avant après chaque essai.
**L'écran « Jouer » seul (liste des salons, sans jouer) reproduit le gel en
6 min 40** : inutile de lancer un match. Mesures d'abord (1-2), puis les
correctifs (a-c), chacun ≤ 8 min.

**Essai 1 — le plus décisif : trafic façon Photon SANS Photon (`udp_soak`).**
Dit si c'est la pile réseau de la console ou Photon.
- Sur le PC, lancer l'écho UDP (le pare-feu Windows laisse passer php-cgi) :
  `php tools/switch/udp_echo.php 9998 udp_soak.txt` (ou
  `php-cgi -q -d max_execution_time=0 -f tools/switch/udp_echo.php 9998 udp_soak.txt`).
- Dans `mvlo.ini`, sous `[debug]` : `udp_soak = <IP du PC sur le réseau>:9998`.
- Lancer le jeu et **rester sur le MENU PRINCIPAL** (ne pas aller dans
  « Jouer »), manette posée, ~10 min. Le PC doit afficher ~20 paquets/s.
- **Résultat :** gel sur le menu principal après ~400 s = **c'est la pile
  réseau de la console** (notre socket UDP connectée + poll 5 ms), pas Photon.
  Pas de gel en 10 min = le motif socket seul ne suffit pas (cause propre à
  Photon, ou à l'état nifm d'une vraie connexion).
- **Variante**, seulement si l'essai 1 ne gèle pas : ajouter
  `udp_soak_connect = 0` (socket non connectée, `sendto`) et refaire. Dit si
  c'est le fait que la socket soit *connectée* (`connect`+`send`) qui compte.
- Envoyer `debug.log` et `udp_soak.txt` (l'heure PC de la dernière ligne date
  le gel).

**Essai 2 (mesure) — capturer le fil au gel + guetter l'épuisement
(`photon_ring`).**
- Dans `mvlo.ini`, sous `[debug]` : `photon_ring = 1`.
- Écran « Jouer » seul jusqu'au gel (~6 min 40), manette posée.
- **Résultat :** l'autopsie contient « last Photon datagrams » (les ~30 s avant
  le gel : sens, taille, 16 premiers octets) et « bsd errno breakdown ». Dans
  les battements, guetter `err: ENOBUFS=…` : s'il **monte** avant le gel, c'est
  un épuisement de tampons (piste sys-ftpd). Envoyer `autopsy.log` + l'écoute PC.

### Correctifs candidats (un par partie « Jouer » seul, ≤ 8 min, sous `[net]`)

Chacun est à `0`/absent par défaut. Si un correctif **supprime ou recule
nettement** le gel, on tient une piste ; sinon on l'élimine. Toujours renvoyer
`debug.log` (+ `autopsy.log` si gel) en notant le réglage.

**a) `chiaki_cfg = 1` — config socket de chiaki-ng.** Gros tampons UDP
(256 Ko), `sb_efficiency 8`, jusqu'à 16 sessions bsd (repli automatique si la
console refuse : regarde la ligne `[net] BSD sockets up … N bsd sessions`).
- **Résultat :** plus de gel / gel bien plus tard = manque de tampons/sessions
  (épuisement). Gel identique à ~400 s = ce n'est pas le dimensionnement.

**b) `udp_unconnected = 1` — ne pas connecter la socket UDP au niveau bsd.**
On garde le pair et on fait `sendto`/`recvfrom` filtré ; Photon n'y voit rien.
- **Résultat :** plus de gel = c'est bien une socket UDP **connectée** de longue
  durée qui pourrit dans bsdsocket (la piste la plus probable). Gel identique =
  ce n'est pas le fait d'être connecté. (Si Photon ne se connecte plus du tout,
  me le dire : le filtrage du pair serait trop strict.)

**c) `photon_rebind_s = 350` — socket neuve avant ~400 s.** À 350 s, une socket
UDP fraîche remplace l'ancienne sous le même fd (nouveau port source). **Risque
connu : Photon peut voir un nouveau flux et se déconnecter (~10 s après).**
Lancer avec `photon_ring = 1`.
- **Résultat :** si la console **ne gèle pas** à ~400 s (même si Photon se
  déconnecte) = le gel est lié à la durée de vie d'UNE socket bsd → une
  rotation périodique serait un contournement. Si ça gèle quand même = la durée
  de vie de la socket n'est pas le facteur. Si Photon tient bon après le
  changement de port, c'est un bonus (rotation viable comme vrai correctif).

**Réglage complémentaire — sessions `bsd:u` (A/B).** `bsd_sessions = 1` puis
`bsd_sessions = 8`. Si `1` retarde le gel et `8` l'aggrave = pression sur les
15 sessions partagées.

Ordre conseillé : **1 → 2 → a → b → c**. L'essai 1 tranche pile-réseau / Photon ;
l'essai 2 montre l'épuisement éventuel ; **b** est le correctif le plus probable.

## 4 septies. Essai de la v0.1.23 : mesures et essais A/B du gel en ligne

> **Fait le 29/10 (v0.1.23 → v0.1.25, session locale), voir STATUS « le gel
> cerné ».** Référence et B faits (B sans effet) ; A et D abandonnés (17
> requêtes mappées sur 162 000 ; les polls de 5 ms viennent de Photon, pas
> du découpage). C non fait. À la place : flood sur le menu (pas de gel),
> écran « Jouer » seul (gel à 396 s connecté), déconnexion/reconnexion
> (compte cumulé), `local_wait` (gel à 409 s).

Copier seulement `mvlo_nx-nro` (pas d'android-build). Garder `mvlo.ini` et
`[debug] netlog` avec l'écoute PC.

1. **Partie de référence, réglages par défaut**, jusqu'au gel (~7 min en
   ligne). Envoyer `autopsy.log`, `debug.log` et le fichier de l'écoute PC.
   Ce qu'il me faut :
   - dans les battements, `bsd: N req (M mapped ...)` et, toutes les 2 s,
     `rl: thr .. ev .. tmem .. sess ..` : est-ce qu'un compteur monte
     régulièrement jusqu'au gel ?
   - la ligne `[net] BSD sockets up ... bsd pointer buffer 0x....` et, si
     elle apparaît, `[net] FIONREAD refused by bsd ...` ;
   - dans `autopsy.log`, le tableau « bsd requests per command » et les 128
     dernières opérations.
2. **Essais A/B**, un par partie, chacun jusqu'au gel ou 15 min en ligne
   sans gel. Mettre la ligne sous `[net]` dans `mvlo.ini`, puis la remettre
   comme avant :
   - A : `small_ipc = 1` (aucune requête réseau mappée) ;
   - B : `socket_config = 1` (tampons libnx par défaut) ;
   - C : `bsd_sessions = 6` ;
   - D : `poll_slice_ms = 0` (beaucoup moins de requêtes par seconde) ;
   Pour chaque essai : gel oui/non, après combien de secondes en ligne, et
   le compteur `bsd: N req` au gel. Si D retarde nettement le gel, c'est le
   nombre d'appels ; si rien ne bouge, c'est une durée.

## 4 sexies. Essai de la v0.1.22 : pendant un gel

1. Joue en ligne jusqu'au gel (~7 min de connexion).
2. Ne touche à rien. ~15 s après le gel : **une vibration longue** =
   rapport écrit dans `autopsy.log` ; **trois courtes** = la carte SD ne l'a
   pas pris (le PC de `netlog` le reçoit peut-être). Ensuite deux courtes
   toutes les 5 s tant que le processus tourne.
3. ~90 s après le gel le jeu se ferme tout seul (« Vous avez quitté le
   logiciel car une erreur est survenue ») : plus besoin de POWER. Note si
   HOME et le reste répondent tout de suite.
4. Envoie `autopsy.log`, `debug.log`, `stall.log`.

Pour tester l'outillage sans attendre : `[debug] fake_stall_frame = 3600`
(arrête la boucle de rendu à cette image), puis remets `0`.

## 4 quinquies. Essai de la v0.1.21, dans l'ordre

Copier seulement `mvlo_nx-nro` (pas d'android-build). Garder `mvlo.ini`.

1. **Room Settings et début de partie** : comparer aux temps de la v0.1.20.
   Lignes utiles dans `debug.log` :
   - `[io] basename fallback: N answered from the game folder listing (...),
     M real stats` — M doit rester presque nul ;
   - `[io] kept-open files: ... reopens served without the SD card ...` —
     plus de réutilisations qu'en v0.1.20 pour le même parcours ;
   - `[hitch]` : `stat` doit coûter nettement moins que 0,3 ms par appel,
     `file open` moins aussi.
   En cas de fichier introuvable ou de chargement raté : `[io] keep_open = 0`
   et dire si ça le règle.
2. Rien d'autre ne change : la vibration pendant un gel et tout le reste
   de la v0.1.20 restent valables.

## 4 quater. Essai de la v0.1.20 (nuit du 28 au 29/10), dans l'ordre

Copier seulement l'artefact `mvlo_nx-nro` (le `.nro` ; pas de nouvelles données
de jeu, aucun android-build nécessaire). Garder `prefs.kv` et `mvlo.ini` : les
nouvelles options ont leurs valeurs par défaut. Garder `[debug] netlog` et
l'écoute PC si possible.

1. **Démarrage + Room Settings** : comme en v0.1.19, en un peu plus rapide
   (fichiers gardés ouverts). Dans `debug.log`, ligne `[io] kept-open files:
   ... reopens served without the SD card`. En cas de souci de fichiers
   (texte manquant, chargement raté) : `[io] keep_open = 0`.
2. **Blocage n° 1 (le jeu se fige, la console vit)** : enchaîner des débuts
   de partie, comme quand il se produisait. Il ne doit plus se figer. Dans
   `debug.log`, des lignes `[sig] main stuck NN ms in game code (...):
   delivering its signals ... on its behalf` = la correction a servi. Si le
   jeu se fige quand même : `stall.log` + `debug.log` + les derniers
   battements (ils disent maintenant `gc: N parked for ... ms, M not acked
   (qui ...)`). Pour comparer, une partie avec `[debug] gc_force_ms = 0`
   (ancien comportement).
3. **Blocage n° 2 (gel de toute la console)** : jouer ~15 min avec l'écoute
   PC. Pendant un gel :
   - **la manette vibre-t-elle deux fois toutes les 5 s ?** (après 12 s de
     jeu figé). Oui = notre processus tourne encore, ce n'est pas toute la
     console ; non = processus arrêté aussi ;
   - envoyer les ~20 derniers battements : nouvelles parties `gc:`, `kmem:`
     (appels noyau de mémoire, avec `IN FLIGHT` s'il y en a un en cours),
     `nv:` (ioctl GPU, `IN FLIGHT`), `send:` (envois UDP lents/en échec) ;
   - noter si le battement s'arrête **en même temps** que la vibration.
4. **Auto-test optionnel** (≈ 1 s, au menu) : `[debug] gc_spin_test = 600`
   → ligne `[selftest] GC with a spinning thread: ... PASSED` vers l'image
   600. À remettre à 0 ensuite.
5. **À-coups** : `[hitch]` (seuil 100 ms) sépare `file open` / `stat` /
   `fstat` / `access` / GL. M'envoyer quelques lignes d'un premier affichage
   d'objet en partie.

Lignes à m'envoyer en priorité : `[sig]` (surtout `stuck ... on its behalf`
et `cannot ...`), `[selftest]`, `[io] kept-open`, `[hitch]`, et pour un gel :
`stall.log`, les derniers battements, vibration oui/non.

## 4 ter. Essai de la v0.1.13 (nuit du 27 au 28/10), dans l'ordre

Copier **les deux** artefacts du dernier commit : `mvlo_nx-nro` (le `.nro` et
`splash/`) et `mvlo-sd-data-v2.2.0.1` (données du jeu reconstruites : correctifs
du clavier et de `UI/Navigate`). Garder `prefs.kv` et son `mvlo.ini` (les
nouvelles options ont des valeurs par défaut, rien à ajouter).

1. **Démarrage** : le 1er lancement après la copie reste noir ~4 s (il note
   la config EGL d'Unity dans `mvlo/.eglconfig`). **Relancer** : l'écran de
   chargement doit apparaître dès ~0,5 s, puis continuer sans saut quand le
   jeu prend la main. Lignes : `[load] early loading screen on` puis
   `handed to Unity after ... ms`. Si l'écran reste figé ou noir après :
   `[video] early_splash = 0` et m'envoyer `debug.log`.
2. **Clavier** : pseudo puis chat d'un salon. Après validation (+) du clavier
   Switch, le clavier du jeu doit se **fermer tout seul**.
3. **Deux parties de suite**, la 2e au moins 5 minutes (c'est là que la
   console gelait). Si elle gèle :
   - noter si la **musique continue normalement** ou **boucle un bout de
     son** (ça dit si le processeur tourne encore) ;
   - envoyer `debug.log`, `stall.log` : les dernières lignes `[gpu]`, `[nv]`,
     `[wd] alive` (elles portent maintenant l'état GPU) disent ce qui
     grossissait ou ce que le GPU attendait.
   Puis réessayer une fois avec `[video] shader_code_mb = 0` et une fois avec
   `[debug] nv_tmem_mb = 8` pour savoir laquelle des deux protections compte.
4. **À-coups** : pendant ces parties, chaque arrêt de plus de 100 ms écrit une
   ligne `[hitch]` qui dit où part le temps (compilation GLSL, édition de
   liens, génération de code nouveau, textures, attente du GPU…). C'est ce
   qu'il me faut pour choisir la suite. Pour comparer : une partie avec
   `[video] compile_boost = 0`, et une avec `[video] nv_shader_opt = 1`.
5. **NullReferenceException** au début de la 2e partie : elle ne doit plus
   apparaître ; si elle revient, envoyer les lignes complètes (avec la pile).

Lignes à m'envoyer en priorité : `[hitch]`, `[gpu]`, `[nv]`, `[load] early
loading screen ...`, `[gpu] shader code segment full ...` (avec l'heure), et le
`stall.log` en cas de gel.

## 4 bis. Essai de la v0.1.9 (nuit du 26 au 27/10), dans l'ordre

Copier le nouveau `.nro` (artifact `mvlo_nx-nro` du dernier run *nro*). Les
données SD n'ont pas changé, sauf si le rebuild Unity de la v0.1.8 (X/Y,
`build_identifier`) est terminé : alors reprendre aussi `mvlo-sd-data-v2.2.0.1`
(tout le dossier, `il2cpp/` compris). Garder `prefs.kv` : les zéros des
réglages réels qui y sont restés sont effacés automatiquement au chargement.

1. **Démarrage** : après 2-4 s, le logo Infenek sur fond noir (2 s), puis les
   images du jeu en fondu enchaîné, qui zooment doucement, le logo du jeu en
   haut, et en bas « CHARGEMENT... », le pourcentage et la barre. Les images
   sont dans `switch/mvlo/splash/` (artefact `mvlo_nx-nro` ou
   `mvlo-sd-data`) ; sans elles, l'écran simple s'affiche. La barre affiche un
   pourcentage à partir du 2e lancement. Si ça plante ou gèle à ce moment, ou
   juste quand le jeu démarre : `[video] loading_screen = 0`. Si l'animation
   saccade trop : `[video] loading_fps = 30`.
2. **Son et HUD** : la musique doit jouer dès l'écran titre (volumes par
   défaut 0,75 / 0,5 / 0,75), le HUD en partie doit être grand (HUD Scale 8).
   Changer un volume dans les options, quitter, relancer : il doit être gardé.
   Plus gros encore : `[game] hud_scale = 10` (ou 12).
3. **Clavier** : *Play Game* → champ *Nickname* → A : le clavier de la Switch
   doit s'ouvrir ; valider (+) → le pseudo change. Le clavier du jeu peut
   rester affiché derrière : B le ferme, A rouvre le clavier Switch. Idem dans le chat d'une salle, et en
   touchant le champ du doigt. S'il se rouvre en boucle : `[input] keyboard = 0`
   (et m'envoyer les lignes `[kbd]`) ; s'il ne s'ouvre jamais, ou s'il
   s'ouvre sur un A appuyé ailleurs qu'un champ de texte :
   `[input] keyboard = 2`.
4. **Lancement d'une partie** : noter la durée des arrêts (avant : 6 à 9 s).
   Si quelque chose casse ou si des données semblent corrompues :
   `[io] cache = 0` (ancien chemin).
5. Jouer quelques minutes en ligne comme d'habitude.

Logs à m'envoyer : `debug.log` (+ `debug.prev.log`), `stall.log` s'il a grossi,
et une phrase par point ci-dessus (OK / KO). Nouvelles lignes utiles dans
`debug.log` :
- `[load] ...` (écran de chargement), `[kbd] ...` (clavier),
- `[io] cache: ...` et `[io]   <fichier> ...` (temps SD par fichier, toutes
  les ~15-30 s et après chaque image de plus d'1 s), `[io] slow SD read`,
- `[perf] per frame: nativeRender X ms ... present Y ms | limiter sleep Z ms`
  (dit ce qui limite à 30 i/s),
- et, si l'`ArgumentOutOfRangeException` de `UI/Navigate` revient, **les
  lignes complètes** qui la contiennent (avec la pile si elle suit).

Si un essai tourne mal, désactiver les nouveautés une à une
(`[video] loading_screen`, `[input] keyboard`, `[io] cache`, puis
`[debug] jni_forname = 0`, `[debug] jni_avariants = 0`).

## 5. Quels logs m'envoyer

Après un essai (qu'il marche ou plante), copie depuis la SD :

1. `sdmc:/switch/mvlo/debug.log` — **le plus important** (horodaté)
2. `sdmc:/switch/mvlo/debug.prev.log` — le lancement précédent
3. `sdmc:/switch/mvlo/stall.log` — s'il existe : blocages (watchdog) et
   **crash dumps** (registres, backtrace `libxxx.so+0x…`)
4. `sdmc:/atmosphere/crash_reports/*.log` récents et
   `sdmc:/atmosphere/fatal_errors/*.bin` récents, s'il y en a
5. `sdmc:/switch/mvlo/BUILDINFO.txt` (identifie les données utilisées)
6. une phrase : jusqu'où ça va (écran noir ? logo ? menu ? partie ?), en
   dock ou en portable, et quel firmware/Atmosphère.

Si la console **gèle** (rien ne répond), mets `log_sync = 1` dans la section
`[debug]` de `mvlo.ini` et refais l'essai : chaque ligne est alors écrite
immédiatement, donc la dernière ligne du log dit où ça s'est arrêté.

## 6. Interrupteurs de diagnostic (`[debug]` dans mvlo.ini)

Ne les change que si je te le demande (tous à `1` par défaut) :

| Clé | Effet si `0` |
|---|---|
| `region_patch` | Unity garde des réservations de 256 Mo (OOM probable sur 4 Go) |
| `choreographer_patch` | le moteur cherche un Choreographer Android |
| `audio_opensl_patch` / `fmod_patch` | audio probablement muet |
| `time_hook` | le temps vient seulement du flux vsync |
| `vsync_feed` | le moteur attend un vsync qui n'arrive jamais (gel) |
| `cpu_boost` | pas de boost CPU au chargement |
| `tls_anchor = 0` | pages de threads non réservées au démarrage (crash `svcMapMemory FAIL` possible ; 64 par défaut) |
| `gc = disable` | GC jamais lancé (contournement si gel au milieu d'une partie) |
| `jni_forname = 0` | `Class.forName` rend `java/lang/Object` comme avant la v0.1.9 |
| `jni_avariants = 0` | les appels JNI `…MethodA` ne reçoivent plus leurs arguments (avant v0.1.9) |
| `log_level = 2` | log verbeux (lent) |
| `log_sync = 1` | chaque ligne écrite tout de suite, sur le thread qui l'écrit (lent ; pour un gel complet de la console) |
| `zlib_level = 0` | gzip du jeu au niveau d'origine (6) : sauvegarde du replay plus lente en fin de partie |
| `[io] write_behind = 0` | fichiers créés par le jeu (replays) écrits sur son thread, comme avant la v0.1.41 |
| `oc_pool_mb` / `gpu_arena_mb` | mémoire réservée au démarrage (448 / 256 Mo par défaut ; plus = crash « Jungle » possible) |
| `autopsy = 0` | pas de rapport `autopsy.log` pendant un gel |
| `stall_exit_s = 0` | le jeu ne se ferme plus tout seul après un gel (90 s par défaut) |
| `unity_args = ...` | ligne de commande passée à Unity (avancé) |

Diagnostic du gel en ligne (tous à off par défaut) : `[debug] photon_ring = 1`
(anneau des 30 dernières s de datagrammes Photon, dans l'autopsie et le
`debug.log`), `[debug] udp_soak = ip:port` (imite le trafic Photon sans Photon,
voir 4 octies ; `udp_soak_connect = 0` pour une socket non connectée),
`[net] bsd_sessions = 1..8` (sessions bsd:u, défaut 3), `socket_config = 1`
(tampons libnx), `small_ipc = 1`, `local_wait = 1`, `poll_slice_ms`.

`[video] resolution = 1280x720` force le 720p (plus fluide en dock),
`frame_limiter = 0` retire le limiteur 60 Hz du wrapper.
`[video] loading_screen = 0` coupe l'écran de chargement,
`[input] keyboard = 0` le clavier Switch, `[io] cache = 0` le cache de
lecture SD (`cache_mb`, `block_kb`, `readahead_kb` pour le régler),
`[game] hud_scale = N` force la taille du HUD.

## 7. Ce qui a été testé, honnêtement

Voir `docs/STATUS.md` : ce qui a tourné en émulation (Eden, dans la CI) et ce
qui n'a jamais tourné sur une vraie console. Le premier essai sur console
est le vrai test.
