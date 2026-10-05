// OpenXR actions for DK64 VR: the action set from dk64vr_input (Frame profile first, Index and
// Touch fallbacks), syncing and sampling once per display frame, the aim poses for the menu
// pointer, and haptics through sfvr's coalescing queue. Workload (XR frame) thread only.
#pragma once

#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>

#include "dk64vr_input.h"
#include "dk64vr_math.h"
#include "sfvr/sfvr_haptics.h"

namespace dk64vr {

class XrInput {
public:
    // After the session exists. Logs each profile's suggest result (lesson 33) and the attach.
    bool create(XrInstance instance, XrSession session, bool frameProfile, bool eyeGaze = false);
    void destroy();

    // Syncs and samples. `focused` false (or a failed sync) gives an invalid sample: every input
    // released. Aim poses are located in `space` at `time`.
    InputSample sample(bool focused, XrSpace space, XrTime time);

    // The dominant hand's aim pose (hand 0 = left, 1 = right), valid when tracked.
    bool aimPose(int hand, Pose &pose) const { pose = aim[hand]; return aimValid[hand]; }
    bool gazePose(Pose &pose) const { pose = gaze; return gazeValid; }

    sfvr_haptic_queue haptics = {};
    // Sends the queued pulses, scaled by `strength` (the haptics setting).
    void flushHaptics(float strength);
    void stopHaptics();

    bool ready() const { return actionSet != XR_NULL_HANDLE; }

private:
    XrInstance instance = XR_NULL_HANDLE;
    XrSession session = XR_NULL_HANDLE;
    XrActionSet actionSet = XR_NULL_HANDLE;
    XrAction actions[ActCount] = {};
    XrSpace aimSpace[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };
    Pose aim[2];
    bool aimValid[2] = {};
    XrAction gazeAction = XR_NULL_HANDLE;
    XrSpace gazeSpace = XR_NULL_HANDLE;
    Pose gaze;
    bool gazeValid = false;
    bool loggedState = false;
    bool syncFailedLogged = false;
    uint32_t samplesLogged = 0;
    XrPath currentProfile[2] = { XR_NULL_PATH, XR_NULL_PATH };

    bool getBool(int action);
    float getFloat(int action);
    void getVec2(int action, float out[2]);
    void logProfiles();
};

}
