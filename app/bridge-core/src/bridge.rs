//! Wires ytmdesktop -> normalize -> board (ports index.js), emitting a single
//! BridgeEvent stream both the CLI and the Tauri shell consume.
use crate::state::{BridgeState, Machine, Signal};
use crate::{auth, board_server, commands, cover, discovery, normalize, ytmd};
use anyhow::Result;
use serde_json::Value;
use std::sync::Arc;
use tokio::sync::{mpsc, Mutex, RwLock};
use tokio::time::{sleep, Duration};

#[derive(Debug, Clone)]
pub enum BridgeEvent {
    State(BridgeState),
    AuthCode { code: String },
    NowPlaying(normalize::NowPlayingVm),
    Log(String),
}

// Manual impl rather than `#[derive(Serialize)]` + `#[serde(tag = "type")]`:
// serde's internally-tagged representation only merges a newtype variant's
// payload into the tag object when that payload itself serializes to a map.
// `BridgeState` and `String` don't (they serialize as bare JSON strings), so
// the derive silently produces `{"type":"state","waiting-for-board":null}`
// instead of the `{"type":"state","data":"waiting-for-board"}` shape the
// frontend needs. Hand-rolling keeps the tuple-variant shape (so
// bridge-cli's `match` is untouched) while giving every variant an explicit
// `data` field.
impl serde::Serialize for BridgeEvent {
    fn serialize<S: serde::Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        use serde::ser::SerializeMap;
        match self {
            BridgeEvent::State(s) => {
                let mut map = serializer.serialize_map(Some(2))?;
                map.serialize_entry("type", "state")?;
                map.serialize_entry("data", s)?;
                map.end()
            }
            BridgeEvent::AuthCode { code } => {
                let mut map = serializer.serialize_map(Some(2))?;
                map.serialize_entry("type", "auth-code")?;
                map.serialize_entry("code", code)?;
                map.end()
            }
            BridgeEvent::NowPlaying(vm) => {
                let mut map = serializer.serialize_map(Some(2))?;
                map.serialize_entry("type", "now-playing")?;
                map.serialize_entry("data", vm)?;
                map.end()
            }
            BridgeEvent::Log(msg) => {
                let mut map = serializer.serialize_map(Some(2))?;
                map.serialize_entry("type", "log")?;
                map.serialize_entry("data", msg)?;
                map.end()
            }
        }
    }
}

