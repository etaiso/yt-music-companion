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

static int g_powered_off = 0;   // set by fake_power_off()
static int g_power_off_result = 1;   // return value of fake_power_off() (1 = success)
static bool fake_power_off(void) { g_powered_off = 1; return g_power_off_result != 0; }

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
    g_power_off_result = 1;
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
    g_power_off_result = 1;
    idle_init(&cfg, 0);
}

int main(void)
{
    printf("# dims after threshold when idle and not playing\n");
    setup(30000, 10);
    idle_tick(0, 0, false, false);          CHECK(!idle_is_dimmed(), "t=0 not dimmed");
    idle_tick(29999, 29999, false, false);  CHECK(!idle_is_dimmed(), "just under threshold");
    idle_tick(30000, 30000, false, false);  CHECK(idle_is_dimmed(),  "at threshold dims");
    CHECK(g_applied == 10, "applies dim_percent (got %d)", g_applied);

    printf("# never dims while playing, even past threshold\n");
    setup(30000, 10);
    idle_tick(60000, 60000, true, false);   CHECK(!idle_is_dimmed(), "playing never dims");
    CHECK(g_applied == -1, "playing applies nothing (got %d)", g_applied);

    printf("# touch activity restores\n");
    setup(1000, 10);
    idle_tick(1000, 1000, false, false);    CHECK(idle_is_dimmed(), "dimmed");
    g_applied = -1;
    idle_tick(0, 1050, false, false);       CHECK(!idle_is_dimmed(), "touch wakes");
    CHECK(g_applied == 60, "restore applies active (got %d)", g_applied);

    printf("# motion activity restores (beats high touch-idle)\n");
    setup(1000, 10);
    idle_tick(1000, 1000, false, false);    CHECK(idle_is_dimmed(), "dimmed");
    idle_notify_activity();
    g_applied = -1;
    idle_tick(2000, 2000, false, false);    CHECK(!idle_is_dimmed(), "motion wakes");
    CHECK(g_applied == 60, "restore applies active (got %d)", g_applied);

    printf("# playback starting while dimmed restores and stays lit\n");
    setup(1000, 10);
    idle_tick(1000, 1000, false, false);    CHECK(idle_is_dimmed(), "dimmed");
    g_applied = -1;
    idle_tick(2000, 2000, true, false);     CHECK(!idle_is_dimmed(), "play wakes");
    CHECK(g_applied == 60, "restore to active on play (got %d)", g_applied);

    printf("# restore uses LIVE active brightness\n");
    setup(1000, 10);
    idle_tick(1000, 1000, false, false);    CHECK(idle_is_dimmed(), "dimmed");
    g_active = 25;                    // user changed brightness meanwhile
    idle_tick(0, 1050, false, false);       CHECK(g_applied == 25, "reads live active (got %d)", g_applied);

    printf("# motion path: idle alone crosses threshold and dims\n");
    setup(1000, 10);
    idle_notify_activity();               // motion at t=0 seeds last_motion
    idle_tick(0, 0, false, false);               CHECK(!idle_is_dimmed(), "fresh, not dimmed");
    idle_tick(1000, 1000, false, false);         CHECK(idle_is_dimmed(),  "idle past threshold dims");
    CHECK(g_applied == 10, "dims to dim_percent via idle (got %d)", g_applied);

    printf("# playback resets the idle timer (grace period after it stops)\n");
    setup(1000, 10);
    idle_tick(5000, 5000, false, false);         CHECK(idle_is_dimmed(),  "long idle dims");
    idle_tick(6000, 6000, true, false);          CHECK(!idle_is_dimmed(), "play wakes + resets timer");
    // Playback stops; touch has still been idle the whole time (6500), but the
    // timer was reset at 6000, so it must NOT re-dim immediately.
    idle_tick(6500, 6500, false, false);         CHECK(!idle_is_dimmed(), "grace period, not re-dimmed");
    idle_tick(7001, 7001, false, false);         CHECK(idle_is_dimmed(),  "dims a full timeout after playback stopped");

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

    printf("# power_off returning false does NOT latch (retries next tick)\n");
    setup_off(1000, 10, 5000, 15000);
    g_power_off_result = 0;                   // simulate I2C write failure
    idle_tick(5000, 5000, false, false);      CHECK(!idle_has_powered_off(), "failed off does not latch");
    g_power_off_result = 1;                   // next tick the write succeeds
    idle_tick(6000, 6000, false, false);      CHECK(idle_has_powered_off(),  "retries and latches on success");

    printf("# per-source zero timeout disables only that source\n");
    setup_off(1000, 10, 0, 15000);            // battery disabled, cable enabled
    idle_tick(60000, 60000, false, false);    CHECK(!idle_has_powered_off(), "battery=0 -> never off on battery");
    setup_off(1000, 10, 0, 15000);
    idle_tick(15000, 15000, false, true);     CHECK(idle_has_powered_off(),  "cable timeout still fires");

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? EXIT_FAILURE : EXIT_SUCCESS;
}
