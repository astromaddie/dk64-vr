#include "dk64vr_math.h"

#include <cmath>

namespace dk64vr {

Mat4 mul(const Mat4 &a, const Mat4 &b) {
    Mat4 r;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            float s = 0.0f;
            for (int k = 0; k < 4; k++) {
                s += a.m[i][k] * b.m[k][j];
            }
            r.m[i][j] = s;
        }
    }
    return r;
}

Vec3 transformPoint(const Vec3 &v, const Mat4 &m) {
    return {
        v.x * m.m[0][0] + v.y * m.m[1][0] + v.z * m.m[2][0] + m.m[3][0],
        v.x * m.m[0][1] + v.y * m.m[1][1] + v.z * m.m[2][1] + m.m[3][1],
        v.x * m.m[0][2] + v.y * m.m[1][2] + v.z * m.m[2][2] + m.m[3][2],
    };
}

Vec3 transformDir(const Vec3 &v, const Mat4 &m) {
    return {
        v.x * m.m[0][0] + v.y * m.m[1][0] + v.z * m.m[2][0],
        v.x * m.m[0][1] + v.y * m.m[1][1] + v.z * m.m[2][1],
        v.x * m.m[0][2] + v.y * m.m[1][2] + v.z * m.m[2][2],
    };
}

Mat4 rigidInverse(const Mat4 &m) {
    Mat4 r;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            r.m[i][j] = m.m[j][i];
        }
        r.m[i][3] = 0.0f;
    }

    // t' = -t * R^T
    for (int j = 0; j < 3; j++) {
        r.m[3][j] = -(m.m[3][0] * r.m[0][j] + m.m[3][1] * r.m[1][j] + m.m[3][2] * r.m[2][j]);
    }

    r.m[3][3] = 1.0f;
    return r;
}

Mat4 poseMatrix(const Pose &pose, float translationScale) {
    // Row i is where the child's axis i points in the parent: rotate(q, e_i).
    const Vec3 x = quatRotate(pose.q, { 1, 0, 0 });
    const Vec3 y = quatRotate(pose.q, { 0, 1, 0 });
    const Vec3 z = quatRotate(pose.q, { 0, 0, 1 });
    Mat4 r;
    r.m[0][0] = x.x; r.m[0][1] = x.y; r.m[0][2] = x.z; r.m[0][3] = 0.0f;
    r.m[1][0] = y.x; r.m[1][1] = y.y; r.m[1][2] = y.z; r.m[1][3] = 0.0f;
    r.m[2][0] = z.x; r.m[2][1] = z.y; r.m[2][2] = z.z; r.m[2][3] = 0.0f;
    r.m[3][0] = pose.p.x * translationScale;
    r.m[3][1] = pose.p.y * translationScale;
    r.m[3][2] = pose.p.z * translationScale;
    r.m[3][3] = 1.0f;
    return r;
}

Quat quatMul(const Quat &a, const Quat &b) {
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

Quat quatConj(const Quat &q) {
    return { -q.x, -q.y, -q.z, q.w };
}

Quat quatFromYaw(float radians) {
    return { 0.0f, std::sin(radians * 0.5f), 0.0f, std::cos(radians * 0.5f) };
}

Quat quatNormalize(const Quat &q) {
    const float len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (!(len > 1e-12f)) {
        return Quat();
    }

    return { q.x / len, q.y / len, q.z / len, q.w / len };
}

Vec3 quatRotate(const Quat &q, const Vec3 &v) {
    // v + 2w (u x v) + 2 u x (u x v)
    const Vec3 u = { q.x, q.y, q.z };
    const Vec3 t = { 2.0f * (u.y * v.z - u.z * v.y), 2.0f * (u.z * v.x - u.x * v.z), 2.0f * (u.x * v.y - u.y * v.x) };
    return {
        v.x + q.w * t.x + (u.y * t.z - u.z * t.y),
        v.y + q.w * t.y + (u.z * t.x - u.x * t.z),
        v.z + q.w * t.z + (u.x * t.y - u.y * t.x),
    };
}

float quatYaw(const Quat &q) {
    const Vec3 f = quatRotate(q, { 0, 0, -1 });
    return std::atan2(-f.x, -f.z);
}

Pose poseMul(const Pose &a, const Pose &b) {
    Pose r;
    r.q = quatNormalize(quatMul(a.q, b.q));
    const Vec3 rotated = quatRotate(a.q, b.p);
    r.p = { rotated.x + a.p.x, rotated.y + a.p.y, rotated.z + a.p.z };
    return r;
}

Pose poseInverse(const Pose &a) {
    Pose r;
    r.q = quatConj(a.q);
    const Vec3 p = quatRotate(r.q, a.p);
    r.p = { -p.x, -p.y, -p.z };
    return r;
}

Mat4 guPerspective(float fovyDegrees, float aspect, float nearZ, float farZ, float scale) {
    Mat4 mf;
    const float fovy = fovyDegrees * (3.1415926f / 180.0f);
    const float cot = std::cos(fovy / 2) / std::sin(fovy / 2);
    mf.m[0][0] = cot / aspect;
    mf.m[1][1] = cot;
    mf.m[2][2] = (nearZ + farZ) / (nearZ - farZ);
    mf.m[2][3] = -1;
    mf.m[3][2] = (2 * nearZ * farZ) / (nearZ - farZ);
    mf.m[3][3] = 0;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            mf.m[i][j] *= scale;
        }
    }

    return mf;
}

