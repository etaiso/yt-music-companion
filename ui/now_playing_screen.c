// now_playing_screen.c — LVGL recreation of the Claude Design "Now Playing" V2
// (design/now-playing-screen-design/project/NowPlayingDeviceV2.dc.html).
//
// Render layer reads ONLY now_playing_vm_t. No network code; controls emit().
#include "now_playing_screen.h"
#include "ring_visualizer.h"
#include "ambient_glow.h"
#include "palette.h"
#include "styles.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static emit_cb_t  s_emit;

static lv_obj_t  *s_screen;
static lv_obj_t  *s_src_label;     // status bar left
static lv_obj_t  *s_state_dot;     // status bar right: colored dot
static lv_obj_t  *s_state_label;   // status bar right: text
static ring_viz_t s_ring;
static ambient_glow_t s_glow;     // album-derived ambient layer (Dark only)
static lv_obj_t  *s_cover_glyph;   // music_note glyph for ad/empty neutral block
static lv_obj_t  *s_cover_img;     // real art (hidden until provided)
static lv_obj_t  *s_title;
static lv_obj_t  *s_artist;
static lv_obj_t  *s_album;
static lv_obj_t  *s_shim1, *s_shim2;  // buffering shimmer bars
static lv_obj_t  *s_prog_row;
static lv_obj_t  *s_elapsed;
static lv_obj_t  *s_total;
static lv_obj_t  *s_slider;
static lv_obj_t  *s_play_label;
static lv_obj_t  *s_like_label;
static lv_obj_t  *s_banner;        // glassy disconnected banner

static bool       s_user_seeking;
static uint32_t   s_pulse;         // status-dot pulse counter
static const void *s_pal_key;      // cover_img for the palette in effect (NULL = neutral)

static void emit(const char *cmd, int arg) { if (s_emit) s_emit(cmd, arg); }

// Re-derive the album palette only when the art behind it changes — once per
// track change, not per frame (palette_derive downsamples the whole cover, and
// the ambient glow repaints its whole canvas). Both the rings and the static
// ambient glow are tinted from the same derived palette. no-art states (ad /
// idle / disconnected) and a missing cover fall back to the fixed neutral
// palette. `cover_img` identity tracks the track: net_backend double-buffers and
// publishes a fresh dsc per cover, the mock swaps the dsc per track, so a pointer
// change is a reliable "new art" signal.
static void refresh_palette(const now_playing_vm_t *vm, bool no_art)
{
    const void *key = no_art ? NULL : vm->cover_img;
    if (key == s_pal_key) return;
    s_pal_key = key;

    if (!key) {
        ring_viz_set_palette(&s_ring, &PALETTE_NEUTRAL);
        ambient_glow_set_palette(&s_glow, &PALETTE_NEUTRAL);
        return;
    }
    const lv_image_dsc_t *d = (const lv_image_dsc_t *)vm->cover_img;
    palette_t pal = palette_derive((const uint16_t *)(const void *)d->data,
                                   d->header.w, d->header.h);
    ring_viz_set_palette(&s_ring, &pal);
    ambient_glow_set_palette(&s_glow, &pal);
}

static void fmt_time(char *buf, size_t n, int32_t sec)
{
    if (sec < 0) sec = 0;
    // int32_t is `long` on the xtensa toolchain; cast so %d matches under -Werror=format.
    snprintf(buf, n, "%d:%02d", (int)(sec / 60), (int)(sec % 60));
}

// ---- events ----
static void on_play(lv_event_t *e)    { (void)e; emit("toggle_play", 0); }
static void on_prev(lv_event_t *e)    { (void)e; emit("prev", 0); }
static void on_next(lv_event_t *e)    { (void)e; emit("next", 0); }
static void on_like(lv_event_t *e)    { (void)e; emit("toggle_favorite", 0); }
static void on_dislike(lv_event_t *e) { (void)e; emit("dislike", 0); }

static void on_slider(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
        case LV_EVENT_PRESSED:  s_user_seeking = true; break;
        case LV_EVENT_RELEASED:
            emit("seek", lv_slider_get_value(s_slider));
            s_user_seeking = false;
            break;
        default: break;
    }
}

