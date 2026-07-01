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

    printf("# motion path: idle alone crosses threshold and dims\n");
    setup(1000, 10);
    idle_notify_activity();               // motion at t=0 seeds last_motion
    idle_tick(0, 0, false);               CHECK(!idle_is_dimmed(), "fresh, not dimmed");
    idle_tick(1000, 1000, false);         CHECK(idle_is_dimmed(),  "idle past threshold dims");
    CHECK(g_applied == 10, "dims to dim_percent via idle (got %d)", g_applied);

    printf("# playback resets the idle timer (grace period after it stops)\n");
    setup(1000, 10);
    idle_tick(5000, 5000, false);         CHECK(idle_is_dimmed(),  "long idle dims");
    idle_tick(6000, 6000, true);          CHECK(!idle_is_dimmed(), "play wakes + resets timer");
    // Playback stops; touch has still been idle the whole time (6500), but the
    // timer was reset at 6000, so it must NOT re-dim immediately.
    idle_tick(6500, 6500, false);         CHECK(!idle_is_dimmed(), "grace period, not re-dimmed");
    idle_tick(7001, 7001, false);         CHECK(idle_is_dimmed(),  "dims a full timeout after playback stopped");

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? EXIT_FAILURE : EXIT_SUCCESS;
}
