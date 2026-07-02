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

static void sim_emit(const char *cmd, int arg)
{
    printf("emit: %s (%d)\n", cmd, arg);
    fflush(stdout);
}

// Perf harness (SIM_MARQUEE_PERF=1): pin the "playing, long title, marquee
// scrolling, position ticking once per second" scenario — no scene rotation or
// battery churn — and report invalidated pixels per main-loop iteration
// ("inval <iter> <px>" lines). The board renders in software into PSRAM, so
// invalidated area is the proxy for frame cost; a spike well above the
// marquee's steady baseline is the once-per-second stutter seen on hardware.
// Checked by scripts/check_marquee_perf.sh. SIM_INVAL_AREAS=1 additionally
// prints each invalidated rect for pinpointing which widget repainted.
static bool     s_marquee_perf;
static bool     s_inval_areas;
static uint64_t s_inval_px;

static void inval_cb(lv_event_t *e)
{
    const lv_area_t *a = lv_event_get_param(e);
    s_inval_px += (uint64_t)lv_area_get_size(a);
    if (s_inval_areas)
        printf("area %d,%d %dx%d (%d px)\n", (int)a->x1, (int)a->y1,
               (int)lv_area_get_width(a), (int)lv_area_get_height(a),
               (int)lv_area_get_size(a));
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    if (s_marquee_perf) {
        // minimal repro: only the timeline advances, once per second
        static uint32_t acc;
        acc += TICK_MS;
        if (acc >= 1000) {
            acc -= 1000;
            if (++s_vm.position_sec >= s_vm.duration_sec) s_vm.position_sec = 0;
        }
    } else {
        mock_tick(&s_vm, TICK_MS);
    }
    now_playing_update(&s_vm);
    idle_tick(lv_display_get_inactive_time(NULL), millis(),
              s_vm.playback == PB_PLAYING);
    quick_panel_set_battery(s_vm.battery_percent, s_vm.charging, s_vm.battery_present);
}

int main(void)
{
    lv_init();
    lv_tick_set_cb(millis);

    lv_display_t *disp = lv_sdl_window_create(480, 480);
    lv_sdl_mouse_create();

    now_playing_set_emit(sim_emit);
    mock_init(&s_vm);
    s_marquee_perf = getenv("SIM_MARQUEE_PERF") != NULL;
    s_inval_areas  = getenv("SIM_INVAL_AREAS") != NULL;
    if (s_marquee_perf) {
        snprintf(s_vm.title, sizeof s_vm.title,
                 "A Very Long Song Title That Definitely Overflows The Label And Scrolls");
        lv_display_add_event_cb(disp, inval_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    }
    now_playing_create(lv_screen_active());
    quick_panel_init(lv_screen_active(), sim_brightness, 40);
    s_active = 40;
    // Short idle window so dimming is observable in a short sim run.
    idle_cfg_t icfg = { 1500, 10, sim_apply, sim_get_active };
    idle_init(&icfg, millis());
    now_playing_update(&s_vm);
    lv_timer_create(tick_cb, TICK_MS, NULL);

    // optional frame cap for headless verification
    const char *cap = getenv("SIM_MAX_FRAMES");
    long max_frames = cap ? strtol(cap, NULL, 10) : 0;
    long frames = 0;

    while (1) {
        uint32_t idle = lv_timer_handler();
        if (s_marquee_perf && s_inval_px) {
            printf("inval %ld %llu\n", frames, (unsigned long long)s_inval_px);
            s_inval_px = 0;
        }
        if (idle == LV_NO_TIMER_READY) idle = 5;
        usleep((idle ? idle : 1) * 1000);
        if (max_frames && ++frames >= max_frames) {
            printf("headless: %ld frames rendered, exiting.\n", frames);
            break;
        }
    }
    return 0;
}
