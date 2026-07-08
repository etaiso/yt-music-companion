# Standalone bridge — Branch T, Part 1: Rust `bridge-core` + CLI

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Port the Node bridge (`bridge/src/*.js`) to a native-Rust `bridge-core` library plus a thin CLI, preserving every wire contract, so Part 2 (Tauri shell) can render it and developers can run it with `cargo run` instead of `npm start`.

**Architecture:** A Cargo workspace under `app/`. `bridge-core` is an async (tokio) library: pure-logic modules (`config`, `normalize`, `cover`, `commands`, `state`) are host-testable with no I/O; I/O modules (`auth`, `ytmd`, `board_server`, `discovery`) are ported from the proven Phase 0 spike (`spike/ytmd-rust/`) and Node source. An orchestrator (`bridge.rs`) wires them and emits a `BridgeEvent` stream over an `mpsc` channel — the single API both the CLI (Part 1) and the Tauri app (Part 2) consume. `bridge-cli` is a thin binary that subscribes and prints.

**Tech Stack:** Rust (GNU toolchain on this box), tokio, `rust_socketio` (async), `reqwest` (async, no-TLS default features — localhost is http), `tokio-tungstenite` (board WS server), `mdns-sd`, `image` (native cover resize — replaces sharp), `serde`/`serde_json`, `anyhow`.

## Global Constraints

Every task's requirements implicitly include these. Values copied verbatim from `docs/superpowers/specs/2026-07-08-standalone-bridge-app-design.md`, the SPEC docs, and the Node source being ported.

- **Toolchain:** build with `stable-x86_64-pc-windows-gnu`; the default MSVC toolchain fails to link on this machine (MSYS2 `link.exe` shadow, no VS Build Tools). MinGW gcc at `C:\msys64\mingw64\bin` must be on PATH. A `rust-toolchain.toml` pins gnu. Bash run prefix: `PATH="$HOME/.cargo/bin:/c/msys64/mingw64/bin:$PATH"`.
- **ytmd Companion identity:** `appId = "ytmboard"`, `appName = "YT Music board"`, `appVersion = "1.0.0"`. Auth requests send header `Authorization: <token>` — **raw token, no `Bearer`**.
- **ytmd endpoints (base `http://127.0.0.1:9863/api/v1`):** `POST /auth/requestcode {appId,appName,appVersion} -> {code}`; `POST /auth/request {appId,code} -> {token}` (blocks on user Allow, ~30s); `GET /state` (auth header); `POST /command {command,data?}` (auth header). IPv4 `127.0.0.1` on purpose — server is not on IPv6.
- **ytmd realtime:** Socket.IO, base `http://127.0.0.1:9863`, **namespace `/api/v1/realtime`**, **websocket transport only**, `auth: {token}`, event `state-update` carries the full `/state` object.
- **Token persistence:** `~/.ytmboard/token.json` as `{appId, token}`; only honor a token whose `appId` matches. (`~` = `%USERPROFILE%` on Windows.)
- **Board WS protocol (plain WS on `:8765`, no auth/TLS):**
  - bridge→board text frame: `{"type":"state","data":<vm>}`
  - bridge→board binary frame: cover art (see cover header below)
  - board→bridge: `{"cmd": <string>, "arg": <number?>}`
  - On client connect: send latest state snapshot, then latest cover frame if any.
  - Heartbeat: ping every 15000 ms; drop a client that missed the previous pong.
- **View-model (`vm`) JSON keys (snake_case, exact):** `source_name`, `is_live`, `track_id`, `title`, `artist`, `album`, `ad_playing`, `cover_url`, `playback` (string `"playing"|"paused"|"buffering"`), `is_favorite`, `position_sec`, `duration_sec`, `level`, `host_connected`.
- **Cover binary frame (little-endian):** off0 `"YC"` magic (2 bytes); off2 version=`1` (u8); off3 format=`0` (u8, RGB565 LE); off4 width (u16); off6 height (u16); off8 pixels = width\*height\*2 bytes RGB565 LE. RGB565 packing: `((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)`.
- **COVER_PX = 172.** CONTRACT PAIR with the board's `COVER_PX` (`firmware/main/net_backend.c`); the board rejects covers whose header dims ≠ its expected size. Square-crop with cover-fit, centre.
- **Command map (board cmd → ytmd command):** `toggle_play→playPause`, `next→next`, `prev→previous`, `toggle_favorite→toggleLike`, `seek→seekTo` (data = arg seconds, clamped to `[0, duration]` when duration>0), `volume_up→volumeUp`, `volume_down→volumeDown`. Unknown → ignore with a warning.
- **State machine:** `starting → ytmd-not-found` (probe `127.0.0.1:9863` fails) `→ not-authorized` (server up, no valid token → surface AUTH CODE) `→ waiting-for-board` (authorized, streaming, no board) `→ board-connected` (board WS attached); `↺ ytmd-disconnected` (server went away mid-run; fall back + retry).
- **Normalize gating:** if `!adPlaying && video.metadataFilled == false` → skip (return `None`) — don't flicker partial data. Ads have no metadata by design and are not gated. Debounce state bursts 120 ms into one board update. Re-render cover only when `cover_url` changes; skip cover for ads.
- **This is a port, not a redesign.** Preserve behavior and comments' intent from `bridge/src/*.js`. Do not add features (YAGNI). No `sharp`/`jimp` — `image` crate only.

---

## File Structure

```
app/
  Cargo.toml                     workspace: members = ["bridge-core", "bridge-cli"]
  rust-toolchain.toml            pins stable-x86_64-pc-windows-gnu
  .gitignore                     /target, Cargo.lock (lib workspace)
  bridge-core/
    Cargo.toml
    src/
      lib.rs                     re-exports; module tree
      config.rs                  constants + env overrides + token_path()
      normalize.rs               NowPlayingVm, Playback, normalize()
      cover.rs                   encode_rgb565(), render_cover(), COVER_HEADER_BYTES
      commands.rs                YtmdCommand, map_command()
      state.rs                   BridgeState, Signal, next_state()
      auth.rs                    load/save token, request_token, ensure_token
      ytmd.rs                    YtmdClient (async socket.io + REST)
      board_server.rs            BoardServer (tokio-tungstenite), frame helpers
      discovery.rs               Discovery (mdns-sd advertise)
      bridge.rs                  Bridge orchestrator, BridgeEvent, run()
  bridge-cli/
    Cargo.toml
    src/
      main.rs                    subscribe to BridgeEvent, print (npm-start parity)
```

Each `bridge-core` module has one responsibility and is small enough to hold in context. Pure-logic modules carry unit tests inline (`#[cfg(test)]`). The spike at `spike/ytmd-rust/` is the reference for `auth`/`ytmd`/`discovery` and stays untouched.

---

## Task 1: Workspace scaffold + `config`

**Files:**
- Create: `app/Cargo.toml`, `app/rust-toolchain.toml`, `app/.gitignore`
- Create: `app/bridge-core/Cargo.toml`, `app/bridge-core/src/lib.rs`, `app/bridge-core/src/config.rs`
- Test: inline in `app/bridge-core/src/config.rs`

**Interfaces:**
- Produces:
  - `config::ytmd_base() -> String` → `"http://{host}:{port}/api/v1"`
  - `config::ytmd_realtime_base() -> String` → `"http://{host}:{port}"`
  - `config::ytmd_realtime_namespace() -> &'static str` → `"/api/v1/realtime"`
  - `config::token_path() -> std::path::PathBuf`
  - consts: `APP_ID: &str`, `APP_NAME: &str`, `APP_VERSION: &str`, `BOARD_PORT: u16`, `VOLUME_STEP: u16`, `COVER_PX: u32`
  - `config::ytmd_host() -> String`, `config::ytmd_port() -> u16` (env-overridable)

- [ ] **Step 1: Create the workspace manifest**

`app/Cargo.toml`:
```toml
[workspace]
resolver = "2"
members = ["bridge-core", "bridge-cli"]

[workspace.package]
version = "1.0.0"
edition = "2021"
publish = false

[workspace.dependencies]
tokio = { version = "1", features = ["rt-multi-thread", "macros", "sync", "time", "net", "signal"] }
serde = { version = "1", features = ["derive"] }
serde_json = "1"
anyhow = "1"
reqwest = { version = "0.12", default-features = false, features = ["json"] }
rust_socketio = { version = "0.6", features = ["async"] }
tokio-tungstenite = "0.21"
futures-util = "0.3"
mdns-sd = "0.13"
image = { version = "0.25", default-features = false, features = ["jpeg", "png", "webp"] }
tracing = "0.1"
tracing-subscriber = "0.3"
```

- [ ] **Step 2: Pin the toolchain and ignore build output**

`app/rust-toolchain.toml`:
```toml
[toolchain]
channel = "stable-x86_64-pc-windows-gnu"
```

`app/.gitignore`:
```
/target
```

- [ ] **Step 3: Create `bridge-core` manifest**

`app/bridge-core/Cargo.toml`:
```toml
[package]
name = "bridge-core"
version.workspace = true
edition.workspace = true
publish.workspace = true

[dependencies]
tokio.workspace = true
serde.workspace = true
serde_json.workspace = true
anyhow.workspace = true
reqwest.workspace = true
rust_socketio.workspace = true
tokio-tungstenite.workspace = true
futures-util.workspace = true
mdns-sd.workspace = true
image.workspace = true
tracing.workspace = true
```

