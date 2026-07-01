// idle.c — pure idle-dim decision logic. No ESP/LVGL; host-testable.
#include "idle.h"
#include <stddef.h>

static idle_cfg_t    s_cfg;
static bool          s_dimmed;
static uint32_t      s_last_motion_ms;
static volatile bool s_motion_pending;
static bool          s_power_off_done;   // latched once power_off() succeeds

void idle_init(const idle_cfg_t *cfg, uint32_t now_ms)
{
    s_cfg            = *cfg;
    s_dimmed         = false;
    s_last_motion_ms = now_ms;
    s_motion_pending = false;
    s_power_off_done = false;
}

void idle_notify_activity(void)
{
    s_motion_pending = true;   // consumed on the next idle_tick()
}

bool idle_is_dimmed(void)
{
    return s_dimmed;
}

static void restore(void)
{
    if (s_dimmed) {
        s_cfg.apply(s_cfg.get_active());
        s_dimmed = false;
    }
}

void idle_tick(uint32_t touch_inactive_ms, uint32_t now_ms,
               bool playing, bool power_off_allowed)
{
    if (s_motion_pending) {
        s_motion_pending = false;
        s_last_motion_ms = now_ms;
    }

    if (playing) {                 // playback counts as activity: stay lit now,
        s_last_motion_ms = now_ms; // keep the idle clock fresh, and never power
        restore();                 // off mid-song.
        return;
    }

    // Unsigned subtraction wraps safely on uint32 overflow (~49 days); ticks
    // arrive every ~33ms so true elapsed time never approaches that range.
    uint32_t motion_idle = now_ms - s_last_motion_ms;      // ms since last motion
    uint32_t idle_ms     = touch_inactive_ms < motion_idle // smaller idle value = more recent activity
                         ? touch_inactive_ms : motion_idle;

    if (s_dimmed) {
        if (idle_ms < s_cfg.dim_after_ms) restore();       // activity -> wake
    } else if (idle_ms >= s_cfg.dim_after_ms) {
        s_cfg.apply(s_cfg.dim_percent);
        s_dimmed = true;
    }

    // Power-off stage: only on battery, only once. If the callback fails (I2C
    // error) the flag stays clear so the next tick retries — never left half-off.
    if (!s_power_off_done && power_off_allowed &&
        s_cfg.power_off_after_ms != 0u && s_cfg.power_off != NULL &&
        idle_ms >= s_cfg.power_off_after_ms) {
        if (s_cfg.power_off()) s_power_off_done = true;
    }
}
