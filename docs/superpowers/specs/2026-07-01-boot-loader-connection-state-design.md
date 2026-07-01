# Boot loader + connection-state UX — design

Date: 2026-07-01

## Problem

On boot the device shows the **error** state first ("Can't reach your computer…"),
then flips to connected a few seconds later once the WebSocket comes up. It looks
broken every time it starts.

Root cause: the view-model exposes a single boolean, `host_connected`, and the UI
renders the disconnected banner whenever it is `false`. On boot
`net_backend_start()` initializes `host_connected = false`
([net_backend.c](../../../firmware/main/net_backend.c)), so the banner paints
*before the board↔bridge WebSocket has ever connected*. There is no way to tell
"still trying to connect" apart from "tried and failed".

## Two connection layers (important)

There are two independent links, and today they are conflated:

1. **Board ↔ bridge (WebSocket)** — "can the board reach your computer at all."
   Tracked only by `esp_websocket` events in net_backend. This is what the boot
   loader is about.
2. **`host_connected`** (payload field) — the bridge ↔ YouTube-Music-app link on
   the Mac ([normalize.js:62](../../../bridge/src/normalize.js)). `false` when
   YT Music isn't open. Drives today's banner.

This design adds explicit state for layer 1 and leaves layer 2 semantics intact.

## Behavior

