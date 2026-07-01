# Auto screen-dim on idle — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Dim the AMOLED to a low brightness after the device is idle (no touch, no motion) while nothing is playing, and instantly restore the user's brightness on any touch or physical movement.

**Architecture:** A pure, host-testable decision module (`idle`) owns all the timing/state logic and calls back to set brightness. Touch idle comes free from LVGL (`lv_display_get_inactive_time`); physical-movement wake comes from a new QMI8658 IMU driver (`imu`) using the sensor's hardware wake-on-motion interrupt on GPIO17. Both are wired into the existing 30 fps `tick_cb` and gated by a Kconfig master switch.

**Tech Stack:** C11, ESP-IDF, LVGL v9, Waveshare `esp32_s3_touch_amoled_2_16` BSP, `esp_driver_i2c` new master API, QMI8658 IMU, CMake/ctest host tests.

**Spec:** [docs/superpowers/specs/2026-07-01-auto-screen-dim-on-idle-design.md](../specs/2026-07-01-auto-screen-dim-on-idle-design.md)

**Toolchain notes (Windows dev box):**
- Host unit tests build with MinGW gcc at `C:\msys64\mingw64\bin` (off PATH). In the Bash tool use `/c/msys64/mingw64/bin/gcc`.
- Firmware build: init IDF (`Initialize-Idf.ps1 -IdfId …`) then `idf.py build` — heavy, on-device steps only.
- Sim build: MinGW + SDL2 (see `sim/README.md`).

---

## File structure

- **Create** `firmware/main/idle.h` — interface for the pure idle-dim decision module.
- **Create** `firmware/main/idle.c` — the decision logic (no ESP/LVGL deps; host-testable).
- **Create** `tests/test_idle.c` — host unit tests for `idle`.
- **Create** `firmware/main/imu.h` — interface for the QMI8658 motion-wake driver.
- **Create** `firmware/main/imu.c` — QMI8658 wake-on-motion driver (firmware-only).
- **Modify** `firmware/main/Kconfig.projbuild` — add the three `YTM_IDLE_DIM_*` options.
- **Modify** `firmware/main/main.c` — track active brightness; wire `idle` + `imu` into `tick_cb`/`app_main`.
- **Modify** `firmware/main/CMakeLists.txt` — add `idle.c`, `imu.c`, `esp_driver_gpio`.
- **Modify** `tests/CMakeLists.txt` — add the `test_idle` executable + test.
- **Modify** `sim/main_sim.c` + `sim/CMakeLists.txt` — wire `idle` into the sim for touch-path verification.
- **Modify** `firmware/README.md` — document the feature + Kconfig knobs.

---

## Task 1: Kconfig options

**Files:**
- Modify: `firmware/main/Kconfig.projbuild` (after the `YTM_DISPLAY_BRIGHTNESS` block)

- [ ] **Step 1: Add the three config options**

Insert immediately after the `config YTM_DISPLAY_BRIGHTNESS` block:

```kconfig
config YTM_IDLE_DIM_ENABLE
    bool "Auto-dim the screen after idle"
    default y
    help
        When set, the screen dims to YTM_IDLE_DIM_PERCENT after
        YTM_IDLE_DIM_MS of no touch and no physical movement, but only while
        nothing is playing. Any touch or motion (QMI8658 IMU) restores the
        user's brightness. When unset, the screen never auto-dims and behaves
        exactly as before.

config YTM_IDLE_DIM_MS
    int "Idle time before dimming (ms)"
    depends on YTM_IDLE_DIM_ENABLE
    default 30000
    range 1000 600000
    help
        Milliseconds with no touch and no motion (and nothing playing) before
        the screen dims.

config YTM_IDLE_DIM_PERCENT
    int "Dimmed brightness (percent)"
    depends on YTM_IDLE_DIM_ENABLE
    default 10
    range 1 100
    help
        Panel brightness while dimmed. Restored to the user's level on any
        interaction.
```

- [ ] **Step 2: Commit**

```bash
git add firmware/main/Kconfig.projbuild
git commit -m "feat(config): add YTM_IDLE_DIM_* options for idle screen-dim"
```

---

## Task 2: `idle` decision module (pure C, TDD)

The brain of the feature. No ESP/LVGL headers → compiles and tests on the host, exactly like `axp2101_decode.c`.

