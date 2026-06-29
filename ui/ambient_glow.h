// ambient_glow.h — the signature V2 album-reactive ambient glow (issue V2 slice 6).
//
// A soft, static glow behind the screen, derived from the current album art. On
// each track change the glow is painted (ambient_paint) into a small ~120x120
// RGB565 canvas in PSRAM, then LVGL upscales it ~4x to fill 480x480 — the
// upscale supplies the blur for free. A native linear-gradient vignette darkens
// the bottom for depth. The layer is STATIC: no per-frame redraw.
//
// Dark-only: in the Light theme (THEME_AMBIENT_ENABLED == 0) nothing is built and
// the screen paints flat. No-art states (ad / idle / disconnected) pass the fixed
// neutral palette so they still read as an intentional wash.
#pragma once

#include "lvgl.h"
#include "palette.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    lv_obj_t *canvas;    // ~120x120 RGB565 canvas, upscaled 4x (NULL in Light theme)
    lv_obj_t *vignette;  // bottom-darkening gradient overlay (NULL in Light theme)
} ambient_glow_t;

// Build the glow + vignette as the back-most children of `parent` (create it
// BEFORE the screen content so it sits behind cover + rings). In Light theme this
// builds nothing and returns {NULL, NULL}.
ambient_glow_t ambient_glow_create(lv_obj_t *parent);

// Repaint the glow canvas from a derived palette. Call ONCE per track change, not
// per frame. NULL => the neutral no-art wash. No-op when the glow is disabled.
void ambient_glow_set_palette(ambient_glow_t *ag, const palette_t *pal);

#ifdef __cplusplus
}
#endif
