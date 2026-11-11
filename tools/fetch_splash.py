#!/usr/bin/env python3
"""fetch_splash.py -- download and convert the loading-screen pictures and the
.nro icon listed in tools/splash_sources.txt (build time, CI only).

    python3 tools/fetch_splash.py --out <dir>/splash [--icon <path>/icon.jpg]

Writes, in the format the console decodes (stb_image: baseline JPEG, PNG):
    studio.png            studio logo, untouched pixel art (RGBA)
    title.png             game logo, SVG rendered / PNG trimmed, <= 1024 wide
    slide1.jpg ...        carousel, 1280x720 (cover crop), baseline JPEG
    SOURCES.txt           where each file came from + sha256 of the download
and, with --icon, a 256x256 baseline JPEG for the .nro.

A failed download is a warning, not an error: the console falls back to its
plain screen, and the build to the repository's icon.jpg. Needs Pillow; an SVG
title also needs rsvg-convert (librsvg2-bin), cairosvg, inkscape or
ImageMagick. MIT -- part of mvlo_nx.
"""
import argparse
import hashlib
import io
import os
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request

try:
    from PIL import Image
except ImportError:
    print("::warning::fetch_splash: Pillow missing (pip install pillow) -- no pictures")
    sys.exit(0)

HERE = os.path.dirname(os.path.abspath(__file__))
UA = "mvlo_nx-build/1.0 (python-urllib)"
W, H = 1280, 720


def warn(msg):
    print("::warning::fetch_splash: " + msg)


def fetch(url):
    last = None
    for attempt in range(3):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": UA})
            with urllib.request.urlopen(req, timeout=30) as r:
                return r.read()
        except Exception as e:          # network errors, HTTP errors
            last = e
            time.sleep(2 * (attempt + 1))
    raise RuntimeError("%s: %s" % (url, last))


def svg_to_png(data, width):
    with tempfile.TemporaryDirectory() as d:
        src, dst = os.path.join(d, "in.svg"), os.path.join(d, "out.png")
        with open(src, "wb") as f:
            f.write(data)
        tries = []
        if shutil.which("rsvg-convert"):
            tries.append(["rsvg-convert", "-w", str(width), src, "-o", dst])
        if shutil.which("inkscape"):
            tries.append(["inkscape", src, "--export-type=png", "-w", str(width), "-o", dst])
        for cmd in tries:
            if subprocess.run(cmd, capture_output=True).returncode == 0 and os.path.exists(dst):
                return Image.open(dst).convert("RGBA")
        try:
            import cairosvg
            return Image.open(io.BytesIO(cairosvg.svg2png(bytestring=data, output_width=width))).convert("RGBA")
        except Exception:
            pass
        for tool in ("magick", "convert"):
            if shutil.which(tool):
                cmd = [tool, "-background", "none", "-density", "400", src, "-resize", "%dx" % width, dst]
                if subprocess.run(cmd, capture_output=True).returncode == 0 and os.path.exists(dst):
                    return Image.open(dst).convert("RGBA")
    raise RuntimeError("no SVG renderer (install librsvg2-bin)")


def cover(im, w, h):
    """Scale to fill w x h, crop the overflow evenly."""
    k = max(w / im.width, h / im.height)
    im = im.resize((max(w, round(im.width * k)), max(h, round(im.height * k))), Image.LANCZOS)
    x, y = (im.width - w) // 2, (im.height - h) // 2
    return im.crop((x, y, x + w, y + h))


def save_jpeg(im, path, quality):
    im.convert("RGB").save(path, "JPEG", quality=quality, optimize=True, progressive=False)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True, help="splash directory to (re)create")
    ap.add_argument("--icon", help="also write the .nro icon here")
    ap.add_argument("--sources", default=os.path.join(HERE, "splash_sources.txt"))
    a = ap.parse_args()

    entries = []
    with open(a.sources, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#"):
                kind, url = line.split(None, 1)
                entries.append((kind, url.strip()))

    if os.path.isdir(a.out):
        shutil.rmtree(a.out)
    os.makedirs(a.out)
    log, nslide, ok = [], 0, 0
    for kind, url in entries:
        if kind == "icon" and not a.icon:
            continue
        try:
            data = fetch(url)
            sha = hashlib.sha256(data).hexdigest()
            if kind == "slide":
                name = "slide%d.jpg" % (nslide + 1)
                im = Image.open(io.BytesIO(data)).convert("RGB")
                save_jpeg(cover(im, W, H), os.path.join(a.out, name), 90)
                nslide += 1
            elif kind == "title":
                name = "title.png"
                if data.lstrip()[:5] in (b"<?xml", b"<svg ") or url.lower().split("?")[0].endswith(".svg"):
                    im = svg_to_png(data, 1024)
                else:
                    im = Image.open(io.BytesIO(data)).convert("RGBA")
                bbox = im.getchannel("A").getbbox()
                if bbox:
                    im = im.crop(bbox)
                if im.width > 1024:
                    im = im.resize((1024, round(im.height * 1024 / im.width)), Image.LANCZOS)
                im.save(os.path.join(a.out, name), optimize=True)
            elif kind == "studio":
                name = "studio.png"
                Image.open(io.BytesIO(data)).convert("RGBA").save(os.path.join(a.out, name), optimize=True)
            elif kind == "icon":
                name = os.path.basename(a.icon)
                os.makedirs(os.path.dirname(os.path.abspath(a.icon)), exist_ok=True)
                save_jpeg(cover(Image.open(io.BytesIO(data)).convert("RGB"), 256, 256), a.icon, 92)
            else:
                warn("unknown kind %r" % kind)
                continue
            ok += 1
            log.append("%-12s %s  sha256:%s" % (name, url, sha))
            print("fetch_splash: %-12s <- %s" % (name, url))
        except Exception as e:
            warn("%s %s: %s" % (kind, url, e))

    with open(os.path.join(a.out, "SOURCES.txt"), "w", encoding="utf-8") as f:
        f.write("Fetched at build time by tools/fetch_splash.py; not part of the repository.\n")
        f.write("\n".join(log) + "\n")
    print("fetch_splash: %d/%d file(s) ready in %s" % (ok, len(entries) - (0 if a.icon else
          sum(1 for k, _ in entries if k == "icon")), a.out))


if __name__ == "__main__":
    main()
