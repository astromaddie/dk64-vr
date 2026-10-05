// Camera math for the DK64 VR eye passes. Pure: no OpenXR, no GPU, no game state.
//
// Conventions match RT64 and libultra: row vectors, v' = v * M, matrices stored m[row][col].
// Camera space is right-handed with +X right, +Y up and +Z behind the viewer (guLookAt and
// OpenXR agree on this). Poses follow OpenXR: p_parent = rotate(q, p_child) + p.
#pragma once

#include <cstdint>

namespace dk64vr {

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

struct Quat {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
};

struct Pose {
    Quat q;
    Vec3 p;
};

struct Mat4 {
    float m[4][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } };
};

// Tangents of the four half-angles of an eye's field of view, as OpenXR's XrFovf gives the
// angles: left and down are negative.
struct FovTangents {
    float left = -1.0f, right = 1.0f, up = 1.0f, down = -1.0f;
};

Mat4 mul(const Mat4 &a, const Mat4 &b);  // v * a * b
Vec3 transformPoint(const Vec3 &v, const Mat4 &m);
Vec3 transformDir(const Vec3 &v, const Mat4 &m);
Mat4 rigidInverse(const Mat4 &m);  // inverse of a rotation + translation
Mat4 poseMatrix(const Pose &pose, float translationScale = 1.0f);

Quat quatMul(const Quat &a, const Quat &b);  // apply b, then a
Quat quatConj(const Quat &q);
Quat quatFromYaw(float radians);  // about +Y
Quat quatNormalize(const Quat &q);
Vec3 quatRotate(const Quat &q, const Vec3 &v);
float quatYaw(const Quat &q);  // heading about +Y of the rotated -Z axis
Pose poseMul(const Pose &a, const Pose &b);  // b expressed in a's parent
Pose poseInverse(const Pose &a);

// The rotation whose columns are the orthonormal axes x, y, z.
Quat quatFromBasis(const Vec3 &x, const Vec3 &y, const Vec3 &z);
Vec3 cross(const Vec3 &a, const Vec3 &b);
float dot(const Vec3 &a, const Vec3 &b);
Vec3 normalize(const Vec3 &v);

// Layer placement (OpenXR quad layers: the quad lies in its pose's XY plane, facing +Z).
// A beam from `from` to `to`: the pose's +Y runs along the beam and the quad is turned about
// that axis to face `eye`. The quad is (width x |to - from|).
Pose beamPose(const Vec3 &from, const Vec3 &to, const Vec3 &eye);
// Head-leashed without roll (standard.md section 4): the head's yaw and pitch, never its roll,
// `distance` ahead of `headPos`.
Pose noRollPose(const Quat &head, const Vec3 &headPos, float distance);

// Heading of a view's camera (counter-clockwise from above, radians, as quatYaw) and its
// position.
float viewHeading(const Mat4 &view);
Vec3 viewPosition(const Mat4 &view);

// The snap turn's instant step (docs/VR_ARCHITECTURE.md, "Turning"): the game eases its camera
// to a new heading over a second or more, so until it arrives the eye view is the game view
// orbited about the point `pivotDistance` ahead of the camera (the look-at target, which the game
// camera itself orbits) by whatever the game camera has still to turn, so the view shows the
// target heading at once. Returns the orbited view; *delta gets the remaining turn.
Mat4 orbitView(const Mat4 &view, float targetHeadingRad, float pivotDistance, float *delta);
float wrapAngle(float radians);  // to -pi..pi

// 3D cinema (standard.md section 4, lessons 45 and 82). The game camera's picture goes on a
// world-locked screen, submitted as a projection layer: each eye's view has the screen's
// orientation and the eye's position, and its field of view is the screen rectangle as that eye
// sees it, so each eye's image lands exactly on the screen.
//
// Tangents (left, right, up, down; left and down negative) of the screen (centre `screen.p`, the
// quad in the pose's XY plane facing +Z, width x height metres) seen from `eye`.
FovTangents screenFov(const Pose &screen, float width, float height, const Vec3 &eye);

// The game camera split into two eyes for the screen: the eyes sit +-separation/2 along the
// camera's x axis and their frusta are shifted to converge at `convergence` game units (the
// game's near plane, so nothing drawn is in front of the screen and the scene keeps positive
// parallax). `gameTan` is the game camera's own frustum; the result is one eye's.
struct CinemaEye {
    Mat4 view;
    FovTangents tan;
};
CinemaEye cinemaEye(const Mat4 &gameView, const FovTangents &gameTan, float separation, float convergence, int eye);

// The separation (game units) that puts a point at infinity `infinityMetres` apart on a screen
// `screenWidth` metres wide (lesson 82's positive parallax only: 0 at the convergence distance,
// growing to this at infinity).
float cinemaSeparation(float infinityMetres, float screenWidth, const FovTangents &gameTan, float convergence);

// The game projection's symmetric frustum tangents (guPerspective: the scale cancels).
FovTangents gameFov(const Mat4 &gameProj);

// The HUD alpha solve, as lib/rt64/src/shaders/VrHudSolvePS.hlsl does it per pixel: from the
// HUD drawn over black (b = a * c) and over white (w = a * c + 1 - a), the coverage
// a = 1 - mean(w - b) and the unpremultiplied colour c = b / a (0 where a < 1/255).
void hudSolve(const float black[3], const float white[3], float colour[3], float &alpha);

// libultra transcriptions (guPerspectiveF, guLookAtF), used as references by the tests.
Mat4 guPerspective(float fovyDegrees, float aspect, float nearZ, float farZ, float scale);
Mat4 guLookAt(const Vec3 &eye, const Vec3 &at, const Vec3 &up);

// The eye projection: the runtime's asymmetric frustum in the game's projection form. The
// depth terms ([2][2], [3][2], [2][3], [3][3]) are the game's own, so z/w for a given eye-space
// depth, and everything derived from it (depth buffer, fog), is unchanged. The x/y terms carry
// the game's overall scale (guPerspective's `scale`, read from -[2][3]).
Mat4 eyeProjection(const FovTangents &fov, const Mat4 &gameProj);

// Drops a view's pitch and roll: the same camera position, facing the horizontal part of its
// forward direction with world +Y up. Looking straight down or up, the camera's up vector
// gives the heading.
Mat4 levelView(const Mat4 &view);

// The view for one eye: gameView * inverse(eye pose in the head frame), the pose's translation
// scaled from metres to game units. The game camera is the head origin.
Mat4 eyeView(const Mat4 &gameView, const Pose &eyeInHead, float unitsPerMetre);

// z/w = A + B/d for a standard perspective projection (d = eye-space depth > 0). With a different
// eye near plane, the game's fog (a function of z/w) needs the game's z/w for the same depth,
// which is linear in the eye's: zw_game = a * zw_eye + b.
struct FogRemap {
    float a = 1.0f, b = 0.0f;
};
FogRemap fogRemap(const Mat4 &gameProj, const Mat4 &eyeProj);
float depthToZw(const Mat4 &proj, float depth);

}
