# Bridge re-auth / self-heal Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Port the Node bridge's re-auth / self-heal behavior into the native-Rust `bridge-core` so a revoked/expired token heals automatically (surfacing a fresh `BridgeEvent::AuthCode`) instead of requiring the user to delete `token.json`.

**Architecture:** A single shared token cell (`Arc<RwLock<String>>`) is read/written by two re-auth paths that funnel through one coalescing helper (`reauth`): (1) the socket, via `rust_socketio`'s `on_reconnect` hook gated by an `auth_failed` flag set in the `Event::Error` handler; (2) `send_command`, which retries once on HTTP 401. `ytmd.rs`/`auth.rs` stay UI-agnostic — `bridge.rs` injects an `on_code: Arc<dyn Fn(&str) + Send + Sync>` closure that emits `BridgeEvent::AuthCode`.

**Tech Stack:** Rust 2021, tokio, `rust_socketio` 0.6.0 (async), reqwest, anyhow, serde_json.

## Global Constraints

- Build/test with the GNU toolchain only (MSVC linking is broken here):
  `cd app && PATH="$HOME/.cargo/bin:/c/msys64/mingw64/bin:$PATH" cargo <cmd>`
- No new crate dependencies — in particular **no `regex`** (classifier is a plain lowercase `contains`).
- No change to the public API shape: `BridgeEvent`, `BridgeState`, and `bridge::run` keep their signatures. Only `ytmd::connect`'s signature changes (crate-internal).
- No new `Signal`/`BridgeState` — re-auth reuses the existing `AuthCode` event + `Log` + the natural disconnect→reconnect state flow.
- Auth-error match set (case-insensitive substring, ported verbatim from `bridge/src/ytmd.js` `/auth|token|unauthor/i`): `auth`, `token`, `unauthor`.
- Token is sent as a raw `Authorization` header value (no `Bearer` prefix).
- Work happens in the worktree `C:/Users/Etai/Projects/yt-music-companion/.claude/worktrees/bridge-reauth-selfheal` on branch `etais/bridge-reauth-selfheal`.

---

## File Structure

- `app/bridge-core/src/ytmd.rs` — **modified**. Gains: `is_auth_error` (pure classifier), `reauth` (generic coalescing helper), `auth_failed` flag + `on_reconnect` wiring, `send_command` 401 retry, `YtmdHandle` fields (`token: Arc<RwLock<String>>`, `on_code`, `reauth_lock`), `connect` signature change, unit tests.
- `app/bridge-core/src/bridge.rs` — **modified**. Builds the `on_code` Arc once, reuses it for `ensure_token`, wraps the token in `Arc<RwLock<String>>`, passes both into `ytmd::connect`.
- `app/bridge-core/src/auth.rs` — **unchanged** (already exposes `request_token`, which persists via `save_token`).

