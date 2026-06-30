# Ring breath animation — design

**Date:** 2026-06-30
**Status:** Reverted (2026-06-30)

> **Reverted.** On-device testing showed the breath starved touch input *while
> playing* — taps on the transport buttons were dropped and had to be repeated.
> The sim "within budget" measurement (~13.8k px/frame) did not predict the
> device's touch starvation: each accepted breath update invalidates a ring's
> full bounding box (~50k px) and re-composites the 4×-upscaled glow + vignette
> beneath it on the no-2D-accel PSRAM renderer, periodically blocking the LVGL
> task that also polls touch. The breath was also imperceptible (~10% alpha
> swing on two thin rings over 6 s). It paid the full render cost for no visible
> benefit, so `ring_viz_breathe` was removed and the rings are a static halo
> again (0 px/frame in every state). Any future ambient motion must avoid
> per-frame repaints in the ring band — see the perf model. Original design
> below, kept for context.

## Goal

Bring a *subtle ambient life* back to the Now Playing rings without re-creating
the performance regression that made them static. The rings should gently
breathe while a track is actively playing, and stay perfectly still (0 px/frame)
in every other state.

This is cosmetic motion only. It does **not** track real audio energy — the live
feed (ytmdesktop) exposes no audio level, so the old "breathing" was already a
synthesized pulse. We are not reintroducing audio reactivity.

## Background — why the rings went static

`f72ef91` ("fix(ui): center cover/rings, fix low-FPS render-bound Now Playing
screen") removed `ring_viz_update` and made the rings a static halo. The board
renders in software into slow PSRAM with no 2D accel; behind every widget sits
the ambient glow (120×120 canvas bilinearly upscaled 4× to 480×480) plus a
full-screen vignette. Any repaint inside the ring band re-composites those
layers beneath it. The old code resized all three rings + pulsed opacity/width
every frame at 30 fps and spawned a peak ripple ring, re-compositing a ~320×320
region 30×/sec on the same LVGL task that services touch. Steady-state redraw
was ~365k invalidated px/frame; it tanked FPS and starved touch.

The cost of motion is therefore **animated area × update cadence × layer depth**.
The static halo costs 0. To buy motion back we spend sparingly on area and
cadence.

## Approach

Reintroduce a single function in `ui/ring_visualizer.c`:

```c
void ring_viz_breathe(ring_viz_t *rv, bool active);
```

- **Module ownership.** Animation knowledge lives back in `ring_visualizer.c`,
  alongside the geometry constants and the resting-opacity formula. The screen
  just calls it.

- **Call site.** `now_playing_update` calls it once per frame, placed *ahead of
  the change-gate* (next to the existing status-dot pulse, around
  `now_playing_screen.c:415`), with `active = playing`. The change-gate would
  otherwise early-return and freeze the breath, so it must run before the gate —
  exactly like the dot pulse already does.

- **Opacity only, never size.** Radii, border widths, and palette stay fixed.
  The breath modulates each ring's border opacity on a slow sine around its
  resting value. Resting opacity follows the existing curve
  `a = (0.8 - i*0.2) * (0.4 + 0.6*level)`; the breath oscillates the `level`
  term in a narrow band around `STATIC_LEVEL` (0.32). Resizing is deliberately
  avoided — `lv_obj_set_size` invalidates both the old and new bounding box and
  re-runs layout; an opacity change invalidates only the ring's current box.

- **Throttling is the real lever.** `ring_viz_breathe` keeps its own phase
  counter and only pushes an opacity update every Nth call (start N = 3 →
  ~10 Hz, since `now_playing_update` runs at ~30 Hz). Slowing the breath period
  alone does not help — mid-sweep the opacity still changes every frame; cutting
  the *update rate* is what cuts invalidations.

- **Settling on exit.** The function tracks a `was_active` flag. When `active`
  flips false, it restores each ring's exact resting opacity once, then does
  nothing further, so the halo settles back to the static look instead of
  freezing mid-breath. No per-frame cost once settled.

### Tuning knobs

1. **Which rings breathe** (start: inner two).
2. **Update cadence** (start: every 3rd frame ≈ 10 Hz).

Both are dialed against the measured budget below.

## Performance budget & verification

Measure with the desktop sim invalidation counter (per the board-render-perf
notes): temporarily `#define LV_USE_SNAPSHOT 1` in `sim/lv_conf.h` and register
an `LV_EVENT_INVALIDATE_AREA` callback in `sim/main_sim.c` that sums
`(x2-x1+1)*(y2-y1+1)` per area; skip the first ~60 boot frames; divide by
measured frames. Revert the instrumentation afterward.

**Targets:**

- Steady-state while playing: **≤ ~30k invalidated px/frame averaged** (under
  10% of the old ~365k).
- Every non-playing state (paused / idle / buffering / ad / disconnected):
  **0 px/frame**.

Procedure: start with the inner two rings at 10 Hz, measure, and reduce ring
subset and/or cadence until comfortably under budget while still visibly alive.
Then confirm the firmware build passes on the Xtensa toolchain.

## Explicitly out of scope

- Per-frame radius resize of the rings.
- The peak ripple echo ring.
- The synthesized audio-energy `level` math (`ring_viz_update`'s old breathing /
  reduce-motion fallback).
- Any real audio reactivity (no data source exists today).

## Affected files

- `ui/ring_visualizer.h` — declare `ring_viz_breathe`; update the header comment
  that currently states the rings are drawn once and left still.
- `ui/ring_visualizer.c` — implement `ring_viz_breathe` (phase counter, opacity
  sine, settle-on-exit); factor out a resting-opacity helper shared with
  `ring_viz_create`.
- `ui/now_playing_screen.c` — call `ring_viz_breathe(&s_ring, playing)` ahead of
  the change-gate.
- `sim/lv_conf.h`, `sim/main_sim.c` — temporary instrumentation only, reverted
  before completion.
