#include "wifi_creds.h"
#include <string.h>

bool ytm_creds_valid(const char *ssid)
{
    return ssid && ssid[0] != '\0' && strcmp(ssid, "changeme") != 0;
}

#ifdef ESP_PLATFORM
#include "nvs.h"

#define NVS_NS   "ytm"
#define KEY_SSID "wifi_ssid"
#define KEY_PASS "wifi_pass"

bool ytm_creds_load(char *ssid, size_t ssid_cap, char *pw, size_t pw_cap)
{
    ssid[0] = '\0';
    pw[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t sl = ssid_cap, pl = pw_cap;
    nvs_get_str(h, KEY_SSID, ssid, &sl);
    if (nvs_get_str(h, KEY_PASS, pw, &pl) != ESP_OK) pw[0] = '\0';
    nvs_close(h);
    return ytm_creds_valid(ssid);
}

esp_err_t ytm_creds_store(const char *ssid, const char *pw)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    if ((e = nvs_set_str(h, KEY_SSID, ssid)) == ESP_OK)
        e = nvs_set_str(h, KEY_PASS, pw ? pw : "");
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}
#endif
