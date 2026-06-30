// ring_visualizer.h — the signature concentric rings (SPEC §5, §6)
//
// 3 concentric rings in the brand gradient palette, behind/around the cover.
// Radii, widths, and palette are fixed; the rings do NOT resize. While a track
// is playing they breathe via a small, throttled opacity pulse (ring_viz_breathe)
// — opacity only, so each update invalidates just a ring's box, never re-runs
// layout. In every other state they sit perfectly still. Recolored per track
// from the album palette.
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

// Subtle "breath" while playing: gently pulses the inner rings' border opacity
// around their resting value. Call ONCE PER FRAME (ahead of any change-gate),
// passing whether a track is actively playing. Opacity only — radii/widths stay
// fixed. Self-throttles its update rate; when `active` is false it restores the
// static resting opacity once and then costs nothing. No-op-cheap to call every
// frame regardless of state.
void ring_viz_breathe(ring_viz_t *rv, bool active);

#ifdef __cplusplus
}
#endif
