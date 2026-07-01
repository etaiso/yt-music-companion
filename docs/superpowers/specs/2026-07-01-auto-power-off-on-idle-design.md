# Auto power-off on idle — design

**Status:** Approved design, pre-implementation
**Date:** 2026-07-01
**Board:** Waveshare ESP32-S3-Touch-AMOLED-2.16 (CO5300 AMOLED, AXP2101 PMIC, QMI8658 IMU)

## Goal

After a long idle — no touch, no physical movement, nothing playing — **and while running
on battery**, hard power off the board via the AXP2101 PMIC to save power. Once off, the
board is fully powered down; only a **PWRON key press** turns it back on (a cold boot that
re-inits the panel and reconnects WiFi/bridge). This is a second, longer stage layered on
top of the existing idle clock that already drives screen dimming.

The feature is **local to the board** — it does not touch the bridge protocol. Its
decision logic runs in the desktop sim (with a logging stub for the actual power-off) so
the sim stays a usable dev loop; the AXP2101 power-cut is hardware-only.

## Background / current state

- The `idle` module (`firmware/main/idle.{c,h}`) already tracks a single idle measure and
  dims the screen. It is pure C (no ESP/LVGL/I²C), host-testable, and shared with the sim.
  See [auto screen-dim on idle design](2026-07-01-auto-screen-dim-on-idle-design.md).
  - `idle_cfg_t { uint32_t dim_after_ms; int dim_percent; void (*apply)(int); int (*get_active)(void); }`
  - `void idle_tick(uint32_t touch_inactive_ms, uint32_t now_ms, bool playing);`
  - Effective idle each tick = `min(touch_inactive_ms, now_ms - last_motion_ms)`.
  - **Playback resets the idle clock** (`playing == true` keeps `last_motion_ms` fresh and
    restores brightness), so nothing idle-driven happens while a song plays and there is a
    full-timeout grace period after playback stops.
  - Motion arrives via `idle_notify_activity()` (set by the IMU ISR path).
- `tick_cb()` in `firmware/main/main.c` (~33 ms / 30 fps) already calls `idle_tick(...)`
  under `#if CONFIG_YTM_IDLE_DIM_ENABLE`, passing `s_vm.playback == PB_PLAYING`. It also has
  the battery snapshot in hand (`s_vm.charging`, `s_vm.battery_present`).
- `imu.c` arms the QMI8658 wake-on-motion (INT on GPIO17) and calls `idle_notify_activity()`
  on motion. This serves the **dim** stage's wake; it is irrelevant to power-off (a fully
  powered-off board can't be woken by the IMU).
- `battery.c` owns the AXP2101 I²C device handle (`0x34`, shared BSP bus) and polls status
  every 10 s. `axp2101_decode.{c,h}` turns raw status registers into `battery_status_t`
  (`present`, `percent`, `charging`).
- There is **no existing power-off / deep-sleep logic** in the firmware.

## Non-goals

- **No deep sleep / light sleep.** "Off" means a genuine AXP2101 power cut, not an ESP
  sleep state. (Deep sleep was considered and rejected in favor of a true off.)
- **No auto-wake from the off state.** Wake is a physical PWRON press only — by design.
- No pre-off warning, countdown, or "powering off…" screen in v1 (the screen is already
  dimmed by this point). Easy to add later.
- No bridge protocol changes; no power state reported upstream.
- No standalone power-off without the dim feature — power-off reuses the idle-tracking and
  IMU setup that live under `YTM_IDLE_DIM_ENABLE` (see the coupling note below).
- No change to the dim stage's behavior.

## Configuration (Kconfig)

Two new options in `firmware/main/Kconfig.projbuild`, after the existing `YTM_IDLE_DIM_*`:

| Symbol | Type | Default | Meaning |
| --- | --- | --- | --- |
| `YTM_IDLE_POWEROFF_ENABLE` | bool | `y` | Enable auto power-off after long idle. `depends on YTM_IDLE_DIM_ENABLE`. When `n`, power-off compiles out; dimming (if enabled) is unaffected. |
| `YTM_IDLE_POWEROFF_MS` | int | `600000` | Idle time (ms) with no activity, not playing, on battery, before powering off. Range `60000`–`3600000`. |

`YTM_IDLE_POWEROFF_MS` depends on `YTM_IDLE_POWEROFF_ENABLE`. The default 600000 ms = 10 min.

**Coupling note:** `depends on YTM_IDLE_DIM_ENABLE` because the idle clock (touch + motion
tracking) and the IMU wake setup are compiled under that switch. `YTM_IDLE_POWEROFF_MS`
should be `> YTM_IDLE_DIM_MS` in practice (10 min ≫ 30 s), so the screen is already dimmed
well before power-off.

---

## Architecture

Data flow (power-off stage adds to the existing dim flow):

```
  touch  ──(lv_display_get_inactive_time)──┐
                                           ├─→ idle module ─┬─(idle≥dim, !playing)──────────────→ apply(dim%)
  IMU motion INT ──(idle_notify_activity)──┘                │
                                                            └─(idle≥poweroff, !playing,          → power_off()
  battery snapshot ──(external_power?)── power_off_allowed ────  power_off_allowed)                  = battery_power_off()
```

No new modules. Changes are confined to `idle`, `battery`/`axp2101_decode`, `main.c`, the
sim, and Kconfig.

### `idle` module — add the power-off threshold