- [ ] **Step 4: Write the failing test for config**

`app/bridge-core/src/config.rs`:
```rust
//! Single source of truth for bridge settings (ports config.js).
//! Env overrides let you point at a non-default host/port without editing code.
use std::path::PathBuf;

pub const APP_ID: &str = "ytmboard";
pub const APP_NAME: &str = "YT Music board";
pub const APP_VERSION: &str = "1.0.0";
pub const COVER_PX: u32 = 172;

pub fn ytmd_host() -> String {
    std::env::var("YTMD_HOST").unwrap_or_else(|_| "127.0.0.1".into())
}
pub fn ytmd_port() -> u16 {
    std::env::var("YTMD_PORT").ok().and_then(|s| s.parse().ok()).unwrap_or(9863)
}
pub fn board_port() -> u16 {
    std::env::var("BOARD_PORT").ok().and_then(|s| s.parse().ok()).unwrap_or(8765)
}
pub fn volume_step() -> u16 {
    std::env::var("VOLUME_STEP").ok().and_then(|s| s.parse().ok()).unwrap_or(5)
}

pub fn ytmd_base() -> String {
    format!("http://{}:{}/api/v1", ytmd_host(), ytmd_port())
}
pub fn ytmd_realtime_base() -> String {
    format!("http://{}:{}", ytmd_host(), ytmd_port())
}
pub const fn ytmd_realtime_namespace() -> &'static str {
    "/api/v1/realtime"
}

/// `~/.ytmboard/token.json` — survives restarts so the ~30s Allow prompt
/// happens once. `~` resolves to %USERPROFILE% on Windows, $HOME elsewhere.
pub fn token_path() -> PathBuf {
    let home = std::env::var("YTMBOARD_TOKEN_PATH");
    if let Ok(explicit) = home {
        return PathBuf::from(explicit);
    }
    let base = std::env::var("USERPROFILE")
        .or_else(|_| std::env::var("HOME"))
        .unwrap_or_else(|_| ".".into());
    PathBuf::from(base).join(".ytmboard").join("token.json")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn base_urls_are_ipv4_and_versioned() {
        assert_eq!(ytmd_base(), "http://127.0.0.1:9863/api/v1");
        assert_eq!(ytmd_realtime_base(), "http://127.0.0.1:9863");
        assert_eq!(ytmd_realtime_namespace(), "/api/v1/realtime");
    }

    #[test]
    fn token_path_ends_with_dotdir() {
        let p = token_path();
        assert!(p.ends_with(".ytmboard/token.json") || p.ends_with(r".ytmboard\token.json"));
    }
}
```

- [ ] **Step 5: Create the crate root**

`app/bridge-core/src/lib.rs`:
```rust
//! Native-Rust port of the ytmdesktop → board bridge (was bridge/src/*.js).
pub mod config;
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `PATH="$HOME/.cargo/bin:/c/msys64/mingw64/bin:$PATH" cargo test -p bridge-core config`
Expected: PASS (2 tests). First build resolves the crate tree.

- [ ] **Step 7: Commit**

```bash
git add app/Cargo.toml app/rust-toolchain.toml app/.gitignore app/bridge-core
git commit -m "feat(bridge-core): workspace scaffold + config module"
```

---

## Task 2: `normalize` (pure logic, TDD)

**Files:**
- Create: `app/bridge-core/src/normalize.rs`
- Modify: `app/bridge-core/src/lib.rs` (add `pub mod normalize;`)
- Test: inline in `normalize.rs`

**Interfaces:**
- Consumes: nothing (takes `&serde_json::Value`).
- Produces:
  - `pub enum Playback { Paused, Playing, Buffering }` — `Serialize` as `"paused"|"playing"|"buffering"`.
  - `pub struct NowPlayingVm { ... }` with the exact snake_case keys from Global Constraints, `Serialize + Clone + Debug`.
  - `pub fn normalize(state: &serde_json::Value, connected: bool) -> Option<NowPlayingVm>`.

- [ ] **Step 1: Write the failing tests**

`app/bridge-core/src/normalize.rs`:
```rust
//! Map ytmdesktop /state -> board view-model (ports normalize.js).
//! Cover art is NOT in this JSON: the board protocol sends it as a separate
//! binary frame; `cover_url` is only a hint the orchestrator renders from.
use serde::Serialize;
use serde_json::Value;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum Playback {
    Paused,
    Playing,
    Buffering,
}

#[derive(Debug, Clone, Serialize)]
pub struct NowPlayingVm {
    pub source_name: String,
    pub is_live: bool,
    pub track_id: String,
    pub title: String,
    pub artist: String,
    pub album: String,
    pub ad_playing: bool,
    pub cover_url: Option<String>,
    pub playback: Playback,
    pub is_favorite: bool,
    pub position_sec: i64,
    pub duration_sec: i64,
    pub level: i64,
    pub host_connected: bool,
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn skips_when_metadata_not_filled_and_not_ad() {
        let state = json!({ "video": { "metadataFilled": false }, "player": {} });
        assert!(normalize(&state, true).is_none());
    }

    #[test]
    fn ad_frame_is_not_gated_on_metadata() {
        let state = json!({
            "video": { "metadataFilled": false },
            "player": { "adPlaying": true, "trackState": 1 }
        });
        let vm = normalize(&state, true).expect("ad frame should pass");
        assert!(vm.ad_playing);
        assert_eq!(vm.playback, Playback::Playing);
    }

    #[test]
    fn maps_core_fields_and_playback() {
        let state = json!({
            "video": {
                "id": "abc123",
                "title": "Creep",
                "author": "Radiohead",
                "album": "Pablo Honey",
                "durationSeconds": 238,
                "isLive": false,
                "likeStatus": 2,
                "thumbnails": [
                    { "url": "small", "width": 60, "height": 60 },
                    { "url": "big", "width": 544, "height": 544 }
                ]
            },
            "player": { "trackState": 1, "videoProgress": 30.7 }
        });
        let vm = normalize(&state, true).unwrap();
        assert_eq!(vm.track_id, "abc123");
        assert_eq!(vm.title, "Creep");
        assert_eq!(vm.artist, "Radiohead");
        assert_eq!(vm.album, "Pablo Honey");
        assert_eq!(vm.playback, Playback::Playing);
        assert!(vm.is_favorite);
        assert_eq!(vm.position_sec, 31); // rounded
        assert_eq!(vm.duration_sec, 238);
        assert_eq!(vm.cover_url.as_deref(), Some("big")); // largest thumbnail
        assert_eq!(vm.level, 0);
        assert!(vm.host_connected);
    }

    #[test]
    fn live_zeroes_duration() {
        let state = json!({
            "video": { "title": "x", "author": "y", "isLive": true, "durationSeconds": 999 },
            "player": { "trackState": 1 }
        });
        let vm = normalize(&state, true).unwrap();
        assert!(vm.is_live);
        assert_eq!(vm.duration_sec, 0);
    }

    #[test]
    fn unknown_trackstate_falls_back_to_buffering() {
        let state = json!({
            "video": { "title": "x", "author": "y" },
            "player": { "trackState": -1 }
        });
        assert_eq!(normalize(&state, true).unwrap().playback, Playback::Buffering);
    }

