// idle.h — idle-dim decision logic (pure C; no ESP/LVGL, host-testable).
//
// Tracks time since the last user activity (touch + motion) and, when the
// device has been idle past a threshold AND nothing is playing, dims the
// screen. Any activity restores the user's brightness. See
// docs/superpowers/specs/2026-07-01-auto-screen-dim-on-idle-design.md
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t dim_after_ms;        // idle time with no activity before dimming
    int      dim_percent;         // panel brightness (%) while dimmed
    void   (*apply)(int percent); // set panel brightness now (must NOT persist)
    int    (*get_active)(void);   // user's current brightness (restore target)
} idle_cfg_t;

// Configure and reset the tracker. `now_ms` is a monotonic millisecond clock;
// the same clock source must be passed to idle_tick().
void idle_init(const idle_cfg_t *cfg, uint32_t now_ms);

// Register non-touch activity (e.g. IMU motion). Safe to call from an ISR or
// another task: it only sets a flag, consumed on the next idle_tick().
void idle_notify_activity(void);

// Run one decision step. `touch_inactive_ms` = ms since the last touch
// (LVGL's lv_display_get_inactive_time()); `now_ms` = the monotonic clock;
// `playing` = true while audio is playing (disables dimming; restores if dimmed).
void idle_tick(uint32_t touch_inactive_ms, uint32_t now_ms, bool playing);

// True when the screen is currently dimmed (inspection / tests).
bool idle_is_dimmed(void);

#ifdef __cplusplus
}
#endif
