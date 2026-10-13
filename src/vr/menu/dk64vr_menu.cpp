#include "dk64vr_menu.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "dk64vr_input.h"

namespace dk64vr::menu {

// ---------------------------------------------------------------------------------------------
// Settings on a dk64vr_settings_t

bool StructSettings::has(const char *key) const {
    return dk64vr_settings_index(key) >= 0;
}

double StructSettings::get(const char *key) const {
    return dk64vr_settings_get(s, key);
}

double StructSettings::def(const char *key) const {
    const int i = dk64vr_settings_index(key);
    return (i >= 0) ? dk64vr_settings_default_for_profile(s, key) : 0.0;
}

double StructSettings::lo(const char *key) const {
    const int i = dk64vr_settings_index(key);
    return (i >= 0) ? dk64vr_settings_key(size_t(i))->min : 0.0;
}

double StructSettings::hi(const char *key) const {
    const int i = dk64vr_settings_index(key);
    return (i >= 0) ? dk64vr_settings_key(size_t(i))->max : 0.0;
}

void StructSettings::set(const char *key, double value) {
    if (!has(key)) {
        return;
    }

    dk64vr_settings_set(s, key, value);
    changes.push_back({ key, dk64vr_settings_get(s, key) });
}

// ---------------------------------------------------------------------------------------------
// World size

double dkHeightFromScale(double worldScale) {
    return (worldScale > 0.0) ? (DkHeightUnits / worldScale) : 0.0;
}

double scaleFromDkHeight(double metres) {
    return (metres > 0.0) ? (DkHeightUnits / metres) : 0.0;
}

namespace {

// Donkey Kong's height in metres, smallest to giant, each distinct at the one decimal shown.
// 1.68 ("1.7 m") is the measured default (world_scale 15); the steps are finer near life size,
// where a change is felt most.
const double HeightSteps[] = { 0.5, 0.8, 1.0, 1.2, 1.4, 1.6, 1.68, 1.8, 2.0, 2.2, 2.5, 3.0, 3.5, 4.0, 5.0, 6.0 };

}

std::vector<double> dkHeightPresets(double scaleLo, double scaleHi) {
    std::vector<double> out;
    for (double h : HeightSteps) {
        const double s = scaleFromDkHeight(h);
        if ((s >= scaleLo - 1e-6) && (s <= scaleHi + 1e-6)) {
            out.push_back(h);
        }
    }

    return out;
}

double stepWorldScale(double worldScale, int direction, double scaleLo, double scaleHi) {
    const std::vector<double> presets = dkHeightPresets(scaleLo, scaleHi);
    if (presets.empty() || (direction == 0)) {
        return worldScale;
    }

    const double h = dkHeightFromScale(worldScale);
    if (direction > 0) {
        for (double p : presets) {
            if (p > h + 1e-3) {
                return scaleFromDkHeight(p);
            }
        }
    }
    else {
        for (auto it = presets.rbegin(); it != presets.rend(); ++it) {
            if (*it < h - 1e-3) {
                return scaleFromDkHeight(*it);
            }
        }
    }

    return worldScale;
}

std::string dkHeightText(double worldScale) {
    char buf[32];
    const double h = dkHeightFromScale(worldScale);
    if (h < 0.995) {
        std::snprintf(buf, sizeof(buf), "DK %.0f cm", h * 100.0);
    }
    else {
        std::snprintf(buf, sizeof(buf), "DK %.1f m", h);
    }

    return buf;
}

const char *dkHeightCompare(double worldScale) {
    const double h = dkHeightFromScale(worldScale);
    if (h < 1.0) {
        return "Small: half your height or less";
    }
    if (h < 1.45) {
        return "Shorter than you";
    }
    if (h < 1.95) {
        return "About as tall as you";
    }
    if (h < 2.9) {
        return "Taller than you";
    }

    return "Giant";
}

// ---------------------------------------------------------------------------------------------
// Rows

namespace {

enum class Kind { Seg, Toggle, Slider, Stepper, Height, Info, Action, Map };
enum class Fmt { None, Pct, PctFlat, Metres, DegPerSec, Blink, Times1, Times2, Percent0, Offset };
enum class When { Always, TurnOn, TurnSnap, TurnSmooth, Vignette, Hud, Cinema, FovOn, ResManual, Roomscale };
enum class Virt { None, MoveRef, ResAuto };

struct Opt {
    double v;
    const char *label;
    const char *tile;  // shorter label for a quick-menu tile (nullptr: the same)
};

const char *TabNames[] = { "Comfort", "Controls", "HUD &amp; menus", "Display &amp; performance", "Room" };

// PromptFont glyphs (recompinput/promptfont.h), Xbox names as the Frame controllers use them.
const char *const GA = "\xe2\x87\x93";     // ⇓ A
const char *const GB = "\xe2\x87\x92";     // ⇒ B
const char *const GX = "\xe2\x87\x90";     // ⇐ X
const char *const GY = "\xe2\x87\x91";     // ⇑ Y
const char *const GLB = "\xe2\x86\x98";    // ↘
const char *const GRB = "\xe2\x86\x99";    // ↙
const char *const GLT = "\xe2\x86\x96";    // ↖
const char *const GRT = "\xe2\x86\x97";    // ↗
const char *const GVIEW = "\xe2\x87\xba";  // ⇺
const char *const GMENU = "\xe2\x87\xbb";  // ⇻
const char *const GDUD = "\xe2\x86\xa3";   // ↣
const char *const GDLR = "\xe2\x86\xa2";   // ↢
const char *const GLS = "\xe2\x87\x8b";    // ⇋
const char *const GRSUD = "\xe2\x87\xb5";  // ⇵
const char *const GRSLR = "\xe2\x87\x86";  // ⇆

std::string fmtNumber(const char *f, double v) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), f, v);
    return buf;
}

}

struct Model::Row {
    const char *id;
    const char *key;  // the settings key (for virtual rows the main one); nullptr for none
    Tab tab;
    const char *label;
    Kind kind;
    std::vector<Opt> opts;
    double lo, hi, step;
    Fmt fmt;
    const char *desc;
    When when;
    bool coming;
    Virt virt;
    const char *tileSub;  // the quick tile's line under the control (nullptr: none)
};

