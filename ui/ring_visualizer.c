#include "ring_visualizer.h"
#include "styles.h"
#include <math.h>

// Geometry transcribed from the Claude Design source (NowPlayingDevice.dc.html
// canvas draw()): cover radius 64, baseGap 18, step 22, per-ring amplitudes,
// line widths 3.4 - i*0.7, alpha (0.85 - i*0.22)*(0.35 + 0.65*level).
#define RING_COUNT 3
#define COVER_PX   128
#define CR         (COVER_PX / 2)   // 64
#define BASE_GAP   18
#define STEP       22

static const float    s_amp[RING_COUNT]   = { 16.0f, 11.0f, 7.0f };
static const float    s_width[RING_COUNT]  = { 3.4f, 2.7f, 2.0f };
// brand-gradient stops approximated per ring (outer->inner): indigo, purple, pink
static lv_color_t     s_color[RING_COUNT];

static lv_obj_t *s_rings[RING_COUNT];
static lv_obj_t *s_ripple;       // single expanding echo ring on peaks
static int       s_box;
static uint32_t  s_phase;
static float     s_ripple_r;     // current ripple radius, <=0 means inactive
static float     s_ripple_life;  // 1 -> 0

static lv_obj_t *make_ring(lv_obj_t *parent, lv_color_t color)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_style_radius(r, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(r, color, 0);
    lv_obj_set_style_border_width(r, 3, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(r);
    return r;
}

ring_viz_t ring_viz_create(lv_obj_t *parent, int box)
{
    s_box = box;
    s_color[0] = COL_INDIGO;
    s_color[1] = COL_PURPLE;
    s_color[2] = COL_PINK;

    ring_viz_t rv;
    rv.cont = lv_obj_create(parent);
    lv_obj_remove_style_all(rv.cont);
    lv_obj_set_size(rv.cont, box, box);
    lv_obj_set_style_bg_opa(rv.cont, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(rv.cont, LV_OBJ_FLAG_SCROLLABLE);

    // outer -> inner
    for (int i = 0; i < RING_COUNT; i++)
        s_rings[i] = make_ring(rv.cont, s_color[i]);

    s_ripple = make_ring(rv.cont, COL_PINK);
    lv_obj_add_flag(s_ripple, LV_OBJ_FLAG_HIDDEN);

    // Cover slot (128x128, radius 20) centered. Caller fills with placeholder,
    // gradient block, or real art.
    rv.cover_slot = lv_obj_create(rv.cont);
    lv_obj_remove_style_all(rv.cover_slot);
    lv_obj_set_size(rv.cover_slot, COVER_PX, COVER_PX);
    lv_obj_set_style_radius(rv.cover_slot, RAD_COVER, 0);
    lv_obj_set_style_clip_corner(rv.cover_slot, true, 0);
    lv_obj_clear_flag(rv.cover_slot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(rv.cover_slot);

    return rv;
}

static void place(lv_obj_t *o, float radius)
{
    int d = (int)(radius * 2.0f + 0.5f);
    lv_obj_set_size(o, d, d);
    lv_obj_center(o);
}

void ring_viz_update(ring_viz_t *rv, float level, bool reduce_motion)
{
    (void)rv;
    s_phase++;

    if (reduce_motion)
        level = 0.15f + 0.05f * sinf((float)s_phase * 0.05f);
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;

    for (int i = 0; i < RING_COUNT; i++) {
        float r = CR + BASE_GAP + i * STEP + s_amp[i] * level * 0.9f;
        place(s_rings[i], r);

        float a = (0.85f - i * 0.22f) * (0.35f + 0.65f * level);
        lv_obj_set_style_border_opa(s_rings[i], (lv_opa_t)(a * 255.0f), 0);
        int w = (int)(s_width[i] + 0.5f);
        lv_obj_set_style_border_width(s_rings[i], w < 1 ? 1 : w, 0);
    }

    // ripple echo on energy peaks (design: spawn when level > 0.7)
    if (!reduce_motion && level > 0.7f && s_ripple_life <= 0.0f) {
        s_ripple_r = CR + BASE_GAP;
        s_ripple_life = 1.0f;
        lv_obj_clear_flag(s_ripple, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_ripple_life > 0.0f) {
        s_ripple_r += 3.0f;
        s_ripple_life -= 0.03f;
        if (s_ripple_life <= 0.0f) {
            lv_obj_add_flag(s_ripple, LV_OBJ_FLAG_HIDDEN);
        } else {
            place(s_ripple, s_ripple_r);
            lv_obj_set_style_border_opa(s_ripple, (lv_opa_t)(s_ripple_life * 110.0f), 0);
            lv_obj_set_style_border_width(s_ripple, 2, 0);
        }
    }
}