**Files:**
- Create: `firmware/main/idle.h`
- Create: `firmware/main/idle.c`
- Test: `tests/test_idle.c`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the header**

Create `firmware/main/idle.h`:

```c
// idle.h — idle-dim decision logic (pure C; no ESP/LVGL, host-testable).
//
// Tracks time since the last user activity (touch + motion) and, when the
// device has been idle past a threshold AND nothing is playing, dims the
// screen. Any activity restores the user's brightness. See
// docs/superpowers/specs/2026-07-01-auto-screen-dim-on-idle-design.md
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t dim_after_ms;        // idle time with no activity before dimming
    int      dim_percent;         // panel brightness (%) while dimmed
    void   (*apply)(int percent); // set panel brightness now (must NOT persist)
    int    (*get_active)(void);   // user's current brightness (restore target)
} idle_cfg_t;

// Configure and reset the tracker. `now_ms` is a monotonic millisecond clock;
// the same clock source must be passed to idle_tick().
void idle_init(const idle_cfg_t *cfg, uint32_t now_ms);

// Register non-touch activity (e.g. IMU motion). Safe to call from an ISR or
// another task: it only sets a flag, consumed on the next idle_tick().
void idle_notify_activity(void);

// Run one decision step. `touch_inactive_ms` = ms since the last touch
// (LVGL's lv_display_get_inactive_time()); `now_ms` = the monotonic clock;
// `playing` = true while audio is playing (disables dimming; restores if dimmed).
void idle_tick(uint32_t touch_inactive_ms, uint32_t now_ms, bool playing);

// True when the screen is currently dimmed (inspection / tests).
bool idle_is_dimmed(void);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Write the failing test**

Create `tests/test_idle.c`:

```c
// test_idle.c — host unit test for the pure idle-dim logic (no ESP/LVGL).
//   cc -std=c11 -I../firmware/main test_idle.c ../firmware/main/idle.c -o test_idle
#include "idle.h"
#include <stdio.h>
#include <stdlib.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...)                                                \
    do {                                                               \
        if (cond) { g_pass++; }                                        \
        else { g_fail++; printf("  FAIL (%s:%d): ", __FILE__, __LINE__); \
               printf(__VA_ARGS__); printf("\n"); }                    \
    } while (0)

static int g_applied = -1;   // last value passed to apply()
static int g_active  = 60;   // user's current brightness

static void fake_apply(int p)     { g_applied = p; }
static int  fake_get_active(void) { return g_active; }

static void setup(uint32_t dim_after_ms, int dim_percent)
{
    idle_cfg_t cfg = { dim_after_ms, dim_percent, fake_apply, fake_get_active };
    g_applied = -1;
    g_active  = 60;
    idle_init(&cfg, 0);
}

int main(void)
{
    printf("# dims after threshold when idle and not playing\n");
    setup(30000, 10);
    idle_tick(0, 0, false);          CHECK(!idle_is_dimmed(), "t=0 not dimmed");
    idle_tick(29999, 29999, false);  CHECK(!idle_is_dimmed(), "just under threshold");
    idle_tick(30000, 30000, false);  CHECK(idle_is_dimmed(),  "at threshold dims");
    CHECK(g_applied == 10, "applies dim_percent (got %d)", g_applied);

    printf("# never dims while playing, even past threshold\n");
    setup(30000, 10);
    idle_tick(60000, 60000, true);   CHECK(!idle_is_dimmed(), "playing never dims");
    CHECK(g_applied == -1, "playing applies nothing (got %d)", g_applied);

    printf("# touch activity restores\n");
    setup(1000, 10);
    idle_tick(1000, 1000, false);    CHECK(idle_is_dimmed(), "dimmed");
    g_applied = -1;
    idle_tick(0, 1050, false);       CHECK(!idle_is_dimmed(), "touch wakes");
    CHECK(g_applied == 60, "restore applies active (got %d)", g_applied);

    printf("# motion activity restores (beats high touch-idle)\n");
    setup(1000, 10);
    idle_tick(1000, 1000, false);    CHECK(idle_is_dimmed(), "dimmed");
    idle_notify_activity();
    g_applied = -1;
    idle_tick(2000, 2000, false);    CHECK(!idle_is_dimmed(), "motion wakes");
    CHECK(g_applied == 60, "restore applies active (got %d)", g_applied);

    printf("# playback starting while dimmed restores and stays lit\n");
    setup(1000, 10);
    idle_tick(1000, 1000, false);    CHECK(idle_is_dimmed(), "dimmed");
    g_applied = -1;
    idle_tick(2000, 2000, true);     CHECK(!idle_is_dimmed(), "play wakes");
    CHECK(g_applied == 60, "restore to active on play (got %d)", g_applied);

    printf("# restore uses LIVE active brightness\n");
    setup(1000, 10);
    idle_tick(1000, 1000, false);    CHECK(idle_is_dimmed(), "dimmed");
    g_active = 25;                    // user changed brightness meanwhile
    idle_tick(0, 1050, false);       CHECK(g_applied == 25, "reads live active (got %d)", g_applied);

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? EXIT_FAILURE : EXIT_SUCCESS;
}
```

- [ ] **Step 3: Run the test to verify it fails**

Run (from repo root):
```bash
/c/msys64/mingw64/bin/gcc -std=c11 -Wall -Wextra -Ifirmware/main \
  tests/test_idle.c firmware/main/idle.c -o test_idle.exe && ./test_idle.exe
