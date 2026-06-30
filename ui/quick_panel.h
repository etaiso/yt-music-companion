// quick_panel.h — swipe-down quick-settings overlay (brightness slider + battery
// echo). Lives on lv_layer_top() above the Now Playing screen. The brightness
// value is applied through a platform-supplied sink (firmware: panel + NVS; sim:
// stub), so this file never calls bsp_*.
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*brightness_cb_t)(int percent);

// Build the panel and install the open gesture on `screen`. `cb` is called with
// the new percent (5..100) whenever the slider moves; the slider starts at
// `initial_percent`.
void quick_panel_init(lv_obj_t *screen, brightness_cb_t cb, int initial_percent);

// Update the panel's battery echo (no-op visually unless the panel is open).
void quick_panel_set_battery(int percent, bool charging, bool present);

#ifdef __cplusplus
}
#endif
