"""classify_threads.py STALL_LOG ELF STALL_NO -- resolve each thread's PC/LR/bt
of one watchdog dump (NRO+0x... via addr2line on the .elf) and print one line
per thread: name, then the resolved call chain (innermost first)."""
import re
import subprocess
import sys

log, elf, want = sys.argv[1], sys.argv[2], int(sys.argv[3])
ADDR2LINE = "C:/devkitPro/devkitA64/bin/aarch64-none-elf-addr2line.exe"

lines = open(log, encoding="utf-8", errors="replace").read().splitlines()
start = next(i for i, l in enumerate(lines) if f"STALL #{want} " in l)
end = next((i for i in range(start + 1, len(lines)) if "===== STALL #" in lines[i]), len(lines))
block = lines[start:end]

threads = []        # (name, [frames])
cur = None
for l in block:
    m = re.match(r"\[wd\]   (\S.*?)  PC=(\S+)  LR=(\S+)", l)
    if m:
        cur = (m.group(1), [m.group(2), m.group(3)])
        threads.append(cur)
        continue
    m = re.match(r"\[wd\]     bt\[\d+\] (\S+)", l)
    if m and cur:
        cur[1].append(m.group(1))

nro = sorted({f[4:] for _, fs in threads for f in fs if f.startswith("NRO+")})
names = {}
if nro:
    out = subprocess.run([ADDR2LINE, "-f", "-e", elf] + nro, capture_output=True, text=True).stdout.splitlines()
    for a, fn in zip(nro, out[0::2]):
        names["NRO+" + a] = fn

for name, fs in threads:
    chain = [names.get(f, f) for f in fs]
    print(f"{name:32} " + " <- ".join(chain[:7]))
