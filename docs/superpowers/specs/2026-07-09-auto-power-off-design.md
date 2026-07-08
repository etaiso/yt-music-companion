# Auto power-off on idle — design

**Date:** 2026-07-09
**Status:** approved (design), pending implementation plan
**Branch:** `etais/device-auto-poweroff-cable-258ace`

## Summary

Add a second idle stage after the existing screen-dim: when the device stays
idle long enough it performs a true AXP2101 soft power-off. Idle powers off
regardless of power source or bridge connection; the cable case just gets a
longer timeout than battery. Waking is by the physical PWRON button only.

## Definitions

- **Idle** = *not playing* **and** no touch **and** no motion. This is the same
  clock the auto-dim feature already computes in `idle.c`
  (`min(touch_inactive_ms, motion_idle)`, reset by playback). Power-off is a
  second, longer stage on that clock.
- **External power** = cable attached (VBUS present), read from AXP2101
  STATUS1 bit 5.
- **Power-off** = AXP2101 soft power-off (reg 0x10). SoC is unpowered; only a
  PWRON button press (or possibly VBUS insertion — see risk) brings it back.
  IMU/touch cannot wake a true power-off.

## Requirements

1. When idle long enough, soft power-off the board.
2. Idle power-off fires regardless of power source or bridge state.
3. Timeout depends on power source:
   - **Battery:** 5 min (default `YTM_IDLE_OFF_BATTERY_MS = 300000`).
   - **Cable:** 15 min (default `YTM_IDLE_OFF_CABLE_MS = 900000`).
   Both Kconfig-tunable; both must exceed the 30 s dim timeout.
4. Screen still dims at 30 s first (unchanged).
5. "Bridge down" needs no dedicated logic: no bridge → no playback updates →
   `playing` is false → the idle clock runs → power-off at the cable timeout.
6. Wake is the physical PWRON button only.
7. Whole feature is Kconfig-gated, like auto-dim.

## Architecture

Three units, clear boundaries:

- **`idle` module (extended)** — owns the idle clock; escalates dim → power-off.
  Pure C, host-testable. The power-off action is injected as a callback, so the
  decision logic never touches I2C or ESP APIs (same pattern as the dim `apply`
  callback today).
- **`battery` module (extended)** — owns the AXP2101 I2C device handle; decodes
  external power and exposes `battery_power_off()`.
- **`main.c`** — wiring: feeds `external_power` into the idle tick and supplies
  the `power_off` callback.

### `idle` changes (`idle.h` / `idle.c`)

`idle_cfg_t` gains:

```c
uint32_t off_after_battery_ms;   // default 300000
uint32_t off_after_cable_ms;     // default 900000
void   (*power_off)(void);
```

`idle_tick` gains one argument:

```c
void idle_tick(uint32_t touch_inactive_ms, uint32_t now_ms,
               bool playing, bool external_power);
```

Logic, after the existing dim stage:

- Threshold = `external_power ? off_after_cable_ms : off_after_battery_ms`.
- If `!playing` and `idle_ms >= threshold`, call `power_off()` **once**, guarded
  by a static `s_powered_off` latch (assert exactly-once in tests; never
  re-fire if a tick runs before the SoC actually drops).
- `playing` and touch/motion reset the clock exactly as today, so power-off
  cannot trigger during playback or interaction.

### `battery` changes (`axp2101_decode.c` / `.h` / `battery.c`)

- Decode **external power = STATUS1 bit 5** → new `bool external;` on
  `battery_status_t`. *(Exact bit to be confirmed against the AXP2101 datasheet
  during implementation.)*
- `void battery_power_off(void)` — writes the AXP2101 soft-off bit in reg 0x10.
  Reuses the existing `s_dev`; the I2C master driver serializes against the
  10 s poll task. *(Exact reg 0x10 bit to be confirmed during implementation.)*

### `main.c` wiring + Kconfig

- Copy `b.external` into the view-model; pass it to `idle_tick`.
- New `idle_power_off()` callback → `battery_power_off()`.
- New Kconfig under the existing idle menu:
  - `YTM_IDLE_POWEROFF_ENABLE` (bool, default y, `depends on YTM_IDLE_DIM_ENABLE`).
  - `YTM_IDLE_OFF_BATTERY_MS` (int, default 300000).
  - `YTM_IDLE_OFF_CABLE_MS` (int, default 900000).
  - Ranges set so both timeouts must exceed the dim timeout.
- Power-off is gated under `YTM_IDLE_DIM_ENABLE` because it is the second stage
  of the same idle clock. Can be decoupled later if an independent power-off
  (no dim) is ever wanted.

## Error handling / edge cases

- External-power read not yet available shortly after boot → default to the
  **battery** (shorter) timeout; corrected within one 10 s poll.
- `battery_power_off()` I2C write failure → log and retry on the next tick; the
  device stays on rather than wedging.
- Latch ensures the callback fires exactly once.

## Hardware risk — VBUS re-wake (verification gate)

Soft power-off **while a cable is attached** may cause the AXP2101 to power back
on immediately (PMICs often auto-power-on when VBUS is present), producing a
boot-loop instead of a clean "off."

- This is the one unconfirmed behavior in the design.
- **Verify early on hardware.** If it re-wakes, mitigate by clearing the
  "power-on-from-VBUS" source bit in the AXP2101 before issuing soft-off.
- The cable power-off path is not trusted until this check passes.

## Testing

- **Host** (extend the idle decision test + `tests/test_battery.c`):
  - battery + idle past 5 min → `power_off` called.
  - cable + idle below 15 min → not called.
  - cable + idle past 15 min → called.
  - playing → never called (regardless of source/idle).
  - touch/motion resets the clock → delays power-off.
  - `power_off` fires exactly once (latch).
  - `axp2101_decode` sets `external` from STATUS1 bit 5.
- **On-device:**
  - Confirm the board actually powers off at the timeouts.
  - Confirm PWRON wakes it.
  - VBUS re-wake check on cable (see risk).

## Out of scope

- Motion/touch wake from power-off (impossible with a true AXP2101 off).
- Deep-sleep alternative (explicitly rejected in favor of true power-off).
- Any separate "no bridge" timer (subsumed by the cable idle timeout).
