# Ring Breath Animation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reintroduce a subtle, opacity-only "breath" on the Now Playing rings while a track is playing, sized to a measured invalidation budget so it never re-creates the render-bound regression that made the rings static.

**Architecture:** Add `ring_viz_breathe(rv, active)` to `ui/ring_visualizer.c`. The screen calls it every frame *ahead of the change-gate* (mirroring the existing status-dot pulse), with `active = playing`. It modulates only border *opacity* (never size), throttles its own update rate, and restores the static resting opacity once when it goes inactive. Verified in the desktop SDL sim with a temporary invalidation counter, then confirmed to compile in the firmware tree.

**Tech Stack:** C, LVGL v9, desktop SDL2 sim (MinGW-w64 / Ninja), ESP-IDF firmware build (Xtensa).

> **Note on testing:** this repo has **no unit-test harness for UI** — the desktop sim is the behavioral check (see the sim README and the board-render-perf notes). So the usual red/green TDD loop is replaced by: build the sim → run headless → read the invalidation counter → confirm against budget. This is a deliberate, codebase-specific substitution.

> **Toolchain prerequisites (every sim command runs in the Bash tool):**
> ```bash
> export PATH="/c/msys64/mingw64/bin:$PATH"   # gcc, cmake, ninja, SDL2.dll
> cd /c/Users/Etai/Projects/yt-music-companion/.claude/worktrees/sleepy-pascal-e8e4ef/sim
> ```

---

## File Structure

- `ui/ring_visualizer.h` — declare `ring_viz_breathe`; correct the header comment that currently claims the rings are drawn once and never move.
- `ui/ring_visualizer.c` — factor a `ring_opa(i, level)` resting-opacity helper (shared with `ring_viz_create`); implement `ring_viz_breathe`.
- `ui/now_playing_screen.c` — call `ring_viz_breathe(&s_ring, playing)` ahead of the change-gate.
- `sim/main_sim.c` — **temporary** invalidation counter for measurement/tuning; fully reverted in Task 4.

---

## Task 1: Add the breath function (inner two rings @ ~10 Hz)

**Files:**
- Modify: `ui/ring_visualizer.h`
- Modify: `ui/ring_visualizer.c`
- Modify: `ui/now_playing_screen.c:401-437` (insert call ahead of change-gate)

- [ ] **Step 1: Declare the function and fix the header comment**

In `ui/ring_visualizer.h`, replace the comment block at the top (lines 1–7) so it no longer claims the rings never move:

```c
// ring_visualizer.h — the signature concentric rings (SPEC §5, §6)
//
// 3 concentric rings in the brand gradient palette, behind/around the cover.
// Radii, widths, and palette are fixed; the rings do NOT resize. While a track
// is playing they breathe via a small, throttled opacity pulse (ring_viz_breathe)
// — opacity only, so each update invalidates just a ring's box, never re-runs
// layout. In every other state they sit perfectly still. Recolored per track
// from the album palette.
```

Then add the declaration after `ring_viz_set_palette` (after line 30):

```c
// Subtle "breath" while playing: gently pulses the inner rings' border opacity
// around their resting value. Call ONCE PER FRAME (ahead of any change-gate),
// passing whether a track is actively playing. Opacity only — radii/widths stay
// fixed. Self-throttles its update rate; when `active` is false it restores the
// static resting opacity once and then costs nothing. No-op-cheap to call every
// frame regardless of state.
void ring_viz_breathe(ring_viz_t *rv, bool active);
```

- [ ] **Step 2: Factor the resting-opacity helper in `ring_visualizer.c`**

Add this helper just above `ring_viz_create` (after `make_ring`, ~line 59):

```c
// Resting border opacity for ring i at a given energy level — the exact curve
// the static halo is drawn at. Shared by create and breathe so both agree.
static lv_opa_t ring_opa(int i, float level)
{
    float a = (0.8f - i * 0.2f) * (0.4f + 0.6f * level);
    if (a < 0.0f) a = 0.0f;
    if (a > 1.0f) a = 1.0f;
    return (lv_opa_t)(a * 255.0f + 0.5f);
}
```

Then in `ring_viz_create`, replace the inline opacity calc (lines 79–80):

```c
        float a = (0.8f - i * 0.2f) * (0.4f + 0.6f * STATIC_LEVEL);
        lv_obj_set_style_border_opa(s_rings[i], (lv_opa_t)(a * 255.0f), 0);
```

