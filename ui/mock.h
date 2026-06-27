// mock.h — fake feed so every state is visible without the bridge (SPEC §7/§8)
#pragma once

#include "now_playing_vm.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the vm to a sane "playing" default.
void mock_init(now_playing_vm_t *vm);

// Advance one frame: animates `level`, advances position, and rotates through
// all six states (playing, paused, buffering, no-track, ad, disconnected) so
// the whole screen is exercised. Call ~30x/sec; `dt_ms` is the frame delta.
void mock_tick(now_playing_vm_t *vm, uint32_t dt_ms);

#ifdef __cplusplus
}
#endif