- **Boot / reconnect:** show a loader (breathing rings + "Connecting to your
  computer…"). No error.
- **Grace period:** if the WebSocket link hasn't delivered a state frame within
  **15 s** (configurable), fall back to the error banner.
- **Mid-session drop:** WebSocket drop returns to the loader ("reconnecting"); the
  error banner reappears only if it stays down past the grace period. Smooths
  transient WiFi blips.
- **YT Music closed** (WS up, `host_connected=false`): unchanged — still shows the
  existing banner. The board *can* reach the computer, so this is not a loader case.

## Components

### 1. `conn` — pure connection-state FSM (`firmware/main/conn.{h,c}`)

Mirrors [idle.c](../../../firmware/main/idle.c): no ESP/LVGL includes, monotonic
`now_ms` passed in by the caller, host-testable standalone.

```c
typedef enum { CONN_CONNECTING, CONN_ONLINE, CONN_OFFLINE } conn_state_t;
typedef enum { CONN_EV_LINK_UP, CONN_EV_LINK_DOWN } conn_event_t;

void         conn_init(uint32_t grace_ms, uint32_t now_ms);  // -> CONNECTING, stamps connecting_since
void         conn_event(conn_event_t ev, uint32_t now_ms);   // link up / down
conn_state_t conn_tick(uint32_t now_ms);                     // applies grace timeout, returns state
conn_state_t conn_get(void);                                 // current state (inspection/tests)
```

Transitions:

| From        | Event / condition                                | To          |
|-------------|--------------------------------------------------|-------------|
| init        | `conn_init`                                      | CONNECTING  |
| any         | `CONN_EV_LINK_UP`                                | ONLINE      |
| any         | `CONN_EV_LINK_DOWN`                              | CONNECTING (re-stamp `connecting_since`) |
| CONNECTING  | `conn_tick` and `now − connecting_since ≥ grace` | OFFLINE     |

`conn_tick` only ever promotes CONNECTING→OFFLINE; ONLINE and OFFLINE are held
until the next event. Unsigned `now_ms` subtraction wraps safely, same as idle.c.

The module is not internally locked (like idle.c). net_backend serializes all
calls under its existing mutex — see below.

### 2. VM contract (`ui/now_playing_vm.h`)

Add one field:

```c
conn_state_t conn_state;   // board<->bridge link state (distinct from host_connected)
```

`host_connected` stays: it remains the bridge↔YouTube-Music status.

### 3. net_backend wiring (`firmware/main/net_backend.c`)

- `net_backend_start`: after allocating the mutex,
  `conn_init(CONFIG_YTM_CONNECT_GRACE_SEC * 1000, now_ms())`.
- `parse_state` (a real state frame arrived): `conn_event(CONN_EV_LINK_UP, now_ms())`
  inside the existing `s_lock` critical section. Leaving CONNECTING only on a real
  frame — not on the bare `WEBSOCKET_EVENT_CONNECTED` — avoids a second flash
  between socket-open and first data.
- `WEBSOCKET_EVENT_DISCONNECTED`: replace the `s_vm.host_connected = false` line
  with `conn_event(CONN_EV_LINK_DOWN, now_ms())` (still under `s_lock`).
- `net_backend_get_vm`: set `out->conn_state = conn_tick(now_ms())` inside the
  critical section, so the grace timeout advances every frame with no extra timer.
- `now_ms()`: `(uint32_t)(esp_timer_get_time() / 1000)`.

All `conn_*` calls happen under `s_lock`, so the FSM statics are single-writer safe
across the WebSocket task (events) and the LVGL task (tick).

### 4. UI (`ui/now_playing_screen.c`)

New loader overlay `s_loader`, built once in `now_playing_create`, hidden by default:

- Full-screen `lv_obj` on `s_screen`, opaque `COL_BG`, no border, covers everything.
- Top: small amber dot + "CONNECTING" label (mono, letter-spaced), matching the
  approved mock.
- Center: 3 concentric circles, `COL_PINK` borders at descending opacity, plus a
  filled center core with an `IC_*` device/link glyph.
- Below: caption label "Connecting to your computer…" in `COL_INK3`.

`now_playing_update` derivation changes to:

```c
bool connecting = vm->conn_state == CONN_CONNECTING;
bool disc       = vm->conn_state == CONN_OFFLINE || !vm->host_connected;
```

- `connecting` shows `s_loader` (and hides it otherwise); it takes precedence over
  every other mode.
- `disc` keeps driving the existing banner + OFFLINE status dot (covers both
  "link offline" and "music app closed").
- Breathing is driven per-frame from `s_pulse` (like the play-dot pulse): ring
  border opacity + a subtle `transform_scale` about a centered pivot. `connecting`
  is added to the change-gate exemption (alongside `buffering`) so the rings keep
  animating when the VM is otherwise static.

Perf: the loader is opaque and full-screen, so it covers the ambient-glow layer.
Per-frame invalidations recomposite only flat `COL_BG` + the small ring region — no
glow upscale beneath — so this stays within the render budget.

### 5. Sim / mock (`ui/mock.c`)

- `mock_init` / `enter_scene`: set `vm->conn_state = CONN_ONLINE` for normal scenes.
- `SC_DISCONNECTED` → `CONN_OFFLINE` (so the banner still shows in the sim).
- Add `SC_CONNECTING` scene → `CONN_CONNECTING`, so the loader cycles in the sim
  and can be previewed without hardware.

### 6. Config (`firmware/main/Kconfig.projbuild`)

```
config YTM_CONNECT_GRACE_SEC
    int "Seconds to show the connecting loader before the offline banner"
    range 3 60
    default 15
```

### 7. Tests (`tests/test_conn.c`)

Mirror [test_idle.c](../../../tests/test_idle.c). Pure, host-compiled. Cases:

- boot → CONNECTING immediately.
- frame before grace → ONLINE; no OFFLINE afterward.
- no frame, tick past grace → OFFLINE.
- ONLINE, LINK_DOWN → CONNECTING; tick past grace → OFFLINE.
- ONLINE, LINK_DOWN, LINK_UP within grace → ONLINE (no OFFLINE).
- grace boundary: exactly at `grace` → OFFLINE; one ms before → still CONNECTING.

## Out of scope

- Rewording the shared banner or splitting "network down" vs "music app closed"
  into distinct copy. The board can't distinguish network-down from bridge-down
  anyway, and layer-2 messaging is unchanged.
- Any WiFi-credential / provisioning UI.
