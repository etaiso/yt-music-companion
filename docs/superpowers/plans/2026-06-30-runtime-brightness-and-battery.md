# Runtime Brightness Control + Battery Indicator — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an on-device swipe-down brightness panel (NVS-persisted) and an AXP2101-sourced battery indicator in the status bar, both runnable in the desktop sim with stubs.

**Architecture:** Battery data flows board→UI through new VM fields filled by a polling AXP2101 driver (firmware) or mock values (sim); the status-bar widget renders them. Brightness is controlled through a new `quick_panel` LVGL overlay that calls a platform-supplied `brightness_cb_t` sink — firmware drives the CO5300 panel + persists to NVS, the sim stubs it. No bridge protocol changes.

**Tech Stack:** ESP-IDF v5.5, LVGL v9, C11, AXP2101 over I²C, NVS, esp_timer. Host tests via MinGW gcc; behavioral verification via the SDL sim.

**Design spec:** [docs/superpowers/specs/2026-06-30-runtime-brightness-and-battery-design.md](../specs/2026-06-30-runtime-brightness-and-battery-design.md)

---

## Conventions used in this plan

**Toolchain PATH (Windows, MinGW off-PATH).** Every host-test and sim command assumes this prefix in a Git Bash shell:

```bash
export PATH="/c/msys64/mingw64/bin:$PATH"
```

**Repo root.** All paths are relative to the worktree root (`.../worktrees/frosty-babbage-8b0bca`). Run commands from there unless a step says otherwise.

**Firmware build.** `idf.py` requires an ESP-IDF-activated shell (PowerShell: `Initialize-Idf.ps1 -IdfId <id>` then `cd firmware; idf.py build`). Firmware build/flash steps are marked **[hardware]** — they need the board and are verified manually. Sim + host steps need no hardware.

---

## File Structure

| File | Responsibility |
|------|----------------|
| `firmware/main/axp2101_decode.h` | **new** — AXP2101 register addresses + `battery_status_t` is in `battery.h`; declares the pure decode function. No ESP/LVGL deps (host-testable). |
| `firmware/main/axp2101_decode.c` | **new** — pure `axp2101_decode()`: raw register bytes → `battery_status_t`. |
| `firmware/main/battery.h` | **new** — `battery_status_t` type + `battery_start()` / `battery_get()`. Pure header (no ESP deps) so it can be included by the host test. |
| `firmware/main/battery.c` | **new** — AXP2101 I²C reads (reusing the BSP bus handle), fuel-gauge enable, 10 s poll task, lock-protected snapshot. ESP-IDF only; compiled into firmware, not the sim. |
| `tests/test_battery.c` | **new** — host unit test for `axp2101_decode()`. |
| `tests/CMakeLists.txt` | add `test_battery` target. |
| `ui/now_playing_vm.h` | add `battery_present` / `battery_percent` / `charging`. |
| `ui/now_playing_screen.c` | battery widget in the status bar; render the new VM fields in `now_playing_update()`. |
| `ui/quick_panel.h` | **new** — `brightness_cb_t` + `quick_panel_init()` + `quick_panel_set_battery()`. |
| `ui/quick_panel.c` | **new** — swipe-down overlay: gesture-to-open, brightness slider → sink, battery echo. |
| `ui/mock.c` | fill mock battery fields (slow drain) so the sim shows the indicator. |
| `sim/main_sim.c` | stub brightness sink + `quick_panel_init()` + feed battery to the panel. |
| `sim/CMakeLists.txt` | add `../ui/quick_panel.c`. |
| `firmware/main/main.c` | NVS brightness load/save + debounce, firmware brightness sink, `battery_start()`, fill VM battery in tick, `quick_panel_init()`. |
| `firmware/main/CMakeLists.txt` | add new sources + `esp_driver_i2c` / `esp_timer` REQUIRES. |
| `docs/CONFIGURATION.md` | note `YTM_DISPLAY_BRIGHTNESS` is now the first-boot default; NVS persists runtime changes. |

---

## Task 1: AXP2101 register decode (pure function + host test)

**Files:**
- Create: `firmware/main/battery.h`
- Create: `firmware/main/axp2101_decode.h`
- Create: `firmware/main/axp2101_decode.c`
- Test: `tests/test_battery.c`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Create the pure header `battery.h`**

