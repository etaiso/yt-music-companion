// net_backend.c — live feed from the Mac-side bridge (SPEC-ytmusic-adapter.md §6).
//
// WiFi (STA) -> mDNS browse _ytmboard._tcp -> WebSocket -> frames:
//   text  : {"type":"state","data":{...now_playing_vm...}}
//   binary: "YC" header + RGB565 cover (see bridge/src/cover.js)
//
// A mutex guards the snapshot the LVGL tick reads via net_backend_get_vm().
#include "net_backend.h"

#include <string.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "lvgl.h"

static const char *TAG = "ytm-net";

#define COVER_PX 120
#define COVER_BYTES (COVER_PX * COVER_PX * 2) // RGB565
#define COVER_HEADER 8
// Max reassembled frame: cover (header + pixels) with slack for JSON state.
#define RX_MAX (COVER_HEADER + COVER_BYTES + 4096)

// ---- shared snapshot -------------------------------------------------------
static SemaphoreHandle_t s_lock;
static now_playing_vm_t s_vm; // protected by s_lock

// Double-buffered cover so a fresh decode can't tear an in-flight LVGL render.
static uint8_t *s_cover_buf[2]; // PSRAM, COVER_BYTES each
static lv_image_dsc_t s_cover_dsc[2];
static int s_cover_back = 0; // index currently being written

static esp_websocket_client_handle_t s_ws;

// ---- WiFi ------------------------------------------------------------------
static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected, retrying");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "WiFi up");
    }
}

static void wifi_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));

    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, CONFIG_YTM_WIFI_SSID, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, CONFIG_YTM_WIFI_PASSWORD, sizeof(wc.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
}

// ---- discovery -------------------------------------------------------------
// Resolve the bridge to a ws:// URL. Uses the static fallback host if set,
// otherwise mDNS-browses _ytmboard._tcp. Returns true on success.
static bool resolve_bridge_url(char *url, size_t url_len)
{
    if (strlen(CONFIG_YTM_BRIDGE_HOST) > 0) {
        snprintf(url, url_len, "ws://%s:%d/", CONFIG_YTM_BRIDGE_HOST, CONFIG_YTM_BRIDGE_PORT);
        return true;
    }

    ESP_ERROR_CHECK(mdns_init());
    mdns_result_t *results = NULL;
    // Browse until the bridge appears (it advertises _ytmboard._tcp).
    for (int attempt = 0; attempt < 30; attempt++) {
        esp_err_t err = mdns_query_ptr("_ytmboard", "_tcp", 3000, 4, &results);
        if (err == ESP_OK && results) break;
        ESP_LOGI(TAG, "browsing for _ytmboard._tcp...");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (!results) {
        ESP_LOGE(TAG, "bridge not found via mDNS");
        return false;
    }

    int port = results->port ? results->port : CONFIG_YTM_BRIDGE_PORT;
    bool ok = false;
    if (results->addr) {
        char ip[46];
        esp_ip4addr_ntoa(&results->addr->addr.u_addr.ip4, ip, sizeof(ip));
        snprintf(url, url_len, "ws://%s:%d/", ip, port);
        ok = true;
    } else if (results->hostname) {
        snprintf(url, url_len, "ws://%s:%d/", results->hostname, port);
        ok = true;
    }
    mdns_query_results_free(results);
    return ok;
}

// ---- frame parsing ---------------------------------------------------------
static playback_t playback_from(const char *s)
{
    if (s && strcmp(s, "playing") == 0) return PB_PLAYING;
    if (s && strcmp(s, "paused") == 0) return PB_PAUSED;
    return PB_BUFFERING; // "buffering" and anything unknown
}

static void copy_str(char *dst, size_t cap, const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    dst[0] = '\0';
    if (cJSON_IsString(v) && v->valuestring) {
        strncpy(dst, v->valuestring, cap - 1);
        dst[cap - 1] = '\0';
    }
}

static bool get_bool(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : false;
}

static int get_int(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? (int)v->valuedouble : 0;
}

static void parse_state(const char *json, int len)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return;
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!cJSON_IsObject(data)) {
        cJSON_Delete(root);
        return;
    }

    const cJSON *pb = cJSON_GetObjectItemCaseSensitive(data, "playback");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    copy_str(s_vm.source_name, sizeof(s_vm.source_name), data, "source_name");
    copy_str(s_vm.title, sizeof(s_vm.title), data, "title");
    copy_str(s_vm.artist, sizeof(s_vm.artist), data, "artist");
    copy_str(s_vm.album, sizeof(s_vm.album), data, "album");
    s_vm.is_live = get_bool(data, "is_live");
    s_vm.ad_playing = get_bool(data, "ad_playing");
    s_vm.is_favorite = get_bool(data, "is_favorite");
    s_vm.playback = playback_from(cJSON_IsString(pb) ? pb->valuestring : NULL);
    s_vm.position_sec = get_int(data, "position_sec");
    s_vm.duration_sec = get_int(data, "duration_sec");
    s_vm.host_connected = get_bool(data, "host_connected");
    // level: bridge sends 0 (no audio energy); ring falls back to its pulse.
    s_vm.level = 0.0f;
    xSemaphoreGive(s_lock);

    cJSON_Delete(root);
}

