#include "ring_visualizer.h"
#include "styles.h"
#include <math.h>

// Geometry transcribed from the V2 design source (NowPlayingDeviceV2.dc.html
// canvas draw()): cover radius 86 (172px cover), baseGap 20, step 24, per-ring
// amplitudes [18,12,8], line widths 3.6 - i*0.7, alpha (0.8 - i*0.2)*(0.4 +
// 0.6*level), ripple echo on peaks (alpha life*0.4, width 2.4*life + 0.4).
#define RING_COUNT 3
#define COVER_PX   172
#define CR         (COVER_PX / 2)   // 86
#define BASE_GAP   20
#define STEP       24

static const float    s_amp[RING_COUNT]   = { 18.0f, 12.0f, 8.0f };
static const float    s_width[RING_COUNT]  = { 3.6f, 2.9f, 2.2f };
// Album-derived palette stops mapped inner -> outer (light -> mid -> accent);
// the ripple echo takes the accent. Default to the neutral no-art palette so the
// rings render before the first track-change palette is pushed in.
static lv_color_t     s_color[RING_COUNT];
static lv_color_t     s_ripple_color;

static lv_obj_t *s_rings[RING_COUNT];
static lv_obj_t *s_ripple;       // single expanding echo ring on peaks
static int       s_box;
static uint32_t  s_phase;
static float     s_ripple_r;     // current ripple radius, <=0 means inactive
static float     s_ripple_life;  // 1 -> 0

static inline lv_color_t pal_color(palette_rgb_t c)
{
    return lv_color_make(c.r, c.g, c.b);
}

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
    // Start neutral; the screen pushes the album palette on the first track.
    for (int i = 0; i < RING_COUNT; i++)
        s_color[i] = pal_color(PALETTE_NEUTRAL.stop[i]);
    s_ripple_color = pal_color(PALETTE_NEUTRAL.stop[2]);

    ring_viz_t rv;
    rv.cont = lv_obj_create(parent);
    lv_obj_remove_style_all(rv.cont);
    lv_obj_set_size(rv.cont, box, box);
    lv_obj_set_style_bg_opa(rv.cont, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(rv.cont, LV_OBJ_FLAG_SCROLLABLE);

    // outer -> inner
    for (int i = 0; i < RING_COUNT; i++)
        s_rings[i] = make_ring(rv.cont, s_color[i]);

    s_ripple = make_ring(rv.cont, s_ripple_color);
    lv_obj_add_flag(s_ripple, LV_OBJ_FLAG_HIDDEN);

    // Cover slot (172x172, radius RAD_COVER) centered. Caller fills with
    // placeholder, gradient block, or real art.
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

void ring_viz_set_palette(ring_viz_t *rv, const palette_t *pal)
{
    (void)rv;
    if (!pal) pal = &PALETTE_NEUTRAL;
    for (int i = 0; i < RING_COUNT; i++) {
        s_color[i] = pal_color(pal->stop[i]);
        lv_obj_set_style_border_color(s_rings[i], s_color[i], 0);
    }
    s_ripple_color = pal_color(pal->stop[2]);  // accent
    lv_obj_set_style_border_color(s_ripple, s_ripple_color, 0);
}

void ring_viz_update(ring_viz_t *rv, float level, bool reduce_motion)
{
    (void)rv;
    s_phase++;

    if (reduce_motion) {
        // accessibility: minimal slow breathing instead of energy-reactive motion
        level = 0.15f + 0.05f * sinf((float)s_phase * 0.05f);
    } else if (level < 0.02f) {
        // No real audio energy from the feed: ytmdesktop exposes none, so the live
        // bridge always sends level 0 and the rings would sit still. Fall back to a
        // synthesized "breathing" pulse — same two-detuned-sines + slow envelope as
        // the mock (mock.c synth_level), retuned from per-second to the ~30fps
        // per-frame call rate. Paused/idle pass level >= 0.06, so they stay calm.
        float t   = (float)s_phase;
        float a   = 0.5f + 0.5f * sinf(t * 0.21f);
        float b   = 0.5f + 0.5f * sinf(t * 0.39f + 1.3f);
        float env = 0.55f + 0.45f * sinf(t * 0.027f);
        level = (0.6f * a + 0.4f * b) * env;
    }
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;

    for (int i = 0; i < RING_COUNT; i++) {
        float r = CR + BASE_GAP + i * STEP + s_amp[i] * level * 0.9f;
        place(s_rings[i], r);

        float a = (0.8f - i * 0.2f) * (0.4f + 0.6f * level);
        lv_obj_set_style_border_opa(s_rings[i], (lv_opa_t)(a * 255.0f), 0);
        int w = (int)(s_width[i] + 0.5f);
        lv_obj_set_style_border_width(s_rings[i], w < 1 ? 1 : w, 0);
    }

    // ripple echo on energy peaks (V2: spawn when level > 0.7)
    if (!reduce_motion && level > 0.7f && s_ripple_life <= 0.0f) {
        s_ripple_r = CR + BASE_GAP;
        s_ripple_life = 1.0f;
        lv_obj_clear_flag(s_ripple, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_ripple_life > 0.0f) {
        s_ripple_r += 2.8f;
        s_ripple_life -= 0.02f;
        if (s_ripple_life <= 0.0f) {
            lv_obj_add_flag(s_ripple, LV_OBJ_FLAG_HIDDEN);
        } else {
            place(s_ripple, s_ripple_r);
            lv_obj_set_style_border_opa(s_ripple, (lv_opa_t)(s_ripple_life * 0.4f * 255.0f), 0);
            int rw = (int)(2.4f * s_ripple_life + 0.4f + 0.5f);
            lv_obj_set_style_border_width(s_ripple, rw < 1 ? 1 : rw, 0);
        }
    }
}
