// The Steam Frame controllers as DK64's N64 pad, plus the system layer and menu navigation
// (standard.md sections 1 and 2, lesson 78). Pure: no OpenXR calls, no game state. The host
// samples the actions once per display frame and feeds them here; the result is OR-merged into
// the game's own N64 input (never key injection) and handed to the recompui menus.
//
// The map starts from DK64 Recompiled's own gamepad defaults (src/game/config.cpp
// set_control_defaults over recompinput's defaults), with the Frame read as an Xbox-layout pad:
//
//   A       N64 A (jump, confirm)         LT  N64 Z (crouch)
//   X       N64 B (attack)                RT  N64 R (centre camera)
//   Y       C-left                        LB  C-down (zoom; Z+C-down fairy camera)
//   B       C-right                       RB  C-up (first person; Z+C-up instrument)
//   Menu    Start                         R3  N64 L (unused by the game)
//   D-pad   N64 D-pad                     L stick  N64 stick
//   R stick X  turn (setting right_stick_x: comfort turn, or the game's own C-left/C-right)
//   R stick Y  nothing in VR (the game's default puts C-up/C-down there; mode switches on stick
//              flicks were rejected in Lambda1VR, lesson 80; RB and LB still do them)
//   View    short: the recomp menu (DK64 Recompiled's Back); hold 1 s: recentre
//   Menu + View held 0.5 s: the VR settings menu (SystemChord)
#pragma once

#include <cstdint>

namespace dk64vr {

// ---------------------------------------------------------------------------------------------
// Actions and bindings (data only, so a host test can check every path; lessons 33 and 35)

enum Action : int {
    ActA,
    ActB,
    ActX,
    ActY,
    ActMenu,
    ActView,
    ActLBumper,
    ActRBumper,
    ActLStickClick,
    ActRStickClick,
    ActDpadUp,
    ActDpadDown,
    ActDpadLeft,
    ActDpadRight,
    ActLTrigger,
    ActRTrigger,
    ActLStick,
    ActRStick,
    ActAimLeft,
    ActAimRight,
    ActHapticLeft,
    ActHapticRight,
    ActCount
};

enum class ActionKind { Boolean, Float, Vector2, Pose, Haptic };

struct ActionDef {
    const char *name;       // unique in the set, lower case (OpenXR rules)
    const char *localized;  // unique in the set
    ActionKind kind;
};

const ActionDef &actionDef(int action);

struct Binding {
    int action;
    const char *path;  // full /user/hand/... path
};

struct ProfileBindings {
    const char *profile;  // /interaction_profiles/...
    const Binding *bindings;
    int count;
    bool needsFrameExtension;  // suggested only when XR_VALVE_frame_controller_interaction is enabled
};

// Frame first, then the Index and Touch fallbacks.
int profileCount();
const ProfileBindings &profileAt(int index);

// ---------------------------------------------------------------------------------------------
// N64 pad (libultra button bits, as recompinput's DEFINE_N64_BUTTON_INPUTS)

namespace n64 {
const uint16_t A = 0x8000, B = 0x4000, Z = 0x2000, Start = 0x1000;
const uint16_t DUp = 0x0800, DDown = 0x0400, DLeft = 0x0200, DRight = 0x0100;
const uint16_t L = 0x0020, R = 0x0010;
const uint16_t CUp = 0x0008, CDown = 0x0004, CLeft = 0x0002, CRight = 0x0001;
}

// Menu navigation for recompui (bits).
namespace nav {
const uint32_t Up = 1u << 0, Down = 1u << 1, Left = 1u << 2, Right = 1u << 3;
const uint32_t Accept = 1u << 4, Back = 1u << 5, Apply = 1u << 6;
const uint32_t TabLeft = 1u << 7, TabRight = 1u << 8, ToggleMenu = 1u << 9;
}

// One display frame's action states.
struct InputSample {
    bool valid = false;  // focused and synced; false releases everything
    bool a = false, b = false, x = false, y = false, menu = false, view = false;
    bool lBumper = false, rBumper = false, lStickClick = false, rStickClick = false;
    bool dpadUp = false, dpadDown = false, dpadLeft = false, dpadRight = false;
    float lTrigger = 0.0f, rTrigger = 0.0f;
    float lStick[2] = {}, rStick[2] = {};  // x right, y up
};

enum class RightStickX { Turn = 0, GameCamera = 1 };
enum class TurnMode { Snap = 0, Smooth = 1, Off = 2 };  // the standard's turn_mode order

// Smooth turning (turn_mode smooth): the orbit's rate in degrees a second, positive = right,
// from right-stick X: the standard's 20% deadzone, then the rest of the travel to the power 1.5
// (fine control near the centre), up to smooth_turn_speed at full deflection.
float smoothTurnRate(float stickX, float speedDps);

struct InputConfig {
    RightStickX rightStickX = RightStickX::Turn;
    TurnMode turnMode = TurnMode::Snap;
    float snapAngleDeg = 45.0f;
    float smoothSpeedDps = 120.0f;
    bool moveFromView = true;  // left stick relative to where the wearer looks (head or hand)
};

// What the game is showing, for routing.
struct InputContext {
    bool menuOpen = false;   // a recompui menu has the input (the game's input is off)
    bool stereoPlay = false; // 3D play (gameplay or first person) in stereo: turning applies
    float viewYawRad = 0.0f; // yaw of the movement reference (head or hand) relative to the
                             // game camera's heading, counter-clockwise from above positive
};

struct InputResult {
    uint16_t buttons = 0;  // N64 buttons
    float stickX = 0.0f, stickY = 0.0f;  // N64 stick, -1..1, y up
    uint32_t navHeld = 0;     // recompui navigation, held state
    float turnDeg = 0.0f;     // comfort turn this frame, positive = right (clockwise from above)
    bool recentre = false;    // View held 1 s (or 3 s: recentre again; no standing height here)
    bool systemBuzz = false;  // the standard's 0.6/80 ms buzz on both hands
    bool vrMenuChord = false; // Menu + View held 0.5 s: open or close the VR settings menu
};

// Menu + View held together for 0.5 s opens the VR settings menu (standard.md section 1, for a
// game whose pause menu cannot take a "VR settings" item). The chord is resolved before either
// button acts on its own:
//  - Menu down alone holds Start back for `grace` (0.15 s). View joining in that time makes the
//    chord; otherwise Start passes from then on, or, for a tap shorter than the grace, as a
//    pulse of `startPulse` after the release (long enough for the game's 20-30 Hz input poll).
//  - View down alone passes to its own state machine (short press on release, recentre at 1 s),
//    which fires nothing while the button is held. Menu joining within `viewJoin` (0.9 s, before
//    the recentre) makes the chord and cancels the View press (`cancelView`) without an event.
//  - Once both are down nothing passes, whether the chord completes or one button is let go
//    early, until both are released. The same holds for anything already down after a reset.
struct ChordConfig {
    double grace = 0.15;
    double hold = 0.5;
    double startPulse = 0.12;
    double viewJoin = 0.9;
};

class SystemChord {
public:
    struct Out {
        bool menu = false;        // the Menu level the rest of the mapper sees (Start, Apply)
        bool view = false;        // the View level the View state machine sees
        bool fired = false;       // the chord completed this update
        bool cancelView = false;  // forget the View press in progress (no event)
    };

