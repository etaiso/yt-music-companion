//! Single source of truth for bridge settings (ports config.js).
//! Env overrides let you point at a non-default host/port without editing code.
use std::path::PathBuf;

pub const APP_ID: &str = "ytmboard";
pub const APP_NAME: &str = "YT Music board";
pub const APP_VERSION: &str = "1.1.0";
pub const COVER_PX: u32 = 172;

pub fn ytmd_host() -> String {
    std::env::var("YTMD_HOST").unwrap_or_else(|_| "127.0.0.1".into())
}
pub fn ytmd_port() -> u16 {
    std::env::var("YTMD_PORT").ok().and_then(|s| s.parse().ok()).unwrap_or(9863)
}
pub fn board_port() -> u16 {
    std::env::var("BOARD_PORT").ok().and_then(|s| s.parse().ok()).unwrap_or(8765)
}
pub fn volume_step() -> u16 {
    std::env::var("VOLUME_STEP").ok().and_then(|s| s.parse().ok()).unwrap_or(5)
}

pub fn ytmd_base() -> String {
    format!("http://{}:{}/api/v1", ytmd_host(), ytmd_port())
}
pub fn ytmd_realtime_base() -> String {
    format!("http://{}:{}", ytmd_host(), ytmd_port())
}
pub const fn ytmd_realtime_namespace() -> &'static str {
    "/api/v1/realtime"
}

/// `~/.ytmboard/token.json` — survives restarts so the ~30s Allow prompt
/// happens once. `~` resolves to %USERPROFILE% on Windows, $HOME elsewhere.
pub fn token_path() -> PathBuf {
    let home = std::env::var("YTMBOARD_TOKEN_PATH");
    if let Ok(explicit) = home {
        return PathBuf::from(explicit);
    }
    let base = std::env::var("USERPROFILE")
        .or_else(|_| std::env::var("HOME"))
        .unwrap_or_else(|_| ".".into());
    PathBuf::from(base).join(".ytmboard").join("token.json")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn base_urls_are_ipv4_and_versioned() {
        assert_eq!(ytmd_base(), "http://127.0.0.1:9863/api/v1");
        assert_eq!(ytmd_realtime_base(), "http://127.0.0.1:9863");
        assert_eq!(ytmd_realtime_namespace(), "/api/v1/realtime");
    }

    #[test]
    fn token_path_ends_with_dotdir() {
        let p = token_path();
        assert!(p.ends_with(".ytmboard/token.json") || p.ends_with(r".ytmboard\token.json"));
    }
}
