#!/usr/bin/env bash
# Builds the PC preview of the loading screen (needs libegl-dev, libgles-dev
# and Mesa's software renderer: apt install libegl-dev libgles-dev libegl-mesa0).
set -euo pipefail
cd "$(dirname "$0")/../.."
cc -O2 -Wall -I source -o tools/splash_preview/splash_preview \
   tools/splash_preview/splash_preview.c source/nx_splash_draw.c source/stb_image_impl.c \
   -lEGL -lGLESv2 -lm
echo "built tools/splash_preview/splash_preview"
