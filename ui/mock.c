#include "mock.h"
#include <math.h>
#include <string.h>

// Each scene holds for SCENE_MS, then we advance to the next so a DoD reviewer
// sees all six states cycle on their own.
#define SCENE_MS 6000

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
    strncpy(vm->source_name, "YOUTUBE MUSIC", sizeof vm->source_name - 1);
    strncpy(vm->title,  "Midnight Drive",  sizeof vm->title  - 1);
    strncpy(vm->artist, "The Reverb Club", sizeof vm->artist - 1);
    strncpy(vm->album,  "Neon Nights - Album", sizeof vm->album - 1);
    vm->duration_sec = 228;   // 3:48
}

void mock_init(now_playing_vm_t *vm)
{
    memset(vm, 0, sizeof *vm);
    load_track(vm);
    vm->position_sec  = 72;   // 1:12
    vm->playback      = PB_PLAYING;
    vm->host_connected = true;
    vm->is_favorite   = false;
    vm->cover_img     = NULL;  // gradient placeholder for the slice
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
        case SC_DISCONNECTED: vm->host_connected = false;   break;
        default:                                            break;
    }
}

void mock_tick(now_playing_vm_t *vm, uint32_t dt_ms)
{
    s_t += dt_ms / 1000.0f;

    // scene rotation
    s_scene_ms += dt_ms;
    if (s_scene_ms >= SCENE_MS) {
        s_scene_ms = 0;
        s_scene = (scene_t)((s_scene + 1) % SC__COUNT);
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
