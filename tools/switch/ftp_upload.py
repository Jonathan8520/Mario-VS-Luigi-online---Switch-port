"""Upload local trees to a Switch FTP server, keeping relative paths.

usage: ftp_upload.py HOST PORT REMOTE_ROOT LOCAL_DIR [LOCAL_DIR ...]
Files of later LOCAL_DIRs are uploaded after earlier ones (put the .nro last).
"""
import ftplib
import os
import sys
import time

host, port, remote_root = sys.argv[1], int(sys.argv[2]), sys.argv[3].rstrip("/")
local_dirs = sys.argv[4:]

jobs = []
for base in local_dirs:
    for dirpath, _, files in os.walk(base):
        for name in sorted(files):
            local = os.path.join(dirpath, name)
            rel = os.path.relpath(local, base).replace(os.sep, "/")
            jobs.append((local, f"{remote_root}/{rel}"))

total_bytes = sum(os.path.getsize(l) for l, _ in jobs)
print(f"{len(jobs)} files, {total_bytes / 1e6:.1f} MB", flush=True)


def connect():
    # Wait up to ~10 min for the server (console asleep, FTP app reopened...).
    for attempt in range(60):
        try:
            ftp = ftplib.FTP()
            ftp.connect(host, port, timeout=60)
            ftp.login()
            ftp.sendcmd("TYPE I")
            return ftp
        except (OSError, EOFError, ftplib.Error) as e:
            print(f"connect failed ({e}), waiting...", flush=True)
            time.sleep(10)
    sys.exit("server unreachable, giving up")


def remote_size(ftp, path):
    try:
        return ftp.size(path)
    except ftplib.error_perm:
        return None


made = set()


def ensure_dir(ftp, path):
    parts = path.strip("/").split("/")[:-1]
    cur = ""
    for p in parts:
        cur += "/" + p
        if cur in made:
            continue
        try:
            ftp.mkd(cur)
        except ftplib.error_perm:
            pass  # already exists
        made.add(cur)


ftp = connect()
done_bytes = 0
start = time.time()
for i, (local, remote) in enumerate(jobs, 1):
    size = os.path.getsize(local)
    skipped = False
    for attempt in range(5):
        try:
            if remote_size(ftp, remote) == size:
                skipped = True
                break
            ensure_dir(ftp, remote)
            with open(local, "rb") as f:
                ftp.storbinary(f"STOR {remote}", f, blocksize=256 * 1024)
            break
        except (ftplib.all_errors + (EOFError,)) as e:
            print(f"retry {attempt + 1} {remote}: {e}", flush=True)
            time.sleep(2)
            try:
                ftp.close()
            except Exception:
                pass
            ftp = connect()
    else:
        sys.exit(f"FAILED {remote}")
    done_bytes += size
    if i % 100 == 0 or size > 5_000_000 or i == len(jobs):
        el = time.time() - start
        print(f"[{i}/{len(jobs)}] {done_bytes / 1e6:.0f}/{total_bytes / 1e6:.0f} MB "
              f"{done_bytes / 1e6 / max(el, 1e-3):.1f} MB/s  {remote}", flush=True)
ftp.quit()
print(f"DONE in {time.time() - start:.0f}s", flush=True)
