#include "dk64vr_camsmooth.h"

#include <algorithm>
#include <cmath>

namespace dk64vr {

namespace {

const float Pi = 3.14159265359f;

Vec3 lerp3(const Vec3 &a, const Vec3 &b, float t) {
    return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t };
}

float length3(const Vec3 &v) {
    return std::sqrt(dot(v, v));
}

}

const char *cameraSmoothingName(CameraSmoothing level) {
    static const char *names[] = { "off", "low", "medium", "high" };
    const int i = int(level);
    return ((i >= 0) && (i < 4)) ? names[i] : "?";
}

float cameraFollowTime(CameraSmoothing level) {
    switch (level) {
    case CameraSmoothing::Medium:
        return 0.06f;
    case CameraSmoothing::High:
        return 0.12f;
    default:
        return 0.0f;
    }
}

Pose cameraPoseFromView(const Mat4 &view) {
    const Mat4 cam = rigidInverse(view);
    const Vec3 back = normalize({ cam.m[2][0], cam.m[2][1], cam.m[2][2] });
    const Vec3 upIn = { cam.m[1][0], cam.m[1][1], cam.m[1][2] };
    Vec3 x = normalize(cross(upIn, back));
    if (dot(x, x) < 0.5f) {
        x = normalize({ cam.m[0][0], cam.m[0][1], cam.m[0][2] });
    }

    const Vec3 y = cross(back, x);
    Pose r;
    r.q = quatFromBasis(x, y, back);
    r.p = { cam.m[3][0], cam.m[3][1], cam.m[3][2] };
    return r;
}

Mat4 viewFromCameraPose(const Pose &camera) {
    return rigidInverse(poseMatrix(camera));
}

Quat quatSlerp(const Quat &a, const Quat &bIn, float t) {
    Quat b = bIn;
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0.0f) {
        b = { -b.x, -b.y, -b.z, -b.w };
        d = -d;
    }

    float wa, wb;
    if (d > 0.9995f) {
        wa = 1.0f - t;
        wb = t;
    }
    else {
        const float theta = std::acos(std::min(1.0f, d));
        const float s = std::sin(theta);
        wa = std::sin((1.0f - t) * theta) / s;
        wb = std::sin(t * theta) / s;
    }

    return quatNormalize({ wa * a.x + wb * b.x, wa * a.y + wb * b.y, wa * a.z + wb * b.z, wa * a.w + wb * b.w });
}

Vec3 quatLog(const Quat &qIn) {
    Quat q = quatNormalize(qIn);
    if (q.w < 0.0f) {
        q = { -q.x, -q.y, -q.z, -q.w };
    }

    const float s = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
    if (s < 1e-7f) {
        return { 2.0f * q.x, 2.0f * q.y, 2.0f * q.z };
    }

    const float angle = 2.0f * std::atan2(s, q.w);
    return { q.x / s * angle, q.y / s * angle, q.z / s * angle };
}

Quat quatExp(const Vec3 &v) {
    const float angle = length3(v);
    if (angle < 1e-7f) {
        return quatNormalize({ 0.5f * v.x, 0.5f * v.y, 0.5f * v.z, 1.0f });
    }

    const float s = std::sin(0.5f * angle) / angle;
    return { v.x * s, v.y * s, v.z * s, std::cos(0.5f * angle) };
}

float quatAngle(const Quat &a, const Quat &b) {
    // atan2 rather than acos: exact for small angles in float.
    const Quat d = quatMul(a, quatConj(b));
    return 2.0f * std::atan2(std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z), std::fabs(d.w));
}

Pose poseLerp(const Pose &a, const Pose &b, float t) {
    Pose r;
    r.q = quatSlerp(a.q, b.q, t);
    r.p = lerp3(a.p, b.p, t);
    return r;
}

float cameraYaw(const Pose &camera) {
    return quatYaw(camera.q);
}

