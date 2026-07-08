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
