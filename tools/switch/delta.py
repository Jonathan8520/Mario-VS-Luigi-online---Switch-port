"""delta.py OLD NEW OUT -- copy into OUT every file of NEW that is missing from
OLD or differs from it (SHA-256), keeping relative paths.

OLD is a local mirror of what is on the SD card (e.g. the previous artifact),
NEW the new artifact; OUT then holds only what has to be sent.
usage: python -I delta.py old/switch/mvlo new/switch/mvlo out/mvlo
"""
import hashlib
import os
import shutil
import sys


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.digest()


old, new, out = sys.argv[1:4]
changed = 0
for dirpath, _, files in os.walk(new):
    for name in files:
        src = os.path.join(dirpath, name)
        rel = os.path.relpath(src, new)
        prev = os.path.join(old, rel)
        if os.path.isfile(prev) and os.path.getsize(prev) == os.path.getsize(src) and sha(prev) == sha(src):
            continue
        dst = os.path.join(out, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(src, dst)
        changed += 1
        print(("new  " if not os.path.isfile(prev) else "diff ") + rel.replace(os.sep, "/"))
gone = [os.path.relpath(os.path.join(d, n), old) for d, _, fs in os.walk(old) for n in fs
        if not os.path.exists(os.path.join(new, os.path.relpath(os.path.join(d, n), old)))]
print(f"{changed} file(s) to send; {len(gone)} only in OLD (left alone on the SD)")
