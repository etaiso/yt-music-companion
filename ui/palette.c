// palette.c — see palette.h. Pure arithmetic only (no <math.h>, no LVGL) so the
// module compiles standalone on the host and links unchanged into firmware/sim.
#include "palette.h"

#define GRID     16     // downsample target (<=16x16 cells)
#define SAT_MIN  0.10f  // peak cell saturation below this => no usable hue

// Cool desaturated gray ramp; light -> mid -> accent.
const palette_t PALETTE_NEUTRAL = {
    .stop = {
        { 0xB9, 0xC0, 0xCC },
        { 0x8A, 0x93, 0xA3 },
        { 0x5C, 0x64, 0x73 },
    },
};

static inline float fclampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline float fmaxf3(float a, float b, float c)
{
    float m = a > b ? a : b;
    return m > c ? m : c;
}

static inline float fminf3(float a, float b, float c)
{
    float m = a < b ? a : b;
    return m < c ? m : c;
}

// RGB (0..1) -> HSL (0..1, h wraps). Matches the canonical formula.
static void rgb01_to_hsl(float r, float g, float b, float *ho, float *so, float *lo)
{
    float max = fmaxf3(r, g, b), min = fminf3(r, g, b);
    float l = (max + min) * 0.5f;
    float h = 0.0f, s = 0.0f;
    float d = max - min;
    if (d > 1e-9f) {
        s = l > 0.5f ? d / (2.0f - max - min) : d / (max + min);
        if (max == r)      h = (g - b) / d + (g < b ? 6.0f : 0.0f);
        else if (max == g) h = (b - r) / d + 2.0f;
        else               h = (r - g) / d + 4.0f;
        h /= 6.0f;
    }
    *ho = h; *so = s; *lo = l;
}

static float hue2rgb(float p, float q, float t)
{
    if (t < 0.0f) t += 1.0f;
    if (t > 1.0f) t -= 1.0f;
    if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
    if (t < 1.0f / 2.0f) return q;
    if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    return p;
}

// HSL (0..1) -> 8-bit RGB.
static palette_rgb_t hsl_to_rgb8(float h, float s, float l)
{
    float r, g, b;
    if (s <= 1e-9f) {
        r = g = b = l;
    } else {
        float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
        float p = 2.0f * l - q;
        r = hue2rgb(p, q, h + 1.0f / 3.0f);
        g = hue2rgb(p, q, h);
        b = hue2rgb(p, q, h - 1.0f / 3.0f);
    }
    palette_rgb_t out;
    out.r = (uint8_t)(fclampf(r, 0.0f, 1.0f) * 255.0f + 0.5f);
    out.g = (uint8_t)(fclampf(g, 0.0f, 1.0f) * 255.0f + 0.5f);
    out.b = (uint8_t)(fclampf(b, 0.0f, 1.0f) * 255.0f + 0.5f);
    return out;
}

void palette_rgb_to_hsl(palette_rgb_t c, float *h, float *s, float *l)
{
    rgb01_to_hsl(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, h, s, l);
}

// Decode one r5g6b5 pixel to 0..1 floats (bit-replicate to 8-bit first).
static void decode565(uint16_t px, float *r, float *g, float *b)
{
    uint8_t r5 = (px >> 11) & 0x1f, g6 = (px >> 5) & 0x3f, b5 = px & 0x1f;
    uint8_t r8 = (uint8_t)((r5 << 3) | (r5 >> 2));
    uint8_t g8 = (uint8_t)((g6 << 2) | (g6 >> 4));
    uint8_t b8 = (uint8_t)((b5 << 3) | (b5 >> 2));
    *r = r8 / 255.0f; *g = g8 / 255.0f; *b = b8 / 255.0f;
}

palette_t palette_derive(const uint16_t *rgb565, int w, int h)
{
    if (!rgb565 || w <= 0 || h <= 0) return PALETTE_NEUTRAL;

    int gx = w < GRID ? w : GRID;
    int gy = h < GRID ? h : GRID;

    float sumR = 0, sumG = 0, sumB = 0;
    int   nTot = 0;
    float maxSat = 0.0f, bestScore = -1.0f;  // maxSat gates the no-hue case
    float accR = 0, accG = 0, accB = 0;

    for (int cy = 0; cy < gy; cy++) {
        int y0 = (cy * h) / gy, y1 = ((cy + 1) * h) / gy;
        for (int cx = 0; cx < gx; cx++) {
            int x0 = (cx * w) / gx, x1 = ((cx + 1) * w) / gx;
            float cr = 0, cg = 0, cb = 0;
            int n = 0;
            for (int y = y0; y < y1; y++) {
                for (int x = x0; x < x1; x++) {
                    float r, g, b;
                    decode565(rgb565[y * w + x], &r, &g, &b);
                    cr += r; cg += g; cb += b; n++;
                }
            }
            if (n == 0) continue;
            cr /= (float)n; cg /= (float)n; cb /= (float)n;
            sumR += cr; sumG += cg; sumB += cb; nTot++;
            float ch, cs, cl;
            rgb01_to_hsl(cr, cg, cb, &ch, &cs, &cl);
            if (cs > maxSat) maxSat = cs;
            // accent favours cells that are both saturated and bright; saturation
            // dominates so a vivid hue still wins over a pale-but-bright cell.
            float score = cs * (0.6f + 0.4f * cl);
            if (score > bestScore) {
                bestScore = score; accR = cr; accG = cg; accB = cb;
            }
        }
    }

    if (nTot == 0 || maxSat < SAT_MIN) return PALETTE_NEUTRAL;

    float baseR = sumR / (float)nTot, baseG = sumG / (float)nTot, baseB = sumB / (float)nTot;
    float hb, sb, lb;
    rgb01_to_hsl(baseR, baseG, baseB, &hb, &sb, &lb);
    float ha, sa, la;
    rgb01_to_hsl(accR, accG, accB, &ha, &sa, &la);

    // mid = base tint pulled into a usable band
    float midS = fclampf(sb, 0.30f, 0.85f);
    float midL = fclampf(lb, 0.30f, 0.60f);
    // light = same hue, gentler saturation, raised lightness
    float liS = fclampf(sb * 0.65f, 0.10f, 0.55f);
    float liL = fclampf(midL + 0.24f, 0.0f, 0.86f);
    // accent = accent hue, boosted saturation, mid lightness
    float maxSab = sa > sb ? sa : sb;
    float acS = fclampf(maxSab * 1.10f + 0.10f, midS + 0.12f, 0.97f);
    float acL = fclampf(la, 0.42f, 0.60f);

    palette_t out;
    out.stop[0] = hsl_to_rgb8(hb, liS, liL);
    out.stop[1] = hsl_to_rgb8(hb, midS, midL);
    out.stop[2] = hsl_to_rgb8(ha, acS, acL);
    return out;
}
