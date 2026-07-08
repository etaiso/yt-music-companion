# Web Installer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship one prebuilt firmware image plus a GitHub Pages install page that flashes the board over USB (Web Serial) and provisions Wi-Fi via Improv-serial — no ESP-IDF, no Node, usually no drivers.

**Architecture:** A pure-C Improv-serial codec and a Wi-Fi-credentials helper (both host-tested) sit under a thin, hardware-only provisioning gate that runs at boot when no valid credentials are stored. `net_backend` reads Wi-Fi creds from NVS (Kconfig becomes fallback only). A static install page under the existing `site/install/` uses ESP Web Tools; the existing Pages workflow builds the firmware, merges it into one flashable `.bin`, and deploys the site + binary to GitHub Pages.

**Tech Stack:** ESP-IDF (C), LVGL v9, pure-C host tests (C11 + CTest via existing `tests/`), ESP Web Tools (`esp-web-install-button`), Improv-serial protocol v1, GitHub Actions + GitHub Pages.

## Global Constraints

- **Language:** Firmware and all host-tested modules are **C11**. Do NOT pull in the C++ `improv/improv` component — the Improv codec is implemented in pure C to match this repo's host-test harness (`tests/` compiles standalone C modules).
- **Theme:** Dark only, non-configurable. Do NOT touch `ui/styles.h` or add any theme provisioning.
- **Config scope:** The installer provisions **Wi-Fi SSID + password only**. No brightness/bridge/theme in the form.
- **Improv protocol v1 framing (exact):** header bytes `I M P R O V` (0x49 0x4D 0x50 0x52 0x4F 0x56), then `version=0x01`, `type` (1 byte), `length` (1 byte), `payload` (`length` bytes), `checksum` (1 byte = sum of every preceding byte in the packet, mod 256). Packet types: `0x01` current-state, `0x02` error-state, `0x03` RPC command (device receives), `0x04` RPC result (device sends).
- **Improv enum values (exact):** State: STOPPED=0x00, AWAITING_AUTHORIZATION=0x01, AUTHORIZED=0x02, PROVISIONING=0x03, PROVISIONED=0x04. Error: NONE=0x00, INVALID_RPC=0x01, UNKNOWN_RPC=0x02, UNABLE_TO_CONNECT=0x03, NOT_AUTHORIZED=0x04, UNKNOWN=0xFF. Command: UNKNOWN=0x00, WIFI_SETTINGS=0x01, GET_CURRENT_STATE=0x02, GET_DEVICE_INFO=0x03, GET_WIFI_NETWORKS=0x04.
- **NVS:** Reuse the existing namespace `"ytm"` (see `firmware/main/main.c:73`). New keys: `wifi_ssid`, `wifi_pass`.
- **Commits:** Frequent, one per task minimum. End commit messages with the repo's `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>` trailer.
- **chipFamily for the manifest:** `"ESP32-S3"`.

## File Structure

| File | Responsibility | New/Modified |
|------|----------------|--------------|
| `firmware/main/improv_serial.h/.c` | Pure-C Improv-serial codec: stateful byte parser + frame builders. No ESP/LVGL deps. | Create |
| `firmware/main/wifi_creds.h/.c` | Wi-Fi credential validity predicate (pure) + NVS load/store (guarded by `ESP_PLATFORM`). | Create |
| `firmware/main/provisioning.h/.c` | Hardware-only gate: USB-serial loop feeding the codec, Wi-Fi verify, setup screen, reboot. Thin — delegates all logic to the two modules above. | Create |
| `firmware/main/net_backend.c` | `wifi_start()` loads creds from NVS instead of Kconfig. | Modify (`:78-82`) |
| `firmware/main/main.c` | Call the provisioning gate before `net_backend_start()`. | Modify (`:153-155`) |
| `firmware/main/CMakeLists.txt` | Add new SRCS + `esp_driver_usb_serial_jtag` REQUIRES. | Modify |
| `tests/test_improv.c` | Host unit test for the Improv codec. | Create |
| `tests/test_wifi_creds.c` | Host unit test for the credential predicate. | Create |
| `tests/CMakeLists.txt` | Register the two new test executables. | Modify |
| `site/install/index.html` | Install page (ESP Web Tools button + copy). | Create |
| `site/install/manifest.json` | ESP Web Tools manifest referencing the merged bin. | Create |
| `site/install/install-button.js` | Vendored ESP Web Tools bundle (pinned, self-hosted). | Create |
| `site/.nojekyll` | Disable Jekyll on the Pages site. | Create |
| `site/install/README.md` | How to build/deploy the installer locally. | Create |
| `.github/workflows/pages.yml` | Extend the **existing** landing-page Pages workflow to also build the firmware and bundle it into `site/install/`. | Modify |
| `docs/CONFIGURATION.md`, `README.md` | Document the web-installer path; note Wi-Fi is now runtime. | Modify |

---

## Task 1: Improv-serial codec (pure C)