```c
// battery.h — board battery status (AXP2101). Pure header: no ESP/LVGL deps so
// the decode logic is host-testable. battery.c provides the ESP-IDF driver.
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool present;    // battery detected on the MX1.25 connector
    int  percent;    // 0..100 state of charge (valid when present)
    bool charging;   // USB present and actively charging
} battery_status_t;

// Start the AXP2101 poll task (call once after the BSP I2C bus is up).
void battery_start(void);

// Copy the latest cached reading. Safe to call from the LVGL tick.
void battery_get(battery_status_t *out);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Create `axp2101_decode.h`**

```c
// axp2101_decode.h — pure decode of AXP2101 status registers into battery_status_t.
// Register addresses per the X-Powers AXP2101 datasheet (V1.x):
//   0x00 PMU status 1  — bit 3 = battery present/online
//   0x01 PMU status 2  — bits [6:5] charge state: 01=charging, 10=discharging
//   0xA4 fuel-gauge SOC — battery percentage 0..100 (read-only)
#pragma once

#include <stdint.h>
#include "battery.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AXP2101_I2C_ADDR        0x34
#define AXP2101_REG_STATUS1     0x00
#define AXP2101_REG_STATUS2     0x01
#define AXP2101_REG_GAUGE_CFG   0x18  // bit 3 enables the fuel gauge
#define AXP2101_REG_BAT_PERCENT 0xA4

// Decode raw register bytes into out. Pure; no I/O.
void axp2101_decode(uint8_t status1, uint8_t status2, uint8_t soc_pct,
                    battery_status_t *out);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 3: Write the failing test `tests/test_battery.c`**

```c
// test_battery.c — host unit test for axp2101_decode() (pure, no ESP/LVGL).
//   gcc -std=c11 -Wall -Wextra -Ifirmware/main \
//       tests/test_battery.c firmware/main/axp2101_decode.c -o tb.exe && ./tb.exe
#include "axp2101_decode.h"
#include <stdio.h>
#include <stdlib.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...)                                            \
    do {                                                            \
        if (cond) { g_pass++; }                                     \
        else { g_fail++; printf("  FAIL (%s:%d): ", __FILE__, __LINE__); \
               printf(__VA_ARGS__); printf("\n"); }                 \
    } while (0)

static battery_status_t dec(uint8_t s1, uint8_t s2, uint8_t soc)
{
    battery_status_t b;
    axp2101_decode(s1, s2, soc, &b);
    return b;
}

int main(void)
{
    printf("# present flag = status1 bit3\n");
    CHECK(dec(0x08, 0, 50).present == true,  "bit3 set -> present");
    CHECK(dec(0x00, 0, 50).present == false, "bit3 clear -> absent");

    printf("# charging = status2 bits[6:5] == 0b01\n");
    CHECK(dec(0x08, 0x20, 50).charging == true,  "0b01 -> charging");   // 0x20 = 001 -> bits[6:5]=01
    CHECK(dec(0x08, 0x40, 50).charging == false, "0b10 -> discharging");// 0x40 = 010
    CHECK(dec(0x08, 0x00, 50).charging == false, "0b00 -> not charging");

    printf("# percent passthrough + clamp 0..100\n");
    CHECK(dec(0x08, 0, 73).percent == 73,  "soc 73 -> 73");
    CHECK(dec(0x08, 0, 0).percent  == 0,   "soc 0 -> 0");
    CHECK(dec(0x08, 0, 100).percent== 100, "soc 100 -> 100");
    CHECK(dec(0x08, 0, 200).percent== 100, "soc 200 (garbage) -> clamped 100");

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? EXIT_FAILURE : EXIT_SUCCESS;
}
```

- [ ] **Step 4: Run the test to verify it fails to link**

```bash
export PATH="/c/msys64/mingw64/bin:$PATH"
gcc -std=c11 -Wall -Wextra -Ifirmware/main tests/test_battery.c firmware/main/axp2101_decode.c -o tb.exe && ./tb.exe
```
Expected: FAIL — link error, `undefined reference to 'axp2101_decode'` (the .c is empty).

- [ ] **Step 5: Implement `axp2101_decode.c`**

