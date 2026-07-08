# Web installer for YT Music Companion — design

**Date:** 2026-07-08
**Status:** Approved (brainstorming)

## Problem

Flashing the board today requires a full ESP-IDF toolchain, `idf.py menuconfig` to set
Wi-Fi credentials (a build-time Kconfig option), and `idf.py flash`. That is far too much
for a non-technical user. We want a **prebuilt firmware plus a simple web UI** that lets a
user flash the board and enter their Wi-Fi with zero toolchain.

## Goal

A static **GitHub Pages** install page that, on Chrome/Edge desktop, flashes a prebuilt
firmware image over USB and provisions Wi-Fi — no ESP-IDF, no Node, usually no drivers.

## Non-goals (YAGNI — explicitly out of scope)

- **Theme selection.** Ship **Dark only, non-configurable** for now. `ui/styles.h` stays a
  compile-time `#if` (dark is already the default). No runtime theme, no two-binary build.
- **Brightness / bridge host in the form.** Brightness is already runtime-tunable on-device
  (swipe-down panel, persists to NVS); bridge host auto-discovers via mDNS. Neither needs
  install-time config.
- **Live vs mock.** Shipped binary is always "live". Mock stays a dev-only Kconfig option.
- **OTA updates** and **fancy re-provisioning UX.** Re-setup = re-flash or NVS erase.

## Key facts established

- Wi-Fi creds are consumed at build time in `firmware/main/net_backend.c:79-80`
  (`CONFIG_YTM_WIFI_SSID` / `CONFIG_YTM_WIFI_PASSWORD`).
- NVS is already initialized and used for brightness persistence (`firmware/main/main.c`,
  namespace + `bright` key). Adding Wi-Fi keys is a natural extension.
- Theme is a compile-time `#if defined(CONFIG_YTM_THEME_LIGHT)` in `ui/styles.h:22`, shared
  by firmware and sim. Making it runtime would be a large refactor — hence dropped.
- Web Serial API (Chrome/Edge desktop, Android Chrome) lets a page flash the ESP32-S3's
  native USB-JTAG serial port with the user's per-session permission. Not available on
  Safari, Firefox, or iOS — this is the one real platform limitation.

## Architecture

```
GitHub Release (CI-built)        GitHub Pages site (installer/)
  ytm-firmware.bin  ──served──►    index.html + ESP Web Tools
                                   ├─ [Install] → esptool-js flashes over Web Serial
                                   └─ Improv-serial → Wi-Fi SSID/pw form → NVS
```

**User flow:** plug board into USB → open page in Chrome/Edge → click **Install** → pick
the serial port → firmware flashes → Improv Wi-Fi form appears → enter SSID/password →
board stores creds to NVS, connects, reports success → normal UI.

## Components

### 1. Firmware — Wi-Fi credentials from NVS

`net_backend.c` reads SSID/password from the existing NVS namespace (new keys, e.g.
`wifi_ssid` / `wifi_pass`) instead of `CONFIG_YTM_WIFI_*`. The Kconfig values remain only as
a fallback default (so a developer `menuconfig` build still works). Interface: a small helper
that returns the effective creds (NVS if present and non-empty, else Kconfig default).

### 2. Firmware — provisioning boot state

On boot, if NVS holds no valid creds (rather than attempting to connect with `changeme`):

- Show a **"Setup — open the installer"** screen on the panel.
- Start the Improv-serial responder and wait for `SET_WIFI`.
- On creds received: store to NVS, attempt connect, report Improv success/error, then proceed
  to the normal UI (reboot or continue).

### 3. Firmware — Improv-serial responder (main technical risk)

Implement the Improv-serial protocol over the board's USB-serial line. ESP-IDF has **no
official Improv component**, so we implement the byte-framing ourselves. The protocol is
small and documented:

- Respond to `GET_DEVICE_INFO`, `GET_CURRENT_STATE`.
- Accept `SET_WIFI` (SSID + password), verify connection, return current-state / error
  packets.

The frame parser must be a **pure function** so it is host-testable without hardware. Coexist
with (or take over) the console on that serial port during provisioning.

**Plan must resolve feasibility of this component before committing** — reading/writing the
USB-CDC serial while the IDF console may also use it, and confirming the Improv handshake ESP
Web Tools expects.

### 4. Web installer (`installer/`)

New top-level directory:

- `index.html` + minimal CSS — board image, one **Install** button
  (`<esp-web-install-button>`), a clear "Chrome or Edge on desktop required" note, brief steps.
- **ESP Web Tools** loaded from a pinned/vendored copy (self-contained, no unpinned CDN).
- `manifest.json` referencing the single merged firmware image (same-origin path).
- Improv Wi-Fi UI is built into ESP Web Tools — no custom form to write.

### 5. CI / release

Extend `.github/workflows/` with a release job triggered on a version tag:

- Build the firmware once (dark, live).
- `idf.py merge-bin` into one flashable image.
- Deploy `installer/` to GitHub Pages with the bin copied in, so the manifest references a
  same-origin file and the page is pinned to that release's binary.

## Testing

- **Host unit tests** (existing MinGW host-test setup): Improv frame parser, and the NVS
  cred read/fallback helper.
- **Manual**: real flash + Wi-Fi provisioning on Chrome against a board; confirm it connects
  to the bridge and plays.
- **Sim unaffected** — always mock; theme via existing `-D` flag.

## Open questions for planning

- Exact Improv-serial coexistence with the IDF console on the USB-CDC port.
- Whether provisioning proceeds by reboot or in-place continuation after `SET_WIFI`.
- GitHub Pages deploy mechanism (Pages Action vs `/docs`) given the repo is currently private
  free-plan (see memory: branch-protection / Pages availability).