namespace {

using Row = Model::Row;

std::vector<Row> buildRows() {
    using K = Kind;
    using F = Fmt;
    using W = When;
    const Tab C = Tab::Comfort, Ct = Tab::Controls, H = Tab::Hud, D = Tab::Display, R = Tab::Room;
    std::vector<Row> rows = {
        { "defaults_profile", "defaults_profile", C, "Default profile", K::Seg, { { 0, "Standard", nullptr }, { 1, "Frame personal", "Frame" } }, 0, 0, 0, F::None,
          "Choose the defaults used by Reset. Saved choices stay as they are.", W::Always, false, Virt::None, nullptr },
        { "turn_mode", "turn_mode", C, "Turning", K::Seg, { { 0, "Snap", nullptr }, { 1, "Smooth", nullptr }, { 2, "Off", nullptr } }, 0, 0, 0, F::None,
          "How the right stick turns you in third-person play. Snap is one instant step.", W::TurnOn, false, Virt::None, "Right stick, third-person play" },
        { "snap_angle", "snap_angle", C, "Snap angle", K::Seg, { { 15, "15\xc2\xb0", nullptr }, { 30, "30\xc2\xb0", nullptr }, { 45, "45\xc2\xb0", nullptr }, { 60, "60\xc2\xb0", nullptr }, { 90, "90\xc2\xb0", nullptr } }, 0, 0, 0, F::None,
          "How far one flick of the right stick turns you.", W::TurnSnap, false, Virt::None, "One flick of the right stick" },
        { "smooth_turn_speed", "smooth_turn_speed", C, "Smooth turn speed", K::Slider, {}, 30, 360, 10, F::DegPerSec,
          "Turn rate with the stick fully over.", W::TurnSmooth, false, Virt::None, nullptr },
        { "snap_blink", "snap_blink", C, "Blink after snap", K::Slider, {}, 0, 0.5, 0.05, F::Blink,
          "A short black blink after each snap. The turn is instant either way.", W::TurnSnap, false, Virt::None, nullptr },
        { "right_stick_x", "right_stick_x", C, "Right stick left / right", K::Seg, { { 0, "Comfort turn", nullptr }, { 1, "Game camera", nullptr } }, 0, 0, 0, F::None,
          "Game camera is DK64's own C-left / C-right: a smooth swing of the whole view.", W::Always, false, Virt::None, nullptr },
        { "move_ref", "move_relative", C, "Left stick walks toward", K::Seg, { { 0, "Head", nullptr }, { 1, "Left hand", nullptr }, { 2, "Right hand", nullptr }, { 3, "Camera", nullptr } }, 0, 0, 0, F::None,
          "Push forward and DK walks where this points.", W::Always, false, Virt::MoveRef, nullptr },
        { "world_size", "world_scale", C, "World size", K::Height, {}, 0, 0, 0, F::None,
          "How tall Donkey Kong stands. You are about 1.7 m.", W::Always, false, Virt::None, nullptr },
        { "camera_lower", "camera_lower", C, "Lower camera", K::Slider, {}, -1, 1, 0.05, F::Pct,
          "Positive moves lower; negative values are saved while the native camera stays in place until collision checks support extension.", W::Always, false, Virt::None, nullptr },
        { "camera_closer", "camera_closer", C, "Camera closer", K::Slider, {}, -1, 0.8, 0.05, F::Pct,
          "Positive moves closer; negative values are saved while the native camera stays in place until collision checks support extension.", W::Always, false, Virt::None, nullptr },
        { "camera_smoothing", "camera_smoothing", C, "Camera smoothing", K::Seg, { { 0, "Off", nullptr }, { 1, "Low", nullptr }, { 2, "Medium", "Med" }, { 3, "High", nullptr } }, 0, 0, 0, F::None,
          "Calms the game camera's follow motion so the world drifts less.", W::Always, false, Virt::None, "Eases the game camera's swings" },
        { "camera_auto_rotate", "camera_auto_rotate", C, "Automatic camera turning", K::Toggle, {}, 0, 0, 0, F::None,
          "Off: the camera turns only when you turn it. It still keeps out of walls.", W::Always, false, Virt::None, nullptr },
        { "level_pitch", "level_pitch", C, "Level horizon", K::Toggle, {}, 0, 0, 0, F::None,
          "Drops the game camera's pitch and roll. Your head does the looking up and down.", W::Always, false, Virt::None, nullptr },
        { "vignette", "vignette", C, "Comfort vignette", K::Toggle, {}, 0, 0, 0, F::None,
          "Narrows your view while the camera moves on its own.", W::Always, true, Virt::None, nullptr },
        { "vignette_strength", "vignette_strength", C, "Vignette strength", K::Slider, {}, 0, 1, 0.05, F::Pct,
          "How far the edges close in.", W::Vignette, true, Virt::None, nullptr },

        { "dominant_hand", "dominant_hand", Ct, "Pointer hand", K::Seg, { { 0, "Right", nullptr }, { 1, "Left", nullptr } }, 0, 0, 0, F::None,
          "The menu laser comes from this hand.", W::Always, false, Virt::None, nullptr },
        { "haptics", "haptics", Ct, "Haptics strength", K::Slider, {}, 0, 1, 0.05, F::Pct,
          "Scales Rumble Pak pulses, menu clicks and the recentre buzz.", W::Always, false, Virt::None, nullptr },
        { "_map", nullptr, Ct, "Controller map", K::Map, {}, 0, 0, 0, F::None,
          "What every button does in play.", W::Always, false, Virt::None, nullptr },

        { "hud_visible", "hud_visible", H, "Show HUD", K::Toggle, {}, 0, 0, 0, F::None,
          "Bananas, coins and melons on a panel that follows your gaze.", W::Always, false, Virt::None, nullptr },
        { "hud_distance", "hud_distance", H, "HUD distance", K::Stepper, {}, 0.8, 20, 0.1, F::Metres,
          "Nearer than the world so the HUD never sinks into a wall.", W::Hud, false, Virt::None, "Counters and speech" },
        { "hud_scale", "hud_scale", H, "HUD size", K::Stepper, {}, 50, 200, 10, F::Percent0,
          "100% is 40\xc2\xb0 wide.", W::Hud, false, Virt::None, nullptr },
        { "cutscenes", "cutscenes", H, "Cutscenes", K::Seg, { { 0, "3D cinema", nullptr }, { 1, "Immersive", nullptr }, { 2, "Flat", nullptr } }, 0, 0, 0, F::None,
          "Immersive puts your head in the game camera. 3D cinema is the comfortable default.", W::Always, false, Virt::None, nullptr },
        { "cinema_distance", "cinema_distance", H, "Cinema distance", K::Slider, {}, 1.5, 10, 0.1, F::Metres,
          "How far away the cinema screen sits.", W::Cinema, false, Virt::None, nullptr },
        { "cinema_width", "cinema_width", H, "Cinema width", K::Slider, {}, 1, 10, 0.1, F::Metres,
          "Width of the cinema screen.", W::Cinema, false, Virt::None, nullptr },
        { "cinema_depth", "cinema_depth", H, "Cinema depth", K::Slider, {}, 0, 1, 0.05, F::PctFlat,
          "Stereo depth behind the screen. 0 is flat.", W::Cinema, false, Virt::None, nullptr },
        { "panel_distance", "panel_distance", H, "Menu distance", K::Stepper, {}, 0.8, 20, 0.1, F::Metres,
          "Where this panel and the game's menus appear.", W::Always, false, Virt::None, nullptr },
        { "panel_width", "panel_width", H, "Menu width", K::Stepper, {}, 0.5, 10, 0.1, F::Metres,
          "Wider is easier to read, narrower is easier to take in.", W::Always, false, Virt::None, nullptr },
        { "settings_page_side", "settings_page_side", H, "Settings panel side", K::Seg, { { 0, "Centre", nullptr }, { 1, "Left", nullptr }, { 2, "Right", nullptr } }, 0, 0, 0, F::None,
          "Where the settings panel sits around you while it is open.", W::Always, false, Virt::None, nullptr },

        { "_rate", nullptr, D, "Display rate", K::Info, {}, 0, 0, 0, F::None,
          "Read only.", W::Always, false, Virt::None, nullptr },
        { "refresh_rate", "refresh_rate", D, "Refresh request", K::Seg, { { 72, "72", nullptr }, { 90, "90", nullptr }, { 120, "120", nullptr }, { 144, "144", nullptr } }, 0, 0, 0, F::None,
          "Asked for once at start-up, in Hz. The Frame usually keeps its per-app rate.", W::Always, false, Virt::None, nullptr },
        { "res_auto", "resolution_scale", D, "Automatic resolution", K::Toggle, {}, 0, 0, 0, F::None,
          "2160 \xc3\x97 2160 per eye, whatever SteamVR recommends. Applies next launch.", W::Always, false, Virt::ResAuto, nullptr },
        { "resolution_scale", "resolution_scale", D, "Resolution scale", K::Stepper, {}, 0.75, 1.5, 0.05, F::Times2,
          "Eye image size against the runtime's recommendation. Applies next launch.", W::ResManual, false, Virt::None, nullptr },
        { "foveation", "foveation", D, "Foveated rendering", K::Seg, { { 0, "Off", nullptr }, { 1, "Low", nullptr }, { 2, "Medium", "Med" }, { 3, "High", nullptr } }, 0, 0, 0, F::None,
          "Draws the edge of each eye at lower detail. High saves the most GPU time.", W::Always, false, Virt::None, "Less detail at the edge of view" },
        { "foveation_eye_tracked", "foveation_eye_tracked", D, "Follow your eyes", K::Toggle, {}, 0, 0, 0, F::None,
          "Moves the sharp area to where you look. Without it the sharp area stays at the lens centre.", W::FovOn, false, Virt::None, nullptr },

        { "_recentre", nullptr, R, "Recentre", K::Action, {}, 0, 0, 0, F::None,
          "Or hold View for 1 s, anywhere.", W::Always, false, Virt::None, nullptr },
        { "head_translation", "head_translation", R, "Head movement", K::Slider, {}, 0, 2, 0.1, F::Times1,
          "How far the view follows you when you lean. 0 holds it still.", W::Always, false, Virt::None, nullptr },
        { "roomscale", "roomscale", R, "Roomscale", K::Toggle, {}, 0, 0, 0, F::None,
          "Walk around your play space.", W::Always, true, Virt::None, nullptr },
        { "lean_limit", "lean_limit", R, "Lean limit", K::Slider, {}, 0.15, 0.5, 0.05, F::Metres,
          "How far you can lean before DK moves.", W::Roomscale, true, Virt::None, nullptr },
        { "height_offset", "height_offset", R, "Height offset", K::Slider, {}, -1, 1, 0.05, F::Offset,
          "Raise or lower your eye height.", W::Always, true, Virt::None, nullptr },
        { "seated", "seated", R, "Seated mode", K::Toggle, {}, 0, 0, 0, F::None,
          "Adds a standing height while you sit.", W::Always, true, Virt::None, nullptr },
    };
    return rows;
}

const std::vector<Row> &allRows() {
    static const std::vector<Row> rows = buildRows();
    return rows;
}

// The quick menu's tiles, in reading order (approved layout C plus World size).
const char *const QuickTiles[] = { "turn_mode", "snap_angle", "world_size", "camera_smoothing", "hud_distance", "foveation" };

const char *whyText(When w) {
    switch (w) {
        case When::TurnOn: return "Right stick is the game camera.";
        case When::TurnSnap: return "Only with Snap turning.";
        case When::TurnSmooth: return "Only with Smooth turning.";
        case When::Vignette: return "Turn on the comfort vignette first.";
        case When::Hud: return "The HUD is hidden.";
        case When::Cinema: return "Only with 3D cinema.";
        case When::FovOn: return "Foveated rendering is off.";
        case When::ResManual: return "Automatic resolution is on.";
        case When::Roomscale: return "Turn on roomscale first.";
        default: return "";
    }
}

std::string fmtValue(Fmt f, double v) {
    switch (f) {
        case Fmt::Pct: return fmtNumber("%.0f%%", v * 100.0);
        case Fmt::PctFlat: return (v < 0.001) ? std::string("Flat") : fmtNumber("%.0f%%", v * 100.0);
        case Fmt::Metres: return fmtNumber("%.1f m", v);
        case Fmt::DegPerSec: return fmtNumber("%.0f\xc2\xb0/s", v);
        case Fmt::Blink: return (v < 0.001) ? std::string("Off") : fmtNumber("%.2f s", v);
        case Fmt::Times1: return fmtNumber("%.1f\xc3\x97", v);
        case Fmt::Times2: return fmtNumber("%.2f\xc3\x97", v);
        case Fmt::Percent0: return fmtNumber("%.0f%%", v);
        case Fmt::Offset: return (v > 0.001 ? std::string("+") : std::string()) + fmtNumber("%.2f m", v);
        default: return fmtNumber("%g", v);
    }
}

bool startsWith(const std::string &s, const char *prefix) {
    return s.compare(0, std::strlen(prefix), prefix) == 0;
}

// The row id inside an act string such as "set:turn_mode:1" or "row:turn_mode".
std::string actRow(const std::string &act) {
    const size_t a = act.find(':');
    if (a == std::string::npos) {
        return "";
    }

    const size_t b = act.find(':', a + 1);
    return act.substr(a + 1, (b == std::string::npos) ? std::string::npos : (b - a - 1));
}

int actArg(const std::string &act) {
    const size_t b = act.rfind(':');
    return (b == std::string::npos) ? 0 : std::atoi(act.c_str() + b + 1);
}

}