Task order: pure units first (Tasks 1–2, verified via `cargo test` so the not-yet-called helpers don't trip a non-test `dead_code` warning), then the wiring that consumes them (Task 3).

---

### Task 1: `is_auth_error` classifier (pure, tested)

**Files:**
- Modify: `app/bridge-core/src/ytmd.rs` (add private fn + extend the existing `#[cfg(test)] mod tests`, or add one if absent)
- Test: same file (`#[cfg(test)] mod tests`)

**Interfaces:**
- Consumes: nothing.
- Produces: `fn is_auth_error(msg: &str) -> bool` — true iff `msg` (lowercased) contains any of `auth`, `token`, `unauthor`. Note `unauthor` is a substring of `unauthorized`/`unauthorised`, and `auth` already covers `unauthor`-with-`auth`; both kept to mirror the JS regex exactly.

- [ ] **Step 1: Write the failing test**

Add at the bottom of `app/bridge-core/src/ytmd.rs`:

```rust
#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn is_auth_error_matches_auth_keywords() {
        assert!(is_auth_error("auth failed"));
        assert!(is_auth_error("invalid token"));
        assert!(is_auth_error("Unauthorized"));
        assert!(is_auth_error("UNAUTHORISED")); // case-insensitive
        assert!(is_auth_error("Received an ConnectError frame: token rejected"));
    }

    #[test]
    fn is_auth_error_ignores_transient_errors() {
        assert!(!is_auth_error("transport close"));
        assert!(!is_auth_error("timeout"));
        assert!(!is_auth_error("connection refused"));
        assert!(!is_auth_error(""));
    }
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd app && PATH="$HOME/.cargo/bin:/c/msys64/mingw64/bin:$PATH" cargo test -p bridge-core is_auth_error`
Expected: FAIL to compile — `cannot find function \`is_auth_error\``.

- [ ] **Step 3: Write minimal implementation**

Add near the top of `app/bridge-core/src/ytmd.rs` (after the imports, before `pub struct YtmdHandle`):

```rust
/// True when a socket/command error message indicates the token was rejected
/// (revoked/expired/overwritten). Ports the JS `/auth|token|unauthor/i` test.
fn is_auth_error(msg: &str) -> bool {
    let m = msg.to_ascii_lowercase();
    ["auth", "token", "unauthor"].iter().any(|kw| m.contains(kw))
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd app && PATH="$HOME/.cargo/bin:/c/msys64/mingw64/bin:$PATH" cargo test -p bridge-core is_auth_error`
Expected: PASS (`test result: ok. 2 passed`).

- [ ] **Step 5: Commit**

```bash
git add app/bridge-core/src/ytmd.rs
git commit -m "feat(bridge-core): add is_auth_error classifier for token-rejection messages"
```

---

### Task 2: `reauth` coalescing helper (tested with injected handshake)

**Files:**
- Modify: `app/bridge-core/src/ytmd.rs` (add generic fn + tests)
- Test: same file

**Interfaces:**
- Consumes: nothing.
- Produces:
  ```rust
  async fn reauth<F, Fut>(
      token: &Arc<RwLock<String>>,
      lock: &Arc<Mutex<()>>,
      stale: &str,
      handshake: F,
  ) -> anyhow::Result<String>
  where
      F: FnOnce() -> Fut,
      Fut: std::future::Future<Output = anyhow::Result<String>>,
  ```
  Holds `lock` for the whole operation; if the cell no longer equals `stale` (a
  concurrent caller already refreshed) returns the current token **without**
  calling `handshake`; otherwise awaits `handshake`, stores the result in the
  cell, and returns it. The `handshake` closure is where the real
  `auth::request_token` (which emits the code and persists the token) is invoked
  in production; injecting it keeps this fn HTTP-free and testable.

- [ ] **Step 1: Add imports**

At the top of `app/bridge-core/src/ytmd.rs`, ensure these imports exist (add the missing ones to the existing `use` block):

```rust
use std::sync::Arc;
use tokio::sync::{Mutex, RwLock};
```

(`std::sync::atomic::{AtomicBool, Ordering}` and `ReconnectSettings` are added in Task 3.)

- [ ] **Step 2: Write the failing test**

Add these two tests inside the existing `#[cfg(test)] mod tests` in `app/bridge-core/src/ytmd.rs`:

```rust
    use std::sync::atomic::{AtomicUsize, Ordering as AtomicOrdering};

    #[tokio::test]
    async fn reauth_runs_handshake_and_updates_cell() {
        let token = Arc::new(RwLock::new("old".to_string()));
        let lock = Arc::new(Mutex::new(()));
        let calls = Arc::new(AtomicUsize::new(0));

        let calls_c = calls.clone();
        let fresh = reauth(&token, &lock, "old", || async move {
            calls_c.fetch_add(1, AtomicOrdering::SeqCst);
            Ok("new".to_string())
        })
        .await
        .unwrap();

        assert_eq!(fresh, "new");
        assert_eq!(*token.read().await, "new");
        assert_eq!(calls.load(AtomicOrdering::SeqCst), 1);
    }

    #[tokio::test]
    async fn reauth_coalesces_when_cell_already_refreshed() {
        // Cell already holds a token newer than the caller's stale value.
        let token = Arc::new(RwLock::new("new".to_string()));
        let lock = Arc::new(Mutex::new(()));
        let calls = Arc::new(AtomicUsize::new(0));

        let calls_c = calls.clone();
        let got = reauth(&token, &lock, "old", || async move {
            calls_c.fetch_add(1, AtomicOrdering::SeqCst);
            Ok("should-not-be-used".to_string())
        })
        .await
        .unwrap();

        assert_eq!(got, "new"); // returned the already-refreshed token
        assert_eq!(calls.load(AtomicOrdering::SeqCst), 0); // handshake skipped
    }
```

- [ ] **Step 3: Run test to verify it fails**

Run: `cd app && PATH="$HOME/.cargo/bin:/c/msys64/mingw64/bin:$PATH" cargo test -p bridge-core reauth`
Expected: FAIL to compile — `cannot find function \`reauth\``.

- [ ] **Step 4: Write minimal implementation**

Add to `app/bridge-core/src/ytmd.rs` (after `is_auth_error`):

```rust
/// Serialized, coalescing token refresh shared by the socket-reconnect and
/// command-401 paths. If another caller refreshed the cell while we waited on
/// `lock`, we adopt their token instead of running a second handshake (so the
/// user only ever sees one auth code). `handshake` performs the real request
/// (surfacing the code + persisting the token) and is injected for testability.
async fn reauth<F, Fut>(
    token: &Arc<RwLock<String>>,
    lock: &Arc<Mutex<()>>,
    stale: &str,
    handshake: F,
) -> Result<String>
where
    F: FnOnce() -> Fut,
    Fut: std::future::Future<Output = Result<String>>,
{
    let _guard = lock.lock().await;
    {
        let current = token.read().await;
        if *current != stale {
            return Ok(current.clone());
        }
    }
    let fresh = handshake().await?;
    *token.write().await = fresh.clone();
    Ok(fresh)
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `cd app && PATH="$HOME/.cargo/bin:/c/msys64/mingw64/bin:$PATH" cargo test -p bridge-core reauth`
Expected: PASS (`2 passed`).

- [ ] **Step 6: Commit**

```bash
git add app/bridge-core/src/ytmd.rs
git commit -m "feat(bridge-core): add coalescing reauth helper (shared token cell)"
```

---

### Task 3: Wire re-auth into the socket + command paths + bridge.rs

**Files:**
- Modify: `app/bridge-core/src/ytmd.rs` (imports, `YtmdHandle` fields, `connect` signature + `Event::Error` + `on_reconnect`, `fetch_state_once`, `send_command`)
- Modify: `app/bridge-core/src/bridge.rs` (build `on_code` Arc, wrap token, pass into `ytmd::connect`)

**Interfaces:**
- Consumes: `is_auth_error` (Task 1), `reauth` (Task 2), `auth::request_token` (existing: `async fn request_token(http: &reqwest::Client, on_code: impl Fn(&str)) -> Result<String>`).
- Produces: new `ytmd::connect` signature —
  ```rust
  pub async fn connect(
      http: reqwest::Client,
      token: Arc<RwLock<String>>,
      on_code: Arc<dyn Fn(&str) + Send + Sync>,
      on_state: UnboundedSender<Value>,
      on_connect: UnboundedSender<()>,
      on_disconnect: UnboundedSender<()>,
  ) -> Result<YtmdHandle>
  ```
  `YtmdHandle` still exposes `fetch_state_once(&self) -> Option<Value>`, `send_command(&self, &YtmdCommand) -> Result<()>`, `disconnect(self)` unchanged.

- [ ] **Step 1: Add remaining imports to `ytmd.rs`**

Update the top-of-file imports so the block includes (merge with existing lines):

```rust
use rust_socketio::asynchronous::{Client, ClientBuilder, ReconnectSettings};
use rust_socketio::{Event, Payload, TransportType};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use tokio::sync::{Mutex, RwLock};
```

(Keep the existing `use crate::...`, `anyhow`, `futures_util::FutureExt`, `serde_json::{json, Value}`, `tokio::sync::mpsc::UnboundedSender` imports.)

- [ ] **Step 2: Replace the `YtmdHandle` struct and `connect` fn**

Replace lines from `pub struct YtmdHandle {` through the end of `connect` (the `Ok(YtmdHandle { socket, http, token })` / closing brace) with:

```rust
pub struct YtmdHandle {
    socket: Client,
    http: reqwest::Client,
    token: Arc<RwLock<String>>,
    on_code: Arc<dyn Fn(&str) + Send + Sync>,
    reauth_lock: Arc<Mutex<()>>,
}

pub async fn connect(
    http: reqwest::Client,
    token: Arc<RwLock<String>>,
    on_code: Arc<dyn Fn(&str) + Send + Sync>,
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
        async move {
            let _ = tx.send(());
        }
        .boxed()
    };

    let dis_tx = on_disconnect.clone();
    let close_cb = move |_: Payload, _: Client| {
        let tx = dis_tx.clone();
        async move {
            let _ = tx.send(());
        }
        .boxed()
    };

    // Set by the Error handler when the socket rejects our token; read (and
    // cleared) by on_reconnect so only auth failures trigger a re-handshake —
    // ordinary network blips reconnect with the existing token.
    let auth_failed = Arc::new(AtomicBool::new(false));
    let reauth_lock = Arc::new(Mutex::new(()));

    let err_flag = auth_failed.clone();
    let error_cb = move |payload: Payload, _: Client| {
        let flag = err_flag.clone();
        async move {
            let msg = match &payload {
                Payload::Text(values) => values
                    .iter()
                    .map(|v| v.as_str().map(str::to_owned).unwrap_or_else(|| v.to_string()))
                    .collect::<Vec<_>>()
                    .join(" "),
                #[allow(deprecated)]
                Payload::String(s) => s.clone(),
                Payload::Binary(_) => String::new(),
            };
            tracing::error!("[ytmd] socket error: {msg}");
            if is_auth_error(&msg) {
                flag.store(true, Ordering::SeqCst);
            }
        }
        .boxed()
    };

    // on_reconnect fires before every reconnect attempt. Re-handshake only when
    // an auth failure was flagged; otherwise return default settings (keeps the
    // current token). Mirrors JS: swap socket.auth = { token } on auth error.
    let rc_http = http.clone();
    let rc_token = token.clone();
    let rc_lock = reauth_lock.clone();
    let rc_on_code = on_code.clone();
    let rc_flag = auth_failed.clone();
    let reconnect_cb = move || {
        let http = rc_http.clone();
        let token = rc_token.clone();
        let lock = rc_lock.clone();
        let on_code = rc_on_code.clone();
        let flag = rc_flag.clone();
        async move {
            let mut settings = ReconnectSettings::new();
            if flag.swap(false, Ordering::SeqCst) {
                let stale = token.read().await.clone();
                match reauth(&token, &lock, &stale, || {
                    auth::request_token(&http, |c| on_code(c))
                })
                .await
                {
                    Ok(fresh) => settings.auth(json!({ "token": fresh })),
                    Err(e) => tracing::error!("[ytmd] re-auth failed: {e}"),
                }
            }
            settings
        }
        .boxed()
    };

    let initial_token = token.read().await.clone();
    let socket = ClientBuilder::new(ytmd_realtime_base())
        .namespace(ytmd_realtime_namespace())
        .auth(json!({ "token": initial_token }))
        .transport_type(TransportType::Websocket)
        // Mirror the JS client (bridge/src/ytmd.js): reconnection:true,
        // reconnectionDelay:1000, reconnectionDelayMax:5000. reconnect_on_disconnect
        // ensures a SERVER-initiated disconnect also triggers reconnection.
        .reconnect(true)
        .reconnect_on_disconnect(true)
        .reconnect_delay(1000, 5000)
        .on("state-update", state_cb)
        .on(Event::Connect, connect_cb)
        .on(Event::Close, close_cb)
        .on(Event::Error, error_cb)
        .on_reconnect(reconnect_cb)
        .connect()
        .await
        .context("Socket.IO connect failed")?;

    Ok(YtmdHandle { socket, http, token, on_code, reauth_lock })
}
```

Note the `auth` import: `use crate::{...}` in `ytmd.rs` does not currently import `auth`. Add `use crate::auth;` to the imports (the crate root exposes it via `bridge-core/src/lib.rs`), OR call it as `crate::auth::request_token`. Use `crate::auth::request_token` inline to avoid touching the import list further.

So in `reconnect_cb`, write `crate::auth::request_token(&http, |c| on_code(c))`.

- [ ] **Step 3: Update `fetch_state_once` to read the token cell**

Replace the `.header("Authorization", &self.token)` line in `fetch_state_once` with a read of the cell. Change the method body's header line to use a local:

```rust
    pub async fn fetch_state_once(&self) -> Option<Value> {
        let url = format!("{}/state", ytmd_base());
        let token = self.token.read().await.clone();
        match self
            .http
            .get(&url)
            .header("Authorization", &token) // raw token, no "Bearer"
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
```

- [ ] **Step 4: Replace `send_command` with a 401-retrying version**

Replace the whole `send_command` method with:

```rust
    pub async fn send_command(&self, c: &YtmdCommand) -> Result<()> {
        let url = format!("{}/command", ytmd_base());
        let body = match &c.data {
            Some(d) => json!({ "command": c.command, "data": d }),
            None => json!({ "command": c.command }),
        };

        let token = self.token.read().await.clone();
        let res = self.post_command(&url, &body, &token).await?;
        if res.status() == reqwest::StatusCode::UNAUTHORIZED {
            // Token died mid-session — re-auth once and retry (ports ytmd.js).
            let on_code = self.on_code.clone();
            let fresh = reauth(&self.token, &self.reauth_lock, &token, || {
                crate::auth::request_token(&self.http, move |c| on_code(c))
            })
            .await?;
            self.post_command(&url, &body, &fresh)
                .await?
                .error_for_status()
                .with_context(|| format!("command {} (after re-auth)", c.command))?;
            return Ok(());
        }
        res.error_for_status()
            .with_context(|| format!("command {}", c.command))?;
        Ok(())
    }

    async fn post_command(
        &self,
        url: &str,
        body: &Value,
        token: &str,
    ) -> reqwest::Result<reqwest::Response> {
        self.http
            .post(url)
            .header("Authorization", token)
            .json(body)
            .send()
            .await
    }
```

(`disconnect(self)` is unchanged.)

- [ ] **Step 5: Update `bridge.rs` to build `on_code`, wrap the token, and pass both in**

In `app/bridge-core/src/bridge.rs`, add `use std::sync::Arc;` and `use tokio::sync::RwLock;` to the imports (the file already has `use tokio::sync::{mpsc, Mutex};` — extend it to `use tokio::sync::{mpsc, Mutex, RwLock};` and add `use std::sync::Arc;` near the top).

Replace the auth + ytmd-wire block (currently lines ~45–58, from the `// Auth (...)` comment through the `let ytmd = Arc::new(ytmd::connect(...))` line) with:

```rust
    // Auth (surfaces the code via BridgeEvent + moves to NotAuthorized first).
    cur = BridgeState::NotAuthorized;
    set_state(cur);
    // One code-surfacing closure, reused for the initial handshake and every
    // later self-heal (socket re-auth / command 401).
    let on_code: Arc<dyn Fn(&str) + Send + Sync> = {
        let ev = events.clone();
        Arc::new(move |code: &str| {
            let _ = ev.send(BridgeEvent::AuthCode { code: code.to_owned() });
        })
    };
    let token = auth::ensure_token(&http, |c| on_code(c)).await?;
    let token = Arc::new(RwLock::new(token));

    // Wire ytmd realtime.
    let (state_tx, mut state_rx) = mpsc::unbounded_channel::<Value>();
    let (yconn_tx, mut yconn_rx) = mpsc::unbounded_channel::<()>();
    let (ydisc_tx, mut ydisc_rx) = mpsc::unbounded_channel::<()>();
    let ytmd = Arc::new(
        ytmd::connect(
            http.clone(),
            token.clone(),
            on_code.clone(),
            state_tx,
            yconn_tx,
            ydisc_tx,
        )
        .await?,
    );
```

- [ ] **Step 6: Build the whole app (GNU toolchain)**

Run: `cd app && PATH="$HOME/.cargo/bin:/c/msys64/mingw64/bin:$PATH" cargo build`
Expected: builds clean (no errors). Resolve any borrow/lifetime error before proceeding — common ones:
- If the `on_reconnect` closure complains about `FnMut`/`'static`: confirm every capture is a clone owned by the outer closure and re-cloned inside `async move`.
- If `reauth`'s `handshake` future borrows `http`/`on_code` and the borrow checker objects: the future is awaited within the same async block, so the borrows are valid; ensure `http`/`on_code`/`token`/`lock` are the block-local clones, not `self` fields, inside `reconnect_cb`.

- [ ] **Step 7: Run the full test suite + clippy**

Run: `cd app && PATH="$HOME/.cargo/bin:/c/msys64/mingw64/bin:$PATH" cargo test -p bridge-core`
Expected: all tests pass (Task 1 + Task 2 tests + the existing `state.rs` tests).

Run: `cd app && PATH="$HOME/.cargo/bin:/c/msys64/mingw64/bin:$PATH" cargo clippy -p bridge-core`
Expected: no warnings introduced by these changes (fix any `dead_code`/unused-import warnings from the edit).

- [ ] **Step 8: Commit**

```bash
git add app/bridge-core/src/ytmd.rs app/bridge-core/src/bridge.rs
git commit -m "feat(bridge-core): self-heal on token rejection (socket re-auth + 401 retry)

Ports bridge/src/ytmd.js re-auth: Event::Error flags auth failures, the
on_reconnect hook re-handshakes and swaps the socket auth, and send_command
retries once on 401. A shared Arc<RwLock<String>> token cell + coalescing
reauth() unify both paths; bridge.rs injects an on_code closure that emits
BridgeEvent::AuthCode. Fixes final-review finding I4."
```

---

## Verification (end-to-end, after all tasks)

Automated coverage is the two pure units (Tasks 1–2) plus a clean build/clippy of the wiring (Task 3). Live behavior (revoke the token in ytmdesktop mid-session, observe a fresh `AuthCode` printed by the CLI and playback control resuming) is a manual/hardware check tracked outside this plan; note it in the PR body as pending, consistent with the repo's convention for on-device verification.

Optional local smoke test with ytmdesktop running:
`cd app && PATH="$HOME/.cargo/bin:/c/msys64/mingw64/bin:$PATH" cargo run -p bridge-cli`
— expect the same startup/`AuthCode` behavior as before; then revoke the token in ytmdesktop's Companion Server settings and confirm a new code is printed rather than the process dying.

## Notes for Part 2 (Tauri) coordination

The `BridgeEvent` stream is unchanged. A mid-session re-auth now re-emits `BridgeEvent::AuthCode { code }`; the Tauri shell should treat a fresh `AuthCode` at any time (not just startup) as a re-prompt trigger. No new event/state variant is introduced.
