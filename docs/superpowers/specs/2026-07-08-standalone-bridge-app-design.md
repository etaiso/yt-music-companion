# Standalone bridge app — design

**Date:** 2026-07-08
**Status:** Approved — **Phase 0 spike PASSED (2026-07-09) → Branch T (Tauri) selected.**

## Problem

The web installer removed the toolchain from flashing the *board*. But the **bridge**
still requires a developer setup: install Node, `git clone`, `npm install`, `npm start`,
click Allow, approve the Windows firewall prompt. A non-technical user can't do that. The
bridge is the last piece standing between "I bought/built the board" and "it just works."

## Goal

A **downloadable standalone bridge app** for Windows and macOS that a non-technical user
runs with no clone, no Node, no terminal. It runs quietly in the background as a **tray /
menu-bar app**, guides the user through the ytmdesktop prerequisites, and surfaces the
one-time authorization code. The landing page presents the whole no-toolchain path:
**flash the board in the browser → download the bridge → done.**

## Decisions locked in brainstorming

- **Form factor:** tray / menu-bar background app (not a bare console, not a full window).
- **Platforms:** Windows **and** macOS in the first release.
- **Onboarding:** *guided* — the app detects each prerequisite state and shows the right
  call-to-action (download ytmdesktop, enable Companion Server, click Allow).
- **Backend stack:** decided by a **de-risking spike** (Phase 0 below), not upfront.

## Non-goals (YAGNI — explicitly out of scope for v1)

- **Auto-update.** Ship via GitHub Releases; updating = re-download. (Both candidate
  stacks support auto-update later.)
- **Code signing.** No paid certs assumed. Accept the SmartScreen / Gatekeeper warning and
  document the "Run anyway" / right-click-Open bypass. Signing is a documented later step.
- **In-app *install* of ytmdesktop.** We detect it and link to their download; we do not
  bundle or silently install it.
- **Settings beyond launch-at-login.** Bridge host auto-discovers via mDNS; brightness is
  on-device; theme is fixed. Nothing else needs a settings surface.
- **Packaging the firmware flasher into the app.** Flashing stays in the browser (web
  installer). The app is bridge-only.

## Phase 0 — the de-risking spike (decision gate)

The one real architectural risk is whether the bridge can be re-implemented in Rust
(enabling a tiny Tauri app) without losing ytmdesktop compatibility. Retire it first with a
**throwaway** Rust program — no UI, no packaging:

**Pass criteria (all three):**
1. Completes the ytmdesktop **Companion auth handshake** (`rust-socketio` + `reqwest`):
   request code → user clicks Allow → receives + persists a token.
2. Connects to the **realtime** channel and prints **live track state** as songs change.
3. Bonus: `mdns-sd` advertises `_ytmboard._tcp` and a client (`wscat` or the real board)
   discovers it.

**Human-in-the-loop:** requires ytmdesktop running with Companion Server enabled and the
user clicking Allow once, with music playing. Scaffolded Rust + a one-line run command are
provided; the maintainer runs it and reports the outcome.

**Decision:**
- **Spike clean → Branch T (Tauri + native Rust bridge).**
- **Spike painful** (protocol/version friction, flaky auth, missing crate behavior) →
  **Branch E (Electron + existing Node bridge).**

The spike is genuinely disposable: its auth/socket/mDNS code informs whichever branch wins
but is not shipped as-is.

**Result (2026-07-09): PASSED clean → Branch T.** The throwaway spike lives at
`spike/ytmd-rust/`. All three criteria met against real ytmdesktop on Windows:
`reqwest` completed the auth handshake (token persisted), `rust_socketio` streamed live
`state-update`s (verified track/playback/position changes), and `mdns-sd` advertised
`_ytmboard._tcp` and resolved it (correct TXT, IPv4 + IPv6). No protocol/version friction.
Build side also de-risked: compiles clean on Windows with the **GNU** toolchain + MinGW gcc
(the default MSVC toolchain fails to link here); TLS via schannel, no OpenSSL. **Proceed on
Branch T.**

## Stack-independent core (true for both branches)

This is the majority of the user-facing design and does not depend on the spike outcome.

### State machine (single source of truth the UI renders)

```
starting
  → ytmd-not-found            (probe 127.0.0.1:9863 fails)
  → not-authorized            (server up, no valid token)  → surface AUTH CODE
  → waiting-for-board         (authorized, streaming state, no board yet)
  → board-connected           (board WS client attached)
  ↺ ytmd-disconnected         (server went away mid-run; fall back + retry)
```

- ytmdesktop **presence is detected by probing `127.0.0.1:9863`** (the Companion Server),
  not by hunting for a process name — simpler and identical on both OSes.
