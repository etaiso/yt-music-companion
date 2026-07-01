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

    printf("# dims before power-off threshold; no power-off while only dimmed\n");
    setup(1000, 10, 5000);
    idle_tick(4999, 4999, false, true);
    CHECK(idle_is_dimmed(), "dimmed at 4999");
    CHECK(g_poweroff_calls == 0, "no power-off before its threshold (got %d)", g_poweroff_calls);

    printf("# touch activity before the power-off threshold prevents power-off\n");
    setup(1000, 10, 5000);
    idle_tick(4000, 4000, false, true);    CHECK(g_poweroff_calls == 0, "dimmed, not yet off");
    idle_tick(0, 4050, false, true);       // touch resets idle
    idle_tick(4999, 9049, false, true);    // wall clock past 5000, but touch idle only 4999
    CHECK(g_poweroff_calls == 0, "recent touch keeps idle below threshold -> no power-off (got %d)", g_poweroff_calls);

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? EXIT_FAILURE : EXIT_SUCCESS;
}