// ---------------------------------------------------------------------------------------------
// Model

Model::Model(Settings &settings, const Facts &facts) : settings(settings), facts(facts) {}

const Model::Row *Model::row(const std::string &id) const {
    for (const Row &r : allRows()) {
        if (id == r.id) {
            return &r;
        }
    }

    return nullptr;
}

bool Model::rowShown(const Row &r) const {
    return (r.key == nullptr) || settings.has(r.key);
}

bool Model::rowEnabled(const Row &r) const {
    if (r.coming) {
        return false;
    }

    const auto g = [&](const char *k) { return settings.get(k); };
    switch (r.when) {
        case When::TurnOn: return g("right_stick_x") == 0.0;
        case When::TurnSnap: return (g("right_stick_x") == 0.0) && (int(g("turn_mode")) == 0);
        case When::TurnSmooth: return (g("right_stick_x") == 0.0) && (int(g("turn_mode")) == 1);
        case When::Vignette: return g("vignette") != 0.0;
        case When::Hud: return g("hud_visible") != 0.0;
        case When::Cinema: return int(g("cutscenes")) == 0;
        case When::FovOn: return int(g("foveation")) != 0;
        case When::ResManual: return g("resolution_scale") != DK64VR_RESOLUTION_AUTO;
        case When::Roomscale: return g("roomscale") != 0.0;
        default: return true;
    }
}