    void reset();
    Out update(bool menu, bool view, double nowS, const ChordConfig &config = ChordConfig());

private:
    enum Mode { Swallow = 0, Idle, MenuWait, MenuPass, Chord };
    int mode = Swallow;  // zero-initialised: buttons already down are ignored until released
    double menuDownS = 0.0, chordS = 0.0, viewDownS = 0.0, pulseUntilS = -1.0;
    bool prevMenu = false, prevView = false;
};

class InputMapper {
public:
    void reset();
    InputResult update(const InputSample &s, const InputConfig &config, const InputContext &context, double nowS, float dtS);
    // Buttons down now are ignored until each is released (after a menu closes, so the B or View
    // press that closed it never reaches the game).
    void ignoreHeld(const InputSample &s);

private:
    struct ViewState {
        int state = 0;
        double pressS = 0.0;
        int recentred = 0;
        int recalibrated = 0;
    } view;
    SystemChord chord;
    uint32_t ignored = 0;  // raw button bits (see heldBits) waiting for their release
    int toggleReads = 0;  // frames left to hold ToggleMenu after a View short press
    int turnDisarmed = 0;
    bool zHeld = false, rHeld = false;
};

// Rotates the N64 stick (x right, y forward) counter-clockwise by yaw: stick forward then means
// "where the reference looks" when the reference is yawed by `yawRad` from the game camera.
void rotateStick(float yawRad, float &x, float &y);

// OR-merge of the VR pad into a real pad's input: buttons OR, the stick whose deflection is
// larger wins (so a real pad keeps working and an idle one never cancels the VR stick).
void mergeN64(uint16_t &buttons, float &x, float &y, const InputResult &vr);

// Trigger click with hysteresis (standard.md section 3: 0.6 press, 0.45 release).
bool triggerClick(bool wasDown, float value);

}
