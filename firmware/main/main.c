// main.c — ESP-IDF entry for the YT Music board.
//
// Brings up the vendor BSP (display + touch + LVGL v9), mounts the Now Playing
// screen, and drives it from mock data. NO network code yet (SPEC §8): render
// reads only now_playing_vm_t; controls log via the emit() stub over serial.
#include "bsp/esp-bsp.h"      // Waveshare BSP: bsp_display_start/lock/unlock
#include "esp_log.h"
#include "lvgl.h"

#include "now_playing_screen.h"
#include "mock.h"

static const char *TAG = "ytm";

#define TICK_MS 33  // ~30 fps

static now_playing_vm_t s_vm;

// Command sink for the slice: log intent over serial. A later task wires this
// to the Mac-side bridge (SPEC-ytmusic-adapter.md).
static void serial_emit(const char *cmd, int arg)
{
    ESP_LOGI(TAG, "emit: %s (%d)", cmd, arg);
}

// Runs inside the BSP's LVGL task (lock already held), so it's safe to touch
// widgets directly here.
static void tick_cb(lv_timer_t *t)
{
    (void)t;
    mock_tick(&s_vm, TICK_MS);
    now_playing_update(&s_vm);
}

void app_main(void)
{
    bsp_display_start();          // panel + touch + LVGL up
    bsp_display_backlight_on();

    bsp_display_lock(0);          // guard LVGL while we build the UI
    now_playing_set_emit(serial_emit);
    mock_init(&s_vm);
    now_playing_create(lv_screen_active());
    now_playing_update(&s_vm);
    lv_timer_create(tick_cb, TICK_MS, NULL);
    bsp_display_unlock();

    ESP_LOGI(TAG, "Now Playing slice up (mock data, %d fps).", 1000 / TICK_MS);
}
