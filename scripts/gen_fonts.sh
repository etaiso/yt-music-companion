#!/usr/bin/env bash
# gen_fonts.sh — regenerate the Inter LVGL text fonts (V2 type scale).
#
# Produces ui/inter_*.c, one LVGL font per V2 type role, from the Inter
# variable TTF. Run from the project root. Companion to gen_icons.sh.
#
# V2 type roles (SPEC §5 / NowPlayingDeviceV2.dc.html — pixel source of truth):
#   title          29 / ExtraBold 800   -> inter_extrabold_29
#   status label   12 / ExtraBold 800   -> inter_extrabold_12  (uppercase row)
#   artist         17 / SemiBold  600   -> inter_semibold_17
#   album          13 / SemiBold  600   -> inter_semibold_13
#   elapsed/total  12 / SemiBold  600   -> inter_semibold_12
#   body / default 12 / Regular   400   -> inter_regular_12   (LVGL default font)
# (Inter 900 is intentionally not bundled — 800 suffices at panel scale.)
#
# Deps: npm i -g lv_font_conv ; pip install fonttools
# Override the interpreters if they are not on PATH, e.g.
#   PYTHON=/path/to/python ./scripts/gen_fonts.sh
set -euo pipefail
cd "$(dirname "$0")/.."

PYTHON="${PYTHON:-python}"
LV_FONT_CONV="${LV_FONT_CONV:-lv_font_conv}"

SRC="assets/Inter.ttf"   # variable font [opsz 14-32, wght 100-900]
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Glyph coverage: printable ASCII + Latin-1 Supplement (accented names from the
# live feed) + the typographic punctuation real track metadata tends to carry
# (en/em dash, curly quotes, bullet, ellipsis). Matches the Montserrat built-ins
# these fonts replace, so live text never loses glyphs.
RANGE="0x20-0x7E,0xA0-0xFF,0x2013,0x2014,0x2018,0x2019,0x201C,0x201D,0x2022,0x2026"

# role|weight|optical-size|render-size
# Inter's opsz axis floors at 14, so the 12/13px cuts instance at opsz=14 (the
# legal minimum) while --size still rasterizes at the true role size.
ROLES="
inter_extrabold_29|800|29|29
inter_extrabold_12|800|14|12
inter_semibold_17|600|17|17
inter_semibold_13|600|14|13
inter_semibold_12|600|14|12
inter_regular_12|400|14|12
"

echo "$ROLES" | while IFS='|' read -r name wght opsz size; do
    [ -z "$name" ] && continue
    # Static instance so the converter gets a single weight/optical-size cut.
    "$PYTHON" -m fontTools.varLib.instancer "$SRC" wght="$wght" opsz="$opsz" \
        -o "$TMP/$name.ttf" >/dev/null
    "$LV_FONT_CONV" --font "$TMP/$name.ttf" --size "$size" --bpp 4 \
        --format lvgl --no-compress -r "$RANGE" -o "ui/$name.c"
    echo "generated ui/$name.c"
done

echo "done: 6 Inter fonts in ui/"
