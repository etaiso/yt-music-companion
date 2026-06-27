// config.js — single source of truth for bridge settings.
// Env overrides let you point at a non-default host/port without editing code.
import { homedir } from "node:os";
import { join } from "node:path";

// ytmdesktop Companion Server. IPv4 on purpose: "localhost" can resolve to IPv6,
// which the server does not listen on (SPEC §8).
export const YTMD_HOST = process.env.YTMD_HOST ?? "127.0.0.1";
export const YTMD_PORT = Number(process.env.YTMD_PORT ?? 9863);
export const YTMD_BASE = `http://${YTMD_HOST}:${YTMD_PORT}/api/v1`;
export const YTMD_REALTIME = `ws://${YTMD_HOST}:${YTMD_PORT}/api/v1/realtime`;

// This app's identity for the Companion auth handshake (SPEC §4).
// appId: lowercase alphanumeric 2-32; appName 2-48; appVersion semver.
export const APP_ID = process.env.YTMD_APP_ID ?? "ytmboard";
export const APP_NAME = "YT Music board";
export const APP_VERSION = "1.0.0";

// Where the persisted Companion token lives. Survives restarts so the 30s
// interactive Allow prompt only happens once.
export const TOKEN_PATH =
  process.env.YTMBOARD_TOKEN_PATH ?? join(homedir(), ".ytmboard", "token.json");

// Board-facing WebSocket server (SPEC §6). Plain WS, no auth, no TLS.
export const BOARD_PORT = Number(process.env.BOARD_PORT ?? 8765);

// Volume step for volume_up/down when falling back to setVolume (SPEC §7).
export const VOLUME_STEP = Number(process.env.VOLUME_STEP ?? 5);

// Cover-art slot on the board (SPEC §4.4): square, ~120px, pushed as RGB565.
export const COVER_PX = Number(process.env.COVER_PX ?? 120);
