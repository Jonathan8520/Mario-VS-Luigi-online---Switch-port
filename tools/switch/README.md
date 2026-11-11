# PC ↔ Switch tools (Windows)

Scripts used for console tests: send a build to the SD card, fetch the logs,
read a `stall.log` dump. They only touch `sdmc:/switch/mvlo/` (and read
`atmosphere/*_reports`).

MTP = DBI in "MTP responder" mode, which a French Windows shows as
"Périphérique USB MTP" (USB MTP device) → `1: SD Card`. The scripts match that
device name: adjust it for a Windows in another language.

| Script | Usage |
|---|---|
| `mtp_push_verify.ps1 -LocalDir <folder>\mvlo -VerifyDir <tmp>` | copies the folder into `switch/` (merge, replace), then reads every file back from the Switch and compares the SHA-256. For a few files (a `.nro`, `mvlo.ini`). |
| `mtp_sync.ps1 -LocalDir <folder>\mvlo -StageRoot <tmp>` | sends only missing files or files with the wrong size, resumes after a disconnection. For the full data set (~1,585 files, ~80-100 files/min). |
| `mtp_pull.ps1 -OutDir <tmp> [-RecentHours 1]` | brings back `*.log` and `*.ini` from `switch/mvlo/` and the recent Atmosphère reports. |
| `ftp_upload.py HOST PORT /switch/mvlo <folder>...` | the same over FTP (ftpd/sys-ftpd), with resume; under Git Bash, `MSYS_NO_PATHCONV=1`. |
| `sbb.py <command>` | drives the console through the sys-botbase module (TCP 6000): `click A`, `seq "DDOWN,W200,A"`, `shot out.jpg` (screenshot), `ping` (is it still answering? useful during a freeze: it goes down with the whole console's network). Address hard-coded at the top of the script. |
| `prof_report.py prof.log mvlo_nx.elf syms/arm64-v8a [MIN_MS]` | ranks where the game spends each slow frame recorded by the profiler (`[debug] prof_ms`): per thread (UnityMain, loaders), "self" and on-stack functions, call chains. `syms/` = `mvlo-symbols-*.zip` from the encrypted `mvlo-sd-data-*` CI artifact, unzipped (`libunity.so`, `libil2cpp.so`); the `.elf` must come from the same build. |
| `classify_threads.py stall.log mvlo_nx.elf N` | call chain of every thread in the `STALL #N` dump (`NRO+0x…` addresses resolved with the `.elf` **of the same build**). |

MTP pitfalls: `FolderItem.Size` is 0 (read `ExtendedProperty('System.Size')`);
`CopyHere` is asynchronous and dies with the process that started it; two
different `.nro` files can have exactly the same size (hence the SHA-256).
