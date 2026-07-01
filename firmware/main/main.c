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

#include "battery.h"
#include "quick_panel.h"
#include "idle.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_timer.h"

static const char *TAG = "ytm";

#define TICK_MS 33  // ~30 fps

static now_playing_vm_t s_vm;
static int s_active_bright;   // user-chosen brightness; the idle-dim restore target

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
#if CONFIG_YTM_USE_NET
    // mock build fills battery itself; live build reads the real AXP2101.
    {
        battery_status_t b;
        battery_get(&b);
        s_vm.battery_present = b.present;
        s_vm.battery_percent = b.percent;
        s_vm.charging        = b.charging;
    }
#endif
    now_playing_update(&s_vm);
#if CONFIG_YTM_IDLE_DIM_ENABLE
    idle_tick(lv_display_get_inactive_time(NULL),
              (uint32_t)(esp_timer_get_time() / 1000),
              s_vm.playback == PB_PLAYING);
#endif
    quick_panel_set_battery(s_vm.battery_percent, s_vm.charging, s_vm.battery_present);
}

#define NVS_NS         "ytm"
#define NVS_KEY_BRIGHT "bright"

static int brightness_load(void)
{
    int32_t v = CONFIG_YTM_DISPLAY_BRIGHTNESS;   // first-boot default
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        int32_t stored;
        if (nvs_get_i32(h, NVS_KEY_BRIGHT, &stored) == ESP_OK) v = stored;
        nvs_close(h);
    }
    return (int)v;
}

static void brightness_save(int pct)
{
    static int last_saved = -1;
    if (pct == last_saved) return;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, NVS_KEY_BRIGHT, pct);
        nvs_commit(h);
        nvs_close(h);
        last_saved = pct;
    }
}

static esp_timer_handle_t s_bright_timer;
static volatile int       s_pending_bright;

static void bright_save_cb(void *arg)   // fires 500 ms after the last change
{
    (void)arg;
    brightness_save(s_pending_bright);
    ESP_LOGI(TAG, "brightness persisted: %d%%", s_pending_bright);
}

static void fw_brightness(int percent)
{
    s_active_bright = percent;
    bsp_display_brightness_set(percent);   // apply immediately
    s_pending_bright = percent;
    esp_timer_stop(s_bright_timer);        // debounce flash writes
    esp_timer_start_once(s_bright_timer, 500 * 1000);
}

// Idle-dim brightness callbacks. apply() must NOT persist — dimming is
// transient, so it goes straight to the panel and never touches NVS. Restore
// reads the user's current level so a slider change mid-idle is respected.
static void idle_apply(int percent) { bsp_display_brightness_set(percent); }
static int  idle_get_active(void)   { return s_active_bright; }

void app_main(void)
{
    bsp_display_start();          // panel + touch + LVGL up
    bsp_display_backlight_on();

    // NVS: brightness persistence (safe if net_backend also inits NVS — returns
    // ESP_OK when already initialized).
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    int initial_bright = brightness_load();
    s_active_bright = initial_bright;
    // AMOLED has no PWM backlight; this sends the panel's brightness command.
    // Boot value is NVS (last runtime choice) or CONFIG_YTM_DISPLAY_BRIGHTNESS on
    // first boot. Tunable live via the swipe-down panel.
    bsp_display_brightness_set(initial_bright);

    const esp_timer_create_args_t targs = { .callback = bright_save_cb, .name = "brt" };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_bright_timer));

    battery_start();

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
    quick_panel_init(lv_screen_active(), fw_brightness, initial_bright);
    now_playing_update(&s_vm);
    lv_timer_create(tick_cb, TICK_MS, NULL);
    bsp_display_unlock();

#if CONFIG_YTM_IDLE_DIM_ENABLE
    idle_cfg_t icfg = {
        .dim_after_ms = CONFIG_YTM_IDLE_DIM_MS,
        .dim_percent  = CONFIG_YTM_IDLE_DIM_PERCENT,
        .apply        = idle_apply,
        .get_active   = idle_get_active,
    };
    idle_init(&icfg, (uint32_t)(esp_timer_get_time() / 1000));
#endif

    ESP_LOGI(TAG, "Now Playing up (%s, %d fps).",
             CONFIG_YTM_USE_NET ? "live feed" : "mock data", 1000 / TICK_MS);
}
