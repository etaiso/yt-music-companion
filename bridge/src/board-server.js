// board-server.js — the board-facing protocol (SPEC §6).
//
// Plain WebSocket so the board needs no Socket.IO / auth / TLS. On connect the
// board gets a full state snapshot; thereafter a fresh snapshot on every change.
// The board sends commands as { cmd, arg }. (Cover-art binary frames are a later
// pass — core slice ships state + commands only.)
//
// Wire format (text frames, JSON):
//   bridge -> board:  { "type": "state", "data": <vm> }
//   board  -> bridge: { "cmd": "toggle_play" | "next" | "prev" |
//                              "toggle_favorite" | "seek" | "volume_up" |
//                              "volume_down", "arg": <number?> }
import { WebSocketServer } from "ws";
import { BOARD_PORT } from "./config.js";

const HEARTBEAT_MS = 15_000;

export class BoardServer {
  constructor({ onCommand } = {}) {
    this.onCommand = onCommand ?? (() => {});
    this.latestVm = null;
    this.wss = null;
  }

  start() {
    this.wss = new WebSocketServer({ port: BOARD_PORT });
    console.log(`[board] WebSocket server on ws://0.0.0.0:${BOARD_PORT}`);

    this.wss.on("connection", (ws) => {
      console.log("[board] client connected");
      ws.isAlive = true;
      ws.on("pong", () => {
        ws.isAlive = true;
      });

      // Snapshot on connect so a freshly-attached board paints immediately.
      if (this.latestVm) ws.send(this.#frame(this.latestVm));

      ws.on("message", (raw) => this.#handleMessage(raw));
      ws.on("close", () => console.log("[board] client disconnected"));
      ws.on("error", (err) => console.error(`[board] client error: ${err.message}`));
    });

    // Liveness: drop boards that stopped answering pings (SPEC §6).
    this.heartbeat = setInterval(() => {
      for (const ws of this.wss.clients) {
        if (ws.isAlive === false) {
          ws.terminate();
          continue;
        }
        ws.isAlive = false;
        ws.ping();
      }
    }, HEARTBEAT_MS);
    this.heartbeat.unref?.();
  }

  #handleMessage(raw) {
    let msg;
    try {
      msg = JSON.parse(raw.toString());
    } catch {
      console.warn("[board] dropped non-JSON message");
      return;
    }
    if (!msg || typeof msg.cmd !== "string") {
      console.warn("[board] dropped message without cmd");
      return;
    }
    this.onCommand(msg.cmd, msg.arg);
  }

  // Push a new vm to every connected board, and remember it for late joiners.
  broadcast(vm) {
    this.latestVm = vm;
    if (!this.wss) return;
    const frame = this.#frame(vm);
    for (const ws of this.wss.clients) {
      if (ws.readyState === ws.OPEN) ws.send(frame);
    }
  }

  #frame(vm) {
    return JSON.stringify({ type: "state", data: vm });
  }

  stop() {
    clearInterval(this.heartbeat);
    this.wss?.close();
  }
}
