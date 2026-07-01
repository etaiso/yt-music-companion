// main_sim.c — desktop SDL preview of the Now Playing screen.
// Runs the SAME ui/ code as the device so you can iterate without flashing.
//
//   Interactive:  ./ytm_sim
//   Headless CI:  SIM_MAX_FRAMES=120 SDL_VIDEODRIVER=dummy ./ytm_sim
#include "lvgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "now_playing_screen.h"
#include "quick_panel.h"
#include "mock.h"
#include "idle.h"

#define TICK_MS 33

static now_playing_vm_t s_vm;

static uint32_t millis(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

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

// Idle power-off path: the sim can't cut power, so just report it.
static bool sim_power_off(void)
{
    printf("power off requested\n");
    fflush(stdout);
    return true;
}

static void sim_emit(const char *cmd, int arg)
{
    printf("emit: %s (%d)\n", cmd, arg);
    fflush(stdout);
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    mock_tick(&s_vm, TICK_MS);
    now_playing_update(&s_vm);
    idle_tick(lv_display_get_inactive_time(NULL), millis(),
              s_vm.playback == PB_PLAYING, true);   // sim: always "on battery"
    quick_panel_set_battery(s_vm.battery_percent, s_vm.charging, s_vm.battery_present);
}

int main(void)
{
    lv_init();
    lv_tick_set_cb(millis);

    lv_sdl_window_create(480, 480);
    lv_sdl_mouse_create();

    now_playing_set_emit(sim_emit);
    mock_init(&s_vm);
    now_playing_create(lv_screen_active());
    quick_panel_init(lv_screen_active(), sim_brightness, 40);
    s_active = 40;
    // Short idle windows so dim + power-off are observable in a short sim run.
    idle_cfg_t icfg = {
        .dim_after_ms       = 1500,
        .dim_percent        = 10,
        .power_off_after_ms = 8000,
        .apply              = sim_apply,
        .get_active         = sim_get_active,
        .power_off          = sim_power_off,
    };
    idle_init(&icfg, millis());
    now_playing_update(&s_vm);
    lv_timer_create(tick_cb, TICK_MS, NULL);

    // optional frame cap for headless verification
    const char *cap = getenv("SIM_MAX_FRAMES");
    long max_frames = cap ? strtol(cap, NULL, 10) : 0;
    long frames = 0;

    while (1) {
        uint32_t idle = lv_timer_handler();
        if (idle == LV_NO_TIMER_READY) idle = 5;
        usleep((idle ? idle : 1) * 1000);
        if (max_frames && ++frames >= max_frames) {
            printf("headless: %ld frames rendered, exiting.\n", frames);
            break;
        }
    }
    return 0;
}
