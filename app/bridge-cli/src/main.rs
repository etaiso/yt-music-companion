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