**Files:**
- Create: `firmware/main/improv_serial.h`, `firmware/main/improv_serial.c`
- Test: `tests/test_improv.c`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing.
- Produces (relied on by Task 4):
  - `improv_cmd_t`, `improv_state_t`, `improv_error_t` enums (values per Global Constraints).
  - `#define IMPROV_STR_MAX 64`
  - `typedef struct { improv_cmd_t command; char ssid[IMPROV_STR_MAX]; char password[IMPROV_STR_MAX]; } improv_command_t;`
  - `typedef struct { uint8_t buf[300]; uint16_t len; } improv_parser_t;`
  - `typedef enum { IMPROV_NEED_MORE, IMPROV_GOT_COMMAND, IMPROV_FRAME_ERROR } improv_feed_t;`
  - `void improv_parser_reset(improv_parser_t *p);`
  - `improv_feed_t improv_parser_feed(improv_parser_t *p, uint8_t b, improv_command_t *out, improv_error_t *err);`
  - `size_t improv_build_state(improv_state_t s, uint8_t *out, size_t cap);`
  - `size_t improv_build_error(improv_error_t e, uint8_t *out, size_t cap);`
  - `size_t improv_build_rpc_result(improv_cmd_t cmd, const char *const *items, uint8_t n, uint8_t *out, size_t cap);`

- [ ] **Step 1: Write the header**

Create `firmware/main/improv_serial.h`:

```c
// improv_serial.h — pure-C Improv-Wifi serial codec (protocol v1).
// No ESP/LVGL deps so it host-tests standalone (see tests/test_improv.c).
// Framing: "IMPROV" | version(1) | type | length | payload | checksum(sum&0xFF).
#pragma once
#include <stdint.h>
#include <stddef.h>

#define IMPROV_STR_MAX 64

typedef enum {
    IMPROV_CMD_UNKNOWN           = 0x00,
    IMPROV_CMD_WIFI_SETTINGS     = 0x01,
    IMPROV_CMD_GET_CURRENT_STATE = 0x02,
    IMPROV_CMD_GET_DEVICE_INFO   = 0x03,
    IMPROV_CMD_GET_WIFI_NETWORKS = 0x04,
} improv_cmd_t;

typedef enum {
    IMPROV_STATE_STOPPED                = 0x00,
    IMPROV_STATE_AWAITING_AUTHORIZATION = 0x01,
    IMPROV_STATE_AUTHORIZED             = 0x02,
    IMPROV_STATE_PROVISIONING           = 0x03,
    IMPROV_STATE_PROVISIONED            = 0x04,
} improv_state_t;

typedef enum {
    IMPROV_ERR_NONE              = 0x00,
    IMPROV_ERR_INVALID_RPC       = 0x01,
    IMPROV_ERR_UNKNOWN_RPC       = 0x02,
    IMPROV_ERR_UNABLE_TO_CONNECT = 0x03,
    IMPROV_ERR_NOT_AUTHORIZED    = 0x04,
    IMPROV_ERR_UNKNOWN           = 0xFF,
} improv_error_t;

typedef struct {
    improv_cmd_t command;
    char ssid[IMPROV_STR_MAX];
    char password[IMPROV_STR_MAX];
} improv_command_t;

typedef struct {
    uint8_t  buf[300];
    uint16_t len;
} improv_parser_t;

typedef enum { IMPROV_NEED_MORE, IMPROV_GOT_COMMAND, IMPROV_FRAME_ERROR } improv_feed_t;

void         improv_parser_reset(improv_parser_t *p);
improv_feed_t improv_parser_feed(improv_parser_t *p, uint8_t b,
                                 improv_command_t *out, improv_error_t *err);

size_t improv_build_state(improv_state_t s, uint8_t *out, size_t cap);
size_t improv_build_error(improv_error_t e, uint8_t *out, size_t cap);
size_t improv_build_rpc_result(improv_cmd_t cmd, const char *const *items,
                               uint8_t n, uint8_t *out, size_t cap);
```

- [ ] **Step 2: Write the failing test**

Create `tests/test_improv.c`:

```c
// test_improv.c — host unit test for the pure Improv-serial codec.
//   cc -std=c11 -I../firmware/main test_improv.c ../firmware/main/improv_serial.c -o test_improv
#include "improv_serial.h"
#include <stdio.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...)                                                     \
    do { if (cond) { g_pass++; }                                            \
         else { g_fail++; printf("  FAIL (%s:%d): ", __FILE__, __LINE__);   \
                printf(__VA_ARGS__); printf("\n"); } } while (0)

// Feed a whole buffer byte-by-byte; return the last non-NEED_MORE result.
static improv_feed_t feed_all(const uint8_t *b, size_t n,
                              improv_command_t *out, improv_error_t *err)
{
    improv_parser_t p;
    improv_parser_reset(&p);
    improv_feed_t r = IMPROV_NEED_MORE;
    for (size_t i = 0; i < n; i++) {
        r = improv_parser_feed(&p, b[i], out, err);
        if (r != IMPROV_NEED_MORE) break;
    }
    return r;
}

// Build an incoming RPC frame (type 0x03) with the given payload; append checksum.
static size_t make_frame(uint8_t *dst, const uint8_t *payload, uint8_t plen)
{
    const uint8_t hdr[] = { 'I','M','P','R','O','V', 0x01, 0x03, plen };
    size_t n = 0;
    memcpy(dst, hdr, sizeof(hdr)); n = sizeof(hdr);
    memcpy(dst + n, payload, plen); n += plen;
    uint32_t sum = 0;
    for (size_t i = 0; i < n; i++) sum += dst[i];
    dst[n++] = (uint8_t)(sum & 0xFF);
    return n;
}

int main(void)
{
    improv_command_t cmd; improv_error_t err;

    printf("# parses WIFI_SETTINGS with ssid + password\n");
    {
        // RPC payload: cmd, data_len, [ssid_len, ssid..., pw_len, pw...]
        const char *ssid = "MyNet"; const char *pw = "secret42";
        uint8_t payload[64]; uint8_t k = 0;
        payload[k++] = IMPROV_CMD_WIFI_SETTINGS;
        uint8_t dl_pos = k++;            // fill data_len after
        uint8_t d0 = k;
        payload[k++] = (uint8_t)strlen(ssid); memcpy(payload+k, ssid, strlen(ssid)); k += strlen(ssid);
        payload[k++] = (uint8_t)strlen(pw);   memcpy(payload+k, pw,   strlen(pw));   k += strlen(pw);
        payload[dl_pos] = (uint8_t)(k - d0);
        uint8_t frame[80]; size_t fn = make_frame(frame, payload, k);
        improv_feed_t r = feed_all(frame, fn, &cmd, &err);
        CHECK(r == IMPROV_GOT_COMMAND, "expected GOT_COMMAND (got %d)", r);
        CHECK(cmd.command == IMPROV_CMD_WIFI_SETTINGS, "command WIFI_SETTINGS");
        CHECK(strcmp(cmd.ssid, "MyNet") == 0, "ssid (got '%s')", cmd.ssid);
        CHECK(strcmp(cmd.password, "secret42") == 0, "password (got '%s')", cmd.password);
    }

    printf("# parses GET_CURRENT_STATE (no data)\n");
    {
        uint8_t payload[2] = { IMPROV_CMD_GET_CURRENT_STATE, 0x00 };
        uint8_t frame[16]; size_t fn = make_frame(frame, payload, 2);
        improv_feed_t r = feed_all(frame, fn, &cmd, &err);
        CHECK(r == IMPROV_GOT_COMMAND && cmd.command == IMPROV_CMD_GET_CURRENT_STATE,
              "GET_CURRENT_STATE (got r=%d cmd=%d)", r, cmd.command);
    }

    printf("# bad checksum -> FRAME_ERROR\n");
    {
        uint8_t payload[2] = { IMPROV_CMD_GET_CURRENT_STATE, 0x00 };
        uint8_t frame[16]; size_t fn = make_frame(frame, payload, 2);
        frame[fn - 1] ^= 0xFF;   // corrupt checksum
        improv_feed_t r = feed_all(frame, fn, &cmd, &err);
        CHECK(r == IMPROV_FRAME_ERROR, "expected FRAME_ERROR (got %d)", r);
    }

    printf("# resync: leading log noise before a valid frame still parses\n");
    {
        uint8_t payload[2] = { IMPROV_CMD_GET_CURRENT_STATE, 0x00 };
        uint8_t frame[16]; size_t fn = make_frame(frame, payload, 2);
        uint8_t noisy[64]; size_t nn = 0;
        const char *log = "I (1234) ytm: hello\nIMP";  // partial header + junk
        memcpy(noisy, log, strlen(log)); nn = strlen(log);
        memcpy(noisy + nn, frame, fn); nn += fn;
        improv_feed_t r = feed_all(noisy, nn, &cmd, &err);
        CHECK(r == IMPROV_GOT_COMMAND && cmd.command == IMPROV_CMD_GET_CURRENT_STATE,
              "resynced parse (got r=%d)", r);
    }

    printf("# build_state emits a well-formed current-state packet\n");
    {
        uint8_t out[16];
        size_t n = improv_build_state(IMPROV_STATE_PROVISIONED, out, sizeof(out));
        CHECK(n == 10, "state packet length 10 (got %zu)", n);
        CHECK(memcmp(out, "IMPROV", 6) == 0, "header");
        CHECK(out[6] == 0x01 && out[7] == 0x01, "version + type=current-state");
        CHECK(out[8] == 0x01 && out[9] == IMPROV_STATE_PROVISIONED, "len + payload");
        uint32_t sum = 0; for (int i = 0; i < 9; i++) sum += out[i];
        CHECK(out[9] /*payload*/ == IMPROV_STATE_PROVISIONED, "payload state");
        // checksum is byte 10? length is 1 -> total 10 -> checksum at index 9? recompute:
        // header(6)+ver(1)+type(1)+len(1)=9, payload(1)=index9, checksum=index10 => total 11
    }

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
```

> Note: the build_state length assertion above is intentionally left for Step 5 to
> reconcile against the real implementation (a state packet is
> 6+1+1+1+1+1 = **11** bytes). Fix the expected value to `11` and the checksum index
> to `10` when you implement, so the test reflects the true framing.

- [ ] **Step 3: Register the test and run it (expect failure)**

Add to `tests/CMakeLists.txt` (after the `idle` block):

```cmake
# Improv-serial codec: pure C (no ESP/LVGL), compiled standalone.
add_executable(test_improv
    test_improv.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/main/improv_serial.c
)
target_include_directories(test_improv PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/main)
if(NOT MSVC)
    target_compile_options(test_improv PRIVATE -Wall -Wextra)
endif()
add_test(NAME improv COMMAND test_improv)
```

Run: `cmake -B build tests && cmake --build build --target test_improv`
Expected: FAIL — link error, `improv_parser_reset` etc. undefined (`improv_serial.c` is empty/absent).

- [ ] **Step 4: Implement the codec**

Create `firmware/main/improv_serial.c`:

```c
#include "improv_serial.h"
#include <string.h>

static const uint8_t HDR[6] = { 'I','M','P','R','O','V' };
#define VERSION 0x01
#define TYPE_CURRENT_STATE 0x01
#define TYPE_ERROR_STATE   0x02
#define TYPE_RPC           0x03
#define TYPE_RPC_RESULT    0x04

void improv_parser_reset(improv_parser_t *p) { p->len = 0; }

// Copy a length-prefixed string at data[*off] into dst (bounded). Advance *off.
static int take_str(const uint8_t *data, uint8_t dlen, uint8_t *off, char *dst)
{
    if (*off >= dlen) return -1;
    uint8_t slen = data[(*off)++];
    if (*off + slen > dlen || slen >= IMPROV_STR_MAX) return -1;
    memcpy(dst, data + *off, slen);
    dst[slen] = '\0';
    *off += slen;
    return 0;
}

improv_feed_t improv_parser_feed(improv_parser_t *p, uint8_t b,
                                 improv_command_t *out, improv_error_t *err)
{
    if (p->len < sizeof(p->buf)) p->buf[p->len++] = b;

    // Resync: keep only a suffix that could still be a valid header prefix.
    while (p->len > 0) {
        uint16_t hchk = p->len < 6 ? p->len : 6;
        if (memcmp(p->buf, HDR, hchk) == 0) break;   // header prefix OK
        memmove(p->buf, p->buf + 1, --p->len);        // drop first byte, retry
    }
    if (p->len < 9) return IMPROV_NEED_MORE;          // need hdr+ver+type+len

    uint8_t type = p->buf[7];
    uint8_t plen = p->buf[8];
    uint16_t total = (uint16_t)(9 + plen + 1);        // + checksum
    if (p->len < total) return IMPROV_NEED_MORE;

    uint32_t sum = 0;
    for (uint16_t i = 0; i < (uint16_t)(total - 1); i++) sum += p->buf[i];
    uint8_t want = (uint8_t)(sum & 0xFF);
    uint8_t got  = p->buf[total - 1];
    improv_feed_t result;
    if (p->buf[6] != VERSION || want != got) {
        if (err) *err = IMPROV_ERR_INVALID_RPC;
        result = IMPROV_FRAME_ERROR;
    } else if (type == TYPE_RPC) {
        const uint8_t *payload = p->buf + 9;
        memset(out, 0, sizeof(*out));
        out->command = (improv_cmd_t)payload[0];
        uint8_t dlen = payload[1];
        const uint8_t *data = payload + 2;
        if (out->command == IMPROV_CMD_WIFI_SETTINGS) {
            uint8_t off = 0;
            if (take_str(data, dlen, &off, out->ssid) != 0 ||
                take_str(data, dlen, &off, out->password) != 0) {
                if (err) *err = IMPROV_ERR_INVALID_RPC;
                result = IMPROV_FRAME_ERROR;
                improv_parser_reset(p);
                return result;
            }
        }
        result = IMPROV_GOT_COMMAND;
    } else {
        result = IMPROV_NEED_MORE;   // ignore non-RPC inbound types
    }
    improv_parser_reset(p);
    return result;
}

static size_t finish(uint8_t *out, size_t n)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < n; i++) sum += out[i];
    out[n++] = (uint8_t)(sum & 0xFF);
    return n;
}

size_t improv_build_state(improv_state_t s, uint8_t *out, size_t cap)
{
    if (cap < 11) return 0;
    memcpy(out, HDR, 6);
    out[6] = VERSION; out[7] = TYPE_CURRENT_STATE; out[8] = 0x01; out[9] = (uint8_t)s;
    return finish(out, 10);
}

size_t improv_build_error(improv_error_t e, uint8_t *out, size_t cap)
{
    if (cap < 11) return 0;
    memcpy(out, HDR, 6);
    out[6] = VERSION; out[7] = TYPE_ERROR_STATE; out[8] = 0x01; out[9] = (uint8_t)e;
    return finish(out, 10);
}

size_t improv_build_rpc_result(improv_cmd_t cmd, const char *const *items,
                               uint8_t n, uint8_t *out, size_t cap)
{
    // payload = cmd, data_len, [len,bytes]*n
    size_t data_len = 0;
    for (uint8_t i = 0; i < n; i++) data_len += 1 + strlen(items[i]);
    size_t total = 9 + 2 + data_len + 1;   // hdr..len + cmd + data_len + data + checksum
    if (cap < total || data_len > 255) return 0;
    memcpy(out, HDR, 6);
    out[6] = VERSION; out[7] = TYPE_RPC_RESULT;
    out[8] = (uint8_t)(2 + data_len);
    out[9] = (uint8_t)cmd;
    out[10] = (uint8_t)data_len;
    size_t k = 11;
    for (uint8_t i = 0; i < n; i++) {
        uint8_t sl = (uint8_t)strlen(items[i]);
        out[k++] = sl;
        memcpy(out + k, items[i], sl); k += sl;
    }
    return finish(out, k);
}
```

- [ ] **Step 5: Reconcile the test's build_state assertion**

In `tests/test_improv.c`, fix the build_state block to the true framing (11-byte packet):

```c
    printf("# build_state emits a well-formed current-state packet\n");
    {
        uint8_t out[16];
        size_t n = improv_build_state(IMPROV_STATE_PROVISIONED, out, sizeof(out));
        CHECK(n == 11, "state packet length 11 (got %zu)", n);
        CHECK(memcmp(out, "IMPROV", 6) == 0, "header");
        CHECK(out[6] == 0x01 && out[7] == 0x01, "version + type=current-state");
        CHECK(out[8] == 0x01 && out[9] == IMPROV_STATE_PROVISIONED, "len + payload");
        uint32_t sum = 0; for (int i = 0; i < 10; i++) sum += out[i];
        CHECK(out[10] == (uint8_t)(sum & 0xFF), "checksum");
    }
```

