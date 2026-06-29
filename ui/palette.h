// palette.h — album-art -> ambient/ring palette (SPEC §"Album-derived palette")
//
// PURE and LVGL-INDEPENDENT by design: takes a raw RGB565 cover bitmap and
// returns three ordered stops (light -> mid -> accent) used for both the
// ambient bloom gradient and the ring strokes. No LVGL, no panel, no malloc —
// so it host-unit-tests standalone (see tests/test_palette.c). Meant to run
// ONCE per track change, not per frame.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One palette color as plain 8-bit RGB. Callers convert to lv_color_t at the
// render edge; keeping this LVGL-free is what makes the module host-testable.
typedef struct {
    uint8_t r, g, b;
} palette_rgb_t;

// Three ordered stops: [0] light, [1] mid, [2] accent.
// Lightness descends light -> mid; the accent is the most-saturated stop.
typedef struct {
    palette_rgb_t stop[3];
} palette_t;

// Fixed neutral palette for the no-art case (ad / idle / disconnected, or a
// cover with no usable hue). A cool, desaturated gray ramp.
extern const palette_t PALETTE_NEUTRAL;

// Derive a 3-stop palette from a row-major RGB565 bitmap (`w`*`h` px, standard
// r5g6b5). Downsamples to <=16x16, takes the mean as the base tint and the
// most-saturated cell as the accent, then builds the stops by adjusting
// lightness/saturation. Pure + deterministic. Returns PALETTE_NEUTRAL when
// there is no art (NULL / non-positive dims) or the image has no usable hue
// (e.g. grayscale, pure black/white).
palette_t palette_derive(const uint16_t *rgb565, int w, int h);

// Pure RGB -> HSL. Each output is 0..1 (h wraps). Exposed so callers and tests
// reason about hue/saturation/lightness without duplicating the conversion.
void palette_rgb_to_hsl(palette_rgb_t c, float *h, float *s, float *l);

#ifdef __cplusplus
}
#endif
