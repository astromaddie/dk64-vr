// The in-headset VR settings menu (docs/VR_ARCHITECTURE.md section 9a; the approved mockup's
// layouts C and B): a quick menu of big tiles opened by the Menu + View chord, and "All VR
// settings..." behind it, a standalone panel with the standard's tabs (standard.md section 7).
//
// Pure: no RmlUi, no OpenXR. The model holds the screen, tab, focus, pointer hover and drag, the
// two-press Defaults and the list scroll; step() takes one display frame of pad and pointer
// input and changes settings through the Settings interface at once. render() writes the body as
// RML for assets/vr_menu.rcss; the recompui glue (dk64vr_menu_ui.cpp) shows it on the panel and
// hit-tests the pointer against the laid-out document (data-act attributes).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "dk64vr_settings.h"

namespace dk64vr::menu {

// The settings as the menu sees them. Keys that are not registered do not exist: their rows
// hide themselves (camera_lower and camera_closer before that work lands).
class Settings {
public:
    virtual ~Settings() = default;
    virtual bool has(const char *key) const = 0;
    virtual double get(const char *key) const = 0;
    virtual double def(const char *key) const = 0;
    virtual double lo(const char *key) const = 0;
    virtual double hi(const char *key) const = 0;
    virtual void set(const char *key, double value) = 0;  // clamps as dk64vr_settings_set does
};

// Settings on a dk64vr_settings_t (the host's copy, or a test's). Every set() is also recorded,
// in order, for the host to apply and save.
class StructSettings final : public Settings {
public:
    explicit StructSettings(dk64vr_settings_t *s) : s(s) {}
    bool has(const char *key) const override;
    double get(const char *key) const override;
    double def(const char *key) const override;
    double lo(const char *key) const override;
    double hi(const char *key) const override;
    void set(const char *key, double value) override;

    struct Change {
        std::string key;
        double value;
    };
    std::vector<Change> changes;

private:
    dk64vr_settings_t *s;
};

// What the host knows that the menu shows.
struct Facts {
    double displayHz = 0.0;     // measured (1 / predictedDisplayPeriod)
    int recommendedEye = 0;     // the runtime's recommended eye width
    bool foveationLive = true;  // the shading-rate backend came up at launch (levels change live)
};

// One display frame of input.
struct Input {
    double nowS = 0.0;
    uint32_t navHeld = 0;  // dk64vr::nav bits (dk64vr_input.h)
    float scroll = 0.0f;   // right stick Y, up positive
};

// The pointer, resolved against the document by the UI layer.
struct Pointer {
    bool on = false;          // the ray hits the panel
    bool down = false;        // trigger held (hysteresis already applied)
    float x = 0.0f, y = 0.0f; // panel pixels
    std::string act;          // data-act of the element under it ("" for none)
    float trackLeft = 0.0f, trackWidth = 0.0f;  // when act is a slider track: its box
};

// Layout read back from the document as it was last drawn (for the B list's scroll).
struct Layout {
    bool valid = false;
    float viewHeight = 0.0f;     // the list's visible height
    float contentHeight = 0.0f;  // all rows
    float focusTop = 0.0f, focusBottom = 0.0f;  // the focused row, in content coordinates
    bool focusValid = false;
};

struct Effects {
    bool recentre = false;
    bool close = false;
    bool hover = false;  // the pointer moved onto a new control (ui_hover haptic)
};

// World size: Donkey Kong's model spans 25.2 game units (docs/VR_ARCHITECTURE.md section 5), so
// with world_scale game units a metre he stands 25.2 / world_scale m tall.
constexpr double DkHeightUnits = 25.2;
double dkHeightFromScale(double worldScale);
double scaleFromDkHeight(double metres);
// The World size stepper's heights in metres, ascending; only those whose world_scale lies in
// [scaleLo, scaleHi] (so the presets follow the registry's range).
std::vector<double> dkHeightPresets(double scaleLo, double scaleHi);
// One step up (+1) or down (-1) from the current world_scale, through the presets; the current
// value need not be a preset. Returns the new world_scale (unchanged at the ends).
double stepWorldScale(double worldScale, int direction, double scaleLo, double scaleHi);
// "DK 1.7 m" and the comparison with a 1.7 m player ("As tall as you", "Giant", ...).
std::string dkHeightText(double worldScale);
const char *dkHeightCompare(double worldScale);

enum class Screen { Quick, Full };

enum class Tab { Comfort, Controls, Hud, Display, Room, Count };

class Model {
public:
    struct Row;  // one setting's row (dk64vr_menu.cpp)

    Model(Settings &settings, const Facts &facts);

    // Opens the quick menu with the focus on its first tile. Buttons held now act only after
    // their release.
    void open(double nowS, uint32_t navHeld);
    void showFull(Tab tab);   // B on a tab (also from "All VR settings...")
    void showQuick();         // back to C
    void setFacts(const Facts &f) { if (f.displayHz != facts.displayHz || f.foveationLive != facts.foveationLive || f.recommendedEye != facts.recommendedEye) { facts = f; dirty = true; } }

    Effects step(const Input &in, const Pointer &pointer, const Layout &layout);

    // RML for the document body; `covering` adds a backdrop over another menu under this one.
    std::string render(bool covering) const;

    // Pad focus: index into stops(); -1 when nothing is focusable.
    int focusIndex() const { return focus; }
    std::string focusedAct() const;  // the focused stop's id (for diagnostics)
    Screen screen() const { return scr; }
    Tab tab() const { return curTab; }
    bool padMode() const { return pad; }
    float scrollPx() const { return scroll; }
    bool dirty = true;  // render() would give a different document

    // Every row the current screen shows, in order (tests). Each entry is a row id.
    std::vector<std::string> visibleRows() const;
    // The focus stops of the current screen, in pad order (ids as in data-act).
    std::vector<std::string> stops() const;
    // Runs an action as a pointer click on it would (tests and diagnostics).
    Effects act(const std::string &id, double nowS);
    // The text a row's value shows (tests).
    std::string valueText(const std::string &rowId) const;
    // The keys a tab's Defaults resets.
    std::vector<std::string> tabKeys(Tab tab) const;

private:
    const Row *row(const std::string &id) const;
    bool rowShown(const Row &r) const;
    bool rowEnabled(const Row &r) const;
    bool rowFocusable(const Row &r) const;
    double rowValue(const Row &r) const;
    void setRow(const Row &r, double v);
    void adjust(const Row &r, int direction);
    void activate(const std::string &stop, double nowS, Effects &e);
    void moveFocus(int direction);
    void defaults(Tab tab, double nowS);
    void setFocusTo(const std::string &stop);
    void dragTo(float x);

    std::string renderQuick() const;
    std::string renderFull() const;
    std::string renderRowControl(const Row &r, bool tile) const;
    std::string renderHints(bool full) const;
    std::string cls(const std::string &stop, const char *base) const;

    Settings &settings;
    Facts facts;
    Screen scr = Screen::Quick;
    Tab curTab = Tab::Comfort;
    int focus = 0;
    bool pad = true;
    float scroll = 0.0f;
    float maxScrollKnown = 0.0f;
    bool ensureFocusVisible = false;
    std::string hover;
    bool pointerWasDown = false;
    std::string dragKey;
    float dragLeft = 0.0f, dragWidth = 0.0f;
    int armedTab = -1;
    double armedUntilS = 0.0;
    std::string toast;
    double toastUntilS = 0.0;
    uint32_t navPrev = 0;
    uint32_t navRepeatBit = 0;
    double navRepeatAtS = 0.0;
    double nowS = 0.0;
};

}
