// The DK64 VR host: the OpenXR session behind RT64's VR interface (lib/rt64/src/hle/rt64_vr.h).
// Only in a DK64_VR build. docs/VR_ARCHITECTURE.md describes the frame protocol.
#pragma once

#include <cstdint>
#include <filesystem>

namespace dk64vr {

// From main(), before the renderer starts. Reads <appFolder>/vr.toml, creates the OpenXR instance
// and finds the headset; when that works, hooks plume's Vulkan creation and RT64's frame loop.
// Returns false (and leaves the flat game untouched) when there is no headset runtime.
bool hostInstall(const std::filesystem::path &appFolder);

// True once hostInstall found a headset: the game runs in VR. The desktop window is then created
// hidden (docs/VR_ARCHITECTURE.md section 4): nothing is ever presented to it, and a mapped,
// never-drawn window is what SteamVR's dashboard shows as a black desktop-game panel.
bool hostActive();

// From main(), after the renderer has shut down.
void hostShutdown();

// The game's N64 input callback, for player 1: OR-merges the VR controllers (and the
// diagnostics' autostart presses) into the real pad's input.
void hostFilterInput(int player, uint16_t *buttons, float *x, float *y);

// From the launcher's init callback (recompui's UI thread, src/main/main.cpp): loads the VR
// settings menu's document (assets/vr_menu.rml).
void hostUiInit();

// The game's Rumble Pak motor (ultramodern set_rumble), as controller haptics.
void hostSetRumble(int channel, bool on);

}
