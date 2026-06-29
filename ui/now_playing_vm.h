// now_playing_vm.h — the board<->bridge contract (see SPEC-ytmusic-now-playing.md §7)
//
// SOURCE-AGNOSTIC BY DESIGN. The render layer reads ONLY this struct and never
// calls a backend directly. Mock fills it now; the Mac-side bridge fills it later.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PB_PLAYING,
    PB_PAUSED,
    PB_BUFFERING
} playback_t;

typedef struct {
    char        source_name[64];   // e.g. "YouTube Music" (or album/playlist context)
    bool        is_live;           // true only for live streams; default false

    char        title[96];         // empty => "Nothing playing right now"
    char        artist[96];
    char        album[96];         // optional context line; may be empty
    bool        ad_playing;        // true => show "Advertisement", ignore stale metadata

    // cover art: NULL => gradient placeholder block.
    // later: bridge pushes a PRE-RESIZED RGB565 lv_image_dsc_t (172x172). Board blits.
    const void *cover_img;         // lv_image_dsc_t* (RGB565) or NULL

    playback_t  playback;
    bool        is_favorite;       // like status (video.likeStatus === 2)

    // finite seekable timeline
    int32_t     position_sec;      // current position
    int32_t     duration_sec;      // total length (ignored when is_live)

    float       level;             // 0..1 audio energy for the ring visualizer

    bool        host_connected;    // false => disconnected state
} now_playing_vm_t;

#ifdef __cplusplus
}
#endif
