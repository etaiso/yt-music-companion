# Bridge re-auth / self-heal (native-Rust) — design

**Date:** 2026-07-09
**Scope:** `app/bridge-core/` (branch `etais/standalone-bridge-app-design-311a40`)
**Origin:** Final-review finding **I4** — the Rust bridge port omitted the re-auth /
self-heal behavior the Node bridge (`bridge/src/ytmd.js`) had. This is a **Part-1**
follow-up; **Part 2 (Tauri)** consumes the same `BridgeEvent` stream, so the public
API shape must stay stable.

## Problem

The Node reference did two things the Rust port lacks:

1. On a socket `connect_error` whose message matches `/auth|token|unauthor/i`, it
   re-ran `requestToken()` and set `socket.auth = { token }` so socket.io retried
   the connection with a fresh token.
2. On a `401` from `POST /command`, it re-ran the handshake once and retried the
   command.

In the Rust port today:

- `ytmd.rs` `Event::Error` handler only logs.
- `send_command` returns the error with no 401 retry.
- `YtmdHandle.token` is an immutable `String`; there is no way to swap it live.

**Consequence:** a revoked/expired token means all commands fail (and/or the socket
never authorizes), with no self-heal until the user manually deletes
`~/.ytmboard/token.json`.

## Key library fact (`rust_socketio` 0.6.0)

- `ClientBuilder::on_reconnect(FnMut() -> BoxFuture<'static, ReconnectSettings>)` is
  invoked before each reconnect attempt. The returned `ReconnectSettings` can set a
  new `auth` (`ReconnectSettings::auth(serde_json::Value)`); if it does **not**, the
  client keeps the existing auth. This is the idiomatic analog of the Node
  `socket.auth = { token }` swap.
- The reconnect loop (`poll_stream`) runs whenever the packet stream ends (transport
  close). A socket.io auth rejection arrives as a `ConnectError` frame → fires the
  `Event::Error` callback → the server then closes the transport → stream ends →
  reconnect loop runs → `on_reconnect` fires.
- `ClientBuilder::connect()` returns `Ok` once the **transport** connects; the
  socket.io-layer auth rejection arrives afterward as a frame. So the reconnect +
  `on_reconnect` mechanism **also self-heals the startup revoked-token case** without
  a hard `Err` from `bridge_core::run()`.

## Design

### 1. Shared token cell

`bridge::run` wraps the token in `Arc<tokio::sync::RwLock<String>>` after
`ensure_token` and passes it into `ytmd::connect`. Both re-auth paths (socket
reconnect, command 401) read/write this one cell, so a refresh from either path is
seen by the other. `auth::request_token` already persists the new token to
`token.json`, so the healed token survives a restart.

### 2. Layering — keep `ytmd.rs` / `auth.rs` UI-agnostic

`ytmd::connect` gains an `on_code: Arc<dyn Fn(&str) + Send + Sync>` parameter — the
same callback shape `bridge.rs` already passes to `auth::ensure_token`. `bridge.rs`
supplies a closure that emits `BridgeEvent::AuthCode`. Neither `ytmd.rs` nor
`auth.rs` learns about `BridgeEvent`.

### 3. One coalescing re-auth helper

A free async fn in `ytmd.rs`:

```rust
async fn reauth(
    http: &reqwest::Client,
    token: &Arc<RwLock<String>>,
    lock: &Arc<Mutex<()>>,
    stale: &str,
    on_code: &Arc<dyn Fn(&str) + Send + Sync>,
) -> Result<String>
```

- Acquire `lock`.
- Read the current token; if it no longer equals `stale`, someone already refreshed
  while we waited → return the current token (no second handshake, **no second
  code**).
- Otherwise `auth::request_token(http, |c| on_code(c)).await`, write the cell, return
  the fresh token.

This coalesces a concurrent socket-reconnect and command-401 into a single handshake.

### 4. Socket auth-failure path

- Add `auth_failed: Arc<AtomicBool>`.
- `Event::Error` handler: keep logging; additionally set `auth_failed = true` when the
  message matches the classifier (see §6).
