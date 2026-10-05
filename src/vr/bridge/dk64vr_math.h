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
