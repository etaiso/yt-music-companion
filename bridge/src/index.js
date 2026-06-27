#!/usr/bin/env node
// index.js — wires ytmdesktop -> normalize -> board (SPEC §3 architecture).
//
//   ytmdesktop ──Socket.IO/REST(127.0.0.1)── BRIDGE ──LAN ws── BOARD
//
// Core slice: auth + realtime state + normalize + board WS server + commands.
// Deferred to a later pass: cover-art RGB565 binary frames, mDNS discovery.
import { BoardServer } from "./board-server.js";
import { VOLUME_STEP } from "./config.js";
import { renderCover } from "./cover.js";
import { normalize } from "./normalize.js";
import { YtmdClient } from "./ytmd.js";

// board command -> ytmdesktop command (SPEC §7).
function mapCommand(cmd, arg, lastVm) {
  switch (cmd) {
    case "toggle_play":
      return { command: "playPause" };
    case "next":
      return { command: "next" };
    case "prev":
      return { command: "previous" };
    case "toggle_favorite":
      return { command: "toggleLike" };
    case "seek": {
      const dur = lastVm?.duration_sec ?? 0;
      const sec = Math.max(0, dur > 0 ? Math.min(Number(arg) || 0, dur) : Number(arg) || 0);
      return { command: "seekTo", data: sec };
    }
    case "volume_up":
      return { command: "volumeUp" };
    case "volume_down":
      return { command: "volumeDown" };
    default:
      return null;
  }
}

async function main() {
  let lastState = null; // last raw /state, for re-emitting on connectivity flips
  let lastVm = null; // last normalized vm, for seek clamping
  let debounceTimer = null;
  let lastCoverUrl = null; // dedupe: only re-render when the art actually changes

  const board = new BoardServer({
    onCommand: async (cmd, arg) => {
      const mapped = mapCommand(cmd, arg, lastVm);
      if (!mapped) {
        console.warn(`[cmd] unknown board command: ${cmd}`);
        return;
      }
      // volumeUp/Down exist; VOLUME_STEP is kept for setVolume fallback if a
      // future ytmdesktop drops the relative commands.
      void VOLUME_STEP;
      try {
        await ytmd.command(mapped.command, mapped.data);
        console.log(`[cmd] ${cmd}${arg !== undefined ? `(${arg})` : ""} -> ${mapped.command}`);
      } catch (err) {
        console.error(`[cmd] ${cmd} failed: ${err.message}`);
      }
    },
  });

  const pushVm = (connected) => {
    if (!lastState) return;
    const vm = normalize(lastState, { connected });
    if (!vm) return; // metadata not ready — skip (SPEC §4.3)
    lastVm = vm;
    board.broadcast(vm);
    maybePushCover(vm);
  };

  // Render + push cover art only when the URL changes. Ads have stale/no art —
  // skip them so the board holds the last good cover instead of flickering.
  const maybePushCover = (vm) => {
    if (vm.ad_playing) return;
    const url = vm.cover_url;
    if (url === lastCoverUrl) return;
    lastCoverUrl = url;
    if (!url) {
      board.broadcastCover(null); // no art for this track
      return;
    }
    renderCover(url)
      .then((frame) => {
        // A newer track may have superseded this render mid-flight — only push
        // if the url we rendered is still current.
        if (frame && url === lastCoverUrl) board.broadcastCover(frame);
      })
      .catch((err) => console.error(`[cover] ${err.message}`));
  };

  const ytmd = new YtmdClient({
    onState: (state) => {
      lastState = state;
      // Light debounce: collapse the burst ytmdesktop emits on a track change
      // into a single board update (SPEC §4.3).
      clearTimeout(debounceTimer);
      debounceTimer = setTimeout(() => pushVm(true), 120);
    },
    onConnected: () => pushVm(true),
    onDisconnected: () => {
      // Tell the board the host went away (shows disconnected state, SPEC §6).
      if (lastVm) board.broadcast({ ...lastVm, host_connected: false });
    },
  });

  board.start();
  await ytmd.start();

  const shutdown = () => {
    console.log("\nShutting down...");
    ytmd.stop();
    board.stop();
    process.exit(0);
  };
  process.on("SIGINT", shutdown);
  process.on("SIGTERM", shutdown);
}

main().catch((err) => {
  console.error("Fatal:", err.message);
  process.exit(1);
});
