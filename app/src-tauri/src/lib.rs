//! YT Music board bridge — Tauri v2 tray shell.
//!
//! Window + tray with Open/Quit, close-hides-to-tray, the notification /
//! autostart / opener plugins registered, and `bridge-core` running on
//! Tauri's async runtime with every `BridgeEvent` forwarded to the webview.

use std::sync::Mutex;

use bridge_core::{BridgeEvent, BridgeState};
use tauri::{
    menu::{Menu, MenuItem},
    tray::{MouseButton, MouseButtonState, TrayIconBuilder, TrayIconEvent},
    Emitter, Manager, WindowEvent,
};

/// Latest bridge state, kept in Tauri managed state for the tray (Task 6).
pub struct LatestBridgeState(pub Mutex<Option<BridgeState>>);

/// Bring the main status window to the foreground.
fn show_main_window(app: &tauri::AppHandle) {
    if let Some(window) = app.get_webview_window("main") {
        let _ = window.show();
        let _ = window.set_focus();
    }
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .plugin(tauri_plugin_notification::init())
        .plugin(tauri_plugin_autostart::init(
            tauri_plugin_autostart::MacosLauncher::LaunchAgent,
            None,
        ))
        .manage(LatestBridgeState(Mutex::new(None)))
        .setup(|app| {
            let handle = app.handle().clone();
            let (tx, mut rx) = tokio::sync::mpsc::unbounded_channel::<BridgeEvent>();
            tauri::async_runtime::spawn(async move {
                if let Err(e) = bridge_core::run(tx).await {
                    let _ = handle.emit("bridge-fatal", e.to_string());
                }
            });

            let handle2 = app.handle().clone();
            tauri::async_runtime::spawn(async move {
                while let Some(ev) = rx.recv().await {
                    if let BridgeEvent::State(s) = &ev {
                        let latest = handle2.state::<LatestBridgeState>();
                        *latest.0.lock().unwrap() = Some(*s);
                    }
                    let _ = handle2.emit("bridge-event", &ev);
                }
            });

            let open_item = MenuItem::with_id(app, "open", "Open", true, None::<&str>)?;
            let quit_item = MenuItem::with_id(app, "quit", "Quit", true, None::<&str>)?;
            let menu = Menu::with_items(app, &[&open_item, &quit_item])?;

            TrayIconBuilder::new()
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
