# Auto screen-dim on idle — design

**Status:** Approved design, pre-implementation
**Date:** 2026-07-01
**Board:** Waveshare ESP32-S3-Touch-AMOLED-2.16 (CO5300 AMOLED, AXP2101 PMIC, QMI8658 IMU)

## Goal

When the device has been idle — no touch and no physical movement — **and no song is
playing**, dim the screen to a low brightness to save power and reduce glare. Any
interaction (touch or physical movement) instantly restores the user's brightness.

The feature is **local to the board** — it does not touch the bridge protocol. Its core
logic runs in the desktop sim (touch-only) so the sim stays the primary dev loop; the
motion source is hardware-only and cleanly isolated.

## Background / current state

- Brightness is applied at runtime via `fw_brightness()` in `firmware/main/main.c`, which
  calls `bsp_display_brightness_set()` (CO5300 DCS `0x51`; the AMOLED has no PWM backlight).
  The active level is user-set from the swipe-down quick panel and persisted to NVS.
  See [runtime brightness + battery design](2026-06-30-runtime-brightness-and-battery-design.md).
- The per-frame tick `tick_cb()` (`main.c`, ~33 ms / 30 fps) fills a `now_playing_vm_t`
  snapshot (`ui/now_playing_vm.h`) from the net backend or the mock, then updates the UI.
  This is the natural place to run the idle check — playback state is already in hand there.
- Touch is handled by the BSP-managed LVGL input device (CST9217). LVGL already tracks
  time since the last touch via `lv_display_get_inactive_time()` — no manual event hooks
  are needed for the touch activity signal.
- The board has a **QMI8658 6-axis IMU** (accel + gyro) on the shared I²C bus, with a
  motion interrupt line on **GPIO17**. It is **not currently used in firmware**. The chip
  supports a hardware **any-motion interrupt** that asserts the INT pin only when the
  device is moved. See the [board spec](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-2.16)
  (QMI8658A/C, INT on GPIO17/21).
- There is **no existing idle / sleep / auto-dim logic** in the firmware.

## Non-goals

- **No two-stage dim and no full screen-off.** One stage only: dim to a low brightness and
  hold. (Deliberately deferred; the module is structured so a second "off" stage could be
  added later.)
- No physical-button wake source (the GPIO18 button feature was dropped).
- No bridge protocol changes; no idle state reported upstream.
- No ambient-light sensing / auto-brightness.
- No IMU features beyond motion-wake (no gestures, orientation, step counting, etc.).
- No smooth brightness fade in v1 — the dim/restore is applied instantly. (A fade is a
  possible later nicety and is not required.)

## Configuration (Kconfig)

Three options, under a menu in `firmware/main/Kconfig.projbuild`:

| Symbol | Type | Default | Meaning |
| --- | --- | --- | --- |
| `YTM_IDLE_DIM_ENABLE` | bool | `y` | Master switch. When `n`, the whole feature compiles out and the screen behaves exactly as today. |
| `YTM_IDLE_DIM_MS` | int | `30000` | Idle time (ms) with no activity before dimming. |
| `YTM_IDLE_DIM_PERCENT` | int | `10` | Target brightness (%) when dimmed. Matches the manual slider floor. |

`YTM_IDLE_DIM_MS` and `YTM_IDLE_DIM_PERCENT` depend on `YTM_IDLE_DIM_ENABLE`.

---

## Architecture

Data flow:

```
  touch  ──(lv_display_get_inactive_time)──┐
                                           ├─→ idle module ──(gated by "playing")──→ fw_brightness()
  IMU motion INT ──(idle_notify_activity)──┘
```

Two new modules plus wiring in `tick_cb()`.

### Module 1 — `idle` (the brain), sim-buildable

`firmware/main/idle.{c,h}` — pure logic, no hardware or LVGL dependencies. It receives
activity signals, playback state, and elapsed touch-inactivity, and decides whether to dim
or restore. Because it takes everything by argument/callback, it is unit-testable on the
host toolchain and runs unchanged in the sim.

