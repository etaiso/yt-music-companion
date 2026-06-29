// test_palette.c — the project's first host unit test.
//
// Exercises ui/palette.c standalone: no LVGL, no panel, no test framework —
// just a tiny assert harness so it compiles and runs anywhere with a C
// compiler (see tests/CMakeLists.txt, or compile by hand:
//   cc -std=c11 -I../ui test_palette.c ../ui/palette.c -o test_palette && ./test_palette
//
// Asserts the behavioral contract from the PRD:
//   - no-art / grayscale input -> the fixed neutral palette
//   - solid color -> all stops carry that hue
//   - stops are ordered (light is lighter than mid)
//   - the accent is the most-saturated stop
//   - two-tone: accent locks onto the more-saturated cell
//   - output is deterministic
#include "palette.h"

#include <stdio.h>
#include <stdlib.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        if (cond) {                                                             \
            g_pass++;                                                           \
        } else {                                                                \
            g_fail++;                                                           \
            printf("  FAIL (%s:%d): ", __FILE__, __LINE__);                     \
            printf(__VA_ARGS__);                                                \
            printf("\n");                                                       \
        }                                                                       \
    } while (0)

// ---- helpers ----
static uint16_t mk565(int r, int g, int b)
{
    return (uint16_t)((((r >> 3) & 0x1f) << 11) | (((g >> 2) & 0x3f) << 5) | ((b >> 3) & 0x1f));
}

static void fill_solid(uint16_t *buf, int n, uint16_t px)
{
    for (int i = 0; i < n; i++) buf[i] = px;
}

static float absf(float v) { return v < 0 ? -v : v; }

// circular distance between two 0..1 hues
static float hue_diff(float a, float b)
{
    float d = absf(a - b);
    return d < 1.0f - d ? d : 1.0f - d;
}

static float sat_of(palette_rgb_t c) { float h, s, l; palette_rgb_to_hsl(c, &h, &s, &l); return s; }
static float lum_of(palette_rgb_t c) { float h, s, l; palette_rgb_to_hsl(c, &h, &s, &l); return l; }
static float hue_of(palette_rgb_t c) { float h, s, l; palette_rgb_to_hsl(c, &h, &s, &l); return h; }

static int pal_eq(palette_t a, palette_t b)
{
    for (int i = 0; i < 3; i++)
        if (a.stop[i].r != b.stop[i].r || a.stop[i].g != b.stop[i].g || a.stop[i].b != b.stop[i].b)
            return 0;
    return 1;
}

// ---- tests ----
static void test_no_art_is_neutral(void)
{
    printf("# no-art -> neutral\n");
    CHECK(pal_eq(palette_derive(NULL, 16, 16), PALETTE_NEUTRAL), "NULL bitmap -> neutral");
    uint16_t one = 0;
    CHECK(pal_eq(palette_derive(&one, 0, 0), PALETTE_NEUTRAL), "zero dims -> neutral");
    CHECK(pal_eq(palette_derive(&one, -4, 16), PALETTE_NEUTRAL), "negative dims -> neutral");
}

static void test_grayscale_is_neutral(void)
{
    printf("# grayscale -> neutral\n");
    uint16_t buf[16 * 16];
    fill_solid(buf, 16 * 16, mk565(128, 128, 128));
    CHECK(pal_eq(palette_derive(buf, 16, 16), PALETTE_NEUTRAL), "mid gray -> neutral");
    fill_solid(buf, 16 * 16, mk565(0, 0, 0));
    CHECK(pal_eq(palette_derive(buf, 16, 16), PALETTE_NEUTRAL), "black -> neutral");
    fill_solid(buf, 16 * 16, mk565(255, 255, 255));
    CHECK(pal_eq(palette_derive(buf, 16, 16), PALETTE_NEUTRAL), "white -> neutral");
    for (int i = 0; i < 16 * 16; i++) buf[i] = (i & 1) ? mk565(40, 40, 40) : mk565(200, 200, 200);
    CHECK(pal_eq(palette_derive(buf, 16, 16), PALETTE_NEUTRAL), "two-tone gray -> neutral");
}

