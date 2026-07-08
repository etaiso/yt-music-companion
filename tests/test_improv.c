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
        CHECK(n == 11, "state packet length 11 (got %zu)", n);
        CHECK(memcmp(out, "IMPROV", 6) == 0, "header");
        CHECK(out[6] == 0x01 && out[7] == 0x01, "version + type=current-state");
        CHECK(out[8] == 0x01 && out[9] == IMPROV_STATE_PROVISIONED, "len + payload");
        uint32_t sum = 0; for (int i = 0; i < 10; i++) sum += out[i];
        CHECK(out[10] == (uint8_t)(sum & 0xFF), "checksum");
    }

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
