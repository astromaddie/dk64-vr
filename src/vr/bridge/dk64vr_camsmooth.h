// Smoothing of the game camera, which in VR is the head origin (docs/VR_ARCHITECTURE.md 5c).
// Pure: no OpenXR, no GPU, no game state.
//
// DK64 moves its camera at 30 Hz. RT64 interpolates linearly between the last two game frames, so
// the camera's velocity still changes in steps at every game frame, and the game's own swings
// (C-left/C-right, auto-follow) start and stop at full speed. In the headset that is felt as the
// world jerking about the wearer. Following Star Fox VR (lesson 31):
//
// - low: a uniform quadratic B-spline through the last three game frames' cameras, evaluated at
//   the display frame's interpolation weight: position and velocity are continuous across game
//   frames, half a game frame behind the linear path (the linear path convolved with a box one
//   game frame wide).
// - medium, high: the B-spline, then a critically damped follower on the orientation only
//   (smoothing time 0.06 s and 0.12 s): a swing's start and stop are eased. Position is left on
//   the B-spline so Donkey Kong does not drift in the view.
//
// A hard cut (a jump in position or angle between two game frames, or the host's own reasons:
// scene changes, mode changes) resets the history and the follower to the new camera, so it
// never swoops. The result is one rigid camera: the host replaces the game view with it for the
// whole scene, so everything moves together.
#pragma once

#include <cstdint>

#include "dk64vr_math.h"

namespace dk64vr {

enum class CameraSmoothing { Off = 0, Low = 1, Medium = 2, High = 3 };
const char *cameraSmoothingName(CameraSmoothing level);
// The follower's smoothing time for a level (0: none).
float cameraFollowTime(CameraSmoothing level);

// Camera poses (camera to world: p_world = rotate(q, p_camera) + p) and RT64/libultra views.
Pose cameraPoseFromView(const Mat4 &view);  // orthonormalised
Mat4 viewFromCameraPose(const Pose &camera);

Quat quatSlerp(const Quat &a, const Quat &b, float t);  // shortest path
Vec3 quatLog(const Quat &q);  // rotation vector (axis * angle), shortest
Quat quatExp(const Vec3 &v);
float quatAngle(const Quat &a, const Quat &b);  // 0..pi
Pose poseLerp(const Pose &a, const Pose &b, float t);

// Heading (as viewHeading), pitch (up positive) of a camera pose, radians.
float cameraYaw(const Pose &camera);
float cameraPitch(const Pose &camera);

struct CameraCutLimits {
    float distance = 60.0f;   // game units between two game frames (4 m at 15 units/m)
    float angle = 0.9f;       // radians between two game frames (about 52 degrees)
};

// The smoother works on game views (world to camera, as RT64 holds them). Its linear path is
// RT64's own: the view's rotation slerped and its translation lerped (RigidBody::lerp), so the
// correction the host applies, inverse(linear) * smoothed, is the identity when smoothing is off
// and carries only the smoothing otherwise.
class CameraSmoother {
public:
    // A new game frame's camera view. A jump past the limits, or forceCut, resets the history
    // and the follower to it. Returns true for a cut.
    bool addTick(const Mat4 &view, bool forceCut = false, const Vec3 *lookAt = nullptr);
    // The look-at point uses the same tick history and interpolation weight as the camera:
    // linear with Off, a quadratic B-spline otherwise. Cuts and resets clear both together.
    bool focusReady() const { return haveFocus; }
    Vec3 focus(float w, CameraSmoothing level) const;
    void reset();
    bool ready() const { return count > 0; }
    uint64_t cuts() const { return cutCount; }

    // The view for a display frame: w is its interpolation weight between the newest two game
    // frames (RT64's), dt the display time since the previous sample (seconds).
    Mat4 sample(float w, float dt, CameraSmoothing level);
    // RT64's linear path through the newest two game frames.
    Mat4 linear(float w) const;
    // The B-spline alone.
    Mat4 spline(float w) const;

    CameraCutLimits limits;

private:
    struct ViewPose {
        Quat q;  // the view's rotation (rows of its 3x3 are rotate(q, e_i))
        Vec3 t;  // its translation row
    };
    static ViewPose fromView(const Mat4 &view);
    static Mat4 toView(const ViewPose &v);
    static ViewPose lerpView(const ViewPose &a, const ViewPose &b, float t);
    ViewPose splinePose(float w) const;

    Vec3 focusHist[3];
    bool haveFocus = false;
    ViewPose hist[3];  // oldest, previous, newest
    int count = 0;
    uint64_t cutCount = 0;
    bool followerValid = false;
    Quat followerQ;    // the follower's camera orientation
    Vec3 followerVel;  // angular velocity of its offset from the target, rad/s (world frame)
};

// Per-display-frame camera steps for the [vr-cam] diagnostics: heading, pitch (degrees) and
// position (game units) steps, and their changes from frame to frame (how abruptly the camera
// speeds up or slows down).
struct CameraStepStats {
    void add(const Pose &camera);
    void restart();          // a cut: the next frame starts a new run (no step across it)
    void clearWindow();      // a new window, keeping the run going
    uint32_t frames = 0;     // steps counted in the window
    double sum[3] = {}, sumSq[3] = {}, maxAbs[3] = {};      // steps: yaw, pitch, position
    double dSum[3] = {}, dSumSq[3] = {}, dMaxAbs[3] = {};   // changes of step
    uint32_t dFrames = 0;
    double stepSd(int i) const;
    double changeSd(int i) const;

private:
    bool havePrev = false, haveStep = false;
    float prevYaw = 0.0f, prevPitch = 0.0f;
    Vec3 prevPos;
    double prevStep[3] = {};
};

}
