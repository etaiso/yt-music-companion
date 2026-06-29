#include "ambient_paint.h"
#include <math.h>

// Base near-black, matching COL_BG (#070709) — kept as literals here so the
// module stays LVGL/styles-free and host-testable. The renderer composites the
// glow over this, so it reads as the same surface as the screen background.
#define BG_R 7
#define BG_G 7
#define BG_B 9

// Two elliptical blooms transcribed from the V2 design ambient() gradient
// (NowPlayingDeviceV2.dc.html). CSS sizes "S% at X% Y%" with the color fading to
// transparent at F% give an effective normalized radius of S*F; values below are
// the midnight palette's, used as the canonical mapping for any album:
//   top:    radial(120% 78% at 50% 4%),  fade 0..56%  -> r ~ (0.67, 0.44)
//   accent: radial(90%  60% at 78% 26%), fade 0..50%  -> r ~ (0.45, 0.30)
// The dominant (mid) stop drives the top bloom; the accent stop drives the
// offset bloom — same roles as the design's two color stops.
#define TOP_CX 0.50f
#define TOP_CY 0.04f
#define TOP_RX 0.67f
#define TOP_RY 0.44f
#define TOP_INTENSITY 0.85f

#define ACC_CX 0.78f
#define ACC_CY 0.26f
#define ACC_RX 0.45f
#define ACC_RY 0.30f
#define ACC_INTENSITY 0.62f

// Base vertical wash: a faint tint of the dominant hue at the very top settling
// to pure background by the bottom (design base linear-gradient #1b1336->#060410).
#define BASE_TINT 0.16f

static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline uint16_t to565(float r, float g, float b)
{
    int ri = (int)(clampf(r, 0.0f, 255.0f) + 0.5f);
    int gi = (int)(clampf(g, 0.0f, 255.0f) + 0.5f);
    int bi = (int)(clampf(b, 0.0f, 255.0f) + 0.5f);
    return (uint16_t)((((ri >> 3) & 0x1f) << 11) |
                      (((gi >> 2) & 0x3f) << 5) |
                      ((bi >> 3) & 0x1f));
}

// Coverage weight (0..1) of an elliptical bloom at (fx,fy): 1 at the center,
// linearly to 0 at the effective radius — the same ramp CSS radial-gradient
// uses between its two stops.
static float bloom_w(float fx, float fy, float cx, float cy, float rx, float ry)
{
    float dx = (fx - cx) / rx;
    float dy = (fy - cy) / ry;
    float d  = sqrtf(dx * dx + dy * dy);
    return clampf(1.0f - d, 0.0f, 1.0f);
}

void ambient_paint(uint16_t *buf, int w, int h, const palette_t *pal)
{
    if (!buf || w <= 0 || h <= 0) return;
    if (!pal) pal = &PALETTE_NEUTRAL;

    const palette_rgb_t mid = pal->stop[1]; // dominant tint -> top bloom + base
    const palette_rgb_t acc = pal->stop[2]; // accent -> offset bloom

    // Top of the base wash: background nudged toward the dominant hue.
    const float topR = BG_R + (mid.r - BG_R) * BASE_TINT;
    const float topG = BG_G + (mid.g - BG_G) * BASE_TINT;
    const float topB = BG_B + (mid.b - BG_B) * BASE_TINT;

    for (int y = 0; y < h; y++) {
        float fy = (h > 1) ? (float)y / (float)(h - 1) : 0.0f;
        // base: dominant-tinted top fading straight down to pure background
        float baseR = topR + (BG_R - topR) * fy;
        float baseG = topG + (BG_G - topG) * fy;
        float baseB = topB + (BG_B - topB) * fy;

        for (int x = 0; x < w; x++) {
            float fx = (w > 1) ? (float)x / (float)(w - 1) : 0.0f;

            float r = baseR, g = baseG, b = baseB;

            // accent bloom first (lower layer), then the dominant top bloom on
            // top of it — matching the design's stacking (top radial over accent).
            float wa = bloom_w(fx, fy, ACC_CX, ACC_CY, ACC_RX, ACC_RY) * ACC_INTENSITY;
            r += (acc.r - r) * wa;
            g += (acc.g - g) * wa;
            b += (acc.b - b) * wa;

            float wt = bloom_w(fx, fy, TOP_CX, TOP_CY, TOP_RX, TOP_RY) * TOP_INTENSITY;
            r += (mid.r - r) * wt;
            g += (mid.g - g) * wt;
            b += (mid.b - b) * wt;

            buf[y * w + x] = to565(r, g, b);
        }
    }
}
