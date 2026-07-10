# YT Music Companion

A hardware companion device for **YouTube Music**. A Waveshare
ESP32-S3-Touch-AMOLED-2.16 board acts as a polished physical UI client — showing the
current track and sending commands (play/pause/skip/like/seek/volume).

> **Requires [ytmdesktop](https://ytmdesktop.app) — this is a companion/remote, not a
> standalone player.** The board has no audio; it mirrors and controls whatever is
> playing in the ytmdesktop app on your PC (with its Companion Server enabled). Without
> ytmdesktop running, the board only shows the built-in mock-data demo.

<p align="center">
  <img src="docs/assets/now-playing.png" alt="Now Playing screen" width="360">
</p>

Audio playback runs on a host PC via the **ytmdesktop** app. A small cross-platform
**bridge** normalizes ytmdesktop's state into the board's view-model and serves it over
Wi-Fi. No audio, decoding, TLS, or auth runs on the board itself. The bridge ships as a
**downloadable tray app** (native Rust + Tauri, Windows & macOS) for non-technical users;
the original Node bridge remains in [`bridge/`](bridge/) for development. See
[bridge/WINDOWS-SETUP.md](bridge/WINDOWS-SETUP.md) for Windows host notes (the recommended
host when the Mac is corporate-managed — its firewall blocks the board's inbound
connection).

## Architecture

```
ytmdesktop (Mac/Win)  ──►  bridge (tray app)  ──Wi-Fi──►  ESP32-S3 board (UI)
  audio + state            normalize → VM                render + emit commands
```

- **Board** — pure UI client/controller. ESP32-S3 (BLE only, no A2DP), so it never
  touches audio.
- **Bridge** — talks to ytmdesktop's Companion Server (`localhost:9863`, Socket.IO +
  REST), pushes pre-resized 172×172 RGB565 cover art the board blits directly.
- **View-model** (`now_playing_vm_t`) — source-agnostic render layer; all
  YouTube-Music-specific terms live in the bridge.

## Layout

| Dir | Contents |
|-----|----------|
| `firmware/` | ESP-IDF firmware (vendor BSP, LVGL v9) for the board |
| `sim/` | Desktop LVGL simulator — run the UI without hardware |
| `ui/` | Shared UI: Now Playing screen, ring visualizer, styles, mock data |
| `design/` | HTML/UX design previews |
| `docs/` | Specs, overview, running instructions, handoff notes |
| `assets/` | Fonts and icon sources |
| `scripts/` | Icon generation, tooling |

## Stack

- **Hardware:** Waveshare ESP32-S3-Touch-AMOLED-2.16 (480×480, CO5300 QSPI)
- **Firmware:** ESP-IDF + vendor BSP, LVGL v9.5.0
- **Backend:** ytmdesktop Companion Server + a downloadable tray bridge app (native Rust +
  Tauri, Windows & macOS); Node bridge in `bridge/` for development

## Status

Spec-and-first-slice phase. Current deliverable: Now Playing vertical slice with mock
data. See [docs/PROJECT-OVERVIEW.md](docs/PROJECT-OVERVIEW.md) for full status and
[docs/RUNNING.md](docs/RUNNING.md) to build and run.

## Quick start

**The no-toolchain path (recommended):** flash the board from your browser, then install
the bridge app on your PC. No clone, no compiler, no CLI. Development paths (simulator,
build-from-source) follow.

### Step 1 — Flash your board (web installer)

On a computer with **Chrome or Edge**, open the install page, plug in the board, click
**Install**, and enter your 2.4&nbsp;GHz Wi-Fi when prompted. No ESP-IDF, no `menuconfig`.
The page is published at **https://etaiso.github.io/yt-music-companion/install/** (from
`site/install/`). See [site/install/README.md](site/install/README.md).

### Step 2 — Get the bridge

