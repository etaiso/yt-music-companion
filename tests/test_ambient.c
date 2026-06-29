// test_ambient.c — host unit test for the pure ambient-glow painter.
//
// Exercises ui/ambient_paint.c standalone: no LVGL, no panel, no framework —
// the same tiny assert harness as test_palette.c (see tests/CMakeLists.txt, or
// compile by hand:
//   cc -std=c11 -I../ui test_ambient.c ../ui/ambient_paint.c ../ui/palette.c \
//      -lm -o test_ambient && ./test_ambient
//
// Asserts the behavioral contract of the bloom field (issue V2 slice 6):
//   - deterministic output
//   - top of the field is brighter than the bottom (top bloom + base fades down)
//   - a colored palette tints the field toward its hue; a neutral palette stays
//     neutral (r ~= g ~= b everywhere) — that's the no-art wash
//   - the offset accent bloom carries the accent stop's hue
//   - graceful handling of NULL / zero-size inputs
#include "ambient_paint.h"
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

#define W 120
#define H 120

static uint16_t g_buf[W * H];

static int r5(uint16_t px) { return (px >> 11) & 0x1f; }
static int g6(uint16_t px) { return (px >> 5) & 0x3f; }
static int b5(uint16_t px) { return px & 0x1f; }

// approximate luma on the 5/6/5 scale (g weighted up to its 6-bit range)
static int luma(uint16_t px) { return r5(px) * 2 + g6(px) + b5(px) * 2; }

static uint16_t at(int x, int y) { return g_buf[y * W + x]; }

// A vivid palette with a blue dominant (mid) and a red-ish accent, so hue tests
// have something to lock onto. Stops: light -> mid -> accent.
static palette_t vivid_blue_red(void)
{
    palette_t p;
    p.stop[0] = (palette_rgb_t){ 180, 200, 255 }; // light (pale blue)
    p.stop[1] = (palette_rgb_t){  40,  70, 230 }; // mid   (dominant blue)
    p.stop[2] = (palette_rgb_t){ 240,  50,  90 }; // accent (red/pink)
    return p;
}

int main(void)
{
    // ---- determinism ----
    ambient_paint(g_buf, W, H, &PALETTE_NEUTRAL);
    static uint16_t snapshot[W * H];
    for (int i = 0; i < W * H; i++) snapshot[i] = g_buf[i];
    ambient_paint(g_buf, W, H, &PALETTE_NEUTRAL);
    int identical = 1;
    for (int i = 0; i < W * H; i++) if (snapshot[i] != g_buf[i]) { identical = 0; break; }
    CHECK(identical, "ambient_paint must be deterministic for the same palette");

    // ---- neutral wash stays neutral (no strong hue) ----
    ambient_paint(g_buf, W, H, &PALETTE_NEUTRAL);
    int neutral_ok = 1;
    for (int y = 0; y < H; y += 7) {
        for (int x = 0; x < W; x += 7) {
            uint16_t px = at(x, y);
            // compare channels on a common 0..31 scale (green is 6-bit)
            int r = r5(px), g = g6(px) / 2, b = b5(px);
            int spread = (r > g ? r - g : g - r) + (g > b ? g - b : b - g) +
                         (r > b ? r - b : b - r);
            if (spread > 6) { neutral_ok = 0; }
        }
    }
    CHECK(neutral_ok, "neutral palette should produce a near-gray wash (no hue)");

    // ---- vertical falloff: top brighter than bottom ----
    ambient_paint(g_buf, W, H, &PALETTE_NEUTRAL);
    int top = luma(at(W / 2, 2));
    int bot = luma(at(W / 2, H - 3));
    CHECK(top > bot, "top of the field should be brighter than the bottom (%d vs %d)", top, bot);

    // ---- colored palette tints toward its hue ----
    palette_t vp = vivid_blue_red();
    ambient_paint(g_buf, W, H, &vp);
    // somewhere in the top bloom, blue should dominate (mid is blue)
    uint16_t topmid = at(W / 2, H / 6);
    CHECK(b5(topmid) > r5(topmid),
          "blue-dominant palette should make the top bloom bluer than red (b=%d r=%d)",
          b5(topmid), r5(topmid));
    // the top bloom region should be more saturated/colored than the dark bottom
    CHECK(luma(topmid) > luma(at(W / 2, H - 3)),
          "colored top bloom should be brighter than the dark base bottom");

    // ---- offset accent bloom carries the accent hue (red) ----
    // accent center is right-of-center, upper area; compare its red against the
    // left edge at the same row where the accent does not reach.
    int ay = H / 4;
    uint16_t accent_px = at((W * 78) / 100, ay);
    uint16_t away_px   = at((W * 8) / 100, ay);
    CHECK(r5(accent_px) > r5(away_px),
          "accent bloom should raise red near its center vs. away (%d vs %d)",
          r5(accent_px), r5(away_px));

    // ---- graceful bad inputs (must not crash) ----
    ambient_paint(NULL, W, H, &vp);
    ambient_paint(g_buf, 0, 0, &vp);
    ambient_paint(g_buf, W, H, NULL); // NULL pal => neutral wash, no crash
    CHECK(1, "bad inputs handled without crashing");

    printf("\n%s: %d passed, %d failed\n",
           g_fail ? "FAILURE" : "OK", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
