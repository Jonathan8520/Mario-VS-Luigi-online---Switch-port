#!/usr/bin/env python3
"""check_imports.py <lib.so>... -- list undefined dynamic symbols of the given
Android libraries that no shim table in source/ provides (and that no other
given library exports). Those resolve to a poison stub at runtime and crash
when called, so every hit here deserves a look.  MIT -- mvlo_nx."""
import re, sys, glob, os
from elftools.elf.elffile import ELFFile
HERE = os.path.dirname(os.path.abspath(__file__))
src = ""
for f in glob.glob(os.path.join(HERE, "..", "..", "source", "*.[ch]")) + glob.glob(os.path.join(HERE, "..", "..", "source", "*.inc")):
    src += open(f, errors="ignore").read()
provided = set(re.findall(r'\{\s*"([A-Za-z_][A-Za-z0-9_@.]*)"\s*,', src))
exports, imports = set(), {}
for p in sys.argv[1:]:
    e = ELFFile(open(p, "rb"))
    ds = e.get_section_by_name(".dynsym")
    for s in ds.iter_symbols():
        if not s.name: continue
        if s["st_shndx"] == "SHN_UNDEF":
            imports.setdefault(s.name, set()).add(os.path.basename(p) + ("(weak)" if s["st_info"]["bind"] == "STB_WEAK" else ""))
        else:
            exports.add(s.name)
missing = sorted(n for n in imports if n not in provided and n not in exports)
print("%d imports, %d provided by shims, %d by the libs themselves, %d MISSING" %
      (len(imports), sum(1 for n in imports if n in provided), sum(1 for n in imports if n in exports and n not in provided), len(missing)))
for n in missing:
    print("  MISSING %-40s %s" % (n, ",".join(sorted(imports[n]))))
