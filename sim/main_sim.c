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
#include "mock.h"

// --- TEMP invalidation measurement (remove in cleanup task) ---
static long g_inval_px;
static long g_bucket_frames;
static void inval_cb(lv_event_t *e)
{
    const lv_area_t *a = (const lv_area_t *)lv_event_get_param(e);
    g_inval_px += (long)(a->x2 - a->x1 + 1) * (long)(a->y2 - a->y1 + 1);
}

#define TICK_MS 33

static now_playing_vm_t s_vm;

static uint32_t millis(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
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
}

int main(void)
{
    lv_init();
    lv_tick_set_cb(millis);

    lv_display_t *disp = lv_sdl_window_create(480, 480);
    lv_display_add_event_cb(disp, inval_cb, LV_EVENT_INVALIDATE_AREA, NULL);  // TEMP
    lv_sdl_mouse_create();

    now_playing_set_emit(sim_emit);
    mock_init(&s_vm);
    now_playing_create(lv_screen_active());
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
        // TEMP: report average invalidated px/frame every 30 frames.
        if (++g_bucket_frames >= 30) {
            printf("inval@frame%ld: %ld px / %ld frames = %ld px/frame\n",
                   frames, g_inval_px, g_bucket_frames,
                   g_inval_px / g_bucket_frames);
            g_inval_px = 0;
            g_bucket_frames = 0;
            fflush(stdout);
        }
        if (max_frames && ++frames >= max_frames) {
            printf("headless: %ld frames rendered, exiting.\n", frames);
            break;
        }
    }
    return 0;
}
