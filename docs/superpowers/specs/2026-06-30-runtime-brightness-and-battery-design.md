# Runtime brightness control + battery level — design

**Status:** Approved design, pre-implementation
**Date:** 2026-06-30
**Board:** Waveshare ESP32-S3-Touch-AMOLED-2.16 (square 480×480, CO5300 AMOLED, AXP2101 PMIC)

## Goal

Add two on-device features to the YT Music Companion board:

1. **Runtime brightness control** via an on-device swipe-down quick-settings panel, with
   the chosen level persisted across reboots.
2. **Battery level indicator** in the status bar, sourced from the board's AXP2101 power
   IC, with a charging indication and a low-battery color cue.

Both are **local to the board** — neither touches the bridge protocol. They run in the
desktop sim with stubs so the sim stays the primary dev loop.

## Background / current state

- Brightness today is **build-time only**: `CONFIG_YTM_DISPLAY_BRIGHTNESS` (Kconfig, 5–100%,
  default 40) is applied once at boot in `firmware/main/main.c:52` via
  `bsp_display_brightness_set()`, which sends the CO5300 panel's `0x51` DCS command (the
  AMOLED has no PWM backlight). `bsp_display_brightness_set()` / `_get()` are already
  callable at runtime — they are simply never called again after boot.
- The board **has battery hardware**: an AXP2101 PMIC on the shared I²C bus
  (addr `0x34`, GPIO14 SCL / GPIO15 SDA) and an MX1.25 LiPo connector. The in-repo vendor
  BSP does not currently read it. The user confirmed a battery is connected.
- The render layer reads only `now_playing_vm_t` (`ui/now_playing_vm.h`) and never calls a
  backend directly. Controls reach the backend through an `emit_cb_t` callback
  (`ui/now_playing_screen.h`). The per-frame tick (`tick_cb` in `main.c`) fills the VM from
  either `net_backend_get_vm()` (live) or `mock_tick()` (mock/sim), then calls
  `now_playing_update()`.

## Non-goals

- No bridge protocol changes. Battery and brightness are board-local.
- No general settings screen / navigation framework — only the single swipe-down panel.
- No auto-brightness, no ambient light sensing.
- No battery telemetry sent back to the bridge.

---

## Feature 1 — Battery level indicator

### Data pipeline

- **New module `firmware/main/battery.{c,h}`** — an AXP2101 driver plus a FreeRTOS task
  that polls every ~10 s and caches the latest reading behind a getter:

  ```c
  typedef struct {
      bool present;       // battery detected on the MX1.25 connector
      int  percent;       // 0..100 state of charge
      bool charging;      // USB present and charging
  } battery_status_t;

  void battery_start(void);                 // init driver + spawn poll task
  void battery_get(battery_status_t *out);  // latest cached snapshot (lock-protected)
  ```

- **I²C bus reuse.** The touch controller already shares the I²C bus, so the BSP brings up
  an I²C master. The driver reuses that handle (e.g. `bsp_i2c_get_handle()`) rather than
  initializing its own. **Implementation must confirm the exact BSP accessor** against the
  managed BSP component (not checked into the repo; pulled at build time).

- **Register decode is a pure function** (`axp2101_decode_soc()` / charge-status decode) so
  it is host-testable without hardware.

### View-model fields

Add to `now_playing_vm_t` (`ui/now_playing_vm.h`):

```c
bool battery_present;   // false => hide the indicator entirely (USB-only / sim default off)
int  battery_percent;   // 0..100
bool charging;          // show charging bolt
```

Battery is source-agnostic device status and fits the VM's charter. `tick_cb` fills these
from `battery_get()` (firmware) or mock values (`mock.c`, sim). `now_playing_update()`
renders them. The bridge is not involved.

### UI

- Battery widget added to the existing status bar — the right-hand `st` flex group in
  `ui/now_playing_screen.c` (~line 162), beside the connection dot.
- **Icon + `%` text** (the user wants the level visible).
- **Charging bolt** overlaid when `charging`.
- **Color:** normal ink at/above threshold; **amber/red below 20%** (threshold a named
  constant). No popup, no toast.