`firmware/main/idle.{c,h}`. Stays pure and host-testable. Two additions to the config
struct and one new argument to the tick:

```c
typedef struct {
    uint32_t dim_after_ms;
    int      dim_percent;
    uint32_t power_off_after_ms;   // 0 = power-off disabled
    void   (*apply)(int percent);
    int    (*get_active)(void);
    bool   (*power_off)(void);     // issue power-off; return true if issued (then no retry)
} idle_cfg_t;

// power_off_allowed is a runtime gate (true only when on battery). It only affects the
// power-off stage; dim/restore are unchanged by it.
void idle_tick(uint32_t touch_inactive_ms, uint32_t now_ms,
               bool playing, bool power_off_allowed);
```

Decision each tick, after the existing dim/restore logic:

- If `playing` → existing behavior (reset clock, restore if dimmed, return). Never powers
  off while playing.
- Else, power-off stage: if `power_off_after_ms != 0 && power_off != NULL &&
  power_off_allowed && effective_idle >= power_off_after_ms` and not already fired →
  call `power_off()`.
- **Fire-once with retry-on-failure:** latch an internal `s_power_off_done` flag *only if*
  `power_off()` returns `true`. On real hardware a successful call cuts power within
  milliseconds, so the latch mostly guards the host/sim path. If the I²C write fails
  (`false`), the flag stays clear and the next tick retries — the device is never left
  stuck "half-off".
- The deferred case falls out naturally: if the threshold is crossed while
  `power_off_allowed == false` (charging), nothing fires and the flag stays clear; when the
  charger is later removed while still idle, the next tick powers off.

Internal state adds `bool s_power_off_done` (reset in `idle_init`).

### `battery` module — issue the AXP2101 power-off

`firmware/main/battery.{c,h}` (owns the AXP2101 `0x34` handle):

```c
// Soft power-off via AXP2101 reg 0x10 (PMU common config) bit 0. Read-modify-write so
// other config bits are preserved. Returns false on I²C failure (caller may retry).
bool battery_power_off(void);
```

Register define added alongside the existing AXP2101 regs (e.g. `AXP2101_REG_COMMON_CFG
0x10`, bit 0 = soft power-off, per XPowersLib `shutdown()`).

### `axp2101_decode` — report external power present

To gate "only on battery" correctly, we need *external power present*, not merely
"actively charging" (a full battery on USB reports not-charging). AXP2101 `STATUS1`
(`0x00`) bit 5 = VBUS good.

- Add `bool external_power` to `battery_status_t`.
- `axp2101_decode()` sets it from `STATUS1` bit 5.

### Wiring — `main.c`

- Under `#if CONFIG_YTM_IDLE_POWEROFF_ENABLE`, add an `idle_power_off()` callback:
  ```c
  static bool idle_power_off(void) {
      bsp_display_backlight_off();     // tidy; power drops immediately after anyway
      return battery_power_off();
  }
  ```
- Populate the idle config: `power_off_after_ms = CONFIG_YTM_IDLE_POWEROFF_MS` and
  `power_off = idle_power_off` when enabled; `0` / `NULL` when not.
- In `tick_cb()`, compute `power_off_allowed = !external_power` from the battery snapshot
  and pass it into `idle_tick(...)`. On the mock/non-net build, source the same flag from
  the mock battery fields (or pass a fixed value — decide during implementation to keep the
  mock demo sane).

### Sim (`sim/main_sim.c`)

- Update the `idle_cfg_t` initializer for the new fields and the `idle_tick(...)` call for
  the new argument.
- Provide a logging `power_off` stub (print "power off requested" — optionally exit the
  sim) so the stage is observable on desktop, and pass `power_off_allowed = true` (the sim
  has no charging concept) so it can be exercised.

---

## Error handling

- **AXP2101 write fails:** `battery_power_off()` returns `false`; `idle` does not latch and
  retries next tick. Device stays on and functional in the meantime.
- **Power-off disabled (`YTM_IDLE_POWEROFF_ENABLE=n`):** `power_off_after_ms = 0`,
  `power_off = NULL`; the stage compiles out / no-ops. Dimming unaffected.
- **Charging:** `power_off_allowed == false` → never powers off while on external power.
- **Playing:** never powers off (existing clock-reset behavior).

## Testing

- **Host unit tests** (`tests/test_idle.c`, extend): powers off after `power_off_after_ms`
  when idle, not playing, and allowed; does **not** power off before the threshold, while
  playing, or when `power_off_allowed == false`; deferred case (threshold crossed while not
  allowed, then allowed later → fires); fires exactly once when the callback returns `true`;
  retries when the callback returns `false`. Update the existing `idle_cfg_t` initializer
  and `idle_tick(...)` calls for the new field/arg.
- **Host unit test** (`tests/test_battery.c`, extend): `axp2101_decode` sets
  `external_power` from `STATUS1` bit 5; existing `present`/`charging`/`percent` cases still
  pass.
- **Sim:** with a short `power_off_after_ms` override, leave it idle with playback stopped
  and confirm the power-off stub fires; confirm activity before the threshold cancels it.
- **On-device:** confirm the AXP2101 soft power-off actually cuts the rails and that a
  PWRON press cold-boots the board; confirm it does **not** power off while charging or
  playing; confirm the Kconfig switch disables the feature. (Register bits `0x10`.0 and
  `STATUS1`.5 follow XPowersLib/datasheet but warrant this hardware confirmation, like the
  other AXP/IMU tuning in this repo.)
