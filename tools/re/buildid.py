#!/usr/bin/env python3
"""buildid.py <elf> -- print the GNU BuildID (hex) of an ELF file, or 'none'."""
import sys
from elftools.elf.elffile import ELFFile

with open(sys.argv[1], "rb") as fh:
    elf = ELFFile(fh)
    bid = "none"
    for sec in elf.iter_sections():
        if sec["sh_type"] == "SHT_NOTE":
            for n in sec.iter_notes():
                if n["n_type"] == "NT_GNU_BUILD_ID":
                    bid = n["n_desc"]
    print(bid)
