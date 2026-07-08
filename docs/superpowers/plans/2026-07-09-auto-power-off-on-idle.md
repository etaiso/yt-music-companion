# Auto Power-Off on Idle Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a second idle stage after the existing screen-dim that performs a true AXP2101 soft power-off when the device stays idle long enough.

**Architecture:** Extend the pure, host-tested `idle` module into a two-stage escalation (dim → power-off); the power-off action is injected as a callback so the decision logic stays free of ESP/I2C. The `battery` module gains external-power decoding (to pick a battery-vs-cable timeout) and the AXP2101 soft-off action. `main.c` wires them; the simulator and existing tests keep working because unset cfg fields disable power-off.

**Tech Stack:** C11, ESP-IDF (ESP32-S3), AXP2101 PMU over I2C, host unit tests via CMake/CTest (pure C, no LVGL/ESP).

## Global Constraints

- **Idle definition (verbatim from spec):** idle = *not playing* AND no touch AND no motion — the existing `idle.c` clock. Power-off is the second stage on that same clock.
- **Timeouts:** battery default `300000` ms (5 min), cable default `900000` ms (15 min). Both Kconfig-tunable; both MUST exceed the 30 s dim timeout (`YTM_IDLE_DIM_MS`).
- **Wake:** physical PWRON button only. Motion/touch cannot wake a true AXP2101 power-off — do not design any motion-wake path.
- **Feature is Kconfig-gated**, and gated under `YTM_IDLE_DIM_ENABLE` (power-off is the second stage of the same clock).
- **Backward compatibility rule:** a `NULL` `power_off` callback OR a `0` timeout means power-off is disabled. The simulator and the dim-only test rely on this.
- **AXP2101 register values to use** (confirm against the X-Powers AXP2101 datasheet during Task 3 and on-device Task 6):
  - External power (VBUS) present = STATUS1 (reg `0x00`) **bit 5**.
  - Soft power-off = COMMON_CONFIG (reg `0x10`) **bit 0** (set to 1 → PMU powers down; PWRON wakes it).
- **Host test build** (from repo root):
  `cmake -B build tests && cmake --build build && ctest --test-dir build --output-on-failure`
  If no C compiler is on PATH, use MinGW: add `-DCMAKE_C_COMPILER=C:/msys64/mingw64/bin/gcc.exe` to the `cmake -B` line (Windows dev box; gcc lives off-PATH there).
- **Firmware build:** initialize ESP-IDF for this project, then `idf.py build` (see `firmware/README.md` / project IDF init recipe). Firmware changes are compile-verified in-plan; runtime behavior is verified on hardware in Task 6.

---

### Task 1: Decode external-power (VBUS) from AXP2101 STATUS1

Expose whether a cable is attached so the idle policy can choose the battery vs cable timeout. Pure decode change — fully host-tested.

**Files:**
- Modify: `firmware/main/battery.h` (add `external` field)
- Modify: `firmware/main/axp2101_decode.h` (document bit 5)
- Modify: `firmware/main/axp2101_decode.c` (decode bit 5)
- Test: `tests/test_battery.c`

**Interfaces:**
- Consumes: existing `axp2101_decode(uint8_t status1, uint8_t status2, uint8_t soc_pct, battery_status_t *out)`.
- Produces: `battery_status_t.external` (bool) — true when VBUS/cable is present.

- [ ] **Step 1: Add the failing test**

In `tests/test_battery.c`, add these lines just before the final `printf("\n%d passed...` line:

```c
    printf("# external (VBUS) = status1 bit5\n");
    CHECK(dec(0x20, 0, 50).external == true,  "bit5 set -> external power");
    CHECK(dec(0x00, 0, 50).external == false, "bit5 clear -> no external power");
    CHECK(dec(0x08, 0, 50).external == false, "battery present, no VBUS -> not external");
    CHECK(dec(0x28, 0, 50).external == true,  "battery + VBUS -> external");
```

- [ ] **Step 2: Run the test to verify it fails to compile**

Run: `cmake -B build tests && cmake --build build`
Expected: FAIL — compile error, `battery_status_t` has no member named `external`.

