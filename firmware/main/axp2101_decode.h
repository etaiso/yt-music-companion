// axp2101_decode.h — pure decode of AXP2101 status registers into battery_status_t.
// Register addresses per the X-Powers AXP2101 datasheet (V1.x):
//   0x00 PMU status 1  — bit 3 = battery present/online; bit 5 = VBUS (external) present
//   0x01 PMU status 2  — bits [6:5] charge state: 01=charging, 10=discharging
//   0xA4 fuel-gauge SOC — battery percentage 0..100 (read-only)
#pragma once

#include <stdint.h>
#include "battery.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AXP2101_I2C_ADDR        0x34
#define AXP2101_REG_STATUS1     0x00
#define AXP2101_REG_STATUS2     0x01
#define AXP2101_REG_GAUGE_CFG   0x18  // bit 3 enables the fuel gauge
#define AXP2101_REG_BAT_PERCENT 0xA4

// Decode raw register bytes into out. Pure; no I/O.
// charging is reported only when a battery is present.
void axp2101_decode(uint8_t status1, uint8_t status2, uint8_t soc_pct,
                    battery_status_t *out);

#ifdef __cplusplus
}
#endif
