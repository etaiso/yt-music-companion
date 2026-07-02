#!/usr/bin/env bash
# Regression check for the once-per-second marquee stutter (2026-07-03).
#
# While a song plays, position_sec ticks once per second; that tick passes
# now_playing_update's change-gate and must repaint ONLY what actually changed
# (elapsed label + slider). A regression re-runs unconditional style/image
# setters (LVGL invalidates even when the value is unchanged), repainting the
# cover/like/status regions over the expensive ambient glow — which reads as a
# visible hiccup on the board's software/PSRAM renderer.
#
# Runs the sim's SIM_MARQUEE_PERF scenario headless and asserts the largest
# invalidation burst per main-loop iteration (after warmup) stays within
# budget. Baseline marquee frame ~20k px; a legit position tick adds the
# progress row (~15k px). The broken state measured ~298k px.
#
# Requires the msys2 MinGW64 toolchain + SDL2 (see sim/README.md).
set -euo pipefail
cd "$(dirname "$0")/../sim"

cmake -B build -G Ninja -DCMAKE_C_COMPILER=gcc >/dev/null
cmake --build build >/dev/null

max=$(SIM_MARQUEE_PERF=1 SIM_MAX_FRAMES=600 SDL_VIDEODRIVER=dummy ./build/ytm_sim.exe \
      | awk '$1 == "inval" && $2 > 60 { if ($3 > m) m = $3 } END { print m + 0 }')

budget=60000
echo "max invalidated px/iteration (steady state): $max (budget $budget)"
if [ "$max" -gt "$budget" ]; then
    echo "FAIL: per-second update repaints more than the progress row + marquee"
    exit 1
fi
echo "OK"
