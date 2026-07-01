# Auto power-off on idle — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** After a long idle (no touch, no motion, nothing playing) **while on battery**, hard power off the board via the AXP2101; only a PWRON key press boots it back.

**Architecture:** Layer a second, longer threshold onto the existing pure `idle` module (which already dims after `YTM_IDLE_DIM_MS`). The module gains a `power_off_after_ms` threshold, a `power_off()` callback, and a runtime `power_off_allowed` gate (true only on battery). `battery.c` (owner of the AXP2101 I²C handle) gains `battery_power_off()` (soft power-off via reg `0x10` bit 0). `axp2101_decode` learns to report external-power-present (VBUS good, `STATUS1` bit 5) so "on battery" is detected correctly. `main.c` wires the callback and passes `!external_power` as the gate.

**Tech Stack:** ESP-IDF (C), LVGL v9, AXP2101 PMIC over I²C, QMI8658 IMU. Pure decision logic is host-tested with MinGW gcc; hardware writes are verified on-device.

**Spec:** [docs/superpowers/specs/2026-07-01-auto-power-off-on-idle-design.md](../specs/2026-07-01-auto-power-off-on-idle-design.md)

---

## File Structure

| File | Change | Responsibility |
| --- | --- | --- |
| `firmware/main/battery.h` | Modify | Add `external_power` to `battery_status_t`; declare `battery_power_off()`. |
| `firmware/main/axp2101_decode.h` | Modify | Add AXP2101 register defines (`0x10` power-off, `STATUS1` VBUS bit). |
| `firmware/main/axp2101_decode.c` | Modify | Decode `external_power` from `STATUS1` bit 5. |
| `firmware/main/battery.c` | Modify | Implement `battery_power_off()` (RMW reg `0x10` bit 0). |
| `firmware/main/idle.h` | Modify | Extend `idle_cfg_t`; add `power_off_allowed` arg to `idle_tick()`. |
| `firmware/main/idle.c` | Modify | Power-off stage: fire-once, retry-on-failure, battery-gated. |
| `firmware/main/Kconfig.projbuild` | Modify | `YTM_IDLE_POWEROFF_ENABLE` + `YTM_IDLE_POWEROFF_MS`. |
| `firmware/main/main.c` | Modify | `idle_power_off()` callback; populate config; pass the gate. |
| `sim/main_sim.c` | Modify | Update `idle_cfg_t` init + `idle_tick()` call; logging power-off stub. |
| `tests/test_battery.c` | Modify | Assert `external_power` decode. |
| `tests/test_idle.c` | Modify | Update harness for new field/arg; assert power-off behavior. |
| `docs/CONFIGURATION.md` | Modify | Document the idle-dim + power-off Kconfig options. |

## Conventions used by every host-test step

MinGW gcc is installed but **not on PATH**, and it needs its own DLLs from that directory, so every host-test command must prepend it. Run from the worktree root:

```bash
export PATH="/c/msys64/mingw64/bin:$PATH"
cd "C:/Users/Etai/Projects/yt-music-companion/.claude/worktrees/elegant-moser-1c4465"
```

Compiled test binaries (`test_battery.exe`, `test_idle.exe`) are throwaways built in the worktree root — do **not** `git add` them (commits below stage only source files).

---

## Task 1: `axp2101_decode` reports external power (VBUS present)

**Files:**
- Modify: `firmware/main/battery.h` (struct field)
- Modify: `firmware/main/axp2101_decode.h` (register doc + define)
- Modify: `firmware/main/axp2101_decode.c` (decode)
- Test: `tests/test_battery.c`

- [ ] **Step 1: Add the `external_power` field to `battery_status_t`**

In `firmware/main/battery.h`, extend the struct:

```c
typedef struct {
    bool present;         // battery detected on the MX1.25 connector
    int  percent;         // 0..100 state of charge (valid when present)
    bool charging;        // USB present and actively charging
    bool external_power;  // external/USB power present (VBUS good) — false = on battery
} battery_status_t;
```

- [ ] **Step 2: Add the VBUS register bit to the decode header**

In `firmware/main/axp2101_decode.h`, update the register doc comment and add a define. Change the header comment block's register list to include the VBUS bit and add the define near `AXP2101_REG_STATUS1`:

```c
//   0x00 PMU status 1  — bit 5 = VBUS (external power) good; bit 3 = battery present/online
```

```c
#define AXP2101_STATUS1_VBUS_GOOD (1u << 5)  // external/USB power present
```

- [ ] **Step 3: Write the failing test**

In `tests/test_battery.c`, add this block just before the final `printf("\n%d passed...` line:

```c
    printf("# external_power = status1 bit5 (VBUS good)\n");
    CHECK(dec(0x20, 0, 50).external_power == true,  "bit5 set -> external power");
    CHECK(dec(0x00, 0, 50).external_power == false, "bit5 clear -> on battery");
    CHECK(dec(0x28, 0, 50).external_power == true,  "bit5+bit3 -> external + present");
```

- [ ] **Step 4: Run the test to verify it fails**

```bash
export PATH="/c/msys64/mingw64/bin:$PATH"
gcc -std=c11 -Wall -Wextra -Ifirmware/main tests/test_battery.c firmware/main/axp2101_decode.c -o test_battery.exe && ./test_battery.exe
```
Expected: FAIL — either a compile error (`external_power` unknown) if Step 1 was skipped, or the three new checks fail because `axp2101_decode` doesn't set the field yet.

- [ ] **Step 5: Implement the decode**

In `firmware/main/axp2101_decode.c`, add one line to `axp2101_decode()`:

```c
void axp2101_decode(uint8_t status1, uint8_t status2, uint8_t soc_pct,
                    battery_status_t *out)
{
    out->present        = (status1 & (1u << 3)) != 0u;
    out->external_power = (status1 & AXP2101_STATUS1_VBUS_GOOD) != 0u;
    out->charging       = out->present && (((status2 >> 5) & 0x3u) == 0x1u);
    out->percent        = clampi((int)soc_pct, 0, 100);
}
```

- [ ] **Step 6: Run the test to verify it passes**

```bash
export PATH="/c/msys64/mingw64/bin:$PATH"
gcc -std=c11 -Wall -Wextra -Ifirmware/main tests/test_battery.c firmware/main/axp2101_decode.c -o test_battery.exe && ./test_battery.exe
```
Expected: PASS — ends with `N passed, 0 failed`.

- [ ] **Step 7: Commit**

```bash
git add firmware/main/battery.h firmware/main/axp2101_decode.h firmware/main/axp2101_decode.c tests/test_battery.c
git commit -m "feat(battery): decode external-power-present from AXP2101 STATUS1"
```

---

## Task 2: `battery_power_off()` — AXP2101 soft power-off

**Files:**
- Modify: `firmware/main/axp2101_decode.h` (register + bit defines)
- Modify: `firmware/main/battery.h` (declaration)
- Modify: `firmware/main/battery.c` (implementation)

No host test — this is a thin hardware I²C write, verified on-device in Task 5. It leaves the build green because the new function is not yet called.

- [ ] **Step 1: Add the power-off register defines**

In `firmware/main/axp2101_decode.h`, add near the other register defines:

```c
#define AXP2101_REG_COMMON_CFG  0x10        // PMU common config
#define AXP2101_SOFT_PWROFF_BIT (1u << 0)   // reg 0x10 bit0: software power-off
```

- [ ] **Step 2: Declare `battery_power_off()`**

In `firmware/main/battery.h`, add after `battery_get()`:

```c
// Hard power off the board via the AXP2101 (soft power-off, reg 0x10 bit0).
// Returns false if the I2C write fails (caller may retry); on success the power
// rails drop within milliseconds, so execution usually ends inside this call.
bool battery_power_off(void);
```

- [ ] **Step 3: Implement `battery_power_off()`**

In `firmware/main/battery.c`, add this function after `battery_get()`. It reuses the module-static `s_dev` handle and the existing `rd()` helper; read-modify-write preserves the other config bits:

```c
bool battery_power_off(void)
{
    uint8_t cfg = 0;
    if (rd(AXP2101_REG_COMMON_CFG, &cfg) != ESP_OK) {
        ESP_LOGW(TAG, "power-off: read reg 0x%02x failed", AXP2101_REG_COMMON_CFG);
        return false;
    }
    uint8_t buf[2] = { AXP2101_REG_COMMON_CFG, (uint8_t)(cfg | AXP2101_SOFT_PWROFF_BIT) };
    if (i2c_master_transmit(s_dev, buf, 2, 100) != ESP_OK) {
        ESP_LOGW(TAG, "power-off: write reg 0x%02x failed", AXP2101_REG_COMMON_CFG);
        return false;
    }
    ESP_LOGW(TAG, "AXP2101 soft power-off issued");
    return true;
}
```

- [ ] **Step 4: Commit**

```bash
git add firmware/main/axp2101_decode.h firmware/main/battery.h firmware/main/battery.c
git commit -m "feat(battery): add battery_power_off() (AXP2101 soft power-off)"
```

---

## Task 3: Kconfig options

**Files:**
- Modify: `firmware/main/Kconfig.projbuild`

- [ ] **Step 1: Add the two options**

In `firmware/main/Kconfig.projbuild`, insert immediately after the `YTM_IDLE_DIM_PERCENT` block (before `config YTM_WIFI_SSID`):

```
config YTM_IDLE_POWEROFF_ENABLE
    bool "Auto power-off after long idle (battery only)"
    depends on YTM_IDLE_DIM_ENABLE
    default y
    help
        When set, the board hard-powers-off via the AXP2101 after
        YTM_IDLE_POWEROFF_MS of no touch, no motion, and nothing playing — but
        only while running on battery (not while charging / on external power).
        Once off, only a PWRON key press turns it back on (a cold boot). Reuses
        the idle tracking that YTM_IDLE_DIM_ENABLE sets up.

config YTM_IDLE_POWEROFF_MS
    int "Idle time before power-off (ms)"
    depends on YTM_IDLE_POWEROFF_ENABLE
    default 600000
    range 60000 3600000
    help
        Milliseconds with no touch, no motion, nothing playing, and on battery
        before the board powers itself off. Should be larger than YTM_IDLE_DIM_MS
        (the screen dims first, then powers off). Default 600000 = 10 minutes.
```

- [ ] **Step 2: Commit**

```bash
git add firmware/main/Kconfig.projbuild
git commit -m "feat(config): add YTM_IDLE_POWEROFF_ENABLE / _MS Kconfig options"
```

---

## Task 4: `idle` power-off stage + wire into main.c and the sim

This is one coherent commit: the `idle_tick()` signature change and `idle_cfg_t` growth touch every caller (tests, sim, firmware), so they are updated together to keep every target buildable.

**Files:**
- Modify: `firmware/main/idle.h`
- Modify: `firmware/main/idle.c`
- Test: `tests/test_idle.c`
- Modify: `sim/main_sim.c`
- Modify: `firmware/main/main.c`

- [ ] **Step 1: Extend `idle.h` (struct + signature)**

Replace the `idle_cfg_t` struct and the `idle_tick` declaration in `firmware/main/idle.h` with:

```c
typedef struct {
    uint32_t dim_after_ms;         // idle time with no activity before dimming
    int      dim_percent;          // panel brightness (%) while dimmed
    uint32_t power_off_after_ms;   // idle time before power-off; 0 = disabled
    void   (*apply)(int percent);  // set panel brightness now (must NOT persist)
    int    (*get_active)(void);    // user's current brightness (restore target)
    bool   (*power_off)(void);     // issue power-off; return true if issued (no retry)
} idle_cfg_t;
```

Update the `idle_tick` prototype and its doc comment:

```c
// Run one decision step. `touch_inactive_ms` = ms since the last touch
// (lv_display_get_inactive_time()); `now_ms` = the monotonic clock;
// `playing` = true while audio is playing (disables dimming/power-off; restores
// if dimmed); `power_off_allowed` = true only while on battery (gates power-off).
void idle_tick(uint32_t touch_inactive_ms, uint32_t now_ms,
               bool playing, bool power_off_allowed);
```

- [ ] **Step 2: Update the `idle.c` state + logic**

In `firmware/main/idle.c`: add `#include <stddef.h>` under `#include "idle.h"` (for `NULL`); add a `s_power_off_done` flag; reset it in `idle_init`; and add the power-off stage to `idle_tick`.

Add the flag alongside the other statics:

```c
static bool          s_power_off_done;   // latched once power_off() succeeds
```

In `idle_init`, reset it (add after `s_motion_pending = false;`):

```c
    s_power_off_done = false;
```

Replace `idle_tick` with:

```c
void idle_tick(uint32_t touch_inactive_ms, uint32_t now_ms,
               bool playing, bool power_off_allowed)
{
    if (s_motion_pending) {
        s_motion_pending = false;
        s_last_motion_ms = now_ms;
    }

    if (playing) {                 // playback counts as activity: stay lit now,
        s_last_motion_ms = now_ms; // keep the idle clock fresh, and never power
        restore();                 // off mid-song.
        return;
    }

    // Unsigned subtraction wraps safely on uint32 overflow (~49 days); ticks
    // arrive every ~33ms so true elapsed time never approaches that range.
    uint32_t motion_idle = now_ms - s_last_motion_ms;      // ms since last motion
    uint32_t idle_ms     = touch_inactive_ms < motion_idle // smaller idle value = more recent activity
                         ? touch_inactive_ms : motion_idle;

    if (s_dimmed) {
        if (idle_ms < s_cfg.dim_after_ms) restore();       // activity -> wake
    } else if (idle_ms >= s_cfg.dim_after_ms) {
        s_cfg.apply(s_cfg.dim_percent);
        s_dimmed = true;
    }

    // Power-off stage: only on battery, only once. If the callback fails (I2C
    // error) the flag stays clear so the next tick retries — never left half-off.
    if (!s_power_off_done && power_off_allowed &&
        s_cfg.power_off_after_ms != 0u && s_cfg.power_off != NULL &&
        idle_ms >= s_cfg.power_off_after_ms) {
        if (s_cfg.power_off()) s_power_off_done = true;
    }
}
```

- [ ] **Step 3: Update the test harness and add failing power-off tests**

Rewrite `tests/test_idle.c` in full:

```c
// test_idle.c — host unit test for the pure idle-dim + power-off logic (no ESP/LVGL).
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

static int  g_applied = -1;   // last value passed to apply()
static int  g_active  = 60;   // user's current brightness
static int  g_poweroff_calls = 0;
static bool g_poweroff_ret   = true;   // what fake_power_off() returns

static void fake_apply(int p)     { g_applied = p; }
static int  fake_get_active(void) { return g_active; }
static bool fake_power_off(void)  { g_poweroff_calls++; return g_poweroff_ret; }

static void setup(uint32_t dim_after_ms, int dim_percent, uint32_t power_off_after_ms)
{
    idle_cfg_t cfg = {
        .dim_after_ms       = dim_after_ms,
        .dim_percent        = dim_percent,
        .power_off_after_ms = power_off_after_ms,
        .apply              = fake_apply,
        .get_active         = fake_get_active,
        .power_off          = fake_power_off,
    };
    g_applied = -1;
    g_active  = 60;
    g_poweroff_calls = 0;
    g_poweroff_ret   = true;
    idle_init(&cfg, 0);
}

int main(void)
{
    printf("# dims after threshold when idle and not playing\n");
    setup(30000, 10, 0);
    idle_tick(0, 0, false, true);          CHECK(!idle_is_dimmed(), "t=0 not dimmed");
    idle_tick(29999, 29999, false, true);  CHECK(!idle_is_dimmed(), "just under threshold");
    idle_tick(30000, 30000, false, true);  CHECK(idle_is_dimmed(),  "at threshold dims");
    CHECK(g_applied == 10, "applies dim_percent (got %d)", g_applied);

    printf("# never dims while playing, even past threshold\n");
    setup(30000, 10, 0);
    idle_tick(60000, 60000, true, true);   CHECK(!idle_is_dimmed(), "playing never dims");
    CHECK(g_applied == -1, "playing applies nothing (got %d)", g_applied);

    printf("# touch activity restores\n");
    setup(1000, 10, 0);
    idle_tick(1000, 1000, false, true);    CHECK(idle_is_dimmed(), "dimmed");
    g_applied = -1;
    idle_tick(0, 1050, false, true);       CHECK(!idle_is_dimmed(), "touch wakes");
    CHECK(g_applied == 60, "restore applies active (got %d)", g_applied);

    printf("# motion activity restores (beats high touch-idle)\n");
    setup(1000, 10, 0);
    idle_tick(1000, 1000, false, true);    CHECK(idle_is_dimmed(), "dimmed");
    idle_notify_activity();
    g_applied = -1;
    idle_tick(2000, 2000, false, true);    CHECK(!idle_is_dimmed(), "motion wakes");
    CHECK(g_applied == 60, "restore applies active (got %d)", g_applied);

    printf("# playback starting while dimmed restores and stays lit\n");
    setup(1000, 10, 0);
    idle_tick(1000, 1000, false, true);    CHECK(idle_is_dimmed(), "dimmed");
    g_applied = -1;
    idle_tick(2000, 2000, true, true);     CHECK(!idle_is_dimmed(), "play wakes");
    CHECK(g_applied == 60, "restore to active on play (got %d)", g_applied);

    printf("# restore uses LIVE active brightness\n");
    setup(1000, 10, 0);
    idle_tick(1000, 1000, false, true);    CHECK(idle_is_dimmed(), "dimmed");
    g_active = 25;                          // user changed brightness meanwhile
    idle_tick(0, 1050, false, true);       CHECK(g_applied == 25, "reads live active (got %d)", g_applied);

    printf("# motion path: idle alone crosses threshold and dims\n");
    setup(1000, 10, 0);
    idle_notify_activity();                 // motion at t=0 seeds last_motion
    idle_tick(0, 0, false, true);           CHECK(!idle_is_dimmed(), "fresh, not dimmed");
    idle_tick(1000, 1000, false, true);     CHECK(idle_is_dimmed(),  "idle past threshold dims");
    CHECK(g_applied == 10, "dims to dim_percent via idle (got %d)", g_applied);

    printf("# playback resets the idle timer (grace period after it stops)\n");
    setup(1000, 10, 0);
    idle_tick(5000, 5000, false, true);     CHECK(idle_is_dimmed(),  "long idle dims");
    idle_tick(6000, 6000, true, true);      CHECK(!idle_is_dimmed(), "play wakes + resets timer");
    idle_tick(6500, 6500, false, true);     CHECK(!idle_is_dimmed(), "grace period, not re-dimmed");
    idle_tick(7001, 7001, false, true);     CHECK(idle_is_dimmed(),  "dims a full timeout after playback stopped");

    printf("# powers off after threshold when idle, not playing, on battery\n");
    setup(1000, 10, 5000);
    idle_tick(4999, 4999, false, true);     CHECK(g_poweroff_calls == 0, "just under off threshold");
    idle_tick(5000, 5000, false, true);     CHECK(g_poweroff_calls == 1, "at threshold powers off");

    printf("# never powers off while playing\n");
    setup(1000, 10, 5000);
    idle_tick(60000, 60000, true, true);    CHECK(g_poweroff_calls == 0, "playing never powers off");

    printf("# never powers off when not on battery (charging)\n");
    setup(1000, 10, 5000);
    idle_tick(60000, 60000, false, false);  CHECK(g_poweroff_calls == 0, "not allowed -> no power off");

    printf("# deferred: crosses threshold while charging, then unplugged -> fires\n");
    setup(1000, 10, 5000);
    idle_tick(6000, 6000, false, false);    CHECK(g_poweroff_calls == 0, "charging: deferred");
    idle_tick(6001, 6001, false, true);     CHECK(g_poweroff_calls == 1, "unplugged while idle -> fires");

    printf("# fires exactly once when callback succeeds\n");
    setup(1000, 10, 5000);
    idle_tick(5000, 5000, false, true);     CHECK(g_poweroff_calls == 1, "first crossing fires");
    idle_tick(5100, 5100, false, true);     CHECK(g_poweroff_calls == 1, "does not fire again");

    printf("# retries when callback fails (returns false)\n");
    setup(1000, 10, 5000);
    g_poweroff_ret = false;
    idle_tick(5000, 5000, false, true);     CHECK(g_poweroff_calls == 1, "first attempt");
    idle_tick(5100, 5100, false, true);     CHECK(g_poweroff_calls == 2, "retries after failure");

    printf("# power-off disabled (after_ms=0) never fires\n");
    setup(1000, 10, 0);
    idle_tick(60000, 60000, false, true);   CHECK(g_poweroff_calls == 0, "disabled -> no power off");

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? EXIT_FAILURE : EXIT_SUCCESS;
}
```