```c
#include "axp2101_decode.h"

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

void axp2101_decode(uint8_t status1, uint8_t status2, uint8_t soc_pct,
                    battery_status_t *out)
{
    out->present  = (status1 & (1u << 3)) != 0u;
    out->charging = (((status2 >> 5) & 0x3u) == 0x1u);
    out->percent  = clampi((int)soc_pct, 0, 100);
}
```

- [ ] **Step 6: Run the test to verify it passes**

```bash
export PATH="/c/msys64/mingw64/bin:$PATH"
gcc -std=c11 -Wall -Wextra -Ifirmware/main tests/test_battery.c firmware/main/axp2101_decode.c -o tb.exe && ./tb.exe
```
Expected: PASS — `10 passed, 0 failed`, exit 0.

- [ ] **Step 7: Wire the test into `tests/CMakeLists.txt`**

Add after the `test_ambient` block (before any trailing content):

```cmake
# AXP2101 decode: pure C (no LVGL/ESP), compiled standalone.
add_executable(test_battery
    test_battery.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/main/axp2101_decode.c
)
target_include_directories(test_battery PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/main)

if(NOT MSVC)
    target_compile_options(test_battery PRIVATE -Wall -Wextra)
endif()

add_test(NAME battery COMMAND test_battery)
```

- [ ] **Step 8: Verify via ctest**

```bash
export PATH="/c/msys64/mingw64/bin:$PATH"
cmake -S tests -B tests/build -G Ninja -DCMAKE_C_COMPILER=gcc && cmake --build tests/build && (cd tests/build && ctest --output-on-failure -R battery)
```
Expected: `100% tests passed, 0 tests failed out of 1`.

- [ ] **Step 9: Commit**

```bash
rm -f tb.exe
git add firmware/main/battery.h firmware/main/axp2101_decode.h firmware/main/axp2101_decode.c tests/test_battery.c tests/CMakeLists.txt
git commit -m "feat(battery): pure AXP2101 status decode + host test"
```

---

## Task 2: AXP2101 I²C driver + poll task (firmware)

**Files:**
- Create: `firmware/main/battery.c`

No host test — this layer is I²C + RTOS and is verified on hardware (Task 6, **[hardware]**). Keep all decode logic in `axp2101_decode()` (already tested); this file is just I/O + caching.

- [ ] **Step 1: Implement `battery.c`**

```c
// battery.c — AXP2101 fuel-gauge reader. Reuses the BSP I2C bus (shared with the
// touch controller). Polls every 10 s and caches a lock-protected snapshot.
#include "battery.h"
#include "axp2101_decode.h"

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "battery";

#define POLL_MS 10000

static battery_status_t s_status;                 // last reading
static portMUX_TYPE     s_mux = portMUX_INITIALIZER_UNLOCKED;
static i2c_master_dev_handle_t s_dev;

static esp_err_t rd(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, pdMS_TO_TICKS(100));
}

static void poll_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint8_t s1 = 0, s2 = 0, soc = 0;
        esp_err_t e1 = rd(AXP2101_REG_STATUS1, &s1);
        esp_err_t e2 = rd(AXP2101_REG_STATUS2, &s2);
        esp_err_t e3 = rd(AXP2101_REG_BAT_PERCENT, &soc);

        if (e1 == ESP_OK && e2 == ESP_OK && e3 == ESP_OK) {
            battery_status_t b;
            axp2101_decode(s1, s2, soc, &b);
            portENTER_CRITICAL(&s_mux);
            s_status = b;
            portEXIT_CRITICAL(&s_mux);
        } else {
            ESP_LOGW(TAG, "AXP2101 read failed (%d/%d/%d)", e1, e2, e3);
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void battery_start(void)
{
    // Reuse the BSP-initialized I2C master bus (touch shares it).
    // NOTE: confirm the BSP accessor name against the managed component; on this
    // BSP it is bsp_i2c_get_handle(). If absent, fall back to bsp_i2c_init() +
    // i2c_master_get_bus_handle(BSP_I2C_NUM, &bus).
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = AXP2101_I2C_ADDR,
        .scl_speed_hz    = 400000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &cfg, &s_dev));

    // Enable the fuel gauge (reg 0x18 bit 3) so reg 0xA4 reports SOC.
    uint8_t cfgreg = 0;
    if (rd(AXP2101_REG_GAUGE_CFG, &cfgreg) == ESP_OK) {
        uint8_t buf[2] = { AXP2101_REG_GAUGE_CFG, (uint8_t)(cfgreg | (1u << 3)) };
        i2c_master_transmit(s_dev, buf, 2, pdMS_TO_TICKS(100));
    }

    xTaskCreate(poll_task, "battery", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "AXP2101 battery poll started (%d ms)", POLL_MS);
}

void battery_get(battery_status_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_status;
    portEXIT_CRITICAL(&s_mux);
}
```

