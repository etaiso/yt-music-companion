//! Client for the ytmdesktop Companion Server (ports ytmd.js).
//! Realtime via Socket.IO (websocket-only), token in the auth object; the
//! `state-update` event carries the full /state object. REST /state seeds once
//! on connect (socket only pushes on change); /command drives control actions.
use crate::commands::YtmdCommand;
use crate::config::{ytmd_base, ytmd_realtime_base, ytmd_realtime_namespace};
use anyhow::{Context, Result};
use futures_util::FutureExt;
use rust_socketio::asynchronous::{Client, ClientBuilder, ReconnectSettings};
use rust_socketio::{Event, Payload, TransportType};
use serde_json::{json, Value};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use tokio::sync::mpsc::UnboundedSender;
use tokio::sync::{Mutex, RwLock};

/// True when a socket/command error message indicates the token was rejected
/// (revoked/expired/overwritten). Ports the JS `/auth|token|unauthor/i` test.
fn is_auth_error(msg: &str) -> bool {
    let m = msg.to_ascii_lowercase();
    ["auth", "token", "unauthor"].iter().any(|kw| m.contains(kw))
}

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
                    crate::auth::request_token(&http, |c| on_code(c))
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

impl YtmdHandle {
    /// One-shot GET /state so a paused/idle player still paints for a board that
    /// just (re)connected. Failures are non-fatal.
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

    pub async fn disconnect(self) {
        let _ = self.socket.disconnect().await;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicUsize, Ordering as AtomicOrdering};

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
}
