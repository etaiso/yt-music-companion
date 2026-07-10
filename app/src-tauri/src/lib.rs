//! YT Music board bridge — Tauri v2 tray shell.
//!
//! Window + tray with Open/Quit, close-hides-to-tray, the notification /
//! autostart / opener plugins registered, and `bridge-core` running on
//! Tauri's async runtime with every `BridgeEvent` forwarded to the webview.

use std::sync::Mutex;

use bridge_core::{BridgeEvent, BridgeState, NowPlayingVm};
use tauri::{
    menu::{Menu, MenuItem},
    tray::{MouseButton, MouseButtonState, TrayIconBuilder, TrayIconEvent},
    Emitter, Manager, WindowEvent,
};
use tauri_plugin_autostart::ManagerExt;
use tauri_plugin_notification::NotificationExt;

/// Latest bridge state, kept in Tauri managed state for the tray (Task 6) and
/// replayed to the webview via `current_state` when the window opens.
pub struct LatestBridgeState(pub Mutex<Option<BridgeState>>);

/// Latest now-playing view-model, cached so a window opened mid-song repaints
/// its track card immediately instead of waiting for the next ytmd push.
pub struct LatestNowPlaying(pub Mutex<Option<NowPlayingVm>>);

/// Bring the main status window to the foreground. `unminimize` first: if the
/// window was minimized, `show` alone leaves it minimized on Windows, so the
/// app appeared to "open minimized" (#4).
fn show_main_window(app: &tauri::AppHandle) {
    if let Some(window) = app.get_webview_window("main") {
        let _ = window.unminimize();
        let _ = window.show();
        let _ = window.set_focus();
    }
}

/// Best-effort launch of the installed YouTube Music Desktop app (#3). Returns
/// `Ok(true)` if we managed to spawn it, `Ok(false)` if it couldn't be found —
/// the UI keeps its download link as the fallback. Platform-specific because
/// there's no cross-platform "launch an app by name".
#[tauri::command]
fn open_ytmd() -> Result<bool, String> {
    use std::process::Command;

    #[cfg(target_os = "windows")]
    {
        // ytmdesktop ships via electron-builder NSIS, which installs per-user
        // under LOCALAPPDATA by default; also probe the per-machine location.
        // The exe name is fixed by the upstream build config.
        let exe = "YouTube Music Desktop App.exe";
        let mut candidates: Vec<std::path::PathBuf> = Vec::new();
        if let Ok(local) = std::env::var("LOCALAPPDATA") {
            candidates.push(
                std::path::Path::new(&local)
                    .join("Programs")
                    .join("youtube-music-desktop-app")
                    .join(exe),
            );
        }
        if let Ok(pf) = std::env::var("ProgramFiles") {
            candidates.push(std::path::Path::new(&pf).join("YouTube Music Desktop App").join(exe));
        }
        for path in candidates {
            if path.exists() {
                return Command::new(&path).spawn().map(|_| true).map_err(|e| e.to_string());
            }
        }
        Ok(false)
    }

    #[cfg(target_os = "macos")]
    {
        // `open -a` launches by app name wherever Launch Services knows it; a
        // non-zero status means it isn't installed.
        let status = Command::new("open")
            .args(["-a", "YouTube Music Desktop App"])
            .status()
            .map_err(|e| e.to_string())?;
        Ok(status.success())
    }

    #[cfg(not(any(target_os = "windows", target_os = "macos")))]
    {
        // Linux: rely on the binary being on PATH (distro package / AppImage).
        match Command::new("youtube-music-desktop-app").spawn() {
            Ok(_) => Ok(true),
            Err(_) => Ok(false),
        }
    }
}

/// Tray tooltip + state-tinted icon for each `BridgeState` (Task 6).
///
/// Icons are small (32x32) badge variants baked in at compile time via
/// `tauri::include_image!`, so no filesystem access is needed at runtime.
fn tray_status_for_state(state: &BridgeState) -> (&'static str, tauri::image::Image<'static>) {
    use BridgeState::*;

    let icon_neutral = tauri::include_image!("icons/tray-neutral.png");
    let icon_connected = tauri::include_image!("icons/tray-connected.png");
    let icon_warning = tauri::include_image!("icons/tray-warning.png");
    let icon_action = tauri::include_image!("icons/tray-action.png");

    match state {
        Starting => ("YT Music board — starting…", icon_neutral),
        YtmdNotFound => (
            "YT Music board — YouTube Music Desktop not running",
            icon_warning,
        ),
        NotAuthorized => ("YT Music board — authorization needed", icon_action),
        WaitingForBoard => ("YT Music board — waiting for board", icon_neutral),
        BoardConnected => ("YT Music board — connected", icon_connected),
        YtmdDisconnected => (
            "YT Music board — YouTube Music Desktop disconnected",
            icon_warning,
        ),
    }
}

