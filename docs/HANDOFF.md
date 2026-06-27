# Handoff — YT Music Companion (resume point)

_Last updated: 2026-06-27_

Cold-start summary so a fresh session (or you, later) can pick up without re-reading
the whole history. Pairs with `SPEC-ytmusic-now-playing.md` (the screen),
`SPEC-ytmusic-adapter.md` (the Mac bridge), and `PROJECT-OVERVIEW.md`.

---

## Task checklist (snapshot of the session task list)

- [x] 1. Scaffold ESP-IDF project structure
- [x] 2. Write shared portable UI layer
- [x] 3. Wire ESP-IDF main entry
- [x] 4. Add SDL desktop simulator target
- [x] 5. Verify simulator builds and renders
- [x] 6. Obtain Material Symbols font + lv_font_conv
- [x] 7. Generate LVGL icon font C files
- [x] 8. Wire icon font into the UI
- [x] 9. Verify build with icon font

Items 6–9 (Material Symbols icon font) are **done** — see "DONE — Material
Symbols icon font" below for what landed.

## Where we are

**Deliverable #1 (Now Playing vertical slice, mock data) is built** and recreated to
match the imported Claude Design bundle in
`design/now-playing-screen-design/project/` (`Now Playing.dc.html` +
`NowPlayingDevice.dc.html`).

The shared UI compiles clean against **LVGL v9.2.2** under `-Wall` (header-level
syntax check). The full SDL simulator link runs on the Mac (this dev sandbox has no
root to install SDL2).

### File map (all under the project root)

```
ui/                       portable LVGL v9 UI — shared by device + simulator
  now_playing_vm.h        view-model (board<->bridge contract)
  styles.h                design tokens (cream theme); single source of truth
  ring_visualizer.{h,c}   3 gradient rings + ripple (geometry from design canvas)
  now_playing_screen.{h,c}  the screen; reads ONLY the view-model
  mock.{h,c}              mock feed cycling all six states (6s each)
firmware/                 ESP-IDF v5.5 project
  main/main.c             BSP bring-up + mock tick + emit() serial stub
  main/idf_component.yml  dep: waveshare/esp32_s3_touch_amoled_2_16 ^1.0.0
  main/CMakeLists.txt     compiles main.c + ../../ui/*.c
  sdkconfig.defaults      octal PSRAM, 16MB, big-APP, Montserrat fonts
  partitions.csv          big-APP scheme
  README.md               build/flash + DoD checklist + Icons note
sim/                      desktop SDL preview (same ui/ code)
  CMakeLists.txt          FetchContent lvgl v9.2.2 + SDL2 (uses SDL2::SDL2 target)
  lv_conf.h               sim-only LVGL config
  main_sim.c              window + mock tick loop
  README.md               brew + cmake build steps
design/now-playing-screen-design/   the imported Claude Design source (read-only ref)
docs/                     specs + this handoff
```

### Verify the slice (on the Mac)

```bash
brew install cmake sdl2
cd sim && cmake -B build && cmake --build build && ./build/ytm_sim
# headless: SIM_MAX_FRAMES=120 SDL_VIDEODRIVER=dummy ./build/ytm_sim
```

Window cycles all six states; clicking transport prints `emit: <cmd> (<arg>)`.

---

## DONE — Material Symbols icon font

**Goal (met):** all six transport glyphs now render as real Material Symbols
Rounded, matching the design. like/dislike are real `thumb_up`/`thumb_down`.

**What landed:**
- `assets/MaterialSymbolsRounded.ttf` — the variable source TTF (14.4 MB).
- `scripts/gen_icons.sh` — instances the variable font (fonttools) into FILL 1 /
  FILL 0 and runs `lv_font_conv`, regenerating the two C fonts. Idempotent.
- `ui/mdi_solid.c` (FILL 1 @30px): skip_previous, skip_next, play_arrow, pause,
  thumb_up. `ui/mdi_line.c` (FILL 0 @26px): thumb_up, thumb_down.
- `ui/styles.h` — `LV_FONT_DECLARE` for both, `FONT_ICONS`/`FONT_ICONS_LINE`, and
  `IC_*` UTF-8 glyph macros.
- `ui/now_playing_screen.c` — `icon_btn(...)` takes a `const lv_font_t *font`;
  transport uses `FONT_ICONS`; dislike uses outline; **like toggles solid+pink
  (favorited) vs outline+grey** in the update path. `NOTE:` stand-in comment gone.
- Both `CMakeLists.txt` (firmware + sim) compile `mdi_solid.c` + `mdi_line.c`.

**Verified:** `cd sim && cmake -B build && cmake --build build` links clean; headless
run (`SIM_MAX_FRAMES=120 SDL_VIDEODRIVER=dummy`) renders all six states, exit 0.
Visual eyeball of the real window (`./build/ytm_sim`) still recommended to confirm
glyph shapes. Regenerate fonts anytime with `./scripts/gen_icons.sh`.

<details><summary>Original generation steps + glyph table (reference)</summary>

### Glyph set + codepoints (Material Symbols Rounded)

| glyph          | codepoint | used for                          |
|----------------|-----------|-----------------------------------|
| skip_previous  | 0xE045    | prev                              |
| skip_next      | 0xE044    | next                              |
| play_arrow     | 0xE037    | play (paused state)               |
| pause          | 0xE034    | pause (playing/buffering state)   |
| thumb_up       | 0xE8DC    | like (pink filled / grey outline) |
| thumb_down     | 0xE8DB    | dislike (grey outline)            |