with:

```c
        lv_obj_set_style_border_opa(s_rings[i], ring_opa(i, STATIC_LEVEL), 0);
```

- [ ] **Step 3: Implement `ring_viz_breathe`**

Append to `ui/ring_visualizer.c` (after `ring_viz_set_palette`):

```c
// Breath tuning. Decimate caps the update rate (now_playing_update ticks ~30 Hz,
// so 3 -> ~10 Hz). Only the inner BREATH_RINGS rings breathe (inner rings have the
// smallest bounding boxes -> the cheapest invalidations). Amplitude/rate are a
// slow, gentle swing around STATIC_LEVEL.
#define BREATH_DECIMATE 3      // push an opacity update every Nth call
#define BREATH_RINGS    2      // inner-most N rings breathe (1..RING_COUNT)
#define BREATH_AMPL     0.10f  // +/- level swing around STATIC_LEVEL
#define BREATH_RATE     0.10f  // sine phase increment per accepted update

void ring_viz_breathe(ring_viz_t *rv, bool active)
{
    (void)rv;
    static bool     was_active;
    static uint32_t calls;
    static uint32_t steps;

    if (!active) {
        // Settle back to the static halo exactly once; then no per-frame cost.
        if (was_active) {
            for (int i = 0; i < RING_COUNT; i++)
                lv_obj_set_style_border_opa(s_rings[i], ring_opa(i, STATIC_LEVEL), 0);
            was_active = false;
        }
        return;
    }
    was_active = true;

    // Throttle: only touch styles every BREATH_DECIMATE calls.
    if (calls++ % BREATH_DECIMATE != 0) return;

    float level = STATIC_LEVEL + BREATH_AMPL * sinf((float)(steps++) * BREATH_RATE);
    for (int i = 0; i < BREATH_RINGS && i < RING_COUNT; i++)
        lv_obj_set_style_border_opa(s_rings[i], ring_opa(i, level), 0);
}
```

> Note: `s_rings[0]` is the **innermost** ring (radius `CR + BASE_GAP + i*STEP + …`), so `i < BREATH_RINGS` breathes the inner rings — the smallest boxes. `sinf` is already available (`#include <math.h>` at line 3).

- [ ] **Step 4: Call it ahead of the change-gate**

In `ui/now_playing_screen.c`, insert immediately after the status-dot pulse block (after line 418, before the `// ---- change-gate` comment at line 420):

```c
    // ---- ring breath: a subtle opacity pulse on the halo while playing, kept
    // ahead of the change-gate (like the dot pulse above) so it animates without
    // a full widget rebuild. Opacity-only + throttled; settles to the static halo
    // when not playing. See ring_viz_breathe.
    ring_viz_breathe(&s_ring, playing);
```

- [ ] **Step 5: Build the sim and run headless (compile + no-crash check)**

```bash
export PATH="/c/msys64/mingw64/bin:$PATH"
cd /c/Users/Etai/Projects/yt-music-companion/.claude/worktrees/sleepy-pascal-e8e4ef/sim
cmake -B build -G Ninja -DCMAKE_C_COMPILER=gcc
cmake --build build
SIM_MAX_FRAMES=150 SDL_VIDEODRIVER=dummy ./build/ytm_sim.exe
```

Expected: build succeeds with no warnings on the touched files; the run prints `headless: 150 frames rendered, exiting.` and exits 0.

- [ ] **Step 6: Commit**

```bash
git add ui/ring_visualizer.h ui/ring_visualizer.c ui/now_playing_screen.c
git commit -m "feat(ui): subtle opacity breath on rings while playing

Reintroduce ring motion as a throttled, opacity-only pulse on the inner
rings, run ahead of the change-gate and gated to the playing state. No
resize (avoids the old layout + old/new-bbox invalidation). Settles to the
static halo when not playing.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 2: Measure invalidation (temporary instrumentation)

**Files:**
- Modify: `sim/main_sim.c` (temporary; reverted in Task 4)

- [ ] **Step 1: Add the invalidation counter**

In `sim/main_sim.c`, add after the includes (after line 13):

```c
// --- TEMP invalidation measurement (remove in cleanup task) ---
static long g_inval_px;
static long g_bucket_frames;
static void inval_cb(lv_event_t *e)
{
    const lv_area_t *a = (const lv_area_t *)lv_event_get_param(e);
    g_inval_px += (long)(a->x2 - a->x1 + 1) * (long)(a->y2 - a->y1 + 1);
}
```

- [ ] **Step 2: Capture the display and register the callback**

Replace line 44:

```c
    lv_sdl_window_create(480, 480);
