//! Wires ytmdesktop -> normalize -> board (ports index.js), emitting a single
//! BridgeEvent stream both the CLI and the Tauri shell consume.
use crate::state::{BridgeState, Signal};
use crate::{auth, board_server, commands, cover, discovery, normalize, state, ytmd};
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

    let emit = |e: BridgeEvent| {
        let _ = events.send(e);
    };
    let set_state = |s: BridgeState| {
        let _ = events.send(BridgeEvent::State(s));
    };

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
    let token = auth::ensure_token(&http, move |code| {
        let _ = ev_for_code.send(BridgeEvent::AuthCode { code: code.to_owned() });
    })
    .await?;

    // Wire ytmd realtime.
    let (state_tx, mut state_rx) = mpsc::unbounded_channel::<Value>();
    let (yconn_tx, mut yconn_rx) = mpsc::unbounded_channel::<()>();
    let (ydisc_tx, mut ydisc_rx) = mpsc::unbounded_channel::<()>();
    let ytmd = Arc::new(ytmd::connect(http.clone(), token.clone(), state_tx, yconn_tx, ydisc_tx).await?);

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
                            let _ = events
                                .send(BridgeEvent::Log(format!("[cmd] {cmd} -> {}", mapped.command)));
                        }
                    }
                    None => {
                        let _ = events.send(BridgeEvent::Log(format!("[cmd] unknown board command: {cmd}")));
                    }
                }
            }
        });
    }

    // Debounced state pipeline. Seeding on (re)connect is handled inside the
    // `yconn_rx` arm below, which is the single consumer of `state_rx`.
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
                if let Some(mut v) = vm {
                    v.host_connected = false;
                    board.broadcast_state(v);
                }
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
    let Some(vm) = normalize::normalize(raw, connected) else {
        return;
    };
    *last_vm.lock().await = Some(vm.clone());
    board.broadcast_state(vm.clone());
    emit(BridgeEvent::NowPlaying(vm.clone()));

    // Cover: only re-render when the URL changes; skip ads (stale/no art).
    if vm.ad_playing {
        return;
    }
    let url = vm.cover_url.clone();
    let mut guard = last_cover_url.lock().await;
    if *guard == url {
        return;
    }
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
