// ring_visualizer.h — the signature concentric audio rings (SPEC §5, §6)
//
// 3-4 concentric rings in the brand gradient palette, behind/around the cover.
// Driven by a normalized energy `level` (0..1); inner rings react more strongly.
// Honors a reduce-motion fallback (gentle timed pulse instead of energy).
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Create the ring stack centered in `parent`, sized to `box` x `box` px.
// Returns the container; the cover slot is the centered child you place art in.
typedef struct {
    lv_obj_t *cont;        // square container, centered in parent
    lv_obj_t *cover_slot;  // centered 128x128 square for cover art / placeholder
} ring_viz_t;

ring_viz_t ring_viz_create(lv_obj_t *parent, int box);

// Update the rings for this frame. `level` 0..1 drives ring radius/alpha/width
// (design geometry: 3 concentric arcs in the brand gradient, ripples on peaks).
// `reduce_motion` ignores `level` and uses a gentle timed pulse instead.
void ring_viz_update(ring_viz_t *rv, float level, bool reduce_motion);

#ifdef __cplusplus
}
#endif