bool Model::rowFocusable(const Row &r) const {
    return rowShown(r) && rowEnabled(r) && (r.kind != Kind::Info) && (r.kind != Kind::Map);
}

double Model::rowValue(const Row &r) const {
    switch (r.virt) {
        case Virt::MoveRef: return (settings.get("move_from_view") == 0.0) ? 3.0 : settings.get("move_relative");
        case Virt::ResAuto: return (settings.get("resolution_scale") == DK64VR_RESOLUTION_AUTO) ? 1.0 : 0.0;
        default: return (r.key != nullptr) ? settings.get(r.key) : 0.0;
    }
}

void Model::setRow(const Row &r, double v) {
    switch (r.virt) {
        case Virt::MoveRef:
            if (v >= 3.0) {
                settings.set("move_from_view", 0.0);
            }
            else {
                settings.set("move_from_view", 1.0);
                settings.set("move_relative", v);
            }
            break;
        case Virt::ResAuto: {
            if (v != 0.0) {
                settings.set("resolution_scale", DK64VR_RESOLUTION_AUTO);
            }
            else {
                // The manual scale that keeps today's eye size: 2160 against the recommendation.
                double scale = (facts.recommendedEye > 0) ? (2160.0 / double(facts.recommendedEye)) : 1.0;
                scale = std::clamp(std::round(scale / 0.05) * 0.05, 0.75, 1.5);
                settings.set("resolution_scale", scale);
            }
            break;
        }
        default:
            if (r.key != nullptr) {
                settings.set(r.key, v);
            }
            break;
    }

    dirty = true;
}

void Model::adjust(const Row &r, int direction) {
    if (!rowEnabled(r) || !rowShown(r)) {
        return;
    }

    const double v = rowValue(r);
    switch (r.kind) {
        case Kind::Seg: {
            int best = 0;
            for (size_t i = 0; i < r.opts.size(); i++) {
                if (std::fabs(r.opts[i].v - v) < std::fabs(r.opts[best].v - v)) {
                    best = int(i);
                }
            }

            const int next = std::clamp(best + direction, 0, int(r.opts.size()) - 1);
            if ((next != best) || (std::fabs(r.opts[best].v - v) > 1e-9)) {
                setRow(r, r.opts[next].v);
            }
            break;
        }
        case Kind::Toggle:
            setRow(r, (direction > 0) ? 1.0 : 0.0);
            break;
        case Kind::Slider:
        case Kind::Stepper: {
            const double lo = std::max(r.lo, settings.lo(r.key)), hi = std::min(r.hi, settings.hi(r.key));
            double next = std::round((v - r.lo) / r.step + direction) * r.step + r.lo;
            next = std::clamp(next, lo, hi);
            if (std::fabs(next - v) > 1e-9) {
                setRow(r, next);
            }
            break;
        }
        case Kind::Height: {
            const double next = stepWorldScale(v, direction, settings.lo(r.key), settings.hi(r.key));
            if (std::fabs(next - v) > 1e-9) {
                setRow(r, next);
            }
            break;
        }
        default:
            break;
    }
}

std::vector<std::string> Model::visibleRows() const {
    std::vector<std::string> out;
    if (scr == Screen::Quick) {
        for (const char *id : QuickTiles) {
            const Row *r = row(id);
            if ((r != nullptr) && rowShown(*r)) {
                out.push_back(id);
            }
        }

        return out;
    }

    for (const Row &r : allRows()) {
        if ((r.tab == curTab) && rowShown(r)) {
            out.push_back(r.id);
        }
    }

    return out;
}

std::vector<std::string> Model::stops() const {
    std::vector<std::string> out;
    if (scr == Screen::Quick) {
        out.push_back("recentre");
        for (const std::string &id : visibleRows()) {
            const Row *r = row(id);
            if ((r != nullptr) && rowFocusable(*r)) {
                out.push_back("row:" + id);
            }
        }

        out.push_back("all");
        return out;
    }

    out.push_back("def");
    for (const std::string &id : visibleRows()) {
        const Row *r = row(id);
        if ((r != nullptr) && rowFocusable(*r)) {
            out.push_back("row:" + id);
        }
    }

    return out;
}

std::string Model::focusedAct() const {
    const std::vector<std::string> s = stops();
    return ((focus >= 0) && (focus < int(s.size()))) ? s[size_t(focus)] : std::string();
}

void Model::setFocusTo(const std::string &stop) {
    const std::vector<std::string> s = stops();
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == stop) {
            if (focus != int(i)) {
                focus = int(i);
                dirty = true;
            }
            return;
        }
    }
}

void Model::moveFocus(int direction) {
    const int count = int(stops().size());
    if (count == 0) {
        focus = -1;
        return;
    }

    const int next = std::clamp(focus + direction, 0, count - 1);
    if (next != focus) {
        focus = next;
        ensureFocusVisible = true;
        dirty = true;
    }
}

void Model::open(double now, uint32_t navHeld) {
    nowS = now;
    scr = Screen::Quick;
    curTab = Tab::Comfort;
    pad = true;
    hover.clear();
    dragKey.clear();
    armedTab = -1;
    toast.clear();
    navPrev = navHeld;  // held buttons act only after their release
    navRepeatBit = 0;
    scroll = 0.0f;
    focus = 0;
    setFocusTo("row:turn_mode");
    if (focusedAct() != "row:turn_mode") {
        focus = std::min(1, int(stops().size()) - 1);
    }
    dirty = true;
}