- [ ] **Step 4: Run the idle test to verify red→green**

```bash
export PATH="/c/msys64/mingw64/bin:$PATH"
gcc -std=c11 -Wall -Wextra -Ifirmware/main tests/test_idle.c firmware/main/idle.c -o test_idle.exe && ./test_idle.exe
```
Expected: PASS — ends with `N passed, 0 failed`. (If you build the test against the *old* `idle.c` first, the power-off checks fail — that is the red state confirming the new tests exercise real logic.)

- [ ] **Step 5: Update the sim (`sim/main_sim.c`)**

Add a logging power-off stub after `sim_get_active()` (around line 45):

```c
// Idle power-off path: the sim can't cut power, so just report it.
static bool sim_power_off(void)
{
    printf("power off requested\n");
    fflush(stdout);
    return true;
}
```

Replace the `idle_tick(...)` call in `tick_cb()`:

```c
    idle_tick(lv_display_get_inactive_time(NULL), millis(),
              s_vm.playback == PB_PLAYING, true);   // sim: always "on battery"
```

Replace the `idle_cfg_t` initializer in `main()`:

```c
    // Short idle windows so dim + power-off are observable in a short sim run.
    idle_cfg_t icfg = {
        .dim_after_ms       = 1500,
        .dim_percent        = 10,
        .power_off_after_ms = 8000,
        .apply              = sim_apply,
        .get_active         = sim_get_active,
        .power_off          = sim_power_off,
    };
```

- [ ] **Step 6: Wire the firmware (`firmware/main/main.c`)**

Add the power-off callback in the idle-dim callback block (after `idle_get_active()`, ~line 126):

```c
#if CONFIG_YTM_IDLE_POWEROFF_ENABLE
// Idle power-off: blank the panel, then AXP2101 soft power-off. Returns false if
// the I2C write fails so idle_tick retries; on success the rails drop within ms.
static bool idle_power_off(void)
{
    bsp_display_brightness_set(0);
    return battery_power_off();
}
#endif
```

Replace the `idle_cfg_t icfg = { ... }` initializer in `app_main()` (~line 172):

```c
    idle_cfg_t icfg = {
        .dim_after_ms = CONFIG_YTM_IDLE_DIM_MS,
        .dim_percent  = CONFIG_YTM_IDLE_DIM_PERCENT,
#if CONFIG_YTM_IDLE_POWEROFF_ENABLE
        .power_off_after_ms = CONFIG_YTM_IDLE_POWEROFF_MS,
        .power_off          = idle_power_off,
#else
        .power_off_after_ms = 0,
        .power_off          = NULL,
#endif
        .apply        = idle_apply,
        .get_active   = idle_get_active,
    };
```

Replace the `idle_tick(...)` call in `tick_cb()` (~line 66) with a battery-gated version:

```c
#if CONFIG_YTM_IDLE_DIM_ENABLE
    bool on_battery = true;
#if CONFIG_YTM_IDLE_POWEROFF_ENABLE
    {
        battery_status_t pb;
        battery_get(&pb);
        on_battery = !pb.external_power;   // never power off while on USB/charging
    }
#endif
    idle_tick(lv_display_get_inactive_time(NULL),
              (uint32_t)(esp_timer_get_time() / 1000),
              s_vm.playback == PB_PLAYING,
              on_battery);
#endif
```

- [ ] **Step 7: Re-run the idle host test (still green after edits)**

```bash
export PATH="/c/msys64/mingw64/bin:$PATH"
gcc -std=c11 -Wall -Wextra -Ifirmware/main tests/test_idle.c firmware/main/idle.c -o test_idle.exe && ./test_idle.exe
```
Expected: PASS — `N passed, 0 failed`.

- [ ] **Step 8: Commit**

```bash
git add firmware/main/idle.h firmware/main/idle.c tests/test_idle.c sim/main_sim.c firmware/main/main.c
git commit -m "feat(firmware): power off after long idle on battery (AXP2101)"
```

---

## Task 5: Build verification + docs