// ---- builders ----
static lv_obj_t *icon_btn(lv_obj_t *parent, const char *sym, int size,
                          const lv_font_t *font, lv_color_t color,
                          lv_event_cb_t cb, lv_obj_t **out_label)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, size, size);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, sym);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, color, 0);
    lv_obj_center(lbl);
    if (out_label) *out_label = lbl;
    return btn;
}

lv_obj_t *now_playing_create(lv_obj_t *parent)
{
    s_screen = parent;
    lv_obj_set_style_bg_color(s_screen, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_top(s_screen, 24, 0);
    lv_obj_set_style_pad_left(s_screen, 28, 0);
    lv_obj_set_style_pad_right(s_screen, 28, 0);
    lv_obj_set_style_pad_bottom(s_screen, 22, 0);
    lv_obj_set_style_pad_row(s_screen, 0, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_screen, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // ---- ambient glow (back-most layer; Dark only) ----
    // Created first so the upscaled glow canvas + vignette sit behind every other
    // widget. Both are IGNORE_LAYOUT, so the flex column above ignores them. In
    // the Light theme ambient_glow_create() builds nothing.
    s_glow = ambient_glow_create(s_screen);

    // ---- status bar ----
    lv_obj_t *bar = lv_obj_create(s_screen);
    lv_obj_remove_style_all(bar);
    lv_obj_set_width(bar, lv_pct(100));
    lv_obj_set_height(bar, 20);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    s_src_label = lv_label_create(bar);
    lv_obj_set_style_text_font(s_src_label, FONT_LABEL, 0);
    lv_obj_set_style_text_color(s_src_label, COL_INK3, 0);
    lv_obj_set_style_text_letter_space(s_src_label, 1, 0);
    lv_label_set_text(s_src_label, "YOUTUBE MUSIC");

    lv_obj_t *st = lv_obj_create(bar);
    lv_obj_remove_style_all(st);
    lv_obj_set_height(st, LV_SIZE_CONTENT);
    lv_obj_set_width(st, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(st, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(st, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(st, 6, 0);
    lv_obj_clear_flag(st, LV_OBJ_FLAG_SCROLLABLE);

    s_state_dot = lv_obj_create(st);
    lv_obj_remove_style_all(s_state_dot);
    lv_obj_set_size(s_state_dot, 8, 8);
    lv_obj_set_style_radius(s_state_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_state_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_state_dot, COL_PINK, 0);

    s_state_label = lv_label_create(st);
    lv_obj_set_style_text_font(s_state_label, FONT_LABEL, 0);
    lv_obj_set_style_text_color(s_state_label, COL_PINK, 0);
    lv_obj_set_style_text_letter_space(s_state_label, 1, 0);
    lv_label_set_text(s_state_label, "PLAYING");

    // ---- hero: rings + cover (flex-grow region) ----
    lv_obj_t *hero = lv_obj_create(s_screen);
    lv_obj_remove_style_all(hero);
    lv_obj_set_width(hero, lv_pct(100));
    lv_obj_set_flex_grow(hero, 1);
    lv_obj_clear_flag(hero, LV_OBJ_FLAG_SCROLLABLE);
    // V2 rings reach ~330px across (cr 86 + gap/step/amps) and ripples expand
    // past that; let them bleed beyond the hero box like the design's full-bleed
    // canvas instead of being clipped.
    lv_obj_add_flag(hero, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    s_ring = ring_viz_create(hero, 360);
    lv_obj_add_flag(s_ring.cont, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    // Raise the ring/cover center so the cover sits high on the screen (V2 design
    // cy ~188 of 480), leaving room for the title block below.
    lv_obj_align(s_ring.cont, LV_ALIGN_CENTER, 0, -30);

    // Default cover fill (a neutral block sits behind real art until it loads).
    lv_obj_set_style_bg_opa(s_ring.cover_slot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_ring.cover_slot, COL_STRIPE, 0);

    // music_note glyph centered in the neutral block (ad / empty states only).
    s_cover_glyph = lv_label_create(s_ring.cover_slot);
    lv_label_set_text(s_cover_glyph, IC_MUSIC);
    lv_obj_set_style_text_font(s_cover_glyph, FONT_ICONS, 0);
    lv_obj_set_style_text_color(s_cover_glyph, COL_INK4, 0);
    lv_obj_center(s_cover_glyph);
    lv_obj_add_flag(s_cover_glyph, LV_OBJ_FLAG_HIDDEN);

    s_cover_img = lv_image_create(s_ring.cover_slot);
    lv_obj_center(s_cover_img);
    lv_obj_add_flag(s_cover_img, LV_OBJ_FLAG_HIDDEN);

    // ---- title / artist / album (with buffering shimmer) ----
    lv_obj_t *meta = lv_obj_create(s_screen);
    lv_obj_remove_style_all(meta);
    lv_obj_set_width(meta, lv_pct(100));
    lv_obj_set_height(meta, 84);
    lv_obj_set_flex_flow(meta, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(meta, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(meta, 6, 0);
    lv_obj_clear_flag(meta, LV_OBJ_FLAG_SCROLLABLE);

    s_shim1 = lv_obj_create(meta);
    lv_obj_remove_style_all(s_shim1);
    lv_obj_set_size(s_shim1, 200, 26);
    lv_obj_set_style_radius(s_shim1, 8, 0);
    lv_obj_set_style_bg_opa(s_shim1, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_shim1, COL_LINE, 0);
    lv_obj_add_flag(s_shim1, LV_OBJ_FLAG_HIDDEN);

    s_shim2 = lv_obj_create(meta);
    lv_obj_remove_style_all(s_shim2);
    lv_obj_set_size(s_shim2, 130, 16);
    lv_obj_set_style_radius(s_shim2, 7, 0);
    lv_obj_set_style_bg_opa(s_shim2, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_shim2, COL_LINE, 0);
    lv_obj_add_flag(s_shim2, LV_OBJ_FLAG_HIDDEN);

    s_title = lv_label_create(meta);
    lv_obj_set_width(s_title, 360);
    // Single-line marquee: LV_LABEL_LONG_DOT only ellipsizes when the label's
    // HEIGHT is constrained; with auto height it wraps long titles onto a 2nd
    // line that grows out of the fixed-height meta box into the progress row.
    // SCROLL_CIRCULAR keeps the title to one line (scrolls if it overflows 340).
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(s_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_title, FONT_TITLE, 0);
    lv_obj_set_style_text_color(s_title, COL_INK, 0);
    lv_label_set_text(s_title, "Midnight Drive");

    s_artist = lv_label_create(meta);
    lv_obj_set_width(s_artist, 340);
    lv_label_set_long_mode(s_artist, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_artist, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_artist, FONT_BODY, 0);
    lv_obj_set_style_text_color(s_artist, COL_INK2, 0);
    lv_label_set_text(s_artist, "The Reverb Club");

    s_album = lv_label_create(meta);
    lv_obj_set_width(s_album, 340);
    lv_label_set_long_mode(s_album, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_album, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_album, FONT_META, 0);
    lv_obj_set_style_text_color(s_album, COL_INK3, 0);
    lv_label_set_text(s_album, "Neon Nights - Album");

    // ---- progress ----
    s_prog_row = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_prog_row);
    lv_obj_set_width(s_prog_row, lv_pct(100));
    lv_obj_set_height(s_prog_row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(s_prog_row, 8, 0);
    lv_obj_set_flex_flow(s_prog_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_prog_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_prog_row, 12, 0);
    lv_obj_clear_flag(s_prog_row, LV_OBJ_FLAG_SCROLLABLE);

    s_elapsed = lv_label_create(s_prog_row);
    lv_obj_set_width(s_elapsed, 34);
    lv_obj_set_style_text_align(s_elapsed, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_font(s_elapsed, FONT_TIME, 0);
    lv_obj_set_style_text_color(s_elapsed, COL_INK3, 0);
    lv_label_set_text(s_elapsed, "1:12");

    s_slider = lv_slider_create(s_prog_row);
    lv_obj_set_flex_grow(s_slider, 1);
    lv_obj_set_height(s_slider, 7);
    // knobless white bar: track rgba(255,255,255,.18)≈COL_LINE, white fill.
    lv_obj_set_style_bg_color(s_slider, COL_LINE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_slider, COL_INK, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    // knobless look (design has no knob), but still drag-to-seek. A generous
    // vertical touch area keeps the thin 7px bar easy to grab.
    lv_obj_set_style_bg_opa(s_slider, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_slider, 0, LV_PART_KNOB);
    lv_obj_set_ext_click_area(s_slider, 7);
    lv_obj_add_event_cb(s_slider, on_slider, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_slider, on_slider, LV_EVENT_RELEASED, NULL);

    s_total = lv_label_create(s_prog_row);
    lv_obj_set_width(s_total, 34);
    lv_obj_set_style_text_align(s_total, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(s_total, FONT_TIME, 0);
    lv_obj_set_style_text_color(s_total, COL_INK3, 0);
    lv_label_set_text(s_total, "3:48");

    // ---- transport ----
    lv_obj_t *tr = lv_obj_create(s_screen);
    lv_obj_remove_style_all(tr);
    lv_obj_set_width(tr, lv_pct(100));
    lv_obj_set_height(tr, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(tr, 16, 0);
    lv_obj_set_flex_flow(tr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tr, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(tr, LV_OBJ_FLAG_SCROLLABLE);

    // Transport glyphs use the bundled Material Symbols fonts (scripts/gen_icons.sh).
    // dislike: outline (mdi_line); like: toggles solid/outline on favorite (see update).
    icon_btn(tr, IC_DISLIKE, 56, FONT_ICONS_LINE, COL_INK3, on_dislike, NULL);
    icon_btn(tr, IC_PREV, 56, FONT_ICONS, COL_INK, on_prev, NULL);

    // V2 primary action: 80px white button with a dark glyph (COL_INK fill,
    // COL_BG glyph — inverts cleanly to a dark button in the Light theme).
    lv_obj_t *play = lv_button_create(tr);
    lv_obj_remove_style_all(play);
    lv_obj_set_size(play, 80, 80);
    lv_obj_set_style_radius(play, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(play, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(play, COL_INK, 0);
    lv_obj_set_style_shadow_color(play, COL_SHADOW, 0);
    lv_obj_set_style_shadow_opa(play, LV_OPA_40, 0);
    lv_obj_set_style_shadow_width(play, 30, 0);
    lv_obj_set_style_shadow_offset_y(play, 12, 0);
    lv_obj_add_event_cb(play, on_play, LV_EVENT_CLICKED, NULL);
    s_play_label = lv_label_create(play);
    lv_label_set_text(s_play_label, IC_PAUSE);
    lv_obj_set_style_text_font(s_play_label, FONT_ICONS, 0);
    lv_obj_set_style_text_color(s_play_label, COL_BG, 0);
    lv_obj_center(s_play_label);

    icon_btn(tr, IC_NEXT, 56, FONT_ICONS, COL_INK, on_next, NULL);
    icon_btn(tr, IC_LIKE, 56, FONT_ICONS_LINE, COL_INK3, on_like, &s_like_label);

    // ---- disconnected banner (glassy panel; LVGL has no backdrop blur, so an
    //      opaque dark rounded panel approximates it) ----
    s_banner = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_banner);
    lv_obj_set_width(s_banner, 440);
    lv_obj_set_height(s_banner, LV_SIZE_CONTENT);
    lv_obj_align(s_banner, LV_ALIGN_BOTTOM_MID, 0, -98);
    lv_obj_set_style_bg_color(s_banner, COL_BG2, 0);
    lv_obj_set_style_bg_opa(s_banner, 235, 0);   // ≈ rgba(.92)
    lv_obj_set_style_border_color(s_banner, COL_LINE, 0);
    lv_obj_set_style_border_width(s_banner, 1, 0);
    lv_obj_set_style_radius(s_banner, 16, 0);
    lv_obj_set_style_pad_hor(s_banner, 15, 0);
    lv_obj_set_style_pad_ver(s_banner, 13, 0);
    lv_obj_set_flex_flow(s_banner, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_banner, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_banner, 11, 0);
    lv_obj_add_flag(s_banner, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_clear_flag(s_banner, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bdot = lv_obj_create(s_banner);
    lv_obj_remove_style_all(bdot);
    lv_obj_set_size(bdot, 9, 9);
    lv_obj_set_style_radius(bdot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(bdot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bdot, COL_DANGER, 0);
    lv_obj_set_flex_grow(bdot, 0);

    lv_obj_t *bl = lv_label_create(s_banner);
    lv_obj_set_flex_grow(bl, 1);
    lv_label_set_long_mode(bl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(bl, FONT_META, 0);
    lv_obj_set_style_text_color(bl, COL_INK2, 0);
    lv_label_set_text(bl, "Can't reach your computer - check it's on and on the same network.");
    lv_obj_add_flag(s_banner, LV_OBJ_FLAG_HIDDEN);

    return s_screen;
}

void now_playing_update(const now_playing_vm_t *vm)
{
    s_pulse++;

    // ---- derive modes (design renderVals) ----
    bool disc      = !vm->host_connected;
    bool ad        = vm->ad_playing && !disc;
    bool empty     = (vm->title[0] == '\0') && !ad && !disc;
    bool buffering = vm->playback == PB_BUFFERING && !ad && !empty && !disc;
    bool paused    = vm->playback == PB_PAUSED && !ad && !empty && !disc;
    bool playing   = vm->playback == PB_PLAYING && !ad && !empty && !disc;

    // ---- rings (always drawn; level varies by state, color tracks the album) ----
    // no_art uses the neutral palette for ad/idle/disconnected (no usable art):
    // ad/empty paint the gradient cover block, disconnected may hold stale art.
    refresh_palette(vm, ad || empty || disc);
    float lvl;
    if (playing)        lvl = vm->level;       // mock/bridge energy
    else if (paused)    lvl = 0.16f;
    else                lvl = 0.06f;
    ring_viz_update(&s_ring, lvl, false /* reduce_motion */);

    // ---- status bar ----
    lv_label_set_text(s_src_label,
        vm->source_name[0] ? vm->source_name : "YOUTUBE MUSIC");
    if (playing) {
        lv_obj_set_style_bg_color(s_state_dot, COL_PINK, 0);
        lv_obj_set_style_text_color(s_state_label, COL_PINK, 0);
        lv_label_set_text(s_state_label, "PLAYING");
        // dot pulse
        float a = 0.65f + 0.35f * sinf((float)s_pulse * 0.18f);
        lv_obj_set_style_bg_opa(s_state_dot, (lv_opa_t)(a * 255), 0);
    } else if (disc) {
        lv_obj_set_style_bg_opa(s_state_dot, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s_state_dot, COL_DANGER, 0);
        lv_obj_set_style_text_color(s_state_label, COL_DANGER, 0);
        lv_label_set_text(s_state_label, "OFFLINE");
    } else {
        lv_obj_set_style_bg_opa(s_state_dot, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s_state_dot, COL_INK4, 0);
        lv_obj_set_style_text_color(s_state_label, COL_INK3, 0);
        lv_label_set_text(s_state_label,
            buffering ? "BUFFERING" : paused ? "PAUSED" : ad ? "AD" : "IDLE");
    }

    // ---- cover: real art, or the neutral music_note block (ad / empty) ----
    bool gradient_cover = empty || ad;
    bool have_art = vm->cover_img && !gradient_cover;
    if (gradient_cover) {
        lv_obj_set_style_bg_color(s_ring.cover_slot, COL_COVER_A, 0);
        lv_obj_set_style_bg_grad_color(s_ring.cover_slot, COL_COVER_B, 0);
        lv_obj_set_style_bg_grad_dir(s_ring.cover_slot, LV_GRAD_DIR_VER, 0);
    } else {
        lv_obj_set_style_bg_grad_dir(s_ring.cover_slot, LV_GRAD_DIR_NONE, 0);
        lv_obj_set_style_bg_color(s_ring.cover_slot, COL_STRIPE, 0);
    }
    if (have_art) {
        lv_image_set_src(s_cover_img, vm->cover_img);
        lv_obj_clear_flag(s_cover_img, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_cover_img, LV_OBJ_FLAG_HIDDEN);
    }
    // music_note glyph only over the neutral block (ad / empty)
    if (gradient_cover) lv_obj_clear_flag(s_cover_glyph, LV_OBJ_FLAG_HIDDEN);
    else                lv_obj_add_flag(s_cover_glyph, LV_OBJ_FLAG_HIDDEN);

    // ---- title / artist / album (or buffering shimmer) ----
    if (buffering) {
        lv_obj_clear_flag(s_shim1, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_shim2, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_title, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_artist, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_album, LV_OBJ_FLAG_HIDDEN);
        // subtle shimmer via opacity pulse
        float a = 0.6f + 0.4f * sinf((float)s_pulse * 0.2f);
        lv_obj_set_style_bg_opa(s_shim1, (lv_opa_t)(a * 255), 0);
        lv_obj_set_style_bg_opa(s_shim2, (lv_opa_t)(a * 255), 0);
    } else {
        lv_obj_add_flag(s_shim1, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_shim2, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_title, LV_OBJ_FLAG_HIDDEN);

        lv_label_set_text(s_title,
            empty ? "Nothing playing right now" :
            ad    ? "Advertisement" : vm->title);

        bool show_meta = (playing || paused || disc);
        if (show_meta && vm->artist[0]) {
            lv_label_set_text(s_artist, vm->artist);
            lv_obj_clear_flag(s_artist, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_artist, LV_OBJ_FLAG_HIDDEN);
        }
        if (show_meta && vm->album[0]) {
            lv_label_set_text(s_album, vm->album);
            lv_obj_clear_flag(s_album, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_album, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // ---- progress (shown unless empty) ----
    if (empty) {
        lv_obj_add_flag(s_prog_row, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(s_prog_row, LV_OBJ_FLAG_HIDDEN);
        char buf[16];
        if (vm->is_live) {
            lv_label_set_text(s_elapsed, "LIVE");
            lv_label_set_text(s_total, "");
            lv_slider_set_value(s_slider, 100, LV_ANIM_OFF);
        } else {
            fmt_time(buf, sizeof buf, vm->position_sec); lv_label_set_text(s_elapsed, buf);
            fmt_time(buf, sizeof buf, vm->duration_sec); lv_label_set_text(s_total, buf);
            int dur = vm->duration_sec > 0 ? vm->duration_sec : 1;
            lv_slider_set_range(s_slider, 0, dur);
            if (!s_user_seeking)
                lv_slider_set_value(s_slider, vm->position_sec, LV_ANIM_OFF);
        }
        bool seekable = (playing || paused) && !vm->is_live && vm->duration_sec > 0;
        if (seekable) lv_obj_clear_state(s_slider, LV_STATE_DISABLED);
        else          lv_obj_add_state(s_slider, LV_STATE_DISABLED);
    }

    // ---- play/pause glyph ----
    lv_label_set_text(s_play_label,
        (playing || buffering) ? IC_PAUSE : IC_PLAY);

    // ---- like: filled+pink when favorited, outline+grey otherwise ----
    lv_label_set_text(s_like_label, IC_LIKE);
    lv_obj_set_style_text_font(s_like_label,
        vm->is_favorite ? FONT_ICONS : FONT_ICONS_LINE, 0);
    lv_obj_set_style_text_color(s_like_label,
        vm->is_favorite ? COL_PINK : COL_INK3, 0);

    // ---- glassy banner (disconnected only; V1's dim scrim is gone) ----
    if (disc) lv_obj_clear_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
    else      lv_obj_add_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
}

void now_playing_set_emit(emit_cb_t cb) { s_emit = cb; }
