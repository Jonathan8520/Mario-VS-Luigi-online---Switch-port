"""ftp_pull.py HOST PORT OUT_DIR -- fetch the mvlo logs, mvlo.ini and prefs.kv
from sdmc:/switch/mvlo, plus the newest Atmosphère reports, over FTP.
usage: python -I ftp_pull.py 192.168.1.98 5000 logs/
"""
import ftplib
import os
import sys

host, port, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
ftp = ftplib.FTP()
ftp.connect(host, port, timeout=60)
ftp.login()
ftp.sendcmd("TYPE I")


def fetch(remote, local):
    os.makedirs(os.path.dirname(local), exist_ok=True)
    try:
        with open(local, "wb") as f:
            ftp.retrbinary("RETR " + remote, f.write, blocksize=256 * 1024)
        print(f"{os.path.getsize(local):>10}  {remote}")
    except ftplib.error_perm:
        os.remove(local)
        print(f"{'absent':>10}  {remote}")


for name in ("debug.log", "debug.prev.log", "stall.log", "mvlo.ini", "prefs.kv"):
    fetch("/switch/mvlo/" + name, os.path.join(out, "mvlo", name))

# Report names start with a timestamp: the last ones by name are the newest.
for sub in ("crash_reports", "fatal_errors", "erpt_reports"):
    try:
        names = sorted(n.rsplit("/", 1)[-1] for n in ftp.nlst(f"/atmosphere/{sub}"))
    except ftplib.error_perm:
        names = []
    names = [n for n in names if "." in n][-3:]
    print(f"atmosphere/{sub}: {len(names)} newest file(s)")
    for n in names:
        fetch(f"/atmosphere/{sub}/{n}", os.path.join(out, sub, n))
ftp.quit()
