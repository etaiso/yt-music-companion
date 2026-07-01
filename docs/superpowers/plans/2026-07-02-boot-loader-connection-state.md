# Boot loader + connection-state UX Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** On boot (and on reconnect) the device shows a breathing-ring loader and only falls back to the "can't reach your computer" banner after a 15 s grace period — instead of flashing the offline banner before the WebSocket ever connects.

**Architecture:** A new pure C FSM (`conn`) tracks the board↔bridge WebSocket link as `CONNECTING → ONLINE → OFFLINE`, driven by net_backend's WebSocket events and a grace timeout advanced each frame in `net_backend_get_vm`. The view-model gains a `conn_state` field (distinct from the existing `host_connected`, which is the bridge↔YouTube-Music link). The UI shows a full-screen loader overlay while `CONNECTING`, and the existing banner when `OFFLINE` or the music app is closed.

**Tech Stack:** C11, ESP-IDF v5.5 (esp_websocket_client, esp_timer), LVGL v9.2, CMake host tests (MinGW gcc), SDL2 desktop sim.

**Spec:** [docs/superpowers/specs/2026-07-01-boot-loader-connection-state-design.md](../specs/2026-07-01-boot-loader-connection-state-design.md)

**Windows toolchain notes (this machine):**
- Host unit tests compile with MinGW gcc at `C:\msys64\mingw64\bin` (off-PATH). Bash-tool commands below prepend it to `PATH`.
- Sim builds via the msys2 MinGW64 toolchain + SDL2 (see memory `sim-toolchain-installed.md`).
- Firmware builds via ESP-IDF activated with `Initialize-Idf.ps1 -IdfId …` (see memory `fw-build-gcc-ice-esp-lcd.md` / [RUNNING.md](../../RUNNING.md) §2.1).

---

## File structure

- **Create** `firmware/main/conn.h` — pure FSM interface (owns `conn_state_t`, `conn_event_t`).
- **Create** `firmware/main/conn.c` — pure FSM implementation.
- **Create** `tests/test_conn.c` — host unit test for the FSM.
- **Modify** `tests/CMakeLists.txt` — register `test_conn`.
- **Modify** `firmware/main/CMakeLists.txt` — add `conn.c` to the component sources.
- **Modify** `ui/now_playing_vm.h` — add `conn_state` field; include `conn.h`.
- **Modify** `ui/mock.c` — set `conn_state` per scene; add an `SC_CONNECTING` scene.
- **Modify** `firmware/main/Kconfig.projbuild` — add `YTM_CONNECT_GRACE_SEC`.
- **Modify** `firmware/main/net_backend.c` — wire WebSocket events → FSM; publish `conn_state`.
- **Modify** `ui/styles.h` — add a `COL_WARN` amber token for the CONNECTING status.
- **Modify** `ui/now_playing_screen.c` — build the loader overlay; drive it from `conn_state`.

---

## Task 1: Pure connection-state FSM (`conn`) + host tests

**Files:**
- Create: `firmware/main/conn.h`
- Create: `firmware/main/conn.c`
- Test: `tests/test_conn.c`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Create the FSM header**

Create `firmware/main/conn.h`:

```c
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
```

- [ ] **Step 2: Write the failing test**

Create `tests/test_conn.c`:

```c
// test_conn.c — host unit test for the pure connection-state FSM (no ESP/LVGL).
//   cc -std=c11 -I../firmware/main test_conn.c ../firmware/main/conn.c -o test_conn
#include "conn.h"
#include <stdio.h>
#include <stdlib.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...)                                                \
    do {                                                               \
        if (cond) { g_pass++; }                                        \
        else { g_fail++; printf("  FAIL (%s:%d): ", __FILE__, __LINE__); \
               printf(__VA_ARGS__); printf("\n"); }                    \
    } while (0)

int main(void)
{
    printf("# boot starts CONNECTING and holds until grace\n");
    conn_init(15000, 0);
    CHECK(conn_get() == CONN_CONNECTING, "boot connecting (got %d)", conn_get());
    CHECK(conn_tick(0) == CONN_CONNECTING, "t=0 connecting");
    CHECK(conn_tick(14999) == CONN_CONNECTING, "just under grace connecting");

    printf("# frame before grace -> ONLINE, stays online forever\n");
    conn_init(15000, 0);
    conn_event(CONN_EV_LINK_UP, 3000);
    CHECK(conn_tick(3000) == CONN_ONLINE, "frame -> online");
    CHECK(conn_tick(999999) == CONN_ONLINE, "online never times out");

    printf("# no frame past grace -> OFFLINE and latches\n");
    conn_init(15000, 0);
    CHECK(conn_tick(15000) == CONN_OFFLINE, "at grace -> offline");
    CHECK(conn_tick(20000) == CONN_OFFLINE, "stays offline");

    printf("# drop -> reconnecting, times out to OFFLINE from the drop\n");
    conn_init(15000, 0);
    conn_event(CONN_EV_LINK_UP, 2000);
    conn_event(CONN_EV_LINK_DOWN, 5000);
    CHECK(conn_get() == CONN_CONNECTING, "drop -> connecting");
    CHECK(conn_tick(19999) == CONN_CONNECTING, "within grace after drop");
    CHECK(conn_tick(20000) == CONN_OFFLINE, "past grace after drop -> offline");

    printf("# reconnect within grace -> ONLINE (no offline flash)\n");
    conn_init(15000, 0);
    conn_event(CONN_EV_LINK_UP, 2000);
    conn_event(CONN_EV_LINK_DOWN, 5000);
    conn_event(CONN_EV_LINK_UP, 8000);
    CHECK(conn_tick(30000) == CONN_ONLINE, "reconnected -> online");

    printf("# grace is measured from the drop, not from boot\n");
    conn_init(15000, 0);
    conn_event(CONN_EV_LINK_UP, 1000);
    conn_event(CONN_EV_LINK_DOWN, 100000);   // long session, then drop
    CHECK(conn_tick(100000) == CONN_CONNECTING, "just dropped, connecting");
    CHECK(conn_tick(114999) == CONN_CONNECTING, "within grace from drop");
    CHECK(conn_tick(115000) == CONN_OFFLINE, "grace from drop -> offline");

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? EXIT_FAILURE : EXIT_SUCCESS;
}
```

- [ ] **Step 3: Register the test in CMake**

In `tests/CMakeLists.txt`, append after the `test_idle` block (after line 66):

```cmake

# Connection-state FSM: pure C (no LVGL/ESP), compiled standalone.
add_executable(test_conn
    test_conn.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/main/conn.c
)
target_include_directories(test_conn PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/main)

if(NOT MSVC)
    target_compile_options(test_conn PRIVATE -Wall -Wextra)
endif()

add_test(NAME conn COMMAND test_conn)
```

- [ ] **Step 4: Run the test to verify it fails**

From the repo root (Bash tool):

```bash
export PATH="/c/msys64/mingw64/bin:$PATH"
mkdir -p .tmp-test
gcc -std=c11 -Wall -Wextra -Ifirmware/main tests/test_conn.c firmware/main/conn.c -o .tmp-test/test_conn.exe && .tmp-test/test_conn.exe
```

Expected: FAIL — link error `undefined reference to 'conn_init'` (conn.c does not exist yet).

- [ ] **Step 5: Implement the FSM**

Create `firmware/main/conn.c`:

```c
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
```

- [ ] **Step 6: Run the test to verify it passes**

```bash
export PATH="/c/msys64/mingw64/bin:$PATH"
gcc -std=c11 -Wall -Wextra -Ifirmware/main tests/test_conn.c firmware/main/conn.c -o .tmp-test/test_conn.exe && .tmp-test/test_conn.exe
```

Expected: `NN passed, 0 failed` and exit 0.

- [ ] **Step 7: Commit**

```bash
git add firmware/main/conn.h firmware/main/conn.c tests/test_conn.c tests/CMakeLists.txt
git commit -m "feat(firmware): pure connection-state FSM (conn) + host tests"
```

---

## Task 2: Add `conn_state` to the VM contract + build `conn.c` into firmware

This keeps every build green: the enum default (`CONN_CONNECTING = 0`) is correct for net_backend's zero-initialized snapshot, and the mock is set to `CONN_ONLINE` so the sim behaves exactly as before.

**Files:**
- Modify: `ui/now_playing_vm.h`
- Modify: `ui/mock.c:94-109` (`mock_init`), `ui/mock.c:121-145` (`enter_scene`)
- Modify: `firmware/main/CMakeLists.txt:4-9`

