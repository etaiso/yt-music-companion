// conn.h — pure board<->bridge connection-state FSM (no ESP/LVGL; host-testable).
//
// Boot and reconnects start in CONNECTING; the UI shows a loader. A real state
// frame over the WebSocket promotes to ONLINE. If no frame arrives within the
// grace window, conn_tick() latches to OFFLINE so the UI shows the error banner.
// Mirrors idle.c: the caller passes a monotonic ms clock and serializes calls.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CONN_CONNECTING = 0,  // reaching the bridge (boot / reconnecting)
    CONN_ONLINE,          // bridge link up, state frame received
    CONN_OFFLINE          // no frame within the grace window; show error banner
} conn_state_t;

typedef enum {
    CONN_EV_LINK_UP,      // a state frame arrived over the WebSocket
    CONN_EV_LINK_DOWN     // the WebSocket dropped
} conn_event_t;

// Reset to CONNECTING and start the grace window at now_ms.
void conn_init(uint32_t grace_ms, uint32_t now_ms);

// Feed a link event. LINK_UP -> ONLINE; LINK_DOWN -> CONNECTING (restart grace).
void conn_event(conn_event_t ev, uint32_t now_ms);

// Advance the grace timeout and return the current state. While CONNECTING, once
// now_ms - connecting_since >= grace_ms the state latches to OFFLINE.
conn_state_t conn_tick(uint32_t now_ms);

// Current state without advancing time (inspection / tests).
conn_state_t conn_get(void);

#ifdef __cplusplus
}
#endif
