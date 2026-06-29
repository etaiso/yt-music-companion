// ambient_paint.h — paint the album-derived ambient glow field (SPEC §"Ambient
// glow render", issue V2 slice 6).
//
// PURE and LVGL-INDEPENDENT by design, like palette.c: fills a caller-owned
// RGB565 buffer from a derived palette (slice 5). Two soft elliptical blooms — a
// dominant one near the top and an offset accent — over a dark vertical base,
// transcribed from the V2 design's ambient gradient (NowPlayingDeviceV2.dc.html
// ambient(): radial top + radial accent + linear base).
//
// The renderer paints into a SMALL (~120x120) buffer ONCE per track change, then
// lets LVGL upscale it ~4x to 480x480 — the upscale supplies the blur for free.
// Keeping the bloom math here (no LVGL, no malloc) makes it host-unit-testable
// standalone (see tests/test_ambient.c).
#pragma once

#include <stdint.h>
#include "palette.h"

#ifdef __cplusplus
extern "C" {
#endif

// Paint a `w`*`h` row-major RGB565 (standard r5g6b5) bloom field from `pal` into
// `buf` (caller-owned, >= w*h uint16_t). Pure + deterministic. A NULL palette,
// NULL buffer, or non-positive dims are handled gracefully (NULL pal => the
// fixed neutral wash; bad buffer/dims => no-op).
void ambient_paint(uint16_t *buf, int w, int h, const palette_t *pal);

#ifdef __cplusplus
}
#endif
