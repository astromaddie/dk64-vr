#include "dk64vr_input.h"

#include <algorithm>
#include <cmath>

#include "sfvr/sfvr_turn.h"
#include "sfvr/sfvr_view.h"

namespace dk64vr {

namespace {

// Names are lower case and unique; localized names unique too (lesson 35: a duplicate failed
// xrCreateAction and left Halo with no input at all).
const ActionDef actionDefs[ActCount] = {
    { "a", "A (N64 A: jump, confirm)", ActionKind::Boolean },
    { "b", "B (C-right)", ActionKind::Boolean },
    { "x", "X (N64 B: attack)", ActionKind::Boolean },
    { "y", "Y (C-left)", ActionKind::Boolean },
    { "menu", "Menu (Start, pause)", ActionKind::Boolean },
    { "view", "View (recomp menu; hold to recentre)", ActionKind::Boolean },
    { "l_bumper", "Left bumper (C-down)", ActionKind::Boolean },
    { "r_bumper", "Right bumper (C-up)", ActionKind::Boolean },
    { "l_stick_click", "Left stick click", ActionKind::Boolean },
    { "r_stick_click", "Right stick click (N64 L)", ActionKind::Boolean },
    { "dpad_up", "D-pad up", ActionKind::Boolean },
    { "dpad_down", "D-pad down", ActionKind::Boolean },
    { "dpad_left", "D-pad left", ActionKind::Boolean },
    { "dpad_right", "D-pad right", ActionKind::Boolean },
    { "l_trigger", "Left trigger (Z)", ActionKind::Float },
    { "r_trigger", "Right trigger (R; menu click)", ActionKind::Float },
    { "l_stick", "Left stick (move)", ActionKind::Vector2 },
    { "r_stick", "Right stick (turn)", ActionKind::Vector2 },
    { "aim_left", "Left hand pointer", ActionKind::Pose },
    { "aim_right", "Right hand pointer", ActionKind::Pose },
    { "haptic_left", "Left hand haptics", ActionKind::Haptic },
    { "haptic_right", "Right hand haptics", ActionKind::Haptic },
};

#define L_ "/user/hand/left/"
#define R_ "/user/hand/right/"

// Steam Frame: a split gamepad (D-pad, View on the left; ABXY, Menu on the right).
const Binding frameBindings[] = {
    { ActA, R_ "input/a/click" },
    { ActB, R_ "input/b/click" },
    { ActX, R_ "input/x/click" },
    { ActY, R_ "input/y/click" },
    { ActMenu, R_ "input/menu/click" },
    { ActView, L_ "input/view/click" },
    { ActLBumper, L_ "input/bumper/click" },
    { ActRBumper, R_ "input/bumper/click" },
    { ActLStickClick, L_ "input/thumbstick/click" },
    { ActRStickClick, R_ "input/thumbstick/click" },
    { ActDpadUp, L_ "input/dpad_up/click" },
    { ActDpadDown, L_ "input/dpad_down/click" },
    { ActDpadLeft, L_ "input/dpad_left/click" },
    { ActDpadRight, L_ "input/dpad_right/click" },
    { ActLTrigger, L_ "input/trigger/value" },
    { ActRTrigger, R_ "input/trigger/value" },
    { ActLStick, L_ "input/thumbstick" },
    { ActRStick, R_ "input/thumbstick" },
    { ActAimLeft, L_ "input/aim/pose" },
    { ActAimRight, R_ "input/aim/pose" },
    { ActHapticLeft, L_ "output/haptic" },
    { ActHapticRight, R_ "output/haptic" },
};

// Index: no View, Menu, bumpers or D-pad. Grips are the bumpers, left a/b are Y/B, the left
// stick click is Start and the right stick click is View (N64 L, unused by DK64, is lost).
const Binding indexBindings[] = {
    { ActA, R_ "input/a/click" },
    { ActX, R_ "input/b/click" },
    { ActY, L_ "input/a/click" },
    { ActB, L_ "input/b/click" },
    { ActMenu, L_ "input/thumbstick/click" },
    { ActView, R_ "input/thumbstick/click" },
    { ActLBumper, L_ "input/squeeze/value" },
    { ActRBumper, R_ "input/squeeze/value" },
    { ActLTrigger, L_ "input/trigger/value" },
    { ActRTrigger, R_ "input/trigger/value" },
    { ActLStick, L_ "input/thumbstick" },
    { ActRStick, R_ "input/thumbstick" },
    { ActAimLeft, L_ "input/aim/pose" },
    { ActAimRight, R_ "input/aim/pose" },
    { ActHapticLeft, L_ "output/haptic" },
    { ActHapticRight, R_ "output/haptic" },
};

// Touch: left x/y are X/Y, the left Menu is Start, the left stick click is View, grips are the
// bumpers. No D-pad.
const Binding touchBindings[] = {
    { ActA, R_ "input/a/click" },
    { ActB, R_ "input/b/click" },
    { ActX, L_ "input/x/click" },
    { ActY, L_ "input/y/click" },
    { ActMenu, L_ "input/menu/click" },
    { ActView, L_ "input/thumbstick/click" },
    { ActLBumper, L_ "input/squeeze/value" },
    { ActRBumper, R_ "input/squeeze/value" },
    { ActRStickClick, R_ "input/thumbstick/click" },
    { ActLTrigger, L_ "input/trigger/value" },
    { ActRTrigger, R_ "input/trigger/value" },
    { ActLStick, L_ "input/thumbstick" },
    { ActRStick, R_ "input/thumbstick" },
    { ActAimLeft, L_ "input/aim/pose" },
    { ActAimRight, R_ "input/aim/pose" },
    { ActHapticLeft, L_ "output/haptic" },
    { ActHapticRight, R_ "output/haptic" },
};

#undef L_
#undef R_

#define COUNT_OF(a) int(sizeof(a) / sizeof((a)[0]))

const ProfileBindings profiles[] = {
    { "/interaction_profiles/valve/frame_controller_valve", frameBindings, COUNT_OF(frameBindings), true },
    { "/interaction_profiles/valve/index_controller", indexBindings, COUNT_OF(indexBindings), false },
    { "/interaction_profiles/oculus/touch_controller", touchBindings, COUNT_OF(touchBindings), false },
};

const float StickDeadzone = 0.15f;  // left stick radial deadzone (recompinput's own default is similar)
const float TriggerOn = 0.5f, TriggerOff = 0.35f;

float clamp1(float v) {
    return (v > 1.0f) ? 1.0f : ((v < -1.0f) ? -1.0f : v);
}

}

const ActionDef &actionDef(int action) {
    return actionDefs[(action >= 0 && action < ActCount) ? action : 0];
}

int profileCount() {
    return COUNT_OF(profiles);
}

const ProfileBindings &profileAt(int index) {
    return profiles[(index >= 0 && index < profileCount()) ? index : 0];
}

void rotateStick(float yawRad, float &x, float &y) {
    const float c = std::cos(yawRad), s = std::sin(yawRad);
    const float rx = x * c - y * s;
    const float ry = x * s + y * c;
    x = rx;
    y = ry;
}

float smoothTurnRate(float stickX, float speedDps) {
    const float d = sfvr_turn_deadzoned(clamp1(stickX), 0.2f);
    const float speed = sfvr_turn_normalise_smooth_speed(speedDps);
    return ((d < 0.0f) ? -1.0f : 1.0f) * std::pow(std::fabs(d), 1.5f) * speed;
}

void mergeN64(uint16_t &buttons, float &x, float &y, const InputResult &vr) {
    buttons |= vr.buttons;
    if ((vr.stickX * vr.stickX + vr.stickY * vr.stickY) > (x * x + y * y)) {
        x = vr.stickX;
        y = vr.stickY;
    }
}

bool triggerClick(bool wasDown, float value) {
    return wasDown ? (value > 0.45f) : (value >= 0.6f);
}

void SystemChord::reset() {
    *this = SystemChord();
}

SystemChord::Out SystemChord::update(bool menu, bool view, double nowS, const ChordConfig &config) {
    Out o;
    const bool menuPressed = menu && !prevMenu;
    if (view && !prevView) {
        viewDownS = nowS;
    }

    prevMenu = menu;
    prevView = view;
    const bool viewFresh = view && ((nowS - viewDownS) < config.viewJoin);
    const auto startChord = [&]() {
        mode = Chord;
        chordS = nowS;
        o.cancelView = true;
    };

    switch (mode) {
        case Swallow:
            if (!menu && !view) {
                mode = Idle;
            }
            break;
        case Chord:
            if (!menu || !view) {
                mode = (!menu && !view) ? Idle : Swallow;  // let go early: nothing, until both are up
            }
            else if ((nowS - chordS) >= config.hold) {
                o.fired = true;
                mode = Swallow;
            }
            break;
        case Idle:
            if (menuPressed && viewFresh) {
                startChord();
            }
            else if (menuPressed && view) {
                mode = MenuPass;  // View held past the join window: two separate actions
                o.menu = true;
                o.view = true;
            }
            else if (menuPressed) {
                mode = MenuWait;
                menuDownS = nowS;
                o.view = view;
            }
            else {
                o.view = view;
            }
            break;
        case MenuWait:
            if (!menu) {
                pulseUntilS = nowS + config.startPulse;  // a tap: Start for a moment, now
                mode = Idle;
                o.view = view;
            }
            else if (view) {
                startChord();
            }
            else if ((nowS - menuDownS) >= config.grace) {
                mode = MenuPass;
                o.menu = true;
            }
            break;
        case MenuPass:
            o.menu = menu;
            o.view = view;
            if (!menu) {
                mode = Idle;
            }
            break;
    }

    if (nowS < pulseUntilS) {
        o.menu = true;
    }

    return o;
}

namespace {

// Raw button bits for InputMapper::ignoreHeld.
uint32_t heldBits(const InputSample &s) {
    uint32_t b = 0;
    const bool held[] = { s.a, s.b, s.x, s.y, s.menu, s.view, s.lBumper, s.rBumper, s.lStickClick, s.rStickClick,
        s.dpadUp, s.dpadDown, s.dpadLeft, s.dpadRight, s.lTrigger > TriggerOff, s.rTrigger > TriggerOff };
    for (size_t i = 0; i < sizeof(held) / sizeof(held[0]); i++) {
        b |= held[i] ? (1u << i) : 0u;
    }

    return b;
}

void clearBits(InputSample &s, uint32_t bits) {
    bool *flags[] = { &s.a, &s.b, &s.x, &s.y, &s.menu, &s.view, &s.lBumper, &s.rBumper, &s.lStickClick, &s.rStickClick,
        &s.dpadUp, &s.dpadDown, &s.dpadLeft, &s.dpadRight };
    for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); i++) {
        if (bits & (1u << i)) {
            *flags[i] = false;
        }
    }

