// now_playing_screen.h — the Now Playing screen (SPEC §6)
//
// Render layer reads ONLY now_playing_vm_t. No network code lives here; controls
// call an emit() stub. Used unchanged by both the ESP-IDF and the desktop sim.
#pragma once

#include "lvgl.h"
#include "now_playing_vm.h"

#ifdef __cplusplus
extern "C" {
#endif

// Command sink. cmd is one of: "toggle_play", "prev", "next",
// "toggle_favorite", "seek" (arg = absolute seconds), "volume_up/down".
typedef void (*emit_cb_t)(const char *cmd, int arg);

void      now_playing_set_emit(emit_cb_t cb);

// Build the screen into `parent` (typically lv_screen_active()).
lv_obj_t *now_playing_create(lv_obj_t *parent);

// Reflect the view-model into the widgets. Call once per frame from the tick.
void      now_playing_update(const now_playing_vm_t *vm);

#ifdef __cplusplus
}
#endif
