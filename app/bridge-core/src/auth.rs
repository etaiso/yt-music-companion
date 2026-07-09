//! Companion Server auth handshake + token persistence (ports auth.js).
//! Flow: POST /auth/requestcode -> {code}; user clicks Allow; POST /auth/request
//! -> {token}. Authenticated requests send `Authorization: <token>` (raw).
use crate::config::{token_path, ytmd_base, APP_ID, APP_NAME, APP_VERSION};
use anyhow::{anyhow, Context, Result};
use serde_json::{json, Value};

pub async fn load_token() -> Option<String> {
    let raw = tokio::fs::read_to_string(token_path()).await.ok()?;
    let v: Value = serde_json::from_str(&raw).ok()?;
    // Token is bound to appId; ignore a token saved under a different identity.
    if v.get("appId")?.as_str()? == APP_ID {
        return v.get("token")?.as_str().map(str::to_owned);
    }
    None
}

pub async fn save_token(token: &str) -> Result<()> {
    let path = token_path();
    if let Some(dir) = path.parent() {
        tokio::fs::create_dir_all(dir).await.ok();
    }
    let body = json!({ "appId": APP_ID, "token": token });
    tokio::fs::write(&path, serde_json::to_string_pretty(&body)?)
        .await
        .with_context(|| format!("writing token to {}", path.display()))?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let _ = tokio::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o600)).await;
    }
    Ok(())
}

async fn post_json(http: &reqwest::Client, path: &str, body: Value) -> Result<Value> {
    let url = format!("{}{}", ytmd_base(), path);
    Ok(http
        .post(&url)
        .json(&body)
        .send()
        .await
        .with_context(|| format!("POST {path}"))?
        .error_for_status()?
        .json()
        .await?)
}

/// Runs the full interactive handshake and returns a fresh token. `on_code` is
/// invoked with the code so the caller can surface it (print / notify).
pub async fn request_token(http: &reqwest::Client, on_code: impl Fn(&str)) -> Result<String> {
    let res = post_json(
        http,
        "/auth/requestcode",
        json!({ "appId": APP_ID, "appName": APP_NAME, "appVersion": APP_VERSION }),
    )
    .await
    .context("is ytmdesktop running with Companion Server AND authorization enabled?")?;
    let code = res
        .get("code")
        .and_then(Value::as_str)
        .ok_or_else(|| anyhow!("no `code` in requestcode response"))?;

    on_code(code);

    let res = post_json(http, "/auth/request", json!({ "appId": APP_ID, "code": code })).await?;
    let token = res
        .get("token")
        .and_then(Value::as_str)
        .ok_or_else(|| anyhow!("no `token` in request response"))?
        .to_owned();

    save_token(&token).await?;
    Ok(token)
}

/// Cached token if present, else run the handshake.
pub async fn ensure_token(http: &reqwest::Client, on_code: impl Fn(&str)) -> Result<String> {
    match load_token().await {
        Some(t) => Ok(t),
        None => request_token(http, on_code).await,
    }
}