- If `!battery_present`, the whole widget is hidden (keeps sim/USB-only clean).

---

## Feature 2 — Runtime brightness control

### Control seam

The shared `ui/` code cannot call `bsp_*`. Extend the existing callback pattern in
`ui/now_playing_screen.h` (or the new `quick_panel.h`):

```c
typedef void (*brightness_cb_t)(int percent);
void now_playing_set_brightness_sink(brightness_cb_t cb, int initial_percent);
```

- The panel's slider initializes to `initial_percent` and calls `cb(percent)` on change.
- **Firmware sink:** calls `bsp_display_brightness_set(percent)` immediately, and schedules
  a **debounced NVS write** (~500 ms after the last change) so dragging does not hammer
  flash.
- **Sim sink:** stub — dims the SDL window or logs.

### Persistence

- At boot, `main.c` reads the saved brightness from **NVS**; on first boot (key absent) it
  falls back to `CONFIG_YTM_DISPLAY_BRIGHTNESS`. It applies the value via
  `bsp_display_brightness_set()` and passes it as `initial_percent` to the sink.
- `CONFIG_YTM_DISPLAY_BRIGHTNESS` therefore becomes the **first-boot default only**; after
  that, NVS wins. Document this one-line behavior change in `docs/CONFIGURATION.md`.

### UI — swipe-down quick panel

- **New focused module `ui/quick_panel.{c,h}`** (keeps it out of the already-large
  `now_playing_screen.c`).
- Built on `lv_layer_top()` so it floats above Now Playing.
- **Open:** a downward swipe gesture (`LV_EVENT_GESTURE` → `LV_DIR_BOTTOM`) on the screen.
- **Close:** tap the dimmed backdrop, or swipe up.
- **Contents:** drag handle; ☀ icon + brightness slider (live %, calls the sink); a small
  battery echo (percent + charging) below it.
- Edge-restricting the open gesture to the top of the screen is a refinement, not required
  for v1.

---

## Sim behavior

Both features compile and run in the sim with stubs:

- Battery: `mock.c` provides a plausible value (e.g. a slowly-draining 64%, `present=true`)
  so the indicator and its states are visible.
- Brightness: a no-op / window-dimming sink.

This keeps the sim usable for verifying the gesture, slider, panel open/close, and battery
indicator states without hardware.

## Testing

- **Host (pure functions):** AXP2101 register → SOC / charge-status decode, via the existing
  host-C test path.
- **Sim (manual):** swipe opens the panel; slider drag updates the live %; tap/​swipe-up
  closes; battery indicator renders correctly across normal / low (<20%) / charging /
  absent states.
- **Board (manual):** real charge % matches a meter; charging bolt appears on USB; brightness
  change persists across a reboot (NVS).

## Affected files

| File | Change |
|------|--------|
| `firmware/main/battery.{c,h}` | **new** — AXP2101 driver + poll task + getter |
| `firmware/main/main.c` | start battery task; fill VM battery fields in tick; NVS brightness read at boot + register firmware brightness sink |
| `firmware/main/CMakeLists.txt` | add `battery.c` |
| `ui/now_playing_vm.h` | add `battery_present` / `battery_percent` / `charging` |
| `ui/now_playing_screen.{c,h}` | battery widget in status bar; `now_playing_set_brightness_sink()` declaration; render battery fields |
| `ui/quick_panel.{c,h}` | **new** — swipe-down panel (gesture, slider, battery echo) |
| `sim/` (mock + sim main) | mock battery values; stub brightness sink |
| `docs/CONFIGURATION.md` | note that `YTM_DISPLAY_BRIGHTNESS` is now the first-boot default; NVS persists runtime changes |

## Open implementation details (resolve during build, not blocking)

- Exact BSP I²C handle accessor for the shared bus.
- AXP2101 SOC source: gauge register vs. voltage-curve estimate (prefer the fuel-gauge SOC
  register if populated).
- NVS namespace/key naming and debounce timer mechanism.