    if (bits & (1u << 14)) {
        s.lTrigger = 0.0f;
    }

    if (bits & (1u << 15)) {
        s.rTrigger = 0.0f;
    }
}

}

void InputMapper::reset() {
    *this = InputMapper();
}

void InputMapper::ignoreHeld(const InputSample &s) {
    ignored |= heldBits(s);
}

InputResult InputMapper::update(const InputSample &raw, const InputConfig &config, const InputContext &context, double nowS, float dtS) {
    InputResult r;
    if (!raw.valid) {
        // Focus lost or no actions: every input released, a held View press is cancelled.
        reset();
        return r;
    }

    InputSample s = raw;
    ignored &= heldBits(raw);
    clearBits(s, ignored);

    // The Menu + View chord first, so neither button acts while it may still form.
    const SystemChord::Out chordOut = chord.update(s.menu, s.view, nowS);
    s.menu = chordOut.menu;
    s.view = chordOut.view;
    r.vrMenuChord = chordOut.fired;
    if (chordOut.cancelView) {
        view = ViewState();  // the press is forgotten; the machine waits for a fresh one
        toggleReads = 0;
    }

    // System layer: View short press opens or closes the recomp menu (DK64 Recompiled's Back);
    // held 1 s it recentres (3 s recentres again: there is no standing height in this game).
    sfvr_view_config viewConfig;
    sfvr_view_config_default(&viewConfig);
    sfvr_view_button viewButton = { view.state, view.pressS, view.recentred, view.recalibrated };
    const unsigned events = sfvr_view_update(&viewButton, &viewConfig, s.view ? 1 : 0, nowS);
    view = { viewButton.state, viewButton.press_s, viewButton.recentred, viewButton.recalibrated };
    if (events & SFVR_VIEW_SHORT_PRESS) {
        toggleReads = 2;
    }

    if (events & (SFVR_VIEW_RECENTRE | SFVR_VIEW_RECALIBRATE_HEIGHT)) {
        r.recentre = true;
        r.systemBuzz = true;
    }

    if (toggleReads > 0) {
        r.navHeld |= nav::ToggleMenu;
        toggleReads--;
    }

    // Menu navigation, whenever a recomp menu is open (the game sees nothing then).
    if (context.menuOpen) {
        const float lx = s.lStick[0], ly = s.lStick[1];
        r.navHeld |= (s.dpadUp || ly > 0.6f) ? nav::Up : 0;
        r.navHeld |= (s.dpadDown || ly < -0.6f) ? nav::Down : 0;
        r.navHeld |= (s.dpadLeft || lx < -0.6f) ? nav::Left : 0;
        r.navHeld |= (s.dpadRight || lx > 0.6f) ? nav::Right : 0;
        r.navHeld |= s.a ? nav::Accept : 0;
        r.navHeld |= s.b ? nav::Back : 0;
        r.navHeld |= s.menu ? nav::Apply : 0;
        r.navHeld |= s.lBumper ? nav::TabLeft : 0;
        r.navHeld |= s.rBumper ? nav::TabRight : 0;
        zHeld = rHeld = false;
        turnDisarmed = 0;
        return r;
    }

    // The N64 pad.
    uint16_t b = 0;
    b |= s.a ? n64::A : 0;
    b |= s.x ? n64::B : 0;
    b |= s.y ? n64::CLeft : 0;
    b |= s.b ? n64::CRight : 0;
    b |= s.lBumper ? n64::CDown : 0;
    b |= s.rBumper ? n64::CUp : 0;
    b |= s.menu ? n64::Start : 0;
    b |= s.rStickClick ? n64::L : 0;
    b |= s.dpadUp ? n64::DUp : 0;
    b |= s.dpadDown ? n64::DDown : 0;
    b |= s.dpadLeft ? n64::DLeft : 0;
    b |= s.dpadRight ? n64::DRight : 0;
    zHeld = zHeld ? (s.lTrigger > TriggerOff) : (s.lTrigger >= TriggerOn);
    rHeld = rHeld ? (s.rTrigger > TriggerOff) : (s.rTrigger >= TriggerOn);
    b |= zHeld ? n64::Z : 0;
    b |= rHeld ? n64::R : 0;

    // Left stick: radial deadzone, then relative to the view in stereo play.
    float x = clamp1(s.lStick[0]), y = clamp1(s.lStick[1]);
    const float mag = std::sqrt(x * x + y * y);
    if (mag < StickDeadzone) {
        x = y = 0.0f;
    }
    else {
        const float scaled = std::fmin(1.0f, (mag - StickDeadzone) / (1.0f - StickDeadzone));
        x = x / mag * scaled;
        y = y / mag * scaled;
        if (config.moveFromView && context.stereoPlay) {
            rotateStick(context.viewYawRad, x, y);
        }
    }

    // Right stick X: a comfort turn, or the game's own C-left/C-right camera swing.
    const float rx = clamp1(s.rStick[0]);
    if (config.rightStickX == RightStickX::GameCamera) {
        b |= (rx < -0.5f) ? n64::CLeft : 0;
        b |= (rx > 0.5f) ? n64::CRight : 0;
        turnDisarmed = 0;
    }
    else if (context.stereoPlay) {
        sfvr_turn_config turn;
        sfvr_turn_config_default(&turn);
        turn.mode = (config.turnMode == TurnMode::Snap) ? SFVR_TURN_SNAP : ((config.turnMode == TurnMode::Smooth) ? SFVR_TURN_SMOOTH : SFVR_TURN_OFF);
        turn.snap_angle_deg = config.snapAngleDeg;
        turn.smooth_speed_dps = config.smoothSpeedDps;
        sfvr_turn_state state = { turnDisarmed };
        r.turnDeg = sfvr_turn_update(&state, &turn, rx, dtS);
        turnDisarmed = state.disarmed;
        if (config.turnMode == TurnMode::Smooth) {
            // The orbit about Donkey Kong: a softer curve than the standard's linear one.
            r.turnDeg = smoothTurnRate(rx, config.smoothSpeedDps) * std::clamp(dtS, 0.0f, 0.1f);
        }
    }
    else {
        turnDisarmed = 0;
    }

    r.buttons = b;
    r.stickX = x;
    r.stickY = y;
    return r;
}

}