- [ ] **Step 6: Run the tests (expect pass)**

Run: `cmake --build build --target test_improv && ctest --test-dir build -R improv --output-on-failure`
Expected: PASS — `N passed, 0 failed`.

- [ ] **Step 7: Commit**

```bash
git add firmware/main/improv_serial.h firmware/main/improv_serial.c tests/test_improv.c tests/CMakeLists.txt
git commit -m "feat(firmware): pure-C Improv-serial codec + host tests

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 2: Wi-Fi credentials module

**Files:**
- Create: `firmware/main/wifi_creds.h`, `firmware/main/wifi_creds.c`
- Test: `tests/test_wifi_creds.c`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing.
- Produces (relied on by Tasks 3 and 4):
  - `bool ytm_creds_valid(const char *ssid);` — pure: true iff non-NULL, non-empty, and not the literal `"changeme"`.
  - `bool ytm_creds_load(char *ssid, size_t ssid_cap, char *pw, size_t pw_cap);` — reads NVS `"ytm"`/`wifi_ssid`,`wifi_pass`; returns true iff a **valid** SSID was loaded. ESP-only.
  - `esp_err_t ytm_creds_store(const char *ssid, const char *pw);` — writes NVS. ESP-only.

- [ ] **Step 1: Write the header**

Create `firmware/main/wifi_creds.h`:

```c
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
```

- [ ] **Step 2: Write the failing test**

Create `tests/test_wifi_creds.c`:

```c
// test_wifi_creds.c — host unit test for the pure credential predicate.
//   cc -std=c11 -I../firmware/main test_wifi_creds.c ../firmware/main/wifi_creds.c -o test_wifi_creds
#include "wifi_creds.h"
#include <stdio.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...)                                                     \
    do { if (cond) { g_pass++; }                                            \
         else { g_fail++; printf("  FAIL (%s:%d): ", __FILE__, __LINE__);   \
                printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    CHECK(ytm_creds_valid("Koko2.4"),  "a real SSID is valid");
    CHECK(!ytm_creds_valid(""),        "empty SSID is invalid");
    CHECK(!ytm_creds_valid("changeme"),"placeholder is invalid");
    CHECK(!ytm_creds_valid(0),         "NULL is invalid");
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
```

- [ ] **Step 3: Register + run (expect failure)**

Add to `tests/CMakeLists.txt`:

```cmake
# Wi-Fi credential predicate: pure C, compiled standalone.
add_executable(test_wifi_creds
    test_wifi_creds.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/main/wifi_creds.c
)
target_include_directories(test_wifi_creds PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/main)
if(NOT MSVC)
    target_compile_options(test_wifi_creds PRIVATE -Wall -Wextra)
endif()
add_test(NAME wifi_creds COMMAND test_wifi_creds)
```

Run: `cmake -B build tests && cmake --build build --target test_wifi_creds`
Expected: FAIL — `ytm_creds_valid` undefined.

- [ ] **Step 4: Implement**

Create `firmware/main/wifi_creds.c`:

```c
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
```

- [ ] **Step 5: Run (expect pass)**

Run: `cmake --build build --target test_wifi_creds && ctest --test-dir build -R wifi_creds --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add firmware/main/wifi_creds.h firmware/main/wifi_creds.c tests/test_wifi_creds.c tests/CMakeLists.txt
git commit -m "feat(firmware): Wi-Fi credentials module (NVS) + predicate test

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 3: net_backend reads creds from NVS

**Files:**
- Modify: `firmware/main/net_backend.c` (`:78-82`, and includes near `:23`)
- Modify: `firmware/main/CMakeLists.txt` (add `wifi_creds.c` to SRCS)

**Interfaces:**
- Consumes: `ytm_creds_load()` from Task 2.
- Produces: no new symbols; `wifi_start()` now sources creds from NVS with the Kconfig values as fallback.

- [ ] **Step 1: Add wifi_creds.c to the firmware build**

In `firmware/main/CMakeLists.txt`, add to `SRCS` (after `"net_backend.c"`):

```cmake
         "wifi_creds.c"
         "improv_serial.c"
         "provisioning.c"
```

(`improv_serial.c`/`provisioning.c` are added now so later tasks compile; `provisioning.c` is created in Task 4 — this step will not build cleanly until then, so DO NOT build the firmware here. Host tests are unaffected.)

- [ ] **Step 2: Include the header in net_backend.c**

Near the existing includes (around `firmware/main/net_backend.c:23`), add:

```c
#include "wifi_creds.h"
```

- [ ] **Step 3: Load creds from NVS in wifi_start()**

Replace `firmware/main/net_backend.c:78-82`:

```c
    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, CONFIG_YTM_WIFI_SSID, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, CONFIG_YTM_WIFI_PASSWORD, sizeof(wc.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
```

with:

```c
    // Credentials come from NVS (set by the web installer via provisioning).
    // Fall back to the Kconfig defaults so a developer menuconfig build still works.
    char ssid[64], pass[64];
    if (!ytm_creds_load(ssid, sizeof(ssid), pass, sizeof(pass))) {
        strncpy(ssid, CONFIG_YTM_WIFI_SSID, sizeof(ssid) - 1); ssid[sizeof(ssid)-1] = '\0';
        strncpy(pass, CONFIG_YTM_WIFI_PASSWORD, sizeof(pass) - 1); pass[sizeof(pass)-1] = '\0';
    }
    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, pass, sizeof(wc.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
```

- [ ] **Step 4: Verify host tests still green (no firmware build yet)**

Run: `ctest --test-dir build --output-on-failure`
Expected: all host tests PASS (this change doesn't affect them; firmware build is deferred to Task 4).

- [ ] **Step 5: Commit**

```bash
git add firmware/main/net_backend.c firmware/main/CMakeLists.txt
git commit -m "feat(firmware): load Wi-Fi creds from NVS (Kconfig as fallback)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 4: Provisioning gate (hardware integration)

**Files:**
- Create: `firmware/main/provisioning.h`, `firmware/main/provisioning.c`
- Modify: `firmware/main/main.c` (`:153-155`)
- Modify: `firmware/main/CMakeLists.txt` (add `esp_driver_usb_serial_jtag` to REQUIRES)

**Interfaces:**
- Consumes: `improv_*` (Task 1), `ytm_creds_valid` / `ytm_creds_store` (Task 2).
- Produces: `void provisioning_gate(void);` — if valid creds already exist, returns immediately; otherwise shows a setup screen, runs the Improv-serial responder until Wi-Fi is provisioned and verified, stores creds, and reboots.

> **This is the one hardware-only, manually-verified task.** Its logic is deliberately
> thin: framing/parsing is Task 1 (host-tested), credential rules are Task 2
> (host-tested). Keep it that way — do not re-implement protocol logic here.

- [ ] **Step 1: Write the header**

Create `firmware/main/provisioning.h`:

```c
// provisioning.h — first-boot Wi-Fi provisioning via Improv-serial (USB).
// Runs only when NVS holds no valid credentials. Blocks (showing a setup
// screen) until the web installer provisions Wi-Fi, then reboots into the
// normal app. No-op when creds already exist.
#pragma once
void provisioning_gate(void);
```

- [ ] **Step 2: Implement provisioning.c**

Create `firmware/main/provisioning.c`:

```c
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
    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    esp_wifi_disconnect();
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
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
```

- [ ] **Step 3: Add the driver requirement**

In `firmware/main/CMakeLists.txt`, add to `REQUIRES`:

```cmake
             esp_driver_usb_serial_jtag
```

- [ ] **Step 4: Call the gate from app_main**

In `firmware/main/main.c`, add the include near the other net include (`:13-17` block):

```c
#if CONFIG_YTM_USE_NET
#include "net_backend.h"
#include "provisioning.h"
#else
#include "mock.h"
#endif
```

Then change `firmware/main/main.c:153-155` from:

```c
#if CONFIG_YTM_USE_NET
    net_backend_start();          // WiFi + mDNS + WebSocket (async)
#endif
```

to:

```c
#if CONFIG_YTM_USE_NET
    provisioning_gate();          // first boot only: Improv Wi-Fi setup, else no-op
    net_backend_start();          // WiFi + mDNS + WebSocket (async)
#endif
```

- [ ] **Step 5: Build the firmware**

Activate ESP-IDF (per `docs/RUNNING.md`), then:

Run: `cd firmware && idf.py build`
Expected: build succeeds, no errors. If `usb_serial_jtag.h` isn't found, confirm the `esp_driver_usb_serial_jtag` REQUIRES line and the installed IDF version (v5.5+ per README).

- [ ] **Step 6: Manual hardware verification (the real gate)**

1. Erase + flash: `idf.py -p <port> erase-flash flash monitor`.
2. On boot with no creds, the panel shows the **Setup** screen and the monitor logs `entering Improv provisioning`.
3. Serve the installer locally (Task 5 done) or use the ESP Web Tools test page; from Chrome/Edge, click **Install** is not needed here — use the **"Connect"/Improv** path or the [Improv Wi-Fi serial demo](https://improv-wifi.com/serial/) to send SSID/password.
4. Enter valid Wi-Fi → device reports PROVISIONED, reboots, and comes up on the normal Now Playing screen connected to the bridge.
5. Enter wrong Wi-Fi → device reports "unable to connect" and stays in setup.

Record the observed result in the commit message.

- [ ] **Step 7: Commit**

```bash
git add firmware/main/provisioning.h firmware/main/provisioning.c firmware/main/main.c firmware/main/CMakeLists.txt
git commit -m "feat(firmware): first-boot Improv-serial Wi-Fi provisioning gate

Verified on hardware: <fill in observed result from Step 6>.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 5: Web installer site (under the existing `site/`)

> **Context:** `origin/main` already publishes a landing page from `site/index.html`
> to GitHub Pages via `.github/workflows/pages.yml`. GitHub Pages allows only **one
> deployment per repo**, so the installer lives **inside** that site at
> `site/install/` and is served by the same (single) Pages deployment. Do NOT add a
> second Pages workflow.

**Files:**
- Create: `site/install/index.html`, `site/install/manifest.json`, `site/install/install-button.js`, `site/.nojekyll`, `site/install/README.md`

**Interfaces:**
- Consumes: the merged firmware binary `ytm-firmware.bin` (produced by Task 6 CI into `site/install/`; a locally-built one can be dropped in for testing).
- Produces: static files under `site/install/`; `manifest.json` references `ytm-firmware.bin` at offset 0 with `improv: true`.

- [ ] **Step 1: Vendor the ESP Web Tools bundle**

Download the pinned bundle so the site is self-contained (no runtime CDN dependency):

Run:
```bash
mkdir -p site/install
curl -L "https://unpkg.com/esp-web-tools@10/dist/web/install-button.js?module" -o site/install/install-button.js
```
Expected: `site/install/install-button.js` exists and is non-empty (a few hundred KB).

- [ ] **Step 2: Write the manifest**

Create `site/install/manifest.json`:

```json
{
  "name": "YT Music Companion",
  "version": "1.0.0",
  "new_install_prompt_erase": true,
  "new_install_improv_wait_time": 10,
  "builds": [
    {
      "chipFamily": "ESP32-S3",
      "improv": true,
      "parts": [
        { "path": "ytm-firmware.bin", "offset": 0 }
      ]
    }
  ]
}
```

- [ ] **Step 3: Write the install page**

Create `site/install/index.html`:

```html
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>YT Music Companion — Installer</title>
  <script type="module" src="./install-button.js"></script>
  <style>
    :root { color-scheme: dark; }
    body { margin: 0; font-family: system-ui, sans-serif; background: #0b0b0d;
           color: #f2f2f4; display: grid; place-items: center; min-height: 100vh; }
    main { max-width: 30rem; padding: 2rem; text-align: center; }
    h1 { font-size: 1.4rem; margin: 0 0 .5rem; }
    p  { color: #b9b9c0; line-height: 1.5; }
    ol { text-align: left; color: #b9b9c0; line-height: 1.7; }
    .note { font-size: .85rem; color: #8a8a92; margin-top: 1.5rem; }
    esp-web-install-button { display: inline-block; margin: 1.5rem 0; }
  </style>
</head>
<body>
  <main>
    <h1>YT Music Companion</h1>
    <p>Plug the board into this computer with a USB cable, then click Install.</p>

    <esp-web-install-button manifest="./manifest.json">
      <button slot="activate">Install</button>
      <span slot="unsupported">Your browser can't flash the board — use
        <b>Chrome</b> or <b>Edge</b> on a desktop computer.</span>
      <span slot="not-allowed">Serial access was blocked. Reload and allow it.</span>
    </esp-web-install-button>

    <ol>
      <li>Click <b>Install</b> and pick the board's serial port.</li>
      <li>Wait for flashing to finish (~1&nbsp;minute).</li>
      <li>Enter your <b>2.4&nbsp;GHz Wi-Fi</b> name and password when asked.</li>
      <li>The board reboots and connects. Play a song on ytmdesktop.</li>
    </ol>

    <p class="note">Requires Chrome or Edge on desktop (Web Serial).
      Safari, Firefox, and iOS aren't supported.</p>
  </main>
</body>
</html>
```

- [ ] **Step 4: Disable Jekyll + document**

Create `site/.nojekyll` (empty file) — covers the whole Pages site:

Run:
```bash
touch site/.nojekyll
```

Create `site/install/README.md`:

```markdown
# Web installer

Lives under the site's `/install/` path and is published by the repo's single
GitHub Pages deployment (`.github/workflows/pages.yml`). Flashes the board and
provisions Wi-Fi via ESP Web Tools + Improv-serial. No toolchain needed by the
end user (Chrome/Edge desktop only — Web Serial).

## Local test

Web Serial needs a secure context; `http://localhost` counts. From `site/`:

    python -m http.server 8000

Open http://localhost:8000/install/ in Chrome/Edge. Drop a locally-built
`ytm-firmware.bin` (see below) next to `manifest.json` to test flashing.

## Building the firmware binary

CI does this in the Pages workflow. Manually:

    cd firmware && idf.py build
    idf.py merge-bin -o ../site/install/ytm-firmware.bin

`ytm-firmware.bin` is a single image flashed at offset 0 (bootloader +
partition table + app). It is git-ignored; CI produces it on deploy.
```

- [ ] **Step 5: Ignore the built binary**

Add to the repo root `.gitignore`:

```
site/install/ytm-firmware.bin
```

- [ ] **Step 6: Manual verification**

1. Build a local binary: `cd firmware && idf.py build && idf.py merge-bin -o ../site/install/ytm-firmware.bin`.
2. From the repo root: `cd site && python -m http.server 8000`.
3. Open http://localhost:8000/install/ in Chrome — confirm the page renders, the **Install** button appears (not the "unsupported" text), and (optionally, with a board attached) the full flash + Improv Wi-Fi flow completes.

- [ ] **Step 7: Commit**

```bash
git add site/install/ site/.nojekyll .gitignore
git commit -m "feat(installer): ESP Web Tools install page under site/install

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 6: Extend the Pages workflow to build + bundle the firmware

**Files:**
- Modify: `.github/workflows/pages.yml`

**Interfaces:**
- Consumes: `firmware/` (ESP-IDF project), `site/` (static site incl. `site/install/`).
- Produces: the repo's single GitHub Pages deployment now also contains
  `install/ytm-firmware.bin`, built fresh in the workflow.

> **Existing workflow (do not duplicate):** `pages.yml` already deploys `site/` to
> Pages on push to `main` (paths-filtered to `site/**`). We add a firmware build
> step that writes `ytm-firmware.bin` into `site/install/` before the existing
> upload, and widen the trigger to include firmware changes. Keep the single
> `concurrency: pages` group and the one `deploy-pages` step.
>
> **Prerequisite (already documented in the file's header comment):** Pages must be
> enabled with Source = GitHub Actions, and — per project memory and that comment —
> this needs the repo **public or GitHub Pro**. If deploy 403s, that's the cause;
> the build + artifact-upload steps still succeed.

- [ ] **Step 1: Rewrite pages.yml**

Replace the full contents of `.github/workflows/pages.yml` with:

```yaml
name: Deploy site (landing + installer)

# Publishes site/ to GitHub Pages, building the firmware into site/install/ so the
# web installer can flash it. Requires Pages enabled with "Source: GitHub Actions"
# (and the repo public, or GitHub Pro).
on:
  push:
    branches: [main]
    paths:
      - "site/**"
      - "firmware/**"
      - ".github/workflows/pages.yml"
  workflow_dispatch:

permissions:
  contents: read
  pages: write
  id-token: write

# Allow one concurrent deployment; a newer push cancels an in-flight run.
concurrency:
  group: pages
  cancel-in-progress: true

jobs:
  deploy:
    runs-on: ubuntu-latest
    environment:
      name: github-pages
      url: ${{ steps.deployment.outputs.page_url }}
    steps:
      - uses: actions/checkout@v4

      - name: Build firmware + merge single-file image
        uses: espressif/esp-idf-ci-action@v1
        with:
          esp_idf_version: v5.5
          target: esp32s3
          path: firmware
          command: idf.py build && idf.py merge-bin -o ../site/install/ytm-firmware.bin

      - name: Verify the installer binary exists
        run: test -f site/install/ytm-firmware.bin

      - uses: actions/configure-pages@v5
      - uses: actions/upload-pages-artifact@v3
        with:
          path: site
      - id: deployment
        uses: actions/deploy-pages@v4
```

- [ ] **Step 2: Validate the workflow YAML locally**

Run:
```bash
python -c "import yaml; yaml.safe_load(open('.github/workflows/pages.yml')); print('yaml ok')"
```
Expected: `yaml ok`.

- [ ] **Step 3: Commit**

```bash
git add .github/workflows/pages.yml
git commit -m "ci: build firmware into site/install and deploy with the landing page

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

- [ ] **Step 4: End-to-end verification (post-merge)**

After this branch merges to `main`:

1. The push to `main` touching `firmware/**` triggers `pages.yml` (or run it via
   `workflow_dispatch`).
2. Confirm the Action builds the firmware, produces `site/install/ytm-firmware.bin`,
   and deploys the site.
3. Open `<pages-url>/install/` in Chrome/Edge and run a real flash + Wi-Fi provision
   against a board.

Record the outcome. If Pages is blocked by the free-plan limitation, note the
fallback host used (e.g. Netlify drop of `site/`).

---

## Task 7: Documentation

**Files:**
- Modify: `docs/CONFIGURATION.md`, `README.md`

**Interfaces:** none (docs only).

- [ ] **Step 1: Update CONFIGURATION.md**

In `docs/CONFIGURATION.md`, under the Wi-Fi rows of the Kconfig table, add a note:

```markdown
> **Wi-Fi is now a runtime setting.** For end users, the recommended path is the
> [web installer](../site/install/README.md): it flashes a prebuilt image and asks for
> Wi-Fi in the browser (stored in NVS). The `YTM_WIFI_SSID` / `YTM_WIFI_PASSWORD`
> Kconfig values remain only as a fallback for developer `menuconfig` builds — NVS
> credentials, when present, always win.
```

- [ ] **Step 2: Update README.md**

In `README.md`, add a new option above "Option B — On the board" (the manual flash path):

```markdown
### Option B — Web installer (no toolchain)

On a computer with **Chrome or Edge**, open the install page, plug in the board,
click **Install**, and enter your 2.4&nbsp;GHz Wi-Fi when prompted. No ESP-IDF, no
`menuconfig`. The page is published at `<pages-url>/install/` (from `site/install/`).
See [site/install/README.md](site/install/README.md).

*(The manual ESP-IDF path below is still available for development.)*
```

Renumber the subsequent manual section heading accordingly (e.g. "Option C — On the board, with ESP-IDF").

- [ ] **Step 3: Commit**

```bash
git add docs/CONFIGURATION.md README.md
git commit -m "docs: document the web installer + runtime Wi-Fi

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Self-Review notes

- **Spec coverage:** Firmware Wi-Fi→NVS (Task 3), provisioning boot state + setup screen (Task 4), Improv-serial responder (Tasks 1+4), theme untouched/dark-only (Global Constraints — no task modifies `styles.h`), web installer under `site/install/` (Task 5), CI/merge-bin/Pages via the **existing** `pages.yml` (Task 6), host tests for parser + creds (Tasks 1–2), docs (Task 7). All spec sections mapped.
- **Existing Pages deployment:** `origin/main` already serves `site/index.html` via `pages.yml`. The installer is folded into that single deployment (`site/install/`, workflow extended) rather than a competing `deploy-pages` — GitHub allows one Pages deployment per repo.
- **The C++-component risk** from the spec is resolved by implementing the codec in pure C (Task 1), which also satisfies the "host-testable pure function" requirement directly.
- **Type consistency:** `improv_command_t`, `improv_parser_feed`, `ytm_creds_load/valid/store` signatures are identical everywhere they appear (Tasks 1, 2, 3, 4).
- **Open risks carried into execution:** (1) USB-Serial-JTAG driver coexisting with the IDF console — verified manually in Task 4 Step 6; our parser's resync tolerates interleaved log bytes on the read side. (2) GitHub Pages on a private free-plan repo — flagged in Task 6 with a fallback.
```
