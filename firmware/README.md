# YT Music Companion — firmware (Now Playing slice)

First vertical slice from `docs/SPEC-ytmusic-now-playing.md`, recreated to match the
imported Claude Design (`design/now-playing-screen-design/project/Now Playing.dc.html`
+ `NowPlayingDevice.dc.html`): the **Now Playing** screen for the Waveshare
ESP32-S3-Touch-AMOLED-2.16, rendering from **mock data**. No network code yet — the
render layer reads only `now_playing_vm_t`; controls log intent through an `emit()`
stub.

Faithful to the design: 3 concentric gradient rings (geometry/alpha/width and the
peak ripple transcribed from the design's canvas `draw()`), a pulsing status dot,
128px cover with striped placeholder + "cover" caption, gradient cover block for
ad/idle, buffering shimmer bars, gradient seek bar, the 5-button transport
(dislike · prev · play/pause · next · like), and the dim scrim + offline banner.

## Layout

```
ui/                      portable LVGL v9 UI — shared by device + simulator
  now_playing_vm.h       the board<->bridge contract (view-model)
  styles.h               design tokens (cream theme); single source of truth
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

The slice uses LVGL's built-in **Montserrat** (enabled in `sdkconfig.defaults`) so it
builds with no extra assets. Production swaps in bundled **Inter ExtraBold/800**
(SPEC §5): convert Inter with `lv_font_conv`, add the `.c`, and point the `FONT_*`
macros in `styles.h` at it. `styles.h` is the only file to touch.

## Icons

Transport uses LVGL's built-in symbols, which map 1:1 to the design's
`skip_previous` / `skip_next` / `pause` / `play_arrow`. The design's **thumb_up /
thumb_down** (like / dislike) have no built-in equivalent, so the slice uses up/down
arrows as stand-ins (correct colors + behavior). To match the design exactly, bundle
a Material Symbols Rounded subset with just `thumb_up`/`thumb_down`, e.g.

```bash
lv_font_conv --font MaterialSymbolsRounded.ttf --range 0xe8dc,0xe8db \
  --size 26 --format lvgl --bpp 4 -o material_thumbs.c
```

then point a `FONT_ICONS` macro in `styles.h` at it and swap the two `LV_SYMBOL_UP/DOWN`
labels in `now_playing_screen.c`.

## Not in this slice (next tasks)

Mac-side ytmdesktop bridge + protocol wiring (`docs/SPEC-ytmusic-adapter.md`),
remaining screens, Wi-Fi/host discovery, real audio-energy `level`.
