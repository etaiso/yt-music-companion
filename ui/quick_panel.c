#include "quick_panel.h"
#include "styles.h"
#include <stdio.h>

static lv_obj_t       *s_backdrop;   // full-screen dim; tap to close
static lv_obj_t       *s_panel;      // the sliding sheet
static lv_obj_t       *s_slider;
static lv_obj_t       *s_pct;        // "NN%" beside the slider
static lv_obj_t       *s_batt_echo;  // "Battery NN% · charging"
static brightness_cb_t s_cb;

// While open, the full-screen top-layer backdrop captures gestures (they don't
// bubble to `screen`), so gesture_cb is registered on the backdrop too — that's
// how swipe-up-to-close reaches us. open/close on the already-correct state are
// harmless no-ops.
static void open_panel(void)  { lv_obj_clear_flag(s_backdrop, LV_OBJ_FLAG_HIDDEN); }
static void close_panel(void) { lv_obj_add_flag(s_backdrop, LV_OBJ_FLAG_HIDDEN); }

static void backdrop_cb(lv_event_t *e)
{
    // tap on the dim area (not the panel) closes
    if (lv_event_get_target(e) == s_backdrop) close_panel();
}

static void slider_cb(lv_event_t *e)
{
    (void)e;
    int v = (int)lv_slider_get_value(s_slider);
    lv_label_set_text_fmt(s_pct, "%d%%", v);
    if (s_cb) s_cb(v);
}

static void gesture_cb(lv_event_t *e)
{
    (void)e;
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_BOTTOM)      open_panel();
    else if (dir == LV_DIR_TOP)    close_panel();
}

void quick_panel_init(lv_obj_t *screen, brightness_cb_t cb, int initial_percent)
{
    if (s_backdrop) return;   // double-init guard: don't leak widgets / re-register the gesture
    s_cb = cb;

    // open gesture: a downward swipe anywhere on the screen
    lv_obj_add_event_cb(screen, gesture_cb, LV_EVENT_GESTURE, NULL);

    // backdrop on the top layer, hidden until opened
    s_backdrop = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_backdrop);
    lv_obj_set_size(s_backdrop, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_backdrop, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_backdrop, LV_OPA_50, 0);
    lv_obj_add_flag(s_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_backdrop, backdrop_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_backdrop, gesture_cb, LV_EVENT_GESTURE, NULL);  // swipe-up closes

    // the sheet
    s_panel = lv_obj_create(s_backdrop);
    lv_obj_remove_style_all(s_panel);
    lv_obj_set_size(s_panel, 480, 172);
    lv_obj_align(s_panel, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(s_panel, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_panel, 22, 0);
    lv_obj_set_style_pad_all(s_panel, 22, 0);
    lv_obj_set_flex_flow(s_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_panel, 14, 0);

    // brightness row: slider + percent
    s_slider = lv_slider_create(s_panel);
    lv_obj_set_width(s_slider, lv_pct(80));
    lv_slider_set_range(s_slider, 5, 100);
    lv_slider_set_value(s_slider, initial_percent, LV_ANIM_OFF);
    lv_obj_add_event_cb(s_slider, slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_pct = lv_label_create(s_panel);
    lv_obj_set_style_text_font(s_pct, FONT_LABEL, 0);
    lv_obj_set_style_text_color(s_pct, COL_INK, 0);
    lv_label_set_text_fmt(s_pct, "%d%%", initial_percent);

    s_batt_echo = lv_label_create(s_panel);
    lv_obj_set_style_text_font(s_batt_echo, FONT_LABEL, 0);
    lv_obj_set_style_text_color(s_batt_echo, COL_INK3, 0);
    lv_label_set_text(s_batt_echo, "");
}

void quick_panel_set_battery(int percent, bool charging, bool present)
{
    if (!s_batt_echo) return;

    // change-gated; this UI is render-bound (match the Task 3 battery widget)
    static int  s_last_pct     = -1;
    static bool s_last_chg     = false;
    static int  s_last_present = -1;   // -1 = uninitialised
    if (percent == s_last_pct && charging == s_last_chg && (int)present == s_last_present)
        return;
    s_last_pct = percent; s_last_chg = charging; s_last_present = (int)present;

    if (!present) { lv_label_set_text(s_batt_echo, "No battery"); return; }
    lv_label_set_text_fmt(s_batt_echo, "Battery %d%%%s", percent,
                          charging ? " \xC2\xB7 charging" : "");
}
