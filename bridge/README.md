# ytmboard-bridge

Cross-platform bridge (Node.js, macOS or Windows): **ytmdesktop Companion Server →
`now_playing_vm` → board**.
Implements the core slice of [`docs/SPEC-ytmusic-adapter.md`](../docs/SPEC-ytmusic-adapter.md).

```
YouTube Music ── ytmdesktop (plays audio) ──Socket.IO/REST(127.0.0.1)── BRIDGE ──LAN ws── BOARD
```

The board stays dumb: it speaks one plain-WebSocket protocol and never touches
Socket.IO, auth, or TLS. The bridge handles all of that on the host PC.

> **Running on Windows?** See [WINDOWS-SETUP.md](WINDOWS-SETUP.md). It's the
> recommended host when the Mac is corporate-managed (Jamf/GlobalProtect/CrowdStrike),
> since the locked-down firewall blocks the board's inbound WebSocket connection.

## Prerequisites

1. **ytmdesktop v2.0.0+** running on this host (macOS or Windows).
2. In ytmdesktop: **Settings → Integration →** enable **Companion Server** *and*
   **"enable companion authorization."**

## Install & run

```bash
cd bridge
npm install
npm start            # first run prints a code; click ALLOW in ytmdesktop within ~30s
```

The Companion token is saved to `~/.ytmboard/token.json` and reused on later runs.
Force a fresh handshake (e.g. token revoked) with `npm run auth`.

## What it does (core slice)

- **Auth** — `requestcode` → `request` handshake, token persisted, re-auth on 401.
- **Subscribe** — Socket.IO to `ws://127.0.0.1:9863/api/v1/realtime`, `state-update`.
- **Normalize** — `/state` → view-model (`ui/now_playing_vm.h`), gating partial
  metadata and flagging ads.
- **Serve the board** — plain WS on `:8765`: snapshot on connect, fresh snapshot on
  every change; accepts `{ cmd, arg }` and maps to `POST /command`.
- **Cover art** — fetch the best thumbnail, square-crop to `COVER_PX`, encode RGB565,
  push as a binary frame keyed to the current track (re-rendered only when the art
  URL changes).
- **Discovery** — advertise `_ytmboard._tcp` over mDNS on the board port (TXT:
  `proto=ws`, `path=/`, `v=1`) so the board finds the host without a hard-coded IP.

## Board protocol (text JSON over WebSocket)

bridge → board:
```json
{ "type": "state", "data": { "title": "...", "artist": "...", "playback": "playing", "...": "..." } }
```
`playback` is one of `"playing" | "paused" | "buffering"`. See `normalize.js` for
the full field set (mirrors `now_playing_vm_t`).

board → bridge:
```json
{ "cmd": "toggle_play" }
{ "cmd": "seek", "arg": 42 }
```
Commands: `toggle_play | next | prev | toggle_favorite | seek | volume_up | volume_down`.

Cover art is a **binary** frame (little-endian), keyed to the current `track_id`:
```
off 0  "YC"   magic (2 bytes)
off 2  1      version (u8)
off 3  0      format (u8) -> RGB565, low byte first (LVGL native LE)
off 4  width  (u16)
off 6  height (u16)
off 8  pixels width*height*2 bytes, RGB565 LE   (blit into an lv_image_dsc_t)
```
Text frame = state JSON; binary frame = cover. The board distinguishes by WS opcode.

## Config (env overrides)

| Env | Default | Purpose |
|---|---|---|
| `YTMD_HOST` / `YTMD_PORT` | `127.0.0.1` / `9863` | Companion Server (IPv4 on purpose) |
| `BOARD_PORT` | `8765` | board-facing WebSocket port |
| `YTMBOARD_TOKEN_PATH` | `~/.ytmboard/token.json` | persisted token location |
| `YTMD_APP_ID` | `ytmboard` | Companion app identity |
| `COVER_PX` | `172` | cover-art square size pushed to the board (contract pair with the board's `COVER_PX`) |

## Quick test without a board

```bash
npm start
# in another shell:
npx wscat -c ws://127.0.0.1:8765        # prints state frames; type a command:
# {"cmd":"toggle_play"}
```
