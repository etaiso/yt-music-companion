// battery.h — board battery status (AXP2101). Pure header: no ESP/LVGL deps so
// the decode logic is host-testable. battery.c provides the ESP-IDF driver.
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool present;    // battery detected on the MX1.25 connector
    int  percent;    // 0..100 state of charge (valid when present)
    bool charging;   // USB present and actively charging
    bool external;   // external power (VBUS/cable) present
} battery_status_t;

// Start the AXP2101 poll task (call once after the BSP I2C bus is up).
void battery_start(void);

// Copy the latest cached reading. Safe to call from the LVGL tick.
void battery_get(battery_status_t *out);

// Issue an AXP2101 soft power-off (board powers down; PWRON wakes it). Safe to
// call from the LVGL tick. Returns true on success, false if the I2C write
// failed (logs a warning either way); caller may retry on a later tick.
bool battery_power_off(void);

#ifdef __cplusplus
}
#endif
