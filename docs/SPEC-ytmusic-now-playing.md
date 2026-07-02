# Spec — YT Music Board: Now Playing screen (LVGL)

**Purpose:** Build the first on-device screen for the **YT Music board**, a hardware
companion that displays and controls **YouTube Music** on a Waveshare
ESP32-S3-Touch-AMOLED-2.16. This document is a cold-start handoff: it carries all
hardware facts, the architecture decision, the design system, and the scoped first
task so a fresh session needs no prior context.

**Status:** Now Playing **V2 has shipped** across slices 1–7 — dark/light build-time
theme + design tokens (`ui/styles.h`), album-derived palette module
(`ui/palette.h`/`.c`) tinting the rings, V2 ring geometry + 172 px cover
(`ui/ring_visualizer.c`), and the full V2 screen with all six states
(`ui/now_playing_screen.c`). It builds and runs on both the desktop sim and the board.
The screen renders from the **§7 view-model** (mock data + the live bridge). This doc
has been updated to describe the V2 that shipped; the **PRD in GitHub issue #2** is the
design source of truth. (The original V1 task description below is kept for context,
annotated where V2 supersedes it.)

---

## 1. What this project is

A physical companion for **YouTube Music**. Audio plays on the Mac (via the
**ytmdesktop** desktop app); the board is a **hardware front-end** ("dock/remote")
that shows the current track and sends commands. The board never streams audio
itself — see §3.

The full control/metadata path (ytmdesktop's Companion Server → a small host-side
bridge → the board) is specified separately in `SPEC-ytmusic-adapter.md`. This
document is about the **screen**.

---

## 2. Hardware reference

**Board:** Waveshare ESP32-S3-Touch-AMOLED-2.16
Docs: https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-2.16
Demo repo: https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.16

| Item | Detail |
|---|---|
| SoC | ESP32-S3R8, dual-core Xtensa LX7 @ 240 MHz |
| RAM | 8 MB PSRAM — **octal mode** (`CONFIG_SPIRAM_MODE_OCT`), consumes GPIO 33–37 |
| Flash | 16 MB NOR |
| Display | 2.16″ AMOLED, **480×480**, driver **CO5300**, **QSPI** interface |
| Touch | **CST9220** capacitive, I2C |
| Audio out | **ES8311** codec (mono DAC) → power amp (enable GPIO46) → onboard speaker |
| Audio in | **ES7210** 4-ch ADC (dual-mic array + AEC) |
| Other | AXP2101 PMIC, QMI8658 6-axis IMU, PCF85063 RTC, microSD slot, battery header |
| Frameworks | Arduino (vendor demos `Arduino-v3.3.5`) and ESP-IDF (`ESP-IDF-v5.5`) |

### Verified pin map (cross-checked: ESP-IDF BSP **and** Arduino `07_ES8311`)

```
# Audio — ES8311 (out) + ES7210 (mic in) share ONE I2S bus
I2S_MCLK  = 42      # NOT 16 (see trap below)
I2S_BCLK  = 9
I2S_WS    = 45      # LRCK
I2S_DOUT  = 8       # data -> ES8311 DAC (playback)
I2S_DIN   = 10      # data <- ES7210 mic ADC
PA_ENABLE = 46      # speaker amp: HIGH = on

# I2C — codecs, touch, IMU, AXP2101, RTC all share this bus
I2C_SDA   = 15
I2C_SCL   = 14

# Display — CO5300 AMOLED, QSPI, 480x480  (use the BSP; don't hand-wire — see note)
```

### Gotchas (do not skip)
- **Stale Arduino header.** `Arduino-v3.3.5/libraries/Mylibrary/pin_config.h` has
  two copy-paste errors inherited from the 1.75″ board: it says `MCLK 16` (real =
  **42**) and `466×466` (real = **480×480**). Trust the ESP-IDF BSP, not that file.
- **Octal PSRAM** eats GPIO 33–37 → very few free broken-out pads. Not relevant to
  this UI task, but matters if/when adding an external DAC later.
- **Partition:** big-APP scheme needed once Wi-Fi/TLS/LVGL are all in (16 MB flash).
- **Display pins:** the only place display GPIOs appeared was that stale Arduino
  header, so treat them as unconfirmed. The UI task should rely on the **vendor
  BSP's panel/touch init** (which has the correct CO5300 QSPI + CST9220 config
  baked in), not hand-entered pins.

