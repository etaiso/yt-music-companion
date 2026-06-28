// main.c — ESP-IDF entry for the YT Music board.
//
// Brings up the vendor BSP (display + touch + LVGL v9), mounts the Now Playing
// screen, and drives it from a feed. The render layer reads only now_playing_vm_t;
// CONFIG_YTM_USE_NET picks the live bridge feed (net_backend) vs mock data.
#include "bsp/esp-bsp.h"      // Waveshare BSP: bsp_display_start/lock/unlock
#include "esp_log.h"
#include "lvgl.h"
#include "sdkconfig.h"

#include "now_playing_screen.h"

#if CONFIG_YTM_USE_NET
#include "net_backend.h"
#else
#include "mock.h"
#endif

static const char *TAG = "ytm";

#define TICK_MS 33  // ~30 fps

static now_playing_vm_t s_vm;

#if !CONFIG_YTM_USE_NET
// Mock build: log control intent over serial (no bridge to talk to).
static void serial_emit(const char *cmd, int arg)
{
    ESP_LOGI(TAG, "emit: %s (%d)", cmd, arg);
}
#endif

// Runs inside the BSP's LVGL task (lock already held), so it's safe to touch
// widgets directly here.
static void tick_cb(lv_timer_t *t)
{
    (void)t;
#if CONFIG_YTM_USE_NET
    net_backend_get_vm(&s_vm);   // copy the latest snapshot from the net task
#else
    mock_tick(&s_vm, TICK_MS);
#endif
    now_playing_update(&s_vm);
}

void app_main(void)
{
    bsp_display_start();          // panel + touch + LVGL up
    bsp_display_backlight_on();
    // AMOLED has no PWM backlight; this sends the panel's brightness command.
    // Tunable via menuconfig (YT Music board -> Display brightness). 100% is harsh.
    bsp_display_brightness_set(CONFIG_YTM_DISPLAY_BRIGHTNESS);

#if CONFIG_YTM_USE_NET
    net_backend_start();          // WiFi + mDNS + WebSocket (async)
#endif

    bsp_display_lock(0);          // guard LVGL while we build the UI
#if CONFIG_YTM_USE_NET
    now_playing_set_emit(net_backend_emit);
    net_backend_get_vm(&s_vm);
#else
    now_playing_set_emit(serial_emit);
    mock_init(&s_vm);
#endif
    now_playing_create(lv_screen_active());
    now_playing_update(&s_vm);
    lv_timer_create(tick_cb, TICK_MS, NULL);
    bsp_display_unlock();

    ESP_LOGI(TAG, "Now Playing up (%s, %d fps).",
             CONFIG_YTM_USE_NET ? "live feed" : "mock data", 1000 / TICK_MS);
}