```

with:

```c
    lv_display_t *disp = lv_sdl_window_create(480, 480);
    lv_display_add_event_cb(disp, inval_cb, LV_EVENT_INVALIDATE_AREA, NULL);  // TEMP
```

- [ ] **Step 3: Print a per-30-frame bucket in the loop**

In the `while (1)` loop, immediately after the `usleep(...)` line (after line 61), add:

```c
        // TEMP: report average invalidated px/frame every 30 frames.
        if (++g_bucket_frames >= 30) {
            printf("inval@frame%ld: %ld px / %ld frames = %ld px/frame\n",
                   frames, g_inval_px, g_bucket_frames,
                   g_inval_px / g_bucket_frames);
            g_inval_px = 0;
            g_bucket_frames = 0;
            fflush(stdout);
        }
```

- [ ] **Step 4: Build and run a full scene cycle**

```bash
export PATH="/c/msys64/mingw64/bin:$PATH"
cd /c/Users/Etai/Projects/yt-music-companion/.claude/worktrees/sleepy-pascal-e8e4ef/sim
cmake --build build
SIM_MAX_FRAMES=360 SDL_VIDEODRIVER=dummy ./build/ytm_sim.exe
```

Expected: a series of `inval@frameN: … px/frame` lines. Interpreting them:
- **Frames 0–60:** boot full-screen repaints — **ignore**.
- **Frames ~90–180 (PLAYING scene):** this is the breath's steady-state cost.
- **Frames ~240–330 (PAUSED scene):** must read **~0 px/frame** (one small blip at the play→pause transition as the halo settles is expected and fine).

- [ ] **Step 5: Record the readings**

Note the playing-scene px/frame and the paused-scene px/frame. Decision gate for Task 3:
- Playing steady-state **≤ ~30k px/frame** → on budget, skip Task 3's tuning.
- Paused steady-state **must be ~0** (ignore the single settle blip). If paused is persistently nonzero, the settle/`was_active` logic is wrong — fix before proceeding.

> Expected first reading: inner-two rings at decimate 3 is ~40k px/frame (inner bboxes ≈49k + 71k px, 10 updates / 30 frames). That is slightly over budget — Task 3 dials it down. (The counter sums overlapping concentric boxes, so it over-counts vs real render cost — a conservative ceiling.)

- [ ] **Step 6: Commit the instrumentation**

```bash
git add sim/main_sim.c
git commit -m "test(sim): temporary per-frame invalidation counter for ring breath tuning"
```

---

## Task 3: Tune to budget

**Files:**
- Modify: `ui/ring_visualizer.c` (the `BREATH_*` constants)

- [ ] **Step 1: If playing steady-state > ~30k px/frame, reduce cost**

Pick **one** lever and prefer the one that keeps the look best:
- Breathe only the innermost ring — set `#define BREATH_RINGS 1` (inner box ≈49k px → ~16k px/frame at decimate 3). Recommended: cheapest, still clearly alive.
- Or slow the update rate — set `#define BREATH_DECIMATE 5` (~6 updates/30 → inner-two ≈24k px/frame).

Apply the change in `ui/ring_visualizer.c`. Example (innermost only):

```c
#define BREATH_DECIMATE 3      // push an opacity update every Nth call
#define BREATH_RINGS    1      // inner-most N rings breathe (1..RING_COUNT)
```

- [ ] **Step 2: Rebuild and re-measure**

```bash
export PATH="/c/msys64/mingw64/bin:$PATH"
cd /c/Users/Etai/Projects/yt-music-companion/.claude/worktrees/sleepy-pascal-e8e4ef/sim
cmake --build build
SIM_MAX_FRAMES=360 SDL_VIDEODRIVER=dummy ./build/ytm_sim.exe
```

Expected: playing-scene bucket now **≤ ~30k px/frame**; paused-scene still **~0**.

- [ ] **Step 3: Repeat Step 1–2 until under budget**

If still over, combine levers (e.g. `BREATH_RINGS 1` + `BREATH_DECIMATE 4`). Stop as soon as a reading is comfortably under ~30k px/frame while the constants still leave a visible swing (amplitude `BREATH_AMPL 0.10` is unchanged).