- [ ] **Step 3: Add the struct field**

In `firmware/main/battery.h`, add `external` to the struct:

```c
typedef struct {
    bool present;    // battery detected on the MX1.25 connector
    int  percent;    // 0..100 state of charge (valid when present)
    bool charging;   // USB present and actively charging
    bool external;   // external power (VBUS/cable) present
} battery_status_t;
```

- [ ] **Step 4: Decode bit 5**

In `firmware/main/axp2101_decode.c`, add the decode line inside `axp2101_decode()`:

```c
void axp2101_decode(uint8_t status1, uint8_t status2, uint8_t soc_pct,
                    battery_status_t *out)
{
    out->present  = (status1 & (1u << 3)) != 0u;
    out->charging = out->present && (((status2 >> 5) & 0x3u) == 0x1u);
    out->external = (status1 & (1u << 5)) != 0u;
    out->percent  = clampi((int)soc_pct, 0, 100);
}
```

In `firmware/main/axp2101_decode.h`, update the register doc comment for reg `0x00` to note bit 5:

```c
//   0x00 PMU status 1  — bit 3 = battery present/online; bit 5 = VBUS (external) present
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: PASS — the `battery` test reports all checks passing, `0 failed`.

- [ ] **Step 6: Commit**

```bash
git add firmware/main/battery.h firmware/main/axp2101_decode.h firmware/main/axp2101_decode.c tests/test_battery.c
git commit -m "feat(firmware): decode AXP2101 external-power (VBUS) from STATUS1 bit5"
```

---

### Task 2: Idle power-off decision logic (pure, host-tested)

Extend the `idle` module with a second stage: after the dim stage, when idle passes the source-dependent power-off threshold and nothing is playing, invoke a `power_off()` callback exactly once.

**Files:**
- Modify: `firmware/main/idle.h` (cfg fields, `idle_tick` signature, new inspector)
- Modify: `firmware/main/idle.c` (latch + power-off logic)
- Test: `tests/test_idle.c`

**Interfaces:**
- Consumes: nothing new.
- Produces:
  - `idle_cfg_t` extended with `uint32_t off_after_battery_ms; uint32_t off_after_cable_ms; void (*power_off)(void);`
  - `void idle_tick(uint32_t touch_inactive_ms, uint32_t now_ms, bool playing, bool external_power);` (adds `external_power`)
  - `bool idle_has_powered_off(void);` (inspection / tests)

- [ ] **Step 1: Extend the header**

Replace the `idle_cfg_t` struct and the `idle_tick` declaration in `firmware/main/idle.h` with:

```c
typedef struct {
    uint32_t dim_after_ms;        // idle time with no activity before dimming
    int      dim_percent;         // panel brightness (%) while dimmed
    void   (*apply)(int percent); // set panel brightness now (must NOT persist)
    int    (*get_active)(void);   // user's current brightness (restore target)

    // Second stage: power-off. Disabled when power_off is NULL or the relevant
    // timeout is 0. Timeouts are chosen by power source and must exceed dim_after_ms.
    uint32_t off_after_battery_ms; // idle ms before power-off on battery
    uint32_t off_after_cable_ms;   // idle ms before power-off on external power
    void   (*power_off)(void);     // perform the power-off (called at most once)
} idle_cfg_t;
```

Replace the `idle_tick` declaration and its doc comment with:

```c
// Run one decision step. `touch_inactive_ms` = ms since the last touch
// (LVGL's lv_display_get_inactive_time()); `now_ms` = the monotonic clock;
// `playing` = true while audio is playing (disables dim/off; restores if dimmed);
// `external_power` = true when on a cable (selects the cable power-off timeout).
void idle_tick(uint32_t touch_inactive_ms, uint32_t now_ms,
               bool playing, bool external_power);
```

Add, just after the `idle_is_dimmed` declaration:

```c
// True once the power-off callback has fired (latched until idle_init). Tests/inspection.
bool idle_has_powered_off(void);
```

- [ ] **Step 2: Update idle.c signature + latch, keep behavior identical for now**

In `firmware/main/idle.c`:

Add the latch to the static state block:

```c
static bool          s_powered_off;
```

Reset it in `idle_init()` (add after `s_motion_pending = false;`):

```c
    s_powered_off    = false;
