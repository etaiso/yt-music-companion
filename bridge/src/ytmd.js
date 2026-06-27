// ytmd.js — client for the ytmdesktop Companion Server (SPEC §2).
//
// Realtime via Socket.IO (NOT raw WS): connect to ws://127.0.0.1:9863/api/v1/realtime,
// websocket-only transport, token in the auth object. The `state-update` event
// carries the full /state object. REST /command is used for control actions; we
// never poll /state for realtime (rate-limited, SPEC §8) — the socket is the feed.
import { io } from "socket.io-client";
import { ensureToken, requestToken } from "./auth.js";
import { YTMD_BASE, YTMD_REALTIME } from "./config.js";

export class YtmdClient {
  constructor({ onState, onConnected, onDisconnected } = {}) {
    this.onState = onState ?? (() => {});
    this.onConnected = onConnected ?? (() => {});
    this.onDisconnected = onDisconnected ?? (() => {});
    this.token = null;
    this.socket = null;
  }

  async start() {
    this.token = await ensureToken();
    this.#connect();
  }

  #connect() {
    this.socket = io(YTMD_REALTIME, {
      transports: ["websocket"], // engine.io polling won't do here (SPEC §2)
      auth: { token: this.token },
      reconnection: true,
      reconnectionDelay: 1000,
      reconnectionDelayMax: 5000,
    });

    this.socket.on("connect", () => {
      console.log("[ytmd] socket connected");
      this.onConnected();
    });

    this.socket.on("state-update", (state) => this.onState(state));

    this.socket.on("disconnect", (reason) => {
      console.log(`[ytmd] socket disconnected: ${reason}`);
      this.onDisconnected();
    });

    this.socket.on("connect_error", async (err) => {
      console.error(`[ytmd] connect_error: ${err.message}`);
      this.onDisconnected();
      // Auth rejected (revoked/overwritten token) -> re-run the handshake once,
      // swap the token, let socket.io retry with it.
      if (/auth|token|unauthor/i.test(err.message)) {
        try {
          this.token = await requestToken();
          this.socket.auth = { token: this.token };
        } catch (e) {
          console.error(`[ytmd] re-auth failed: ${e.message}`);
        }
      }
    });
  }

  // POST /command {command, data?} with auth header (SPEC §7).
  async command(command, data) {
    const body = data === undefined ? { command } : { command, data };
    let res = await this.#postCommand(body);
    if (res.status === 401) {
      // Token died mid-session — re-auth and retry once.
      this.token = await requestToken();
      if (this.socket) this.socket.auth = { token: this.token };
      res = await this.#postCommand(body);
    }
    if (!res.ok) {
      const text = await res.text().catch(() => "");
      throw new Error(`command ${command} -> ${res.status} ${text}`);
    }
  }

  #postCommand(body) {
    return fetch(`${YTMD_BASE}/command`, {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        Authorization: this.token, // raw token, no "Bearer" (SPEC §2)
      },
      body: JSON.stringify(body),
    });
  }

  stop() {
    this.socket?.close();
  }
}
