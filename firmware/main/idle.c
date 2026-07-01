// idle.c — pure idle-dim decision logic. No ESP/LVGL; host-testable.
#include "idle.h"

static idle_cfg_t    s_cfg;
static bool          s_dimmed;
static uint32_t      s_last_motion_ms;
static volatile bool s_motion_pending;

void idle_init(const idle_cfg_t *cfg, uint32_t now_ms)
{
    s_cfg            = *cfg;
    s_dimmed         = false;
    s_last_motion_ms = now_ms;
    s_motion_pending = false;
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

void idle_tick(uint32_t touch_inactive_ms, uint32_t now_ms, bool playing)
{
    if (s_motion_pending) {
        s_motion_pending = false;
        s_last_motion_ms = now_ms;
    }

    if (playing) {          // never dim during playback; ensure lit
        restore();
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
}
