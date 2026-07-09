//! Board-facing protocol (ports board-server.js). Plain WebSocket so the board
//! needs no Socket.IO / auth / TLS. On connect: latest state snapshot, then the
//! latest cover frame. Thereafter a fresh frame on every change. Heartbeat ping
//! every 15s; drop a client that missed the previous pong.
use crate::config::board_port;
use crate::normalize::NowPlayingVm;
use anyhow::Result;
use futures_util::{SinkExt, StreamExt};
use serde_json::json;
use std::sync::Arc;
use tokio::net::{TcpListener, TcpStream};
use tokio::sync::{broadcast, mpsc, Mutex};
use tokio::time::{interval, Duration};
use tokio_tungstenite::tungstenite::Message;

const HEARTBEAT: Duration = Duration::from_secs(15);

pub fn state_frame(vm: &NowPlayingVm) -> String {
    json!({ "type": "state", "data": vm }).to_string()
}

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

/// Signals `board_disconnected_tx` on drop. Only constructed after a
/// successful WS upgrade, so a raw-TCP probe that never upgrades can't fire
/// a spurious disconnect for a connect that was never signaled either.
struct DisconnectGuard(mpsc::UnboundedSender<()>);
impl Drop for DisconnectGuard {
    fn drop(&mut self) {
        let _ = self.0.send(());
    }
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
                if let Err(e) = handle_client(stream, sub, snap, cmd, conn, disc).await {
                    tracing::debug!("[board] client ended: {e}");
                }
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
    board_connected_tx: mpsc::UnboundedSender<()>,
    board_disconnected_tx: mpsc::UnboundedSender<()>,
) -> Result<()> {
    // Only a successful WS upgrade counts as a board attaching; a raw TCP
    // probe (port scan, health check) that never upgrades must not flip the
    // orchestrator's attach/detach state.
    let ws = match tokio_tungstenite::accept_async(stream).await {
        Ok(ws) => ws,
        Err(e) => return Err(e.into()),
    };
    let _ = board_connected_tx.send(());
    let _disconnect_guard = DisconnectGuard(board_disconnected_tx);

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