- [ ] **Step 2: Add sources + components to `firmware/main/CMakeLists.txt`**

In the `SRCS` list add (after `"net_backend.c"`):

```cmake
         "battery.c"
         "axp2101_decode.c"
         "${UI_DIR}/quick_panel.c"
```

In `REQUIRES` add `esp_driver_i2c` and `esp_timer` (keep the existing entries):

```cmake
    REQUIRES esp32_s3_touch_amoled_2_16
             esp_websocket_client esp_wifi mdns json nvs_flash esp_netif esp_event
             esp_driver_i2c esp_timer
```

(`quick_panel.c` is added now so later firmware builds compile; it is created in Task 4. Building firmware before Task 4 is not required — firmware verification is Task 6.)

- [ ] **Step 3: Commit**

```bash
git add firmware/main/battery.c firmware/main/CMakeLists.txt
git commit -m "feat(battery): AXP2101 I2C poll task over the shared BSP bus"
```

---

## Task 3: Battery view-model fields + status-bar widget + mock feed

**Files:**
- Modify: `ui/now_playing_vm.h`
- Modify: `ui/now_playing_screen.c`
- Modify: `ui/mock.c`

This is sim-verifiable: the indicator and its states show in the SDL window driven by mock data.

- [ ] **Step 1: Add VM fields in `ui/now_playing_vm.h`**

Add inside `now_playing_vm_t`, right after `bool host_connected;`:

```c
    // board-local device status (filled by battery.c on hardware, mock.c in sim;
    // NOT from the bridge). present=false hides the indicator.
    bool        battery_present;
    int         battery_percent;   // 0..100
    bool        charging;          // USB present and charging
```

- [ ] **Step 2: Declare the battery widget statics + colors in `ui/now_playing_screen.c`**

Near the other `static lv_obj_t *s_*` declarations at the top of the file, add:

```c
static lv_obj_t *s_batt_box;    // battery outline container (status bar, right)
static lv_obj_t *s_batt_fill;   // inner fill bar (width = percent)
static lv_obj_t *s_batt_label;  // "NN%" text

#define BATTERY_LOW_PCT 20
```

- [ ] **Step 3: Build the battery widget in the status bar**

In `now_playing_create()`, inside the right-hand status group `st` (immediately after the `s_state_label` block, before the hero section at `// ---- hero:`), add:

```c
    // ---- battery indicator (board-local; hidden when no battery present) ----
    s_batt_label = lv_label_create(st);
    lv_obj_set_style_text_font(s_batt_label, FONT_LABEL, 0);
    lv_obj_set_style_text_color(s_batt_label, COL_INK3, 0);
    lv_obj_set_style_text_letter_space(s_batt_label, 1, 0);
    lv_label_set_text(s_batt_label, "");

    s_batt_box = lv_obj_create(st);
    lv_obj_remove_style_all(s_batt_box);
    lv_obj_set_size(s_batt_box, 22, 12);
    lv_obj_set_style_radius(s_batt_box, 3, 0);
    lv_obj_set_style_border_width(s_batt_box, 1, 0);
    lv_obj_set_style_border_color(s_batt_box, COL_INK3, 0);
    lv_obj_set_style_pad_all(s_batt_box, 2, 0);
    lv_obj_clear_flag(s_batt_box, LV_OBJ_FLAG_SCROLLABLE);

    s_batt_fill = lv_obj_create(s_batt_box);
    lv_obj_remove_style_all(s_batt_fill);
    lv_obj_set_height(s_batt_fill, lv_pct(100));
    lv_obj_set_width(s_batt_fill, lv_pct(60));
    lv_obj_set_style_radius(s_batt_fill, 1, 0);
    lv_obj_set_style_bg_opa(s_batt_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_batt_fill, COL_INK3, 0);
    lv_obj_align(s_batt_fill, LV_ALIGN_LEFT_MID, 0, 0);
```

