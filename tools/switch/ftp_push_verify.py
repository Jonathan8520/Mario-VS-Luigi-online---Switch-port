"""ftp_push_verify.py HOST PORT REMOTE_DIR LOCAL_DIR -- make REMOTE_DIR hold
LOCAL_DIR's files over FTP (ftpd / sphaira, anonymous): read every remote file
back and compare SHA-256, upload only the ones that differ or are missing,
repeat until all match (so a re-run resumes where a cut transfer stopped).
Reconnects and retries on errors: the console's FTP server takes one
connection at a time and now and then leaves a transfer hanging, hence the
short timeout.

Prints DONE only when every file matched: tell the user the console can leave
FTP mode after that line, not before.
usage: python -I ftp_push_verify.py 192.168.1.98 5000 /switch/mvlo out/mvlo [--check]
--check only compares and lists what differs (DIFF lines), sending nothing.
"""
import ftplib
import hashlib
import os
import socket
import struct
import sys
import time

host, port, remote_root, local_root = sys.argv[1], int(sys.argv[2]), sys.argv[3].rstrip("/"), sys.argv[4]
check_only = "--check" in sys.argv[5:]

files = []
for dirpath, _, names in os.walk(local_root):
    for name in sorted(names):
        path = os.path.join(dirpath, name)
        rel = os.path.relpath(path, local_root).replace(os.sep, "/")
        files.append((path, rel, hashlib.sha256(open(path, "rb").read()).hexdigest()))
total = sum(os.path.getsize(p) for p, _, _ in files)
print(f"{len(files)} files, {total / 1e6:.1f} MB", flush=True)

ftp = None


def connect():
    for _ in range(60):          # up to ~10 min: console asleep, FTP app reopened
        try:
            f = ftplib.FTP()
            f.connect(host, port, timeout=15)
            f.login()
            f.sendcmd("TYPE I")
            return f
        except (OSError, EOFError, ftplib.Error) as e:
            print(f"connect failed ({e}), waiting...", flush=True)
            time.sleep(10)
    sys.exit("FTP server unreachable, giving up")


def retrying(what, fn):
    global ftp
    for attempt in range(5):
        try:
            if ftp is None:
                ftp = connect()
            return fn()
        except (OSError, EOFError, ftplib.Error) as e:
            print(f"retry {attempt + 1} {what}: {e}", flush=True)
            try:
                ftp.close()
            except Exception:
                pass
            ftp = None
            time.sleep(2)
    sys.exit(f"FAILED {what}")


made = set()


def ensure_dirs(rel):
    cur = remote_root
    for part in rel.split("/")[:-1]:
        cur += "/" + part
        if cur not in made:
            try:
                ftp.mkd(cur)
            except ftplib.error_perm:
                pass             # exists
            made.add(cur)


def remote_hash(rel):
    def get():
        h = hashlib.sha256()
        try:
            ftp.retrbinary(f"RETR {remote_root}/{rel}", h.update, blocksize=256 * 1024)
        except ftplib.error_perm:
            return None          # missing
        return h.hexdigest()
    return retrying("RETR " + rel, get)


def remote_size(rel):
    def get():
        try:
            return ftp.size(f"{remote_root}/{rel}")
        except ftplib.error_perm:
            return None          # missing, or no SIZE command
    return retrying("SIZE " + rel, get)


def put(path, rel):
    def go():
        global ftp
        ensure_dirs(rel)
        with open(path, "rb") as f:
            data = f.read()
        # DBI's server now and then never sees the end of a STOR (2026-11-03:
        # every byte received, then stuck for minutes, single-threaded; the
        # connections we gave up on meanwhile then kill the server: "accept:
        # 113"). Only a reset of the control connection gets it out (426,
        # every byte written), so: wait 5 s for it to close the data
        # connection, else reset the control connection before anything else
        # connects. The next round checks the file anyway.
        with ftp.transfercmd(f"STOR {remote_root}/{rel}") as conn:
            conn.sendall(data)
            time.sleep(0.3)
            conn.shutdown(socket.SHUT_WR)
            conn.settimeout(5)
            try:
                while conn.recv(4096):
                    pass
                stuck = False
            except OSError:
                stuck = True
        if stuck:
            print(f"reset   {rel}", flush=True)
            linger = struct.pack("HH" if os.name == "nt" else "ii", 1, 0)
            ftp.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, linger)
            ftp.sock.close()
            ftp = None
            time.sleep(1)
            return
        ftp.voidresp()
    retrying("STOR " + rel, go)


start = time.time()
todo = files
for rnd in range(1, 5):
    left = []
    for i, (path, rel, want) in enumerate(todo, 1):
        size = os.path.getsize(path)
        # Another size: no need to read the old file back to know it differs.
        ok = remote_size(rel) == size and remote_hash(rel) == want
        tag = "OK  " if ok else "DIFF" if check_only else "SEND"
        print(f"{tag} {size:>10}  {rel}  [{i}/{len(todo)}, {time.time() - start:.0f}s]", flush=True)
        if not ok:
            if not check_only:
                put(path, rel)
            left.append((path, rel, want))
    if check_only:
        if left:
            sys.exit(f"{len(left)} file(s) differ")
        print(f"SAME: all {len(files)} files match ({time.time() - start:.0f}s)", flush=True)
        sys.exit(0)
    if not left:
        break
    print(f"round {rnd}: {len(left)} file(s) sent, checking them again...", flush=True)
    todo = left
else:
    sys.exit(f"{len(todo)} file(s) still differ after 4 rounds")
try:
    ftp.quit()
except Exception:
    pass
print(f"DONE: all {len(files)} files verified by SHA-256 ({time.time() - start:.0f}s)", flush=True)