pub async fn run(events: mpsc::UnboundedSender<BridgeEvent>) -> Result<()> {
    let http = reqwest::Client::builder()
        .timeout(Duration::from_secs(60)) // > the ~30s Allow window
        .build()?;

    let emit = |e: BridgeEvent| {
        let _ = events.send(e);
    };

    // Two orthogonal connection facts (ytmd socket up, board attached) tracked
    // in one place; the rendered `BridgeState` is DERIVED from them so neither
    // event can clobber the other (see state.rs). `publish` emits only on a
    // real change, so the tray/window don't redraw on every no-op signal.
    let mut machine = Machine::new();
    let mut shown: Option<BridgeState> = None;
    let mut publish = |m: &Machine| {
        let s = m.state();
        if shown != Some(s) {
            shown = Some(s);
            let _ = events.send(BridgeEvent::State(s));
        }
    };
    publish(&machine);

    // Board server + command handling.
    let (cmd_tx, mut cmd_rx) = mpsc::unbounded_channel::<(String, Option<f64>)>();
    let (board_conn_tx, mut board_conn_rx) = mpsc::unbounded_channel::<()>();
    let (board_disc_tx, mut board_disc_rx) = mpsc::unbounded_channel::<()>();
    let board = Arc::new(board_server::start(cmd_tx, board_conn_tx, board_disc_tx).await?);

    // mDNS advertisement (kept alive for the process lifetime).
    let _discovery = discovery::start()?;

    // One code-surfacing closure, reused for the initial handshake and every
    // later self-heal (socket re-auth / command 401).
    let on_code: Arc<dyn Fn(&str) + Send + Sync> = {
        let ev = events.clone();
        Arc::new(move |code: &str| {
            let _ = ev.send(BridgeEvent::AuthCode { code: code.to_owned() });
        })
    };

    // Acquire an authorized, connected ytmd session. RETRY (rather than exit)
    // while ytmdesktop is unreachable, so a host that isn't running yet shows
    // YtmdNotFound and self-heals when it comes up — instead of the whole
    // bridge returning Err and the window freezing on its last state. `token`
    // is shared with the socket's re-auth path, so it must outlive the loop.
    let token = Arc::new(RwLock::new(String::new()));
    let (mut state_rx, mut yconn_rx, mut ydisc_rx, ytmd) = loop {
        // Probe first so we can distinguish "host down" (retry quietly) from
        // "up but not yet authorized" (surface a pairing code).
        if !ytmd::is_reachable(&http).await {
            machine.apply(Signal::YtmdProbeFailed);
            publish(&machine);
            sleep(Duration::from_secs(3)).await;
            continue;
        }

        machine.apply(Signal::ServerUpNoToken);
        publish(&machine);
        let fresh = match auth::ensure_token(&http, |c| on_code(c)).await {
            Ok(t) => t,
            Err(e) => {
                emit(BridgeEvent::Log(format!("[auth] handshake failed: {e:#}")));
                sleep(Duration::from_secs(3)).await;
                continue;
            }
        };
        *token.write().await = fresh;

        let (state_tx, state_rx) = mpsc::unbounded_channel::<Value>();
        let (yconn_tx, yconn_rx) = mpsc::unbounded_channel::<()>();
        let (ydisc_tx, ydisc_rx) = mpsc::unbounded_channel::<()>();
        match ytmd::connect(
            http.clone(),
            token.clone(),
            on_code.clone(),
            state_tx,
            yconn_tx,
            ydisc_tx,
        )
        .await
        {
            Ok(y) => break (state_rx, yconn_rx, ydisc_rx, Arc::new(y)),
            Err(e) => {
                emit(BridgeEvent::Log(format!("[ytmd] connect failed: {e:#}")));
                sleep(Duration::from_secs(3)).await;
                continue;
            }
        }
    };

    // Socket is connected: mark the feed authorized/up. Board attachment is
    // driven separately by the board server's connect events in the loop below,
    // so this no longer force-resets an already-attached board (the #1 bug).
    machine.apply(Signal::Authorized);
    publish(&machine);

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
                // Seed the board on (re)connect. The socket only pushes on a
                // CHANGE, so a board attached during idle/paused playback would
                // otherwise see nothing. If the player has no track yet (fresh
                // launch, nothing ever played) `normalize` yields None — send an
                // explicit idle frame so the board still leaves its loader
                // instead of timing out to OFFLINE (the board-side #1 symptom).
                let seeded = match ytmd.fetch_state_once().await {
                    Some(st) if normalize::normalize(&st, true).is_some() => {
                        last_state = Some(st.clone());
                        push_vm(&st, true, &board, &last_vm, &last_cover_url, &emit).await;
                        true
                    }
                    _ => false,
                };
                if !seeded {
                    board.broadcast_state(normalize::idle_vm(true));
                }
                // Fires on every connect incl. reconnect. board_up is untouched,
                // so a mid-song reconnect returns straight to BoardConnected.
                machine.apply(Signal::Authorized);
                publish(&machine);
            }
            Some(()) = ydisc_rx.recv() => {
                emit(BridgeEvent::Log("[ytmd] socket disconnected".into()));
                machine.apply(Signal::YtmdDropped);
                publish(&machine);
                // Blank the track before telling the board the host went away,
                // so neither the board nor a reopened window keeps showing a
                // stale song from a source that's no longer live (#2). Keep
                // `last_vm` untouched so command mapping still has the last real
                // track; a reconnect reseeds the display anyway.
                let mut v = last_vm
                    .lock()
                    .await
                    .clone()
                    .unwrap_or_else(|| normalize::idle_vm(false));
                v.host_connected = false;
                v.title = String::new();
                v.artist = String::new();
                v.album = String::new();
                v.cover_url = None;
                board.broadcast_state(v);
                board.broadcast_cover(None);
                *last_cover_url.lock().await = None;
            }
            Some(()) = board_conn_rx.recv() => {
                machine.apply(Signal::BoardAttached);
                publish(&machine);
            }
            Some(()) = board_disc_rx.recv() => {
                machine.apply(Signal::BoardDetached);
                publish(&machine);
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

#[cfg(test)]
mod event_serde_tests {
    use super::*;
    use crate::state::BridgeState;

    #[test]
    fn state_event_serializes_tagged() {
        let j = serde_json::to_value(BridgeEvent::State(BridgeState::WaitingForBoard)).unwrap();
        assert_eq!(j["type"], "state");
        assert_eq!(j["data"], "waiting-for-board");
    }

    #[test]
    fn auth_code_event_serializes_tagged() {
        let j = serde_json::to_value(BridgeEvent::AuthCode { code: "1234".into() }).unwrap();
        assert_eq!(j["type"], "auth-code");
        assert_eq!(j["code"], "1234");
    }

    #[test]
    fn now_playing_event_nests_vm_under_data() {
        let state = serde_json::json!({
            "video": { "title": "Creep", "author": "Radiohead" },
            "player": { "trackState": 1 }
        });
        let vm = crate::normalize::normalize(&state, true).unwrap();
        let j = serde_json::to_value(BridgeEvent::NowPlaying(vm)).unwrap();
        assert_eq!(j["type"], "now-playing");
        // vm nests under "data" as an object (not flattened) with its snake_case keys.
        assert!(j["data"].is_object());
        assert_eq!(j["data"]["title"], "Creep");
        assert_eq!(j["data"]["artist"], "Radiohead");
        assert_eq!(j["data"]["playback"], "playing");
    }

    #[test]
    fn log_event_serializes_tagged() {
        let j = serde_json::to_value(BridgeEvent::Log("hello".into())).unwrap();
        assert_eq!(j["type"], "log");
        assert_eq!(j["data"], "hello");
    }
}