**Files:**
- Modify: `docs/CONFIGURATION.md`

- [ ] **Step 1: Build the sim (confirms sim + idle wiring compile & run)**

Build the desktop sim with the MinGW toolchain and run it headless (per the invocation documented in `sim/main_sim.c`). From the worktree root:

```bash
export PATH="/c/msys64/mingw64/bin:$PATH"
cmake -G "MinGW Makefiles" -B build-sim sim && cmake --build build-sim
SIM_MAX_FRAMES=120 SDL_VIDEODRIVER=dummy ./build-sim/ytm_sim.exe
```
Expected: builds with no errors; runs and prints `headless: 120 frames rendered, exiting.` (If your local sim build differs, use the project's documented sim build — the code change to verify is just that `main_sim.c` compiles against the new `idle` API.)

- [ ] **Step 2: Build the firmware (confirms main.c + battery + Kconfig compile)**

Initialize ESP-IDF (see `firmware/README.md` / your `Initialize-Idf.ps1` recipe), then from `firmware/`:

```
idf.py build
```
Expected: build succeeds. Sanity-check that `menuconfig` shows **YT Music board → Auto power-off after long idle (battery only)** and **Idle time before power-off (ms)** (the latter only when the former is enabled).

- [ ] **Step 3: Document the Kconfig options**

In `docs/CONFIGURATION.md`, add these rows to the firmware Kconfig table (immediately after the `Display brightness` row, before the Wi-Fi rows). These backfill the idle-dim options (currently undocumented) and add the new power-off options:

```
| Auto-dim on idle | `YTM_IDLE_DIM_ENABLE` | `y` | Dim the screen after `YTM_IDLE_DIM_MS` of no touch/motion while nothing is playing; any touch or IMU motion restores brightness. Off = screen never auto-dims. |
| Idle time before dimming | `YTM_IDLE_DIM_MS` | `30000` | Milliseconds idle before dimming. |
| Dimmed brightness | `YTM_IDLE_DIM_PERCENT` | `10` | Panel brightness (%) while dimmed. |
| Auto power-off on idle | `YTM_IDLE_POWEROFF_ENABLE` | `y` | Hard power-off via the AXP2101 after `YTM_IDLE_POWEROFF_MS` of no touch/motion, nothing playing, **and on battery** (not while charging). Wake = PWRON key press (cold boot). Requires `YTM_IDLE_DIM_ENABLE`. |
| Idle time before power-off | `YTM_IDLE_POWEROFF_MS` | `600000` | Milliseconds idle (on battery) before powering off. Should exceed `YTM_IDLE_DIM_MS`. Default 10 min. |
```

- [ ] **Step 4: Commit**

```bash
git add docs/CONFIGURATION.md
git commit -m "docs(config): document idle-dim + auto power-off Kconfig options"
```

- [ ] **Step 5: On-device verification (hardware required)**

Flash a battery build and confirm:
- On battery, screen dims after `YTM_IDLE_DIM_MS`, then the board powers off after `YTM_IDLE_POWEROFF_MS`; a PWRON press cold-boots it back.
- While charging / on USB, it dims but does **not** power off.
- While playing, it neither dims nor powers off.
- `YTM_IDLE_POWEROFF_ENABLE=n` disables power-off (dimming still works).

If the AXP2101 does not actually cut power, re-check reg `0x10` bit 0 against the datasheet/XPowersLib; if `external_power` reads wrong (powers off while on USB, or never on battery), re-check `STATUS1` bit 5.

---

## Self-review notes

- **Spec coverage:** external-power decode (T1), `battery_power_off` (T2), Kconfig `_ENABLE`/`_MS` with `depends on YTM_IDLE_DIM_ENABLE` (T3), idle threshold + fire-once + retry + battery gate + main/sim wiring (T4), tests (T1/T4), docs + build + on-device (T5). All spec sections map to a task.
- **Buildability:** every commit leaves host tests, sim, and firmware buildable — the `idle_tick` signature change and its callers land in one commit (T4).
- **Type consistency:** `battery_status_t.external_power`, `battery_power_off()`, `idle_cfg_t.{power_off_after_ms,power_off}`, and `idle_tick(..., bool power_off_allowed)` are used identically across header, impl, tests, sim, and main.
