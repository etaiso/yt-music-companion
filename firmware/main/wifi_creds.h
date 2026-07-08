// wifi_creds.h — Wi-Fi credentials: pure validity predicate + NVS load/store.
// The predicate is host-tested; the NVS functions compile only on-device
// (guarded by ESP_PLATFORM) so the pure part links into tests without stubs.
#pragma once
#include <stddef.h>
#include <stdbool.h>

#ifdef ESP_PLATFORM
#include "esp_err.h"
#endif

// True iff ssid is non-NULL, non-empty, and not the placeholder "changeme".
bool ytm_creds_valid(const char *ssid);

#ifdef ESP_PLATFORM
// Load creds from NVS. Returns true iff a valid SSID was found (password may be
// empty for open networks). Buffers are always NUL-terminated.
bool ytm_creds_load(char *ssid, size_t ssid_cap, char *pw, size_t pw_cap);

// Persist creds to NVS namespace "ytm" (keys wifi_ssid / wifi_pass).
esp_err_t ytm_creds_store(const char *ssid, const char *pw);
#endif
