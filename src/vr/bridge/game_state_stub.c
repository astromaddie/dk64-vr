// Weak stand-ins for the game-state interface (src/vr/include/dk64vr_game_state.h), so the VR
// build links before the game side (src/game/vr_api.cpp) lands. Any strong definition from the
// game side replaces these at link time; this file can then be dropped.
#include <string.h>

#include "dk64vr_game_state.h"

static volatile int stub_active = 0;

__attribute__((weak)) void dk64vr_get_game_state(dk64vr_game_state *out) {
    memset(out, 0, sizeof(*out));
}

__attribute__((weak)) void dk64vr_publish_game_state(const dk64vr_game_state *state) {
    (void)state;
}

__attribute__((weak)) int dk64vr_is_active(void) {
    return stub_active;
}

__attribute__((weak)) void dk64vr_set_active(int active) {
    stub_active = active;
}
