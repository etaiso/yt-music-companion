// ring_visualizer.h — the signature concentric audio rings (SPEC §5, §6)
//
// 3-4 concentric rings in the brand gradient palette, behind/around the cover.
// Driven by a normalized energy `level` (0..1); inner rings react more strongly.
// Honors a reduce-motion fallback (gentle timed pulse instead of energy).
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
// ordered stops (light -> mid -> accent) map inner -> outer, and the peak-ripple
// echo takes the accent. Pass NULL (or PALETTE_NEUTRAL) for the no-art states
// (ad / idle / disconnected). Cheap — call ONCE per track change, not per frame.
void ring_viz_set_palette(ring_viz_t *rv, const palette_t *pal);

// Update the rings for this frame. `level` 0..1 drives ring radius/alpha/width
// (V2 geometry: 3 concentric arcs in the album palette, ripples on peaks).
// `reduce_motion` ignores `level` and uses a gentle timed pulse instead.
void ring_viz_update(ring_viz_t *rv, float level, bool reduce_motion);

#ifdef __cplusplus
}
#endif