```
Expected: FAIL — link/compile error (`idle.c` does not exist yet / undefined references).

- [ ] **Step 4: Write the minimal implementation**

Create `firmware/main/idle.c`:

```c
// idle.c — pure idle-dim decision logic. No ESP/LVGL; host-testable.
#include "idle.h"

static idle_cfg_t    s_cfg;
static bool          s_dimmed;
static uint32_t      s_last_motion_ms;
static volatile bool s_motion_pending;

void idle_init(const idle_cfg_t *cfg, uint32_t now_ms)
{
    s_cfg            = *cfg;
    s_dimmed         = false;
    s_last_motion_ms = now_ms;
    s_motion_pending = false;
}

void idle_notify_activity(void)
{
    s_motion_pending = true;   // consumed on the next idle_tick()
}

bool idle_is_dimmed(void)
{
    return s_dimmed;
}

static void restore(void)
{
    if (s_dimmed) {
        s_cfg.apply(s_cfg.get_active());
        s_dimmed = false;
    }
}

void idle_tick(uint32_t touch_inactive_ms, uint32_t now_ms, bool playing)
{
    if (s_motion_pending) {
        s_motion_pending = false;
        s_last_motion_ms = now_ms;
    }

    if (playing) {          // never dim during playback; ensure lit
        restore();
        return;
    }

    uint32_t motion_idle = now_ms - s_last_motion_ms;      // ms since last motion
    uint32_t idle_ms     = touch_inactive_ms < motion_idle // whichever is more recent
                         ? touch_inactive_ms : motion_idle;

    if (s_dimmed) {
        if (idle_ms < s_cfg.dim_after_ms) restore();       // activity -> wake
    } else if (idle_ms >= s_cfg.dim_after_ms) {
        s_cfg.apply(s_cfg.dim_percent);
        s_dimmed = true;
    }
}
```

- [ ] **Step 5: Run the test to verify it passes**

Run:
```bash
/c/msys64/mingw64/bin/gcc -std=c11 -Wall -Wextra -Ifirmware/main \
  tests/test_idle.c firmware/main/idle.c -o test_idle.exe && ./test_idle.exe
```
Expected: PASS — final line `17 passed, 0 failed`.

- [ ] **Step 6: Register the test in CMake (so CI runs it)**

In `tests/CMakeLists.txt`, after the `test_battery` block, add:

```cmake
# Idle-dim decision logic: pure C (no LVGL/ESP), compiled standalone.
add_executable(test_idle
    test_idle.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/main/idle.c
)
target_include_directories(test_idle PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/main)

if(NOT MSVC)
    target_compile_options(test_idle PRIVATE -Wall -Wextra)
endif()

add_test(NAME idle COMMAND test_idle)
```

- [ ] **Step 7: Verify via ctest (the CI path)**

Run:
```bash
cmake -B build tests -G "MinGW Makefiles" \
  -DCMAKE_C_COMPILER=/c/msys64/mingw64/bin/gcc.exe && \
  cmake --build build && ctest --test-dir build --output-on-failure