```c
// Called once at startup. `apply` sets panel brightness (0..100); `get_active`
// returns the user's current (non-dimmed) brightness so restore is always correct.
void idle_init(void (*apply)(int percent), int (*get_active)(void));

// Activity from a non-touch source (IMU motion). Resets the idle clock.
void idle_notify_activity(void);

// Current playback state; when true the idle clock is disabled and, if dimmed,
// brightness is restored.
void idle_set_playing(bool playing);

// Call every tick with ms since the last touch (lv_display_get_inactive_time()).
// Combines touch inactivity with the internal motion clock, compares against
// YTM_IDLE_DIM_MS, and applies dim/restore via the `apply` callback.
void idle_tick(uint32_t touch_inactive_ms);
```

Internal state: `dimmed` (bool), `last_motion_ms` (monotonic), `playing` (bool). Effective
idle = `min(touch_inactive_ms, now - last_motion_ms)`.

Decision each tick:
- If `!YTM_IDLE_DIM_ENABLE` → no-op (compiled out).
- If `playing` → if `dimmed`, restore and clear `dimmed`; else nothing. (Screen stays lit
  during playback, including when a song *starts* while dimmed.)
- Else (not playing) and `!dimmed`: if `effective_idle >= YTM_IDLE_DIM_MS` → apply
  `YTM_IDLE_DIM_PERCENT` and set `dimmed`. (No need to capture the current level — restore
  reads it live via `get_active()`.)
- If activity arrives while `dimmed` (touch inactivity resets to ~0, or `idle_notify_activity`)
  → restore via `get_active()`, clear `dimmed`.

Restore always uses `get_active()` (the current user-set brightness), so a quick-panel
slider change is respected and there is no stale captured value.

### Module 2 — `imu` (motion source), firmware-only

`firmware/main/imu.{c,h}` — QMI8658 driver. Guarded by `#if CONFIG_YTM_IDLE_DIM_ENABLE`
and excluded from the sim build (the sim has no IMU).

- `imu_start()`: probe the QMI8658 on the shared BSP I²C bus, configure the any-motion
  (wake-on-motion) interrupt with a sensible threshold, and install a GPIO17 ISR.
- On the motion interrupt, signal the main context (ISR-safe: set a flag / give a
  semaphore) which calls `idle_notify_activity()`. The ISR does no I²C work itself.

The threshold is chosen so incidental table vibration does not wake the screen but picking
the device up does. Threshold constant lives in `imu.c` (tunable; not exposed as Kconfig in
v1).

### Wiring — `tick_cb()` in `main.c`

Per tick, when the feature is enabled:

```c
idle_set_playing(s_vm.playing);                       // playback gate
idle_tick(lv_display_get_inactive_time(NULL));        // touch inactivity + decide
```

`idle_init(fw_brightness, fw_brightness_get)` is called at startup (a small getter for the
current active brightness is added alongside the existing `fw_brightness()` setter).
`imu_start()` is called at startup under the same `#if`. Motion interrupts feed
`idle_notify_activity()` asynchronously between ticks.

`s_vm.playing` is the existing playback flag in `now_playing_vm_t`; confirm the exact field
name during implementation and use it (do not add a new one).

---

## Error handling

- **IMU absent / probe fails:** `imu_start()` logs a warning and returns without installing
  the ISR. The feature degrades gracefully to **touch-only** wake — dimming still works,
  motion just won't wake it. It never blocks boot.
- **Brightness apply:** reuses the existing `fw_brightness()` path; no new failure modes.
- **Feature disabled (`YTM_IDLE_DIM_ENABLE=n`):** all new calls compile out; zero runtime
  cost and no behavior change.

## Testing

- **Host unit tests** for the `idle` module (pure logic): dims after the configured idle
  with no playback; does *not* dim while playing; restores on touch activity; restores on
  `idle_notify_activity`; restores (and stays lit) when playback starts while dimmed;
  restore uses the live active brightness, not a stale value; no-op when disabled.
- **Sim:** exercise the touch path end-to-end — leave the sim idle with playback stopped,
  confirm it dims after the timeout, confirm a click restores. (IMU is stubbed/absent in
  the sim.)
- **On-device:** verify motion wake (pick the device up while dimmed → restores) and that
  incidental vibration does not; verify playback keeps the screen lit; verify the Kconfig
  master switch fully disables the feature.
