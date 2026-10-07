// The VR settings menu on recompui (DK64_VR builds only): one recompui context holding
// assets/vr_menu.rml, whose body the menu model (dk64vr_menu.h) writes. It is drawn on the VR
// panel like the recomp menus, but takes its input from the VR host directly (pad navigation and
// the pointer, hit-tested against the laid-out document), not through SDL events.
//
// Threads: uiCreate runs on the UI thread (the launcher's init callback, src/main/main.cpp).
// Everything else is called from the VR host's frame thread; the model itself only ever runs on
// the UI thread, inside recompui's context update (VrMenuRoot::process_event), so it never races
// the document.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "dk64vr_menu.h"
#include "dk64vr_settings.h"

namespace dk64vr::menu {

// One display frame from the host.
struct FrameIn {
    double nowS = 0.0;
    uint32_t navHeld = 0;      // dk64vr::nav bits
    float scroll = 0.0f;       // right stick Y
    bool pointerOn = false;    // the ray hits the panel
    bool pointerDown = false;  // trigger (hysteresis applied)
    float px = 0.0f, py = 0.0f;  // panel pixels
};

// What the menu did since the last uiTake().
struct FrameOut {
    std::vector<StructSettings::Change> changes;  // in order; the host applies and saves them
    bool recentre = false;
    bool close = false;
    bool hover = false;
    bool rendered = false;     // a new document was drawn
    Screen screen = Screen::Quick;
    Tab tab = Tab::Comfort;
    std::string focus;         // the focused stop
    bool locateValid = false;  // the element uiLocate asked for, centre in panel pixels
    float locateX = 0.0f, locateY = 0.0f;
};

void uiCreate();
bool uiReady();

// Opens the quick menu on a copy of the host's settings. False when the document is missing.
bool uiOpen(const dk64vr_settings_t &settings, const Facts &facts, double nowS, uint32_t navHeld);
void uiClose();
void uiRefreshSettings(const dk64vr_settings_t &settings);
bool uiIsOpen();
void uiFrame(const FrameIn &in, const Facts &facts);
FrameOut uiTake();

// Diagnostics: run an action as a click would ("all", "tab:2", ...), and report where the
// element with data-act `act` is (FrameOut::locate*).
void uiAct(const std::string &act);
void uiLocate(const std::string &act);

}
