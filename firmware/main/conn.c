// conn.c — pure connection-state FSM. No ESP/LVGL; host-testable.
#include "conn.h"

static conn_state_t s_state;
static uint32_t     s_grace_ms;
static uint32_t     s_connecting_since;

void conn_init(uint32_t grace_ms, uint32_t now_ms)
{
    s_state            = CONN_CONNECTING;
    s_grace_ms         = grace_ms;
    s_connecting_since = now_ms;
}

void conn_event(conn_event_t ev, uint32_t now_ms)
{
    switch (ev) {
    case CONN_EV_LINK_UP:
        s_state = CONN_ONLINE;
        break;
    case CONN_EV_LINK_DOWN:
        // Return to the loader and restart the grace window so a transient blip
        // doesn't jump straight to OFFLINE.
        s_state            = CONN_CONNECTING;
        s_connecting_since = now_ms;
        break;
    }
}

conn_state_t conn_tick(uint32_t now_ms)
{
    // Unsigned subtraction wraps safely on uint32 overflow (~49 days), as in idle.c.
    if (s_state == CONN_CONNECTING && (now_ms - s_connecting_since) >= s_grace_ms)
        s_state = CONN_OFFLINE;
    return s_state;
}

conn_state_t conn_get(void)
{
    return s_state;
}
