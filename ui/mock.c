#include "mock.h"
#include "lvgl.h"
#include <math.h>
#include <string.h>

// Each scene holds for SCENE_MS, then we advance to the next so a DoD reviewer
// sees all six states cycle on their own.
#define SCENE_MS 6000

// Mock cover dimension. Mirrors the board/bridge cover contract (COVER_PX in
// firmware/main/net_backend.c and bridge/src/config.js) so the sim exercises
// the real on-the-wire cover size — 172x172 RGB565.
#define MOCK_COVER_PX 172

// A few synthetic 172x172 RGB565 covers with clearly different dominant hues, so
// the album-derived palette (rings + ambient glow) visibly changes as the demo
// cycles tracks — without hardware or the bridge. Built once in mock_init().
#define N_TRACKS 3

static uint16_t       s_cover_px[N_TRACKS][MOCK_COVER_PX * MOCK_COVER_PX];
static lv_image_dsc_t s_cover_dsc[N_TRACKS];

typedef struct {
    const char *title, *artist, *album;
    int32_t     duration;
    uint8_t     c0[3], c1[3];  // diagonal gradient endpoints -> the album's hue
} mock_track_t;

// Hues mirror the V2 design's three cover palettes (midnight / tides / lanterns).
static const mock_track_t s_tracks[N_TRACKS] = {
    { "Midnight Drive", "The Reverb Club", "Neon Nights - Album", 228,
      {  49,  46, 129 }, { 236,  72, 153 } },  // indigo -> pink
    { "Coastal Tides",  "Marina Vale",     "Undertow - EP",       195,
      {   8, 100, 120 }, { 103, 232, 249 } },  // deep -> bright cyan
    { "Paper Lanterns", "Akiko Mori",      "Festival - Single",   243,
      { 124,  45,  18 }, { 245, 158,  11 } },  // ember -> amber
};

static int s_track;  // current album index; advances each full scene cycle

