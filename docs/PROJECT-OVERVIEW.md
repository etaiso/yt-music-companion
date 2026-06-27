# YT Music Companion — project overview

**What it is**

A hardware companion device for **YouTube Music**. The Waveshare
ESP32-S3-Touch-AMOLED-2.16 (already in hand) is a polished physical UI client: it
shows the current track and sends commands (play/pause/skip/like/seek/volume). All
audio playback runs on the Mac via the **ytmdesktop** desktop app; a small Mac-side
bridge normalizes ytmdesktop's state into the board's view-model and serves it to the
board over Wi-Fi. No audio, decoding, TLS, or auth runs on the board itself.

**Current state**

Spec-and-first-slice phase. Two specs are written:

- `SPEC-ytmusic-now-playing.md` — the first vertical slice: the Now Playing screen,
  rendering from mock data.
- `SPEC-ytmusic-adapter.md` — the Mac-side bridge that talks to ytmdesktop's
  Companion Server (localhost:9863, Socket.IO + REST) and feeds the board.

Key decisions locked:

- **Framework:** ESP-IDF + vendor BSP (not Arduino — the board is a pure UI client
  with no audio decode needed).
- **LVGL:** v9 line (v9.5.0, matches vendor BSP).
- **Font:** Inter ExtraBold/800 (OFL-licensed).
- **Cover art:** the bridge pushes pre-resized ~120×120 RGB565 bitmaps; the board
  blits directly (no on-device JPEG decode or TLS).
- **Timeline:** finite `position_sec` + `duration_sec` seekable model; `is_live`
  switches the UI to a LIVE badge for live streams.
- **Theme:** cream/warm first; dark/AMOLED variant later as a token-table swap.
- **View-model** (`now_playing_vm_t` C struct): source-agnostic render layer; all
  YouTube-Music-specific terminology lives in the bridge.

An HTML UX preview (`ytmusic-board-ux-preview.html`) covers four screens: Now Playing,
Browse, Explore, and Idle/clock.

**Sequenced deliverables** (one at a time, fully scoped before the next):

1. **Build the Now Playing vertical slice with mock data** ← current
2. Build the Mac-side YT Music bridge; wire the board's `emit(...)` stubs to it and
   replace mock data with the live feed.
3. Remaining screens (Browse, Explore, Idle/clock).
4. System-audio energy → ring visualizer `level` (derive on the Mac).

**Key learnings & principles**

- **Hardware constraints shaped the architecture.** ESP32-S3 has BLE only (no
  Bluetooth Classic/A2DP) and cannot run desktop libraries, so audio stays on the
  Mac and the board is a client/controller.
- **No official YouTube Music API.** The control/state path goes through ytmdesktop's
  Companion Server, fronted by a Mac-side bridge.
- **Verified pin map matters.** The Waveshare Arduino header (`pin_config.h`) has
  confirmed copy-paste errors — MCLK listed as GPIO 16 (correct: 42) and resolution
  466×466 (correct: 480×480). Verified: I2S MCLK=42, BCLK=9, WS=45, DOUT=8, DIN=10,
  PA_ENABLE=46, I2C SDA=15, SCL=14; display 480×480 via CO5300 QSPI.
- **Source-agnostic view-model.** The `now_playing_vm_t` struct is backend-neutral;
  the bridge handles all ytmdesktop specifics.

**Tools & resources**

- **Hardware:** Waveshare ESP32-S3-Touch-AMOLED-2.16
- **Firmware stack:** ESP-IDF + vendor BSP, LVGL v9
- **Backend:** ytmdesktop Companion Server API (localhost:9863) + a Mac-side bridge
- **Reference repos:** Waveshare demo repository, ytmdesktop