    #[test]
    fn serializes_playback_as_string() {
        let s = serde_json::to_string(&Playback::Buffering).unwrap();
        assert_eq!(s, "\"buffering\"");
    }
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `PATH="$HOME/.cargo/bin:/c/msys64/mingw64/bin:$PATH" cargo test -p bridge-core normalize`
Expected: FAIL — `cannot find function 'normalize'`.

- [ ] **Step 3: Implement `normalize` (append above the `#[cfg(test)]` block)**

```rust
fn best_thumbnail(thumbs: &Value) -> Option<String> {
    let arr = thumbs.as_array()?;
    arr.iter()
        .max_by_key(|t| {
            let w = t.get("width").and_then(Value::as_i64).unwrap_or(0);
            let h = t.get("height").and_then(Value::as_i64).unwrap_or(0);
            w * h
        })
        .and_then(|t| t.get("url").and_then(Value::as_str))
        .map(str::to_owned)
}

fn playback_from(track_state: i64) -> Playback {
    match track_state {
        0 => Playback::Paused,
        1 => Playback::Playing,
        2 => Playback::Buffering,
        _ => Playback::Buffering, // -1 unknown -> buffering
    }
}

/// Returns `None` to mean "skip this update" — used while the player has no
/// metadata yet, so the board doesn't flicker partial data.
pub fn normalize(state: &Value, connected: bool) -> Option<NowPlayingVm> {
    let video = state.get("video").cloned().unwrap_or(Value::Null);
    let player = state.get("player").cloned().unwrap_or(Value::Null);

    let ad_playing = player.get("adPlaying").and_then(Value::as_bool).unwrap_or(false);

    // Wait for complete metadata before showing a track — but ads have no
    // metadata by design, so don't gate ad frames on it.
    let metadata_filled = video.get("metadataFilled").and_then(Value::as_bool);
    if !ad_playing && metadata_filled == Some(false) {
        return None;
    }

    let is_live = video.get("isLive").and_then(Value::as_bool).unwrap_or(false);
    let track_state = player.get("trackState").and_then(Value::as_i64).unwrap_or(-1);
    let progress = player.get("videoProgress").and_then(Value::as_f64).unwrap_or(0.0);
    let duration = video.get("durationSeconds").and_then(Value::as_f64).unwrap_or(0.0);

    let s = |v: &Value, k: &str| v.get(k).and_then(Value::as_str).unwrap_or("").to_owned();

    Some(NowPlayingVm {
        source_name: "YouTube Music".into(),
        is_live,
        track_id: s(&video, "id"),
        title: s(&video, "title"),
        artist: s(&video, "author"),
        album: s(&video, "album"),
        ad_playing,
        cover_url: best_thumbnail(video.get("thumbnails").unwrap_or(&Value::Null)),
        playback: playback_from(track_state),
        is_favorite: video.get("likeStatus").and_then(Value::as_i64) == Some(2),
        position_sec: progress.round() as i64,
        duration_sec: if is_live { 0 } else { duration.round() as i64 },
        level: 0, // ytmdesktop exposes no audio energy; ring falls back to pulse
        host_connected: connected,
    })
}
```

- [ ] **Step 4: Add module to lib.rs**

In `app/bridge-core/src/lib.rs` add: `pub mod normalize;`

- [ ] **Step 5: Run to verify it passes**

Run: `PATH="$HOME/.cargo/bin:/c/msys64/mingw64/bin:$PATH" cargo test -p bridge-core normalize`
Expected: PASS (6 tests).

- [ ] **Step 6: Commit**

```bash
git add app/bridge-core/src/normalize.rs app/bridge-core/src/lib.rs
git commit -m "feat(bridge-core): normalize /state -> NowPlayingVm"
```

---

## Task 3: `cover` — RGB565 encode (TDD) + fetch/resize

**Files:**
- Create: `app/bridge-core/src/cover.rs`
- Modify: `app/bridge-core/src/lib.rs` (add `pub mod cover;`)
- Test: inline in `cover.rs`

**Interfaces:**
- Consumes: `config::COVER_PX`.
- Produces:
  - `pub const COVER_HEADER_BYTES: usize = 8;`
  - `pub fn encode_rgb565(rgb: &[u8], width: u16, height: u16) -> Vec<u8>` — `rgb` is tightly-packed RGB8 (3 bytes/px).
  - `pub async fn render_cover(url: &str) -> Option<Vec<u8>>`.

- [ ] **Step 1: Write the failing test for the encoder**

`app/bridge-core/src/cover.rs`:
```rust
//! Fetch + resize + RGB565-encode cover art (ports cover.js). The board can't
//! decode JPEG/PNG or resize, so we do it and emit a binary frame it blits
//! straight into an lv_image_dsc_t.
//!
//! Frame layout (LE): "YC"(2) | ver=1(u8) | fmt=0(u8) | w(u16) | h(u16) | pixels(w*h*2 RGB565 LE)
use crate::config::COVER_PX;

pub const COVER_HEADER_BYTES: usize = 8;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn header_is_well_formed() {
        // 1x1 pure red: r=255,g=0,b=0 -> 0xF800
        let frame = encode_rgb565(&[255, 0, 0], 1, 1);
        assert_eq!(&frame[0..2], b"YC");
        assert_eq!(frame[2], 1); // version
        assert_eq!(frame[3], 0); // format RGB565 LE
        assert_eq!(u16::from_le_bytes([frame[4], frame[5]]), 1); // width
        assert_eq!(u16::from_le_bytes([frame[6], frame[7]]), 1); // height
        assert_eq!(frame.len(), COVER_HEADER_BYTES + 1 * 1 * 2);
        // pixel: 0xF800 little-endian -> [0x00, 0xF8]
        assert_eq!(frame[8], 0x00);
        assert_eq!(frame[9], 0xF8);
    }

    #[test]
    fn packs_green_and_blue_channels() {
        // pure green 0x07E0 -> LE [0xE0,0x07]; pure blue 0x001F -> LE [0x1F,0x00]
        let frame = encode_rgb565(&[0, 255, 0, 0, 0, 255], 2, 1);
        assert_eq!(&frame[8..10], &[0xE0, 0x07]);
        assert_eq!(&frame[10..12], &[0x1F, 0x00]);
    }

    #[test]
    fn length_matches_dimensions() {
        let px = 4 * 4;
        let frame = encode_rgb565(&vec![0u8; px * 3], 4, 4);
        assert_eq!(frame.len(), COVER_HEADER_BYTES + px * 2);
    }
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `PATH="$HOME/.cargo/bin:/c/msys64/mingw64/bin:$PATH" cargo test -p bridge-core cover`
Expected: FAIL — `cannot find function 'encode_rgb565'`.

- [ ] **Step 3: Implement the encoder (insert before the test module)**

```rust
pub fn encode_rgb565(rgb: &[u8], width: u16, height: u16) -> Vec<u8> {
    let px = width as usize * height as usize;
    let mut out = Vec::with_capacity(COVER_HEADER_BYTES + px * 2);
    out.extend_from_slice(b"YC");
    out.push(1); // version
    out.push(0); // format: RGB565 LE
    out.extend_from_slice(&width.to_le_bytes());
    out.extend_from_slice(&height.to_le_bytes());
    for chunk in rgb.chunks_exact(3) {
        let (r, g, b) = (chunk[0], chunk[1], chunk[2]);
        let v: u16 = (((r as u16) & 0xF8) << 8) | (((g as u16) & 0xFC) << 3) | ((b as u16) >> 3);
        out.extend_from_slice(&v.to_le_bytes());
    }
    out
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `PATH="$HOME/.cargo/bin:/c/msys64/mingw64/bin:$PATH" cargo test -p bridge-core cover`
Expected: PASS (3 tests).

- [ ] **Step 5: Implement `render_cover` (append after the encoder)**

```rust
/// Fetch `url`, square-crop to COVER_PX (cover-fit, centre), return the RGB565
/// frame. Returns `None` on any failure (board keeps its gradient placeholder).
pub async fn render_cover(url: &str) -> Option<Vec<u8>> {
    if url.is_empty() {
        return None;
    }
    match render_cover_inner(url).await {
        Ok(frame) => Some(frame),
        Err(e) => {
            tracing::error!("[cover] render failed ({url}): {e}");
            None
        }
    }
}

async fn render_cover_inner(url: &str) -> anyhow::Result<Vec<u8>> {
    let bytes = reqwest::get(url).await?.error_for_status()?.bytes().await?;
    let img = image::load_from_memory(&bytes)?;
    // cover-fit centre crop to a COVER_PX square, then drop alpha -> RGB8.
    let filled = img.resize_to_fill(COVER_PX, COVER_PX, image::imageops::FilterType::Lanczos3);
    let rgb = filled.to_rgb8();
    Ok(encode_rgb565(rgb.as_raw(), COVER_PX as u16, COVER_PX as u16))
}
```

- [ ] **Step 6: Add module to lib.rs and re-check the build**

In `app/bridge-core/src/lib.rs` add: `pub mod cover;`

Run: `PATH="$HOME/.cargo/bin:/c/msys64/mingw64/bin:$PATH" cargo test -p bridge-core cover`
Expected: PASS (3 tests; `render_cover` is exercised end-to-end later against a real URL).

- [ ] **Step 7: Commit**

```bash
git add app/bridge-core/src/cover.rs app/bridge-core/src/lib.rs
git commit -m "feat(bridge-core): RGB565 cover encode + native image resize"
```

---

## Task 4: `commands` — board→ytmd mapping (pure logic, TDD)

**Files:**
- Create: `app/bridge-core/src/commands.rs`
- Modify: `app/bridge-core/src/lib.rs` (add `pub mod commands;`)
- Test: inline in `commands.rs`

**Interfaces:**
- Consumes: `normalize::NowPlayingVm` (for seek clamping — reads `duration_sec`).
- Produces:
  - `pub struct YtmdCommand { pub command: &'static str, pub data: Option<serde_json::Value> }`
  - `pub fn map_command(cmd: &str, arg: Option<f64>, last_vm: Option<&NowPlayingVm>) -> Option<YtmdCommand>`

- [ ] **Step 1: Write the failing tests**

`app/bridge-core/src/commands.rs`:
```rust
//! Board command -> ytmdesktop command mapping (ports index.js mapCommand).
use crate::normalize::NowPlayingVm;
use serde_json::{json, Value};

#[derive(Debug, Clone, PartialEq)]
pub struct YtmdCommand {
    pub command: &'static str,
    pub data: Option<Value>,
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::normalize::Playback;

    fn vm(duration: i64) -> NowPlayingVm {
        NowPlayingVm {
            source_name: "YouTube Music".into(), is_live: false, track_id: String::new(),
            title: String::new(), artist: String::new(), album: String::new(), ad_playing: false,
            cover_url: None, playback: Playback::Playing, is_favorite: false,
            position_sec: 0, duration_sec: duration, level: 0, host_connected: true,
        }
    }

    #[test]
    fn simple_commands_map_by_name() {
        assert_eq!(map_command("toggle_play", None, None).unwrap().command, "playPause");
        assert_eq!(map_command("next", None, None).unwrap().command, "next");
        assert_eq!(map_command("prev", None, None).unwrap().command, "previous");
        assert_eq!(map_command("toggle_favorite", None, None).unwrap().command, "toggleLike");
        assert_eq!(map_command("volume_up", None, None).unwrap().command, "volumeUp");
        assert_eq!(map_command("volume_down", None, None).unwrap().command, "volumeDown");
    }

    #[test]
    fn seek_clamps_to_duration() {
        let m = map_command("seek", Some(500.0), Some(&vm(238))).unwrap();
        assert_eq!(m.command, "seekTo");
        assert_eq!(m.data, Some(json!(238)));
    }

    #[test]
    fn seek_floors_at_zero() {
        let m = map_command("seek", Some(-5.0), Some(&vm(238))).unwrap();
        assert_eq!(m.data, Some(json!(0)));
    }

    #[test]
    fn seek_without_duration_passes_through_nonnegative() {
        let m = map_command("seek", Some(42.0), None).unwrap();
        assert_eq!(m.data, Some(json!(42)));
    }

    #[test]
    fn unknown_command_is_none() {
        assert!(map_command("launch_rockets", None, None).is_none());
    }
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `PATH="$HOME/.cargo/bin:/c/msys64/mingw64/bin:$PATH" cargo test -p bridge-core commands`
Expected: FAIL — `cannot find function 'map_command'`.

- [ ] **Step 3: Implement `map_command` (insert before the test module)**

```rust
pub fn map_command(cmd: &str, arg: Option<f64>, last_vm: Option<&NowPlayingVm>) -> Option<YtmdCommand> {
    let simple = |command| Some(YtmdCommand { command, data: None });
    match cmd {
        "toggle_play" => simple("playPause"),
        "next" => simple("next"),
        "prev" => simple("previous"),
        "toggle_favorite" => simple("toggleLike"),
        "volume_up" => simple("volumeUp"),
        "volume_down" => simple("volumeDown"),
        "seek" => {
            let raw = arg.unwrap_or(0.0);
            let dur = last_vm.map(|v| v.duration_sec).unwrap_or(0);
            let sec = if dur > 0 {
                raw.max(0.0).min(dur as f64)
            } else {
                raw.max(0.0)
            };
            Some(YtmdCommand { command: "seekTo", data: Some(json!(sec as i64)) })
        }
        _ => None,
    }
}
```

- [ ] **Step 4: Add module to lib.rs**

In `app/bridge-core/src/lib.rs` add: `pub mod commands;`

- [ ] **Step 5: Run to verify it passes**

Run: `PATH="$HOME/.cargo/bin:/c/msys64/mingw64/bin:$PATH" cargo test -p bridge-core commands`
Expected: PASS (5 tests).

- [ ] **Step 6: Commit**

```bash
git add app/bridge-core/src/commands.rs app/bridge-core/src/lib.rs
git commit -m "feat(bridge-core): board->ytmd command mapping"
```

---

## Task 5: `state` — bridge state machine (pure logic, TDD)

**Files:**
- Create: `app/bridge-core/src/state.rs`
- Modify: `app/bridge-core/src/lib.rs` (add `pub mod state;`)
- Test: inline in `state.rs`

**Interfaces:**
- Produces:
  - `pub enum BridgeState { Starting, YtmdNotFound, NotAuthorized, WaitingForBoard, BoardConnected, YtmdDisconnected }` — `Serialize` (rename_all kebab-case), `Clone/Copy/PartialEq/Debug`.
  - `pub enum Signal { YtmdProbeFailed, ServerUpNoToken, Authorized, BoardAttached, BoardDetached, YtmdDropped }`
  - `pub fn next_state(cur: BridgeState, sig: Signal) -> BridgeState`

- [ ] **Step 1: Write the failing tests**

`app/bridge-core/src/state.rs`:
```rust
//! The single source of truth the UI renders. Ports the state diagram in the
//! design doc: starting -> ytmd-not-found -> not-authorized -> waiting-for-board
//! -> board-connected, with ytmd-disconnected as a recoverable fallback.
use serde::Serialize;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "kebab-case")]
pub enum BridgeState {
    Starting,
    YtmdNotFound,
    NotAuthorized,
    WaitingForBoard,
    BoardConnected,
    YtmdDisconnected,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Signal {
    YtmdProbeFailed,
    ServerUpNoToken,
    Authorized,
    BoardAttached,
    BoardDetached,
    YtmdDropped,
}

#[cfg(test)]
mod tests {
    use super::{BridgeState::*, Signal::*, *};

