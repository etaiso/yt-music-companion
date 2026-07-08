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
