# YT Music Companion — firmware (Now Playing slice)

The **Now Playing** screen for the Waveshare ESP32-S3-Touch-AMOLED-2.16, built to the
**V2 dark** design (`docs/SPEC-ytmusic-now-playing.md` /
`design/now-playing-screen-design/project/Now Playing v2.dc.html` +
`NowPlayingDeviceV2.dc.html`). The render layer reads only `now_playing_vm_t` — fed by
`mock.c` in the simulator or the live Mac bridge on device (see `main/net_backend.c`) —
and controls emit intent through `emit()`.

Faithful to the design: 3 concentric rings (geometry/alpha/width and the peak ripple
transcribed from the design's canvas `draw()`), tinted from an album-derived palette
(`ui/palette.c`); a pulsing red status dot; a 172px hero cover, with a neutral
`music_note` block for ad/idle; buffering shimmer bars; a white knobless seek bar; the
5-button transport (dislike · prev · play/pause · next · like); and a glassy offline
banner (V1's 50% dim scrim is gone). Theme is Dark by default — Light is the same
layout painted with light tokens and the album glow disabled.

## Layout

```
ui/                      portable LVGL v9 UI — shared by device + simulator
  now_playing_vm.h       the board<->bridge contract (view-model)
  styles.h               design tokens (Dark/Light, build-time theme; default Dark)
  palette.{h,c}          pure album-art -> 3-stop palette (host-unit-tested)
  ring_visualizer.{h,c}  signature concentric audio rings (level-reactive)
  now_playing_screen.{h,c}  the screen; reads ONLY the view-model
  mock.{h,c}             fake feed cycling all six states
firmware/                ESP-IDF v5.5 project (this folder)
  main/main.c            BSP bring-up + mock tick loop + emit() stub
  main/idf_component.yml  depends on waveshare/esp32_s3_touch_amoled_2_16
  sdkconfig.defaults     octal PSRAM, 16MB flash, big-APP, LVGL fonts
  partitions.csv         big-APP partition scheme
sim/                     desktop SDL preview (see sim/README.md)
```

## Build & flash (device)

```bash
cd firmware
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/tty.usbmodem* flash monitor
```

The component manager pulls the Waveshare BSP (`waveshare/esp32_s3_touch_amoled_2_16`),
which configures the CO5300 QSPI panel, CST9220 touch, and LVGL v9 for this exact
board. Serial log shows each control press (`emit: toggle_play (0)` etc.).

### BSP header note

`main.c` includes `bsp/esp-bsp.h` and calls `bsp_display_start()` /
`bsp_display_lock()` / `bsp_display_backlight_on()` (the esp-bsp convention). If the
Waveshare component exposes a different header or init entry point, adjust the include
and the three calls in `main.c` — that is the only board-specific surface.

## Definition of done (SPEC §8)

- [x] ESP-IDF project builds against the vendor BSP; renders at 480×480.
- [x] All six states reachable via mock (playing, paused, buffering, no-track, ad,
      disconnected) — `mock.c` rotates them every 6 s.
- [x] Ring visualizer animates from `vm.level`; reduce-motion pulse fallback in
      `ring_visualizer.c`.
- [x] Design tokens centralized in `styles.h`; no ad-hoc colors elsewhere.
- [x] Controls call `emit(...)` (logged over serial).
- [x] Clean separation: render reads only `now_playing_vm_t`; no network code.

## Fonts

The UI renders in bundled **Inter** at the V2 type scale (SPEC §5 /
`NowPlayingDeviceV2.dc.html`): ExtraBold 800 @29 (title) and @12 (uppercase
status row), SemiBold 600 @17 (artist) / @13 (album) / @12 (elapsed-total time),
and Regular @12 (LVGL default / body). The `.c` fonts in `ui/inter_*.c` are
generated from the Inter variable TTF by
`scripts/gen_fonts.sh` (deps: `npm i -g lv_font_conv`, `pip install fonttools`),
and `styles.h` points the `FONT_*` macros at them. There is no Montserrat —
LVGL's internal default is the tiny built-in `unscii_8`, which the screen never
renders. Regenerate fonts with `./scripts/gen_fonts.sh`.

## Icons

Glyphs come from bundled **Material Symbols** subsets — `ui/mdi_solid.c` (filled:
transport, filled `thumb_up`, `music_note`) and `ui/mdi_line.c` (outline thumbs for the
unfavorited/dislike state) — matching the design exactly rather than approximating with
built-in symbols. `styles.h` points the `FONT_ICONS` macro at them. Regenerate the icon
fonts with `./scripts/gen_icons.sh` (companion to `gen_fonts.sh`).

## Auto screen-dim on idle

When nothing is playing and the screen receives no touch or physical motion for
`CONFIG_YTM_IDLE_DIM_MS` (default 30000 ms), the AMOLED dims to
`CONFIG_YTM_IDLE_DIM_PERCENT` (default 10%). Touch or motion detected by the QMI8658
IMU (via the motion-interrupt line on GPIO17) restores the user's brightness instantly.
While a song is playing, the screen never dims.

Dimming is transient and does not persist to NVS — the dimmed level never becomes boot
brightness. Restore always returns to the user's current brightness (e.g., as set via
the brightness slider in the quick panel).

### Configuration

Three Kconfig options under **YT Music board**:

- `YTM_IDLE_DIM_ENABLE` (default y) — set to n to disable the feature entirely; screen
  behaves as before.
- `YTM_IDLE_DIM_MS` (default 30000) — idle timeout in milliseconds before dimming.
- `YTM_IDLE_DIM_PERCENT` (default 10) — brightness target as a percentage (1–100).

### Simulator

The simulator exercises the TOUCH wake path only (no IMU on desktop). In headless mode,
the mock cycles playback states and loop iterations are not 1:1 with UI ticks; to
observe a dim→restore cycle, use a high frame cap:

```bash
SIM_MAX_FRAMES=15000 SDL_VIDEODRIVER=dummy ./build/ytm_sim.exe
```

### Hardware bring-up

The QMI8658 motion-interrupt tuning (I2C address 0x6B/0x6A, WoM threshold, INT edge
NEGEDGE/POSEDGE) is datasheet-configurable and flagged "tune on-device" in
`firmware/main/imu.c`. If the IMU probe fails at boot, the feature degrades gracefully
to touch-only wake, logged as a warning.

## Not in this slice (next tasks)

Pending: remaining screens and real audio-energy `level` from the bridge (the rings
currently fall back to a synthesized breathing pulse when the feed reports no energy).
