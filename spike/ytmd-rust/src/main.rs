// Phase 0 de-risking spike — drive the ytmdesktop Companion protocol from Rust.
//
// This is a THROWAWAY proof (design doc §"Phase 0"). It has no UI and no
// packaging. It exists to answer one question: can the bridge be re-implemented
// in Rust without losing ytmdesktop compatibility? If yes -> Branch T (Tauri +
// native Rust bridge); if this is painful -> Branch E (Electron + Node bridge).
//
// Pass criteria (all three):
//   1. Complete the Companion auth handshake: request code -> user clicks Allow
//      -> receive + persist a token.
//   2. Connect the realtime Socket.IO channel and print live track state as
//      songs change.
//   3. (Bonus) Advertise `_ytmboard._tcp` over mDNS and have a browser resolve
//      it — here we self-verify by browsing from the same process.
//
// The protocol details mirror the existing Node bridge (bridge/src/auth.js,
// ytmd.js, discovery.js) so a pass here maps 1:1 onto the real port.

use std::io::Write;
use std::path::PathBuf;
use std::time::Duration;

use anyhow::{anyhow, Context, Result};
use serde_json::{json, Value};

// --- config (mirrors bridge/src/config.js) --------------------------------

const YTMD_HOST: &str = "127.0.0.1"; // IPv4 on purpose; server isn't on IPv6
const YTMD_PORT: u16 = 9863;

// Distinct identity from the real bridge ("ytmboard"): the Companion token is
// bound to appId and re-requesting OVERWRITES it, so a shared id would clobber
// the real bridge's saved token. The spike stays self-contained.
const APP_ID: &str = "ytmboardspike";
const APP_NAME: &str = "YT Music board spike";
const APP_VERSION: &str = "1.0.0";

const BOARD_PORT: u16 = 8765;
const MDNS_SERVICE: &str = "_ytmboard._tcp.local.";

fn base_url() -> String {
    format!("http://{YTMD_HOST}:{YTMD_PORT}/api/v1")
}

fn token_path() -> PathBuf {
    // ~/.ytmboard/spike-token.json — separate file from the real bridge's token.
    let home = std::env::var("USERPROFILE")
        .or_else(|_| std::env::var("HOME"))
        .unwrap_or_else(|_| ".".into());
    PathBuf::from(home).join(".ytmboard").join("spike-token.json")
}

// --- auth handshake (criterion 1) -----------------------------------------

fn load_token() -> Option<String> {
    let raw = std::fs::read_to_string(token_path()).ok()?;
    let v: Value = serde_json::from_str(&raw).ok()?;
    // Only accept a token saved under our appId.
    if v.get("appId")?.as_str()? == APP_ID {
        return v.get("token")?.as_str().map(str::to_owned);
    }
    None
}

fn save_token(token: &str) -> Result<()> {
    let path = token_path();
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir).ok();
    }
    let body = json!({ "appId": APP_ID, "token": token });
    std::fs::write(&path, serde_json::to_string_pretty(&body)?)
        .with_context(|| format!("writing token to {}", path.display()))?;
    Ok(())
}

fn request_token(http: &reqwest::blocking::Client) -> Result<String> {
    // 1. POST /auth/requestcode {appId, appName, appVersion} -> {code}
    let code: Value = http
        .post(format!("{}/auth/requestcode", base_url()))
        .json(&json!({
            "appId": APP_ID,
            "appName": APP_NAME,
            "appVersion": APP_VERSION,
        }))
        .send()
        .context("POST /auth/requestcode failed — is ytmdesktop running with the \
                  Companion Server enabled?")?
        .error_for_status()?
        .json()?;
    let code = code
        .get("code")
        .and_then(Value::as_str)
        .ok_or_else(|| anyhow!("no `code` in requestcode response: {code}"))?;

    println!("\n>>> ytmdesktop is asking you to allow \"{APP_NAME}\".");
    println!(">>> Code: {code}");
    println!(">>> Click ALLOW in the app within ~30 seconds...\n");

    // 2. POST /auth/request {appId, code} -> {token}. The server holds this
    //    request open until the user clicks Allow (or ~30s timeout), so the
    //    HTTP client needs a timeout comfortably longer than that window.
    let token: Value = http
        .post(format!("{}/auth/request", base_url()))
        .json(&json!({ "appId": APP_ID, "code": code }))
        .send()
        .context("POST /auth/request failed (did you click Allow in time?)")?
        .error_for_status()?
        .json()?;
    let token = token
        .get("token")
        .and_then(Value::as_str)
        .ok_or_else(|| anyhow!("no `token` in request response: {token}"))?
        .to_owned();

    save_token(&token)?;
    println!("Authorized. Token saved to {}.\n", token_path().display());
    Ok(token)
}

fn ensure_token(http: &reqwest::blocking::Client) -> Result<String> {
    match load_token() {
        Some(t) => {
            println!("Using cached token from {}.", token_path().display());
            Ok(t)
        }
        None => request_token(http),
    }
}

// One-shot GET /state to seed current playback: the socket only pushes on a
// CHANGE, so without this a paused/idle player shows nothing until the next
// track change (mirrors ytmd.js #fetchState).
fn fetch_state_once(http: &reqwest::blocking::Client, token: &str) {
    match http
        .get(format!("{}/state", base_url()))
        .header("Authorization", token) // raw token, no "Bearer"
        .send()
        .and_then(|r| r.error_for_status())
        .and_then(|r| r.json::<Value>())
    {
        Ok(state) => print_track("seed /state", &state),
        Err(e) => eprintln!("[ytmd] initial /state fetch failed: {e}"),
    }
}