void Model::showFull(Tab tab) {
    scr = Screen::Full;
    curTab = tab;
    scroll = 0.0f;
    armedTab = -1;
    focus = (stops().size() > 1) ? 1 : 0;
    ensureFocusVisible = true;
    dirty = true;
}

void Model::showQuick() {
    scr = Screen::Quick;
    armedTab = -1;
    scroll = 0.0f;
    focus = 0;
    setFocusTo("all");
    dirty = true;
}

std::vector<std::string> Model::tabKeys(Tab tab) const {
    std::vector<std::string> keys;
    const auto add = [&](const char *k) {
        if ((k != nullptr) && settings.has(k) && (std::find(keys.begin(), keys.end(), k) == keys.end())) {
            keys.push_back(k);
        }
    };
    for (const Row &r : allRows()) {
        if (r.tab != tab) {
            continue;
        }

        if (r.virt == Virt::MoveRef) {
            add("move_from_view");
        }

        if ((r.key == nullptr) || (std::strcmp(r.key, "defaults_profile") != 0)) {
            add(r.key);
        }
    }

    return keys;
}

void Model::defaults(Tab tab, double now) {
    if ((armedTab == int(tab)) && (now < armedUntilS)) {
        for (const std::string &k : tabKeys(tab)) {
            settings.set(k.c_str(), settings.def(k.c_str()));
        }

        armedTab = -1;
        toast = std::string(TabNames[int(tab)]) + " set back to defaults";
        toastUntilS = now + 2.2;
    }
    else {
        armedTab = int(tab);
        armedUntilS = now + 3.0;
    }

    dirty = true;
}

Effects Model::act(const std::string &id, double now) {
    Effects e;
    nowS = now;
    activate(id, now, e);
    return e;
}

void Model::activate(const std::string &stop, double now, Effects &e) {
    if (stop == "close") {
        e.close = true;
        return;
    }

    if (stop == "all") {
        showFull(Tab::Comfort);
        return;
    }

    if (stop == "recentre") {
        e.recentre = true;
        toast = "Recentred";
        toastUntilS = now + 1.6;
        dirty = true;
        return;
    }

    if (stop == "def") {
        defaults(curTab, now);
        return;
    }

    if (startsWith(stop, "tab:")) {
        const int t = actArg(stop);
        if ((t >= 0) && (t < int(Tab::Count)) && ((scr != Screen::Full) || (t != int(curTab)))) {
            showFull(Tab(t));
        }
        return;
    }

    const std::string id = actRow(stop);
    const Row *r = row(id);
    if ((r == nullptr) || !rowShown(*r) || !rowEnabled(*r)) {
        return;
    }

    if (startsWith(stop, "set:")) {
        const int i = actArg(stop);
        if ((i >= 0) && (i < int(r->opts.size()))) {
            setRow(*r, r->opts[size_t(i)].v);
        }
    }
    else if (startsWith(stop, "tog:")) {
        setRow(*r, (rowValue(*r) != 0.0) ? 0.0 : 1.0);
    }
    else if (startsWith(stop, "step:")) {
        adjust(*r, (actArg(stop) < 0) ? -1 : 1);
    }
    else if (startsWith(stop, "row:")) {
        // A on a focused row: segments cycle, toggles flip, the recentre row recentres.
        if (r->kind == Kind::Seg) {
            const double v = rowValue(*r);
            size_t i = 0;
            for (size_t k = 0; k < r->opts.size(); k++) {
                if (std::fabs(r->opts[k].v - v) < 1e-6) {
                    i = k;
                }
            }

            setRow(*r, r->opts[(i + 1) % r->opts.size()].v);
        }
        else if (r->kind == Kind::Toggle) {
            setRow(*r, (rowValue(*r) != 0.0) ? 0.0 : 1.0);
        }
        else if (r->kind == Kind::Action) {
            activate("recentre", now, e);
        }
    }
}

void Model::dragTo(float x) {
    const Row *r = row(dragKey);
    if ((r == nullptr) || (dragWidth <= 1.0f) || !rowEnabled(*r)) {
        return;
    }

    const double f = std::clamp(double(x - dragLeft) / double(dragWidth), 0.0, 1.0);
    const double lo = std::max(r->lo, settings.lo(r->key)), hi = std::min(r->hi, settings.hi(r->key));
    double v = r->lo + f * (r->hi - r->lo);
    v = std::clamp(std::round((v - r->lo) / r->step) * r->step + r->lo, lo, hi);
    if (std::fabs(v - rowValue(*r)) > 1e-9) {
        setRow(*r, v);
    }
}

