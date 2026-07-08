#include "provisioning.h"
#include "improv_serial.h"
#include "wifi_creds.h"

#include "driver/usb_serial_jtag.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include <string.h>

static const char *TAG = "prov";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static EventGroupHandle_t s_wifi_events;

static void wr(const uint8_t *b, size_t n) { usb_serial_jtag_write_bytes(b, n, pdMS_TO_TICKS(100)); }

static void send_state(improv_state_t s) { uint8_t o[16]; wr(o, improv_build_state(s, o, sizeof(o))); }
static void send_error(improv_error_t e) { uint8_t o[16]; wr(o, improv_build_error(e, o, sizeof(o))); }

static void send_device_info(void)
{
    const char *items[] = { "YT Music Companion", "1.0.0", "ESP32-S3", "YT Music Board" };
    uint8_t o[128];
    size_t n = improv_build_rpc_result(IMPROV_CMD_GET_DEVICE_INFO, items, 4, o, sizeof(o));
    if (n) wr(o, n);
}

static void send_wifi_result(void)
{
    // Improv success requires an RPC result for WIFI_SETTINGS; the datum is the
    // redirect-URL list, which we leave empty (no companion web app to open).
    uint8_t o[32];
    size_t n = improv_build_rpc_result(IMPROV_CMD_WIFI_SETTINGS, NULL, 0, o, sizeof(o));
    if (n) wr(o, n);
}

static void wifi_ev(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)        esp_wifi_connect();
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
        xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
}

// Bring Wi-Fi STA up once; used to verify creds before we commit them.
static void wifi_verify_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_ev, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_ev, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}

// Try to connect with the given creds; true on GOT_IP within ~15s.
static bool wifi_try(const char *ssid, const char *pass)
{
    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, pass, sizeof(wc.sta.password) - 1);
    esp_wifi_disconnect();
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    esp_wifi_connect();
    EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(15000));
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

static void show_setup_screen(void)
{
    if (bsp_display_lock((uint32_t)-1)) {   // blocking lock (see LVGL adapter note)
        lv_obj_t *scr = lv_screen_active();
        lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
        lv_obj_t *label = lv_label_create(scr);
        lv_label_set_text(label,
            "Setup\n\nOpen the installer page\nin Chrome or Edge\nto connect Wi-Fi");
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(label);
        bsp_display_unlock();
    }
}

void provisioning_gate(void)
{
    char ssid[64], pass[64];
    if (ytm_creds_load(ssid, sizeof(ssid), pass, sizeof(pass))) return;  // already set up

    ESP_LOGI(TAG, "no Wi-Fi creds; entering Improv provisioning");
    show_setup_screen();

    usb_serial_jtag_driver_config_t ucfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&ucfg));

    s_wifi_events = xEventGroupCreate();
    wifi_verify_init();

    improv_parser_t parser; improv_parser_reset(&parser);
    send_state(IMPROV_STATE_AUTHORIZED);   // no auth step; ready for commands

    uint8_t byte;
    for (;;) {
        if (usb_serial_jtag_read_bytes(&byte, 1, pdMS_TO_TICKS(200)) != 1) continue;
        improv_command_t cmd; improv_error_t perr;
        improv_feed_t r = improv_parser_feed(&parser, byte, &cmd, &perr);
        if (r == IMPROV_FRAME_ERROR) { send_error(IMPROV_ERR_INVALID_RPC); continue; }
        if (r != IMPROV_GOT_COMMAND) continue;

        switch (cmd.command) {
        case IMPROV_CMD_GET_CURRENT_STATE: send_state(IMPROV_STATE_AUTHORIZED); break;
        case IMPROV_CMD_GET_DEVICE_INFO:   send_device_info();                  break;
        case IMPROV_CMD_WIFI_SETTINGS:
            send_state(IMPROV_STATE_PROVISIONING);
            if (wifi_try(cmd.ssid, cmd.password)) {
                ESP_ERROR_CHECK(ytm_creds_store(cmd.ssid, cmd.password));
                send_state(IMPROV_STATE_PROVISIONED);
                send_wifi_result();               // Improv v1: RPC result closes setup
                vTaskDelay(pdMS_TO_TICKS(500));   // let the frame drain
                esp_restart();                     // clean reboot into normal app
            } else {
                send_error(IMPROV_ERR_UNABLE_TO_CONNECT);
                send_state(IMPROV_STATE_AUTHORIZED);
            }
            break;
        default:
            send_error(IMPROV_ERR_UNKNOWN_RPC);
            break;
        }
    }
}