```
Expected: all tests pass, including `idle`.

- [ ] **Step 8: Commit**

```bash
git add firmware/main/idle.h firmware/main/idle.c tests/test_idle.c tests/CMakeLists.txt
git commit -m "feat(firmware): pure idle-dim decision module + host tests"
```

---

## Task 3: Wire `idle` into the sim (touch-path verification)

The sim runs the same `ui/` code with no hardware. Wiring `idle` here proves the touch path end-to-end before we build firmware.

**Files:**
- Modify: `sim/main_sim.c`
- Modify: `sim/CMakeLists.txt`

- [ ] **Step 1: Add `idle.c` to the sim build**

In `sim/CMakeLists.txt`, inside the `add_executable(ytm_sim ...)` source list, add:
```cmake
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/main/idle.c
```
Then add its include dir after the executable (near the other `target_include_directories`, or add one):
```cmake
target_include_directories(ytm_sim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/main)
```

- [ ] **Step 2: Wire the idle module into `main_sim.c`**

Add the include near the others:
```c
#include "idle.h"
```

Replace the existing `sim_brightness` function and add helpers + an active-brightness tracker:
```c
static int s_active = 40;   // user's current brightness (restore target)

// User path (quick-panel slider): remember + report.
static void sim_brightness(int percent)
{
    s_active = percent;
    printf("brightness: %d%%\n", percent);
    fflush(stdout);
}

// Idle path: apply transiently (no persistence in the sim, just report).
static void sim_apply(int percent)
{
    printf("brightness: %d%%\n", percent);
    fflush(stdout);
}

static int sim_get_active(void) { return s_active; }
```

In `tick_cb`, after `now_playing_update(&s_vm);`, add:
```c
    idle_tick(lv_display_get_inactive_time(NULL), millis(),
              s_vm.playback == PB_PLAYING);
```

In `main()`, after `quick_panel_init(lv_screen_active(), sim_brightness, 40);` add:
```c
    s_active = 40;
    // Short idle window so dimming is observable in a short sim run.
    idle_cfg_t icfg = { 1500, 10, sim_apply, sim_get_active };
    idle_init(&icfg, millis());
```

- [ ] **Step 3: Build the sim**

Run (MinGW + SDL2 toolchain, see `sim/README.md`):
```bash
cmake -B sim/build sim -G "MinGW Makefiles" \
  -DCMAKE_C_COMPILER=/c/msys64/mingw64/bin/gcc.exe && cmake --build sim/build
```
Expected: builds cleanly, no warnings about `idle_*`.

- [ ] **Step 4: Verify dim + restore-on-play headlessly**

Run:
```bash
SIM_MAX_FRAMES=400 SDL_VIDEODRIVER=dummy ./sim/build/ytm_sim.exe | grep "brightness"
```
Expected: with no input, the mock cycles through playback states; during a non-playing window > 1.5 s the log shows `brightness: 10%` (dimmed), and when the mock returns to PLAYING it shows `brightness: 40%` (restored). Seeing both a `10%` line and a later `40%` line confirms the dim + restore path.

- [ ] **Step 5: Commit**

```bash
git add sim/main_sim.c sim/CMakeLists.txt
git commit -m "feat(sim): wire idle-dim so the touch path runs in the simulator"
```

---

## Task 4: Wire `idle` into firmware `main.c`

**Files:**
- Modify: `firmware/main/main.c`
- Modify: `firmware/main/CMakeLists.txt`

- [ ] **Step 1: Add `idle.c` to the firmware component**

In `firmware/main/CMakeLists.txt`, add `"idle.c"` to the `SRCS` list (after `"battery.c"`).

- [ ] **Step 2: Include the header and track active brightness**

In `main.c`, add after `#include "quick_panel.h"`:
```c
#include "idle.h"
```

Add a module-level active-brightness tracker near `static now_playing_vm_t s_vm;`:
```c
static int s_active_bright;   // user-chosen brightness; the idle-dim restore target
```

- [ ] **Step 3: Make the user brightness path update the restore target**

In `fw_brightness()`, record the user's level so restore always targets it. Change:
```c
static void fw_brightness(int percent)
{
    bsp_display_brightness_set(percent);   // apply immediately
    s_pending_bright = percent;
    esp_timer_stop(s_bright_timer);        // debounce flash writes
    esp_timer_start_once(s_bright_timer, 500 * 1000);
}
```
to:
```c
static void fw_brightness(int percent)
{
    s_active_bright = percent;              // remember as the restore target
    bsp_display_brightness_set(percent);    // apply immediately
    s_pending_bright = percent;
    esp_timer_stop(s_bright_timer);         // debounce flash writes
    esp_timer_start_once(s_bright_timer, 500 * 1000);
}
```