Effects Model::step(const Input &in, const Pointer &p, const Layout &layout) {
    Effects e;
    const double dt = std::clamp(in.nowS - nowS, 0.0, 0.1);
    nowS = in.nowS;

    if ((armedTab >= 0) && (nowS >= armedUntilS)) {
        armedTab = -1;
        dirty = true;
    }

    if (!toast.empty() && (nowS >= toastUntilS)) {
        toast.clear();
        dirty = true;
    }

    // Focus may have gone (a row was disabled by a change): keep it in range.
    const int stopCount = int(stops().size());
    if (focus >= stopCount) {
        focus = stopCount - 1;
        dirty = true;
    }

    // Pad: edges, with repeats for held directions (steppers and sliders step on).
    const uint32_t held = in.navHeld;
    uint32_t pressed = held & ~navPrev;
    const uint32_t dirs = nav::Up | nav::Down | nav::Left | nav::Right;
    if (pressed & dirs) {
        navRepeatBit = pressed & dirs & (~(pressed & dirs) + 1);  // the lowest new direction
        navRepeatAtS = nowS + 0.4;
    }
    else if (navRepeatBit && (held & navRepeatBit)) {
        if (nowS >= navRepeatAtS) {
            pressed |= navRepeatBit;
            navRepeatAtS = nowS + 0.09;
        }
    }
    else {
        navRepeatBit = 0;
    }

    navPrev = held;
    if (pressed != 0) {
        if (!pad) {
            pad = true;
            dirty = true;
        }

        const std::vector<std::string> s = stops();
        const std::string cur = ((focus >= 0) && (focus < int(s.size()))) ? s[size_t(focus)] : std::string();
        const Row *r = startsWith(cur, "row:") ? row(actRow(cur)) : nullptr;
        if (pressed & nav::Up) {
            moveFocus(-1);
        }
        if (pressed & nav::Down) {
            moveFocus(1);
        }
        if ((pressed & nav::Left) && (r != nullptr)) {
            adjust(*r, -1);
        }
        if ((pressed & nav::Right) && (r != nullptr)) {
            adjust(*r, 1);
        }
        if (pressed & nav::Accept) {
            activate(cur, nowS, e);
        }
        if (pressed & (nav::Back | nav::ToggleMenu)) {
            if (scr == Screen::Full) {
                showQuick();
            }
            else {
                e.close = true;
            }
        }
        if ((pressed & (nav::TabLeft | nav::TabRight)) && (scr == Screen::Full)) {
            const int n = int(Tab::Count);
            const int t = (int(curTab) + ((pressed & nav::TabRight) ? 1 : (n - 1))) % n;
            showFull(Tab(t));
        }
    }

    // Right stick Y scrolls the list.
    if (layout.valid) {
        maxScrollKnown = std::max(0.0f, layout.contentHeight - layout.viewHeight);
    }

    if ((scr == Screen::Full) && (std::fabs(in.scroll) > 0.3f)) {
        const float next = std::clamp(scroll - float(in.scroll) * 900.0f * float(dt), 0.0f, maxScrollKnown);
        if (next != scroll) {
            scroll = next;
            dirty = true;
        }
    }

    // Pointer.
    const std::string newHover = p.on ? p.act : std::string();
    if (newHover != hover) {
        if (!newHover.empty()) {
            e.hover = true;
        }

        hover = newHover;
        dirty = true;
    }

    if (p.on && pad && (!p.act.empty() || p.down)) {
        pad = false;  // the pointer took over: no focus ring until the pad is used again
        dirty = true;
    }

    if (p.down && !pointerWasDown && p.on) {
        if (startsWith(p.act, "slider:")) {
            const Row *r = row(actRow(p.act));
            if ((r != nullptr) && rowEnabled(*r)) {
                dragKey = r->id;
                dragLeft = p.trackLeft;
                dragWidth = p.trackWidth;
                dragTo(p.x);
                setFocusTo(std::string("row:") + r->id);
            }
        }
        else if (!p.act.empty()) {
            const std::string id = actRow(p.act);
            if (!id.empty() && !startsWith(p.act, "tab:")) {
                setFocusTo("row:" + id);
            }

            activate(p.act, nowS, e);
        }
    }
    else if (p.down && !dragKey.empty()) {
        dragTo(p.x);
    }

    if (!p.down) {
        dragKey.clear();
    }

    pointerWasDown = p.down;

    // Keep the scroll in range and the pad focus in view.
    // The layout read back is the last render's: it matches the model only if nothing changed
    // since, this step included (scrolling a moved focus into view waits for the render that
    // shows it).
    const bool layoutCurrent = !dirty;
    if (scr == Screen::Full && layout.valid && layoutCurrent) {
        const float maxScroll = std::max(0.0f, layout.contentHeight - layout.viewHeight);
        if (ensureFocusVisible && layout.focusValid && pad) {
            const float margin = 16.0f;
            if (layout.focusTop - margin < scroll) {
                scroll = std::max(0.0f, layout.focusTop - margin);
                dirty = true;
            }
            else if (layout.focusBottom + margin > scroll + layout.viewHeight) {
                scroll = layout.focusBottom + margin - layout.viewHeight;
                dirty = true;
            }
            if (focus <= 1) {
                scroll = 0.0f;
            }
        }

        ensureFocusVisible = false;
        const float clamped = std::clamp(scroll, 0.0f, maxScroll);
        if (clamped != scroll) {
            scroll = clamped;
            dirty = true;
        }
    }
    else if (scr == Screen::Quick) {
        scroll = 0.0f;
    }

    return e;
}

std::string Model::valueText(const std::string &rowId) const {
    const Row *r = row(rowId);
    if (r == nullptr) {
        return "";
    }

    const double v = rowValue(*r);
    switch (r->kind) {
        case Kind::Seg:
            for (const Opt &o : r->opts) {
                if (std::fabs(o.v - v) < 1e-6) {
                    return o.label;
                }
            }
            return fmtValue(Fmt::None, v);
        case Kind::Toggle:
            return (v != 0.0) ? "On" : "Off";
        case Kind::Height:
            return dkHeightText(v);
        case Kind::Stepper:
            if (r->virt == Virt::None && std::strcmp(r->id, "resolution_scale") == 0 && v == DK64VR_RESOLUTION_AUTO) {
                return "Auto";
            }
            return fmtValue(r->fmt, v);
        case Kind::Slider:
            return fmtValue(r->fmt, v);
        case Kind::Info: {
            char buf[32];
            if (facts.displayHz > 0.0) {
                std::snprintf(buf, sizeof(buf), "%.1f Hz", facts.displayHz);
            }
            else {
                std::snprintf(buf, sizeof(buf), "--");
            }
            return buf;
        }
        default:
            return "";
    }
}

// ---------------------------------------------------------------------------------------------
// RML (assets/vr_menu.rcss). The pointer finds controls by data-act; the focused stop carries
// a ring and, on the full panel, id="focus" so the UI layer can keep it scrolled into view.

std::string Model::cls(const std::string &stop, const char *base) const {
    std::string c = base;
    if (!hover.empty() && hover == stop) {
        c += " hov";
    }

    if ((dragKey.size() > 0) && (stop == std::string("slider:") + dragKey)) {
        c += " drag";
    }

    return c;
}

namespace {

std::string attrAct(const std::string &act, bool enabled) {
    return enabled ? (" data-act=\"" + act + "\"") : std::string();
}

std::string hint(const std::vector<const char *> &glyphs, const char *text) {
    std::string out = "<div class=\"p-hint\">";
    for (const char *g : glyphs) {
        out += std::string("<span class=\"g\">") + g + "</span>";
    }

    return out + "<span class=\"ht\">" + text + "</span></div>";
}

const char *Ring = "<div class=\"ring\"></div>";
const char *CloseX = "<div class=\"xl xa\"></div><div class=\"xl xb\"></div>";

}