Download the bridge app for your OS from the
[**latest release**](https://github.com/etaiso/yt-music-companion/releases/latest)
(Windows `.exe` / macOS `.dmg`). It runs in the tray/menu bar, finds ytmdesktop, waits
for your board, and relays playback — nothing to build.

**Requires [ytmdesktop](https://ytmdesktop.app)** on the same PC, with **Companion Server**
+ **authorization** enabled (Settings → Integration). On first run the app shows a one-time
code — click **Allow** in ytmdesktop within ~30s. Both board and PC must share the same
**2.4GHz** Wi-Fi (the ESP32-S3 is 2.4GHz only). Unsigned for now, so accept the SmartScreen
/ Gatekeeper "Run anyway" / right-click-Open prompt.

Then **play a song** — the board auto-discovers the bridge over mDNS and switches to the
live track, cover art, and controls.

---

## For development

### Desktop simulator (no hardware)

Runs the exact same `ui/` code in an SDL window, cycling through all six player states:

```sh
brew install cmake sdl2                                            # macOS
cd sim && cmake -B build && cmake --build build && ./build/ytm_sim
```

On Windows, see [sim/README.md](sim/README.md) for the MSYS2 + SDL2 setup.

### Build the firmware from source (ESP-IDF)

**Prerequisites:**

- **ESP-IDF v5.5+** — to build and flash the firmware ([install guide](docs/RUNNING.md#21-install-esp-idf-one-time)).
- **[ytmdesktop](https://ytmdesktop.app)** on the host PC — **required** (the board is a
  remote, not a standalone player), with **Companion Server** + **authorization** enabled
  (Settings → Integration).
- **A 2.4GHz Wi-Fi network** the board and host PC both share (the ESP32-S3 is 2.4GHz only).
- **Your Wi-Fi SSID + password** — these are **mandatory** and set at build time via
  `menuconfig` (step 1); the board can't reach the bridge without them.

**1. Flash the firmware** (Dark theme is the default):

First **activate ESP-IDF in this terminal** — `idf.py` does not exist until you do,
and the activation only lasts for the current shell:

- **macOS/Linux:** `. ~/esp/esp-idf/export.sh`
- **Windows:** launch the **"ESP-IDF PowerShell"** shortcut from the Start Menu (it
  opens an already-activated shell), or dot-source the installer's init script. See
  [docs/RUNNING.md](docs/RUNNING.md#21-install-esp-idf-one-time) for both.

Then, in that same activated terminal:

```sh
cd firmware
idf.py set-target esp32s3            # FIRST, and only once — this (re)generates sdkconfig
idf.py menuconfig                    # REQUIRED: set Wi-Fi SSID/password under "YT Music board"
idf.py -p <port> flash monitor      # <port>: /dev/tty.usbmodem* (mac) or COMx (Windows)
```

`menuconfig` is also where you switch to the **Light** theme, adjust brightness, or set
a bridge-host fallback — see [docs/CONFIGURATION.md](docs/CONFIGURATION.md) for every
option. (For a mock-data demo with no Wi-Fi, disable the live feed there instead.)

Find `<port>` by listing serial devices with the board unplugged, then plugged in —
the new entry is the board:

- **macOS/Linux:** `ls /dev/tty.usb*` (board is usually `/dev/tty.usbmodem*`)
- **Windows:** `Get-CimInstance Win32_PnPEntity | ? Name -match 'COM\d+' | % Name`
  (board shows as **"USB Serial Device (COMx)"** — the ESP32-S3's native USB-JTAG)

**2. Run the bridge from source** on the host PC running [ytmdesktop](https://ytmdesktop.app)
(enable **Companion Server** + **authorization** in its Settings → Integration first).
The native-Rust bridge is the current implementation — developer parity with the shipped
tray app:

```sh
cd app
cargo run -p bridge-cli             # first run: click ALLOW in ytmdesktop within ~30s
```

See [app/README.md](app/README.md) for the workspace layout and the Windows toolchain
note. The original Node bridge is still available (`cd bridge && npm install && npm start`)
— see [bridge/README.md](bridge/README.md).

**3. Play a song.** The board auto-discovers the bridge over mDNS (both must share the
same subnet — Ethernet or the **2.4GHz** Wi-Fi band) and switches to the live track,
cover art, and controls.

Full details: [docs/RUNNING.md](docs/RUNNING.md) (build/flash), [app/README.md](app/README.md)
(Rust bridge + tray app), [bridge/README.md](bridge/README.md) and
[bridge/WINDOWS-SETUP.md](bridge/WINDOWS-SETUP.md) (Node bridge, recommended on Windows when
the Mac is corporate-managed).

## Landing page

A static marketing page lives in [`site/`](site/) and is published to GitHub Pages at
**https://etaiso.github.io/yt-music-companion/** (via [.github/workflows/pages.yml](.github/workflows/pages.yml)).
It's a single self-contained `index.html` — no build step. Preview it locally with the
`site` config in `.claude/launch.json`, or any static server:

```sh
cd site && python3 -m http.server 8090   # then open http://localhost:8090
```

## License

MIT — see [LICENSE](LICENSE). © 2026 Etai Solomon.

Not affiliated with YouTube or Google. "YouTube Music" is a trademark of Google LLC.

**Third-party assets & dependencies:**

- [Inter](https://rsms.me/inter/) font — SIL Open Font License 1.1
- [Material Symbols](https://fonts.google.com/icons) — Apache License 2.0
- [LVGL](https://lvgl.io) — MIT License
- [ESP-IDF](https://github.com/espressif/esp-idf) — Apache License 2.0
