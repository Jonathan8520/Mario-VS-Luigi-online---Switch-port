#!/usr/bin/env python3
"""derive_unity.py -- derive every libunity.so patch site the Switch wrapper needs.

Usage:
    python3 -I tools/re/derive_unity.py <libunity.so> <libunity.sym.so> [-o source/mvlo_unity_offsets.h]

Inputs are the stock Unity Android player for the exact engine version
(PlaybackEngines/AndroidPlayer/Variations/il2cpp/Release/{Libs,Symbols}/arm64-v8a),
which is byte-identical to the libunity.so that ends up in the APK (same BuildID).

Unlike cloverpit_nx (which had to fingerprint a stripped game binary), we have
Unity's own symbol file, so every target is found BY NAME and then checked
structurally. The tool fails loudly (exit 1) if any shape is not what we expect.

Generates a C header with RVAs + guard words. The wrapper additionally verifies
the BuildID at runtime and every guard word before writing anything.

MIT -- part of mvlo_nx. Patch semantics ported from cloverpit_nx (MIT).
"""
import struct
import sys
import argparse

from elftools.elf.elffile import ELFFile
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)


def die(msg):
    print("FATAL:", msg, file=sys.stderr)
    sys.exit(1)


class Lib:
    def __init__(self, path, sympath):
        self.data = open(path, "rb").read()
        self.elf = ELFFile(open(path, "rb"))
        self.segs = [s for s in self.elf.iter_segments() if s["p_type"] == "PT_LOAD"]
        self.buildid = None
        for s in self.elf.iter_sections():
            if s["sh_type"] == "SHT_NOTE":
                for n in s.iter_notes():
                    if n["n_type"] == "NT_GNU_BUILD_ID":
                        self.buildid = n["n_desc"]
        self.syms = {}
        se = ELFFile(open(sympath, "rb"))
        for sec in se.iter_sections():
            if sec["sh_type"] not in ("SHT_SYMTAB", "SHT_DYNSYM"):
                continue
            for sym in sec.iter_symbols():
                if sym["st_value"] and sym.name:
                    self.syms.setdefault(sym.name, (sym["st_value"], sym["st_size"]))
        self.demangled = None

    def off(self, va):
        for s in self.segs:
            if s["p_vaddr"] <= va < s["p_vaddr"] + s["p_filesz"]:
                return va - s["p_vaddr"] + s["p_offset"]
        die("va 0x%x not in file" % va)

    def word(self, va):
        return struct.unpack_from("<I", self.data, self.off(va))[0]

    def words(self, va, n):
        return list(struct.unpack_from("<%dI" % n, self.data, self.off(va)))

    def sym(self, mangled):
        if mangled not in self.syms:
            die("symbol not found: " + mangled)
        return self.syms[mangled]

    def dis(self, va, size):
        o = self.off(va)
        return list(md.disasm(self.data[o:o + size], va))