static void parse_cover(const uint8_t *buf, int len)
{
    if (len < COVER_HEADER || buf[0] != 'Y' || buf[1] != 'C') return;
    int version = buf[2];
    int format = buf[3];
    int w = buf[4] | (buf[5] << 8);
    int h = buf[6] | (buf[7] << 8);
    if (version != 1 || format != 0 || w != COVER_PX || h != COVER_PX) {
        ESP_LOGW(TAG, "unexpected cover header v%d fmt%d %dx%d", version, format, w, h);
        return;
    }
    if (len < COVER_HEADER + w * h * 2) return;

    int back = s_cover_back;
    memcpy(s_cover_buf[back], buf + COVER_HEADER, w * h * 2);

    lv_image_dsc_t *dsc = &s_cover_dsc[back];
    memset(dsc, 0, sizeof(*dsc));
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565; // RGB565 LE, matches the panel
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.stride = w * 2;
    dsc->data = s_cover_buf[back];
    dsc->data_size = w * h * 2;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_vm.cover_img = dsc;        // publish the freshly filled buffer
    s_cover_back ^= 1;          // next decode writes the other buffer
    xSemaphoreGive(s_lock);
}

// ---- WebSocket -------------------------------------------------------------
static uint8_t *s_rx;   // reassembly buffer (PSRAM)
static int s_rx_len;    // bytes accumulated for the current message
static int s_rx_op;     // op_code of the message in progress (1 text, 2 binary)

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    esp_websocket_event_data_t *e = (esp_websocket_event_data_t *)data;

    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "bridge connected");
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "bridge disconnected");
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_vm.host_connected = false; // show disconnected state (SPEC §6)
        xSemaphoreGive(s_lock);
        break;
    case WEBSOCKET_EVENT_DATA: {
        if (e->op_code == 0x8) break; // close frame
        // Control frames (ping/pong) carry no app payload.
        if (e->op_code == 0x9 || e->op_code == 0xA) break;

        // Reassemble fragments: a new message (op 1/2) resets the buffer;
        // continuation (op 0) appends to it.
        if (e->op_code == 0x1 || e->op_code == 0x2) {
            s_rx_op = e->op_code;
            s_rx_len = 0;
        }
        if (e->payload_offset == 0 && e->op_code != 0x0) s_rx_len = 0;

        if (s_rx_len + e->data_len <= RX_MAX) {
            memcpy(s_rx + s_rx_len, e->data_ptr, e->data_len);
            s_rx_len += e->data_len;
        } else {
            ESP_LOGW(TAG, "frame over RX_MAX, dropping");
            s_rx_len = 0;
            break;
        }

        // Whole message in hand?
        if (e->payload_offset + e->data_len >= e->payload_len) {
            if (s_rx_op == 0x1) {
                parse_state((const char *)s_rx, s_rx_len);
            } else if (s_rx_op == 0x2) {
                parse_cover(s_rx, s_rx_len);
            }
            s_rx_len = 0;
        }
        break;
    }
    default:
        break;
    }
}

static void ws_connect(void)
{
    char url[128];
    while (!resolve_bridge_url(url, sizeof(url))) {
        ESP_LOGE(TAG, "no bridge URL; retrying in 5s");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    ESP_LOGI(TAG, "connecting to %s", url);
    esp_websocket_client_config_t cfg = {
        .uri = url,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 10000,
    };
    s_ws = esp_websocket_client_init(&cfg);
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event, NULL);
    esp_websocket_client_start(s_ws);
}

// ---- bring-up task ---------------------------------------------------------
static void net_task(void *arg)
{
    (void)arg;
    wifi_start();
    // Wait until WiFi has an IP before browsing/connecting.
    esp_netif_ip_info_t ip;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    while (!netif || esp_netif_get_ip_info(netif, &ip) != ESP_OK || ip.ip.addr == 0) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (!netif) netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    }
    ws_connect();
    vTaskDelete(NULL);
}

// ---- public API ------------------------------------------------------------
void net_backend_start(void)
{
    s_lock = xSemaphoreCreateMutex();

    memset(&s_vm, 0, sizeof(s_vm));
    strncpy(s_vm.source_name, "YouTube Music", sizeof(s_vm.source_name) - 1);
    s_vm.playback = PB_BUFFERING;
    s_vm.host_connected = false;

    for (int i = 0; i < 2; i++) {
        s_cover_buf[i] = heap_caps_malloc(COVER_BYTES, MALLOC_CAP_SPIRAM);
    }
    s_rx = heap_caps_malloc(RX_MAX, MALLOC_CAP_SPIRAM);

    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    xTaskCreate(net_task, "ytm-net", 6144, NULL, 5, NULL);
}

void net_backend_get_vm(now_playing_vm_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_vm;
    xSemaphoreGive(s_lock);
}

void net_backend_emit(const char *cmd, int arg)
{
    if (!s_ws || !esp_websocket_client_is_connected(s_ws)) return;
    char msg[96];
    int n;
    // seek carries an absolute-seconds arg; the rest are bare commands.
    if (strcmp(cmd, "seek") == 0) {
        n = snprintf(msg, sizeof(msg), "{\"cmd\":\"seek\",\"arg\":%d}", arg);
    } else {
        n = snprintf(msg, sizeof(msg), "{\"cmd\":\"%s\"}", cmd);
    }
    esp_websocket_client_send_text(s_ws, msg, n, portMAX_DELAY);
}