- [ ] **Step 1: Add the field to the VM**

In `ui/now_playing_vm.h`, add the include after the existing includes (after line 8 `#include <stdint.h>`):

```c
#include "conn.h"   // conn_state_t (board<->bridge link)
```

Then add the field immediately after the `host_connected` line (line 42):

```c
    bool        host_connected;    // false => bridge<->YouTube-Music link down
    conn_state_t conn_state;       // board<->bridge link: loader / online / offline
```

- [ ] **Step 2: Keep the mock ONLINE by default**

In `ui/mock.c`, in `mock_init` add after `vm->host_connected = true;` (line 103):

```c
    vm->conn_state    = CONN_ONLINE;
```

In `enter_scene`, add after `vm->host_connected = true;` (line 125):

```c
    vm->conn_state     = CONN_ONLINE;
```

And in the `switch` inside `enter_scene`, extend the `SC_DISCONNECTED` case (line 142) so the offline banner still shows under the new UI logic:

```c
        case SC_DISCONNECTED: vm->host_connected = false;
                              vm->conn_state = CONN_OFFLINE;   break;
```

- [ ] **Step 3: Compile `conn.c` into the firmware component**

In `firmware/main/CMakeLists.txt`, add `"conn.c"` to the `SRCS` list (after `"net_backend.c"`, line 6):

```cmake
    SRCS "main.c"
         "net_backend.c"
         "conn.c"
         "battery.c"
```

- [ ] **Step 4: Verify the sim still builds and behaves as before**

Build the sim (msys2 MinGW64 shell / PATH per the toolchain note). From `sim/`:

```bash
cmake -B build && cmake --build build
SIM_MAX_FRAMES=120 SDL_VIDEODRIVER=dummy ./build/ytm_sim.exe
```

Expected: builds clean; headless run renders 120 frames and exits `0`. (No visible change yet — this task is contract-only.)

- [ ] **Step 5: Commit**

```bash
git add ui/now_playing_vm.h ui/mock.c firmware/main/CMakeLists.txt
git commit -m "feat: add conn_state to the view-model contract"
```

---

## Task 3: Kconfig grace period + net_backend wiring

**Files:**
- Modify: `firmware/main/Kconfig.projbuild`
- Modify: `firmware/main/net_backend.c`

- [ ] **Step 1: Add the Kconfig option**

