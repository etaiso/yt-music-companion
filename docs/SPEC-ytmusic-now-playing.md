# Spec — YT Music Board: Now Playing screen (LVGL)

**Purpose:** Build the first on-device screen for the **YT Music board**, a hardware
companion that displays and controls **YouTube Music** on a Waveshare
ESP32-S3-Touch-AMOLED-2.16. This document is a cold-start handoff: it carries all
hardware facts, the architecture decision, the design system, and the scoped first
task so a fresh session needs no prior context.

**Status:** Greenfield. Nothing built yet. First deliverable = the **Now Playing**
screen, rendering from **mock data**, styled in the project's design system, with an
**audio-reactive concentric-ring visualizer** as the hero element.

---

## 1. What this project is

A physical companion for **YouTube Music**. Audio plays on the Mac (via the
**ytmdesktop** desktop app); the board is a **hardware front-end** ("dock/remote")
that shows the current track and sends commands. The board never streams audio
itself — see §3.

The full control/metadata path (ytmdesktop's Companion Server → a small Mac-side
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

### Color tokens
```
--bg     #FBF7F3   warm cream — app background
--bg2    #FFFFFF   cards / raised surfaces
--ink    #1A1410   primary text
--ink2   #4A3F37   secondary text
--ink3   #8A7A6E   muted / meta
--ink4   #B8A99A   faint / placeholders / icons
--line   #ECE2D8   borders / dividers
--danger #C0392B   destructive / error only

Brand gradient (135°):  #6366F1 indigo → #A855F7 purple → #EC4899 pink
  - pink   #EC4899 = primary interactive accent (icons, hearts, active text, focus)
  - purple #A855F7 = secondary (play controls, status dots, likes ★, section labels)
  - Use the GRADIENT sparingly — brand moments, primary actions, active/"on" states only.
```
> LVGL note: LVGL can't gradient-fill text. For the wordmark/brand titles, either
> use a pre-rendered gradient image/font or approximate with solid `pink`. For the
> ring visualizer, LVGL supports gradient strokes via `lv_grad_dsc_t` / arc styling;
> a 3–4 stop indigo→purple→pink gradient is the target.

### Typography
**Bundled font: Inter** (SIL OFL, embeddable). Inter is a UI-designed face that is
strong at heavy weights and reads cleanly at small sizes on the AMOLED panel. Bundle
**ExtraBold/800** for titles + section labels and **Regular/Medium** for body/meta.
| Role | Style |
|---|---|
| Section label | 11 px, weight 800, UPPERCASE, letter-spacing ~1.5 px |
| Display title | large (~22–28 px on this screen), weight 800, tight tracking |
| Body | ~15 px, ink / ink2 |
| Meta / caption | 11–13 px, ink3 |

### Shapes
- Pills: fully rounded (radius 999). Cards/dialogs: ~16 px radius. Soft shadows.

### Signature element
**Concentric audio rings** — the project's identity (logo + splash + now-playing).
On this board they are the hero of Now Playing and **react to live audio energy**.
This is the user's original "sound visualization" goal.

---

## 6. THE TASK — Now Playing screen

A single full-screen (480×480) LVGL screen. Reference mockup:
`ytmusic-board-ux-preview.html` (the Now Playing panel). Build it to render from the
**view-model in §7**, populated with mock data for now.

### Layout (regions, top → bottom)
```
┌─────────────────────────────────────────┐
│  YouTube Music                  ▷ PLAYING │   status bar: source label (l) · state (r)
│                                          │
│            ╭───────────────╮             │
│         (   concentric rings  )          │   HERO: audio-reactive gradient rings
│        (    ┌─────────┐       )           │   with cover art centered (rounded ~20px)
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
- 3–4 concentric arcs/circles, brand gradient stroke, behind/around the cover.
- **Audio-reactive:** rings scale + fade outward driven by a normalized energy
  value (`level` 0..1) updated per frame. Mock now (sine/eased random); later fed by
  the bridge if a system-audio energy source is added (ytmdesktop exposes no FFT —
  see §9 / adapter spec §5). Rings ripple outward; inner rings react more strongly.
- Respect a "reduce motion / visualizer off" setting (gentle timed pulse fallback).

### Interactions (emit intent only — no transport logic on board)
| Control | Emits |
|---|---|
| Play/pause (center, purple) | `toggle_play` |
| Prev / Next | `prev` / `next` |
| Like ♥ (right) | `toggle_favorite` (→ toggleLike) |
| Timeline drag | `seek(offset_seconds)` |
| (optional) side button / gesture | `volume_up` / `volume_down` |

In this task, commands just log / call a stub `emit(cmd, args)`. Wiring them to the
bridge is a later task (§9).

### States & copy (plain, active, sentence case)
- **Playing** — full layout.
- **Paused** — play glyph; rings fall back to gentle pulse.
- **Buffering** — shimmer on title line; rings idle.
- **No track info** — title area shows `Nothing playing right now`; no artist, no
  album. Cover falls back to a gradient block.
- **Ad playing** — title area shows `Advertisement`; cover = gradient block; controls
  dimmed (metadata is stale during ads — see adapter spec §8).
- **Disconnected from host** — small banner: `Can't reach the Mac — check it's on and
  on the same network.` (error states explain + how to fix; no apology.)

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
    //   (~120x120, ~28KB). Board blits directly — no on-device TLS or JPEG decode.
    //   (Decided: pre-resize on the Mac for quality+performance — see §11.)
    const void  *cover_img;        // lv_img_dsc_t* (RGB565) or NULL

    playback_t   playback;
    bool         is_favorite;      // like status (video.likeStatus === 2)

    // finite seekable timeline
    int32_t      position_sec;     // current position
    int32_t      duration_sec;     // total length (ignored when is_live)

    float        level;            // 0..1 audio energy for the ring visualizer

    bool         host_connected;   // false => disconnected state
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
1. **YT Music bridge + protocol:** the Mac-side bridge that talks to ytmdesktop's
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
- Mockup: `ytmusic-board-ux-preview.html` (Now Playing = first panel)

## 11. Decisions (resolved at kickoff)
- **Framework:** ESP-IDF + vendor BSP (board is a pure UI client; §4).
- **LVGL:** v9 line, latest v9.x — current major, matches the vendor BSP (§4).
- **Font:** **Inter** (OFL), ExtraBold/800 for titles, Regular/Medium for body (§5).
- **Cover art:** the bridge pushes a **pre-resized RGB565 bitmap** (~120 px) to the
  board; the board blits it — no on-device decode (§7).
- **Timeline:** finite **`position_sec` + `duration_sec`** seekable model;
  `is_live` switches the UI to a LIVE badge for live streams (§7).
- **Theme:** ship the **cream** look now; structure tokens in one styles header so a
  **dark/AMOLED variant** is a later token-table swap, not a rewrite (§5).