- [ ] **Step 4: Commit the tuned constants (only if changed)**

```bash
git add ui/ring_visualizer.c
git commit -m "tune(ui): set ring breath to <=30k px/frame steady-state

Measured in the sim invalidation counter; keeps the breath under ~10% of
the old 365k px/frame regression while staying visibly alive."
```

---

## Task 4: Remove instrumentation, final builds, finalize

**Files:**
- Modify: `sim/main_sim.c` (revert all temporary measurement code)

- [ ] **Step 1: Revert the sim instrumentation**

Restore `sim/main_sim.c` to its committed-before-Task-2 state:

```bash
cd /c/Users/Etai/Projects/yt-music-companion/.claude/worktrees/sleepy-pascal-e8e4ef
git show HEAD~2:sim/main_sim.c > sim/main_sim.c   # HEAD~2 = pre-instrumentation; adjust if Task 3 added a commit
git diff --stat sim/main_sim.c
```

Verify by inspection that the three temporary blocks (counter globals + `inval_cb`, the `lv_display_add_event_cb` line, and the per-30-frame print) are gone and line 44 reads `lv_sdl_window_create(480, 480);` again.

> If unsure which commit is pre-instrumentation, `git log --oneline -5 -- sim/main_sim.c` and restore from the commit just before "temporary per-frame invalidation counter".

- [ ] **Step 2: Final sim build + headless run**

```bash
export PATH="/c/msys64/mingw64/bin:$PATH"
cd /c/Users/Etai/Projects/yt-music-companion/.claude/worktrees/sleepy-pascal-e8e4ef/sim
cmake --build build
SIM_MAX_FRAMES=150 SDL_VIDEODRIVER=dummy ./build/ytm_sim.exe
```

Expected: clean build, prints `headless: 150 frames rendered, exiting.`, exits 0, and **no more `inval@…` lines**.

- [ ] **Step 3: Firmware compile check (Xtensa)**

In PowerShell:

```powershell
. C:\Espressif\Initialize-Idf.ps1 -IdfId esp-idf-20ee62e792ea89630ac6a777ab3ebc57
idf.py -C C:\Users\Etai\Projects\yt-music-companion\.claude\worktrees\sleepy-pascal-e8e4ef\firmware build
```

Expected: `Project build complete`. (If the old `esp_lcd` ICE recurs — unrelated to these files — fall back to building just the changed objects per the firmware-build memory note.)

- [ ] **Step 4: Commit the revert**

```bash
git add sim/main_sim.c
git commit -m "test(sim): remove temporary ring-breath invalidation counter"
```

- [ ] **Step 5: Mark the spec done and refresh the perf memory**

- Update `docs/superpowers/specs/2026-06-30-ring-breath-animation-design.md` status line to `Implemented`.
- Update the `board-render-perf-model` memory: the rings are no longer purely static — note the throttled opacity breath while playing and its measured px/frame, so the next session doesn't "re-fix" it back to static.

```bash
git add docs/superpowers/specs/2026-06-30-ring-breath-animation-design.md
git commit -m "docs: mark ring breath spec implemented"
```

---

## Self-Review

**Spec coverage:**
- Subtle ambient life while playing → Task 1 (`ring_viz_breathe`, opacity sine).
- Only while playing; 0 px/frame otherwise → Task 1 Step 4 (`active = playing`) + settle-on-exit; verified Task 2 Step 4 (paused bucket ~0).
- Opacity only, never size → Task 1 Step 3 (border_opa only).
- Throttling lever → `BREATH_DECIMATE`; tuned in Task 3.
- Module ownership in ring_visualizer → Task 1 Steps 2–3.
- Call ahead of change-gate → Task 1 Step 4.
- Budget ≤30k px/frame playing, 0 otherwise → Tasks 2–3.
- Sim invalidation measurement method → Task 2.
- Firmware build passes → Task 4 Step 3.
- Out of scope (resize / ripple / level math / audio reactivity) → none reintroduced; old code already removed in `f72ef91`.

**Placeholder scan:** none — every code and command step is concrete.

**Type consistency:** `ring_viz_breathe(ring_viz_t *, bool)` and `ring_opa(int, float)` are used identically wherever they appear. `s_rings`, `RING_COUNT`, `STATIC_LEVEL`, `BREATH_*` all match their definitions.
