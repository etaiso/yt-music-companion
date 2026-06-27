#!/usr/bin/env bash
# gen_icons.sh — regenerate the Material Symbols LVGL icon fonts.
#
# Produces ui/mdi_solid.c (FILL 1) and ui/mdi_line.c (FILL 0) from the
# Material Symbols Rounded variable TTF. Run from the project root.
#
# Deps: npm i -g lv_font_conv ; pip install fonttools
set -euo pipefail
cd "$(dirname "$0")/.."

SRC="assets/MaterialSymbolsRounded.ttf"   # variable font [FILL,GRAD,opsz,wght]
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Static instances so the converter gets one fill each.
fonttools varLib.instancer "$SRC" FILL=1 wght=500 GRAD=0 opsz=24 \
  -o "$TMP/msym-fill.ttf" >/dev/null
fonttools varLib.instancer "$SRC" FILL=0 wght=500 GRAD=0 opsz=24 \
  -o "$TMP/msym-line.ttf" >/dev/null

# Solid: skip_previous, skip_next, play_arrow, pause, thumb_up
lv_font_conv --font "$TMP/msym-fill.ttf" --size 30 --bpp 4 --format lvgl \
  --no-compress -r 0xE045,0xE044,0xE037,0xE034,0xE8DC -o ui/mdi_solid.c
# Line: thumb_up, thumb_down
lv_font_conv --font "$TMP/msym-line.ttf" --size 26 --bpp 4 --format lvgl \
  --no-compress -r 0xE8DC,0xE8DB -o ui/mdi_line.c

echo "generated ui/mdi_solid.c + ui/mdi_line.c"
