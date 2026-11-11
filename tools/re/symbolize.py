#!/usr/bin/env python3
"""symbolize.py <log> name.so=path/to/name.sym.so [...]

Rewrites every "<name.so>+0xOFFSET" in a mvlo_nx log (debug.log / stall.log
crash dumps and watchdog backtraces) as "<name.so>+0xOFFSET <symbol+0xdelta>"
using the given symbol files. Demangles with c++filt when available.
MIT -- part of mvlo_nx."""
import bisect, re, subprocess, sys
from elftools.elf.elffile import ELFFile

tables = {}
for arg in sys.argv[2:]:
    name, path = arg.split("=", 1)
    e = ELFFile(open(path, "rb"))
    syms = []
    for sec in e.iter_sections():
        if sec["sh_type"] in ("SHT_SYMTAB", "SHT_DYNSYM"):
            for s in sec.iter_symbols():
                if s.name and s["st_value"] and s["st_info"]["type"] in ("STT_FUNC", "STT_NOTYPE"):
                    syms.append((s["st_value"], s.name))
    syms.sort()
    tables[name] = ([a for a, _ in syms], [n for _, n in syms])

def demangle(names):
    try:
        out = subprocess.run(["c++filt"], input="\n".join(names), capture_output=True, text=True).stdout.split("\n")
        return out[:len(names)]
    except Exception:
        return names

pat = re.compile(r"(lib[A-Za-z0-9_]+\.so)\+0x([0-9a-fA-F]+)")
text = open(sys.argv[1], errors="replace").read()
hits = {}
for m in pat.finditer(text):
    lib, off = m.group(1), int(m.group(2), 16)
    if lib in tables:
        addrs, names = tables[lib]
        i = bisect.bisect_right(addrs, off) - 1
        if i >= 0:
            hits[(lib, off)] = (names[i], off - addrs[i])
dem = dict(zip([v[0] for v in hits.values()], demangle([v[0] for v in hits.values()])))
def repl(m):
    k = (m.group(1), int(m.group(2), 16))
    if k not in hits: return m.group(0)
    n, d = hits[k]
    return "%s <%s+0x%x>" % (m.group(0), dem.get(n, n), d)
sys.stdout.write(pat.sub(repl, text))
