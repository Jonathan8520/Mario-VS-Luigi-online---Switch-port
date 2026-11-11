"""resolve_stall.py STALL_LOG N ELF SYMDIR [THREAD] -- print one watchdog dump
(STALL #N) with every address named: NRO+0x... through addr2line on the
build's .elf, libunity.so/libil2cpp.so+0x... through the CI symbol files
(SYMDIR/libunity.nm, SYMDIR/libil2cpp.nm = `nm --defined-only -C | sort`).
THREAD (default UnityMain) picks the thread; "all" prints every thread.

usage: python -I resolve_stall.py stall.log 3 mvlo_nx.elf sym/ UnityMain
"""
import bisect
import os
import re
import subprocess
import sys

log, want, elf, symdir = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
only = sys.argv[5] if len(sys.argv) > 5 else "UnityMain"
ADDR2LINE = "C:/devkitPro/devkitA64/bin/aarch64-none-elf-addr2line.exe"

tables = {}
for mod in ("libunity", "libil2cpp"):
    path = os.path.join(symdir, mod + ".nm")
    syms = []
    if os.path.exists(path):
        for line in open(path, encoding="utf-8", errors="replace"):
            p = line.rstrip("\n").split(" ", 2)
            if len(p) == 3 and p[1] in "TtWw":
                syms.append((int(p[0], 16), p[2]))
    syms.sort()
    tables[mod + ".so"] = ([a for a, _ in syms], syms)

lines = open(log, encoding="utf-8", errors="replace").read().splitlines()
start = next(i for i, l in enumerate(lines) if f"STALL #{want} " in l)
end = next((i for i in range(start + 1, len(lines)) if "===== STALL #" in lines[i]), len(lines))
block = lines[start:end]

nro = sorted({m for l in block for m in re.findall(r"NRO\+(0x[0-9a-f]+)", l)})
nro_names = {}
if nro:
    out = subprocess.run([ADDR2LINE, "-f", "-C", "-e", elf] + nro, capture_output=True, text=True).stdout.splitlines()
    nro_names = {a: fn for a, fn in zip(nro, out[0::2])}


def name(tok):
    m = re.match(r"NRO\+(0x[0-9a-f]+)", tok)
    if m:
        return "NRO:" + nro_names.get(m.group(1), tok)
    m = re.match(r"(lib\w+\.so)\+(0x[0-9a-f]+)", tok)
    if m and m.group(1) in tables:
        keys, syms = tables[m.group(1)]
        x = int(m.group(2), 16)
        i = bisect.bisect_right(keys, x) - 1
        if i >= 0:
            return f"{m.group(1).split('.')[0]}:{syms[i][1][:110]} +0x{x - syms[i][0]:x}"
    return tok


print(block[0])
cur = None
for l in block:
    m = re.match(r"\[wd\]   (\S.*?)  PC=(\S+)  LR=(\S+)", l)
    if m:
        cur = m.group(1)
        if only == "all" or cur == only:
            print(f"\n== {cur}\n   PC {name(m.group(2))}\n   LR {name(m.group(3))}")
        continue
    if cur and (only == "all" or cur == only):
        m = re.match(r"\[wd\]\s+(bt\[\d+\]|ret@\S+)\s+(\S+)", l)
        if m:
            print(f"   {m.group(1):<10} {name(m.group(2))}")
