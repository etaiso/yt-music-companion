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

    let socket = ClientBuilder::new(ytmd_realtime_base())
        .namespace(ytmd_realtime_namespace())
        .auth(json!({ "token": token }))
        .transport_type(TransportType::Websocket)
        .on("state-update", state_cb)
        .on(Event::Connect, connect_cb)
        .on(Event::Close, close_cb)
        .on(Event::Error, |err, _: Client| {
            async move { tracing::error!("[ytmd] socket error: {err:?}") }.boxed()
        })
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