- [ ] **Step 4: Render battery state in `now_playing_update()`**

At the end of `now_playing_update()`, add:

```c
    // ---- battery indicator ----
    if (!vm->battery_present) {
        lv_obj_add_flag(s_batt_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_batt_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(s_batt_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_batt_label, LV_OBJ_FLAG_HIDDEN);

        int pct = vm->battery_percent;
        if (pct < 0) pct = 0; else if (pct > 100) pct = 100;
        lv_label_set_text_fmt(s_batt_label, "%d%%", pct);
        lv_obj_set_width(s_batt_fill, lv_pct(pct < 8 ? 8 : pct)); // keep a sliver visible

        lv_color_t c;
        if (vm->charging)             c = lv_color_hex(0x5AD17A); // green
        else if (pct <= BATTERY_LOW_PCT) c = lv_color_hex(0xF59E0B); // amber
        else                          c = COL_INK3;               // normal
        lv_obj_set_style_bg_color(s_batt_fill, c, 0);
        lv_obj_set_style_text_color(s_batt_label, c, 0);
    }
```

- [ ] **Step 5: Feed mock battery values in `ui/mock.c`**

In `mock_init()`, after `vm->host_connected = true;` add:

```c
    vm->battery_present = true;
    vm->battery_percent = 64;
    vm->charging        = false;
```

In `mock_tick()`, after the `s_t += ...` line, add a slow drain + charge flip so the states are visible:

```c
    // slow battery animation for the sim: drain, then "plug in" and recharge
    static float batt_acc; static bool batt_chg;
    batt_acc += dt_ms / 1000.0f;
    if (batt_acc >= 2.0f) {            // step every 2 s
        batt_acc = 0;
        if (batt_chg) {
            if (++vm->battery_percent >= 100) batt_chg = false;
        } else {
            if (--vm->battery_percent <= 10) batt_chg = true;
        }
        vm->charging = batt_chg;
    }
    vm->battery_present = true;
```

- [ ] **Step 6: Build and run the sim to verify the indicator**

```bash
export PATH="/c/msys64/mingw64/bin:$PATH"
cd sim && cmake -B build -G Ninja -DCMAKE_C_COMPILER=gcc && cmake --build build
SIM_MAX_FRAMES=300 SDL_VIDEODRIVER=dummy ./build/ytm_sim.exe
```
Expected: builds clean; headless run prints `headless: 300 frames rendered, exiting.` and exits 0. For a visual check, run `./build/ytm_sim.exe` (no env) and confirm a battery box + `%` appears at the top-right, the fill shrinks/grows, and color goes amber under 20% / green while "charging".

- [ ] **Step 7: Commit**

```bash
cd ..
git add ui/now_playing_vm.h ui/now_playing_screen.c ui/mock.c
git commit -m "feat(ui): battery indicator in the status bar + mock feed"
```

---

## Task 4: Quick-settings panel (brightness) + sim wiring

**Files:**
- Create: `ui/quick_panel.h`
- Create: `ui/quick_panel.c`
- Modify: `sim/CMakeLists.txt`
- Modify: `sim/main_sim.c`

Sim-verifiable: swipe down in the SDL window opens the panel; dragging the slider logs the new brightness.

- [ ] **Step 1: Create `ui/quick_panel.h`**

```c
// quick_panel.h — swipe-down quick-settings overlay (brightness slider + battery
// echo). Lives on lv_layer_top() above the Now Playing screen. The brightness
// value is applied through a platform-supplied sink (firmware: panel + NVS; sim:
// stub), so this file never calls bsp_*.
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*brightness_cb_t)(int percent);

// Build the panel and install the open gesture on `screen`. `cb` is called with
// the new percent (5..100) whenever the slider moves; the slider starts at
// `initial_percent`.
void quick_panel_init(lv_obj_t *screen, brightness_cb_t cb, int initial_percent);

// Update the panel's battery echo (no-op visually unless the panel is open).
void quick_panel_set_battery(int percent, bool charging, bool present);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Create `ui/quick_panel.c`**

```c
#include "quick_panel.h"
#include "styles.h"
#include <stdio.h>