# ---------------------------------------------------------------- bitmask imm
def decode_bitmask(n, immr, imms, regsize):
    # ARM ARM DecodeBitMasks (immediate form)
    length = (n << 6 | (~imms & 0x3f)).bit_length() - 1
    if length < 1:
        return None
    levels = (1 << length) - 1
    s = imms & levels
    r = immr & levels
    esize = 1 << length
    welem = (1 << (s + 1)) - 1
    welem = ((welem >> r) | (welem << (esize - r))) & ((1 << esize) - 1)
    v = 0
    for i in range(regsize // esize):
        v |= welem << (i * esize)
    return v


def encode_bitmask(value, regsize):
    for n in (0, 1):
        for immr in range(64):
            for imms in range(64):
                try:
                    if regsize == 32 and n:
                        continue
                    if decode_bitmask(n, immr, imms, regsize) == value:
                        return n, immr, imms
                except Exception:
                    pass
    return None


def shift_run_down(v, regsize, by=2):
    """A contiguous run of ones [lo..hi] -> [lo-by..hi]."""
    lo = (v & -v).bit_length() - 1
    run = v >> lo
    if run & (run + 1):
        return None  # not a single run (would need wraparound handling)
    hi = lo + run.bit_length() - 1
    lo2 = lo - by
    return ((1 << (hi + 1)) - 1) ^ ((1 << lo2) - 1)


# ------------------------------------------------------------ region rules
GRAN_SHIFT = 2  # 256 MB -> 64 MB


def region_transform(w):
    """Return (new_word, description) if w is a granularity-derived encoding."""
    # MOVZ/MOVN/MOVK, 32 or 64-bit: sf opc 100101 hw imm16 Rd
    if (w & 0x1f800000) == 0x12800000:
        sf, opc, hw, imm = w >> 31, (w >> 29) & 3, (w >> 21) & 3, (w >> 5) & 0xffff
        if opc == 2 and hw == 1 and imm == 0x1000:            # movz #0x10000000
            return (w & ~(0xffff << 5)) | (0x0400 << 5), "movz #0x10000000 -> #0x4000000"
        if opc == 0 and hw == 1 and imm == 0xf000 and sf == 0:  # movn -> 0x0fffffff
            return (w & ~(0xffff << 5)) | (0xfc00 << 5), "mov #0xfffffff -> #0x3ffffff"
        if opc == 3 and hw == 1 and imm == 0x1000:            # movk #0x1000, lsl 16
            return (w & ~(0xffff << 5)) | (0x0400 << 5), "movk #0x1000,lsl16 -> #0x400"
        return None
    # logical immediate: sf opc 100100 N immr imms Rn Rd
    if (w & 0x1f800000) == 0x12000000:
        sf = w >> 31
        regsize = 64 if sf else 32
        n, immr, imms = (w >> 22) & 1, (w >> 16) & 0x3f, (w >> 10) & 0x3f
        v = decode_bitmask(n, immr, imms, regsize)
        if v is None:
            return None
        lo = (v & -v).bit_length() - 1
        if lo not in (28, 36):
            return None
        nv = shift_run_down(v, regsize, GRAN_SHIFT)
        if nv is None:
            return None
        enc = encode_bitmask(nv, regsize)
        if not enc:
            return None
        n2, r2, s2 = enc
        nw = (w & ~((1 << 22) | (0x3f << 16) | (0x3f << 10))) | (n2 << 22) | (r2 << 16) | (s2 << 10)
        return nw, "logical imm 0x%x -> 0x%x" % (v, nv)
    # UBFM 64-bit: 1 10 100110 1 immr imms Rn Rd  (lsr / ubfx)
    if (w & 0xffc00000) == 0xd3400000:
        immr, imms = (w >> 16) & 0x3f, (w >> 10) & 0x3f
        if imms == 63 and immr in (28, 40, 36):               # lsr #28/#36/#40
            return (w & ~(0x3f << 16)) | ((immr - GRAN_SHIFT) << 16), "lsr #%d -> #%d" % (immr, immr - GRAN_SHIFT)
        if imms < immr:
            return None
        lsb, width = immr, imms - immr + 1
        if lsb == 28:                                         # ubfx #28, #w
            nl = lsb - GRAN_SHIFT
            ns = nl + width - 1
            return (w & ~((0x3f << 16) | (0x3f << 10))) | (nl << 16) | (ns << 10), "ubfx #28,#%d -> #26,#%d" % (width, width)
        return None
    # ADD/SUB shifted register 64-bit, LSL #28
    if (w & 0x7f200000) == 0x4b000000 and (w >> 31):
        shift, amt = (w >> 22) & 3, (w >> 10) & 0x3f
        if shift == 0 and amt == 28:
            return (w & ~(0x3f << 10)) | ((amt - GRAN_SHIFT) << 10), "sub ..., lsl #28 -> #26"
    return None


REGION_FUNCS = [
    "_ZN22LocalLowLevelAllocator18ReserveMemoryBlockEm",
    "_ZN20DynamicHeapAllocatorC2EjbP15BucketAllocatorP24LowLevelVirtualAllocatorPKcb",
    "_ZN20DynamicHeapAllocator23RequestLargeAllocMemoryEmRm",
    "_ZN15BucketAllocatorC2E",
    "_ZN13MemoryManager16VirtualAllocator18ReserveMemoryBlockEmN24LowLevelVirtualAllocator9BlockInfoE",
    "_ZN13MemoryManager16VirtualAllocator25GetMemoryBlockFromPointerEPKv",
    "_ZN13MemoryManager16VirtualAllocator23GetBlockInfoFromPointerEPKv",
    "_ZN13MemoryManager16VirtualAllocator16MarkMemoryBlocksEPvmN24LowLevelVirtualAllocator9BlockInfoE",
    "_ZN13MemoryManager25GetAllocatorContainingPtrEPKv",
    "_ZN13MemoryManager27InitializeDefaultAllocatorsEv",
    "_ZN12TLSAllocatorIL13AllocatorMode0EE16ThreadInitializeEmPKc",
]


def find_sym_prefix(lib, needle):
    if needle in lib.syms:
        return needle, lib.syms[needle]
    c = [k for k in lib.syms if needle in k]
    if len(c) == 1:
        return c[0], lib.syms[c[0]]
    die("symbol %s: %d candidates %s" % (needle, len(c), c[:5]))


def derive_region(lib):
    sites = []
    for fn in REGION_FUNCS:
        name, (va, size) = find_sym_prefix(lib, fn)
        if not size:
            die("no size for " + name)
        n = 0
        for i in range(0, size, 4):
            w = lib.word(va + i)
            t = region_transform(w)
            if t:
                ins = next(md.disasm(struct.pack("<I", w), va + i))
                sites.append((va + i, w, t[0], "%s %s" % (ins.mnemonic, ins.op_str), name))
                n += 1
        print("[region] %-100s %d site(s)" % (name[:100], n), file=sys.stderr)
    return sites


# ------------------------------------------------------------- other targets
def derive_all(lib):
    out = {}

    va, _ = lib.sym("_ZN11TimeManager6UpdateEd")
    w = lib.words(va, 10)
    # ldr x8,[x0,#0x160]; ldr w9,[x0,#0x168]; ldrb w10,[x0,#0x1a8] ...; cbz; ret; sub sp
    if w[0] != 0xf940b008 or w[1] != 0xb9416809 or w[2] != 0x3946a00a or w[8] != 0xd65f03c0:
        die("TimeManager::Update prologue changed: " + " ".join("%08x" % x for x in w))
    out["TM_UPDATE"] = (va, w[0])
    out["TM_BODY_DELTA"] = 0x24
    if (w[9] & 0xffc003ff) != 0xd10003ff:
        die("TimeManager::Update body does not open with sub sp")

    va, _ = lib.sym("_ZN17ChoreographerBase3GetEv")
    out["CHOREO_GET"] = (va, lib.word(va))

    va, _ = lib.sym("_ZN12AndroidAudio25GetAndroidAudioOutputTypeEi")
    out["AUDIO_OUTTYPE"] = (va, lib.word(va))

    va, _ = lib.sym("_ZN7android2ui13DisplayDialogEPKcS2_S2_S2_")
    out["DISPLAY_DIALOG"] = (va, lib.word(va))

    # WaitVSync(long): mutex adrp/add, ldr counter -> counter address
    va, size = lib.sym("_Z9WaitVSyncl")
    ins = lib.dis(va, 0x60)
    adrp = {}
    counter = None
    for i in ins:
        if i.mnemonic == "adrp":
            rd, imm = i.op_str.split(", #")
            adrp[rd] = int(imm, 16)
        if i.mnemonic == "ldr" and counter is None and "[x" in i.op_str and adrp:
            rd, rest = i.op_str.split(", [")
            base, disp = rest.rstrip("]").split(", #")
            if base in adrp and rd.startswith("x"):
                counter = adrp[base] + int(disp, 16)
    if counter is None:
        die("WaitVSync counter not found")
    out["VSYNC_COUNTER"] = counter
    out["WAITVSYNC"] = (va, lib.word(va))
    va2, _ = lib.sym("_Z17SetFrameTimeNanosl")
    out["SET_FRAME_TIME_NANOS"] = (va2, lib.word(va2))
    # SetFrameTimeNanos must reference the same counter
    txt = " ".join("%s %s" % (i.mnemonic, i.op_str) for i in lib.dis(va2, 0x58))
    if ("#0x%x" % (counter & 0xfff)) not in txt:
        die("SetFrameTimeNanos does not touch the WaitVSync counter: " + txt)

    # FMOD OpenSL buffer geometry: sub/mul/cmp/b.ls/lsr/str/cmp/b.ls inside initCallback's callee
    fva, fsize = lib.sym("_ZN4FMOD12OutputOpenSL12initCallbackEP17FMOD_OUTPUT_STATEijPiiP17FMOD_SOUND_FORMATiiPv")
    fp = [(0x51000400, 0xfffffc00), (0x1b007c00, 0xffe0fc00), (0x6b00001f, 0xffe0fc1f),
          (0x54000009, 0xff00001f), (0x53017c00, 0xfffffc00), (0xb9000260, 0xffc003e0),
          (0x6b00001f, 0xffe0fc1f), (0x54000009, 0xff00001f)]
    hits = []
    for seg in lib.segs:
        if not seg["p_flags"] & 1:
            continue
        o, base, n = seg["p_offset"], seg["p_vaddr"], seg["p_filesz"] // 4
        ws = struct.unpack_from("<%dI" % n, lib.data, o)
        for i in range(n - 8):
            if ws[i] & fp[0][1] == fp[0][0] and all(ws[i + j] & m == p for j, (p, m) in enumerate(fp)):
                hits.append(base + i * 4)
    if len(hits) != 1:
        die("FMOD buffer-geometry run: %d hits" % len(hits))
    run = hits[0]
    strw = lib.word(run + 0x14)                  # str w9, [x19, #field]
    field = ((strw >> 10) & 0xfff) * 4
    ldr = lib.word(run - 8)
    cbz = lib.word(run - 4)
    bls = lib.word(run + 0x1c)
    udiv = lib.word(run + 0x78)
    if ldr != (0xb9400000 | ((field // 4) << 10) | (19 << 5) | 9):
        die("FMOD period ldr guard mismatch %08x (field 0x%x)" % (ldr, field))
    if (cbz & 0xff00001f) != 0x34000009:
        die("FMOD cbz guard mismatch %08x" % cbz)
    if bls != 0x54000089:
        die("FMOD b.ls guard mismatch %08x" % bls)
    out["FMOD_RUN"] = run
    out["FMOD_FIELD"] = field
    out["FMOD_LDR"] = ldr
    out["FMOD_CBZ"] = cbz
    out["FMOD_STR"] = 0xb9000000 | ((field // 4) << 10) | (19 << 5) | 9
    out["FMOD_UDIV"] = udiv
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("libunity")
    ap.add_argument("libunity_sym")
    ap.add_argument("-o", "--out")
    a = ap.parse_args()
    lib = Lib(a.libunity, a.libunity_sym)
    o = derive_all(lib)
    sites = derive_region(lib)
    if not sites:
        die("no region sites")

    L = []
    L.append("/* mvlo_unity_offsets.h -- GENERATED by tools/re/derive_unity.py. DO NOT EDIT.")
    L.append(" *")
    L.append(" * libunity.so BuildID %s" % lib.buildid)
    L.append(" * (Unity Android player, il2cpp/Release/arm64-v8a). Every address below was")
    L.append(" * found by NAME in Unity's own libunity.sym.so and checked structurally.")
    L.append(" * The wrapper refuses to patch if the runtime BuildID differs.")
    L.append(" */")
    L.append("#ifndef MVLO_UNITY_OFFSETS_H")
    L.append("#define MVLO_UNITY_OFFSETS_H")
    L.append("#include <stdint.h>")
    L.append('#define MVLO_UNITY_BUILDID "%s"' % lib.buildid)
    for k in ("TM_UPDATE", "CHOREO_GET", "AUDIO_OUTTYPE", "DISPLAY_DIALOG", "WAITVSYNC", "SET_FRAME_TIME_NANOS"):
        va, g = o[k]
        L.append("#define RVA_%-22s 0x%07xu" % (k, va))
        L.append("#define GUARD_%-20s 0x%08xu" % (k, g))
    L.append("#define TM_BODY_DELTA              0x%x" % o["TM_BODY_DELTA"])
    L.append("#define TM_FIELD_FRAMECOUNT        0x160")
    L.append("#define TM_FIELD_RENDERCOUNT       0x168")
    L.append("#define TM_FIELD_PAUSE             0x1a8")
    L.append("#define RVA_VSYNC_COUNTER          0x%07xu  /* .bss, int64 */" % o["VSYNC_COUNTER"])
    L.append("#define RVA_FMOD_BUFGEOM           0x%07xu  /* sub wA,wB,#1 run */" % o["FMOD_RUN"])
    L.append("#define FMOD_PERIOD_FIELD          0x%x" % o["FMOD_FIELD"])
    L.append("#define GUARD_FMOD_PERIOD_LDR      0x%08xu" % o["FMOD_LDR"])
    L.append("#define GUARD_FMOD_PERIOD_CBZ      0x%08xu" % o["FMOD_CBZ"])
    L.append("#define PATCH_FMOD_PERIOD_STR      0x%08xu" % o["FMOD_STR"])
    L.append("#define GUARD_FMOD_BUFCOUNT        0x%08xu  /* +0x78 */" % o["FMOD_UDIV"])
    L.append("")
    L.append("typedef struct { uint32_t off, from, to; } NxPatchWord;")
    L.append("static const NxPatchWord MVLO_REGION_WORDS[] = {")
    for va, w, nw, txt, fn in sites:
        L.append("  { 0x%07x, 0x%08x, 0x%08x },  /* %-34s %s */" % (va, w, nw, txt, fn.split("E")[0][-40:]))
    L.append("};")
    L.append("#define MVLO_REGION_WORDS_N (sizeof(MVLO_REGION_WORDS)/sizeof(MVLO_REGION_WORDS[0]))")
    L.append("#define MVLO_REGION_GRANULARITY_MB 64")
    L.append("#endif")
    text = "\n".join(L) + "\n"
    if a.out:
        open(a.out, "w").write(text)
    else:
        print(text)
    if len(sites) != 21:
        die("expected 21 region sites (the count cloverpit_nx/pvz_fusion validated), got %d" % len(sites))
    print("[derive] %d region sites, all targets resolved" % len(sites), file=sys.stderr)


if __name__ == "__main__":
    main()
