// ring_visualizer.h — the signature concentric rings (SPEC §5, §6)
//
// 3 concentric rings in the brand gradient palette, behind/around the cover,
// drawn as a STATIC halo (fixed radii/opacity). Recolored per track from the
// album palette. The rings used to pulse with audio energy, but on the board's
// software/PSRAM renderer animating them every frame tanked FPS and touch
// responsiveness, so they are now drawn once and left still.
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