- [ ] **Step 4: Add the idle apply/get callbacks (transient — no NVS)**

Add just above `app_main`:
```c
// Idle-dim brightness callbacks. apply() must NOT persist — dimming is
// transient, so it goes straight to the panel and never touches NVS. Restore
// reads the user's current level so a slider change mid-idle is respected.
static void idle_apply(int percent) { bsp_display_brightness_set(percent); }
static int  idle_get_active(void)   { return s_active_bright; }
```

- [ ] **Step 5: Initialize active brightness + the idle tracker in `app_main`**

After `int initial_bright = brightness_load();` add:
```c
    s_active_bright = initial_bright;
```

After `bsp_display_unlock();` (the timer is already running), add:
```c
#if CONFIG_YTM_IDLE_DIM_ENABLE
    idle_cfg_t icfg = {
        .dim_after_ms = CONFIG_YTM_IDLE_DIM_MS,
        .dim_percent  = CONFIG_YTM_IDLE_DIM_PERCENT,
        .apply        = idle_apply,
        .get_active   = idle_get_active,
    };
    idle_init(&icfg, (uint32_t)(esp_timer_get_time() / 1000));
#endif
```

- [ ] **Step 6: Run the idle decision each tick**

In `tick_cb`, after `now_playing_update(&s_vm);`, add:
```c
#if CONFIG_YTM_IDLE_DIM_ENABLE
    idle_tick(lv_display_get_inactive_time(NULL),
              (uint32_t)(esp_timer_get_time() / 1000),
              s_vm.playback == PB_PLAYING);
#endif
```

- [ ] **Step 7: Build the firmware**

Init IDF, then:
```bash
idf.py build
```
Expected: builds cleanly. (No host unit test — logic is covered by Task 2; this step confirms it compiles and links into the firmware.)

- [ ] **Step 8: Commit**

```bash
git add firmware/main/main.c firmware/main/CMakeLists.txt
git commit -m "feat(firmware): dim screen on idle (touch path) via idle module"
```

---

## Task 5: QMI8658 motion-wake driver (`imu`)

Firmware-only. Brings up the QMI8658 over the shared BSP I²C bus and configures its hardware wake-on-motion (WoM) interrupt on GPIO17. On motion → `idle_notify_activity()`. This is hardware bring-up: verified on-device incrementally, not by host tests. If the IMU is absent/unresponsive it logs a warning and returns, degrading to touch-only wake.

> **Register values below are the best-known QMI8658 WoM recipe.** Confirm the WoM threshold/INT-config bytes and the accel-config value against the QMI8658 datasheet during Step 5 bring-up and tune the threshold there. The WHO_AM_I probe in Step 3 de-risks the I²C address before any configuration.

**Files:**
- Create: `firmware/main/imu.h`
- Create: `firmware/main/imu.c`
- Modify: `firmware/main/CMakeLists.txt`
- Modify: `firmware/main/main.c`

- [ ] **Step 1: Write the header**

Create `firmware/main/imu.h`:
```c
// imu.h — QMI8658 wake-on-motion driver. Configures the IMU's hardware motion
// interrupt (GPIO17) to signal user activity for the idle-dim feature.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Probe + configure the QMI8658 for wake-on-motion and install the GPIO17 ISR.
// On motion it calls idle_notify_activity() (from a task, not the ISR).
// Safe to call once at startup; logs a warning and returns if the IMU is
// absent/unresponsive (feature degrades to touch-only wake).
void imu_start(void);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Write the driver skeleton (I²C init + WHO_AM_I probe)**

Create `firmware/main/imu.c`:
```c
// imu.c — QMI8658 wake-on-motion driver. Reuses the BSP I2C bus (shared with
// the touch controller / AXP2101). See the design spec for the wake model.
#include "imu.h"
#include "idle.h"

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "imu";

