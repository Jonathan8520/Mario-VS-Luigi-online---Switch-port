# Outils PC ↔ Switch (Windows)

Scripts utilisés pour les essais sur console : envoyer un build sur la SD,
récupérer les logs, lire un dump de `stall.log`. Ils ne touchent qu'à
`sdmc:/switch/mvlo/` (et lisent `atmosphere/*_reports`).

MTP = DBI en mode « MTP responder », vu par Windows comme
« Périphérique USB MTP » → `1: SD Card`.

| Script | Usage |
|---|---|
| `mtp_push_verify.ps1 -LocalDir <dossier>\mvlo -VerifyDir <tmp>` | copie le dossier dans `switch/` (fusion, remplace), puis relit chaque fichier depuis la Switch et compare le SHA-256. Pour quelques fichiers (un `.nro`, `mvlo.ini`). |
| `mtp_sync.ps1 -LocalDir <dossier>\mvlo -StageRoot <tmp>` | n'envoie que les fichiers absents ou de mauvaise taille, reprend après une déconnexion. Pour le jeu de données complet (~1 585 fichiers, ~80-100 fichiers/min). |
| `mtp_pull.ps1 -OutDir <tmp> [-RecentHours 1]` | rapatrie `*.log` et `*.ini` de `switch/mvlo/` et les rapports Atmosphère récents. |
| `ftp_upload.py HOST PORT /switch/mvlo <dossier>...` | même chose par FTP (ftpd/sys-ftpd), avec reprise ; sous Git Bash, `MSYS_NO_PATHCONV=1`. |
| `sbb.py <commande>` | pilote la console via le module sys-botbase (TCP 6000) : `click A`, `seq "DDOWN,W200,A"`, `shot out.jpg` (capture d'écran), `ping` (répond-il encore ? utile pendant un gel : il tombe avec le réseau de toute la console). Adresse en dur en tête du script. |
| `prof_report.py prof.log mvlo_nx.elf syms/arm64-v8a [MIN_MS]` | classe où le jeu passe chaque image lente notée par le profileur (`[debug] prof_ms`) : par thread (UnityMain, chargeurs), fonctions « self » et sur la pile, chaînes d'appel. `syms/` = l'artifact CI `mvlo-symbols` dézippé (`libunity.so`, `libil2cpp.so`) ; l'`.elf` doit être celui du même build. |
| `classify_threads.py stall.log mvlo_nx.elf N` | chaîne d'appels de chaque thread du dump `STALL #N` (adresses `NRO+0x…` résolues avec l'`.elf` **du même build**). |

Pièges MTP : `FolderItem.Size` vaut 0 (lire `ExtendedProperty('System.Size')`) ;
`CopyHere` est asynchrone et meurt avec le processus qui l'a lancé ; deux
`.nro` différents peuvent avoir exactement la même taille (d'où le SHA-256).