/// Current bridge snapshot the webview pulls on load. `bridge-event`s are
/// emitted once, near startup, into a window that is hidden until the user
/// opens it — and Tauri does not replay events for listeners that attach
/// later. Without this pull the window would sit on its default "Starting…"
/// card forever. Shapes match the live `bridge-event` payloads so the
/// frontend can feed them through the same handler.
#[tauri::command]
fn current_state(
    state: tauri::State<'_, LatestBridgeState>,
    now_playing: tauri::State<'_, LatestNowPlaying>,
) -> serde_json::Value {
    let state_val = *state.0.lock().unwrap();
    let state = state_val.map(|s| serde_json::json!({ "type": "state", "data": s }));
    // Only replay a track when the board is connected. Otherwise a window
    // reopened after YouTube Music Desktop went away would repaint the last
    // song from a source that's no longer live (#2).
    let now_playing = if matches!(state_val, Some(BridgeState::BoardConnected)) {
        now_playing
            .0
            .lock()
            .unwrap()
            .clone()
            .map(|vm| serde_json::json!({ "type": "now-playing", "data": vm }))
    } else {
        None
    };
    serde_json::json!({ "state": state, "nowPlaying": now_playing })
}

/// App version string (from `tauri.conf.json` / Cargo), shown in the status
/// window so a user can report which build they're on. Single source of truth:
/// the bundle's package version, so release bumps carry through automatically.
#[tauri::command]
fn app_version(app: tauri::AppHandle) -> String {
    app.package_info().version.to_string()
}

/// Whether the app is currently registered as a login item (Task 8).
#[tauri::command]
fn get_autostart(app: tauri::AppHandle) -> bool {
    app.autolaunch().is_enabled().unwrap_or(false)
}

/// Register/unregister the app as a login item (Task 8).
#[tauri::command]
fn set_autostart(app: tauri::AppHandle, enabled: bool) -> Result<(), String> {
    let m = app.autolaunch();
    (if enabled { m.enable() } else { m.disable() }).map_err(|e| e.to_string())
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        // Must be the first plugin: a second launch (double-click, autostart +
        // manual open) hands its argv to this callback and exits, rather than
        // spawning a rival tray whose bridge would then die fighting for the
        // board-server port. We just surface the window that's already running.
        .plugin(tauri_plugin_single_instance::init(|app, _argv, _cwd| {
            show_main_window(app);
        }))
        .plugin(tauri_plugin_opener::init())
        .plugin(tauri_plugin_notification::init())
        .plugin(tauri_plugin_autostart::init(
            tauri_plugin_autostart::MacosLauncher::LaunchAgent,
            None,
        ))
        .invoke_handler(tauri::generate_handler![
            current_state,
            app_version,
            get_autostart,
            set_autostart,
            open_ytmd
        ])
        .manage(LatestBridgeState(Mutex::new(None)))
        .manage(LatestNowPlaying(Mutex::new(None)))
        .setup(|app| {
            // Tray-only app: don't show a Dock icon on macOS.
            #[cfg(target_os = "macos")]
            app.set_activation_policy(tauri::ActivationPolicy::Accessory);

            let handle = app.handle().clone();
            let (tx, mut rx) = tokio::sync::mpsc::unbounded_channel::<BridgeEvent>();
            tauri::async_runtime::spawn(async move {
                if let Err(e) = bridge_core::run(tx).await {
                    let _ = handle.emit("bridge-fatal", e.to_string());
                }
            });

            let open_item = MenuItem::with_id(app, "open", "Open", true, None::<&str>)?;
            let quit_item = MenuItem::with_id(app, "quit", "Quit", true, None::<&str>)?;
            let menu = Menu::with_items(app, &[&open_item, &quit_item])?;

            let tray = TrayIconBuilder::new()
                .icon(app.default_window_icon().unwrap().clone())
                .tooltip("YT Music board bridge")
                .menu(&menu)
                .show_menu_on_left_click(false)
                .on_menu_event(|app, event| match event.id.as_ref() {
                    "open" => show_main_window(app),
                    "quit" => app.exit(0),
                    _ => {}
                })
                .on_tray_icon_event(|tray, event| {
                    if let TrayIconEvent::Click {
                        button: MouseButton::Left,
                        button_state: MouseButtonState::Up,
                        ..
                    } = event
                    {
                        show_main_window(tray.app_handle());
                    }
                })
                .build(app)?;

            let handle2 = app.handle().clone();
            tauri::async_runtime::spawn(async move {
                while let Some(ev) = rx.recv().await {
                    if let BridgeEvent::State(s) = &ev {
                        let latest = handle2.state::<LatestBridgeState>();
                        *latest.0.lock().unwrap() = Some(*s);

                        let (tooltip, icon) = tray_status_for_state(s);
                        let _ = tray.set_tooltip(Some(tooltip));
                        let _ = tray.set_icon(Some(icon));
                    }
                    if let BridgeEvent::NowPlaying(vm) = &ev {
                        let latest = handle2.state::<LatestNowPlaying>();
                        *latest.0.lock().unwrap() = Some(vm.clone());
                    }
                    if let BridgeEvent::AuthCode { code } = &ev {
                        // Seen with the window closed/hidden — the tray has no
                        // room for the code itself, so nudge via OS notification.
                        let _ = handle2
                            .notification()
                            .builder()
                            .title("YT Music board — action needed")
                            .body(format!("Enter code {code} — click Allow in ytmdesktop"))
                            .show();
                    }
                    let _ = handle2.emit("bridge-event", &ev);
                }
            });

            Ok(())
        })
        .on_window_event(|window, event| {
            // Closing the window hides it to the tray rather than exiting.
            if let WindowEvent::CloseRequested { api, .. } = event {
                let _ = window.hide();
                api.prevent_close();
            }
        })
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
