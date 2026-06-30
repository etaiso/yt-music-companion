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

#define TICK_MS 33

static now_playing_vm_t s_vm;

static uint32_t millis(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static void sim_brightness(int percent)
{
    printf("brightness: %d%%\n", percent);
    fflush(stdout);
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