static void build_mock_cover(int i)
{
    const mock_track_t *t = &s_tracks[i];
    // Asymmetric diagonal blend (weights != 0.5) so a wrong width/orientation
    // shows as a skewed image, as the original single cover did.
    for (int y = 0; y < MOCK_COVER_PX; y++) {
        for (int x = 0; x < MOCK_COVER_PX; x++) {
            float fx = (float)x / (MOCK_COVER_PX - 1);
            float fy = (float)y / (MOCK_COVER_PX - 1);
            float d  = fx * 0.65f + fy * 0.35f;
            int r = (int)(t->c0[0] + (t->c1[0] - t->c0[0]) * d);
            int g = (int)(t->c0[1] + (t->c1[1] - t->c0[1]) * d);
            int b = (int)(t->c0[2] + (t->c1[2] - t->c0[2]) * d);
            uint16_t v = ((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3);
            s_cover_px[i][y * MOCK_COVER_PX + x] = v;
        }
    }

    memset(&s_cover_dsc[i], 0, sizeof s_cover_dsc[i]);
    s_cover_dsc[i].header.magic  = LV_IMAGE_HEADER_MAGIC;
    s_cover_dsc[i].header.cf     = LV_COLOR_FORMAT_RGB565; // RGB565 LE, matches the panel/board
    s_cover_dsc[i].header.w      = MOCK_COVER_PX;
    s_cover_dsc[i].header.h      = MOCK_COVER_PX;
    s_cover_dsc[i].header.stride = MOCK_COVER_PX * 2;
    s_cover_dsc[i].data          = (const uint8_t *)s_cover_px[i];
    s_cover_dsc[i].data_size     = sizeof s_cover_px[i];
}

typedef enum {
    SC_PLAYING = 0,
    SC_PAUSED,
    SC_BUFFERING,
    SC_AD,
    SC_NOTRACK,
    SC_DISCONNECTED,
    SC__COUNT
} scene_t;

static scene_t  s_scene;
static uint32_t s_scene_ms;
static float    s_t;        // free-running time for level synthesis

static void load_track(now_playing_vm_t *vm)
{
    const mock_track_t *t = &s_tracks[s_track];
    strncpy(vm->source_name, "YOUTUBE MUSIC", sizeof vm->source_name - 1);
    strncpy(vm->title,  t->title,  sizeof vm->title  - 1);
    strncpy(vm->artist, t->artist, sizeof vm->artist - 1);
    strncpy(vm->album,  t->album,  sizeof vm->album  - 1);
    vm->duration_sec = t->duration;
    vm->cover_img    = &s_cover_dsc[s_track];  // swaps per track -> palette re-derives
}

void mock_init(now_playing_vm_t *vm)
{
    memset(vm, 0, sizeof *vm);
    for (int i = 0; i < N_TRACKS; i++)
        build_mock_cover(i);   // 172x172 RGB565 hero art (ad/empty still show the gradient block)
    s_track = 0;
    load_track(vm);
    vm->position_sec  = 72;   // 1:12
    vm->playback      = PB_PLAYING;
    vm->host_connected = true;
    vm->conn_state    = CONN_ONLINE;
    vm->battery_present = true;
    vm->battery_percent = 64;
    vm->charging        = false;
    vm->is_favorite   = false;
    s_scene = SC_PLAYING;
}

// smooth pseudo-random energy in 0..1 (two detuned sines + a slow envelope)
static float synth_level(float t)
{
    float a = 0.5f + 0.5f * sinf(t * 6.3f);
    float b = 0.5f + 0.5f * sinf(t * 11.7f + 1.3f);
    float env = 0.55f + 0.45f * sinf(t * 0.8f);
    float v = (0.6f * a + 0.4f * b) * env;
    return v < 0 ? 0 : (v > 1 ? 1 : v);
}

static void enter_scene(now_playing_vm_t *vm, scene_t sc)
{
    // reset to a known playing track, then apply the scene's deltas
    load_track(vm);
    vm->host_connected = true;
    vm->conn_state     = CONN_ONLINE;
    vm->ad_playing     = false;
    vm->playback       = PB_PLAYING;

    vm->position_sec = 72;  // 1:12 default (design)

    switch (sc) {
        case SC_PLAYING:                                   break;
        case SC_PAUSED:       vm->playback = PB_PAUSED;     break;
        case SC_BUFFERING:    vm->playback = PB_BUFFERING;
                              vm->position_sec = 41;        break;
        case SC_AD:           vm->ad_playing = true;
                              vm->duration_sec = 30;
                              vm->position_sec = 9;         break;
        case SC_NOTRACK:
            vm->title[0] = vm->artist[0] = vm->album[0] = '\0';
            vm->duration_sec = 0; vm->position_sec = 0;     break;
        case SC_DISCONNECTED: vm->host_connected = false;
                              vm->conn_state = CONN_OFFLINE;   break;
        default:                                            break;
    }
}

void mock_tick(now_playing_vm_t *vm, uint32_t dt_ms)
{
    s_t += dt_ms / 1000.0f;

    // slow battery animation for the sim: drain, then "plug in" and recharge
    static float batt_acc;
    static bool  batt_chg;
    batt_acc += dt_ms / 1000.0f;
    if (batt_acc >= 2.0f) {            // step every 2 s
        batt_acc = 0;
        if (batt_chg) {
            if (++vm->battery_percent >= 100) batt_chg = false;
        } else {
            if (--vm->battery_percent <= 10) batt_chg = true;
        }
        vm->charging = batt_chg;
    }
    vm->battery_present = true;   // always present in the sim

    // scene rotation
    s_scene_ms += dt_ms;
    if (s_scene_ms >= SCENE_MS) {
        s_scene_ms = 0;
        s_scene = (scene_t)((s_scene + 1) % SC__COUNT);
        // each full cycle (back to PLAYING) advances to the next album, so the
        // glow + rings visibly re-derive on a track change.
        if (s_scene == SC_PLAYING) s_track = (s_track + 1) % N_TRACKS;
        // flip the like state each loop so both appearances are seen
        bool fav = vm->is_favorite;
        enter_scene(vm, s_scene);
        vm->is_favorite = (s_scene == SC_PLAYING) ? !fav : fav;
    }

    // energy only while actually playing audible content
    bool playing = vm->host_connected && vm->playback == PB_PLAYING &&
                   !vm->ad_playing;
    vm->level = playing ? synth_level(s_t) : 0.0f;

    // advance + loop the timeline while playing a real track
    if (playing && vm->duration_sec > 0) {
        static float acc;
        acc += dt_ms / 1000.0f;
        if (acc >= 1.0f) {
            acc -= 1.0f;
            vm->position_sec++;
            if (vm->position_sec >= vm->duration_sec) vm->position_sec = 0;
        }
    }
}