static lv_obj_t       *s_backdrop;   // full-screen dim; tap to close
static lv_obj_t       *s_panel;      // the sliding sheet
static lv_obj_t       *s_slider;
static lv_obj_t       *s_pct;        // "NN%" beside the slider
static lv_obj_t       *s_batt_echo;  // "Battery NN% · charging"
static brightness_cb_t s_cb;

static void open_panel(void)  { lv_obj_clear_flag(s_backdrop, LV_OBJ_FLAG_HIDDEN); }
static void close_panel(void) { lv_obj_add_flag(s_backdrop, LV_OBJ_FLAG_HIDDEN); }

static void backdrop_cb(lv_event_t *e)
{
    // tap on the dim area (not the panel) closes
    if (lv_event_get_target(e) == s_backdrop) close_panel();
}

static void slider_cb(lv_event_t *e)
{
    (void)e;
    int v = (int)lv_slider_get_value(s_slider);
    lv_label_set_text_fmt(s_pct, "%d%%", v);
    if (s_cb) s_cb(v);
}

static void gesture_cb(lv_event_t *e)
{
    (void)e;
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_BOTTOM) open_panel();
}

void quick_panel_init(lv_obj_t *screen, brightness_cb_t cb, int initial_percent)
{
    s_cb = cb;

    // open gesture: a downward swipe anywhere on the screen
    lv_obj_add_event_cb(screen, gesture_cb, LV_EVENT_GESTURE, NULL);

    // backdrop on the top layer, hidden until opened
    s_backdrop = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_backdrop);
    lv_obj_set_size(s_backdrop, 480, 480);
    lv_obj_set_style_bg_color(s_backdrop, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_backdrop, LV_OPA_50, 0);
    lv_obj_add_flag(s_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_backdrop, backdrop_cb, LV_EVENT_CLICKED, NULL);

    // the sheet
    s_panel = lv_obj_create(s_backdrop);
    lv_obj_remove_style_all(s_panel);
    lv_obj_set_size(s_panel, 480, 172);
    lv_obj_align(s_panel, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(s_panel, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_panel, 22, 0);
    lv_obj_set_style_pad_all(s_panel, 22, 0);
    lv_obj_set_flex_flow(s_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_panel, 14, 0);

    // brightness row: slider + percent
    s_slider = lv_slider_create(s_panel);
    lv_obj_set_width(s_slider, lv_pct(80));
    lv_slider_set_range(s_slider, 5, 100);
    lv_slider_set_value(s_slider, initial_percent, LV_ANIM_OFF);
    lv_obj_add_event_cb(s_slider, slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_pct = lv_label_create(s_panel);
    lv_obj_set_style_text_font(s_pct, FONT_LABEL, 0);
    lv_obj_set_style_text_color(s_pct, COL_INK1, 0);
    lv_label_set_text_fmt(s_pct, "%d%%", initial_percent);

    s_batt_echo = lv_label_create(s_panel);
    lv_obj_set_style_text_font(s_batt_echo, FONT_LABEL, 0);
    lv_obj_set_style_text_color(s_batt_echo, COL_INK3, 0);
    lv_label_set_text(s_batt_echo, "");
}

void quick_panel_set_battery(int percent, bool charging, bool present)
{
    if (!s_batt_echo) return;
    if (!present) { lv_label_set_text(s_batt_echo, "No battery"); return; }
    lv_label_set_text_fmt(s_batt_echo, "Battery %d%%%s", percent,
                          charging ? " \xC2\xB7 charging" : "");
}
```

> **Note on `COL_BG` / `COL_INK1` / `FONT_LABEL`:** these come from `styles.h`. Open `ui/styles.h` and use the exact token names it defines for the screen background, primary ink, and the small label font (the status bar uses `FONT_LABEL` and `COL_INK3`/`COL_INK1`). If a name differs, match the existing usage in `now_playing_screen.c`.

- [ ] **Step 3: Add `quick_panel.c` to the sim build (`sim/CMakeLists.txt`)**

In the `add_executable(ytm_sim ...)` source list, after the `now_playing_screen.c` line add:

```cmake
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/quick_panel.c
```

- [ ] **Step 4: Wire the panel into the sim (`sim/main_sim.c`)**

Add the include near the others:

```c
#include "quick_panel.h"
```

Add a stub sink above `main()`:

```c
static void sim_brightness(int percent)
{
    printf("brightness: %d%%\n", percent);
    fflush(stdout);
}
```

In `main()`, after `now_playing_create(lv_screen_active());` add:

```c
    quick_panel_init(lv_screen_active(), sim_brightness, 40);
```

In `tick_cb()`, after `now_playing_update(&s_vm);` add:

```c
    quick_panel_set_battery(s_vm.battery_percent, s_vm.charging, s_vm.battery_present);
```

- [ ] **Step 5: Build + headless-verify the sim**

```bash
export PATH="/c/msys64/mingw64/bin:$PATH"
cd sim && cmake --build build
SIM_MAX_FRAMES=300 SDL_VIDEODRIVER=dummy ./build/ytm_sim.exe
```
Expected: builds clean; headless run exits 0.

- [ ] **Step 6: Interactive verification**

Run `./build/ytm_sim.exe` (no env vars). With the mouse, **drag downward** from near the top of the window: the dim backdrop + brightness sheet appear. Drag the slider — the terminal prints `brightness: NN%` and the `NN%` label tracks. Click the dimmed area outside the sheet — it closes. Confirm the battery echo text shows while open.

- [ ] **Step 7: Commit**

```bash
cd ..
git add ui/quick_panel.h ui/quick_panel.c sim/CMakeLists.txt sim/main_sim.c
git commit -m "feat(ui): swipe-down quick panel with brightness slider (sim-wired)"
```

---

## Task 5: Firmware wiring — NVS persistence, sink, battery, panel

**Files:**
- Modify: `firmware/main/main.c`

**[hardware]** for runtime verification, but the code must compile in the firmware build (Task 6).

- [ ] **Step 1: Add includes to `main.c`**

After the existing includes, add:

```c
#include "battery.h"
#include "quick_panel.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_timer.h"
```

- [ ] **Step 2: Add NVS brightness load/save + debounced sink**

Above `app_main()` (after the `tick_cb` definition), add:

```c
#define NVS_NS         "ytm"
#define NVS_KEY_BRIGHT "bright"

static int brightness_load(void)
{
    int32_t v = CONFIG_YTM_DISPLAY_BRIGHTNESS;   // first-boot default
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        int32_t stored;
        if (nvs_get_i32(h, NVS_KEY_BRIGHT, &stored) == ESP_OK) v = stored;
        nvs_close(h);
    }
    return (int)v;
}

