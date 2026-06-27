// net_backend.h — live feed for the board (SPEC-ytmusic-adapter.md §6).
//
// DEVICE-ONLY. This is the counterpart to mock.c: it fills now_playing_vm_t, but
// from the Mac-side bridge over WebSocket instead of synthetic data. It lives in
// firmware/ (not ui/) so the portable UI stays network-free. Bring-up:
//   WiFi (STA) -> mDNS browse _ytmboard._tcp -> WebSocket -> JSON/binary frames.
//
// Threading: the WebSocket runs on its own task and updates an internal snapshot
// under a mutex. The LVGL tick calls net_backend_get_vm() to copy that snapshot,
// then renders — never touching backend internals directly.
#pragma once

#include "now_playing_vm.h"

#ifdef __cplusplus
extern "C" {
#endif

// Start WiFi + discovery + the WebSocket client. Non-blocking; the feed comes up
// asynchronously. Until connected, the vm reports host_connected = false.
void net_backend_start(void);

// Copy the latest view-model into *out (thread-safe). cover_img points at a
// backend-owned, stable RGB565 buffer (or NULL). Call once per frame.
void net_backend_get_vm(now_playing_vm_t *out);

// Send a board command to the bridge as {"cmd":..,"arg":..} (SPEC §6/§7).
// Matches emit_cb_t: cmd in {toggle_play,prev,next,toggle_favorite,seek,
// volume_up,volume_down}; arg = seconds for seek, else ignored.
void net_backend_emit(const char *cmd, int arg);

#ifdef __cplusplus
}
#endif