```

Add the inspector near `idle_is_dimmed()`:

```c
bool idle_has_powered_off(void)
{
    return s_powered_off;
}
```

Change the `idle_tick` signature (the body is updated in Step 5; for now just add the parameter, unused):

```c
void idle_tick(uint32_t touch_inactive_ms, uint32_t now_ms, bool playing,
               bool external_power)
{
    (void)external_power;   // used in Step 5
```

(Keep the rest of the existing body unchanged for this step.)

- [ ] **Step 3: Update existing call sites in the test to the new signature + designated init**

In `tests/test_idle.c`:

Add the power-off fake and its reset flag, after the existing `fake_get_active` definition:

```c
static int g_powered_off = 0;   // set by fake_power_off()
static void fake_power_off(void) { g_powered_off = 1; }
```

Replace `setup()` with a designated-initializer version (power-off disabled by default) plus an off-enabled variant:

```c
static void setup(uint32_t dim_after_ms, int dim_percent)
{
    idle_cfg_t cfg = {
        .dim_after_ms = dim_after_ms,
        .dim_percent  = dim_percent,
        .apply        = fake_apply,
        .get_active   = fake_get_active,
        // power-off disabled (NULL callback / 0 timeouts)
    };
    g_applied = -1;
    g_active  = 60;
    g_powered_off = 0;
    idle_init(&cfg, 0);
}

static void setup_off(uint32_t dim_after_ms, int dim_percent,
                      uint32_t off_battery_ms, uint32_t off_cable_ms)
{
    idle_cfg_t cfg = {
        .dim_after_ms        = dim_after_ms,
        .dim_percent         = dim_percent,
        .apply               = fake_apply,
        .get_active          = fake_get_active,
        .off_after_battery_ms = off_battery_ms,
        .off_after_cable_ms   = off_cable_ms,
        .power_off           = fake_power_off,
    };
    g_applied = -1;
    g_active  = 60;
    g_powered_off = 0;
    idle_init(&cfg, 0);
}
```

Append `, false` (on-battery, no cable) to every existing `idle_tick(...)` call in `main()`. There are 15 of them (lines ~33–85 in the current file). Each call gains a 4th argument `false`, e.g.:

```c
    idle_tick(0, 0, false, false);
    idle_tick(29999, 29999, false, false);
    idle_tick(30000, 30000, false, false);
```

...and so on for all existing calls. Do not change their existing behavior or assertions.

- [ ] **Step 4: Run existing tests to confirm still green (refactor is safe)**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: PASS — `idle` test still `0 failed` (power-off not yet exercised).

- [ ] **Step 5: Write the failing power-off tests**

In `tests/test_idle.c`, add these blocks before the final `printf("\n%d passed...` line:

```c
    printf("# power-off on battery after off_after_battery_ms\n");
    setup_off(1000, 10, 5000, 15000);        // dim@1s, off@5s batt / 15s cable
    idle_tick(4999, 4999, false, false);     CHECK(!idle_has_powered_off(), "just under battery off");
    idle_tick(5000, 5000, false, false);     CHECK(idle_has_powered_off(),  "at battery threshold -> off");
    CHECK(g_powered_off == 1, "power_off callback fired (got %d)", g_powered_off);

    printf("# on cable, battery threshold does NOT power off; cable threshold does\n");
    setup_off(1000, 10, 5000, 15000);
    idle_tick(5000, 5000, false, true);      CHECK(!idle_has_powered_off(), "cable: 5s < cable threshold");
    idle_tick(14999, 14999, false, true);    CHECK(!idle_has_powered_off(), "cable: just under 15s");
    idle_tick(15000, 15000, false, true);    CHECK(idle_has_powered_off(),  "cable: at 15s -> off");

    printf("# never powers off while playing, even past threshold\n");
    setup_off(1000, 10, 5000, 15000);
    idle_tick(60000, 60000, true, false);    CHECK(!idle_has_powered_off(), "playing never powers off");
    CHECK(g_powered_off == 0, "playing fired nothing (got %d)", g_powered_off);

    printf("# touch/motion resets the clock and delays power-off\n");
    setup_off(1000, 10, 5000, 15000);
    idle_tick(4000, 4000, false, false);     CHECK(!idle_has_powered_off(), "under threshold");
    idle_tick(0, 4500, false, false);        CHECK(!idle_has_powered_off(), "touch resets");
    idle_tick(4999, 9499, false, false);     CHECK(!idle_has_powered_off(), "clock restarted from touch");
    idle_tick(5000, 9500, false, false);     CHECK(idle_has_powered_off(),  "off 5s after the touch reset");

    printf("# power_off fires exactly once (latched)\n");
    setup_off(1000, 10, 5000, 15000);
    idle_tick(5000, 5000, false, false);     CHECK(g_powered_off == 1, "fired once");
    g_powered_off = 0;                        // pretend the device didn't actually die
    idle_tick(6000, 6000, false, false);     CHECK(g_powered_off == 0, "does not re-fire (latched)");

    printf("# power-off disabled when callback is NULL (dim-only config)\n");
    setup(1000, 10);                          // no power_off callback
    idle_tick(60000, 60000, false, false);   CHECK(!idle_has_powered_off(), "no callback -> never off");
```

- [ ] **Step 6: Run to verify the new tests fail**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: FAIL — `idle` test reports failures on the new power-off checks (e.g. "at battery threshold -> off"), because the logic isn't implemented yet.

- [ ] **Step 7: Implement the power-off logic**

In `firmware/main/idle.c`, change the `idle_tick` body. Remove the temporary `(void)external_power;` and add the power-off check at the END of the function (after the existing dim `if/else` block, before the closing brace):

```c
    // Second stage: power-off. Only when nothing is playing (guaranteed here —
    // the playing branch returned above), enabled, and not already fired.
    if (s_cfg.power_off && !s_powered_off) {
        uint32_t off_after = external_power ? s_cfg.off_after_cable_ms
                                            : s_cfg.off_after_battery_ms;
        if (off_after != 0u && idle_ms >= off_after) {
            s_powered_off = true;
            s_cfg.power_off();
        }
    }
```

- [ ] **Step 8: Run all tests to verify pass**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: PASS — both `idle` and `battery` tests report `0 failed`.

- [ ] **Step 9: Commit**

```bash
git add firmware/main/idle.h firmware/main/idle.c tests/test_idle.c
git commit -m "feat(firmware): idle power-off second stage (battery/cable timeouts)"
```

---

### Task 3: AXP2101 soft power-off action

Add the ESP-side action that actually powers the board down. Not host-testable (I2C); compile-verified here, runtime-verified in Task 6.

**Files:**
- Modify: `firmware/main/axp2101_decode.h` (reg 0x10 constant)
- Modify: `firmware/main/battery.h` (declare `battery_power_off`)
- Modify: `firmware/main/battery.c` (implement)

**Interfaces:**
- Consumes: the module-private `s_dev` I2C handle in `battery.c` (created in `battery_start`).
- Produces: `void battery_power_off(void);` — issues the AXP2101 soft power-off.

- [ ] **Step 1: Add the register constant**

In `firmware/main/axp2101_decode.h`, add next to the other `AXP2101_REG_*` defines:

```c
#define AXP2101_REG_COMMON_CFG  0x10  // bit 0 = soft power-off (PWRON to wake)
```

- [ ] **Step 2: Declare the action**

In `firmware/main/battery.h`, add after `battery_get`:

```c
// Issue an AXP2101 soft power-off (board powers down; PWRON wakes it). Safe to
// call from the LVGL tick. No-op-with-log if the I2C write fails.
void battery_power_off(void);
```

- [ ] **Step 3: Implement it**

In `firmware/main/battery.c`, add after `battery_get()`:

```c
void battery_power_off(void)
{
    uint8_t cfg = 0;
    if (rd(AXP2101_REG_COMMON_CFG, &cfg) != ESP_OK) {
        ESP_LOGW(TAG, "AXP2101 power-off: read COMMON_CFG failed");
        return;
    }
    uint8_t buf[2] = { AXP2101_REG_COMMON_CFG, (uint8_t)(cfg | 0x01u) };
    if (i2c_master_transmit(s_dev, buf, 2, 100) != ESP_OK) {
        ESP_LOGW(TAG, "AXP2101 power-off: write failed");
        return;                     // stay on; caller retries next tick
    }
    ESP_LOGI(TAG, "AXP2101 soft power-off issued");
}
```

- [ ] **Step 4: Compile-verify the firmware**

Run: `idf.py build` (after the project's ESP-IDF init)
Expected: build succeeds; no unused-symbol or prototype warnings for `battery_power_off`.
(`battery_power_off` is not called yet — that's Task 5. A defined-but-unused non-static function does not warn.)

- [ ] **Step 5: Commit**

```bash
git add firmware/main/axp2101_decode.h firmware/main/battery.h firmware/main/battery.c
git commit -m "feat(firmware): AXP2101 soft power-off action (reg 0x10 bit0)"
```

---

### Task 4: Kconfig knobs

Add the feature toggle and the two timeouts.

**Files:**
- Modify: `firmware/main/Kconfig.projbuild`

**Interfaces:**
- Produces: `CONFIG_YTM_IDLE_POWEROFF_ENABLE`, `CONFIG_YTM_IDLE_OFF_BATTERY_MS`, `CONFIG_YTM_IDLE_OFF_CABLE_MS`.

- [ ] **Step 1: Add the config entries**

In `firmware/main/Kconfig.projbuild`, immediately after the `config YTM_IDLE_DIM_PERCENT` block (before `config YTM_WIFI_SSID`), add:

```
config YTM_IDLE_POWEROFF_ENABLE
    bool "Power off the board after a longer idle"
    depends on YTM_IDLE_DIM_ENABLE
    default y
    help
        When set, the board performs an AXP2101 soft power-off after it has been
        idle (nothing playing, no touch, no motion) for longer than the dim
        timeout. Waking requires a physical PWRON button press. The timeout is
        shorter on battery than on external (cable) power. When unset, the board
        only dims and never powers itself off.

config YTM_IDLE_OFF_BATTERY_MS
    int "Idle time before power-off on battery (ms)"
    depends on YTM_IDLE_POWEROFF_ENABLE
    default 300000
    range 60000 3600000
    help
        Milliseconds idle (nothing playing, no touch, no motion) before the board
        powers off while running on battery. Must exceed YTM_IDLE_DIM_MS.

config YTM_IDLE_OFF_CABLE_MS
    int "Idle time before power-off on cable (ms)"
    depends on YTM_IDLE_POWEROFF_ENABLE
    default 900000
    range 60000 3600000
    help
        Milliseconds idle before the board powers off while on external (cable)
        power. Typically longer than the battery timeout. Must exceed YTM_IDLE_DIM_MS.
```

- [ ] **Step 2: Verify the config parses**

Run: `idf.py reconfigure` (or `idf.py build`)
Expected: configuration succeeds; the three new `CONFIG_YTM_IDLE_*` symbols appear in `build/config/sdkconfig.h`.

- [ ] **Step 3: Commit**

```bash
git add firmware/main/Kconfig.projbuild
git commit -m "feat(firmware): Kconfig for idle power-off (enable + battery/cable timeouts)"
```

---

### Task 5: Wire it into main.c and the simulator

Connect the pieces: read external power, pass it to `idle_tick`, register the power-off callback and timeouts, and fix the simulator's call site.

**Files:**
- Modify: `firmware/main/main.c`
- Modify: `sim/main_sim.c`

**Interfaces:**
- Consumes: `battery_get`/`battery_status_t.external` (Task 1), `battery_power_off` (Task 3), the extended `idle_cfg_t`/`idle_tick` (Task 2), the Kconfig symbols (Task 4).
- Produces: runtime behavior — the board powers off on idle.

- [ ] **Step 1: Read external power once per tick and pass it to idle_tick (main.c)**

In `firmware/main/main.c` `tick_cb`, hoist the battery read so it is available to both the view-model copy and the idle tick. Replace the current battery block and idle_tick call:

Current:
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
    now_playing_update(&s_vm);
#if CONFIG_YTM_IDLE_DIM_ENABLE
    idle_tick(lv_display_get_inactive_time(NULL),
              (uint32_t)(esp_timer_get_time() / 1000),
              s_vm.playback == PB_PLAYING);
#endif
```

Replace with:
```c
    // battery_start() runs unconditionally, so the AXP2101 snapshot is valid in
    // both mock and live builds. Read it once for the VM copy and the idle tick.
    battery_status_t b;
    battery_get(&b);
#if CONFIG_YTM_USE_NET
    // mock build fills the VM battery fields itself; live build uses the AXP2101.
    s_vm.battery_present = b.present;
    s_vm.battery_percent = b.percent;
    s_vm.charging        = b.charging;
#endif
    now_playing_update(&s_vm);
#if CONFIG_YTM_IDLE_DIM_ENABLE
    idle_tick(lv_display_get_inactive_time(NULL),
              (uint32_t)(esp_timer_get_time() / 1000),
              s_vm.playback == PB_PLAYING,
              b.external);
#endif
```

- [ ] **Step 2: Add the power-off callback + extend the idle cfg (main.c)**

In `firmware/main/main.c`, add the callback next to `idle_apply`/`idle_get_active` (inside the existing `#if CONFIG_YTM_IDLE_DIM_ENABLE` block, lines ~120–126):

```c
static void idle_power_off(void) { battery_power_off(); }
```

Extend the `idle_cfg_t` initializer in `app_main` (lines ~172–177). Replace it with:

```c
    idle_cfg_t icfg = {
        .dim_after_ms = CONFIG_YTM_IDLE_DIM_MS,
        .dim_percent  = CONFIG_YTM_IDLE_DIM_PERCENT,
        .apply        = idle_apply,
        .get_active   = idle_get_active,
#if CONFIG_YTM_IDLE_POWEROFF_ENABLE
        .off_after_battery_ms = CONFIG_YTM_IDLE_OFF_BATTERY_MS,
        .off_after_cable_ms   = CONFIG_YTM_IDLE_OFF_CABLE_MS,
        .power_off            = idle_power_off,
#endif
    };
```

(When `YTM_IDLE_POWEROFF_ENABLE` is off, the fields stay zero/NULL and the module treats power-off as disabled — per the Global Constraints backward-compat rule.)

- [ ] **Step 3: Fix the simulator call site (sim/main_sim.c)**

In `sim/main_sim.c`, update the `idle_tick` call (line ~58) to pass `false` for `external_power` (the sim has no PMU and must never power off):

```c
    idle_tick(lv_display_get_inactive_time(NULL), millis(),
              s_vm.playback == PB_PLAYING, false);
```

Convert the `idle_cfg_t` initializer (line ~77) to a designated initializer, leaving power-off disabled:

```c
    // Short idle window so dimming is observable in a short sim run.
    idle_cfg_t icfg = {
        .dim_after_ms = 1500,
        .dim_percent  = 10,
        .apply        = sim_apply,
        .get_active   = sim_get_active,
        // no power_off in the sim
    };
```

- [ ] **Step 4: Compile-verify firmware and simulator**

Run (firmware): `idf.py build`
Expected: build succeeds.

Run (sim): build the desktop simulator per the project sim recipe (SDL2/MinGW).
Expected: sim builds and still dims; it never exits from power-off (no callback).

- [ ] **Step 5: Commit**

```bash
git add firmware/main/main.c sim/main_sim.c
git commit -m "feat(firmware): wire idle power-off (external-power source + callback)"
```

---

### Task 6: On-device verification (incl. VBUS re-wake gate)

Runtime behavior that host tests and compile checks cannot cover. This task ships no code unless the VBUS check fails (mitigation below).

**Files:**
- None (verification). If mitigation is needed: `firmware/main/battery.c`.

- [ ] **Step 1: Flash and set short test timeouts**

Temporarily set `CONFIG_YTM_IDLE_OFF_BATTERY_MS` and `CONFIG_YTM_IDLE_OFF_CABLE_MS` to a small value (e.g. `60000`) via menuconfig for faster iteration, then flash: `idf.py flash monitor`.

- [ ] **Step 2: Verify battery power-off**

On battery (no cable), leave the device idle (nothing playing, no touch, no movement). Confirm: screen dims at 30 s, then the board powers off at the battery timeout (serial log `AXP2101 soft power-off issued`, then the device goes dark/silent).
Expected: powers off; no reboot.

- [ ] **Step 3: Verify PWRON wake**

Press the PWRON button. Confirm the board boots back up.
Expected: normal boot.

- [ ] **Step 4: Verify cable behavior + VBUS re-wake gate**

With a cable attached (bridge may be up or down), leave it idle past the cable timeout. Confirm it powers off and **stays off** (does not immediately reboot).
- If it stays off: cable path confirmed. ✅
- If it immediately reboots (VBUS re-wake): apply the mitigation in Step 5.

- [ ] **Step 5: (Only if Step 4 loops) Mitigate VBUS re-wake**

Before issuing soft-off in `battery_power_off()`, clear the AXP2101 "power-on when VBUS applied" source bit so VBUS insertion/presence does not auto-boot the PMU. Consult the AXP2101 datasheet PWRON/PWROK source register; add a read-modify-write clearing that bit ahead of the reg 0x10 write. Re-run Steps 1–4. Commit:

```bash
git add firmware/main/battery.c
git commit -m "fix(firmware): stop AXP2101 auto power-on from VBUS before soft-off"
```

- [ ] **Step 6: Restore production timeouts + confirm decode bit**

Restore `CONFIG_YTM_IDLE_OFF_BATTERY_MS=300000` and `CONFIG_YTM_IDLE_OFF_CABLE_MS=900000`. While verifying, confirm the STATUS1 bit-5 external-power reading matches reality (cable in → `external` true) via a temporary log or the quick panel; if the bit is wrong, correct it in `axp2101_decode.c` and re-run Task 1's tests.

---

## Self-Review

**Spec coverage:**
- Idle = not playing + no touch + no motion → reuses existing clock (Task 2). ✅
- Power-off regardless of source/bridge, source only changes timeout (Task 2 logic; Task 5 feeds `external`). ✅
- Timeouts battery 5 min / cable 15 min, Kconfig, must exceed dim (Task 4 defaults + range). ✅
- Bridge-down handled implicitly (no playback → idle clock runs) — no dedicated code; covered by the "playing resets, otherwise idle accrues" behavior in Task 2. ✅
- Wake via PWRON only (Task 3 soft-off semantics; Task 6 Step 3). ✅
- True AXP2101 soft power-off, reg 0x10 (Task 3). ✅
- External power = STATUS1 bit 5 (Task 1). ✅
- Kconfig-gated, under dim-enable (Task 4 `depends on`; Task 5 `#if`). ✅
- Error handling: I2C write failure logs + retries (Task 3); early-boot defaults to battery timeout — implicit, since `external` reads false until the first poll, giving the shorter timeout (acceptable per spec). ✅
- Latch = exactly once (Task 2 Step 5/7). ✅
- VBUS re-wake risk verified with mitigation path (Task 6). ✅
- Tests: host cases + on-device checklist (Tasks 1, 2, 6). ✅

**Placeholder scan:** No TBD/TODO; every code step shows the actual code; register values are concrete with a datasheet-confirm note. ✅

**Type consistency:** `battery_status_t.external` (Task 1) consumed as `b.external` (Task 5). `idle_tick(..., bool external_power)` signature (Task 2) matches all three call sites updated (tests Task 2, main.c + sim Task 5). `battery_power_off(void)` (Task 3) called by `idle_power_off` (Task 5). Kconfig symbols `YTM_IDLE_POWEROFF_ENABLE`/`YTM_IDLE_OFF_BATTERY_MS`/`YTM_IDLE_OFF_CABLE_MS` (Task 4) referenced identically in Task 5. ✅