float cameraPitch(const Pose &camera) {
    const Vec3 f = quatRotate(camera.q, { 0.0f, 0.0f, -1.0f });
    return std::atan2(f.y, std::sqrt(f.x * f.x + f.z * f.z));
}

// ---------------------------------------------------------------------------------------------

CameraSmoother::ViewPose CameraSmoother::fromView(const Mat4 &view) {
    // Rows 0-2 are rotate(q, e_i) (poseMatrix's layout), orthonormalised.
    const Vec3 z = normalize({ view.m[2][0], view.m[2][1], view.m[2][2] });
    Vec3 x = normalize(cross({ view.m[1][0], view.m[1][1], view.m[1][2] }, z));
    if (dot(x, x) < 0.5f) {
        x = normalize({ view.m[0][0], view.m[0][1], view.m[0][2] });
    }

    ViewPose r;
    r.q = quatFromBasis(x, cross(z, x), z);
    r.t = { view.m[3][0], view.m[3][1], view.m[3][2] };
    return r;
}

Mat4 CameraSmoother::toView(const ViewPose &v) {
    return poseMatrix(Pose { v.q, v.t });
}

CameraSmoother::ViewPose CameraSmoother::lerpView(const ViewPose &a, const ViewPose &b, float t) {
    return { quatSlerp(a.q, b.q, t), lerp3(a.t, b.t, t) };
}

void CameraSmoother::reset() {
    count = 0;
    haveFocus = false;
    followerValid = false;
    followerVel = {};
}

bool CameraSmoother::addTick(const Mat4 &view, bool forceCut, const Vec3 *lookAt) {
    const ViewPose v = fromView(view);
    bool cut = forceCut;
    if (count > 0) {
        const Pose a = cameraPoseFromView(view), b = cameraPoseFromView(toView(hist[2]));
        const Vec3 d = { a.p.x - b.p.x, a.p.y - b.p.y, a.p.z - b.p.z };
        cut = cut || (length3(d) > limits.distance) || (quatAngle(a.q, b.q) > limits.angle);
    }

    if (lookAt != nullptr) {
        if (!haveFocus || count == 0 || cut) {
            focusHist[0] = focusHist[1] = focusHist[2] = *lookAt;
        }
        else {
            focusHist[0] = focusHist[1];
            focusHist[1] = focusHist[2];
            focusHist[2] = *lookAt;
        }
        haveFocus = true;
    }
    else {
        haveFocus = false;
    }

    if ((count == 0) || cut) {
        hist[0] = hist[1] = hist[2] = v;
        count = 3;
        followerValid = false;
        followerVel = {};
        if (cut) {
            cutCount++;
        }

        return cut;
    }

    hist[0] = hist[1];
    hist[1] = hist[2];
    hist[2] = v;
    return false;
}

Vec3 CameraSmoother::focus(float w, CameraSmoothing level) const {
    if (!haveFocus) {
        return {};
    }
    w = std::clamp(w, 0.0f, 1.0f);
    if (level == CameraSmoothing::Off) {
        return lerp3(focusHist[1], focusHist[2], w);
    }
    return lerp3(lerp3(focusHist[0], focusHist[1], 0.5f + 0.5f * w), lerp3(focusHist[1], focusHist[2], 0.5f * w), w);
}

Mat4 CameraSmoother::linear(float w) const {
    return toView(lerpView(hist[1], hist[2], std::clamp(w, 0.0f, 1.0f)));
}

CameraSmoother::ViewPose CameraSmoother::splinePose(float w) const {
    // Uniform quadratic B-spline by de Boor (Star Fox VR, scene_interpolation.cpp): at w = 0 the
    // midpoint of the oldest two, at w = 1 the midpoint of the newest two.
    w = std::clamp(w, 0.0f, 1.0f);
    return lerpView(lerpView(hist[0], hist[1], 0.5f + 0.5f * w), lerpView(hist[1], hist[2], 0.5f * w), w);
}

Mat4 CameraSmoother::spline(float w) const {
    return toView(splinePose(w));
}

