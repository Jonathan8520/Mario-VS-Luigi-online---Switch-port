"""prof_report.py PROF_LOG ELF SYMDIR [MIN_MS] -- rank where the game's threads
spent each slow frame recorded by the sampling profiler ([debug] prof_ms,
nx_prof.c).

ELF    = that build's mvlo_nx.elf (NRO+0x... addresses)
SYMDIR = folder with the CI `mvlo-symbols` libunity.so / libil2cpp.so
         (unstripped); their symbol tables are cached as SYMDIR/<lib>.nm.
MIN_MS = only frames at least this long (default 0).

Per frame and thread (T0 UnityMain, T1 Loading.AsyncRead, T2
Loading.PreloadManager; untagged logs before v0.1.32 are all UnityMain):
"self" = the sampled PC was in that function, "total" = the function was
anywhere on the sampled call chain (frame pointers), as a share of the
thread's samples (one sample = 4 ms). Then the most frequent chains.

usage: python -I prof_report.py prof.log mvlo_nx.elf syms/arm64-v8a 500
"""
import bisect, collections, os, re, subprocess, sys

NM = r"C:/devkitPro/devkitA64/bin/aarch64-none-elf-nm.exe"
THREADS = {0: "UnityMain", 1: "Loading.AsyncRead", 2: "Loading.PreloadManager"}


def load_syms(path, cache):
    if not os.path.exists(cache) or os.path.getmtime(cache) < os.path.getmtime(path):
        out = subprocess.run([NM, "--defined-only", "-C", "-n", path], capture_output=True, text=True,
                             errors="replace").stdout
        with open(cache, "w", encoding="utf-8") as f:
            f.write(out)
    addrs, names = [], []
    with open(cache, encoding="utf-8", errors="replace") as f:
        for line in f:
            p = line.rstrip("\n").split(" ", 2)
            if len(p) < 3 or p[1] not in "tTwW":
                continue
            addrs.append(int(p[0], 16))
            names.append(p[2])
    return addrs, names


def report(fr, tname, samples, name):
    n = len(samples)
    own, tot, chains, mods = (collections.Counter() for _ in range(4))
    for s in samples:
        names = [name(t, i == 0) for i, t in enumerate(s)]
        own[names[0]] += 1
        mods[names[0].rsplit("[", 1)[-1].rstrip("]") if "[" in names[0] else "?"] += 1
        seen = []
        for x in names:
            if x not in seen:
                seen.append(x)
        for x in seen:
            tot[x] += 1
        chains[" <- ".join(seen[:4])] += 1
    print(f"\n===== frame {fr['frame']}: {fr['ms']} ms -- {tname}: {n} samples (~{n * 4} ms covered) =====")
    print("modules (self): " + ", ".join(f"{k} {100 * v // n}%" for k, v in mods.most_common()))
    print("-- self --")
    for k, v in own.most_common(15):
        print(f"{100 * v / n:5.1f}%  {k[:150]}")
    print("-- total (on the call chain) --")
    for k, v in tot.most_common(25):
        print(f"{100 * v / n:5.1f}%  {k[:150]}")
    print("-- chains (leaf <- callers) --")
    for k, v in chains.most_common(8):
        print(f"{100 * v / n:5.1f}%  {k[:300]}")


def main(argv):
    log, elf, symdir = argv[1], argv[2], argv[3]
    min_ms = int(argv[4]) if len(argv) > 4 else 0
    tables = {"NRO": load_syms(elf, os.path.join(symdir, "mvlo_nx.nm"))}
    for lib in ("libunity.so", "libil2cpp.so"):
        p = os.path.join(symdir, lib)
        if os.path.exists(p):
            tables[lib] = load_syms(p, os.path.join(symdir, lib[:-3] + ".nm"))

    cache = {}

    def name(tok, is_pc):
        if tok in cache:
            return cache[tok]
        m = re.match(r"(.+)\+0x([0-9a-f]+)$", tok)
        if not m or m.group(1) not in tables:
            cache[tok] = tok
            return tok
        mod, off = m.group(1), int(m.group(2), 16)
        addrs, names = tables[mod]
        i = bisect.bisect_right(addrs, off if is_pc else off - 1) - 1
        r = f"{names[i]} [{mod}]" if i >= 0 else tok
        cache[tok] = r
        return r

    frames, cur = [], None
    for line in open(log, encoding="utf-8", errors="replace"):
        if line.startswith("=== frame"):
            m = re.match(r"=== frame (\d+): (\d+) ms, (\d+) samples", line)
            cur = {"frame": int(m.group(1)), "ms": int(m.group(2)), "samples": collections.defaultdict(list)}
            frames.append(cur)
        elif cur is not None and line.strip():
            toks = line.split()
            th = 0
            if len(toks) > 1 and re.match(r"T\d+$", toks[1]):
                th = int(toks[1][1:])
                toks = toks[1:]
            cur["samples"][th].append(toks[1:])

    for fr in frames:
        if fr["ms"] < min_ms:
            continue
        for th in sorted(fr["samples"]):
            report(fr, THREADS.get(th, f"T{th}"), fr["samples"][th], name)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
