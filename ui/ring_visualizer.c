#include "ring_visualizer.h"
#include "styles.h"
#include <math.h>

// Geometry transcribed from the V2 design source (NowPlayingDeviceV2.dc.html
// canvas draw()): cover radius 86 (172px cover), baseGap 20, step 24, per-ring
// amplitudes [18,12,8], line widths 3.6 - i*0.7, alpha (0.8 - i*0.2)*(0.4 +
// 0.6*level).
//
// The rings are a STATIC halo: drawn once at a fixed resting level, never
// resized per frame. The board renders in software into slow PSRAM (no 2D
// accel), so animating the ~340px ring stack every frame re-composited the
// upscaled ambient glow + vignette beneath it 30x/sec, starving the render loop
// and the touch input it shares a task with. A fixed halo costs nothing per
// frame and lets the screen sit idle between data changes.
#define RING_COUNT 3
#define COVER_PX   172
#define CR         (COVER_PX / 2)   // 86
#define BASE_GAP   20
#define STEP       24

// Resting energy the static halo is drawn at (mid of the old idle..playing
// range): sets the ring radii, stroke opacity, and width once at create time.
#define STATIC_LEVEL 0.32f

static const float    s_amp[RING_COUNT]   = { 18.0f, 12.0f, 8.0f };
static const float    s_width[RING_COUNT]  = { 3.6f, 2.9f, 2.2f };
// Album-derived palette stops mapped inner -> outer (light -> mid -> accent).
// Default to the neutral no-art palette so the rings render before the first
// track-change palette is pushed in.
static lv_color_t     s_color[RING_COUNT];

static lv_obj_t *s_rings[RING_COUNT];

static inline lv_color_t pal_color(palette_rgb_t c)
{
    return lv_color_make(c.r, c.g, c.b);
}

static void place(lv_obj_t *o, float radius)
{
    int d = (int)(radius * 2.0f + 0.5f);
    lv_obj_set_size(o, d, d);
    lv_obj_center(o);
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

// Resting border opacity for ring i at a given energy level — the exact curve
// the static halo is drawn at. Shared by create and breathe so both agree.
static lv_opa_t ring_opa(int i, float level)
{
    float a = (0.8f - i * 0.2f) * (0.4f + 0.6f * level);
    if (a < 0.0f) a = 0.0f;
    if (a > 1.0f) a = 1.0f;
    return (lv_opa_t)(a * 255.0f + 0.5f);
}

ring_viz_t ring_viz_create(lv_obj_t *parent, int box)
{
    // Start neutral; the screen pushes the album palette on the first track.
    for (int i = 0; i < RING_COUNT; i++)
        s_color[i] = pal_color(PALETTE_NEUTRAL.stop[i]);

    ring_viz_t rv;
    rv.cont = lv_obj_create(parent);
    lv_obj_remove_style_all(rv.cont);
    lv_obj_set_size(rv.cont, box, box);
    lv_obj_set_style_bg_opa(rv.cont, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(rv.cont, LV_OBJ_FLAG_SCROLLABLE);

    // outer -> inner; placed once at the resting level and left static.
    for (int i = 0; i < RING_COUNT; i++) {
        s_rings[i] = make_ring(rv.cont, s_color[i]);
        float r = CR + BASE_GAP + i * STEP + s_amp[i] * STATIC_LEVEL * 0.9f;
        place(s_rings[i], r);
        lv_obj_set_style_border_opa(s_rings[i], ring_opa(i, STATIC_LEVEL), 0);
        int w = (int)(s_width[i] + 0.5f);
        lv_obj_set_style_border_width(s_rings[i], w < 1 ? 1 : w, 0);
    }

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

void ring_viz_set_palette(ring_viz_t *rv, const palette_t *pal)
{
    (void)rv;
    if (!pal) pal = &PALETTE_NEUTRAL;
    for (int i = 0; i < RING_COUNT; i++) {
        s_color[i] = pal_color(pal->stop[i]);
        lv_obj_set_style_border_color(s_rings[i], s_color[i], 0);
    }
}

// Breath tuning. Decimate caps the update rate (now_playing_update ticks ~30 Hz,
// so 3 -> ~10 Hz). Only the inner BREATH_RINGS rings breathe (inner rings have the
// smallest bounding boxes -> the cheapest invalidations). Amplitude/rate are a
// slow, gentle swing around STATIC_LEVEL.
#define BREATH_DECIMATE 3      // push an opacity update every Nth call
#define BREATH_RINGS    2      // inner-most N rings breathe (1..RING_COUNT)
#define BREATH_AMPL     0.10f  // +/- level swing around STATIC_LEVEL
#define BREATH_RATE     0.10f  // sine phase increment per accepted update

void ring_viz_breathe(ring_viz_t *rv, bool active)
{
    (void)rv;
    static bool     was_active;
    static uint32_t calls;
    static uint32_t steps;

    if (!active) {
        // Settle back to the static halo exactly once; then no per-frame cost.
        if (was_active) {
            for (int i = 0; i < RING_COUNT; i++)
                lv_obj_set_style_border_opa(s_rings[i], ring_opa(i, STATIC_LEVEL), 0);
            was_active = false;
        }
        return;
    }
    was_active = true;

    // Throttle: only touch styles every BREATH_DECIMATE calls.
    if (calls++ % BREATH_DECIMATE != 0) return;

    float level = STATIC_LEVEL + BREATH_AMPL * sinf((float)(steps++) * BREATH_RATE);
    for (int i = 0; i < BREATH_RINGS && i < RING_COUNT; i++)
        lv_obj_set_style_border_opa(s_rings[i], ring_opa(i, level), 0);
}
