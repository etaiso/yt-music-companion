#!/usr/bin/env bash
# gen_fonts.sh — regenerate the Inter LVGL text fonts (V2 type scale).
#
# Produces ui/inter_*.c, one LVGL font per V2 type role, from the Inter
# variable TTF. Run from the project root. Companion to gen_icons.sh.
#
# V2 type roles — scaled up ~15% from the original 480px mockup sizes so the
# panel reads at arm's length. The sim and board render an identical 480px frame;
# the 2.16" physical panel just shows it ~3x smaller than the sim window, so the
# mockup's px sizes felt tiny in the hand. Bracketed value = original mockup size.
#   title          33 / ExtraBold 800   -> inter_extrabold_33  [was 29]
#   status label   13 / ExtraBold 800   -> inter_extrabold_13  [was 12] uppercase row
#   artist         19 / SemiBold  600   -> inter_semibold_19   [was 17]
#   album          15 / SemiBold  600   -> inter_semibold_15   [was 13]
#   elapsed/total  14 / SemiBold  600   -> inter_semibold_14   [was 12]
#   body / default 12 / Regular   400   -> inter_regular_12    (LVGL default font)
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
# Inter's opsz axis spans 14-32, so cuts below 14 instance at opsz=14 (the legal
# minimum) and the 33px title clamps opsz at 32 (the max); --size still rasterizes
# at the true role size in every case.
ROLES="
inter_extrabold_33|800|32|33
inter_extrabold_13|800|14|13
inter_semibold_19|600|19|19
inter_semibold_15|600|15|15
inter_semibold_14|600|14|14
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