static void brightness_save(int pct)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, NVS_KEY_BRIGHT, pct);
        nvs_commit(h);
        nvs_close(h);
    }
}

static esp_timer_handle_t s_bright_timer;
static int                s_pending_bright;

static void bright_save_cb(void *arg)   // fires 500 ms after the last change
{
    (void)arg;
    brightness_save(s_pending_bright);
    ESP_LOGI(TAG, "brightness persisted: %d%%", s_pending_bright);
}

static void fw_brightness(int percent)
{
    bsp_display_brightness_set(percent);   // apply immediately
    s_pending_bright = percent;
    esp_timer_stop(s_bright_timer);        // debounce flash writes
    esp_timer_start_once(s_bright_timer, 500 * 1000);
}
```

- [ ] **Step 3: Update `app_main()` boot sequence**

Replace the brightness line `bsp_display_brightness_set(CONFIG_YTM_DISPLAY_BRIGHTNESS);` (currently `main.c:52`) with NVS-restored brightness, and start the new subsystems. The block from after `bsp_display_backlight_on();` becomes:

```c
    bsp_display_backlight_on();

    // NVS: brightness persistence (safe if net_backend also inits NVS — returns
    // ESP_OK when already initialized).
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    int initial_bright = brightness_load();
    // AMOLED has no PWM backlight; this sends the panel's brightness command.
    // Boot value is NVS (last runtime choice) or CONFIG_YTM_DISPLAY_BRIGHTNESS on
    // first boot. Tunable live via the swipe-down panel.
    bsp_display_brightness_set(initial_bright);

    const esp_timer_create_args_t targs = { .callback = bright_save_cb, .name = "brt" };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_bright_timer));

    battery_start();
```

(Leave the `#if CONFIG_YTM_USE_NET ... net_backend_start();` block that follows as-is.)