Mat4 CameraSmoother::sample(float w, float dt, CameraSmoothing level) {
    if (level == CameraSmoothing::Off) {
        followerValid = false;
        return linear(w);
    }

    const Mat4 splineView = spline(w);
    const float time = cameraFollowTime(level);
    if (time <= 0.0f) {
        followerValid = false;
        return splineView;
    }

    // The follower turns the camera about its own position (the B-spline's), never moves it.
    Pose target = cameraPoseFromView(splineView);
    if (!followerValid) {
        followerValid = true;
        followerQ = target.q;
        followerVel = {};
        return splineView;
    }

    // A critically damped spring on the rotation from the target to the follower (Game
    // Programming Gems 4, 1.10: stable for any step), in the world frame.
    dt = std::clamp(dt, 0.0f, 0.1f);
    Vec3 offset = quatLog(quatMul(followerQ, quatConj(target.q)));
    if (length3(offset) > 1.2f) {
        // Too far behind (a cut the limits missed): no swoop.
        followerQ = target.q;
        followerVel = {};
        return splineView;
    }

    const float omega = 2.0f / time;
    const float x = omega * dt;
    const float e = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
    const Vec3 temp = { (followerVel.x + omega * offset.x) * dt, (followerVel.y + omega * offset.y) * dt, (followerVel.z + omega * offset.z) * dt };
    followerVel = { (followerVel.x - omega * temp.x) * e, (followerVel.y - omega * temp.y) * e, (followerVel.z - omega * temp.z) * e };
    offset = { (offset.x + temp.x) * e, (offset.y + temp.y) * e, (offset.z + temp.z) * e };
    followerQ = quatNormalize(quatMul(quatExp(offset), target.q));
    target.q = followerQ;
    return viewFromCameraPose(target);
}

// ---------------------------------------------------------------------------------------------

void CameraStepStats::restart() {
    havePrev = false;
    haveStep = false;
}

void CameraStepStats::clearWindow() {
    frames = dFrames = 0;
    for (int i = 0; i < 3; i++) {
        sum[i] = sumSq[i] = maxAbs[i] = 0.0;
        dSum[i] = dSumSq[i] = dMaxAbs[i] = 0.0;
    }
}

void CameraStepStats::add(const Pose &camera) {
    const float yaw = cameraYaw(camera), pitch = cameraPitch(camera);
    if (havePrev) {
        const double step[3] = {
            double(wrapAngle(yaw - prevYaw)) * 180.0 / Pi,
            double(pitch - prevPitch) * 180.0 / Pi,
            double(length3({ camera.p.x - prevPos.x, camera.p.y - prevPos.y, camera.p.z - prevPos.z })),
        };
        frames++;
        for (int i = 0; i < 3; i++) {
            sum[i] += step[i];
            sumSq[i] += step[i] * step[i];
            maxAbs[i] = std::max(maxAbs[i], std::fabs(step[i]));
            if (haveStep) {
                const double d = step[i] - prevStep[i];
                dSum[i] += d;
                dSumSq[i] += d * d;
                dMaxAbs[i] = std::max(dMaxAbs[i], std::fabs(d));
            }

            prevStep[i] = step[i];
        }

        dFrames += haveStep ? 1 : 0;
        haveStep = true;
    }

    havePrev = true;
    prevYaw = yaw;
    prevPitch = pitch;
    prevPos = camera.p;
}

double CameraStepStats::stepSd(int i) const {
    if (frames < 2) {
        return 0.0;
    }

    const double mean = sum[i] / frames;
    return std::sqrt(std::max(0.0, sumSq[i] / frames - mean * mean));
}

double CameraStepStats::changeSd(int i) const {
    if (dFrames < 2) {
        return 0.0;
    }

    const double mean = dSum[i] / dFrames;
    return std::sqrt(std::max(0.0, dSumSq[i] / dFrames - mean * mean));
}

}