    #[test]
    fn happy_path() {
        assert_eq!(next_state(Starting, ServerUpNoToken), NotAuthorized);
        assert_eq!(next_state(NotAuthorized, Authorized), WaitingForBoard);
        assert_eq!(next_state(WaitingForBoard, BoardAttached), BoardConnected);
    }

    #[test]
    fn probe_fail_goes_not_found_from_any_pre_auth_state() {
        assert_eq!(next_state(Starting, YtmdProbeFailed), YtmdNotFound);
        assert_eq!(next_state(NotAuthorized, YtmdProbeFailed), YtmdNotFound);
    }

    #[test]
    fn board_detach_returns_to_waiting() {
        assert_eq!(next_state(BoardConnected, BoardDetached), WaitingForBoard);
    }

    #[test]
    fn ytmd_drop_is_recoverable_fallback() {
        assert_eq!(next_state(BoardConnected, YtmdDropped), YtmdDisconnected);
        assert_eq!(next_state(WaitingForBoard, YtmdDropped), YtmdDisconnected);
        // recovery: server comes back authorized -> resume waiting-for-board
        assert_eq!(next_state(YtmdDisconnected, Authorized), WaitingForBoard);
    }

    #[test]
    fn serializes_kebab_case() {
        assert_eq!(serde_json::to_string(&YtmdNotFound).unwrap(), "\"ytmd-not-found\"");
        assert_eq!(serde_json::to_string(&WaitingForBoard).unwrap(), "\"waiting-for-board\"");
    }
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `PATH="$HOME/.cargo/bin:/c/msys64/mingw64/bin:$PATH" cargo test -p bridge-core state`
Expected: FAIL — `cannot find function 'next_state'`.

- [ ] **Step 3: Implement `next_state` (insert before the test module)**

```rust
pub fn next_state(cur: BridgeState, sig: Signal) -> BridgeState {
    use BridgeState::*;
    use Signal::*;
    match sig {
        // ytmd server unreachable takes priority from any pre-connected state.
        YtmdProbeFailed => YtmdNotFound,
        ServerUpNoToken => NotAuthorized,
        // Authorized resumes the pipeline whether starting fresh or recovering.
        Authorized => WaitingForBoard,
        BoardAttached => BoardConnected,
        BoardDetached => WaitingForBoard,
        // Server vanished mid-run: fall back so the UI shows disconnected while
        // the client retries. Preserve BoardConnected-vs-Waiting is irrelevant —
        // both collapse to the recoverable fallback.
        YtmdDropped => {
            let _ = cur;
            YtmdDisconnected
        }
    }
}
```

- [ ] **Step 4: Add module to lib.rs**

In `app/bridge-core/src/lib.rs` add: `pub mod state;`

- [ ] **Step 5: Run to verify it passes**

Run: `PATH="$HOME/.cargo/bin:/c/msys64/mingw64/bin:$PATH" cargo test -p bridge-core state`
Expected: PASS (5 tests).

- [ ] **Step 6: Commit**

```bash
git add app/bridge-core/src/state.rs app/bridge-core/src/lib.rs
git commit -m "feat(bridge-core): bridge state machine"
```

---

## Task 6: `auth` — Companion handshake + token persistence

**Files:**
- Create: `app/bridge-core/src/auth.rs`
- Modify: `app/bridge-core/src/lib.rs` (add `pub mod auth;`)
- Reference: `spike/ytmd-rust/src/main.rs` (proven handshake), `bridge/src/auth.js`

**Interfaces:**
- Consumes: `config::{APP_ID, APP_NAME, APP_VERSION, ytmd_base, token_path}`.
- Produces:
  - `pub async fn load_token() -> Option<String>`
  - `pub async fn save_token(token: &str) -> anyhow::Result<()>`
  - `pub async fn request_token(http: &reqwest::Client, on_code: impl Fn(&str)) -> anyhow::Result<String>` — calls `on_code(code)` after `requestcode` so callers can surface it (CLI prints; Tauri notifies).
  - `pub async fn ensure_token(http: &reqwest::Client, on_code: impl Fn(&str)) -> anyhow::Result<String>`

- [ ] **Step 1: Implement `auth` (ported from the spike; async reqwest)**

`app/bridge-core/src/auth.rs`:
```rust
//! Companion Server auth handshake + token persistence (ports auth.js).
//! Flow: POST /auth/requestcode -> {code}; user clicks Allow; POST /auth/request
//! -> {token}. Authenticated requests send `Authorization: <token>` (raw).
use crate::config::{token_path, ytmd_base, APP_ID, APP_NAME, APP_VERSION};
use anyhow::{anyhow, Context, Result};
use serde_json::{json, Value};

pub async fn load_token() -> Option<String> {
    let raw = tokio::fs::read_to_string(token_path()).await.ok()?;
    let v: Value = serde_json::from_str(&raw).ok()?;
    // Token is bound to appId; ignore a token saved under a different identity.
    if v.get("appId")?.as_str()? == APP_ID {
        return v.get("token")?.as_str().map(str::to_owned);
    }
    None
}

pub async fn save_token(token: &str) -> Result<()> {
    let path = token_path();
    if let Some(dir) = path.parent() {
        tokio::fs::create_dir_all(dir).await.ok();
    }
    let body = json!({ "appId": APP_ID, "token": token });
    tokio::fs::write(&path, serde_json::to_string_pretty(&body)?)
        .await
        .with_context(|| format!("writing token to {}", path.display()))?;
    Ok(())
}

async fn post_json(http: &reqwest::Client, path: &str, body: Value) -> Result<Value> {
    let url = format!("{}{}", ytmd_base(), path);
    Ok(http
        .post(&url)
        .json(&body)
        .send()
        .await
        .with_context(|| format!("POST {path}"))?
        .error_for_status()?
        .json()
        .await?)
}

/// Runs the full interactive handshake and returns a fresh token. `on_code` is
/// invoked with the code so the caller can surface it (print / notify).
pub async fn request_token(http: &reqwest::Client, on_code: impl Fn(&str)) -> Result<String> {
    let res = post_json(
        http,
        "/auth/requestcode",
        json!({ "appId": APP_ID, "appName": APP_NAME, "appVersion": APP_VERSION }),
    )
    .await
    .context("is ytmdesktop running with Companion Server AND authorization enabled?")?;
    let code = res
        .get("code")
        .and_then(Value::as_str)
        .ok_or_else(|| anyhow!("no `code` in requestcode response"))?;

    on_code(code);

    let res = post_json(http, "/auth/request", json!({ "appId": APP_ID, "code": code })).await?;
    let token = res
        .get("token")
        .and_then(Value::as_str)
        .ok_or_else(|| anyhow!("no `token` in request response"))?
        .to_owned();

    save_token(&token).await?;
    Ok(token)
}

/// Cached token if present, else run the handshake.
pub async fn ensure_token(http: &reqwest::Client, on_code: impl Fn(&str)) -> Result<String> {
    match load_token().await {
        Some(t) => Ok(t),
        None => request_token(http, on_code).await,
    }
}
```

- [ ] **Step 2: Add module to lib.rs and verify it compiles**

In `app/bridge-core/src/lib.rs` add: `pub mod auth;`

Run: `PATH="$HOME/.cargo/bin:/c/msys64/mingw64/bin:$PATH" cargo build -p bridge-core`
Expected: compiles clean (auth is I/O; exercised end-to-end by the CLI in Task 11).

- [ ] **Step 3: Commit**

```bash
git add app/bridge-core/src/auth.rs app/bridge-core/src/lib.rs
git commit -m "feat(bridge-core): Companion auth handshake + token persistence"
```

---

## Task 7: `ytmd` — realtime Socket.IO client + REST

**Files:**
- Create: `app/bridge-core/src/ytmd.rs`
- Modify: `app/bridge-core/src/lib.rs` (add `pub mod ytmd;`)
- Reference: `spike/ytmd-rust/src/main.rs`, `bridge/src/ytmd.js`

**Interfaces:**
- Consumes: `config::{ytmd_base, ytmd_realtime_base, ytmd_realtime_namespace}`, `commands::YtmdCommand`.
- Produces:
  - `pub struct YtmdHandle { socket: rust_socketio::asynchronous::Client, http: reqwest::Client, token: String }`
  - `pub async fn connect(http: reqwest::Client, token: String, on_state: mpsc::UnboundedSender<Value>, on_connect: mpsc::UnboundedSender<()>, on_disconnect: mpsc::UnboundedSender<()>) -> anyhow::Result<YtmdHandle>` — websocket-only, auth token; wires `state-update`, connect, disconnect to the given senders.
  - `impl YtmdHandle { pub async fn fetch_state_once(&self) -> Option<Value>; pub async fn send_command(&self, c: &YtmdCommand) -> anyhow::Result<()>; pub async fn disconnect(self); }`

- [ ] **Step 1: Implement `ytmd` (async socket.io + REST)**

`app/bridge-core/src/ytmd.rs`:
```rust
//! Client for the ytmdesktop Companion Server (ports ytmd.js).
//! Realtime via Socket.IO (websocket-only), token in the auth object; the
//! `state-update` event carries the full /state object. REST /state seeds once
//! on connect (socket only pushes on change); /command drives control actions.
use crate::commands::YtmdCommand;
use crate::config::{ytmd_base, ytmd_realtime_base, ytmd_realtime_namespace};
use anyhow::{Context, Result};
use futures_util::FutureExt;
use rust_socketio::asynchronous::{Client, ClientBuilder};
use rust_socketio::{Event, Payload, TransportType};
use serde_json::{json, Value};
use tokio::sync::mpsc::UnboundedSender;

pub struct YtmdHandle {
    socket: Client,
    http: reqwest::Client,
    token: String,
}

pub async fn connect(
    http: reqwest::Client,
    token: String,
    on_state: UnboundedSender<Value>,
    on_connect: UnboundedSender<()>,
    on_disconnect: UnboundedSender<()>,
) -> Result<YtmdHandle> {
    let state_tx = on_state.clone();
    let state_cb = move |payload: Payload, _: Client| {
        let tx = state_tx.clone();
        async move {
            if let Payload::Text(values) = payload {
                if let Some(state) = values.into_iter().next() {
                    let _ = tx.send(state);
                }
            }
        }
        .boxed()
    };

    let conn_tx = on_connect.clone();
    let connect_cb = move |_: Payload, _: Client| {
        let tx = conn_tx.clone();
        async move { let _ = tx.send(()); }.boxed()
    };

    let dis_tx = on_disconnect.clone();
    let close_cb = move |_: Payload, _: Client| {
        let tx = dis_tx.clone();
        async move { let _ = tx.send(()); }.boxed()
    };

    let socket = ClientBuilder::new(ytmd_realtime_base())
        .namespace(ytmd_realtime_namespace())
        .auth(json!({ "token": token }))
        .transport_type(TransportType::Websocket)
        .on("state-update", state_cb)
        .on(Event::Connect, connect_cb)
        .on(Event::Close, close_cb)
        .on(Event::Error, |err, _| async move { tracing::error!("[ytmd] socket error: {err:?}"); }.boxed())
        .connect()
        .await
        .context("Socket.IO connect failed")?;

    Ok(YtmdHandle { socket, http, token })
}

impl YtmdHandle {
    /// One-shot GET /state so a paused/idle player still paints for a board that
    /// just (re)connected. Failures are non-fatal.
    pub async fn fetch_state_once(&self) -> Option<Value> {
        let url = format!("{}/state", ytmd_base());
        match self
            .http
            .get(&url)
            .header("Authorization", &self.token) // raw token, no "Bearer"
            .send()
            .await
            .and_then(|r| r.error_for_status())
        {
            Ok(r) => r.json().await.ok(),
            Err(e) => {
                tracing::warn!("[ytmd] initial /state fetch failed: {e}");
                None
            }
        }
    }

    pub async fn send_command(&self, c: &YtmdCommand) -> Result<()> {
        let url = format!("{}/command", ytmd_base());
        let body = match &c.data {
            Some(d) => json!({ "command": c.command, "data": d }),
            None => json!({ "command": c.command }),
        };
        self.http
            .post(&url)
            .header("Authorization", &self.token)
            .json(&body)
            .send()
            .await?
            .error_for_status()
            .with_context(|| format!("command {}", c.command))?;
        Ok(())
    }

    pub async fn disconnect(self) {
        let _ = self.socket.disconnect().await;
    }
}
```

- [ ] **Step 2: Add module to lib.rs and verify it compiles**

In `app/bridge-core/src/lib.rs` add: `pub mod ytmd;`

Run: `PATH="$HOME/.cargo/bin:/c/msys64/mingw64/bin:$PATH" cargo build -p bridge-core`
Expected: compiles clean.

- [ ] **Step 3: Commit**

```bash
git add app/bridge-core/src/ytmd.rs app/bridge-core/src/lib.rs
git commit -m "feat(bridge-core): ytmd realtime socket.io client + REST"
```

---

## Task 8: `board_server` — plain-WS server for the board

**Files:**
- Create: `app/bridge-core/src/board_server.rs`
- Modify: `app/bridge-core/src/lib.rs` (add `pub mod board_server;`)
- Reference: `bridge/src/board-server.js`

**Interfaces:**
- Consumes: `config::board_port`, `normalize::NowPlayingVm`.
- Produces:
  - `pub enum BoardOutbound { State(NowPlayingVm), Cover(Vec<u8>) }`
  - `pub struct BoardServer { outbound_tx: broadcast::Sender<BoardOutbound>, latest: Arc<Mutex<Snapshot>>, cmd_tx: mpsc::UnboundedSender<(String, Option<f64>)> }`
  - `pub fn state_frame(vm: &NowPlayingVm) -> String` → `{"type":"state","data":<vm>}`
  - `pub async fn start(cmd_tx: mpsc::UnboundedSender<(String, Option<f64>)>) -> anyhow::Result<BoardServer>`
  - `impl BoardServer { pub fn broadcast_state(&self, vm: NowPlayingVm); pub fn broadcast_cover(&self, frame: Option<Vec<u8>>); }`
  - `client_count` semantics surfaced via a `board_connected_tx`/`board_disconnected_tx` pair passed into `start` (mirrors the state machine's BoardAttached/BoardDetached signals).

- [ ] **Step 1: Write the failing test for the frame helper**

`app/bridge-core/src/board_server.rs`:
```rust
//! Board-facing protocol (ports board-server.js). Plain WebSocket so the board
//! needs no Socket.IO / auth / TLS. On connect: latest state snapshot, then the
//! latest cover frame. Thereafter a fresh frame on every change. Heartbeat ping
//! every 15s; drop a client that missed the previous pong.
use crate::normalize::NowPlayingVm;
use serde_json::json;

pub fn state_frame(vm: &NowPlayingVm) -> String {
    json!({ "type": "state", "data": vm }).to_string()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::normalize::Playback;

    #[test]
    fn state_frame_wraps_vm_under_type_and_data() {
        let vm = NowPlayingVm {
            source_name: "YouTube Music".into(), is_live: false, track_id: "id".into(),
            title: "T".into(), artist: "A".into(), album: "Al".into(), ad_playing: false,
            cover_url: None, playback: Playback::Playing, is_favorite: false,
            position_sec: 1, duration_sec: 2, level: 0, host_connected: true,
        };
        let v: serde_json::Value = serde_json::from_str(&state_frame(&vm)).unwrap();
        assert_eq!(v["type"], "state");
        assert_eq!(v["data"]["title"], "T");
        assert_eq!(v["data"]["playback"], "playing");
        assert_eq!(v["data"]["host_connected"], true);
    }
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `PATH="$HOME/.cargo/bin:/c/msys64/mingw64/bin:$PATH" cargo test -p bridge-core board_server`
Expected: FAIL — module/type not found (`NowPlayingVm` import unused until impl) — the test won't compile because `board_server` isn't in lib yet.

- [ ] **Step 3: Implement the server (insert before the test module)**

```rust
use crate::config::board_port;
use anyhow::Result;
use futures_util::{SinkExt, StreamExt};
use std::sync::Arc;
use tokio::net::{TcpListener, TcpStream};
use tokio::sync::{broadcast, mpsc, Mutex};
use tokio::time::{interval, Duration};
use tokio_tungstenite::tungstenite::Message;

const HEARTBEAT: Duration = Duration::from_secs(15);

#[derive(Clone)]
pub enum BoardOutbound {
    State(NowPlayingVm),
    Cover(Vec<u8>),
}

#[derive(Default, Clone)]
struct Snapshot {
    state: Option<NowPlayingVm>,
    cover: Option<Vec<u8>>,
}

pub struct BoardServer {
    outbound_tx: broadcast::Sender<BoardOutbound>,
    latest: Arc<Mutex<Snapshot>>,
}

pub async fn start(
    cmd_tx: mpsc::UnboundedSender<(String, Option<f64>)>,
    board_connected_tx: mpsc::UnboundedSender<()>,
    board_disconnected_tx: mpsc::UnboundedSender<()>,
) -> Result<BoardServer> {
    let (outbound_tx, _) = broadcast::channel::<BoardOutbound>(32);
    let latest = Arc::new(Mutex::new(Snapshot::default()));

    let listener = TcpListener::bind(("0.0.0.0", board_port())).await?;
    tracing::info!("[board] WebSocket server on ws://0.0.0.0:{}", board_port());

    let accept_tx = outbound_tx.clone();
    let accept_latest = latest.clone();
    tokio::spawn(async move {
        loop {
            let Ok((stream, _)) = listener.accept().await else { continue };
            let sub = accept_tx.subscribe();
            let snap = accept_latest.clone();
            let cmd = cmd_tx.clone();
            let conn = board_connected_tx.clone();
            let disc = board_disconnected_tx.clone();
            tokio::spawn(async move {
                let _ = conn.send(());
                if let Err(e) = handle_client(stream, sub, snap, cmd).await {
                    tracing::debug!("[board] client ended: {e}");
                }
                let _ = disc.send(());
            });
        }
    });

    Ok(BoardServer { outbound_tx, latest })
}

async fn handle_client(
    stream: TcpStream,
    mut sub: broadcast::Receiver<BoardOutbound>,
    latest: Arc<Mutex<Snapshot>>,
    cmd_tx: mpsc::UnboundedSender<(String, Option<f64>)>,
) -> Result<()> {
    let ws = tokio_tungstenite::accept_async(stream).await?;
    let (mut tx, mut rx) = ws.split();
    tracing::info!("[board] client connected");

    // Snapshot on connect so a freshly-attached board paints immediately.
    {
        let snap = latest.lock().await.clone();
        if let Some(vm) = snap.state {
            tx.send(Message::Text(state_frame(&vm))).await?;
        }
        if let Some(cover) = snap.cover {
            tx.send(Message::Binary(cover)).await?;
        }
    }

    let mut hb = interval(HEARTBEAT);
    let mut awaiting_pong = false;

    loop {
        tokio::select! {
            out = sub.recv() => match out {
                Ok(BoardOutbound::State(vm)) => tx.send(Message::Text(state_frame(&vm))).await?,
                Ok(BoardOutbound::Cover(frame)) => tx.send(Message::Binary(frame)).await?,
                Err(broadcast::error::RecvError::Lagged(_)) => continue,
                Err(broadcast::error::RecvError::Closed) => break,
            },
            msg = rx.next() => match msg {
                Some(Ok(Message::Text(raw))) => handle_board_msg(&raw, &cmd_tx),
                Some(Ok(Message::Pong(_))) => awaiting_pong = false,
                Some(Ok(Message::Close(_))) | None => break,
                Some(Err(_)) => break,
                _ => {}
            },
            _ = hb.tick() => {
                if awaiting_pong {
                    tracing::info!("[board] client missed pong; dropping");
                    break;
                }
                awaiting_pong = true;
                tx.send(Message::Ping(Vec::new())).await?;
            }
        }
    }
    tracing::info!("[board] client disconnected");
    Ok(())
}

fn handle_board_msg(raw: &str, cmd_tx: &mpsc::UnboundedSender<(String, Option<f64>)>) {
    let Ok(v) = serde_json::from_str::<serde_json::Value>(raw) else {
        tracing::warn!("[board] dropped non-JSON message");
        return;
    };
    let Some(cmd) = v.get("cmd").and_then(|c| c.as_str()) else {
        tracing::warn!("[board] dropped message without cmd");
        return;
    };
    let arg = v.get("arg").and_then(|a| a.as_f64());
    let _ = cmd_tx.send((cmd.to_owned(), arg));
}

impl BoardServer {
    pub fn broadcast_state(&self, vm: NowPlayingVm) {
        // Remember for late joiners, then fan out to connected boards.
        if let Ok(mut snap) = self.latest.try_lock() {
            snap.state = Some(vm.clone());
        }
        let _ = self.outbound_tx.send(BoardOutbound::State(vm));
    }

    pub fn broadcast_cover(&self, frame: Option<Vec<u8>>) {
        if let Ok(mut snap) = self.latest.try_lock() {
            snap.cover = frame.clone();
        }
        if let Some(f) = frame {
            let _ = self.outbound_tx.send(BoardOutbound::Cover(f));
        }
    }
}
```

> Note: `broadcast_state`/`broadcast_cover` use `try_lock` for the snapshot to stay non-async (callable from sync contexts). Under contention the snapshot update is skipped for that tick; the fan-out still fires. If the reviewer prefers guaranteed snapshot updates, make these `async` and `.lock().await` — the orchestrator (Task 10) already calls them from an async task, so either is fine.

- [ ] **Step 4: Add module to lib.rs**

In `app/bridge-core/src/lib.rs` add: `pub mod board_server;`

- [ ] **Step 5: Run to verify the frame test passes**

Run: `PATH="$HOME/.cargo/bin:/c/msys64/mingw64/bin:$PATH" cargo test -p bridge-core board_server`
Expected: PASS (1 test). Server accept loop is exercised end-to-end by the CLI + a `wscat` client in Task 11.

- [ ] **Step 6: Commit**

```bash
git add app/bridge-core/src/board_server.rs app/bridge-core/src/lib.rs
git commit -m "feat(bridge-core): board-facing plain-WS server"
```

---

## Task 9: `discovery` — mDNS advertisement

**Files:**
- Create: `app/bridge-core/src/discovery.rs`
- Modify: `app/bridge-core/src/lib.rs` (add `pub mod discovery;`)
- Reference: `spike/ytmd-rust/src/main.rs`, `bridge/src/discovery.js`

**Interfaces:**
- Consumes: `config::board_port`.
- Produces:
  - `pub struct Discovery { daemon: mdns_sd::ServiceDaemon }`
  - `pub fn start() -> anyhow::Result<Discovery>` — advertises `_ytmboard._tcp` on the board port with TXT `proto=ws`,`path=/`,`v=1`.
  - `impl Discovery { pub fn stop(self); }`

- [ ] **Step 1: Implement `discovery` (ported from the spike)**

`app/bridge-core/src/discovery.rs`:
```rust
//! mDNS advertisement so the board finds the host (ports discovery.js).
//! Advertises `_ytmboard._tcp` on the board port; the board browses for this
//! service and connects to the advertised host:port — no hard-coded IP.
use crate::config::board_port;
use anyhow::{Context, Result};
use mdns_sd::{ServiceDaemon, ServiceInfo};

const SERVICE_TYPE: &str = "_ytmboard._tcp.local.";
const SERVICE_NAME: &str = "YT Music board bridge";

pub struct Discovery {
    daemon: ServiceDaemon,
}

pub fn start() -> Result<Discovery> {
    let daemon = ServiceDaemon::new().context("mdns ServiceDaemon::new")?;
    let service = ServiceInfo::new(
        SERVICE_TYPE,
        SERVICE_NAME,
        "ytmboard.local.",
        "",
        board_port(),
        &[("proto", "ws"), ("path", "/"), ("v", "1")][..],
    )
    .context("mdns ServiceInfo::new")?
    .enable_addr_auto();
    daemon.register(service).context("mdns register")?;
    tracing::info!("[mdns] advertising {SERVICE_TYPE} on :{}", board_port());
    Ok(Discovery { daemon })
}

impl Discovery {
    pub fn stop(self) {
        // Send goodbye packets so boards drop us promptly, then shut down.
        let _ = self.daemon.unregister(&format!("{SERVICE_NAME}.{SERVICE_TYPE}"));
        let _ = self.daemon.shutdown();
    }
}
```

- [ ] **Step 2: Add module to lib.rs and verify it compiles**

In `app/bridge-core/src/lib.rs` add: `pub mod discovery;`

Run: `PATH="$HOME/.cargo/bin:/c/msys64/mingw64/bin:$PATH" cargo build -p bridge-core`
Expected: compiles clean.

- [ ] **Step 3: Commit**

```bash
git add app/bridge-core/src/discovery.rs app/bridge-core/src/lib.rs
git commit -m "feat(bridge-core): mDNS _ytmboard._tcp advertisement"
```

---

## Task 10: `bridge` — orchestrator + `BridgeEvent` API

**Files:**
- Create: `app/bridge-core/src/bridge.rs`
- Modify: `app/bridge-core/src/lib.rs` (add `pub mod bridge;` and re-exports)
- Reference: `bridge/src/index.js` (wiring, debounce, cover dedupe)

**Interfaces:**
- Consumes: every module above.
- Produces (the API Part 2 consumes):
  - `pub enum BridgeEvent { State(state::BridgeState), AuthCode { code: String }, NowPlaying(normalize::NowPlayingVm), Log(String) }` — `Clone`.
  - `pub struct Bridge;`
  - `pub async fn run(events: tokio::sync::mpsc::UnboundedSender<BridgeEvent>) -> anyhow::Result<()>` — runs until the process is torn down; drives the whole pipeline and emits `BridgeEvent`s.
  - `lib.rs` re-exports: `pub use bridge::{run, BridgeEvent};` `pub use state::BridgeState;` `pub use normalize::{NowPlayingVm, Playback};`

- [ ] **Step 1: Implement the orchestrator**

`app/bridge-core/src/bridge.rs`:
```rust
//! Wires ytmdesktop -> normalize -> board (ports index.js), emitting a single
//! BridgeEvent stream both the CLI and the Tauri shell consume.
use crate::{auth, board_server, commands, cover, discovery, normalize, state, ytmd};
use crate::state::{BridgeState, Signal};
use anyhow::Result;
use serde_json::Value;
use std::sync::Arc;
use tokio::sync::{mpsc, Mutex};
use tokio::time::{sleep, Duration};

#[derive(Debug, Clone)]
pub enum BridgeEvent {
    State(BridgeState),
    AuthCode { code: String },
    NowPlaying(normalize::NowPlayingVm),
    Log(String),
}

pub async fn run(events: mpsc::UnboundedSender<BridgeEvent>) -> Result<()> {
    let http = reqwest::Client::builder()
        .timeout(Duration::from_secs(60)) // > the ~30s Allow window
        .build()?;

    let emit = |e: BridgeEvent| { let _ = events.send(e); };
    let set_state = |s: BridgeState| { let _ = events.send(BridgeEvent::State(s)); };

    set_state(BridgeState::Starting);

    // Board server + command handling.
    let (cmd_tx, mut cmd_rx) = mpsc::unbounded_channel::<(String, Option<f64>)>();
    let (board_conn_tx, mut board_conn_rx) = mpsc::unbounded_channel::<()>();
    let (board_disc_tx, mut board_disc_rx) = mpsc::unbounded_channel::<()>();
    let board = Arc::new(board_server::start(cmd_tx, board_conn_tx, board_disc_tx).await?);

    // mDNS advertisement (kept alive for the process lifetime).
    let _discovery = discovery::start()?;

    // Auth (surfaces the code via BridgeEvent + moves to NotAuthorized first).
    set_state(BridgeState::NotAuthorized);
    let ev_for_code = events.clone();
    let token = auth::ensure_token(&http, |code| {
        let _ = ev_for_code.send(BridgeEvent::AuthCode { code: code.to_owned() });
    })
    .await?;

    // Wire ytmd realtime.
    let (state_tx, mut state_rx) = mpsc::unbounded_channel::<Value>();
    let (yconn_tx, mut yconn_rx) = mpsc::unbounded_channel::<()>();
    let (ydisc_tx, mut ydisc_rx) = mpsc::unbounded_channel::<()>();
    let ytmd = Arc::new(
        ytmd::connect(http.clone(), token.clone(), state_tx, yconn_tx, ydisc_tx).await?,
    );

    set_state(BridgeState::WaitingForBoard);

    // Shared last-vm for seek clamping + cover dedupe.
    let last_vm = Arc::new(Mutex::new(None::<normalize::NowPlayingVm>));
    let last_cover_url = Arc::new(Mutex::new(None::<String>));

    // Command handler task.
    {
        let ytmd = ytmd.clone();
        let last_vm = last_vm.clone();
        let events = events.clone();
        tokio::spawn(async move {
            while let Some((cmd, arg)) = cmd_rx.recv().await {
                let vm = last_vm.lock().await.clone();
                match commands::map_command(&cmd, arg, vm.as_ref()) {
                    Some(mapped) => {
                        if let Err(e) = ytmd.send_command(&mapped).await {
                            let _ = events.send(BridgeEvent::Log(format!("[cmd] {cmd} failed: {e}")));
                        } else {
                            let _ = events.send(BridgeEvent::Log(format!("[cmd] {cmd} -> {}", mapped.command)));
                        }
                    }
                    None => { let _ = events.send(BridgeEvent::Log(format!("[cmd] unknown board command: {cmd}"))); }
                }
            }
        });
    }

    // Seed state on (re)connect.
    {
        let ytmd = ytmd.clone();
        let state_tx2 = state_rx_sender(&state_rx); // see helper note below
        let _ = state_tx2; // placeholder; seeding handled in the select loop instead
    }

    // Debounced state pipeline.
    let mut last_state: Option<Value> = None;
    let mut debounce: Option<tokio::task::JoinHandle<()>> = None;
    let (flush_tx, mut flush_rx) = mpsc::unbounded_channel::<()>();

    loop {
        tokio::select! {
            Some(raw) = state_rx.recv() => {
                last_state = Some(raw);
                if let Some(h) = debounce.take() { h.abort(); }
                let flush = flush_tx.clone();
                debounce = Some(tokio::spawn(async move {
                    sleep(Duration::from_millis(120)).await;
                    let _ = flush.send(());
                }));
            }
            Some(()) = flush_rx.recv() => {
                if let Some(ref st) = last_state {
                    push_vm(st, true, &board, &last_vm, &last_cover_url, &emit).await;
                }
            }
            Some(()) = yconn_rx.recv() => {
                emit(BridgeEvent::Log("[ytmd] socket connected".into()));
                if let Some(st) = ytmd.fetch_state_once().await {
                    last_state = Some(st.clone());
                    push_vm(&st, true, &board, &last_vm, &last_cover_url, &emit).await;
                }
            }
            Some(()) = ydisc_rx.recv() => {
                emit(BridgeEvent::Log("[ytmd] socket disconnected".into()));
                set_state(state::next_state(BridgeState::WaitingForBoard, Signal::YtmdDropped));
                // Tell the board the host went away.
                let vm = last_vm.lock().await.clone();
                if let Some(mut v) = vm { v.host_connected = false; board.broadcast_state(v); }
            }
            Some(()) = board_conn_rx.recv() => {
                set_state(state::next_state(BridgeState::WaitingForBoard, Signal::BoardAttached));
            }
            Some(()) = board_disc_rx.recv() => {
                set_state(state::next_state(BridgeState::BoardConnected, Signal::BoardDetached));
            }
            else => break,
        }
    }

    let _ = ytmd; let _ = board;
    Ok(())
}

// Render + push the vm (and cover, deduped by URL) to boards + emit NowPlaying.
async fn push_vm(
    raw: &Value,
    connected: bool,
    board: &Arc<board_server::BoardServer>,
    last_vm: &Arc<Mutex<Option<normalize::NowPlayingVm>>>,
    last_cover_url: &Arc<Mutex<Option<String>>>,
    emit: &impl Fn(BridgeEvent),
) {
    let Some(vm) = normalize::normalize(raw, connected) else { return };
    *last_vm.lock().await = Some(vm.clone());
    board.broadcast_state(vm.clone());
    emit(BridgeEvent::NowPlaying(vm.clone()));

    // Cover: only re-render when the URL changes; skip ads (stale/no art).
    if vm.ad_playing { return; }
    let url = vm.cover_url.clone();
    let mut guard = last_cover_url.lock().await;
    if *guard == url { return; }
    *guard = url.clone();
    drop(guard);
    match url {
        None => board.broadcast_cover(None),
        Some(u) => {
            let board = board.clone();
            let last_cover_url = last_cover_url.clone();
            tokio::spawn(async move {
                if let Some(frame) = cover::render_cover(&u).await {
                    // Only push if still current (a newer track may have superseded).
                    if last_cover_url.lock().await.as_deref() == Some(u.as_str()) {
                        board.broadcast_cover(Some(frame));
                    }
                }
            });
        }
    }
}
```

> **Implementation note for the executor:** the `state_rx_sender` placeholder line above is a dead stub — delete it; seeding is handled inside the `yconn_rx` arm of the select loop. It's called out here so you don't wire a second seeding path. Keep the select loop as the single consumer of `state_rx`.

- [ ] **Step 2: Wire re-exports in lib.rs**

Append to `app/bridge-core/src/lib.rs`:
```rust
pub mod bridge;

pub use bridge::{run, BridgeEvent};
pub use normalize::{NowPlayingVm, Playback};
pub use state::BridgeState;
```

- [ ] **Step 3: Verify the crate compiles**

Run: `PATH="$HOME/.cargo/bin:/c/msys64/mingw64/bin:$PATH" cargo build -p bridge-core`
Expected: compiles clean. Fix any borrow/lifetime issues the compiler flags (the executor iterates here — this is the one module with real integration complexity).

- [ ] **Step 4: Run the full unit suite**

Run: `PATH="$HOME/.cargo/bin:/c/msys64/mingw64/bin:$PATH" cargo test -p bridge-core`
Expected: PASS (all prior unit tests: config 2, normalize 6, cover 3, commands 5, state 5, board_server 1).

- [ ] **Step 5: Commit**

```bash
git add app/bridge-core/src
git commit -m "feat(bridge-core): orchestrator + BridgeEvent API"
```

---

## Task 11: `bridge-cli` — thin CLI (npm-start parity) + end-to-end verify

**Files:**
- Create: `app/bridge-cli/Cargo.toml`, `app/bridge-cli/src/main.rs`
- Verify against real ytmdesktop + a `wscat` board stand-in.

**Interfaces:**
- Consumes: `bridge_core::{run, BridgeEvent, BridgeState}`.
- Produces: a runnable binary `bridge-cli`.

- [ ] **Step 1: Create the CLI manifest**

`app/bridge-cli/Cargo.toml`:
```toml
[package]
name = "bridge-cli"
version.workspace = true
edition.workspace = true
publish.workspace = true

[dependencies]
bridge-core = { path = "../bridge-core" }
tokio.workspace = true
anyhow.workspace = true
tracing-subscriber.workspace = true
```

- [ ] **Step 2: Write the CLI**

`app/bridge-cli/src/main.rs`:
```rust
//! Thin CLI: run the bridge and print its event stream. Developer parity with
//! the old `npm start` — the same lifecycle, now native Rust.
use bridge_core::{run, BridgeEvent};
use tokio::sync::mpsc;

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    tracing_subscriber::fmt().with_target(false).init();

    let (tx, mut rx) = mpsc::unbounded_channel::<BridgeEvent>();

    let printer = tokio::spawn(async move {
        while let Some(ev) = rx.recv().await {
            match ev {
                BridgeEvent::State(s) => println!("[state] {}", serde_json::to_string(&s).unwrap()),
                BridgeEvent::AuthCode { code } => {
                    println!("\n>>> ytmdesktop is asking you to allow \"YT Music board\".");
                    println!(">>> Code: {code}");
                    println!(">>> Click ALLOW in the app within ~30 seconds...\n");
                }
                BridgeEvent::NowPlaying(vm) => {
                    println!("[now-playing] {:?}  {} — {}  @{}s", vm.playback, vm.artist, vm.title, vm.position_sec);
                }
                BridgeEvent::Log(msg) => println!("{msg}"),
            }
        }
    });

    // Ctrl-C shuts the process (and thus the bridge) down.
    tokio::select! {
        r = run(tx) => r?,
        _ = tokio::signal::ctrl_c() => println!("\nShutting down..."),
    }
    printer.abort();
    Ok(())
}
```

- [ ] **Step 3: Build the whole workspace**

Run: `PATH="$HOME/.cargo/bin:/c/msys64/mingw64/bin:$PATH" cargo build`
Expected: both crates compile.

- [ ] **Step 4: End-to-end verify against ytmdesktop (human-in-the-loop)**

Preconditions: ytmdesktop running, Companion Server **and** "enable companion authorization" on, music playing. Delete any stale spike token so this exercises the real path if desired: token lives at `~/.ytmboard/token.json`.

Run: `PATH="$HOME/.cargo/bin:/c/msys64/mingw64/bin:$PATH" cargo run -p bridge-cli`
Expected, in order:
- `[state] "starting"` → `[state] "not-authorized"`
- `>>> Code: XXXX` → click Allow → `[state] "waiting-for-board"`
- `[now-playing] Playing <artist> — <title> @Ns` lines that change as you skip tracks.

- [ ] **Step 5: Verify the board WS contract with a stand-in client**

In a second shell (Node is available):
```bash
npx wscat -c ws://127.0.0.1:8765
```
Expected: on connect, a `{"type":"state","data":{...}}` frame arrives immediately (snapshot); skipping tracks in YouTube Music pushes fresh frames. The CLI prints `[state] "board-connected"` when wscat attaches. Send a command to confirm the reverse path:
```
> {"cmd":"toggle_play"}
```
Expected: playback toggles in ytmdesktop; CLI prints `[cmd] toggle_play -> playPause`.

- [ ] **Step 6: Verify mDNS advertisement**

```bash
npx dns-sd -B _ytmboard._tcp     # or: dns-sd on macOS / Bonjour browser
```
Expected: `YT Music board bridge` listed. (The board itself resolves this in the field.)

- [ ] **Step 7: Commit**

```bash
git add app/bridge-cli
git commit -m "feat(bridge-cli): thin CLI over bridge-core (npm-start parity)"
```

---

## Task 12: Cover art end-to-end + README

**Files:**
- Create: `app/README.md`
- Verify cover frames reach a board stand-in.

**Interfaces:** none new.

- [ ] **Step 1: Verify a cover binary frame is emitted**

With the CLI running and a real track playing, reconnect `wscat` (binary frames print as hex/length). Confirm a binary frame arrives after the text state frame on a track change, and that it starts with `59 43` (`"YC"`), version `01`, format `00`, and width/height `AC 00 AC 00` (172 = 0x00AC, LE). If `wscat` can't show binary bytes cleanly, add a 6-line throwaway Node `ws` client that logs `buf.subarray(0,8)` — delete after verifying.

Expected: header `59 43 01 00 AC 00 AC 00`, total length `8 + 172*172*2 = 59176` bytes.

- [ ] **Step 2: Write the app README**

`app/README.md`:
```markdown
# ytmboard bridge (native Rust)

Branch T of the standalone bridge (see
`docs/superpowers/specs/2026-07-08-standalone-bridge-app-design.md`). A native
Rust port of the Node bridge: ytmdesktop Companion Server → `now_playing_vm` →
board, plus `_ytmboard._tcp` mDNS and RGB565 cover art.

## Layout
- `bridge-core/` — the library (auth, ytmd, normalize, cover, board server, mDNS,
  orchestrator). Host-testable pure logic + async I/O.
- `bridge-cli/` — thin CLI; developer parity with the old `npm start`.
- `src-tauri/`, `ui/` — the tray app (Part 2).

## Build & run (Windows)
The default MSVC toolchain fails to link here; use the GNU toolchain + MinGW gcc.
`rust-toolchain.toml` pins gnu.

```sh
PATH="$HOME/.cargo/bin:/c/msys64/mingw64/bin:$PATH" cargo run -p bridge-cli
```

Requires ytmdesktop with Companion Server **and** "enable companion
authorization" on. First run prints a code — click **Allow** within ~30s. The
token is cached at `~/.ytmboard/token.json`.

## Test
```sh
PATH="$HOME/.cargo/bin:/c/msys64/mingw64/bin:$PATH" cargo test -p bridge-core
```
```

- [ ] **Step 3: Commit**

```bash
git add app/README.md
git commit -m "docs(app): bridge-core + CLI README"
```

---

## Self-Review

**1. Spec coverage** (against the design doc's "Stack-independent core" + "Branch T"):
- State machine → Task 5 (`state.rs`), driven in Task 10. ✓
- ytmd presence probe / auth code + ~30s window → Task 6 (auth), surfaced as `BridgeEvent::AuthCode` (Task 10); the countdown UI is Part 2. ✓ (core)
- Bridge behavior/contracts: auth+realtime → Tasks 6–7; normalize → Task 2; board WS `:8765` → Task 8; mDNS `_ytmboard._tcp` → Task 9; 172×172 RGB565 cover matching `COVER_PX` → Task 3. ✓
- Branch T "native cover resize (the sharp/jimp question disappears)" → Task 3 uses the `image` crate. ✓
- Tray/window/notifications/login-item, packaging/CI, landing page → **Part 2** (explicitly out of scope here). Flagged, not dropped.

**2. Placeholder scan:** The only non-literal spots are the two annotated notes in Tasks 8 and 10 (try_lock vs async lock; the `state_rx_sender` dead stub to delete). Both are explicit executor instructions with the resolution given, not TBDs. No "add error handling"/"write tests for the above" placeholders — every code step shows real code.

**3. Type consistency:** `NowPlayingVm`/`Playback` defined in Task 2, consumed by name in Tasks 4, 8, 10. `YtmdCommand{command,data}` defined Task 4, consumed Task 7 (`send_command`) and Task 10. `BridgeState`/`Signal`/`next_state` defined Task 5, consumed Task 10. `BridgeEvent` defined Task 10, consumed Task 11. `BoardServer::{broadcast_state,broadcast_cover}` defined Task 8, consumed Task 10. `state_frame` defined Task 8, tested Task 8. Config accessors defined Task 1, consumed throughout. Consistent.

---

## Execution Handoff

Part 1 is the foundation; Part 2 (Tauri shell + CI matrix + landing page) is written after this lands, against the finalized `BridgeEvent` API.
