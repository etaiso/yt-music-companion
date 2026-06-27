// discovery.js — mDNS advertisement so the board finds the Mac (SPEC §4.6).
//
// Advertises `_ytmboard._tcp` on the board-facing WS port. The board browses for
// this service and connects to the advertised host:port — no hard-coded IP.
import { Bonjour } from "bonjour-service";
import { BOARD_PORT } from "./config.js";

const SERVICE_TYPE = "ytmboard"; // -> _ytmboard._tcp
const SERVICE_NAME = "YT Music board bridge";

export class Discovery {
  constructor() {
    this.bonjour = null;
    this.service = null;
  }

  start() {
    this.bonjour = new Bonjour();
    this.service = this.bonjour.publish({
      name: SERVICE_NAME,
      type: SERVICE_TYPE,
      protocol: "tcp",
      port: BOARD_PORT,
      // Board reads these to know how to talk to us (plain WS, protocol v1).
      txt: { proto: "ws", path: "/", v: "1" },
    });
    console.log(`[mdns] advertising _${SERVICE_TYPE}._tcp on :${BOARD_PORT}`);
    return this.service;
  }

  stop() {
    // Send goodbye packets so boards drop us promptly, then tear down.
    try {
      this.bonjour?.unpublishAll(() => this.bonjour?.destroy());
    } catch {
      this.bonjour?.destroy?.();
    }
  }
}