---

## 3. Architecture (the key decision)

**The board is a thin client. The Mac is the engine.**

| Concern | Runs on |
|---|---|
| YouTube Music playback, track metadata, like status, queue | **Mac (ytmdesktop)** |
| Audio output (DAC / speakers / Bluetooth headphones) | **Mac** |
| State normalization + cover-art resizing (the bridge) | **Mac** |
| UI rendering, touch input, sending commands | **Board** |

Wire: **board ← WS/SSE** (state: now-playing, cover art, position, playback) and
**board → commands** (play/pause/skip/like/seek/volume), over Wi-Fi on the LAN.
The bridge that produces this state is specified in `SPEC-ytmusic-adapter.md`.

**Why this shape:**
- Audio quality + Bluetooth: the **ESP32-S3 has BLE only — no Bluetooth Classic /
  A2DP**, so it physically can't stream to standard BT speakers/headphones. Letting
  the Mac own audio sidesteps this entirely (Mac's BT stack handles headphones) and
  removes the on-board mono-codec quality ceiling.
- No official YouTube Music playback API exists, so the Mac side leans on
  **ytmdesktop**, which already plays the audio and exposes a control/state API.
- The board stays light: no TLS / decode / RAM pressure.

**Parked (documented for completeness, not this task):** a *standalone* audio mode
where the board plays a stream itself via ES8311 + the schreibfaul1 `ESP32-audioI2S`
library. Not applicable to YouTube Music (no streamable endpoint) and **out of scope
here.**

---

## 4. Tech stack for the board

**Locked base: ESP-IDF v5.5 + the vendor BSP + LVGL v9.** (Decided at kickoff — see §11.)

Start from the repo example `examples/ESP-IDF-v5.5/02_lvgl_demo_v9`, which uses the
`esp32_s3_touch_amoled_2_16` BSP component to bring up the CO5300 QSPI panel,
CST9220 touch, and LVGL v9 — a known-good display+touch+LVGL stack for *this exact
board*. `05_Spec_Analyzer` is a useful reference for tapping audio energy if/when
the visualizer is driven by real audio.

**LVGL version:** use the **v9 line** (current major; latest is **v9.5.0**, Feb 2026 —
v8 is legacy). v9 is also what the vendor BSP targets, so you build *with* the board
support rather than porting it. Pin a specific v9.x in the manifest for reproducibility.

Networking for §3 (WS/SSE client + command channel) is first-class in ESP-IDF
(`esp_http_client`, `esp_websocket_client`, `esp_event`, `esp_wifi`).

---

## 5. Design system

These tokens are the project's own design language — a warm, editorial look with a
single bold gradient reserved for brand and "on" states. Translate them into LVGL
theme constants (an `lv_theme` or a shared `styles.h`). The same tokens drive the
HTML UX preview (`ytmusic-board-ux-preview.html`).

> **V2 update (PRD #2) — SHIPPED.** `ui/styles.h` now splits these tokens into a
> **Dark** set (the default — near-black/AMOLED) and a **Light** set (the cream look).
> Theme is chosen at **build time** (§11), a token swap with no runtime toggle. The
> V2 dark theme is what the device renders by default; the cream values are the Light
> set, painted on the same V2 layout. Both sets are listed below as they exist in
> `ui/styles.h`. The 135° indigo→purple→pink brand gradient is **retired from this
> screen** — the rings are tinted from the **album-derived palette** (see below); the
> play button and progress bar are solid `COL_INK` (white on Dark), and the only
> accent the Now Playing screen paints is `COL_PINK` (the V2 red) on the playing-state
> dot/label and the favorited like. `COL_INDIGO`/`COL_PURPLE` stay defined for brand
> use elsewhere but are unused by this screen.

### Color tokens — Dark (default, V2)
```
COL_BG        #070709   near-black — app background
COL_BG2       #121215   cards / raised surfaces (disconnected banner)
COL_INK       #FFFFFF   primary text · play button · progress fill
COL_INK2      #B7B7BB   secondary text  (~white .72)
COL_INK3      #949499   muted / meta    (~white .55)
COL_INK4      #6A6A6E   faint / placeholders / icons (~white .4)
COL_LINE      #2A2A2D   borders / dividers / progress track (~white .12)
COL_DANGER    #FF6B6B   offline accent (status + banner dot)
COL_STRIPE    #16161A   cover placeholder fill (raised dark)
COL_ON_ACCENT #FFFFFF   glyph/text drawn on an accent fill
COL_COVER_A   #3A3A3D   neutral cover block (ad/empty) — top
COL_COVER_B   #1C1C1F   neutral cover block (ad/empty) — bottom
COL_SHADOW    #000000   drop-shadow color (play button)

Accent stops (rings use the album palette; only COL_PINK is used on this screen):
  COL_INDIGO  #6366F1   brand gradient start (defined, unused here)
  COL_PURPLE  #A855F7   brand secondary (defined, unused here)
  COL_PINK    #FF4458   primary interactive accent — V2 RED (playing dot/label, like)
```

### Color tokens — Light (cream, build-time `-DYTM_THEME=LIGHT`)
```
COL_BG      #FBF7F3   warm cream — app background
COL_BG2     #FFFFFF   cards / raised surfaces
COL_INK     #1A1410   primary text · play button · progress fill
COL_INK2    #4A3F37   secondary text
COL_INK3    #8A7A6E   muted / meta
COL_INK4    #B8A99A   faint / placeholders / icons
COL_LINE    #ECE2D8   borders / dividers / progress track
COL_DANGER  #C0392B   destructive / offline accent
COL_STRIPE  #F3EADF   cover placeholder fill
COL_COVER_A #D8CFC2   neutral cover block (ad/empty) — top
COL_COVER_B #BFB3A2   neutral cover block (ad/empty) — bottom
COL_SHADOW  #000000   drop-shadow color (play button)

Accent stops:  #6366F1 indigo → #A855F7 purple → #EC4899 pink (Light keeps pink as
the primary accent; Dark swaps pink for the V2 red #FF4458).
```
> LVGL note: LVGL can't gradient-fill text. For the wordmark/brand titles, either
> use a pre-rendered gradient image/font or approximate with a solid accent. The ring
> visualizer uses **per-ring solid strokes** sampled from the album palette's three
> stops (not a single `lv_grad_dsc_t` sweep). The play button is a solid `COL_INK`
> circle with an inverted (`COL_BG`) glyph and a soft `COL_SHADOW` drop shadow; the
> progress bar is a knobless solid `COL_INK` fill on a `COL_LINE` track — no gradients
> remain on the V2 Now Playing screen.

### Album-derived palette (`ui/palette.h` / `palette.c`)
A pure, LVGL-independent module that turns the current cover art into a **3-stop
palette** — `[0] light → [1] mid → [2] accent` — driving the ring strokes (and, by
design, the ambient glow; see below). `palette_derive()` takes the raw RGB565 cover
bitmap, downsamples it to ≤16×16 cells, takes the mean as the base tint and the
most-saturated cell as the accent, then builds the stops by adjusting
lightness/saturation in HSL. It is **deterministic, malloc-free, and host-unit-tested**
(`tests/test_palette.c`), and runs **once per track change** (keyed on the cover
`lv_image_dsc_t` pointer), never per frame.

`PALETTE_NEUTRAL` is the fixed fallback — a cool desaturated gray ramp
(`#B9C0CC → #8A93A3 → #5C6473`) — returned for any **no-art** case: NULL/zero-dim
art, or a cover with no usable hue (grayscale, pure black/white, peak saturation
< 0.10). The renderer also forces neutral for the ad / idle (nothing playing) /
disconnected states. Rings start neutral until the first track's palette is pushed.

> **Ambient glow — designed, not yet built.** The V2 design
> (`NowPlayingDeviceV2.dc.html`) paints a full-bleed album-mood glow behind the
> cover, and the palette module is meant to feed it as well as the rings. In the
> firmware/sim today the glow is **gated behind `THEME_AMBIENT_ENABLED`** (Dark-only;
> Light paints flat) and is left to a later slice — the renderer wires the palette
> into the **ring strokes only**.

### Typography
**Bundled font: Inter** (SIL OFL, embeddable). Inter is a UI-designed face that is
strong at heavy weights and reads cleanly at small sizes on the AMOLED panel. The
shipped V2 weight set is **Regular / SemiBold 600 / ExtraBold 800** (900 is not
bundled — 800 suffices at panel scale). Sizes below are the generated LVGL faces in
`ui/styles.h`.
| Role | Token | Shipped face |
|---|---|---|
| Section label / status | `FONT_LABEL` | Inter ExtraBold 12, UPPERCASE, letter-spacing ~1 px |
| Display title | `FONT_TITLE` | Inter ExtraBold 29, tight tracking |
| Body (artist) | `FONT_BODY` | Inter SemiBold 17, ink2 |
| Meta (album) | `FONT_META` | Inter SemiBold 13, ink3 |
| Time (elapsed/total) | `FONT_TIME` | Inter SemiBold 12, ink3 |

Transport/like glyphs are bundled **Material Symbols** icon fonts (`mdi_solid` FILL 1,
`mdi_line` FILL 0), generated by `scripts/gen_icons.sh`.

### Shapes
- Pills: fully rounded (`RAD_PILL`). Cards/banner: `RAD_CARD` 16 px. Hero cover:
  `RAD_COVER` **26 px** (V2, was 20). Soft shadows (play button drop shadow).

### Signature element
**Concentric audio rings** — the project's identity (logo + splash + now-playing).
On this board they are the hero of Now Playing and **react to live audio energy**.
This is the user's original "sound visualization" goal. In V2 the rings are **tinted
from the album-derived palette** (above) rather than the fixed brand gradient — they
take on the color of whatever's playing, and fall back to the neutral gray ramp for
no-art states. Exact V2 geometry is in §6.

---

## 6. THE TASK — Now Playing screen

A single full-screen (480×480) LVGL screen. **Reference design (V2, current source of
truth):** `design/now-playing-screen-design/project/NowPlayingDeviceV2.dc.html` (the
dark theme, album-tinted rings, 172 px cover). The V1 file `NowPlayingDevice.dc.html`
and `ytmusic-board-ux-preview.html` are the superseded cream mockups. Build it to
render from the **view-model in §7**, populated with mock data for now.

### Layout (regions, top → bottom)
```
┌─────────────────────────────────────────┐
│  YouTube Music                  ▷ PLAYING │   status bar: source label (l) · state (r)
│                                          │
│            ╭───────────────╮             │
│         (   concentric rings  )          │   HERO: audio-reactive album-tinted rings
│        (    ┌─────────┐       )           │   with 172 px cover centered (rounded, RAD_COVER)
│         (   │  cover  │      )            │
│            ╰───────────────╯             │
│                                          │
│             Midnight Drive               │   title (display, 800)
│             The Reverb Club              │   artist (ink2)
│             Neon Nights · Album          │   album/context (ink3, optional)
│                                          │
│   1:12  ▓▓▓▓▓▓▓░░░░░░░  3:48              │   elapsed / total seekable bar
│                                          │
│     ♡    ⏮     (❚❚)     ⏭     ♥          │   transport row
└─────────────────────────────────────────┘
```

### The ring visualizer (the one bold thing — spend effort here)
**Shipped V2 geometry** (`ui/ring_visualizer.c`), transcribed 1:1 from the
`NowPlayingDeviceV2.dc.html` canvas `draw()`:
- **3 concentric rings** behind/around the cover, centered on the cover (`cr = 86`,
  i.e. the **172 px cover** radius), raised high on the screen (design `cy ≈ 188`).
- Per-ring radius: `r = cr + baseGap + i·step + amp[i]·level·0.9`, with
  **`baseGap = 20`, `step = 24`, `amp = [18, 12, 8]`** (outer→inner index `i = 0..2`).
- Per-ring stroke **width `3.6 − 0.7·i`** → `[3.6, 2.9, 2.2]`; per-ring
  **alpha `(0.8 − 0.2·i)·(0.4 + 0.6·level)`** (inner rings react more strongly).
- **Color:** each ring takes a stop from the **album palette** mapped light→mid→accent
  across inner→outer (the ripple uses the accent). Neutral gray ramp until the first
  track palette is pushed; neutral for no-art states. (V1 used a fixed
  indigo/purple/pink stroke — replaced.)
- **Ripple echo** on energy peaks: spawned when `level > 0.7`; expands `r += 2.8`,
  decays `life −= 0.02`, alpha `life·0.4`, width `2.4·life + 0.4`.
- The rings/ripples deliberately **bleed past the hero box** (overflow-visible, ring
  container 360 px) instead of clipping — matching the design's full-bleed canvas.
- **Audio-reactive:** `level` 0..1 updated per frame. Mock now (sine/eased random);
  the live bridge sends `level 0` (ytmdesktop exposes no FFT — see §9 / adapter §5),
  so a synthesized two-detuned-sines "breathing" fallback animates the rings when no
  real energy arrives. Paused passes `level ≈ 0.16`, idle/buffering `≈ 0.06`.
- Respect a **reduce-motion** setting (minimal slow breathing instead of energy).

### Interactions (emit intent only — no transport logic on board)
| Control | Emits |
|---|---|
| Play/pause (center, white 80 px circle, dark glyph) | `toggle_play` |
| Prev / Next | `prev` / `next` |
| Like ♥ (right) | `toggle_favorite` (→ toggleLike) |
| Timeline drag | `seek(offset_seconds)` |
| (optional) side button / gesture | `volume_up` / `volume_down` |

In this task, commands just log / call a stub `emit(cmd, args)`. Wiring them to the
bridge is a later task (§9).

### States & copy (V2 state frames, `now_playing_screen.c`)
The V1 50% dim **scrim was removed** in V2; states differentiate via the rings, the
cover block, the status dot/label, and (for disconnected) a banner.
- **Playing** — full layout; status dot/label in `COL_PINK` (V2 red) with a pulsing
  dot; rings album-tinted and energy-reactive.
- **Paused** — play glyph; status `PAUSED` (`COL_INK4`/`COL_INK3`); rings gentle pulse
  (`level ≈ 0.16`).
- **Buffering** — `BUFFERING`; shimmer placeholder bars replace title/artist/album;
  rings idle (`level ≈ 0.06`).
- **No track info (idle)** — title `Nothing playing right now`, `IDLE` state, no
  artist/album, progress row hidden; cover = neutral `COL_COVER_A→COL_COVER_B` block
  with a centered `music_note` glyph; rings neutral palette.
- **Ad playing** — title `Advertisement`, `AD` state; same neutral cover block +
  `music_note`; rings neutral. **No dim scrim** (metadata is stale during ads — see
  adapter spec §8).
- **Disconnected from host** — `OFFLINE` (`COL_DANGER`); a **glassy banner** (opaque
  `COL_BG2` rounded panel ≈ rgba .92, red glow dot) reads `Can't reach your computer —
  check it's on and on the same network.` (error states explain + how to fix; no
  apology.)

---

## 7. View-model (the board↔bridge contract seed)

This struct is the **interface**. The screen reads only from it; mock fills it now,
the bridge's WS/SSE feed fills it later. Designing the screen against this is what
makes the §9 protocol fall out cleanly.

> **Source-agnostic by design.** The render layer must read *only* this struct and
> never call a backend directly. A YouTube Music adapter (the bridge) fills the
> struct; if another source is ever added, it maps its own fields onto the same slots
> without touching the UI. Keep all YT-Music-specific wording confined to the bridge,
> not the render code.

> **Timeline model.** YouTube Music is a *finite, seekable* track, so the timeline is
> `position_sec` + `duration_sec` (elapsed / total). `is_live` is kept for the rare
> live-stream case: when true, hide the total and show a LIVE badge instead of a
> seekable total.

```c
typedef enum { PB_PLAYING, PB_PAUSED, PB_BUFFERING } playback_t;

typedef struct {
    char         source_name[64];  // e.g. "YouTube Music" (or album / playlist context)
    bool         is_live;          // true only for live streams; default false

    char         title[96];        // empty => "Nothing playing right now"
    char         artist[96];
    char         album[96];        // optional context line; may be empty
    bool         ad_playing;       // true => show "Advertisement", ignore stale metadata
    // cover art: start with a bundled placeholder / gradient.
    // later: the bridge pushes a PRE-RESIZED RGB565 bitmap sized to the cover slot
    //   (172x172, ~58KB). Board blits directly — no on-device TLS or JPEG decode.
    //   (Decided: pre-resize on the Mac for quality+performance — see §11.)
    const void  *cover_img;        // lv_img_dsc_t* (RGB565) or NULL

    playback_t   playback;
    bool         is_favorite;      // like status (video.likeStatus === 2)

    // finite seekable timeline
    int32_t      position_sec;     // current position
    int32_t      duration_sec;     // total length (ignored when is_live)

    float        level;            // 0..1 audio energy for the ring visualizer

    bool         host_connected;   // false => bridge<->YouTube-Music link down
    conn_state_t conn_state;       // board<->bridge link: loader / online / offline
} now_playing_vm_t;
```

Provide a `mock_tick(now_playing_vm_t*)` that animates `level`, advances `position_sec`
toward `duration_sec`, loops a fake track, and occasionally flips
`is_favorite`/`ad_playing` so all states are visible.

---

## 8. Definition of done (this task)
- ESP-IDF project builds & flashes on the board; Now Playing renders at 480×480.
- All six states (§6) reachable via mock data.
- Ring visualizer animates from `vm.level`; reduce-motion fallback works.
- Design tokens centralized in one styles header; no ad-hoc colors.
- Controls call `emit(...)` stubs (logged over serial).
- Clean separation: **render reads only `now_playing_vm_t`**; no network code yet.

## 9. Out of scope / next steps (record, don't build)
1. **YT Music bridge + protocol:** the host-side bridge that talks to ytmdesktop's
   Companion Server and feeds this view-model — fully specified in
   `SPEC-ytmusic-adapter.md`. Build after this screen renders from mock data.
2. **Other screens:** Browse / Search (queue + search + mini-player), Explore
   (playlists/moods), Idle/clock splash. (All in `ytmusic-board-ux-preview.html`.)
3. **Wi-Fi provisioning / host discovery** (mDNS for the Mac running the bridge).
4. **Standalone audio mode** (§3, parked — not applicable to YouTube Music).
5. **System-audio energy → `level`:** derive ring energy on the Mac from system audio
   and push it in the vm. Until then the rings use the reduce-motion pulse.

---

## 10. References
- Board docs: https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-2.16
- Board demos (BSP + LVGL v9 base): https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.16 → `examples/ESP-IDF-v5.5/02_lvgl_demo_v9`
- Schematic: linked from the board docs "Resources" page (confirm tap points there if needed)
- ytmdesktop Companion Server API: https://github.com/ytmdesktop/ytmdesktop/wiki/v2-%E2%80%90-Companion-Server-API-v1
- Bridge + field mapping: `SPEC-ytmusic-adapter.md`
- **V2 design (current source of truth):** `design/now-playing-screen-design/project/NowPlayingDeviceV2.dc.html`; PRD in GitHub issue #2 ("Now Playing screen V2")
- V1 mockups (superseded): `NowPlayingDevice.dc.html`, `ytmusic-board-ux-preview.html`
- Shipped UI source: `ui/styles.h` (tokens), `ui/palette.h`+`.c` (album palette), `ui/ring_visualizer.c` (rings), `ui/now_playing_screen.c` (screen)

## 11. Decisions (resolved at kickoff)
- **Framework:** ESP-IDF + vendor BSP (board is a pure UI client; §4).
- **LVGL:** v9 line, latest v9.x — current major, matches the vendor BSP (§4).
- **Font:** **Inter** (OFL), ExtraBold/800 for titles, Regular/Medium for body (§5).
- **Cover art:** the bridge pushes a **pre-resized RGB565 bitmap** (172 px) to the
  board; the board blits it — no on-device decode (§7). The bridge resize target and
  the board's expected cover dimension are a **contract pair** — bump them together.
- **Timeline:** finite **`position_sec` + `duration_sec`** seekable model;
  `is_live` switches the UI to a LIVE badge for live streams (§7).
- **Theme:** ~~ship the **cream** look now~~ → **superseded by the V2 redesign (PRD
  #2).** The dark/AMOLED variant is now the default, selected at **build time** (firmware
  Kconfig `choice`, default Dark; desktop sim `-DYTM_THEME=LIGHT`). The styles header
  splits the tokens of §5 into Dark and Light sets; "Light" is the cream look painted on
  the V2 layout, not V1. The token-table-swap structure anticipated here is exactly what
  made that possible. See `ui/styles.h` and PRD #2 / issue #3.
