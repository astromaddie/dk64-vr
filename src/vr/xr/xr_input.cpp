#include "xr_input.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "dk64vr_log.h"

namespace dk64vr {

namespace {

XrActionType xrType(ActionKind kind) {
    switch (kind) {
    case ActionKind::Boolean: return XR_ACTION_TYPE_BOOLEAN_INPUT;
    case ActionKind::Float: return XR_ACTION_TYPE_FLOAT_INPUT;
    case ActionKind::Vector2: return XR_ACTION_TYPE_VECTOR2F_INPUT;
    case ActionKind::Pose: return XR_ACTION_TYPE_POSE_INPUT;
    case ActionKind::Haptic: return XR_ACTION_TYPE_VIBRATION_OUTPUT;
    }

    return XR_ACTION_TYPE_BOOLEAN_INPUT;
}

}

bool XrInput::create(XrInstance xrInstance, XrSession xrSession, bool frameProfile, bool eyeGaze) {
    instance = xrInstance;
    session = xrSession;
    XrActionSetCreateInfo setInfo = { XR_TYPE_ACTION_SET_CREATE_INFO };
    snprintf(setInfo.actionSetName, sizeof(setInfo.actionSetName), "dk64");
    snprintf(setInfo.localizedActionSetName, sizeof(setInfo.localizedActionSetName), "Donkey Kong 64");
    XrResult res = xrCreateActionSet(instance, &setInfo, &actionSet);
    if (XR_FAILED(res)) {
        dk64vr_logpf("[openxr]", "input: xrCreateActionSet failed (%d); controller actions unavailable", int(res));
        actionSet = XR_NULL_HANDLE;
        return false;
    }

    for (int i = 0; i < ActCount; i++) {
        const ActionDef &def = actionDef(i);
        XrActionCreateInfo info = { XR_TYPE_ACTION_CREATE_INFO };
        info.actionType = xrType(def.kind);
        snprintf(info.actionName, sizeof(info.actionName), "%s", def.name);
        snprintf(info.localizedActionName, sizeof(info.localizedActionName), "%s", def.localized);
        res = xrCreateAction(actionSet, &info, &actions[i]);
        if (XR_FAILED(res)) {
            // Lesson 35: a blocker, not a warning.
            dk64vr_logpf("[openxr]", "input: xrCreateAction %s failed (%d); controller actions unavailable", def.name, int(res));
            xrDestroyActionSet(actionSet);
            actionSet = XR_NULL_HANDLE;
            return false;
        }
    }

    int accepted = 0;
    for (int p = 0; p < profileCount(); p++) {
        const ProfileBindings &profile = profileAt(p);
        if (profile.needsFrameExtension && !frameProfile) {
            dk64vr_logpf("[openxr]", "input: %s skipped (XR_VALVE_frame_controller_interaction not offered)", profile.profile);
            continue;
        }

        XrActionSuggestedBinding list[64];
        uint32_t n = 0;
        bool pathsOk = true;
        for (int b = 0; (b < profile.count) && (n < 64); b++) {
            XrPath path = XR_NULL_PATH;
            if (XR_FAILED(xrStringToPath(instance, profile.bindings[b].path, &path))) {
                pathsOk = false;
                dk64vr_logpf("[openxr]", "input: bad path %s", profile.bindings[b].path);
                continue;
            }

            list[n].action = actions[profile.bindings[b].action];
            list[n].binding = path;
            n++;
        }

        XrInteractionProfileSuggestedBinding suggested = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
        xrStringToPath(instance, profile.profile, &suggested.interactionProfile);
        suggested.countSuggestedBindings = n;
        suggested.suggestedBindings = list;
        res = xrSuggestInteractionProfileBindings(instance, &suggested);
        dk64vr_logpf("[openxr]", "input: bindings %s: %s (%d, %u paths%s)", profile.profile, XR_SUCCEEDED(res) ? "suggested" : "REJECTED", int(res), n, pathsOk ? "" : ", some bad");
        accepted += XR_SUCCEEDED(res) ? 1 : 0;
    }

    for (int hand = 0; hand < 2; hand++) {
        XrActionSpaceCreateInfo spaceInfo = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
        spaceInfo.action = actions[(hand == 0) ? ActAimLeft : ActAimRight];
        spaceInfo.poseInActionSpace.orientation.w = 1.0f;
        xrCreateActionSpace(session, &spaceInfo, &aimSpace[hand]);
    }

    if (eyeGaze) {
        XrActionCreateInfo action = { XR_TYPE_ACTION_CREATE_INFO };
        action.actionType = XR_ACTION_TYPE_POSE_INPUT;
        strcpy(action.actionName, "foveation_gaze");
        strcpy(action.localizedActionName, "Foveation gaze");
        res = xrCreateAction(actionSet, &action, &gazeAction);
        if (XR_SUCCEEDED(res)) {
            XrActionSuggestedBinding binding = { gazeAction, XR_NULL_PATH };
            XrInteractionProfileSuggestedBinding suggested = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
            res = xrStringToPath(instance, "/user/eyes_ext/input/gaze_ext/pose", &binding.binding);
            if (XR_SUCCEEDED(res)) res = xrStringToPath(instance, "/interaction_profiles/ext/eye_gaze_interaction", &suggested.interactionProfile);
            suggested.countSuggestedBindings = 1;
            suggested.suggestedBindings = &binding;
            if (XR_SUCCEEDED(res)) res = xrSuggestInteractionProfileBindings(instance, &suggested);
            if (XR_SUCCEEDED(res)) {
                XrActionSpaceCreateInfo spaceInfo = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
                spaceInfo.action = gazeAction;
                spaceInfo.poseInActionSpace.orientation.w = 1.0f;
                res = xrCreateActionSpace(session, &spaceInfo, &gazeSpace);
            }
        }
        dk64vr_logpf("[vr-fov]", "eye-gaze action=%s result=%d (invalid/inactive gaze falls back to fixed)", XR_SUCCEEDED(res) ? "bound" : "unavailable", int(res));
    }

    XrSessionActionSetsAttachInfo attach = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    attach.countActionSets = 1;
    attach.actionSets = &actionSet;
    res = xrAttachSessionActionSets(session, &attach);
    if (XR_FAILED(res)) {
        dk64vr_logpf("[openxr]", "input: xrAttachSessionActionSets failed (%d); controller actions unavailable", int(res));
        destroy();
        return false;
    }

    dk64vr_logpf("[openxr]", "input: action set attached (%d actions, %d of %d profiles accepted%s)", int(ActCount), accepted, profileCount(), frameProfile ? ", Frame first" : ", no Frame profile");
    return accepted > 0;
}

void XrInput::destroy() {
    if (gazeSpace != XR_NULL_HANDLE) xrDestroySpace(gazeSpace);
    gazeSpace = XR_NULL_HANDLE;
    gazeAction = XR_NULL_HANDLE;
    gazeValid = false;
    for (int hand = 0; hand < 2; hand++) {
        if (aimSpace[hand] != XR_NULL_HANDLE) {
            xrDestroySpace(aimSpace[hand]);
            aimSpace[hand] = XR_NULL_HANDLE;
        }
    }

    if (actionSet != XR_NULL_HANDLE) {
        xrDestroyActionSet(actionSet);
        actionSet = XR_NULL_HANDLE;
    }
}

bool XrInput::getBool(int action) {
    XrActionStateGetInfo info = { XR_TYPE_ACTION_STATE_GET_INFO };
    info.action = actions[action];
    XrActionStateBoolean state = { XR_TYPE_ACTION_STATE_BOOLEAN };
    return XR_SUCCEEDED(xrGetActionStateBoolean(session, &info, &state)) && state.isActive && state.currentState;
}

float XrInput::getFloat(int action) {
    XrActionStateGetInfo info = { XR_TYPE_ACTION_STATE_GET_INFO };
    info.action = actions[action];
    XrActionStateFloat state = { XR_TYPE_ACTION_STATE_FLOAT };
    if (XR_SUCCEEDED(xrGetActionStateFloat(session, &info, &state)) && state.isActive) {
        return state.currentState;
    }

    return 0.0f;
}

void XrInput::getVec2(int action, float out[2]) {
    XrActionStateGetInfo info = { XR_TYPE_ACTION_STATE_GET_INFO };
    info.action = actions[action];
    XrActionStateVector2f state = { XR_TYPE_ACTION_STATE_VECTOR2F };
    out[0] = out[1] = 0.0f;
    if (XR_SUCCEEDED(xrGetActionStateVector2f(session, &info, &state)) && state.isActive) {
        out[0] = state.currentState.x;
        out[1] = state.currentState.y;
    }
}

void XrInput::logProfiles() {
    for (int hand = 0; hand < 2; hand++) {
        XrPath handPath = XR_NULL_PATH;
        xrStringToPath(instance, (hand == 0) ? "/user/hand/left" : "/user/hand/right", &handPath);
        XrInteractionProfileState state = { XR_TYPE_INTERACTION_PROFILE_STATE };
        if (XR_FAILED(xrGetCurrentInteractionProfile(session, handPath, &state)) || (state.interactionProfile == currentProfile[hand])) {
            continue;
        }

        currentProfile[hand] = state.interactionProfile;
        char name[XR_MAX_PATH_LENGTH] = "none";
        uint32_t length = 0;
        if (state.interactionProfile != XR_NULL_PATH) {
            xrPathToString(instance, state.interactionProfile, sizeof(name), &length, name);
        }

        dk64vr_logpf("[openxr]", "input: %s hand uses %s", (hand == 0) ? "left" : "right", name);
    }
}

InputSample XrInput::sample(bool focused, XrSpace space, XrTime time) {
    InputSample s;
    aimValid[0] = aimValid[1] = false;
    gazeValid = false;
    if (!focused || (actionSet == XR_NULL_HANDLE)) {
        return s;
    }

    XrActiveActionSet active = { actionSet, XR_NULL_PATH };
    XrActionsSyncInfo syncInfo = { XR_TYPE_ACTIONS_SYNC_INFO };
    syncInfo.countActiveActionSets = 1;
    syncInfo.activeActionSets = &active;
    const XrResult res = xrSyncActions(session, &syncInfo);
    if (res != XR_SUCCESS) {
        // XR_SESSION_NOT_FOCUSED is a success code with every action inactive.
        if (XR_FAILED(res) && !syncFailedLogged) {
            syncFailedLogged = true;
            dk64vr_logpf("[openxr]", "input: xrSyncActions failed (%d)", int(res));
        }

        return s;
    }

    s.valid = true;
    s.a = getBool(ActA);
    s.b = getBool(ActB);
    s.x = getBool(ActX);
    s.y = getBool(ActY);
    s.menu = getBool(ActMenu);
    s.view = getBool(ActView);
    s.lBumper = getBool(ActLBumper);
    s.lGrip = getFloat(ActLGrip) >= 0.7f;
    s.rBumper = getBool(ActRBumper);
    s.lStickClick = getBool(ActLStickClick);
    s.rStickClick = getBool(ActRStickClick);
    s.dpadUp = getBool(ActDpadUp);
    s.dpadDown = getBool(ActDpadDown);
    s.dpadLeft = getBool(ActDpadLeft);
    s.dpadRight = getBool(ActDpadRight);
    s.lTrigger = getFloat(ActLTrigger);
    s.rTrigger = getFloat(ActRTrigger);
    getVec2(ActLStick, s.lStick);
    getVec2(ActRStick, s.rStick);
    for (int hand = 0; hand < 2; hand++) {
        XrSpaceLocation location = { XR_TYPE_SPACE_LOCATION };
        if ((aimSpace[hand] != XR_NULL_HANDLE) && XR_SUCCEEDED(xrLocateSpace(aimSpace[hand], space, time, &location))) {
            const XrSpaceLocationFlags needed = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
            aimValid[hand] = (location.locationFlags & needed) == needed;
            aim[hand].q = { location.pose.orientation.x, location.pose.orientation.y, location.pose.orientation.z, location.pose.orientation.w };
            aim[hand].p = { location.pose.position.x, location.pose.position.y, location.pose.position.z };
        }
    }

    if (gazeAction != XR_NULL_HANDLE && gazeSpace != XR_NULL_HANDLE) {
        XrActionStateGetInfo info = { XR_TYPE_ACTION_STATE_GET_INFO };
        info.action = gazeAction;
        XrActionStatePose state = { XR_TYPE_ACTION_STATE_POSE };
        XrSpaceLocation location = { XR_TYPE_SPACE_LOCATION };
        if (XR_SUCCEEDED(xrGetActionStatePose(session, &info, &state)) && state.isActive
            && XR_SUCCEEDED(xrLocateSpace(gazeSpace, space, time, &location))) {
            const XrSpaceLocationFlags needed = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
            gazeValid = (location.locationFlags & needed) == needed;
            gaze = { { location.pose.orientation.x, location.pose.orientation.y, location.pose.orientation.z, location.pose.orientation.w },
                { location.pose.position.x, location.pose.position.y, location.pose.position.z } };
        }
    }

    if (!loggedState) {
        loggedState = true;
        logProfiles();
    }
    else if ((++samplesLogged % 720) == 0) {
        logProfiles();
    }

    return s;
}

void XrInput::flushHaptics(float strength) {
    struct Context {
        XrInput *self;
    } context = { this };
    sfvr_haptic_flush(&haptics, strength, [](int hand, float amplitude, float seconds, void *user) {
        XrInput *self = static_cast<Context *>(user)->self;
        XrHapticActionInfo info = { XR_TYPE_HAPTIC_ACTION_INFO };
        info.action = self->actions[(hand == SFVR_HAND_LEFT) ? ActHapticLeft : ActHapticRight];
        XrHapticVibration vibration = { XR_TYPE_HAPTIC_VIBRATION };
        vibration.amplitude = amplitude;
        vibration.duration = XrDuration(std::llround(double(seconds) * 1e9));
        vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
        xrApplyHapticFeedback(self->session, &info, reinterpret_cast<const XrHapticBaseHeader *>(&vibration));
    }, &context);
}

void XrInput::stopHaptics() {
    if (actionSet == XR_NULL_HANDLE) {
        return;
    }

    sfvr_haptic_queue_clear(&haptics);
    for (int hand = 0; hand < 2; hand++) {
        XrHapticActionInfo info = { XR_TYPE_HAPTIC_ACTION_INFO };
        info.action = actions[(hand == 0) ? ActHapticLeft : ActHapticRight];
        xrStopHapticFeedback(session, &info);
    }
}

}
