#!/usr/bin/env python3
"""xwd2txt.py <shot.xwd> [cols] [--png out.png]

Turns an X11 window dump (xwd -root) of the emulator into something readable
in a CI log: colour statistics plus an ASCII thumbnail, so a headless run can
tell "black screen" from "intro logo" from "main menu". Optional PNG output
(needs Pillow). MIT -- part of mvlo_nx."""
import struct
import sys


def load(path):
    d = open(path, "rb").read()
    h = struct.unpack(">25I", d[:100])
    hsize, ver, fmt, depth, w, hgt = h[0], h[1], h[2], h[3], h[4], h[5]
    byte_order, bpp, bpl, ncolors = h[7], h[11], h[12], h[19]
    off = hsize + ncolors * 12
    px = d[off:off + bpl * hgt]
    if bpp != 32:
        raise SystemExit("unsupported bpp %d" % bpp)
    rows = []
    for y in range(hgt):
        line = px[y * bpl:(y * bpl) + w * 4]
        if byte_order == 0:   # LSBFirst: B G R X
            rows.append([(line[i + 2], line[i + 1], line[i]) for i in range(0, w * 4, 4)])
        else:                 # MSBFirst: X R G B
            rows.append([(line[i + 1], line[i + 2], line[i + 3]) for i in range(0, w * 4, 4)])
    return w, hgt, rows


def main():
    path = sys.argv[1]
    cols = int(sys.argv[2]) if len(sys.argv) > 2 and sys.argv[2].isdigit() else 96
    w, h, rows = load(path)
    n = w * h
    lit = 0
    hist = {}
    acc = [0, 0, 0]
    for r in rows:
        for p in r:
            if sum(p) > 30:
                lit += 1
            acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]
            q = (p[0] >> 5, p[1] >> 5, p[2] >> 5)
            hist[q] = hist.get(q, 0) + 1
    print("%s: %dx%d, non-black %.1f%%, mean rgb (%d,%d,%d), %d colour buckets" % (
        path, w, h, 100.0 * lit / n, acc[0] / n, acc[1] / n, acc[2] / n, len(hist)))
    ramp = " .:-=+*#%@"
    lines = max(1, int(cols * h / w / 2))
    for ly in range(lines):
        y = int(ly * h / lines)
        out = []
        for cx in range(cols):
            x = int(cx * w / cols)
            r, g, b = rows[y][x]
            lum = (2 * r + 5 * g + b) / 8
            out.append(ramp[min(len(ramp) - 1, int(lum * len(ramp) / 256))])
        print("|" + "".join(out) + "|")
    if "--png" in sys.argv:
        try:
            from PIL import Image
            im = Image.new("RGB", (w, h))
            im.putdata([p for r in rows for p in r])
            im.save(sys.argv[sys.argv.index("--png") + 1])
        except Exception as e:
            print("png: %s" % e)


if __name__ == "__main__":
    main()