std::string Model::renderRowControl(const Row &r, bool tile) const {
    const bool en = rowEnabled(r) && rowShown(r);
    const double v = rowValue(r);
    std::string out;
    switch (r.kind) {
        case Kind::Seg: {
            const size_t count = (tile && (std::strcmp(r.id, "turn_mode") == 0)) ? 2 : r.opts.size();  // the tile offers Snap and Smooth
            out += std::string("<div class=\"p-seg") + (tile ? " fill" : "") + " n" + std::to_string(count) + "\">";
            for (size_t i = 0; i < count; i++) {
                const Opt &o = r.opts[i];
                const std::string a = "set:" + std::string(r.id) + ":" + std::to_string(i);
                std::string c = cls(a, "sb");
                if (std::fabs(o.v - v) < 1e-6) {
                    c += " on";
                }

                out += "<div class=\"" + c + "\"" + attrAct(a, en) + ">" + ((tile && o.tile) ? o.tile : o.label) + "</div>";
            }
            out += "</div>";
            break;
        }
        case Kind::Toggle: {
            const std::string a = "tog:" + std::string(r.id);
            std::string c = cls(a, "p-tog");
            if (v != 0.0) {
                c += " on";
            }

            out += "<div class=\"" + c + "\"" + attrAct(a, en) + "><div class=\"tr\"><div class=\"kn\"></div></div><div class=\"tt\">" + (v != 0.0 ? "On" : "Off") + "</div></div>";
            break;
        }
        case Kind::Slider: {
            const double f = std::clamp((v - r.lo) / (r.hi - r.lo), 0.0, 1.0);
            const double d = std::clamp((settings.def(r.key) - r.lo) / (r.hi - r.lo), 0.0, 1.0);
            const std::string a = "slider:" + std::string(r.id);
            char pct[64];
            out += "<div class=\"p-sl\"><div class=\"" + cls(a, "trk") + "\"" + attrAct(a, en) + "><div class=\"rail\">";
            std::snprintf(pct, sizeof(pct), "<div class=\"fill\" style=\"width: %.2f%%;\"></div></div>", f * 100.0);
            out += pct;
            std::snprintf(pct, sizeof(pct), "<div class=\"notch\" style=\"left: %.2f%%;\"></div>", d * 100.0);
            out += pct;
            std::snprintf(pct, sizeof(pct), "<div class=\"knob\" style=\"left: %.2f%%;\"></div>", f * 100.0);
            out += pct;
            out += "</div><div class=\"val\">" + valueText(r.id) + "</div></div>";
            break;
        }
        case Kind::Stepper:
        case Kind::Height: {
            bool atLo, atHi;
            if (r.kind == Kind::Height) {
                atLo = std::fabs(stepWorldScale(v, -1, settings.lo(r.key), settings.hi(r.key)) - v) < 1e-9;
                atHi = std::fabs(stepWorldScale(v, 1, settings.lo(r.key), settings.hi(r.key)) - v) < 1e-9;
            }
            else {
                atLo = v <= std::max(r.lo, settings.lo(r.key)) + 1e-9;
                atHi = v >= std::min(r.hi, settings.hi(r.key)) - 1e-9;
            }

            const std::string minus = "step:" + std::string(r.id) + ":-1", plus = "step:" + std::string(r.id) + ":1";
            std::string core = std::string("<div class=\"p-step") + (tile ? " fill" : "") + "\">";
            core += "<div class=\"" + cls(minus, atLo ? "sbt off" : "sbt") + "\"" + attrAct(minus, en && !atLo) + ">\xe2\x88\x92</div>";
            core += "<div class=\"out\">" + valueText(r.id) + "</div>";
            core += "<div class=\"" + cls(plus, atHi ? "sbt off" : "sbt") + "\"" + attrAct(plus, en && !atHi) + ">+</div></div>";
            std::string extra;
            if (!tile) {
                char buf[96] = "";
                if (r.kind == Kind::Height) {
                    std::snprintf(buf, sizeof(buf), "%s", dkHeightCompare(v));
                }
                else if (std::strcmp(r.id, "hud_scale") == 0) {
                    std::snprintf(buf, sizeof(buf), "\xe2\x89\x88 %.0f\xc2\xb0 wide", 40.0 * v / 100.0);
                }
                else if (std::strcmp(r.id, "panel_width") == 0) {
                    const double dist = std::max(0.1, settings.get("panel_distance"));
                    std::snprintf(buf, sizeof(buf), "\xe2\x89\x88 %.0f\xc2\xb0 wide", 2.0 * std::atan(v / 2.0 / dist) * 180.0 / 3.14159265358979);
                }
                else if ((std::strcmp(r.id, "resolution_scale") == 0) && (facts.recommendedEye > 0) && (v > 0.0)) {
                    const int px = int(std::lround(facts.recommendedEye * v));
                    std::snprintf(buf, sizeof(buf), "%d \xc3\x97 %d per eye", px, px);
                }
                if (buf[0] != '\0') {
                    extra = std::string("<div class=\"p-extra\">") + buf + "</div>";
                }
            }
            out += tile ? core : ("<div class=\"p-stepw\">" + core + extra + "</div>");
            break;
        }
        case Kind::Info:
            out += "<div class=\"p-info\"><div class=\"iv\">" + valueText(r.id) +
                "</div><div class=\"ic\">Measured. SteamVR's per-app setting for DK64VR decides the real rate.</div></div>";
            break;
        case Kind::Action:
            out += "<div class=\"" + cls("recentre", "p-btn primary") + "\"" + attrAct("recentre", true) + ">Recentre now</div>";
            break;
        case Kind::Map: {
            static const struct { const char *g; const char *t; } map[] = {
                { GA, "Jump, confirm" }, { GX, "Attack (N64 B)" }, { GY, "Camera left (C-left)" }, { GB, "Camera right (C-right)" },
                { GLB, "Zoom (C-down)" }, { GRB, "First person (C-up)" }, { GLT, "Crouch (Z)" }, { GRT, "Centre camera (R), menu click" },
                { GMENU, "Pause (Start)" }, { GVIEW, "Recomp menu; hold 1 s to recentre" }, { GLS, "Move" }, { GRSLR, "Turn" },
                { "LGrip", "Hold with right stick: adjust size and camera" },
            };
            constexpr size_t mapCount = sizeof(map) / sizeof(map[0]);
            out += "<div class=\"p-map\"><div class=\"mc\">";
            for (size_t i = 0; i < mapCount; i++) {
                if (i == 6) {
                    out += "</div><div class=\"mc\">";
                }
                out += std::string("<div class=\"me\"><span class=\"g\">") + map[i].g + "</span><span class=\"mt\">" + map[i].t + "</span></div>";
            }
            out += std::string("</div></div><div class=\"me wide\"><span class=\"g two\">") + GMENU + GVIEW +
                "</span><span class=\"mt\">Hold Menu + View for 0.5 s: VR settings, from anywhere</span></div>";
            break;
        }
    }

    return out;
}