- The auth code has a ~30s validity window; the UI shows a countdown / "waiting for Allow"
  and re-requests on expiry.

### Tray / menu-bar app with a guided status window

- **Tray icon** whose state/color reflects the state machine; tooltip = current status.
- **Guided window** (opened from the tray) shows the state and the matching CTA:
  - *ytmd-not-found* → "YouTube Music Desktop isn't running. **[Download ytmdesktop]** ·
    [How to enable Companion Server]".
  - *not-authorized* → large **auth code** + "Click **Allow** in ytmdesktop within 30s",
    also fired as an **OS notification** so it's seen with the window closed.
  - *board-connected* → "✓ Connected — your board is live." plus a **launch-at-login**
    toggle.
- Both candidate stacks (Tauri, Electron) provide tray + notifications + login-item +
  cross-platform installers, so this layer is portable across the decision gate.

### Bridge behavior (unchanged contract)

Whatever the language, the bridge keeps its current responsibilities and wire contracts:
ytmdesktop Companion auth + realtime state, `normalize` → `now_playing_vm`, board WS
server on `:8765`, `_ytmboard._tcp` mDNS advertisement, and **172×172 RGB565** cover art
matching the board's `COVER_PX` (see `bridge/src/config.js`, `bridge/src/cover.js`).

## Branch T — Tauri + native Rust bridge (if spike passes)

- Rust core ports the existing bridge: `rust-socketio` (ytmd), `reqwest` (auth REST),
  `tokio-tungstenite` (board WS server), `mdns-sd` (advertise), the `image` crate
  (**native** cover resize — the `sharp`/`jimp` question disappears), token persistence.
- Tauri v2 shell provides tray, notifications, login-item, and a small webview status
  window rendering the state machine.
- **Artifact:** ~5–10 MB, no bundled runtime, no Chromium — sits naturally next to the
  zero-download browser installer.

## Branch E — Electron + Node bridge (if spike is painful)

- **Refactor the existing bridge into a library:** `bridge/src/core.js` exports
  `createBridge({ onStatus, onAuthCode, onBoardConnected, ... })` returning an
  `EventEmitter`. Every current `console.log` lifecycle point becomes an **emitted event**.
- `bridge/src/index.js` becomes a **thin CLI** that subscribes and prints — the developer
  `npm start` experience is unchanged.
- **`app/`** (new): Electron main runs `createBridge()`, owns tray + window + notifications
  + login-item; `preload.js` exposes a safe IPC surface; `renderer/` is the guided window.
- **`sharp` → `jimp`:** replace the only native module with a pure-JS resizer (one 172×172
  resize per track — perf is a non-issue), keeping the RGB565 byte contract. This makes the
  Windows + macOS (x64 + arm64) build far simpler and smaller.
- **Artifact:** ~80–120 MB (bundles Chromium + Node). Normal for a desktop helper; reuses
  known-good Socket.IO compatibility.

## Packaging, CI & distribution (shared shape)

- **Installers:** Windows (`.exe`/NSIS or MSI) + macOS (`.dmg`, arm64 + x64), built on a
  GitHub Actions **matrix** (windows + macos runners). Same shape whether `tauri-action` or
  `electron-builder`.
- **Distribution:** **GitHub Releases**, alongside the firmware image already attached on
  version tags (`.github/workflows/` — see the existing release job). One release tells the
  whole story: *flash the board / get the bridge*.
- Extend the existing workflows; do not add a parallel release pipeline.

## Landing page

Restructure `site/index.html`'s getting-started story into the **two-step, no-toolchain
path**, demoting developer instructions:

1. **Flash your board** → the web installer (existing `site/install/`).
2. **Get the bridge** → new download button, OS-detected (Windows / macOS), linking to the
   latest GitHub Release asset.
3. A collapsed **"Build from source (developers)"** section retains today's
   menuconfig / `npm` content.

## Testing

- **Spike** is its own proof (the three pass criteria above).
- **Pure logic host-testable** in whichever language wins: `normalize`, the cover RGB565
  contract, and the state-machine transitions.
- **Manual end-to-end, once per OS:** download the app → guided ytmdesktop setup → Allow →
  board connects and plays; verify launch-at-login and the not-found / not-authorized CTAs.

## Open questions for planning

- ~~**Spike outcome** determines Branch T vs E~~ — **resolved: spike passed, Branch T.**
- macOS: unsigned `.app` Gatekeeper flow wording; whether a universal binary or two arch
  builds. (Signing remains out of scope for v1.)
- Launch-at-login mechanics per OS (Login Items vs registry Run key) — provided by the
  chosen framework; confirm during planning.
- Whether the bridge library refactor (Branch E) is worth doing **regardless** so the CLI
  and any future GUI share one core — decide at plan time based on the spike result.
