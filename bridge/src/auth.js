// auth.js — Companion Server auth handshake + token persistence (SPEC §2, §4).
//
// Flow (one-time, interactive):
//   1. POST /auth/requestcode {appId, appName, appVersion} -> {code}
//   2. POST /auth/request {appId, code} -> {token}
//      ^ user MUST click Allow in ytmdesktop; server times out after ~30s.
//   3. Authenticated requests send header `Authorization: <token>` (raw, no Bearer).
//
// Token is bound to appId; re-requesting overwrites. We persist it so the Allow
// prompt only happens once.
import { mkdir, readFile, writeFile } from "node:fs/promises";
import { dirname } from "node:path";
import {
  APP_ID,
  APP_NAME,
  APP_VERSION,
  TOKEN_PATH,
  YTMD_BASE,
} from "./config.js";

async function postJson(path, body) {
  const res = await fetch(`${YTMD_BASE}${path}`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
  if (!res.ok) {
    const text = await res.text().catch(() => "");
    throw new Error(`POST ${path} -> ${res.status} ${res.statusText} ${text}`);
  }
  return res.json();
}

export async function loadToken() {
  try {
    const raw = await readFile(TOKEN_PATH, "utf8");
    const { token, appId } = JSON.parse(raw);
    // Token is bound to appId; ignore a token saved under a different identity.
    if (token && appId === APP_ID) return token;
  } catch {
    /* no token yet */
  }
  return null;
}

export async function saveToken(token) {
  await mkdir(dirname(TOKEN_PATH), { recursive: true });
  await writeFile(
    TOKEN_PATH,
    JSON.stringify({ appId: APP_ID, token }, null, 2),
    { mode: 0o600 },
  );
}

// Runs the full interactive handshake and returns a fresh token.
export async function requestToken() {
  const { code } = await postJson("/auth/requestcode", {
    appId: APP_ID,
    appName: APP_NAME,
    appVersion: APP_VERSION,
  });
  console.log(
    `\n>>> ytmdesktop is asking you to allow "${APP_NAME}".` +
      `\n>>> Code: ${code}` +
      `\n>>> Click ALLOW in the app within ~30 seconds...\n`,
  );
  const { token } = await postJson("/auth/request", { appId: APP_ID, code });
  await saveToken(token);
  console.log("Authorized. Token saved.\n");
  return token;
}

// Returns a usable token: cached if present, else runs the handshake.
export async function ensureToken() {
  return (await loadToken()) ?? (await requestToken());
}

// `npm run auth` — force a fresh handshake (e.g. token revoked / lost).
if (import.meta.url === `file://${process.argv[1]}`) {
  requestToken().catch((err) => {
    console.error("Auth failed:", err.message);
    process.exit(1);
  });
}