Mat4 guLookAt(const Vec3 &eye, const Vec3 &at, const Vec3 &upIn) {
    Mat4 mf;
    float xLook = at.x - eye.x, yLook = at.y - eye.y, zLook = at.z - eye.z;
    float len = -1.0f / std::sqrt(xLook * xLook + yLook * yLook + zLook * zLook);
    xLook *= len; yLook *= len; zLook *= len;

    float xUp = upIn.x, yUp = upIn.y, zUp = upIn.z;
    float xRight = yUp * zLook - zUp * yLook;
    float yRight = zUp * xLook - xUp * zLook;
    float zRight = xUp * yLook - yUp * xLook;
    len = 1.0f / std::sqrt(xRight * xRight + yRight * yRight + zRight * zRight);
    xRight *= len; yRight *= len; zRight *= len;

    xUp = yLook * zRight - zLook * yRight;
    yUp = zLook * xRight - xLook * zRight;
    zUp = xLook * yRight - yLook * xRight;
    len = 1.0f / std::sqrt(xUp * xUp + yUp * yUp + zUp * zUp);
    xUp *= len; yUp *= len; zUp *= len;

    mf.m[0][0] = xRight; mf.m[1][0] = yRight; mf.m[2][0] = zRight;
    mf.m[3][0] = -(eye.x * xRight + eye.y * yRight + eye.z * zRight);
    mf.m[0][1] = xUp; mf.m[1][1] = yUp; mf.m[2][1] = zUp;
    mf.m[3][1] = -(eye.x * xUp + eye.y * yUp + eye.z * zUp);
    mf.m[0][2] = xLook; mf.m[1][2] = yLook; mf.m[2][2] = zLook;
    mf.m[3][2] = -(eye.x * xLook + eye.y * yLook + eye.z * zLook);
    mf.m[0][3] = 0; mf.m[1][3] = 0; mf.m[2][3] = 0; mf.m[3][3] = 1;
    return mf;
}

Mat4 eyeProjection(const FovTangents &fov, const Mat4 &gameProj) {
    // The game's overall scale: -[2][3] is 1 for guPerspective(..., scale = 1).
    float s = -gameProj.m[2][3];
    if (!(std::fabs(s) > 1e-12f)) {
        s = 1.0f;
    }

    Mat4 r = gameProj;
    const float w = fov.right - fov.left;
    const float h = fov.up - fov.down;
    r.m[0][0] = s * 2.0f / w;
    r.m[0][1] = 0.0f;
    r.m[0][2] = 0.0f;
    r.m[0][3] = 0.0f;
    r.m[1][0] = 0.0f;
    r.m[1][1] = s * 2.0f / h;
    r.m[1][2] = 0.0f;
    r.m[1][3] = 0.0f;
    r.m[2][0] = s * (fov.right + fov.left) / w;
    r.m[2][1] = s * (fov.up + fov.down) / h;
    r.m[3][0] = 0.0f;
    r.m[3][1] = 0.0f;
    return r;
}

Mat4 levelView(const Mat4 &view) {
    // Camera axes in world space are the columns of the 3x3 part; its position is -t * R^T.
    const Mat4 camToWorld = rigidInverse(view);
    const Vec3 pos = { camToWorld.m[3][0], camToWorld.m[3][1], camToWorld.m[3][2] };
    const Vec3 back = { camToWorld.m[2][0], camToWorld.m[2][1], camToWorld.m[2][2] };
    const Vec3 up = { camToWorld.m[1][0], camToWorld.m[1][1], camToWorld.m[1][2] };
    Vec3 forward = { -back.x, 0.0f, -back.z };
    float len = std::sqrt(forward.x * forward.x + forward.z * forward.z);
    if (len < 1e-3f) {
        // Straight down: up points the way the camera faces. Straight up: the opposite way.
        const float sign = (-back.y < 0.0f) ? 1.0f : -1.0f;
        forward = { up.x * sign, 0.0f, up.z * sign };
        len = std::sqrt(forward.x * forward.x + forward.z * forward.z);
        if (len < 1e-6f) {
            forward = { 0.0f, 0.0f, -1.0f };
            len = 1.0f;
        }
    }

    forward.x /= len;
    forward.z /= len;
    return guLookAt(pos, { pos.x + forward.x, pos.y, pos.z + forward.z }, { 0.0f, 1.0f, 0.0f });
}

Mat4 eyeView(const Mat4 &gameView, const Pose &eyeInHead, float unitsPerMetre) {
    return mul(gameView, rigidInverse(poseMatrix(eyeInHead, unitsPerMetre)));
}

float depthToZw(const Mat4 &proj, float depth) {
    // A camera-space point at z = -depth.
    const float z = -depth;
    const float zc = z * proj.m[2][2] + proj.m[3][2];
    const float wc = z * proj.m[2][3] + proj.m[3][3];
    return zc / wc;
}

FogRemap fogRemap(const Mat4 &gameProj, const Mat4 &eyeProj) {
    // z/w = A + B/d with A = -m22/s... derived from two depths, which is exact for any
    // projection whose z/w is affine in 1/d.
    FogRemap r;
    const float d0 = 1.0f, d1 = 2.0f;
    const float e0 = depthToZw(eyeProj, d0), e1 = depthToZw(eyeProj, d1);
    const float g0 = depthToZw(gameProj, d0), g1 = depthToZw(gameProj, d1);
    if (!(std::fabs(e1 - e0) > 1e-12f)) {
        return r;
    }

    r.a = (g1 - g0) / (e1 - e0);
    r.b = g0 - r.a * e0;
    return r;
}

}