static void test_solid_color_keeps_hue(void)
{
    printf("# solid color -> that hue\n");
    const int cases[][3] = { { 255, 40, 40 }, { 40, 120, 255 }, { 40, 220, 80 }, { 240, 200, 40 } };
    uint16_t buf[20 * 20];
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        int r = cases[c][0], g = cases[c][1], b = cases[c][2];
        palette_rgb_t in = { (uint8_t)r, (uint8_t)g, (uint8_t)b };
        float H = hue_of(in);
        fill_solid(buf, 20 * 20, mk565(r, g, b));
        palette_t p = palette_derive(buf, 20, 20);
        for (int i = 0; i < 3; i++)
            CHECK(hue_diff(hue_of(p.stop[i]), H) < 0.03f,
                  "solid (%d,%d,%d) stop%d hue %.3f ~ input %.3f", r, g, b, i, hue_of(p.stop[i]), H);
    }
}

static void test_stops_are_ordered(void)
{
    printf("# ordered stops (light L > mid L)\n");
    const uint16_t inputs[] = { mk565(255, 40, 40), mk565(40, 120, 255), mk565(200, 60, 180) };
    uint16_t buf[16 * 16];
    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
        fill_solid(buf, 16 * 16, inputs[i]);
        palette_t p = palette_derive(buf, 16, 16);
        CHECK(lum_of(p.stop[0]) > lum_of(p.stop[1]),
              "light L %.2f > mid L %.2f", lum_of(p.stop[0]), lum_of(p.stop[1]));
    }
}

static void test_accent_is_most_saturated(void)
{
    printf("# accent is the most-saturated stop\n");
    const uint16_t inputs[] = { mk565(255, 40, 40), mk565(40, 120, 255), mk565(240, 200, 40) };
    uint16_t buf[16 * 16];
    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
        fill_solid(buf, 16 * 16, inputs[i]);
        palette_t p = palette_derive(buf, 16, 16);
        float sA = sat_of(p.stop[2]), s0 = sat_of(p.stop[0]), s1 = sat_of(p.stop[1]);
        CHECK(sA >= s0 && sA >= s1, "accent sat %.2f >= light %.2f & mid %.2f", sA, s0, s1);
    }
}

static void test_two_tone_accent_picks_saturated_cell(void)
{
    printf("# two-tone: accent picks the more-saturated cell\n");
    enum { W = 16, H = 16 };
    uint16_t buf[W * H];
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            buf[y * W + x] = x < W / 2 ? mk565(230, 30, 30) : mk565(90, 100, 120);
    palette_t p = palette_derive(buf, W, H);
    palette_rgb_t red = { 230, 30, 30 };
    CHECK(hue_diff(hue_of(p.stop[2]), hue_of(red)) < 0.06f,
          "accent hue %.3f ~ vivid-red hue %.3f", hue_of(p.stop[2]), hue_of(red));
}

static void test_accent_prefers_brighter_vivid_cell(void)
{
    printf("# accent prefers the brighter of two vivid cells\n");
    enum { W = 16, H = 16 };
    uint16_t buf[W * H];
    // left: dark-but-vivid green; right: bright vivid orange. Both saturated,
    // so the "brightest" half of the selector should land on orange.
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            buf[y * W + x] = x < W / 2 ? mk565(0, 90, 0) : mk565(255, 170, 40);
    palette_t p = palette_derive(buf, W, H);
    palette_rgb_t orange = { 255, 170, 40 };
    CHECK(hue_diff(hue_of(p.stop[2]), hue_of(orange)) < 0.06f,
          "accent hue %.3f ~ bright-orange hue %.3f", hue_of(p.stop[2]), hue_of(orange));
}

static void test_deterministic(void)
{
    printf("# deterministic\n");
    uint16_t buf[16 * 16];
    fill_solid(buf, 16 * 16, mk565(120, 200, 60));
    CHECK(pal_eq(palette_derive(buf, 16, 16), palette_derive(buf, 16, 16)),
          "same input -> identical output");
}

int main(void)
{
    test_no_art_is_neutral();
    test_grayscale_is_neutral();
    test_solid_color_keeps_hue();
    test_stops_are_ordered();
    test_accent_is_most_saturated();
    test_two_tone_accent_picks_saturated_cell();
    test_accent_prefers_brighter_vivid_cell();
    test_deterministic();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? EXIT_FAILURE : EXIT_SUCCESS;
}