// --- QMI8658 I2C + registers -------------------------------------------------
#define QMI8658_I2C_ADDR   0x6B   // SA0=1 on this board; try 0x6A if WHO_AM_I fails
#define QMI8658_WHO_AM_I   0x00   // expect 0x05
#define QMI8658_WHOAMI_VAL 0x05
#define QMI8658_CTRL1      0x02   // serial IF / addr auto-increment
#define QMI8658_CTRL2      0x03   // accel: ODR + full-scale
#define QMI8658_CTRL7      0x08   // sensor enable (aEN = bit0)
#define QMI8658_CTRL8      0x09   // motion engine / INT selection
#define QMI8658_CTRL9      0x0A   // host command register
#define QMI8658_CAL1_L     0x0B   // command arg: WoM threshold (mg)
#define QMI8658_CAL1_H     0x0C   // command arg: INT map / blanking
#define QMI8658_STATUSINT  0x2D   // bit0 = ctrl9 command done

// CTRL9 host commands
#define QMI8658_CMD_WOM    0x08   // CTRL_CMD_WRITE_WOM_SETTING
#define QMI8658_CMD_ACK    0x00   // CTRL_CMD_ACK

// Config values (confirm/tune against datasheet at bring-up)
#define QMI8658_CTRL2_ACC  0x03   // low ODR, moderate full-scale for WoM
#define QMI8658_WOM_THRESH 0x20   // motion threshold in mg (~32 mg); raise to reduce sensitivity
#define QMI8658_WOM_INTCFG 0x00   // INT map / blanking (INT2 default)

#define IMU_INT_GPIO       GPIO_NUM_17

static i2c_master_dev_handle_t s_dev;
static SemaphoreHandle_t       s_motion_sem;

static esp_err_t rd(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, 100);
}

static esp_err_t wr(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, 2, 100);
}

// ISR: just release the semaphore; all I2C / logic runs in the task.
static void IRAM_ATTR imu_isr(void *arg)
{
    (void)arg;
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_motion_sem, &hp);
    if (hp) portYIELD_FROM_ISR();
}

static void motion_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (xSemaphoreTake(s_motion_sem, portMAX_DELAY) == pdTRUE) {
            idle_notify_activity();
        }
    }
}

void imu_start(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = QMI8658_I2C_ADDR,
        .scl_speed_hz    = 400000,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) {
        ESP_LOGW(TAG, "add device failed; motion wake disabled");
        return;
    }

    uint8_t who = 0;
    if (rd(QMI8658_WHO_AM_I, &who) != ESP_OK || who != QMI8658_WHOAMI_VAL) {
        ESP_LOGW(TAG, "QMI8658 not found (WHO_AM_I=0x%02x); motion wake disabled", who);
        return;
    }
    ESP_LOGI(TAG, "QMI8658 detected (WHO_AM_I=0x%02x)", who);
    // WoM configuration + ISR wiring added in Step 4.
}
```

- [ ] **Step 3: Compile + on-device WHO_AM_I check**

Add `"imu.c"` to `firmware/main/CMakeLists.txt` `SRCS`, and add `esp_driver_gpio` to `REQUIRES`. Temporarily call `imu_start();` in `app_main` (after `battery_start();`), then:
```bash
idf.py build flash monitor
```
Expected log: `QMI8658 detected (WHO_AM_I=0x05)`. If it logs "not found", change `QMI8658_I2C_ADDR` to `0x6A` and retry. Do not proceed until WHO_AM_I is confirmed.

- [ ] **Step 4: Add WoM config + INT/ISR wiring**

Replace the comment `// WoM configuration + ISR wiring added in Step 4.` at the end of `imu_start()` with:
```c
    // Configure accel + wake-on-motion via the CTRL9 command interface.
    wr(QMI8658_CTRL1, 0x60);              // serial IF: addr auto-increment, little-endian
    wr(QMI8658_CTRL7, 0x00);              // disable sensors while configuring
    wr(QMI8658_CTRL2, QMI8658_CTRL2_ACC);// accel ODR + full-scale
    wr(QMI8658_CAL1_L, QMI8658_WOM_THRESH);
    wr(QMI8658_CAL1_H, QMI8658_WOM_INTCFG);
    wr(QMI8658_CTRL9, QMI8658_CMD_WOM);  // apply WoM setting

    // Wait for the command-done handshake, then ack.
    for (int i = 0; i < 10; i++) {
        uint8_t st = 0;
        if (rd(QMI8658_STATUSINT, &st) == ESP_OK && (st & 0x01)) break;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    wr(QMI8658_CTRL9, QMI8658_CMD_ACK);
    wr(QMI8658_CTRL7, 0x01);              // enable accel (low-power WoM mode)

    // Motion signal path: ISR -> semaphore -> task -> idle_notify_activity().
    s_motion_sem = xSemaphoreCreateBinary();
    if (!s_motion_sem) { ESP_LOGE(TAG, "sem alloc failed"); return; }

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << IMU_INT_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&io);
    gpio_install_isr_service(0);          // harmless if already installed
    gpio_isr_handler_add(IMU_INT_GPIO, imu_isr, NULL);

    if (xTaskCreate(motion_task, "imu", 2560, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create motion task");
        return;
    }
    ESP_LOGI(TAG, "QMI8658 wake-on-motion armed (thr=0x%02x)", QMI8658_WOM_THRESH);
```

