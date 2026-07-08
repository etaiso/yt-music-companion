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

// The 4th arg to idle_tick() is external_power: false = on battery, true = on cable.
static void setup(uint32_t dim_after_ms, int dim_percent,
                  uint32_t off_battery_ms, uint32_t off_cable_ms)
{
    idle_cfg_t cfg = {
        .dim_after_ms             = dim_after_ms,
        .dim_percent              = dim_percent,
        .power_off_after_ms       = off_battery_ms,
        .power_off_after_cable_ms = off_cable_ms,
        .apply                    = fake_apply,
        .get_active               = fake_get_active,
        .power_off                = fake_power_off,
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
    setup(30000, 10, 0, 0);
    idle_tick(0, 0, false, false);          CHECK(!idle_is_dimmed(), "t=0 not dimmed");
    idle_tick(29999, 29999, false, false);  CHECK(!idle_is_dimmed(), "just under threshold");
    idle_tick(30000, 30000, false, false);  CHECK(idle_is_dimmed(),  "at threshold dims");
    CHECK(g_applied == 10, "applies dim_percent (got %d)", g_applied);

    printf("# never dims while playing, even past threshold\n");
    setup(30000, 10, 0, 0);
    idle_tick(60000, 60000, true, false);   CHECK(!idle_is_dimmed(), "playing never dims");
    CHECK(g_applied == -1, "playing applies nothing (got %d)", g_applied);

    printf("# touch activity restores\n");
    setup(1000, 10, 0, 0);
    idle_tick(1000, 1000, false, false);    CHECK(idle_is_dimmed(), "dimmed");
    g_applied = -1;
    idle_tick(0, 1050, false, false);       CHECK(!idle_is_dimmed(), "touch wakes");
    CHECK(g_applied == 60, "restore applies active (got %d)", g_applied);

    printf("# motion activity restores (beats high touch-idle)\n");
    setup(1000, 10, 0, 0);
    idle_tick(1000, 1000, false, false);    CHECK(idle_is_dimmed(), "dimmed");
    idle_notify_activity();
    g_applied = -1;
    idle_tick(2000, 2000, false, false);    CHECK(!idle_is_dimmed(), "motion wakes");
    CHECK(g_applied == 60, "restore applies active (got %d)", g_applied);

    printf("# playback starting while dimmed restores and stays lit\n");
    setup(1000, 10, 0, 0);
    idle_tick(1000, 1000, false, false);    CHECK(idle_is_dimmed(), "dimmed");
    g_applied = -1;
    idle_tick(2000, 2000, true, false);     CHECK(!idle_is_dimmed(), "play wakes");
    CHECK(g_applied == 60, "restore to active on play (got %d)", g_applied);

    printf("# restore uses LIVE active brightness\n");
    setup(1000, 10, 0, 0);
    idle_tick(1000, 1000, false, false);    CHECK(idle_is_dimmed(), "dimmed");
    g_active = 25;                          // user changed brightness meanwhile
    idle_tick(0, 1050, false, false);       CHECK(g_applied == 25, "reads live active (got %d)", g_applied);

    printf("# motion path: idle alone crosses threshold and dims\n");
    setup(1000, 10, 0, 0);
    idle_notify_activity();                 // motion at t=0 seeds last_motion
    idle_tick(0, 0, false, false);          CHECK(!idle_is_dimmed(), "fresh, not dimmed");
    idle_tick(1000, 1000, false, false);    CHECK(idle_is_dimmed(),  "idle past threshold dims");
    CHECK(g_applied == 10, "dims to dim_percent via idle (got %d)", g_applied);

    printf("# playback resets the idle timer (grace period after it stops)\n");
    setup(1000, 10, 0, 0);
    idle_tick(5000, 5000, false, false);    CHECK(idle_is_dimmed(),  "long idle dims");
    idle_tick(6000, 6000, true, false);     CHECK(!idle_is_dimmed(), "play wakes + resets timer");
    idle_tick(6500, 6500, false, false);    CHECK(!idle_is_dimmed(), "grace period, not re-dimmed");
    idle_tick(7001, 7001, false, false);    CHECK(idle_is_dimmed(),  "dims a full timeout after playback stopped");

    printf("# on battery: powers off at the battery timeout\n");
    setup(1000, 10, 5000, 15000);
    idle_tick(4999, 4999, false, false);    CHECK(g_poweroff_calls == 0, "just under battery threshold");
    idle_tick(5000, 5000, false, false);    CHECK(g_poweroff_calls == 1, "at battery threshold powers off");

    printf("# on cable: battery timeout does NOT fire; cable timeout does\n");
    setup(1000, 10, 5000, 15000);
    idle_tick(5000, 5000, false, true);     CHECK(g_poweroff_calls == 0, "cable: past battery timeout, no off");
    idle_tick(14999, 14999, false, true);   CHECK(g_poweroff_calls == 0, "cable: just under cable threshold");
    idle_tick(15000, 15000, false, true);   CHECK(g_poweroff_calls == 1, "cable: at cable threshold powers off");

    printf("# never powers off while playing (battery or cable)\n");
    setup(1000, 10, 5000, 15000);
    idle_tick(60000, 60000, true, false);   CHECK(g_poweroff_calls == 0, "playing on battery never off");
    idle_tick(60000, 60000, true, true);    CHECK(g_poweroff_calls == 0, "playing on cable never off");

    printf("# source switch: idle on cable below cable timeout, then unplugged past battery timeout -> fires\n");
    setup(1000, 10, 5000, 15000);
    idle_tick(6000, 6000, false, true);     CHECK(g_poweroff_calls == 0, "cable: 6000 < cable timeout, deferred");
    idle_tick(6001, 6001, false, false);    CHECK(g_poweroff_calls == 1, "unplugged, idle past battery timeout -> fires");

    printf("# fires exactly once when callback succeeds\n");
    setup(1000, 10, 5000, 15000);
    idle_tick(5000, 5000, false, false);    CHECK(g_poweroff_calls == 1, "first crossing fires");
    idle_tick(5100, 5100, false, false);    CHECK(g_poweroff_calls == 1, "does not fire again");

    printf("# retries when callback fails (returns false)\n");
    setup(1000, 10, 5000, 15000);
    g_poweroff_ret = false;
    idle_tick(5000, 5000, false, false);    CHECK(g_poweroff_calls == 1, "first attempt");
    idle_tick(5100, 5100, false, false);    CHECK(g_poweroff_calls == 2, "retries after failure");

    printf("# power-off disabled (both timeouts 0) never fires\n");
    setup(1000, 10, 0, 0);
    idle_tick(60000, 60000, false, false);  CHECK(g_poweroff_calls == 0, "battery disabled -> no off");
    idle_tick(60000, 60000, false, true);   CHECK(g_poweroff_calls == 0, "cable disabled -> no off");

    printf("# per-source disable: battery=0 disables battery only; cable still fires\n");
    setup(1000, 10, 0, 15000);
    idle_tick(60000, 60000, false, false);  CHECK(g_poweroff_calls == 0, "battery=0 -> never off on battery");
    setup(1000, 10, 0, 15000);
    idle_tick(15000, 15000, false, true);   CHECK(g_poweroff_calls == 1, "cable timeout still fires");

    printf("# per-source disable: cable=0 disables cable only; battery still fires\n");
    setup(1000, 10, 5000, 0);
    idle_tick(60000, 60000, false, true);   CHECK(g_poweroff_calls == 0, "cable=0 -> never off on cable");
    setup(1000, 10, 5000, 0);
    idle_tick(5000, 5000, false, false);    CHECK(g_poweroff_calls == 1, "battery timeout still fires");

    printf("# dims before power-off threshold; no power-off while only dimmed\n");
    setup(1000, 10, 5000, 15000);
    idle_tick(4999, 4999, false, false);
    CHECK(idle_is_dimmed(), "dimmed at 4999");
    CHECK(g_poweroff_calls == 0, "no power-off before its threshold (got %d)", g_poweroff_calls);

    printf("# touch activity before the power-off threshold prevents power-off\n");
    setup(1000, 10, 5000, 15000);
    idle_tick(4000, 4000, false, false);    CHECK(g_poweroff_calls == 0, "dimmed, not yet off");
    idle_tick(0, 4050, false, false);       // touch resets idle
    idle_tick(4999, 9049, false, false);    // wall clock past 5000, but touch idle only 4999
    CHECK(g_poweroff_calls == 0, "recent touch keeps idle below threshold -> no power-off (got %d)", g_poweroff_calls);

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? EXIT_FAILURE : EXIT_SUCCESS;
}