// --- track pretty-printer (criterion 2 evidence) --------------------------

fn print_track(source: &str, state: &Value) {
    let video = state.get("video").cloned().unwrap_or(Value::Null);
    let player = state.get("player").cloned().unwrap_or(Value::Null);

    let title = video.get("title").and_then(Value::as_str).unwrap_or("");
    let artist = video.get("author").and_then(Value::as_str).unwrap_or("");
    let track_state = player
        .get("trackState")
        .and_then(Value::as_i64)
        .unwrap_or(-1);
    let playback = match track_state {
        0 => "paused",
        1 => "playing",
        2 => "buffering",
        _ => "unknown",
    };
    let progress = player
        .get("videoProgress")
        .and_then(Value::as_f64)
        .unwrap_or(0.0);

    if title.is_empty() && artist.is_empty() {
        println!("[{source}] (no track metadata yet) playback={playback}");
    } else {
        println!(
            "[{source}] {playback}  {artist} — {title}  @{:.0}s",
            progress
        );
    }
}

// --- realtime channel (criterion 2) ---------------------------------------

fn connect_realtime(token: &str) -> Result<rust_socketio::client::Client> {
    use rust_socketio::{ClientBuilder, Event, Payload, RawClient, TransportType};

    let state_cb = |payload: Payload, _socket: RawClient| match payload {
        // Socket.IO delivers the /state object as a one-element array of JSON.
        Payload::Text(values) => {
            if let Some(state) = values.first() {
                print_track("state-update", state);
            }
        }
        Payload::Binary(bin) => eprintln!("[ytmd] unexpected binary frame ({} bytes)", bin.len()),
        #[allow(deprecated)]
        Payload::String(s) => println!("[ytmd] state-update (string): {s}"),
    };

    // URL = base host:port; the `/api/v1/realtime` path is the Socket.IO
    // NAMESPACE, not the engine.io mount path (that stays default /socket.io/).
    // websocket-only transport matches the Node client (engine.io polling won't
    // do here).
    let socket = ClientBuilder::new(format!("http://{YTMD_HOST}:{YTMD_PORT}"))
        .namespace("/api/v1/realtime")
        .auth(json!({ "token": token }))
        .transport_type(TransportType::Websocket)
        .on("state-update", state_cb)
        .on(Event::Connect, |_, _| println!("[ytmd] socket connected"))
        .on(Event::Error, |err, _| eprintln!("[ytmd] socket error: {err:?}"))
        .on(Event::Close, |_, _| println!("[ytmd] socket closed"))
        .connect()
        .context("Socket.IO connect failed")?;

    Ok(socket)
}

// --- mDNS advertise + self-discover (criterion 3) -------------------------

fn start_mdns() -> Result<mdns_sd::ServiceDaemon> {
    use mdns_sd::{ServiceDaemon, ServiceEvent, ServiceInfo};

    let mdns = ServiceDaemon::new().context("ServiceDaemon::new")?;

    // Advertise, matching discovery.js (txt: proto/path/v). enable_addr_auto()
    // lets the daemon fill in this host's addresses.
    let service = ServiceInfo::new(
        MDNS_SERVICE,
        "YT Music board bridge (spike)",
        "ytmboard-spike.local.",
        "", // ip filled by enable_addr_auto
        BOARD_PORT,
        &[("proto", "ws"), ("path", "/"), ("v", "1")][..],
    )
    .context("ServiceInfo::new")?
    .enable_addr_auto();

    mdns.register(service).context("mdns register")?;
    println!("[mdns] advertising {MDNS_SERVICE} on :{BOARD_PORT}");

    // Self-verify: browse for the same type and print what resolves. Proves a
    // client on this network can discover the advertisement.
    let recv = mdns.browse(MDNS_SERVICE).context("mdns browse")?;
    std::thread::spawn(move || {
        while let Ok(event) = recv.recv() {
            if let ServiceEvent::ServiceResolved(info) = event {
                println!(
                    "[mdns] discovered {} at {:?}:{} txt={:?}",
                    info.get_fullname(),
                    info.get_addresses(),
                    info.get_port(),
                    info.get_properties(),
                );
            }
        }
    });

    Ok(mdns)
}

fn main() -> Result<()> {
    println!("=== ytmd Rust spike (Phase 0 de-risking) ===");

    let http = reqwest::blocking::Client::builder()
        .timeout(Duration::from_secs(60)) // > the ~30s Allow window
        .build()?;

    // Criterion 1.
    let token = ensure_token(&http)?;

    // Criterion 3 (start early so discovery has time while music plays).
    let _mdns = start_mdns()?;

    // Criterion 2.
    let _socket = connect_realtime(&token)?;
    fetch_state_once(&http, &token);

    println!("\nStreaming live state. Change songs in YouTube Music to see updates.");
    println!("Press Ctrl-C to exit.\n");
    std::io::stdout().flush().ok();

    // Park forever; the socket + mDNS run on their own threads. Dropping either
    // handle would tear it down, so we hold both alive here.
    loop {
        std::thread::sleep(Duration::from_secs(3600));
    }
}
