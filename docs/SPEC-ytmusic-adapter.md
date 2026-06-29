# Spec — YouTube Music bridge/adapter (board)

**Purpose:** Make the board control + display **YouTube Music**, behind the same
`now_playing_vm_t` view-model defined in `SPEC-ytmusic-now-playing.md` (§7). This is
the **board's only backend** — a small host-side bridge that talks to ytmdesktop and
feeds the board.

> **"Mac-side" / "on the Mac" throughout this spec means "on the host PC running
> ytmdesktop," not Mac-specifically.** The bridge is plain Node.js and runs on macOS
> or Windows; the placement decision in §3 is host-vs-board, host-OS-agnostic. Windows
> is in fact the recommended host when the Mac is corporate-managed — see
> `bridge/WINDOWS-SETUP.md`.

**Status:** Spec only. Build after the Now Playing screen renders from mock data (the
screen is what gives this bridge a visible target). Companion to the Now Playing spec.

**Read first:** `SPEC-ytmusic-now-playing.md` (§3 architecture, §6 screen, §7 view-model).

---

## 1. How it fits

The board render layer reads **only** `now_playing_vm_t` and never calls a backend
directly (Now Playing spec §7). A *backend adapter* fills that struct. This document
specs that adapter for **YouTube Music**.

There is **no official Google YouTube Music API** for playback/now-playing. The route
is a local desktop wrapper that exposes a control API: **ytmdesktop** (YouTube Music
Desktop App) and its **Companion Server API**. Audio plays on the Mac; the board
never streams YouTube Music.

---

## 2. Target API — ytmdesktop Companion Server (verified)

App: ytmdesktop/ytmdesktop, v2.0.0+. Enable: **Settings → Integration → Companion
Server**, plus "enable companion authorization."
Base URL (on the Mac): `http://localhost:9863/api/v1`
Friendlier docs: https://ytmdesktop.github.io/developer/companion-server/getting-started.html

**Auth (one-time, interactive):**
1. `POST /auth/requestcode` `{appId, appName, appVersion}` → `{code}`
   (`appId`: lowercase alphanumeric, 2–32 chars; `appName` 2–48; `appVersion` semver)
2. `POST /auth/request` `{appId, code}` → `{token}`
   ⚠️ **Requires the user to click Allow in the app; times out after 30 s.**
3. Authenticated requests send header `Authorization: <token>`.
   Token is bound to `appId`; re-requesting overwrites. Persist it.

**State:** `GET /state` (and the realtime `state-update` event) →
```
player.trackState   : -1 unknown | 0 paused | 1 playing | 2 buffering
player.videoProgress: seconds (current position)
player.volume       : 0..100
player.adPlaying    : bool  (metadata is stale/absent while true)
player.queue        : { items[], repeatMode, selectedItemIndex, ... } | null
video.title         : string
video.author        : string   (artist)
video.album         : string | null
video.thumbnails[]  : { url, width, height }
video.likeStatus    : -1 unknown | 0 dislike | 1 indifferent | 2 like | null
video.durationSeconds: number
video.id            : string (videoId)
video.isLive        : bool   (v>=2.0.6)
video.videoType     : 0 audio | 1 video | 2 uploaded | 3 podcast (v>=2.0.6)
video.metadataFilled: bool   (false = data still incomplete; wait before showing)
playlistId          : string
```

**Commands:** `POST /command` `{command, data?}` —
`playPause | play | pause | next | previous | seekTo(data=sec) | setVolume(0..100) |
volumeUp | volumeDown | mute | unmute | repeatMode(0|1|2) | shuffle | toggleLike |
toggleDislike | playQueueIndex(n) | changeVideo({videoId, playlistId})`.

**Realtime:** **Socket.IO** server at `/api/v1/realtime` (NOT raw WebSocket). Use
**websocket-only transport**, pass `auth: { token }`, and connect over **IPv4**
(`127.0.0.1`, not `localhost`, which can resolve to IPv6 and fail). Event
`state-update` carries the full `/state` object. Socket.IO is the preferred channel;
the REST API is rate-limited (honor `x-ratelimit-*`) to discourage polling.

---

## 3. Placement decision — Mac-side **bridge** (recommended)