- [ ] **Step 4: Install the panel after the screen is built**

Inside the `bsp_display_lock(0); ... bsp_display_unlock();` block, after `now_playing_create(lv_screen_active());` add:

```c
    quick_panel_init(lv_screen_active(), fw_brightness, initial_bright);
```

- [ ] **Step 5: Fill VM battery fields each tick**

In `tick_cb()`, after the feed fills `s_vm` (after the `#if/#else/#endif` mock-vs-net block, before `now_playing_update(&s_vm);`) add:

```c
#if CONFIG_YTM_USE_NET
    // mock build fills battery itself; live build reads the real AXP2101.
    {
        battery_status_t b;
        battery_get(&b);
        s_vm.battery_present = b.present;
        s_vm.battery_percent = b.percent;
        s_vm.charging        = b.charging;
    }
#endif
```

And after `now_playing_update(&s_vm);` add:

```c
    quick_panel_set_battery(s_vm.battery_percent, s_vm.charging, s_vm.battery_present);
```

- [ ] **Step 6: Commit**

```bash
git add firmware/main/main.c
git commit -m "feat(firmware): NVS-persisted runtime brightness + battery + panel wiring"
```

---

## Task 6: Docs + full verification

**Files:**
- Modify: `docs/CONFIGURATION.md`

- [ ] **Step 1: Update `docs/CONFIGURATION.md` brightness row**

Change the `YTM_DISPLAY_BRIGHTNESS` row's Notes cell to:

```
AMOLED brightness, 5–100%. Applied via panel command. This is now the **first-boot default only** — the swipe-down on-device panel adjusts brightness live and persists the choice to NVS, which wins on later boots. Lower it if the screen is harsh.
```

- [ ] **Step 2: Run the host test suite**

```bash
export PATH="/c/msys64/mingw64/bin:$PATH"
cmake -S tests -B tests/build -G Ninja -DCMAKE_C_COMPILER=gcc && cmake --build tests/build && (cd tests/build && ctest --output-on-failure)
```
Expected: all tests pass, including `battery`.

- [ ] **Step 3: Build + headless-run the sim**

```bash
export PATH="/c/msys64/mingw64/bin:$PATH"
cd sim && cmake --build build && SIM_MAX_FRAMES=300 SDL_VIDEODRIVER=dummy ./build/ytm_sim.exe && cd ..
```
Expected: clean build, exit 0.

- [ ] **Step 4: [hardware] Firmware build**

In an ESP-IDF-activated shell (see [docs/RUNNING.md](../../RUNNING.md)):

```
cd firmware
idf.py build
```
Expected: builds clean. If `bsp_i2c_get_handle()` is undefined, open the managed BSP header (`firmware/managed_components/.../include/bsp/`) and use the actual I²C-bus accessor it exports (see the NOTE in `battery.c` Step 1), then rebuild.

- [ ] **Step 5: [hardware] On-board verification**

Flash (`idf.py -p <port> flash monitor`) and confirm:
1. Battery box + `%` in the top-right matches a known charge level.
2. Plugging USB shows the green/charging color; unplugging returns to normal/amber.
3. Swipe down → panel opens; the slider changes screen brightness live.
4. Set a distinct brightness, wait ~1 s, reboot → the board comes up at that brightness (NVS persisted), not the Kconfig default.

- [ ] **Step 6: Commit**

```bash
git add docs/CONFIGURATION.md
git commit -m "docs: brightness is now first-boot default; NVS persists runtime changes"
```

---

## Self-Review notes (for the implementer)

- **Battery presence semantics** (`status1` bit 3) and **charge bits** (`status2[6:5]`) are the values pinned by the Task 1 host test. If on-hardware behavior disagrees (e.g. presence reads inverted), fix `axp2101_decode()` **and** update the test together — the test is the contract.
- **`COL_*` / `FONT_LABEL` token names** in `quick_panel.c` must match `ui/styles.h`; verify before building the sim (Task 4 Step 2 note).
- **`bsp_i2c_get_handle()`** is the one unverified external symbol — confirm against the managed BSP at firmware-build time (Task 6 Step 4).
- The brightness sink is the only path that writes the panel/NVS; the shared `ui/` code stays hardware-agnostic.