> If `gpio_install_isr_service` returns `ESP_ERR_INVALID_STATE` because the BSP already installed it, that is fine — ignore the return. Keep the call for the standalone case.

- [ ] **Step 5: On-device motion verification + threshold tuning**

Temporarily add `ESP_LOGI(TAG, "motion");` inside `motion_task` before `idle_notify_activity()`. Then:
```bash
idf.py build flash monitor
```
Verify:
- Resting still on the desk: **no** `motion` logs (if it chatters, raise `QMI8658_WOM_THRESH`, e.g. `0x40`).
- Picking the device up / nudging it: `motion` logs appear.
- If `NEGEDGE` never fires, the WoM INT is active-high on this wiring — change `.intr_type` to `GPIO_INTR_POSEDGE`.

Once tuned, remove the temporary `motion` log.

- [ ] **Step 6: Wire `imu_start()` into the idle block (remove the temporary call)**

Remove the temporary `imu_start();` from Step 3. In `main.c` add near the other includes:
```c
#if CONFIG_YTM_IDLE_DIM_ENABLE
#include "imu.h"
#endif
```
Inside the `#if CONFIG_YTM_IDLE_DIM_ENABLE` block in `app_main` (added in Task 4), after `idle_init(...);` add:
```c
    imu_start();   // motion wake; no-op-with-warning if the IMU is absent
```

- [ ] **Step 7: Full on-device verification**

```bash
idf.py build flash monitor
```
Verify end-to-end:
- Stop playback, leave the device untouched → after `YTM_IDLE_DIM_MS` the screen dims to `YTM_IDLE_DIM_PERCENT`.
- Tap the screen → restores to the user's brightness.
- Dim again, then physically move the device → restores.
- Start playback while dimmed → restores and stays lit; while playing, it never dims.

- [ ] **Step 8: Commit**

```bash
git add firmware/main/imu.h firmware/main/imu.c firmware/main/CMakeLists.txt firmware/main/main.c
git commit -m "feat(firmware): QMI8658 wake-on-motion for idle-dim"
```

---

## Task 6: Documentation

**Files:**
- Modify: `firmware/README.md`

- [ ] **Step 1: Document the feature**

Add a short section to `firmware/README.md` describing auto screen-dim: dims to `YTM_IDLE_DIM_PERCENT` after `YTM_IDLE_DIM_MS` of no touch/motion while nothing plays; wakes on touch or movement (QMI8658 IMU, GPIO17); disable with `YTM_IDLE_DIM_ENABLE=n`. Note the sim exercises the touch path only.

- [ ] **Step 2: Commit**

```bash
git add firmware/README.md
git commit -m "docs(firmware): document auto screen-dim on idle"
```

---

## Notes for the implementer

- **Do not route idle-dim through `fw_brightness()`** — that debounces a write to NVS. Dimming uses `idle_apply` (bare `bsp_display_brightness_set`) so a dim never persists and can't become the boot brightness.
- **`uint32_t` millis wraps** after ~49 days; the `now - last_motion` math tolerates a single wrap. Acceptable for this device.
- **The `idle` module holds static state** (one instance) — matches the single-screen firmware; the tests reset it via `idle_init` each case.
- **Deviation from the spec interface:** the spec sketched a separate `idle_set_playing(bool)`; this plan folds `playing` into `idle_tick()` to keep one entry point and less state. Same behavior.