- `.on_reconnect(closure)`: if `auth_failed` (swap to `false`), call `reauth(...)` and
  return `ReconnectSettings` with `auth = json!({ "token": fresh })`; otherwise return
  `ReconnectSettings::default()` (keeps the existing token, so ordinary network blips
  do **not** re-prompt). Re-auth errors are logged and swallowed (return default);
  the loop retries — matching the Node `catch`.

### 5. Command 401 path

`YtmdHandle` stores `token: Arc<RwLock<String>>`, `on_code`, `http`, and the re-auth
`lock: Arc<Mutex<()>>`. `send_command`:

1. Read current token; `POST /command`.
2. On HTTP `401`: `reauth(...)`, then retry **once** with the fresh token.
3. A second `401` (or other error) returns `Err` as today.

### 6. Auth-error classifier (pure, tested)

Extract the `/auth|token|unauthor/i` decision into a pure fn:

```rust
fn is_auth_error(msg: &str) -> bool // case-insensitive contains any of: auth, token, unauthor
```

Used by the `Event::Error` handler. Kept regex-free (simple lowercase `contains`) to
avoid adding a `regex` dependency; unit-tested against representative messages.

### 7. State / UX (confirmed decision)

**Reuse existing signals only — no new `Signal` or `BridgeState`.** The socket's
natural disconnect → reconnect already flows
`YtmdDropped → YtmdDisconnected → (reconnect) → Authorized → WaitingForBoard`. The
command-401 path emits `BridgeEvent::AuthCode` + a `BridgeEvent::Log`. The fresh code
reaches the CLI/Tauri via the existing `AuthCode` event.

**Public API unchanged:** `BridgeEvent`, `BridgeState`, and `run` keep their shapes.
The only signature change is internal to the crate: `ytmd::connect` gains the
`on_code` parameter and its token param becomes `Arc<RwLock<String>>`.

## Testing (TDD)

Network paths hit a live localhost companion server, so the network itself is not
unit-tested. The two decision points are pure and covered red-green:

1. **`is_auth_error`** — table test: `"auth failed"`, `"invalid token"`,
   `"Unauthorized"`, `"Received an ConnectError frame: ..."` (with an auth-ish body)
   → `true`; `"transport close"`, `"timeout"`, `""` → `false`. Case-insensitivity
   asserted.
2. **`reauth` coalescing** — with a fake token-provider (a closure standing in for
   `request_token`) and a fake `on_code` counter: assert that when the cell already
   differs from `stale`, the provider is **not** called and no code is emitted; when
   it equals `stale`, the provider is called once and the cell is updated. (Factor the
   handshake behind a small provider param so the test needs no HTTP.)

Manual/hardware verification (revoke token mid-session; observe fresh `AuthCode` and
resumed playback) is out of scope for this change's automated tests and tracked
separately.

## Files touched

- `app/bridge-core/src/ytmd.rs` — `connect` signature (`on_code`, token cell),
  `Event::Error` sets `auth_failed`, `on_reconnect` closure, `send_command` 401 retry,
  `reauth` helper, `is_auth_error` classifier + tests.
- `app/bridge-core/src/bridge.rs` — wrap token in `Arc<RwLock<String>>`, pass the
  `on_code` closure into `ytmd::connect`.
- `app/bridge-core/src/auth.rs` — unchanged (already exposes `request_token`).

## Build / verify

```
cd app && PATH="$HOME/.cargo/bin:/c/msys64/mingw64/bin:$PATH" cargo build
cd app && PATH="$HOME/.cargo/bin:/c/msys64/mingw64/bin:$PATH" cargo test -p bridge-core
```
(GNU toolchain — MSVC linking is broken in this environment.)

## Non-goals

- No new `BridgeState`/`Signal` for re-auth.
- No `regex` dependency.
- No change to the `token.json` format or `auth.rs` handshake protocol.
- No retry-count/backoff tuning beyond the library defaults already configured.
