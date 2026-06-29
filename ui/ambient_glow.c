#include "ambient_glow.h"
#include "ambient_paint.h"
#include "styles.h"

#if THEME_AMBIENT_ENABLED

#include <stdlib.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"  // PSRAM allocation on the board
#endif

// Small source canvas; LVGL upscales it GLOW_SCALE/256 = 4x to the 480 panel.
// 120 keeps the buffer at 28 KB and divisible by the draw-buffer stride.
#define GLOW_PX     120
#define GLOW_UPSCALE 4
#define GLOW_SCALE  (LV_SCALE_NONE * GLOW_UPSCALE)  // 120 -> 480
#define SCREEN_PX   (GLOW_PX * GLOW_UPSCALE)        // full-bleed size = 480

// Vignette: a vertical fade deepening the bottom for depth, transcribed from the
// V2 design (NowPlayingDeviceV2.dc.html:25):
//   linear-gradient(180deg, rgba(7,7,9,0) 28%, .55 74%, .9 100%)
// LV_GRADIENT_MAX_STOPS is 2, so a 2-stop ramp clear@28% -> .9@bottom is used;
// linear interpolation through it lands ~.57 at 74%, reproducing the .55 midpoint.
#define VIG_CLEAR_FRAC  71               // 28% of 255: fully clear above here
#define VIG_DARK_OPA    ((lv_opa_t)229)  // 0.9 * 255 at the bottom edge

// The glow buffer lives in PSRAM on the board (like the cover buffers in
// net_backend.c) and on the heap in the desktop sim. ~28 KB, allocated once.
static uint16_t *glow_alloc(size_t bytes)
{
#ifdef ESP_PLATFORM
    return (uint16_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
#else
    return (uint16_t *)malloc(bytes);
#endif
}

// Vignette gradient descriptor must outlive create() (the style keeps a pointer).
static lv_grad_dsc_t s_vig_grad;

ambient_glow_t ambient_glow_create(lv_obj_t *parent)
{
    ambient_glow_t ag = { NULL, NULL };

    size_t bytes = LV_CANVAS_BUF_SIZE(GLOW_PX, GLOW_PX, 16, LV_DRAW_BUF_STRIDE_ALIGN);
    uint16_t *buf = glow_alloc(bytes);
    if (!buf) return ag;  // out of PSRAM: skip the glow rather than crash

    // ---- glow canvas (back-most layer) ----
    ag.canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(ag.canvas, buf, GLOW_PX, GLOW_PX, LV_COLOR_FORMAT_RGB565);
    lv_obj_add_flag(ag.canvas, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_clear_flag(ag.canvas, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    // Upscale around the canvas center, then center it on the screen so the 4x
    // draw fills 480x480. Antialiasing on the upscale is what blurs the blooms.
    lv_image_set_antialias(ag.canvas, true);
    lv_image_set_pivot(ag.canvas, GLOW_PX / 2, GLOW_PX / 2);
    lv_image_set_scale(ag.canvas, GLOW_SCALE);
    lv_obj_center(ag.canvas);

    // ---- vignette: transparent at the top, darkening toward the bottom ----
    s_vig_grad.dir = LV_GRAD_DIR_VER;
    s_vig_grad.stops_count = 2;
    s_vig_grad.stops[0].color = COL_BG;
    s_vig_grad.stops[0].opa   = LV_OPA_TRANSP;
    s_vig_grad.stops[0].frac  = VIG_CLEAR_FRAC;
    s_vig_grad.stops[1].color = COL_BG;
    s_vig_grad.stops[1].opa   = VIG_DARK_OPA;
    s_vig_grad.stops[1].frac  = 255;  // bottom edge

    ag.vignette = lv_obj_create(parent);
    lv_obj_remove_style_all(ag.vignette);
    // Full-bleed 480x480, centered like the glow canvas — NOT pct(100), which
    // would resolve against the screen's padded content area and leave the
    // panel edges unvignetted.
    lv_obj_set_size(ag.vignette, SCREEN_PX, SCREEN_PX);
    lv_obj_center(ag.vignette);
    lv_obj_add_flag(ag.vignette, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_clear_flag(ag.vignette, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(ag.vignette, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad(ag.vignette, &s_vig_grad, 0);

    // Paint the neutral wash up front; the screen pushes the album palette on the
    // first track change.
    ambient_glow_set_palette(&ag, &PALETTE_NEUTRAL);
    return ag;
}

void ambient_glow_set_palette(ambient_glow_t *ag, const palette_t *pal)
{
    if (!ag || !ag->canvas) return;
    // get_buf() is the recommended accessor (handles the canvas's internal align).
    uint16_t *buf = (uint16_t *)(void *)lv_canvas_get_buf(ag->canvas);
    ambient_paint(buf, GLOW_PX, GLOW_PX, pal);
    lv_obj_invalidate(ag->canvas);  // raw buffer write -> force a redraw
}

#else  // Light theme: no glow at all.

ambient_glow_t ambient_glow_create(lv_obj_t *parent)
{
    (void)parent;
    ambient_glow_t ag = { NULL, NULL };
    return ag;
}

void ambient_glow_set_palette(ambient_glow_t *ag, const palette_t *pal)
{
    (void)ag;
    (void)pal;
}

#endif // THEME_AMBIENT_ENABLED