| | A. On-board adapter | B. Mac-side bridge ✅ |
|---|---|---|
| Talks Socket.IO | ESP32 must speak Socket.IO/engine.io (painful; raw WS won't do) | Node `socket.io-client` / Python `python-socketio` — trivial |
| Reachability | Companion Server may bind **loopback only** → unreachable from LAN | Bridge is on the Mac (localhost) → no problem |
| Auth dance | 30 s interactive flow + token storage on the MCU | Handled once on the Mac; token persisted there |
| Board complexity | High | Board speaks one simple protocol (§6) |

**Decision: B.** A small Mac-side bridge connects to ytmdesktop (localhost, Socket.IO,
auth), normalizes state into the board's view-model, and exposes the simple
board-facing protocol (§6) on the LAN. The board stays dumb. This is what makes the
board reliable without putting Socket.IO, auth, or TLS on the MCU.

> Open item: confirm whether the Companion Server binds `0.0.0.0` or loopback-only.
> Either way the bridge wins, but if it binds all interfaces, A becomes *possible*
> (still not recommended given Socket.IO + auth on the MCU).

### Architecture
```
YouTube Music ── ytmdesktop (plays audio) ──Socket.IO/REST(localhost)── BRIDGE ──LAN(ws)── BOARD
                                                                         │
                                              normalizes → now_playing_vm_t JSON + cover bitmap
```

---

## 4. Bridge responsibilities (Mac-side, Node or Python)
1. **Auth:** run the requestcode→request flow once (`appId:"ytmboard"`,
   `appName:"YT Music board"`); store the token; re-auth on 401.
2. **Subscribe:** connect Socket.IO to `127.0.0.1:9863/api/v1/realtime`, listen for
   `state-update`. Fall back to `GET /state` poll (slow, rate-limited) only if needed.
3. **Normalize:** map `/state` → `now_playing_vm_t` (§5). Debounce; skip updates while
   `metadataFilled === false`; set `ad_playing` while `adPlaying`.
4. **Cover art:** pick the best `video.thumbnails[]`, fetch, resize to the cover slot
   (172×172), convert to **RGB565**, push to the board. The resize target and the
   board's expected cover dimension are a contract pair — bump them together.
5. **Serve the board protocol (§6):** push vm JSON on change; accept commands and
   translate to `POST /command` (§7).
6. **Discovery:** advertise over mDNS (e.g. `_ytmboard._tcp`) so the board finds the Mac.

---

## 5. Field mapping — `/state` → `now_playing_vm_t`
```
title           ← video.title
artist          ← video.author
album           ← video.album                            // optional context line
source_name     ← "YouTube Music" (or video.album / playlist context)
cover_img       ← best of video.thumbnails[] → RGB565 172px (bridge-side)
playback        ← player.trackState: 0→PB_PAUSED, 1→PB_PLAYING, 2→PB_BUFFERING, -1→buffering
is_favorite     ← (video.likeStatus === 2)               // command: toggleLike
is_live         ← video.isLive
ad_playing      ← player.adPlaying
host_connected  ← bridge socket connected
position_sec    ← player.videoProgress
duration_sec    ← video.durationSeconds                  // ignored when is_live
level           ← n/a (no audio energy from ytmdesktop) → bridge sends 0, or a synthetic
                  pulse; the ring visualizer falls back to its reduce-motion pulse
```

> **Timeline.** The view-model uses a finite, seekable timeline
> (`position_sec` + `duration_sec`), which matches YouTube Music directly. For the
> rare live stream (`video.isLive`), set `is_live = true` and let the UI hide the
> total and show a LIVE badge instead of a seekable total.

> **`level` (ring visualizer):** ytmdesktop exposes no audio energy/FFT. Options:
> (a) rings fall back to the timed pulse, (b) bridge synthesizes a tempo-ish pulse,
> (c) later, derive energy on the Mac from system audio. Ship (a); note (c) as future.

---

## 6. Board-facing protocol
Keep it dead simple so the board needs no Socket.IO / no auth dance / no TLS:
- **Transport:** plain WebSocket, board → `ws://<mac-ip>:<port>` (port advertised via mDNS).
- **State:** on connect, bridge sends a full `now_playing_vm_t` JSON snapshot; then a
  fresh snapshot (or diff) on every change. (Plain WS or SSE — pick one; recommend WS.)
- **Cover:** sent as a binary frame (RGB565 + small header: w,h) keyed to the current track.
- **Commands:** board → bridge as `{ "cmd": "...", "arg": ... }`:
  `toggle_play | next | prev | toggle_favorite | seek(sec) | volume_up | volume_down`.
- **Liveness:** heartbeat/ping; board shows the "disconnected" state (Now Playing spec §6)
  when the socket drops.

---

## 7. Command mapping — board → `POST /command`
```
toggle_play     → playPause
next            → next
prev            → previous
toggle_favorite → toggleLike
seek(sec)       → seekTo            data = clamp(sec, 0, durationSeconds)
volume_up       → volumeUp          (or setVolume with a step)
volume_down     → volumeDown
```

---

## 8. Gotchas (bank these)
- **Socket.IO ≠ WebSocket.** It layers engine.io framing/handshake on top. The bridge
  uses a real Socket.IO client; the board never touches it.
- **IPv4 only / loopback binding.** Connect to `127.0.0.1`, not `localhost`. Confirm
  whether the server is even reachable off-box (it may be loopback-only) — another
  reason the bridge lives on the Mac.
- **`metadataFilled`.** YTM first emits partial data, then sets this true. Don't update
  title/cover until true, or the board flickers wrong metadata.
- **`adPlaying`.** Metadata is stale/absent during ads — set `ad_playing` so the board
  shows an "Advertisement" placeholder rather than the last song.
- **Rate limits.** Honor `x-ratelimit-*`; never poll `/state` for realtime — use the
  socket. Auth `/auth/request` can take up to 30 s (user prompt).
- **Unofficial app.** ytmdesktop is community software automating the user's own
  session — lower risk than scraping, but not Google-sanctioned. Note for the user.

---

## 9. Out of scope / sequencing
1. Now Playing screen must exist first (renders the vm) — `SPEC-ytmusic-now-playing.md`.
2. After this bridge validates the board protocol (§6), wire the board's `emit(...)`
   stubs to it and replace mock data with the live feed.
3. Library/search/browse over YouTube Music (playlists, queue) — later; the
   Browse/Explore screens are still parked.
4. th-ch/youtube-music (pear-desktop) as an alternative wrapper — its API Server is
   newer/less settled; ytmdesktop's Companion API is the stable target. Keep as fallback.

## 10. Open questions / decisions
- Bridge language: **Node** (matches the ytmdesktop JS/TS ecosystem and Socket.IO
  client) or **Python**? Recommend Node.
- Board transport for the protocol: **WebSocket vs SSE**? Recommend WebSocket.
- Confirm Companion Server bind address (loopback vs all interfaces).

## 11. References
- Companion Server API v1: https://github.com/ytmdesktop/ytmdesktop/wiki/v2-%E2%80%90-Companion-Server-API-v1
- Friendlier docs: https://ytmdesktop.github.io/developer/companion-server/getting-started.html
- ytmdesktop app: https://github.com/ytmdesktop/ytmdesktop
- View-model + screen: `SPEC-ytmusic-now-playing.md` (§7, §6)
