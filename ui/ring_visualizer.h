// ring_visualizer.h — the signature concentric rings (SPEC §5, §6)
//
// 3 concentric rings in the brand gradient palette, behind/around the cover.
// Radii, widths, opacity, and palette are fixed: the rings are a STATIC halo,
// drawn once and never animated per frame. The board renders in software into
// slow PSRAM (no 2D accel) with the 4x-upscaled ambient glow + vignette behind
// every widget, so any per-frame repaint in the ring band re-composites those
// layers and starves the touch input it shares a task with. A fixed halo costs
// 0 px/frame. Recolored per track from the album palette.
#pragma once

#include "lvgl.h"
#include "palette.h"

#ifdef __cplusplus
extern "C" {
#endif

// Create the ring stack centered in `parent`, sized to `box` x `box` px.
// Returns the container; the cover slot is the centered child you place art in.
typedef struct {
    lv_obj_t *cont;        // square container, centered in parent
    lv_obj_t *cover_slot;  // centered cover square (COVER_PX) for art / placeholder
} ring_viz_t;

ring_viz_t ring_viz_create(lv_obj_t *parent, int box);

// Recolor the ring strokes from a derived album palette (slice 5): the three
// ordered stops (light -> mid -> accent) map inner -> outer. Pass NULL (or
// PALETTE_NEUTRAL) for the no-art states (ad / idle / disconnected). Cheap —
// call ONCE per track change, not per frame.
void ring_viz_set_palette(ring_viz_t *rv, const palette_t *pal);

#ifdef __cplusplus
}
#endif
