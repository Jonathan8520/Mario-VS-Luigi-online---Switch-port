#!/usr/bin/env python3
"""il2cpp_info.py <libil2cpp.sym.so> <libil2cpp.so>

Small text digest of the game's libil2cpp for crash triage and for checking
the runtime-learned GC bridge (source/nx_gc.c) against what the binary says:
  - BuildIDs of both files (they must match);
  - addresses of the Boehm GC stop-the-world functions and globals, if the
    symbol file has them;
  - the signal numbers GC_stop_init stores (expected SIGPWR=30, SIGXCPU=24).
"""
import sys
import struct
from elftools.elf.elffile import ELFFile
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM


def buildid(elf):
    for sec in elf.iter_sections():
        if sec["sh_type"] == "SHT_NOTE":
            for n in sec.iter_notes():
                if n["n_type"] == "NT_GNU_BUILD_ID":
                    return n["n_desc"]
    return "none"


sym = ELFFile(open(sys.argv[1], "rb"))
lib_data = open(sys.argv[2], "rb").read()
lib = ELFFile(open(sys.argv[2], "rb"))
print("sym BuildID ", buildid(sym))
print("lib BuildID ", buildid(lib))

syms = {}
for sec in sym.iter_sections():
    if sec["sh_type"] in ("SHT_SYMTAB", "SHT_DYNSYM"):
        for s in sec.iter_symbols():
            if s.name and s["st_value"]:
                syms[s.name] = (s["st_value"], s["st_size"])
print("symbols     ", len(syms))
WANT = ["GC_stop_world", "GC_start_world", "GC_suspend_all", "GC_restart_all",
        "GC_stop_init", "GC_suspend_handler", "GC_suspend_handler_inner",
        "GC_restart_handler", "GC_sig_suspend", "GC_sig_thr_restart",
        "GC_suspend_ack_sem", "GC_retry_signals", "GC_push_all_stacks",
        "GC_init", "il2cpp_init", "il2cpp_gc_disable", "il2cpp_gc_enable"]
for w in WANT:
    v = syms.get(w)
    print("  %-26s %s" % (w, ("0x%x size %d" % v) if v else "-"))

segs = [s for s in lib.iter_segments() if s["p_type"] == "PT_LOAD"]


def read(va, n):
    for s in segs:
        if s["p_vaddr"] <= va < s["p_vaddr"] + s["p_filesz"]:
            o = va - s["p_vaddr"] + s["p_offset"]
            return lib_data[o:o + n]
    return b""


if "GC_stop_init" in syms:
    va, size = syms["GC_stop_init"]
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    print("GC_stop_init disassembly (first 48 insns):")
    for i, ins in enumerate(md.disasm(read(va, min(size or 0x200, 0x200)), va)):
        if i >= 48:
            break
        print("   0x%x  %s %s" % (ins.address, ins.mnemonic, ins.op_str))