In `firmware/main/Kconfig.projbuild`, add this block after the `YTM_IDLE_DIM_PERCENT` option (after line 64), among the **unconditional** options. Do **not** add `depends on YTM_USE_NET`: `net_backend.c` is always compiled (only `main.c` guards the *calls* with `#if CONFIG_YTM_USE_NET`), so the macro must always be defined or the always-compiled `net_backend.c` fails to build. (The existing `YTM_WIFI_*` options *do* depend on `YTM_USE_NET` and would break a `USE_NET=n` build for the same reason — that path is simply never built. Don't copy that pattern here.)

```
config YTM_CONNECT_GRACE_SEC
    int "Seconds to show the connecting loader before the offline banner"
    range 3 60
    default 15
    help
        On boot (and after a mid-session WebSocket drop) the board shows a
        loader while it reaches the bridge. If no state frame arrives within
        this many seconds, it falls back to the offline banner.
```

- [ ] **Step 2: Include conn + esp_timer and add a clock helper**

In `firmware/main/net_backend.c`, add to the includes (after `#include "sdkconfig.h"`, line 24):

```c
#include "esp_timer.h"
#include "conn.h"
```

Add a monotonic-ms helper just after the `TAG` definition (after line 28):

```c
// Monotonic milliseconds for the connection FSM (esp_timer is us since boot).
static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
```

- [ ] **Step 3: Initialize the FSM at start**

In `net_backend_start`, after `s_vm.host_connected = false;` (line 331), add:

```c
    conn_init((uint32_t)CONFIG_YTM_CONNECT_GRACE_SEC * 1000, now_ms());
```

- [ ] **Step 4: Promote to ONLINE when a real state frame arrives**

In `parse_state`, inside the existing `s_lock` critical section, add just before `xSemaphoreGive(s_lock);` (line 189):

```c
    conn_event(CONN_EV_LINK_UP, now_ms());
```

- [ ] **Step 5: Return to the loader when the WebSocket drops**

In `ws_event`, replace the body of the `WEBSOCKET_EVENT_DISCONNECTED` case (lines 242-245) — swap the `host_connected = false` line for a link-down event (conn_state now drives the UI):

```c
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "bridge disconnected");
        xSemaphoreTake(s_lock, portMAX_DELAY);
        conn_event(CONN_EV_LINK_DOWN, now_ms());   // -> loader; grace then OFFLINE
        xSemaphoreGive(s_lock);
        break;
```

- [ ] **Step 6: Publish conn_state (and advance the grace timeout) each read**

In `net_backend_get_vm`, add inside the critical section after `*out = s_vm;` (line 350):

```c
    out->conn_state = conn_tick(now_ms());
```

- [ ] **Step 7: Verify the firmware builds**

Activate ESP-IDF (PowerShell, per RUNNING.md §2.1), then from `firmware/`:

```
idf.py build
```

Expected: build succeeds. (No unit test — the FSM logic is covered by Task 1; this is integration wiring.)

- [ ] **Step 8: Commit**

```bash
git add firmware/main/Kconfig.projbuild firmware/main/net_backend.c
git commit -m "feat(firmware): drive conn FSM from WebSocket events + grace Kconfig"
```

---

## Task 4: Add the amber CONNECTING token to the theme

**Files:**
- Modify: `ui/styles.h`

- [ ] **Step 1: Add `COL_WARN` to both theme blocks**

In `ui/styles.h`, in the **Light** block add after `COL_DANGER` (line 46):

```c
#define COL_WARN      lv_color_hex(0xB26B00)  // connecting / in-progress accent (amber)
```

In the **Dark** block add after `COL_DANGER` (line 66):

```c
#define COL_WARN      lv_color_hex(0xFFB020)  // connecting / in-progress accent (amber)
```

- [ ] **Step 2: Commit**

```bash
git add ui/styles.h
git commit -m "feat(ui): add COL_WARN amber token for the connecting state"
```

---

## Task 5: Build the loader overlay and drive it from `conn_state`

The loader is a full-screen opaque overlay on `lv_layer_top()` (above everything, independent of the screen's flex layout and padding). While `CONNECTING`, `now_playing_update` shows it, animates the rings from `s_pulse`, and returns early — so the per-frame rebuild (and the ambient-glow recomposite it triggers) never runs behind the opaque overlay. `s_force_full` guarantees one full rebuild on the frame the loader hides, so content is correct after connecting.

**Files:**
- Modify: `ui/now_playing_screen.c`

- [ ] **Step 1: Add module state for the loader**

In `ui/now_playing_screen.c`, after the `s_banner` declaration (line 34) add:

```c
static lv_obj_t  *s_loader;         // full-screen connecting overlay (layer_top)
static lv_obj_t  *s_load_ring[3];   // breathing rings
static bool       s_force_full;     // force one rebuild when leaving the loader
```

- [ ] **Step 2: Build the loader in `now_playing_create`**

In `now_playing_create`, add just before the final `return s_screen;` (line 436), after the disconnected-banner block:

```c
    // ---- connecting loader (breathing rings) ----
    // Full-screen opaque overlay on the top layer so boot no longer flashes the
    // offline banner: shown while conn_state == CONN_CONNECTING (see
    // docs/superpowers/specs/2026-07-01-boot-loader-connection-state-design.md).
    s_loader = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_loader);
    lv_obj_set_size(s_loader, lv_pct(100), lv_pct(100));
    lv_obj_center(s_loader);
    lv_obj_set_style_bg_color(s_loader, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_loader, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_loader, LV_OBJ_FLAG_SCROLLABLE);

    // amber "CONNECTING" status near the top
    lv_obj_t *lstat = lv_obj_create(s_loader);
    lv_obj_remove_style_all(lstat);
    lv_obj_set_size(lstat, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(lstat, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_flex_flow(lstat, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(lstat, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(lstat, 6, 0);
    lv_obj_clear_flag(lstat, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ldot = lv_obj_create(lstat);
    lv_obj_remove_style_all(ldot);
    lv_obj_set_size(ldot, 8, 8);
    lv_obj_set_style_radius(ldot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ldot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(ldot, COL_WARN, 0);

    lv_obj_t *ltxt = lv_label_create(lstat);
    lv_obj_set_style_text_font(ltxt, FONT_LABEL, 0);
    lv_obj_set_style_text_color(ltxt, COL_WARN, 0);
    lv_obj_set_style_text_letter_space(ltxt, 1, 0);
    lv_label_set_text(ltxt, "CONNECTING");

    // three concentric breathing rings, centered
    static const int ring_sz[3] = { 64, 104, 144 };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *r = lv_obj_create(s_loader);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, ring_sz[i], ring_sz[i]);
        lv_obj_center(r);
        lv_obj_set_style_radius(r, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(r, 3, 0);
        lv_obj_set_style_border_color(r, COL_PINK, 0);
        lv_obj_set_style_transform_pivot_x(r, lv_pct(50), 0);
        lv_obj_set_style_transform_pivot_y(r, lv_pct(50), 0);
        lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        s_load_ring[i] = r;
    }

    // center music glyph (bundled IC_MUSIC — no font regen needed)
    lv_obj_t *lcore = lv_label_create(s_loader);
    lv_label_set_text(lcore, IC_MUSIC);
    lv_obj_set_style_text_font(lcore, FONT_ICONS, 0);
    lv_obj_set_style_text_color(lcore, COL_PINK, 0);
    lv_obj_center(lcore);

    // caption
    lv_obj_t *lcap = lv_label_create(s_loader);
    lv_label_set_long_mode(lcap, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lcap, 320);
    lv_obj_set_style_text_align(lcap, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(lcap, FONT_META, 0);
    lv_obj_set_style_text_color(lcap, COL_INK3, 0);
    lv_label_set_text(lcap, "Connecting to your computer\xE2\x80\xA6");  // trailing ellipsis
    lv_obj_align(lcap, LV_ALIGN_CENTER, 0, 108);

    lv_obj_add_flag(s_loader, LV_OBJ_FLAG_HIDDEN);   // shown only while connecting
```

- [ ] **Step 3: Handle the connecting state at the top of `now_playing_update`**

In `now_playing_update`, immediately after `s_pulse++;` (line 441), before the `// ---- derive modes` comment, add:

```c
    // ---- connecting: show the loader, animate it, and skip the rebuild so the
    // per-frame glow recomposite doesn't run behind the opaque overlay ----
    bool connecting = vm->conn_state == CONN_CONNECTING;
    if (connecting) {
        if (lv_obj_has_flag(s_loader, LV_OBJ_FLAG_HIDDEN))
            lv_obj_clear_flag(s_loader, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < 3; i++) {
            float ph = (float)s_pulse * 0.10f - (float)i * 0.7f;  // staggered
            float a  = 0.5f + 0.5f * sinf(ph);                    // 0..1 breathing
            lv_obj_set_style_border_opa(s_load_ring[i],
                (lv_opa_t)((0.15f + 0.45f * a) * 255.0f), 0);
            lv_obj_set_style_transform_scale(s_load_ring[i],
                (int32_t)(230.0f + 40.0f * a), 0);                // ~0.90x..1.05x (256=1.0)
        }
        s_force_full = true;   // force a full rebuild on the frame we leave the loader
        return;
    }
    if (!lv_obj_has_flag(s_loader, LV_OBJ_FLAG_HIDDEN))
        lv_obj_add_flag(s_loader, LV_OBJ_FLAG_HIDDEN);
```

- [ ] **Step 4: Fold OFFLINE into the disconnected banner**

In `now_playing_update`, change the `disc` derivation (line 444) from:

```c
    bool disc      = !vm->host_connected;
```

to:

```c
    bool disc      = vm->conn_state == CONN_OFFLINE || !vm->host_connected;
```

- [ ] **Step 5: Honor `s_force_full` in the change-gate**

In `now_playing_update`, change the change-gate early-return (line 472) from:

```c
    if (!buffering && s_have_last && memcmp(&s_last, &cur, sizeof cur) == 0)
        return;
```

to:

```c
    if (!buffering && !s_force_full && s_have_last && memcmp(&s_last, &cur, sizeof cur) == 0)
        return;
    s_force_full = false;
```

- [ ] **Step 6: Build the sim and verify the firmware compiles**

Sim (from `sim/`):

```bash
cmake --build build
SIM_MAX_FRAMES=120 SDL_VIDEODRIVER=dummy ./build/ytm_sim.exe
```

Expected: builds clean; headless run exits `0`.

Firmware (ESP-IDF activated, from `firmware/`):

```
idf.py build
```

Expected: build succeeds.

- [ ] **Step 7: Commit**

```bash
git add ui/now_playing_screen.c
git commit -m "feat(ui): breathing-ring connecting loader driven by conn_state"
```

---

## Task 6: Sim preview scene for the loader

Adds a rotating `SC_CONNECTING` scene so the loader is visible in the sim without hardware or a real bridge.

**Files:**
- Modify: `ui/mock.c:69-77` (scene enum), `ui/mock.c:121-145` (`enter_scene`)

- [ ] **Step 1: Add the scene to the enum**

In `ui/mock.c`, add `SC_CONNECTING` to the `scene_t` enum before `SC__COUNT` (line 75-76):

```c
    SC_DISCONNECTED,
    SC_CONNECTING,
    SC__COUNT
```

- [ ] **Step 2: Handle it in `enter_scene`**

In the `enter_scene` switch, add a case after `SC_DISCONNECTED` (line 142):

```c
        case SC_CONNECTING:   vm->conn_state = CONN_CONNECTING;  break;
```

- [ ] **Step 3: Build and watch the loader cycle**

From `sim/`:

```bash
cmake --build build
./build/ytm_sim.exe
```

Expected: the scene rotation now includes a **CONNECTING** screen — three breathing pink rings around a music glyph, an amber "CONNECTING" label at the top, and "Connecting to your computer…" beneath — followed by the DISCONNECTED banner scene. (Headless alternative: `SIM_MAX_FRAMES=600 SDL_VIDEODRIVER=dummy ./build/ytm_sim.exe` exits `0`.)

- [ ] **Step 4: Commit**

```bash
git add ui/mock.c
git commit -m "test(sim): add a CONNECTING scene to preview the loader"
```

---

## Task 7: Full verification

- [ ] **Step 1: Run the full host test suite**

From the repo root (Bash tool):

```bash
export PATH="/c/msys64/mingw64/bin:$PATH"
cmake -G "MinGW Makefiles" -B build-tests tests && cmake --build build-tests && ctest --test-dir build-tests --output-on-failure
```

Expected: all tests pass, including `conn`. (If the CMake generator isn't available, the direct compile from Task 1 Step 6 is a sufficient check for `conn`.)

- [ ] **Step 2: Visual confirmation in the sim**

From `sim/`, run `./build/ytm_sim.exe` and confirm, over one full ~40 s scene cycle:
- The **CONNECTING** scene shows the breathing-ring loader (no banner).
- The **DISCONNECTED** scene shows the offline banner.
- All other scenes (playing / paused / buffering / no-track / ad) render normally with no loader.

- [ ] **Step 3: On-device confirmation (hardware)**

Flash and watch a cold boot (ESP-IDF activated, from `firmware/`):

```
idf.py -p <port> flash monitor
```

Expected on boot: the loader shows immediately (no offline-banner flash); it switches to Now Playing within a few seconds once the bridge connects. Pull the bridge (stop it) and confirm the loader reappears, then the banner after ~15 s. This is the acceptance check for the original bug.

---

## Self-review notes

- **Spec coverage:** conn FSM (Task 1) ✓; VM field (Task 2) ✓; net_backend wiring incl. LINK_UP-on-frame and get_vm tick (Task 3) ✓; Kconfig 15 s (Task 3) ✓; loader overlay + `disc` fold + change-gate exemption (Tasks 4–5) ✓; sim scene (Task 6) ✓; tests (Task 1) ✓.
- **`host_connected` unchanged semantics:** still drives the banner when the music app is closed while online (Task 5 Step 4). ✓
- **Type consistency:** `conn_state_t` / `conn_event_t` / `conn_init` / `conn_event` / `conn_tick` / `conn_get` used identically across conn.h, conn.c, test_conn.c, net_backend.c. Field name `conn_state` used identically across now_playing_vm.h, mock.c, net_backend.c, now_playing_screen.c. ✓
