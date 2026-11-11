#!/usr/bin/env bash
# package_sd.sh <android-out dir (mvlo.apk [+ symbols.zip])> <output dir>
#
# Lays out exactly what goes on the SD card under sdmc:/switch/mvlo/:
#   libmain.so libunity.so libil2cpp.so [lib_burst_generated.so]
#   assets/bin/Data/...            (straight from the APK)
#   il2cpp/                        (Managed/ pre-extracted, see below)
#   mvlo.ini.example               (config template; the .nro also creates
#                                   mvlo.ini on first boot if it is missing)
#   splash/                        (loading-screen pictures, tools/fetch_splash.py)
#   BUILDINFO.txt                  (BuildIDs, so a log can be matched to data)
# The .nro itself comes from the nro workflow.
#
# MIT -- part of mvlo_nx.
set -euo pipefail
IN="$1"; OUT="$2"
HERE="$(cd "$(dirname "$0")" && pwd)"
DST="$OUT/switch/mvlo"
rm -rf "$OUT"; mkdir -p "$DST"
TMP="$(mktemp -d)"
unzip -q "$IN/mvlo.apk" 'lib/arm64-v8a/*' 'assets/*' -d "$TMP"
cp "$TMP"/lib/arm64-v8a/*.so "$DST/"
cp -r "$TMP/assets" "$DST/assets"
cp "$HERE/../dist/mvlo.ini.example" "$DST/mvlo.ini.example"

# Loading-screen pictures (downloaded now, never stored in the repository).
python3 "$HERE/fetch_splash.py" --out "$DST/splash" || echo "::warning::loading-screen pictures unavailable"

# On first launch UnityInitApplication() copies assets/bin/Data/Managed/ to
# <filesDir>/il2cpp/ ("Extracting il2cpp resources") unless il2cpp/unity.ver
# equals assets/bin/Data/unity_app_guid and every file is already there. On a
# Switch SD card that copy took 105 s of white screen; do it here instead.
# filesDir is the data root (unity_jni.c), so il2cpp/ sits next to assets/.
D="$DST/assets/bin/Data"
if [ -d "$D/Managed" ] && [ -f "$D/unity_app_guid" ]; then
  mkdir -p "$DST/il2cpp"
  cp -r "$D/Managed/." "$DST/il2cpp/"
  cp "$D/unity_app_guid" "$DST/il2cpp/unity.ver"
  echo "il2cpp pre-extracted: $(find "$DST/il2cpp" -type f | wc -l) files, $(du -sh "$DST/il2cpp" | cut -f1)"
else
  echo "::warning::no Managed/ or unity_app_guid in the APK -- Unity will extract il2cpp resources on first launch"
fi
ls -la "$DST"

{
  echo "mvlo_nx SD data"
  echo "built: $(date -u +%Y-%m-%dT%H:%MZ)  port commit: ${GITHUB_SHA:-local}"
  for f in "$DST"/*.so; do
    echo "$(basename "$f"): $(python3 "$HERE/re/buildid.py" "$f")"
  done
} > "$DST/BUILDINFO.txt"
cat "$DST/BUILDINFO.txt"

# libunity must be the build our patch table was derived from.
want=$(grep -o '"[0-9a-f]\{40\}"' "$HERE/../source/mvlo_unity_offsets.h" | tr -d '"')
got=$(python3 "$HERE/re/buildid.py" "$DST/libunity.so")
if [ "$want" != "$got" ]; then
  echo "::warning::libunity.so BuildID $got != $want baked into source/mvlo_unity_offsets.h -- re-run tools/re/derive_unity.py for this Unity version (the wrapper will refuse its engine patches)"
  echo "WARNING: libunity BuildID mismatch ($got vs wrapper $want)" >> "$DST/BUILDINFO.txt"
fi
echo "libunity BuildID matches the wrapper's patch table ($got)"

# Imports none of the wrapper's shim tables provide (each would crash if called).
python3 "$HERE/re/check_imports.py" "$DST"/*.so | tee "$DST/IMPORTS_CHECK.txt" | head -80 || true

# Symbol digest of libil2cpp for crash triage (small text file, not the .so).
if [ -f "$IN/symbols.zip" ]; then
  mkdir -p "$TMP/sym"
  unzip -q "$IN/symbols.zip" -d "$TMP/sym" || true
  find "$TMP/sym" -type f | head -20
  s=$(find "$TMP/sym" -name 'libil2cpp*.so' | head -1 || true)
  if [ -n "$s" ]; then
    python3 "$HERE/re/il2cpp_info.py" "$s" "$DST/libil2cpp.so" > "$DST/IL2CPP_INFO.txt" || echo "::warning::il2cpp_info failed"
    head -60 "$DST/IL2CPP_INFO.txt" || true
  fi
fi
rm -rf "$TMP"