std::string Model::renderHints(bool full) const {
    std::string out = "<div class=\"p-hints\">";
    out += hint({ GDUD }, "Choose");
    out += hint({ GDLR }, "Change");
    out += hint({ GA }, "Select");
    if (full) {
        out += hint({ GLB, GRB }, "Tab");
        out += hint({ GRT }, "Click");
        out += hint({ GRSUD }, "Scroll");
        out += hint({ GB }, "Back to quick menu");
    }
    else {
        out += hint({ GB }, "Close");
    }

    return out + "</div>";
}

std::string Model::renderQuick() const {
    const std::string focused = pad ? focusedAct() : std::string();
    std::string out = "<div class=\"vc-win\"><div class=\"vc-top\"><div class=\"h3\">Quick VR settings</div><div class=\"chip\">Game paused</div><div class=\"sp\"></div>";
    out += "<div class=\"" + cls("recentre", "p-btn primary") + (focused == "recentre" ? " focus" : "") + "\"" + attrAct("recentre", true) + ">Recentre now" + (focused == "recentre" ? Ring : "") + "</div>";
    out += "<div class=\"" + cls("close", "p-x") + "\" data-act=\"close\">" + CloseX + "</div></div>";
    out += "<div class=\"vc-grid\">";
    const std::vector<std::string> tiles = visibleRows();
    for (size_t i = 0; i < tiles.size(); i++) {
        if ((i % 3) == 0) {
            out += (i == 0) ? "<div class=\"vc-row\">" : "</div><div class=\"vc-row r2\">";
        }

        const Row &r = *row(tiles[i]);
        const bool en = rowEnabled(r);
        const bool isFocus = focused == ("row:" + tiles[i]);
        std::string c = "vc-tile";
        c += en ? "" : " dis";
        c += isFocus ? " focus" : "";
        out += "<div class=\"" + c + "\"><div class=\"vc-tt\"><div class=\"p-label\">" + r.label + "</div>";
        if (r.kind == Kind::Seg) {
            out += "<div class=\"vc-val\">" + valueText(r.id) + "</div>";
        }
        out += "</div><div class=\"vc-ctl\">" + renderRowControl(r, true) + "</div>";
        std::string sub;
        if (!en) {
            sub = std::string("<div class=\"vc-sub why\">") + whyText(r.when) + "</div>";
        }
        else if (r.kind == Kind::Height) {
            sub = std::string("<div class=\"vc-sub\">") + dkHeightCompare(rowValue(r)) + "</div>";
        }
        else if (r.tileSub != nullptr) {
            sub = std::string("<div class=\"vc-sub\">") + r.tileSub + "</div>";
        }
        out += sub + (isFocus ? Ring : "") + "</div>";
    }
    if (!tiles.empty()) {
        out += "</div>";
    }
    out += "</div><div class=\"vc-foot\">";
    out += "<div class=\"" + cls("all", "p-btn big") + (focused == "all" ? " focus" : "") + "\" data-act=\"all\">All VR settings\xe2\x80\xa6" + (focused == "all" ? Ring : "") + "</div>";
    out += "<div class=\"sp\"></div>" + renderHints(false) + "</div></div>";
    return out;
}

std::string Model::renderFull() const {
    const std::string focused = pad ? focusedAct() : std::string();
    std::string out = "<div class=\"vb-win\"><div class=\"vb-top\"><div class=\"h3\">VR settings</div><div class=\"chip\">Game paused</div><div class=\"sp\"></div>";
    out += "<div class=\"" + cls("close", "p-x") + "\" data-act=\"close\">" + CloseX + "</div></div>";
    out += "<div class=\"vb-body\"><div class=\"vb-rail\">";
    out += std::string("<div class=\"bump\"><span class=\"g\">") + GLB + "</span><span class=\"bt\">Previous</span></div>";
    for (int t = 0; t < int(Tab::Count); t++) {
        const std::string a = "tab:" + std::to_string(t);
        std::string c = cls(a, "vb-tab");
        c += (t == int(curTab)) ? " on" : "";
        out += "<div class=\"" + c + "\" data-act=\"" + a + "\"><div class=\"mark\"></div><div class=\"tl\">" + TabNames[t] + "</div></div>";
    }
    out += std::string("<div class=\"sp\"></div><div class=\"bump\"><span class=\"g\">") + GRB + "</span><span class=\"bt\">Next</span></div></div>";

    const bool armed = armedTab == int(curTab);
    out += "<div class=\"vb-main\"><div class=\"vb-head\"><div class=\"h4\">" + std::string(TabNames[int(curTab)]) + "</div>";
    std::string dc = cls("def", "p-btn small");
    dc += armed ? " armed" : "";
    dc += (focused == "def") ? " focus" : "";
    out += "<div class=\"" + dc + "\" data-act=\"def\">" + (armed ? ("Reset " + std::string(TabNames[int(curTab)]) + "?") : std::string("Defaults")) + (focused == "def" ? Ring : "") + "</div></div>";

    char style[64];
    std::snprintf(style, sizeof(style), " style=\"margin-top: %.0fpx;\"", -scroll);  // in flow, so the list clips it
    out += std::string("<div class=\"vb-list\" id=\"list\"><div class=\"vb-in\" id=\"listin\"") + style + ">";
    for (const std::string &id : visibleRows()) {
        const Row &r = *row(id);
        const bool en = rowEnabled(r);
        const bool isFocus = focused == ("row:" + id);
        std::string c = "vb-row";
        c += en ? "" : " dis";
        c += (r.kind == Kind::Map) ? " wide" : "";
        c += isFocus ? " focus" : "";
        out += "<div class=\"" + c + "\"" + (isFocus ? " id=\"focus\"" : "") + "><div class=\"vb-text\"><div class=\"p-label\">" + r.label;
        if (r.coming) {
            out += "<span class=\"p-badge\">Coming</span>";
        }
        std::string desc = (en || r.coming) ? r.desc : whyText(r.when);
        if (en && (std::strcmp(r.id, "foveation") == 0) && !facts.foveationLive) {
            desc += " Turning it on applies next launch.";
        }
        out += std::string("</div><div class=\"p-desc") + ((en || r.coming) ? "" : " why") + "\">" + desc + "</div></div>";
        out += "<div class=\"vb-ctl\">" + renderRowControl(r, false) + "</div></div>";
    }
    out += "</div></div></div></div><div class=\"vb-foot\">" + renderHints(true) + "</div></div>";
    return out;
}

std::string Model::render(bool covering) const {
    std::string out = std::string("<div class=\"vrm") + (covering ? " covering" : "") + (pad ? " pad" : "") + "\">";
    out += (scr == Screen::Quick) ? renderQuick() : renderFull();
    if (!toast.empty()) {
        out += "<div class=\"toast\"><div class=\"tw\">" + toast + "</div></div>";
    }

    return out + "</div>";
}

}