Design fills: prev/next/play/pause use FILL 1; thumb_down is FILL 0; thumb_up is
FILL 1 when favorited, FILL 0 otherwise. Variable-font instancing applies ONE fill per
generated file, so to be exact, generate **two** instances:
- `mdi_solid` (FILL 1, ~size 30): skip_previous, skip_next, play_arrow, pause, thumb_up
- `mdi_line`  (FILL 0, ~size 26): thumb_up, thumb_down

(Acceptable shortcut: a single FILL 1 file for all six, toggling thumb_up color
pink/grey — drops the outline-vs-filled distinction only.)

### Resume steps (run on the Mac)

1. **Get the tools + font**
   ```bash
   npm install -g lv_font_conv
   # Material Symbols Rounded variable TTF, e.g. from
   # github.com/google/material-design-icons → variablefont/
   #   MaterialSymbolsRounded[FILL,GRAD,opsz,wght].ttf
   ```
2. **Instance the variable font** (fonttools) so the converter gets static fills:
   ```bash
   pip install fonttools
   fonttools varLib.instancer "MaterialSymbolsRounded[FILL,GRAD,opsz,wght].ttf" \
     FILL=1 wght=500 GRAD=0 opsz=24 -o msym-fill.ttf
   fonttools varLib.instancer "MaterialSymbolsRounded[FILL,GRAD,opsz,wght].ttf" \
     FILL=0 wght=500 GRAD=0 opsz=24 -o msym-line.ttf
   ```
3. **Convert to LVGL C** (into `ui/`):
   ```bash
   lv_font_conv --font msym-fill.ttf --size 30 --bpp 4 --format lvgl --no-compress \
     -r 0xE045,0xE044,0xE037,0xE034,0xE8DC -o ui/mdi_solid.c
   lv_font_conv --font msym-line.ttf --size 26 --bpp 4 --format lvgl --no-compress \
     -r 0xE8DC,0xE8DB -o ui/mdi_line.c
   ```
   Save the two commands as `scripts/gen_icons.sh` for repeatability.
4. **Wire it in** (3 small edits):
   - `ui/styles.h` — declare the fonts and glyph macros:
     ```c
     LV_FONT_DECLARE(mdi_solid);
     LV_FONT_DECLARE(mdi_line);
     #define FONT_ICONS       (&mdi_solid)
     #define FONT_ICONS_LINE  (&mdi_line)
     #define IC_PREV   "\xEE\x81\x85"   // U+E045 skip_previous
     #define IC_NEXT   "\xEE\x81\x84"   // U+E044 skip_next
     #define IC_PLAY   "\xEE\x80\xB7"   // U+E037 play_arrow
     #define IC_PAUSE  "\xEE\x80\xB4"   // U+E034 pause
     #define IC_LIKE   "\xEE\xA3\x9C"   // U+E8DC thumb_up
     #define IC_DISLIKE "\xEE\xA3\x9B"  // U+E8DB thumb_down
     ```
     (UTF-8 bytes for each codepoint; double-check with `printf` if editing.)
   - `ui/now_playing_screen.c` — in `icon_btn(...)` set the label font to
     `FONT_ICONS` (and `FONT_ICONS_LINE` for the thumbs), and replace the transport
     glyph strings: `LV_SYMBOL_PREV→IC_PREV`, `LV_SYMBOL_NEXT→IC_NEXT`,
     `LV_SYMBOL_PAUSE/PLAY→IC_PAUSE/IC_PLAY`, `LV_SYMBOL_DOWN→IC_DISLIKE`,
     `LV_SYMBOL_UP→IC_LIKE`. Remove the `NOTE:` stand-in comment.
   - **Build files** — add the generated sources to both targets:
     `firmware/main/CMakeLists.txt` SRCS: `"${UI_DIR}/mdi_solid.c"
     "${UI_DIR}/mdi_line.c"`; `sim/CMakeLists.txt` add_executable list: the same two.
5. **Verify**: `cd sim && cmake -B build && cmake --build build && ./build/ytm_sim` —
   thumbs render as real Material Symbols.

> Sandbox-only fallback (no npm): a Python script using `freetype-py`/`Pillow` to
> rasterize the six glyphs from the TTF and emit a 4bpp `lv_font_t` C file. More code,
> but works where only PyPI-preinstalled libs are available. Only needed if you can't
> run `lv_font_conv`.

</details>

---

## Then — remaining sequenced deliverables

2. **Mac-side ytmdesktop bridge + protocol** (`SPEC-ytmusic-adapter.md`): build the
   bridge that talks to ytmdesktop's Companion Server (localhost:9863, Socket.IO +
   REST), normalizes into `now_playing_vm_t`, pushes pre-resized RGB565 cover bitmaps,
   and replace the board's mock feed + wire the `emit(...)` stubs to it.
3. **Other screens**: Browse, Explore, Idle/clock (all in the UX preview).
4. **System-audio energy → `level`**: derive ring energy on the Mac, push in the vm.

## Notes / gotchas carried forward

- BSP surface is isolated to `firmware/main/main.c` (`bsp/esp-bsp.h` +
  `bsp_display_start/lock/unlock`). If the Waveshare component's header differs, that's
  the only file to adjust.
- Pin-map trap: Arduino `pin_config.h` lists MCLK 16 (real 42) and 466×466 (real
  480×480). Trust the ESP-IDF BSP.
- Striped cover placeholder is a solid warm block + "cover" caption (LVGL can't do a
  repeating diagonal gradient); real art from the bridge replaces it anyway.
```
