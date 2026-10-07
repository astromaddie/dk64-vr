// Host unit tests for the DK64 VR bridge: the display-time scheduler, the eye projection and
// view, level pitch, the fog remap and the settings file. No window, GPU or runtime.
//
//   tools/docker/build-linux.sh vrtest     (or any C++17 compiler, see src/vr/tests/CMakeLists.txt)
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "dk64vr_camsmooth.h"
#include "dk64vr_input.h"
#include "dk64vr_foveation.h"
#include "dk64vr_math.h"
#include "dk64vr_sched.h"
#include "dk64vr_settings.h"
#include "dk64vr_log.h"
#include "shared/rt64_projection_fog.h"
#include "shared/rt64_vr_billboard.h"
#include "dk64vr_menu.h"
#include "sfvr/sfvr_adjust.h"

using namespace dk64vr;

static int failures = 0;
static int checks = 0;

#define CHECK(cond) do { checks++; if (!(cond)) { failures++; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_NEAR(a, b, eps) do { checks++; const double va = (a), vb = (b); if (!(std::fabs(va - vb) <= (eps))) { failures++; std::printf("FAIL %s:%d: %s = %.9g, %s = %.9g (eps %g)\n", __FILE__, __LINE__, #a, va, #b, vb, double(eps)); } } while (0)

static bool matNear(const Mat4 &a, const Mat4 &b, float eps) {
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            if (!(std::fabs(a.m[i][j] - b.m[i][j]) <= eps)) {
                std::printf("  [%d][%d] %.9g vs %.9g\n", i, j, a.m[i][j], b.m[i][j]);
                return false;
            }
        }
    }
    return true;
}

static Vec3 clipToNdc(const Vec3 &cam, const Mat4 &proj, float *wOut = nullptr) {
    const float x = cam.x * proj.m[0][0] + cam.y * proj.m[1][0] + cam.z * proj.m[2][0] + proj.m[3][0];
    const float y = cam.x * proj.m[0][1] + cam.y * proj.m[1][1] + cam.z * proj.m[2][1] + proj.m[3][1];
    const float z = cam.x * proj.m[0][2] + cam.y * proj.m[1][2] + cam.z * proj.m[2][2] + proj.m[3][2];
    const float w = cam.x * proj.m[0][3] + cam.y * proj.m[1][3] + cam.z * proj.m[2][3] + proj.m[3][3];
    if (wOut != nullptr) {
        *wOut = w;
    }

    return { x / w, y / w, z / w };
}

// ---------------------------------------------------------------------------------------------
// Projection

static void testEyeProjectionMatchesGuPerspective() {
    // Symmetric tangents reproduce guPerspective, for DK64's camera (fov 45, near 10) and for
    // first person (fov 13), at several aspects, and with a non-unit scale.
    const float fovs[] = { 45.0f, 13.0f, 90.0f };
    const float aspects[] = { 4.0f / 3.0f, 1.0f, 16.0f / 9.0f };
    const float scales[] = { 1.0f, 0.5f };
    for (float fov : fovs) {
        for (float aspect : aspects) {
            for (float scale : scales) {
                const Mat4 game = guPerspective(fov, aspect, 10.0f, 6000.0f, scale);
                const float t = std::tan(fov * 3.1415926f / 360.0f);
                FovTangents tan;
                tan.left = -t * aspect;
                tan.right = t * aspect;
                tan.up = t;
                tan.down = -t;
                const Mat4 eye = eyeProjection(tan, game);
                CHECK(matNear(eye, game, 2e-5f * std::max(1.0f, std::fabs(game.m[3][2]))));
            }
        }
    }
}

static void testEyeProjectionAsymmetricEdges() {
    // The Frame's left eye (platform.md): points on each frustum edge land on NDC +-1.
    FovTangents tan;
    tan.left = std::tan(-1.028f);
    tan.right = std::tan(0.885f);
    tan.up = std::tan(0.864f);
    tan.down = std::tan(-1.048f);
    const Mat4 game = guPerspective(45.0f, 4.0f / 3.0f, 10.0f, 6000.0f, 1.0f);
    const Mat4 eye = eyeProjection(tan, game);
    const float d = 250.0f;
    CHECK_NEAR(clipToNdc({ tan.right * d, 0.0f, -d }, eye).x, 1.0, 1e-5);
    CHECK_NEAR(clipToNdc({ tan.left * d, 0.0f, -d }, eye).x, -1.0, 1e-5);
    CHECK_NEAR(clipToNdc({ 0.0f, tan.up * d, -d }, eye).y, 1.0, 1e-5);
    CHECK_NEAR(clipToNdc({ 0.0f, tan.down * d, -d }, eye).y, -1.0, 1e-5);

    // Same depth terms: z/w at a depth is the game's (so depth and fog are unchanged).
    const float depths[] = { 10.0f, 37.0f, 500.0f, 5999.0f };
    for (float depth : depths) {
        CHECK_NEAR(depthToZw(eye, depth), depthToZw(game, depth), 1e-6);
    }

    // w is the eye-space depth times the game's scale, as in the game's projection.
    float w = 0.0f;
    clipToNdc({ 1.0f, 2.0f, -d }, eye, &w);
    CHECK_NEAR(w, d, 1e-4);
}

// ---------------------------------------------------------------------------------------------
// View composition

static Pose eyePose(float yawRad, float px, float py, float pz) {
    Pose p;
    p.q = quatFromYaw(yawRad);
    p.p = { px, py, pz };
    return p;
}

static void testEyeViewIdentityPose() {
    const Mat4 game = guLookAt({ 100.0f, 50.0f, -30.0f }, { 140.0f, 45.0f, -90.0f }, { 0.0f, 1.0f, 0.0f });
    CHECK(matNear(eyeView(game, Pose(), 25.0f), game, 1e-5f));
}

static void testEyeViewTranslationAndScale() {
    // Camera at the origin looking down -Z. A left eye 32 mm left of the head: a world point
    // straight ahead of the head appears 32 mm * 25 units/m = 0.8 units to the right of the eye.
    const Mat4 game = guLookAt({ 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, -1.0f }, { 0.0f, 1.0f, 0.0f });
    const Mat4 view = eyeView(game, eyePose(0.0f, -0.032f, 0.0f, 0.0f), 25.0f);
    const Vec3 p = transformPoint({ 0.0f, 0.0f, -100.0f }, view);
    CHECK_NEAR(p.x, 0.8, 1e-5);
    CHECK_NEAR(p.y, 0.0, 1e-5);
    CHECK_NEAR(p.z, -100.0, 1e-4);

    // Head 0.1 m up: the world moves down by 2.5 units.
    const Mat4 up = eyeView(game, eyePose(0.0f, 0.0f, 0.1f, 0.0f), 25.0f);
    CHECK_NEAR(transformPoint({ 0.0f, 0.0f, -100.0f }, up).y, -2.5, 1e-5);
}

static void testEyeViewYaw() {
    // Head turned 90 degrees left: what was on the game camera's left is now straight ahead.
    const Mat4 game = guLookAt({ 10.0f, 20.0f, 30.0f }, { 10.0f, 20.0f, 0.0f }, { 0.0f, 1.0f, 0.0f });
    const Mat4 view = eyeView(game, eyePose(3.1415926f / 2.0f, 0.0f, 0.0f, 0.0f), 25.0f);
    // Game camera faces -Z from z=30, so its left is -X.
    const Vec3 p = transformPoint({ 10.0f - 50.0f, 20.0f, 30.0f }, view);
    CHECK_NEAR(p.x, 0.0, 1e-4);
    CHECK_NEAR(p.z, -50.0, 1e-4);

    // And a turn by 180 degrees puts the point behind the game camera in front of the eye.
    const Mat4 back = eyeView(game, eyePose(3.1415926f, 0.0f, 0.0f, 0.0f), 25.0f);
    const Vec3 q = transformPoint({ 10.0f, 20.0f, 30.0f + 40.0f }, back);
    CHECK_NEAR(q.x, 0.0, 1e-3);
    CHECK_NEAR(q.z, -40.0, 1e-3);
}

static void testEyeDisparityDirection() {
    // Both eyes see a near point; the left eye sees it further right than the right eye does
    // (crossed toward the nose), and the two agree in y.
    const Mat4 game = guLookAt({ 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, -1.0f }, { 0.0f, 1.0f, 0.0f });
    const Mat4 proj = guPerspective(45.0f, 1.0f, 10.0f, 6000.0f, 1.0f);
    const Vec3 point = { 0.0f, 3.0f, -50.0f };
    const Vec3 l = clipToNdc(transformPoint(point, eyeView(game, eyePose(0.0f, -0.032f, 0.0f, 0.0f), 25.0f)), proj);
    const Vec3 r = clipToNdc(transformPoint(point, eyeView(game, eyePose(0.0f, 0.032f, 0.0f, 0.0f), 25.0f)), proj);
    CHECK(l.x > 0.0f);
    CHECK(r.x < 0.0f);
    CHECK_NEAR(l.y, r.y, 1e-6);
}

static void testLevelView() {
    // A chase camera pitched down 30 degrees and rolled: levelled, it keeps its position and
    // heading, looks along the horizon, and has world up.
    const Vec3 eye = { 200.0f, 80.0f, 300.0f };
    const float pitch = -30.0f * 3.1415926f / 180.0f;
    const Vec3 dir = { std::cos(pitch) * std::sin(0.7f), std::sin(pitch), -std::cos(pitch) * std::cos(0.7f) };
    const Mat4 game = guLookAt(eye, { eye.x + dir.x, eye.y + dir.y, eye.z + dir.z }, { 0.2f, 1.0f, 0.0f });
    const Mat4 level = levelView(game);
    const Mat4 camToWorld = rigidInverse(level);
    CHECK_NEAR(camToWorld.m[3][0], eye.x, 1e-3);
    CHECK_NEAR(camToWorld.m[3][1], eye.y, 1e-3);
    CHECK_NEAR(camToWorld.m[3][2], eye.z, 1e-3);
    // back axis horizontal, up axis is +Y
    CHECK_NEAR(camToWorld.m[2][1], 0.0, 1e-6);
    CHECK_NEAR(camToWorld.m[1][1], 1.0, 1e-6);
    // the heading is the game's
    const float hx = -camToWorld.m[2][0], hz = -camToWorld.m[2][2];
    CHECK_NEAR(std::atan2(hx, -hz), 0.7, 1e-4);

    // Straight down: the heading comes from the camera's up vector.
    const Mat4 down = guLookAt(eye, { eye.x, eye.y - 1.0f, eye.z }, { 0.0f, 0.0f, -1.0f });
    const Mat4 downLevel = rigidInverse(levelView(down));
    CHECK_NEAR(-downLevel.m[2][2], -1.0, 1e-5);
}

static void testPoseMath() {
    const Pose a = eyePose(0.4f, 1.0f, 2.0f, 3.0f);
    const Pose b = eyePose(-1.1f, -0.5f, 0.25f, 4.0f);
    const Pose ab = poseMul(a, b);
    const Mat4 mab = mul(poseMatrix(b), poseMatrix(a));
    CHECK(matNear(poseMatrix(ab), mab, 1e-5f));
    const Pose i = poseMul(poseInverse(a), a);
    CHECK(matNear(poseMatrix(i), Mat4(), 1e-5f));
    CHECK_NEAR(quatYaw(quatFromYaw(0.9f)), 0.9, 1e-5);
    CHECK_NEAR(quatYaw(quatFromYaw(-2.5f)), -2.5, 1e-5);
}

// ---------------------------------------------------------------------------------------------
// Fog remap

static void testFogRemap() {
    const Mat4 game = guPerspective(45.0f, 4.0f / 3.0f, 10.0f, 6000.0f, 1.0f);
    const Mat4 nearer = guPerspective(45.0f, 4.0f / 3.0f, 2.0f, 6000.0f, 1.0f);
    const FogRemap r = fogRemap(game, nearer);
    const float depths[] = { 10.0f, 25.0f, 333.0f, 4000.0f, 5999.0f };
    for (float d : depths) {
        CHECK_NEAR(r.a * depthToZw(nearer, d) + r.b, depthToZw(game, d), 2e-5);
    }

    // Identity when the depth terms agree.
    const FogRemap same = fogRemap(game, game);
    CHECK_NEAR(same.a, 1.0, 1e-5);
    CHECK_NEAR(same.b, 0.0, 1e-5);
}

static void testEyeNearAndFog() {
    const FovTangents fov = { -1.66f, 1.22f, 1.17f, -1.73f };
    float minimumMargin = 1.0f;
    for (float farZ : { 1000.0f, 6000.0f, 30000.0f }) {
        for (float scale : { 1.0f, 0.5f }) {
            Mat4 game = guPerspective(45, 4.0f / 3, 10, farZ, scale);
            // N64 input matrices can be s15.16. The eye projection is subsequently FLOAT,
            // so it must not inherit a fixed-point rounding of its new depth coefficients.
            for (auto &row : game.m) for (float &v : row) v = std::round(v * 65536.0f) / 65536.0f;
            const Mat4 old = eyeProjection(fov, game);
            const Mat4 fallback = eyeProjection(fov, game, 0);
            CHECK(std::memcmp(&old, &fallback, sizeof(old)) == 0);
            const double gameFar = double(game.m[3][2]) / (double(game.m[2][2]) - game.m[2][3]);
            for (float units : { 4.0f, 8.0f, 15.0f, 100.0f }) {
                for (float metres : { 0.05f, 0.10f, 0.25f }) {
                    const float nearZ = metres * units;
                    const Mat4 eye = eyeProjection(fov, game, nearZ);
                    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) {
                        if ((c == 2) && ((r == 2) || (r == 3))) continue;
                        CHECK_NEAR(eye.m[r][c], old.m[r][c], 0);
                    }
                    CHECK_NEAR(depthToZw(eye, nearZ), -1, 3e-7);
                    CHECK_NEAR(depthToZw(eye, float(gameFar)), 1, 3e-7);
                    // RSP's standard 511/1024 viewport maps NDC -1 to depth 0, +1 to 1022/1024.
                    CHECK_NEAR((depthToZw(eye, nearZ) + 1) * (511.0 / 1024), 0, 2e-7);
                    CHECK_NEAR(clipToNdc({ fov.left * nearZ, 0, -nearZ }, eye).x, -1, 3e-7);
                    CHECK_NEAR(clipToNdc({ fov.right * nearZ, 0, -nearZ }, eye).x, 1, 3e-7);
                    CHECK_NEAR(clipToNdc({ 0, fov.up * nearZ, -nearZ }, eye).y, 1, 3e-7);
                    CHECK_NEAR(clipToNdc({ 0, fov.down * nearZ, -nearZ }, eye).y, -1, 3e-7);
                    const FogRemap fog = fogRemap(game, eye); // production coefficient helper
                    for (float viewportScale : { 511.0f / 1024.0f, 0.5f }) {
                        const auto screen = RT64::projectionScreenRemap(fog.a, fog.b, viewportScale, viewportScale);
                        const float cap = 1022.0f / 1024.0f;
                        const float eyeCap = RT64::projectionEyeDepth(cap, screen.a, screen.b);
                        CHECK_NEAR(RT64::projectionGameDepth(eyeCap, screen.a, screen.b), cap, 4e-6);
                        const double capZw = (double(cap) - viewportScale) / viewportScale;
                        const double ga = double(game.m[2][2]) / game.m[2][3];
                        const double gb = -double(game.m[3][2]) / game.m[2][3];
                        const float capDistance = float(gb / (capZw - ga));
                        const float eyeAtCap = depthToZw(eye, capDistance) * viewportScale + viewportScale;
                        CHECK_NEAR(eyeAtCap, eyeCap, 3e-7); // original F3D far cutoff in metres
                        for (float primitive : { 0.25f, 0.5f, 0.9f, cap }) {
                            const float converted = RT64::projectionEyeDepth(primitive, screen.a, screen.b);
                            CHECK_NEAR(RT64::projectionGameDepth(converted, screen.a, screen.b), primitive, 4e-6);
                        }
                        // Decal coplanar tolerance and primitive dz use the inverse slope;
                        // screen-space derivatives already follow the eye projection naturally.
                        const float gameTolerance = 0.0000625f, eyeTolerance = gameTolerance / std::fabs(screen.a);
                        CHECK_NEAR(eyeTolerance * std::fabs(screen.a), gameTolerance, 1e-10);
                        const float surface = RT64::projectionEyeDepth(0.97f, screen.a, screen.b);
                        for (float offset : { 0.25f, 4.0f }) {
                            const float decal = RT64::projectionEyeDepth(0.97f + offset * gameTolerance, screen.a, screen.b);
                            CHECK((std::fabs(decal - surface) <= eyeTolerance) == (offset < 1));
                        }
                    }
                    for (float d : { nearZ * 0.5f, nearZ, 5.0f, 10.0f, 25.0f, 333.0f, float(gameFar * 0.9), float(gameFar) }) {
                        const float eyeZ = -d * eye.m[2][2] + eye.m[3][2], eyeW = -d * eye.m[2][3];
                        const float gameZ = -d * game.m[2][2] + game.m[3][2], gameW = -d * game.m[2][3];
                        // Calls the very function included by RSPProcessCS, including clamp order.
                        const float remappedZ = RT64::projectionFogZ(eyeZ, eyeW, fog.a, fog.b);
                        const float got = std::max(remappedZ, 0.0f) / eyeW;
                        const float expected = std::max(gameZ, 0.0f) / gameW;
                        CHECK_NEAR(got, expected, 2e-5);
                        for (float mul : { -150.0f, 500.0f, -32768.0f }) {
                            const float offset = 255.0f - mul * expected;
                            const float ga = std::clamp((got * mul + offset) / 255.0f, 0.0f, 1.0f);
                            const float ea = std::clamp((expected * mul + offset) / 255.0f, 0.0f, 1.0f);
                            CHECK_NEAR(ga, ea, 1.0 / 255); // <1 fog-alpha byte, even worst s16 mul
                        }
                    }
                    // Combined float view-projection at large world coordinates, after N64
                    // view rounding; sky follows the game camera, the closer eye is displaced.
                    const Vec3 original = { 10000, -3000, 9000 }, origin = { 10000, -3015, 8920 };
                    Mat4 view = guLookAt(origin, { origin.x, origin.y, origin.z - 1 }, { 0, 1, 0 });
                    for (auto &row : view.m) for (float &v : row) v = std::round(v * 65536.0f) / 65536.0f;
                    const Mat4 vp = mul(view, eye);
                    const Vec3 sky = { original.x, original.y, original.z - float(gameFar * 0.9) };
                    const float zw = clipToNdc(sky, vp).z;
                    const float margin = 1.0f - zw;
                    CHECK(margin > 0); // no far-plane crossing
                    if (metres == 0.10f) {
                        minimumMargin = std::min(minimumMargin, margin);
                        const float depth = (zw + 1.0f) * (511.0f / 1024.0f);
                        const float depthLimit = 1022.0f / 1024.0f;
                        const float ulp = depthLimit - std::nextafter(depthLimit, 0.0f);
                        CHECK((depthLimit - depth) / ulp >= 16.0f);
                    }
                }
            }
        }
    }
    std::printf("eye near 0.10 m: minimum tested sky NDC margin %.8g (far <=30000, scale >=4)\n", minimumMargin);
    CHECK_NEAR(RT64::projectionFogZ(0.7f, 123, 1, 0), 0.7f, 0);
}

static void testEyeNearSettings() {
    dk64vr_settings_t s = {}, saved = {};
    dk64vr_settings_defaults(&s);
    dk64vr_settings_defaults(&saved);
    const int index = dk64vr_settings_index("eye_near_m");
    CHECK(index >= 0);
    const sfvr_setting_key *key = dk64vr_settings_key(index);
    CHECK(key->type == SFVR_SETTING_FLOAT);
    CHECK_NEAR(key->min, 0, 0); CHECK_NEAR(key->max, 0.25, 0);
    CHECK_NEAR(dk64vr_settings_get(&s, "eye_near_m"), 0.1, 1e-9);
    dk64vr_settings_set(&s, "eye_near_m", 0.12);
    CHECK(dk64vr_settings_apply_env(&s, [](const char *name) -> const char * {
        return std::strcmp(name, "DK64_VR_EYE_NEAR_M") == 0 ? "0.08" : nullptr;
    }) == 1);
    CHECK_NEAR(dk64vr_settings_get(&s, "eye_near_m"), double(0.08f), 0);
    CHECK_NEAR(s.file_values[index], double(0.12f), 0);
    size_t length = 0;
    char *text = dk64vr_settings_format(&s, &length);
    CHECK(text != nullptr);
    if (text) CHECK(dk64vr_settings_parse(&saved, text, length) == 0);
    CHECK_NEAR(saved.file_values[index], 0.12, 1e-6);
    CHECK_NEAR(saved.file_values[dk64vr_settings_index("port_schema")], 5, 0);
    std::free(text);
    dk64vr_settings_set(&s, "eye_near_m", -1); CHECK_NEAR(dk64vr_settings_get(&s, "eye_near_m"), 0, 0);
    dk64vr_settings_set(&s, "eye_near_m", 99); CHECK_NEAR(dk64vr_settings_get(&s, "eye_near_m"), 0.25, 0);
    const char *fallback = "version=1\n[vr]\nport_schema=5\neye_near_m=0\nworld_scale=4\ncamera_closer=0.8\n";
    // This sparse A/B config adds every missing registry key, without changing its four choices.
    CHECK(dk64vr_settings_parse(&saved, fallback, std::strlen(fallback)) == int(dk64vr_settings_count()) - 4);
    CHECK_NEAR(saved.file_values[index], 0, 0);
    CHECK_NEAR(saved.file_values[dk64vr_settings_index("world_scale")], 4, 0);
    CHECK_NEAR(saved.file_values[dk64vr_settings_index("camera_closer")], 0.8, 1e-6);
    dk64vr_settings_free(&s); dk64vr_settings_free(&saved);
}

// ---------------------------------------------------------------------------------------------
// Scheduler

struct SimResult {
    std::vector<double> shown;  // game time shown, in ticks, per display frame
    uint64_t rebases = 0;
    double maxStepError = 0.0;
    double minStep = 1e9;
    double maxLag = 0.0;
    double minLag = 1e9;
};

// Game ticks at k * tick (plus jitter), display frames every period, consumed when the frame
// wakes (two periods before its display time), at most one per frame.
static SimResult simulate(double tickHz, double displayHz, double seconds, double jitter, double stallAt = -1.0, double stallLen = 0.0) {
    SimResult res;
    TickScheduler s;
    const double tick = 1.0 / tickHz, period = 1.0 / displayHz, latency = 2.0 * period;
    s.period_s = period;
    int64_t produced = 0;  // ticks the game has produced
    int64_t consumed = -1;  // index of the newest consumed tick
    double nextTick = 0.0;
    uint32_t rng = 12345;
    auto rand01 = [&]() { rng = rng * 1664525u + 1013904223u; return double(rng >> 8) / double(1u << 24); };
    const int frames = int(seconds * displayHz);
    for (int j = 0; j < frames; j++) {
        const double wake = j * period;
        const double display = wake + latency;
        while (nextTick <= wake) {
            produced++;
            // A stall delays every later tick: the game does not catch up with a burst, it
            // reports one long frame (DK64's frame delta).
            const double shift = ((stallAt >= 0.0) && (produced * tick >= stallAt)) ? stallLen : 0.0;
            const double next = produced * tick + shift + (rand01() - 0.5) * jitter;
            nextTick = std::max(next, nextTick);
        }

        if (consumed + 1 < produced) {
            consumed++;
            s.arrive(display, tick);
        }

        if (consumed >= 1) {
            res.shown.push_back(double(consumed - 1) + s.weight(display));
            const double lag = display / tick - res.shown.back();
            if (j > frames / 10) {
                res.maxLag = std::max(res.maxLag, lag);
                res.minLag = std::min(res.minLag, lag);
            }
        }
    }

    res.rebases = s.rebases;
    for (size_t i = 1; i < res.shown.size(); i++) {
        const double step = res.shown[i] - res.shown[i - 1];
        res.minStep = std::min(res.minStep, step);
        if (i > res.shown.size() / 10) {
            res.maxStepError = std::max(res.maxStepError, std::fabs(step - tickHz / displayHz));
        }
    }

    return res;
}

static void testSchedulerSteady120() {
    const SimResult r = simulate(30.0, 120.0, 20.0, 0.0);
    CHECK(r.rebases == 1);
    CHECK(r.minStep >= -1e-9);
    CHECK_NEAR(r.maxStepError, 0.0, 0.02);
    // about one tick of latency
    CHECK(r.maxLag < 2.3);
    CHECK(r.minLag > 0.5);
}

static void testSchedulerSteady90And72() {
    const SimResult r90 = simulate(30.0, 90.0, 20.0, 0.0);
    CHECK(r90.minStep >= -1e-9);
    CHECK(r90.maxStepError < 0.03);
    const SimResult r72 = simulate(30.0, 72.0, 20.0, 0.0);
    CHECK(r72.minStep >= -1e-9);
    // 72 / 30 is not an integer: arrivals land on varying phases, all within one frame of the
    // earliest, which the grid follows.
    CHECK(r72.maxStepError < 0.01);
    CHECK(r72.rebases == 1);
}

static void testSchedulerJitter() {
    // Game frames up to +-4 ms off their tick: shown time never goes backwards, and steps stay
    // even (an arrival within one frame of the earliest phase is seamless).
    const SimResult r = simulate(30.0, 120.0, 30.0, 0.008);
    CHECK(r.minStep >= -1e-9);
    CHECK(r.maxStepError < 0.01);
    CHECK(r.rebases == 1);
}

static void testSchedulerTwentyHz() {
    const SimResult r = simulate(20.0, 120.0, 10.0, 0.0);
    CHECK(r.minStep >= -1e-9);
    CHECK(r.maxStepError < 0.01);
}

static void testSchedulerDrift() {
    // The game's clock 0.5% slow against the display: the grid follows without re-anchoring,
    // and only the rare frame where the drift crosses a display period is uneven.
    const SimResult r = simulate(29.85, 120.0, 60.0, 0.0);
    CHECK(r.rebases == 1);
    CHECK(r.minStep >= -1e-9);
    int uneven = 0;
    for (size_t i = r.shown.size() / 10; i < r.shown.size(); i++) {
        uneven += std::fabs((r.shown[i] - r.shown[i - 1]) - 29.85 / 120.0) > 0.01;
    }
    CHECK(uneven < int(r.shown.size() / 100));
}

static void testSchedulerStall() {
    // A 400 ms load: the image holds, then the grid re-anchors once without a jump backwards.
    const SimResult r = simulate(30.0, 120.0, 10.0, 0.0, 4.0, 0.4);
    CHECK(r.minStep >= -1e-9);
    CHECK(r.rebases == 2);
}

static void testSchedulerWeightEdges() {
    TickScheduler s;
    CHECK_NEAR(s.weight(5.0), 1.0, 0.0);  // not anchored: show the newest
    CHECK(s.arrive(10.0, 1.0 / 30.0));
    CHECK_NEAR(s.weight(10.0), 0.0, 1e-6);  // arrival continues the previous image
    CHECK_NEAR(s.weight(10.0 + 0.5 / 30.0), 0.5, 1e-6);
    CHECK_NEAR(s.weight(10.0 + 2.0 / 30.0), 1.0, 0.0);
    // A change of rate re-anchors.
    // A change of rate re-anchors once it has held for rate_hold arrivals.
    CHECK(!s.arrive(10.0 + 1.0 / 30.0, 1.0 / 20.0));
    CHECK(!s.arrive(10.0 + 1.0 / 30.0 + 0.05, 1.0 / 20.0));
    CHECK(s.arrive(10.0 + 1.0 / 30.0 + 0.10, 1.0 / 20.0));
    CHECK_NEAR(s.tick_s, 0.05, 1e-12);
    // A one-off long frame does not.
    const uint64_t rebases = s.rebases;
    s.arrive(10.0 + 1.0 / 30.0 + 0.15, 1.0 / 10.0);
    s.arrive(10.0 + 1.0 / 30.0 + 0.20, 1.0 / 20.0);
    CHECK(s.rebases == rebases);
    CHECK_NEAR(s.tick_s, 0.05, 1e-12);
}

// ---------------------------------------------------------------------------------------------
// Settings

static std::vector<std::string> logLines;
static void captureLog(const char *line) {
    logLines.emplace_back(line);
}

static const char *autoEnv(const char *name) {
    return (std::strcmp(name, "DK64_VR_RESOLUTION_SCALE") == 0) ? "auto" : nullptr;
}

static const char *fakeEnv(const char *name) {
    if (std::strcmp(name, "DK64_VR_WORLD_SCALE") == 0) {
        return "40";
    }

    if (std::strcmp(name, "DK64_VR_FORCE_RENDER") == 0) {
        return "true";
    }

    return nullptr;
}

static void testSettings() {
    dk64vr_log_set_sink(captureLog);
    dk64vr_settings_t s;
    std::memset(&s, 0, sizeof(s));
    dk64vr_settings_defaults(&s);
    CHECK_NEAR(dk64vr_settings_get(&s, "refresh_rate"), 72.0, 0.0);
    CHECK_NEAR(dk64vr_settings_get(&s, "resolution_scale"), DK64VR_RESOLUTION_AUTO, 0.0);
    CHECK_NEAR(dk64vr_settings_get(&s, "camera_smoothing"), 2.0, 0.0);
    CHECK_NEAR(dk64vr_settings_get(&s, "port_schema"), 5.0, 0.0);
    CHECK_NEAR(dk64vr_settings_get(&s, "world_scale"), 15.0, 0.0);
    CHECK_NEAR(dk64vr_settings_get(&s, "hud_distance"), 1.5, 1e-9);
    CHECK_NEAR(dk64vr_settings_get(&s, "turn_mode"), 0.0, 0.0);
    CHECK_NEAR(dk64vr_settings_get(&s, "snap_angle"), 30.0, 0.0);
    CHECK_NEAR(dk64vr_settings_get(&s, "defaults_profile"), 0.0, 0.0);
    CHECK_NEAR(dk64vr_settings_get(&s, "settings_page_side"), 2.0, 0.0);
    CHECK_NEAR(dk64vr_settings_default_for_profile(&s, "turn_mode"), 0.0, 0.0);
    CHECK_NEAR(dk64vr_settings_default_for_profile(&s, "snap_angle"), 30.0, 0.0);
    dk64vr_settings_set(&s, "defaults_profile", 1.0);
    CHECK_NEAR(dk64vr_settings_default_for_profile(&s, "turn_mode"), 1.0, 0.0);
    CHECK_NEAR(dk64vr_settings_default_for_profile(&s, "smooth_turn_speed"), 120.0, 0.0);
    CHECK_NEAR(dk64vr_settings_default_for_profile(&s, "snap_angle"), 30.0, 0.0);
    dk64vr_settings_set(&s, "defaults_profile", 0.0);
    const sfvr_setting_key *scaleKey = dk64vr_settings_key((size_t)dk64vr_settings_index("world_scale"));
    CHECK(scaleKey != nullptr && scaleKey->min <= 15.0 / 4.0 && scaleKey->max >= 15.0 * 4.0);
    CHECK_NEAR(dk64vr_settings_get(&s, "right_stick_x"), 0.0, 0.0);
    CHECK_NEAR(dk64vr_settings_get(&s, "cutscenes"), 0.0, 0.0);
    CHECK_NEAR(dk64vr_settings_get(&s, "level_pitch"), 1.0, 0.0);
    CHECK_NEAR(dk64vr_settings_get(&s, "foveation"), 3.0, 0.0);
    CHECK_NEAR(dk64vr_settings_get(&s, "foveation_eye_tracked"), 1.0, 0.0);

    // A file with a clamped value, a bad value, an unknown key and a comment.
    const char *text =
        "# mine\n"
        "version = 1\n"
        "[vr]\n"
        "world_scale = 500 # too big\n"
        "timing = maybe\n"
        "my_own_key = 3\n"
        "dump_frame = 600\n";
    const int repairs = dk64vr_settings_parse(&s, text, std::strlen(text));
    CHECK(repairs > 0);
    CHECK_NEAR(s.file_values[dk64vr_settings_index("world_scale")], 100.0, 0.0);
    CHECK_NEAR(s.file_values[dk64vr_settings_index("timing")], 0.0, 0.0);
    CHECK_NEAR(s.file_values[dk64vr_settings_index("dump_frame")], 600.0, 0.0);
    CHECK_NEAR(s.file_values[dk64vr_settings_index("resolution_scale")], DK64VR_RESOLUTION_AUTO, 0.0);

    // Round trip: what format() writes parses back to the same values with nothing to repair.
    size_t len = 0;
    char *out = dk64vr_settings_format(&s, &len);
    CHECK(out != nullptr);
    if (out != nullptr) {
        CHECK(std::strstr(out, "my_own_key = 3") != nullptr);
        CHECK(std::strstr(out, "resolution_scale = \"auto\"") != nullptr);
        CHECK(std::strstr(out, "camera_smoothing = \"medium\"") != nullptr);
        CHECK(std::strstr(out, "# mine") != nullptr);
        dk64vr_settings_t again;
        std::memset(&again, 0, sizeof(again));
        dk64vr_settings_defaults(&again);
        CHECK(dk64vr_settings_parse(&again, out, len) == 0);
        for (size_t i = 0; i < dk64vr_settings_count(); i++) {
            CHECK_NEAR(again.file_values[i], s.file_values[i], 1e-6);
        }

        dk64vr_settings_free(&again);
        std::free(out);
    }

    for (size_t i = 0; i < dk64vr_settings_count(); i++) {
        s.values[i] = s.file_values[i];
    }

    CHECK(dk64vr_settings_apply_env(&s, fakeEnv) == 2);
    CHECK_NEAR(dk64vr_settings_get(&s, "world_scale"), 40.0, 0.0);
    CHECK_NEAR(dk64vr_settings_get(&s, "force_render"), 1.0, 0.0);
    // the environment never reaches the file
    CHECK_NEAR(s.file_values[dk64vr_settings_index("world_scale")], 100.0, 0.0);
    CHECK(dk64vr_settings_count() <= DK64VR_SETTINGS_MAX_KEYS);

    // Schema migrations (lesson 79): old defaults in an older file move to the new ones once and
    // values the user chose stay. resolution_scale went 1.25 -> 1.0 (schema 2), back to 1.25
    // (schema 4) and to auto (schema 5).
    const char *old =
        "version = 1\n"
        "[vr]\n"
        "world_scale = 25\n"
        "resolution_scale = 1.25\n"
        "refresh_rate = 90\n";
    CHECK(dk64vr_settings_parse(&s, old, std::strlen(old)) > 0);
    CHECK_NEAR(s.file_values[dk64vr_settings_index("world_scale")], 15.0, 0.0);
    CHECK_NEAR(s.file_values[dk64vr_settings_index("resolution_scale")], DK64VR_RESOLUTION_AUTO, 0.0);
    CHECK_NEAR(s.file_values[dk64vr_settings_index("refresh_rate")], 90.0, 0.0);
    CHECK_NEAR(s.file_values[dk64vr_settings_index("port_schema")], 5.0, 0.0);
    // A schema-3 file still at the old default 1.0 moves on to auto; one the user set to 0.75 stays.
    const char *schema3 =
        "version = 1\n"
        "[vr]\n"
        "port_schema = 3\n"
        "resolution_scale = 1.0\n";
    CHECK(dk64vr_settings_parse(&s, schema3, std::strlen(schema3)) > 0);
    CHECK_NEAR(s.file_values[dk64vr_settings_index("resolution_scale")], DK64VR_RESOLUTION_AUTO, 0.0);
    // The Frame's vr.toml: schema 4 at the old default 1.25 moves to auto; 1.0 chosen at schema 4
    // stays; auto parses by name (any case) and from the environment.
    const char *schema4Old =
        "version = 1\n"
        "[vr]\n"
        "port_schema = 4\n"
        "resolution_scale = 1.25\n";
    CHECK(dk64vr_settings_parse(&s, schema4Old, std::strlen(schema4Old)) > 0);
    CHECK_NEAR(s.file_values[dk64vr_settings_index("resolution_scale")], DK64VR_RESOLUTION_AUTO, 0.0);
    CHECK_NEAR(s.file_values[dk64vr_settings_index("port_schema")], 5.0, 0.0);
    const char *schema4Chosen =
        "version = 1\n"
        "[vr]\n"
        "port_schema = 4\n"
        "resolution_scale = 1.0\n";
    dk64vr_settings_parse(&s, schema4Chosen, std::strlen(schema4Chosen));
    CHECK_NEAR(s.file_values[dk64vr_settings_index("resolution_scale")], 1.0, 1e-9);
    const char *autoNamed =
        "version = 1\n"
        "[vr]\n"
        "port_schema = 5\n"
        "resolution_scale = \"Auto\"\n"
        "camera_smoothing = high\n";
    dk64vr_settings_parse(&s, autoNamed, std::strlen(autoNamed));
    CHECK_NEAR(s.file_values[dk64vr_settings_index("resolution_scale")], DK64VR_RESOLUTION_AUTO, 0.0);
    CHECK_NEAR(s.file_values[dk64vr_settings_index("camera_smoothing")], 3.0, 0.0);
    dk64vr_settings_set(&s, "resolution_scale", 0.0);
    CHECK_NEAR(dk64vr_settings_get(&s, "resolution_scale"), DK64VR_RESOLUTION_AUTO, 0.0);
    dk64vr_settings_set(&s, "resolution_scale", 9.0);
    CHECK_NEAR(dk64vr_settings_get(&s, "resolution_scale"), 1.5, 1e-6);
    CHECK(dk64vr_settings_apply_env(&s, autoEnv) == 1);
    CHECK_NEAR(dk64vr_settings_get(&s, "resolution_scale"), DK64VR_RESOLUTION_AUTO, 0.0);
    const char *schema3Chosen =
        "version = 1\n"
        "[vr]\n"
        "port_schema = 3\n"
        "resolution_scale = 0.75\n";
    dk64vr_settings_parse(&s, schema3Chosen, std::strlen(schema3Chosen));
    CHECK_NEAR(s.file_values[dk64vr_settings_index("resolution_scale")], 0.75, 1e-9);
    // New optional keys need no schema migration. A schema-4 file with a chosen
    // scale gets the defaults (high, eye-tracked) for missing foveation keys and keeps that scale.
    dk64vr_settings_t prior = {};
    dk64vr_settings_defaults(&prior);
    prior.file_values[dk64vr_settings_index("resolution_scale")] = 1.0;
    size_t priorLength = 0;
    char *priorText = dk64vr_settings_format(&prior, &priorLength);
    std::string schema4(priorText ? priorText : "", priorText ? priorLength : 0);
    free(priorText);
    dk64vr_settings_free(&prior);
    for (const char *key : { "foveation", "foveation_eye_tracked", "dump_foveation" }) {
        const size_t line = schema4.find("\n" + std::string(key) + " = ");
        CHECK(line != std::string::npos);
        if (line != std::string::npos) {
            const size_t start = line + 1;
            schema4.erase(start, schema4.find('\n', start) - start + 1);
        }
    }
    // Exactly the three absent keys are inserted; no schema/default migration.
    CHECK(dk64vr_settings_parse(&s, schema4.data(), schema4.size()) == 3);
    CHECK_NEAR(s.file_values[dk64vr_settings_index("resolution_scale")], 1.0, 1e-9);
    CHECK_NEAR(s.file_values[dk64vr_settings_index("foveation")], 3.0, 0.0);
    CHECK_NEAR(s.file_values[dk64vr_settings_index("foveation_eye_tracked")], 1.0, 0.0);
    // Schema 5: a schema-4 file at the old foveation defaults (off, false) takes high and
    // eye-tracked; a level the user chose stays.
    const char *fov4 =
        "version = 1\n"
        "[vr]\n"
        "port_schema = 4\n"
        "foveation = \"off\"\n"
        "foveation_eye_tracked = false\n";
    dk64vr_settings_parse(&s, fov4, std::strlen(fov4));
    CHECK_NEAR(s.file_values[dk64vr_settings_index("foveation")], 3.0, 0.0);
    CHECK_NEAR(s.file_values[dk64vr_settings_index("foveation_eye_tracked")], 1.0, 0.0);
    const char *fov4Chosen =
        "version = 1\n"
        "[vr]\n"
        "port_schema = 4\n"
        "foveation = \"low\"\n"
        "foveation_eye_tracked = true\n";
    dk64vr_settings_parse(&s, fov4Chosen, std::strlen(fov4Chosen));
    CHECK_NEAR(s.file_values[dk64vr_settings_index("foveation")], 1.0, 0.0);
    CHECK_NEAR(s.file_values[dk64vr_settings_index("foveation_eye_tracked")], 1.0, 0.0);
    // snap_angle: the old default 30 moves to 45 at schema 5; 60 chosen stays.
    const char *snap4 = "version = 1\n[vr]\nport_schema = 4\nsnap_angle = 30\n";
    dk64vr_settings_parse(&s, snap4, std::strlen(snap4));
    CHECK_NEAR(s.file_values[dk64vr_settings_index("snap_angle")], 45.0, 0.0);
    const char *snap4Chosen = "version = 1\n[vr]\nport_schema = 4\nsnap_angle = 60\n";
    dk64vr_settings_parse(&s, snap4Chosen, std::strlen(snap4Chosen));
    CHECK_NEAR(s.file_values[dk64vr_settings_index("snap_angle")], 60.0, 0.0);
    const char *fov5Off =
        "version = 1\n"
        "[vr]\n"
        "port_schema = 5\n"
        "foveation = \"off\"\n";
    dk64vr_settings_parse(&s, fov5Off, std::strlen(fov5Off));
    CHECK_NEAR(s.file_values[dk64vr_settings_index("foveation")], 0.0, 0.0);
    const char *chosen =
        "version = 1\n"
        "[vr]\n"
        "port_schema = 2\n"
        "world_scale = 25\n"
        "snap_blink = 0.15\n";
    dk64vr_settings_parse(&s, chosen, std::strlen(chosen));
    CHECK_NEAR(s.file_values[dk64vr_settings_index("world_scale")], 25.0, 0.0);
    CHECK_NEAR(s.file_values[dk64vr_settings_index("snap_blink")], 0.0, 0.0);
    // Enum keys by name.
    const char *names =
        "[vr]\n"
        "right_stick_x = camera\n"
        "cutscenes = panel\n";
    dk64vr_settings_parse(&s, names, std::strlen(names));
    CHECK_NEAR(s.file_values[dk64vr_settings_index("right_stick_x")], 1.0, 0.0);
    CHECK_NEAR(s.file_values[dk64vr_settings_index("cutscenes")], 2.0, 0.0);
    dk64vr_settings_free(&s);
    dk64vr_log_set_sink(nullptr);
}

static void testFoveation() {
    const FovTangents symmetric = { -1, 1, 1, -1 };
    CHECK_NEAR(projectionCentre(symmetric).x, 0.5, 1e-6);
    CHECK_NEAR(projectionCentre(symmetric).y, 0.5, 1e-6);
    const FovTangents asymmetric = { -2, 1, 1, -3 };
    CHECK_NEAR(projectionCentre(asymmetric).x, 2.0 / 3.0, 1e-6);
    CHECK_NEAR(projectionCentre(asymmetric).y, 0.25, 1e-6);
    CHECK_NEAR(gazeCentre(asymmetric, { 0, 0, -1 }).x, projectionCentre(asymmetric).x, 1e-6);
    CHECK_NEAR(gazeCentre(asymmetric, { 0, 0, -1 }).y, projectionCentre(asymmetric).y, 1e-6);
    CHECK_NEAR(gazeCentre(asymmetric, { -2, 1, -1 }).x, 0, 1e-6);
    CHECK_NEAR(gazeCentre(asymmetric, { -2, 1, -1 }).y, 0, 1e-6);
    CHECK_NEAR(gazeCentre(asymmetric, { 1, -3, -1 }).x, 1, 1e-6);
    CHECK_NEAR(gazeCentre(asymmetric, { 1, -3, -1 }).y, 1, 1e-6);
    CHECK_NEAR(gazeCentre(asymmetric, { 1, 1, 1 }).x, projectionCentre(asymmetric).x, 1e-6);
    CHECK_NEAR(gazeCentre(symmetric, { NAN, 0, -1 }).x, 0.5, 1e-6);
    CHECK_NEAR(gazeCentre(symmetric, { 10, 10, -1 }).x, 1, 1e-6);
    CHECK_NEAR(gazeCentre(symmetric, { 10, 10, -1 }).y, 0, 1e-6);
    CHECK_NEAR(projectionCentre({ 0, 0, 0, 0 }).x, 0.5, 1e-6);

    const uint16_t supported = (1u << 0) | (1u << 5) | (1u << 10); // 1x1, 2x2, 4x4
    const auto off = makeShadingRateMap(2176, 2176, 2160, 2160, 32, 32, {}, FoveationLevel::Off, supported);
    CHECK(off.width == 68 && off.height == 68);
    CHECK(std::all_of(off.rates.begin(), off.rates.end(), [](uint8_t r) { return r == 0; }));
    const auto high = makeShadingRateMap(2176, 2176, 2160, 2160, 32, 32, {}, FoveationLevel::High, supported);
    CHECK(high.rates[0] == 10);
    CHECK(high.rates[33 * high.width + 33] == 0);
    const auto low = makeShadingRateMap(2176, 2176, 2160, 2160, 32, 32, {}, FoveationLevel::Low, supported);
    const auto medium = makeShadingRateMap(2176, 2176, 2160, 2160, 32, 32, {}, FoveationLevel::Medium, supported);
    CHECK(low.rates[0] == 5);
    auto full = [](const ShadingRateMap &m) { return std::count(m.rates.begin(), m.rates.end(), 0); };
    CHECK(full(low) > full(medium) && full(medium) > full(high));
    // Every tile intersecting the inner disc stays full density, including its edge.
    CHECK(high.rates[33 * high.width + 47] == 0);
    const auto moved = makeShadingRateMap(2160, 2160, 2160, 2160, 32, 32, { 0.1f, 0.1f }, FoveationLevel::High, supported);
    CHECK(moved.rates[6 * moved.width + 6] == 0);
    CHECK(moved.rates[33 * moved.width + 33] == 10);
    const auto fallback = makeShadingRateMap(100, 100, 100, 100, 16, 16, {}, FoveationLevel::High, (1u << 0) | (1u << 5));
    CHECK(fallback.width == 7 && fallback.height == 7);
    CHECK(std::all_of(fallback.rates.begin(), fallback.rates.end(), [](uint8_t r) { return r == 0 || r == 5; }));
    const auto unsupported = makeShadingRateMap(100, 100, 100, 100, 16, 16, {}, FoveationLevel::High, 1);
    CHECK(std::all_of(unsupported.rates.begin(), unsupported.rates.end(), [](uint8_t r) { return r == 0; }));
    // Low never exceeds two pixels on either axis, even when only 1x4 is offered.
    const auto wideOnly = makeShadingRateMap(100, 100, 100, 100, 16, 16, {}, FoveationLevel::Low, (1u << 0) | (1u << 2));
    CHECK(std::all_of(wideOnly.rates.begin(), wideOnly.rates.end(), [](uint8_t r) { return r == 0; }));
    CHECK(makeShadingRateMap(0, 100, 100, 100, 16, 16, {}, FoveationLevel::High, supported).rates.empty());
    const auto visualization = shadingRateVisualization(high);
    CHECK(visualization.size() == high.rates.size() * 4);
    CHECK(visualization[3] == 255 && visualization[1] == 130);

    // New settings round-trip, repair bad enum names, and keep env overrides out of the file.
    dk64vr_settings_t s = {};
    dk64vr_settings_defaults(&s);
    const char *text = "version = 1\n[vr]\nport_schema = 4\nresolution_scale = 0.75\nfoveation = high\nfoveation_eye_tracked = true\ndump_foveation = true\n";
    dk64vr_settings_parse(&s, text, strlen(text));
    CHECK_NEAR(s.file_values[dk64vr_settings_index("foveation")], 3, 0);
    size_t length = 0;
    char *formatted = dk64vr_settings_format(&s, &length);
    dk64vr_settings_t roundTrip = {};
    dk64vr_settings_defaults(&roundTrip);
    CHECK(dk64vr_settings_parse(&roundTrip, formatted, length) == 0);
    CHECK_NEAR(roundTrip.file_values[dk64vr_settings_index("port_schema")], 5, 0);  // migrated; 0.75 kept
    CHECK_NEAR(roundTrip.file_values[dk64vr_settings_index("resolution_scale")], 0.75, 1e-9);
    CHECK_NEAR(roundTrip.file_values[dk64vr_settings_index("dump_foveation")], 1, 0);
    CHECK_NEAR(roundTrip.file_values[dk64vr_settings_index("foveation_eye_tracked")], 1, 0);
    CHECK(dk64vr_settings_apply_env(&s, [](const char *name) -> const char * {
        return strcmp(name, "DK64_VR_FOVEATION") == 0 ? "low" : nullptr;
    }) == 1);
    CHECK_NEAR(dk64vr_settings_get(&s, "foveation"), 1, 0);
    CHECK_NEAR(s.file_values[dk64vr_settings_index("foveation")], 3, 0);
    free(formatted);
    dk64vr_settings_free(&roundTrip);
    const char *bad = "[vr]\nfoveation = extreme\nfoveation_eye_tracked = maybe\n";
    dk64vr_settings_parse(&s, bad, strlen(bad));
    CHECK_NEAR(s.file_values[dk64vr_settings_index("foveation")], 3, 0);  // bad values take the defaults
    CHECK_NEAR(s.file_values[dk64vr_settings_index("foveation_eye_tracked")], 1, 0);
    dk64vr_settings_free(&s);
}

// ---------------------------------------------------------------------------------------------
// Input: action names, bindings, the N64 map and the merge

static void testActionNamesUnique() {
    // Lesson 35: a duplicate name or localized name fails xrCreateAction and nothing attaches.
    for (int i = 0; i < ActCount; i++) {
        const ActionDef &a = actionDef(i);
        CHECK(std::strlen(a.name) > 0 && std::strlen(a.name) < 64);
        CHECK(std::strlen(a.localized) > 0 && std::strlen(a.localized) < 128);
        for (const char *c = a.name; *c; c++) {
            CHECK((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_' || *c == '-' || *c == '.');
        }
        for (int j = i + 1; j < ActCount; j++) {
            CHECK(std::strcmp(a.name, actionDef(j).name) != 0);
            CHECK(std::strcmp(a.localized, actionDef(j).localized) != 0);
        }
    }
}

// Component lists (OpenXR spec; Valve's Frame profile as Halo CE VR, F-Zero X VR and GoldenEye VR
// bind it, goldeneye-vr/tools/steam_frame/tests/check_bindings.py).
struct ProfileComponents {
    const char *profile;
    std::vector<std::string> components;
};

static std::vector<ProfileComponents> knownProfiles() {
    const std::vector<std::string> poses = { "input/grip/pose", "input/aim/pose", "output/haptic" };
    const std::vector<std::string> stick = { "input/thumbstick", "input/thumbstick/x", "input/thumbstick/y", "input/thumbstick/click", "input/thumbstick/touch" };
    auto join = [](std::vector<std::string> a, const std::vector<std::string> &b) { a.insert(a.end(), b.begin(), b.end()); return a; };
    std::vector<ProfileComponents> list;
    list.push_back({ "/interaction_profiles/valve/frame_controller_valve", join(join({ "input/a/click", "input/b/click", "input/x/click", "input/y/click", "input/menu/click",
        "input/view/click", "input/bumper/click", "input/trigger/value", "input/trigger/click", "input/squeeze/value", "input/dpad_up/click", "input/dpad_down/click",
        "input/dpad_left/click", "input/dpad_right/click" }, stick), poses) });
    list.push_back({ "/interaction_profiles/valve/index_controller", join(join({ "input/system/click", "input/system/touch", "input/a/click", "input/a/touch", "input/b/click",
        "input/b/touch", "input/squeeze/value", "input/squeeze/force", "input/trigger/click", "input/trigger/value", "input/trigger/touch", "input/trackpad",
        "input/trackpad/x", "input/trackpad/y", "input/trackpad/touch", "input/trackpad/force" }, stick), poses) });
    list.push_back({ "/interaction_profiles/oculus/touch_controller", join(join({ "input/x/click", "input/x/touch", "input/y/click", "input/y/touch", "input/menu/click",
        "input/a/click", "input/a/touch", "input/b/click", "input/b/touch", "input/squeeze/value", "input/trigger/value", "input/trigger/touch",
        "input/thumbrest/touch" }, stick), poses) });
    return list;
}

static bool hasComponent(const ProfileComponents &p, const std::string &hand, const std::string &component) {
    // Touch's Menu is on the left only (the right one is the system button); x/y left, a/b right.
    if (std::strstr(p.profile, "touch_controller")) {
        if (component == "input/menu/click" || component == "input/x/click" || component == "input/y/click") {
            if (hand != "left") return false;
        }
        if (component == "input/a/click" || component == "input/b/click") {
            if (hand != "right") return false;
        }
    }
    for (const std::string &c : p.components) {
        if (c == component) return true;
    }
    return false;
}

static void testBindingPaths() {
    // Lesson 33: one path a profile lacks makes the runtime refuse the whole profile.
    const std::vector<ProfileComponents> known = knownProfiles();
    CHECK(profileCount() == 3);
    CHECK(std::strcmp(profileAt(0).profile, "/interaction_profiles/valve/frame_controller_valve") == 0);
    CHECK(profileAt(0).needsFrameExtension);
    for (int i = 0; i < profileCount(); i++) {
        const ProfileBindings &pb = profileAt(i);
        const ProfileComponents *comps = nullptr;
        for (const ProfileComponents &k : known) {
            if (std::strcmp(k.profile, pb.profile) == 0) comps = &k;
        }
        CHECK(comps != nullptr);
        if (comps == nullptr) continue;
        bool bound[ActCount] = {};
        for (int b = 0; b < pb.count; b++) {
            const std::string path = pb.bindings[b].path;
            const std::string prefix = "/user/hand/";
            CHECK(path.rfind(prefix, 0) == 0);
            const size_t slash = path.find('/', prefix.size());
            const std::string hand = path.substr(prefix.size(), slash - prefix.size());
            const std::string component = path.substr(slash + 1);
            CHECK(hand == "left" || hand == "right");
            const bool ok = hasComponent(*comps, hand, component);
            if (!ok) std::printf("  %s: %s is not a component\n", pb.profile, path.c_str());
            CHECK(ok);
            // Kinds: poses on pose actions, haptics on haptic actions, vectors on the stick.
            const ActionKind kind = actionDef(pb.bindings[b].action).kind;
            CHECK((kind == ActionKind::Pose) == (component.find("/pose") != std::string::npos));
            CHECK((kind == ActionKind::Haptic) == (component == "output/haptic"));
            CHECK((kind == ActionKind::Vector2) == (component == "input/thumbstick"));
            CHECK(!bound[pb.bindings[b].action] || kind == ActionKind::Pose);
            bound[pb.bindings[b].action] = true;
        }
        // Every gameplay and system-layer action is reachable on every profile (standard section 2).
        const int needed[] = { ActA, ActX, ActMenu, ActView, ActLBumper, ActRBumper, ActLTrigger, ActRTrigger, ActLStick, ActRStick, ActAimLeft, ActAimRight, ActHapticLeft, ActHapticRight };
        for (int a : needed) {
            if (!bound[a]) std::printf("  %s does not bind %s\n", pb.profile, actionDef(a).name);
            CHECK(bound[a]);
        }
        if (i == 0) {
            for (int a = 0; a < ActCount; a++) {
                if (!bound[a]) std::printf("  the Frame profile does not bind %s\n", actionDef(a).name);
                CHECK(bound[a]);
            }
        }
    }
    // The Frame modifier uses a Float action bound to the validated squeeze/value path.
    const ProfileBindings &frame = profileAt(0);
    CHECK(actionDef(ActLGrip).kind == ActionKind::Float);
    bool frameGripBound = false;
    for (int b = 0; b < frame.count; b++) {
        if (frame.bindings[b].action == ActLGrip) {
            frameGripBound = true;
            CHECK(std::strcmp(frame.bindings[b].path, "/user/hand/left/input/squeeze/value") == 0);
        }
    }
    CHECK(frameGripBound);

    // The Frame is asymmetric: these must be on the hand the Frame has them on.
    for (int b = 0; b < frame.count; b++) {
        const std::string path = frame.bindings[b].path;
        const bool left = path.find("/user/hand/left/") == 0;
        const int a = frame.bindings[b].action;
        if (a == ActA || a == ActB || a == ActX || a == ActY || a == ActMenu) CHECK(!left);
        if (a == ActView || a == ActDpadUp || a == ActDpadDown || a == ActDpadLeft || a == ActDpadRight) CHECK(left);
    }
}

static InputSample sampleIdle() {
    InputSample s;
    s.valid = true;
    return s;
}

static void testN64Map() {
    InputMapper m;
    InputConfig config;
    InputContext context;
    context.stereoPlay = true;
    InputSample s = sampleIdle();
    s.a = s.x = s.y = s.b = s.menu = s.lBumper = s.rBumper = s.rStickClick = true;
    s.dpadUp = s.dpadLeft = true;
    s.lTrigger = 0.9f;
    s.rTrigger = 0.9f;
    m.update(sampleIdle(), config, context, 0.9, 1.0f / 72.0f);
    InputResult r = m.update(s, config, context, 1.0, 1.0f / 72.0f);
    // DK64 Recompiled's defaults on an Xbox-layout pad (src/game/config.cpp set_control_defaults).
    // Start follows Menu once the Menu + View chord can no longer form (0.15 s).
    const uint16_t all = n64::A | n64::B | n64::CLeft | n64::CRight | n64::CDown | n64::CUp | n64::L | n64::DUp | n64::DLeft | n64::Z | n64::R;
    CHECK(r.buttons == all);
    r = m.update(s, config, context, 1.16, 1.0f / 72.0f);
    CHECK(r.buttons == (all | n64::Start));
    // Trigger hysteresis: 0.4 keeps Z held, 0.3 releases it, 0.45 does not re-press.
    s = sampleIdle();
    s.lTrigger = 0.4f;
    r = m.update(s, config, context, 1.18, 1.0f / 72.0f);
    CHECK((r.buttons & n64::Z) != 0);
    s.lTrigger = 0.3f;
    r = m.update(s, config, context, 1.19, 1.0f / 72.0f);
    CHECK((r.buttons & n64::Z) == 0);
    s.lTrigger = 0.45f;
    r = m.update(s, config, context, 1.195, 1.0f / 72.0f);
    CHECK((r.buttons & n64::Z) == 0);
    // Right stick Y does nothing in VR (lesson 80).
    s = sampleIdle();
    s.rStick[1] = 1.0f;
    r = m.update(s, config, context, 1.1, 1.0f / 72.0f);
    CHECK(r.buttons == 0 && r.turnDeg == 0.0f);
    // Focus lost: everything released.
    s = sampleIdle();
    s.a = true;
    s.valid = false;
    r = m.update(s, config, context, 1.2, 1.0f / 72.0f);
    CHECK(r.buttons == 0 && r.stickX == 0.0f && r.stickY == 0.0f);
}

static void testStickRelativeToView() {
    InputMapper m;
    InputConfig config;
    InputContext context;
    context.stereoPlay = true;
    context.viewYawRad = 3.1415926f / 2.0f;  // looking 90 degrees left of the game camera
    InputSample s = sampleIdle();
    s.lStick[1] = 1.0f;  // forward
    InputResult r = m.update(s, config, context, 1.0, 0.014f);
    CHECK_NEAR(r.stickX, -1.0, 1e-5);  // forward where the head looks: left of the camera
    CHECK_NEAR(r.stickY, 0.0, 1e-5);
    // Not in stereo play (a panel scene) or with the setting off: the raw stick.
    context.stereoPlay = false;
    r = m.update(s, config, context, 1.1, 0.014f);
    CHECK_NEAR(r.stickY, 1.0, 1e-5);
    context.stereoPlay = true;
    config.moveFromView = false;
    r = m.update(s, config, context, 1.2, 0.014f);
    CHECK_NEAR(r.stickY, 1.0, 1e-5);
    // Deadzone.
    s.lStick[0] = 0.1f;
    s.lStick[1] = 0.05f;
    r = m.update(s, config, context, 1.3, 0.014f);
    CHECK(r.stickX == 0.0f && r.stickY == 0.0f);
    float x = 1.0f, y = 0.0f;
    rotateStick(3.1415926f / 2.0f, x, y);
    CHECK_NEAR(x, 0.0, 1e-5);
    CHECK_NEAR(y, 1.0, 1e-5);
}

static void testTurnAndView() {
    InputMapper m;
    InputConfig config;
    InputContext context;
    context.stereoPlay = true;
    InputSample s = sampleIdle();
    double t = 1.0;
    // Snap: one step per flick, re-armed under 0.3.
    s.rStick[0] = 0.9f;
    CHECK_NEAR(m.update(s, config, context, t += 0.014, 0.014f).turnDeg, 45.0, 1e-5);
    CHECK_NEAR(m.update(s, config, context, t += 0.014, 0.014f).turnDeg, 0.0, 1e-5);
    s.rStick[0] = 0.1f;
    m.update(s, config, context, t += 0.014, 0.014f);
    s.rStick[0] = -0.9f;
    CHECK_NEAR(m.update(s, config, context, t += 0.014, 0.014f).turnDeg, -45.0, 1e-5);
    // The game's own camera instead: C-left / C-right.
    config.rightStickX = RightStickX::GameCamera;
    InputResult r = m.update(s, config, context, t += 0.014, 0.014f);
    CHECK(r.buttons == n64::CLeft && r.turnDeg == 0.0f);
    config.rightStickX = RightStickX::Turn;
    // No turning outside stereo play.
    context.stereoPlay = false;
    s.rStick[0] = 0.0f;
    m.update(s, config, context, t += 0.014, 0.014f);
    s.rStick[0] = 0.9f;
    CHECK(m.update(s, config, context, t += 0.014, 0.014f).turnDeg == 0.0f);
    // View: a short press toggles the recomp menu (held for two reads); 1 s recentres with a buzz.
    m.reset();
    s = sampleIdle();
    t = 10.0;
    m.update(s, config, context, t, 0.014f);
    s.view = true;
    m.update(s, config, context, t += 0.1, 0.014f);
    s.view = false;
    r = m.update(s, config, context, t += 0.1, 0.014f);
    CHECK((r.navHeld & nav::ToggleMenu) != 0 && !r.recentre);
    r = m.update(s, config, context, t += 0.014, 0.014f);
    CHECK((r.navHeld & nav::ToggleMenu) != 0);
    r = m.update(s, config, context, t += 0.014, 0.014f);
    CHECK((r.navHeld & nav::ToggleMenu) == 0);
    s.view = true;
    m.update(s, config, context, t += 0.014, 0.014f);
    r = m.update(s, config, context, t += 1.1, 0.014f);
    CHECK(r.recentre && r.systemBuzz);
    s.view = false;
    r = m.update(s, config, context, t += 0.1, 0.014f);
    CHECK((r.navHeld & nav::ToggleMenu) == 0 && !r.recentre);
    s.view = true;
    m.update(s, config, context, t += 0.1, 0.014f);
    r = m.update(s, config, context, t += 3.1, 0.014f);
    CHECK(r.recentre && r.recalibrateHeight && r.systemBuzz);
    s.view = false;
    r = m.update(s, config, context, t += 0.1, 0.014f);
    CHECK(!r.recalibrateHeight);
}

static void testLiveAdjustment() {
    sfvr_adjust_config config;
    sfvr_adjust_config_default(&config);
    CHECK_NEAR(config.deadzone, 0.20, 1e-6);
    CHECK_NEAR(config.world_size_factor_per_s, 1.5, 1e-6);
    CHECK_NEAR(config.camera_units_per_s, 0.25, 1e-6);
    CHECK_NEAR(config.max_dt_s, 1.0 / 30.0, 1e-6);
    config.world_scale_min = 25.0f;
    config.world_scale_max = 400.0f;

    sfvr_adjust_state state;
    sfvr_adjust_init(&state);
    float worldScale = 100.0f, closer = 0.0f, lower = 0.0f;
    float stick[2] = { 1.0f, 1.0f };
    CHECK(!sfvr_adjust_update(&state, &config, stick, 1, 1, 0, 0, 1.0, &worldScale, &closer, &lower));
    CHECK_NEAR(stick[0], 0.0, 0.0);
    CHECK_NEAR(stick[1], 0.0, 0.0);
    stick[0] = stick[1] = 1.0f;
    CHECK(sfvr_adjust_update(&state, &config, stick, 1, 1, 0, 0, 1.1, &worldScale, &closer, &lower));
    CHECK_NEAR(worldScale, 100.0 * std::exp(-std::log(1.5) / 30.0), 1e-4);
    CHECK_NEAR(closer, -0.25 / 30.0, 1e-5);
    CHECK_NEAR(lower, -0.25 / 30.0, 1e-5);
    CHECK_NEAR(stick[0], 0.0, 0.0);
    CHECK_NEAR(stick[1], 0.0, 0.0);

    // Menu and View win over the modifier while still consuming its right stick.
    sfvr_adjust_init(&state);
    worldScale = 100.0f; closer = lower = 0.0f;
    stick[0] = 1.0f; stick[1] = 0.0f;
    sfvr_adjust_update(&state, &config, stick, 1, 1, 0, 0, 2.0, &worldScale, &closer, &lower);
    stick[0] = 1.0f;
    CHECK(!sfvr_adjust_update(&state, &config, stick, 1, 1, 1, 0, 2.01, &worldScale, &closer, &lower));
    CHECK_NEAR(worldScale, 100.0, 0.0);
    CHECK_NEAR(stick[0], 0.0, 0.0);

    // On release, a deflected stick remains muted until a neutral sample, then returns to gameplay.
    sfvr_adjust_init(&state);
    stick[0] = 0.8f; stick[1] = 0.0f;
    sfvr_adjust_update(&state, &config, stick, 1, 1, 0, 0, 3.0, &worldScale, &closer, &lower);
    stick[0] = 0.8f;
    sfvr_adjust_update(&state, &config, stick, 0, 1, 0, 0, 3.01, &worldScale, &closer, &lower);
    CHECK_NEAR(stick[0], 0.0, 0.0);
    stick[0] = stick[1] = 0.0f;
    sfvr_adjust_update(&state, &config, stick, 0, 1, 0, 0, 3.02, &worldScale, &closer, &lower);
    stick[0] = 0.8f;
    sfvr_adjust_update(&state, &config, stick, 0, 1, 0, 0, 3.03, &worldScale, &closer, &lower);
    CHECK_NEAR(stick[0], 0.8, 1e-6);

    sfvr_adjust_focus_lost(&state);
    stick[0] = 0.8f;
    sfvr_adjust_update(&state, &config, stick, 0, 1, 0, 0, 4.0, &worldScale, &closer, &lower);
    CHECK_NEAR(stick[0], 0.0, 0.0);
    stick[0] = stick[1] = 0.0f;
    sfvr_adjust_update(&state, &config, stick, 0, 1, 0, 0, 4.01, &worldScale, &closer, &lower);
    stick[0] = 0.8f;
    sfvr_adjust_update(&state, &config, stick, 0, 1, 0, 0, 4.02, &worldScale, &closer, &lower);
    CHECK_NEAR(stick[0], 0.8, 1e-6);

    // Predicted-display-time integration gives the same scale change at the supported rates.
    float scaleAtRate[3] = {};
    const int rates[] = { 72, 90, 120 };
    for (int r = 0; r < 3; r++) {
        sfvr_adjust_init(&state);
        worldScale = 100.0f; closer = lower = 0.0f;
        stick[0] = 1.0f; stick[1] = 0.0f;
        sfvr_adjust_update(&state, &config, stick, 1, 1, 0, 0, 0.0, &worldScale, &closer, &lower);
        for (int frame = 1; frame <= rates[r]; frame++) {
            stick[0] = 1.0f;
            sfvr_adjust_update(&state, &config, stick, 1, 1, 0, 0,
                double(frame) / rates[r], &worldScale, &closer, &lower);
        }
        scaleAtRate[r] = worldScale;
    }
    for (int r = 0; r < 3; r++) CHECK_NEAR(scaleAtRate[r], 100.0 / 1.5, 3e-4);
    CHECK_NEAR(scaleAtRate[0], scaleAtRate[1], 2e-4);
    CHECK_NEAR(scaleAtRate[1], scaleAtRate[2], 2e-4);
}

static void testMenuNavigation() {
    InputMapper m;
    InputConfig config;
    InputContext context;
    context.menuOpen = true;
    InputSample s = sampleIdle();
    s.a = true;
    s.lStick[1] = -0.9f;
    s.rBumper = true;
    InputResult r = m.update(s, config, context, 1.0, 0.014f);
    CHECK(r.buttons == 0);  // the game sees nothing while a menu is open
    CHECK(r.navHeld == (nav::Accept | nav::Down | nav::TabRight));
    s = sampleIdle();
    s.b = true;
    s.dpadLeft = true;
    r = m.update(s, config, context, 1.1, 0.014f);
    CHECK(r.navHeld == (nav::Back | nav::Left));
}

static void testMerge() {
    InputResult vr;
    vr.buttons = n64::A;
    vr.stickX = 0.5f;
    vr.stickY = 0.0f;
    uint16_t buttons = n64::B;
    float x = 0.0f, y = 0.2f;
    mergeN64(buttons, x, y, vr);
    CHECK(buttons == (n64::A | n64::B));
    CHECK_NEAR(x, 0.5, 1e-6);
    CHECK_NEAR(y, 0.0, 1e-6);
    // A real stick pushed further wins.
    x = 0.0f;
    y = -0.9f;
    mergeN64(buttons, x, y, vr);
    CHECK_NEAR(y, -0.9, 1e-6);
    // An idle VR pad changes nothing.
    InputResult idle;
    x = 0.3f;
    y = 0.1f;
    buttons = n64::Z;
    mergeN64(buttons, x, y, idle);
    CHECK(buttons == n64::Z);
    CHECK_NEAR(x, 0.3, 1e-6);
    CHECK(!triggerClick(false, 0.59f) && triggerClick(false, 0.6f) && triggerClick(true, 0.46f) && !triggerClick(true, 0.45f));
}

static void testLayerPoses() {
    // quatFromBasis round trip.
    const Quat q = quatNormalize({ 0.2f, -0.4f, 0.1f, 0.85f });
    const Quat r = quatFromBasis(quatRotate(q, { 1, 0, 0 }), quatRotate(q, { 0, 1, 0 }), quatRotate(q, { 0, 0, 1 }));
    const Vec3 a = quatRotate(q, { 0.3f, 0.5f, -0.7f }), b = quatRotate(r, { 0.3f, 0.5f, -0.7f });
    CHECK_NEAR(a.x, b.x, 1e-5);
    CHECK_NEAR(a.y, b.y, 1e-5);
    CHECK_NEAR(a.z, b.z, 1e-5);
    // The beam's +Y runs from start to end, and its face (+Z) points at the eye's side.
    const Vec3 from = { 0.2f, 1.0f, -0.3f }, to = { 0.0f, 1.2f, -2.0f }, eye = { 0.0f, 1.6f, 0.0f };
    const Pose beam = beamPose(from, to, eye);
    const Vec3 y = quatRotate(beam.q, { 0, 1, 0 }), z = quatRotate(beam.q, { 0, 0, 1 });
    const Vec3 dir = normalize({ to.x - from.x, to.y - from.y, to.z - from.z });
    CHECK_NEAR(dot(y, dir), 1.0, 1e-5);
    CHECK(dot(z, { eye.x - beam.p.x, eye.y - beam.p.y, eye.z - beam.p.z }) > 0.0f);
    CHECK_NEAR(beam.p.z, -1.15, 1e-5);
    // No roll: a rolled, pitched, yawed head gives a quad with the same yaw and pitch, level x axis.
    const Quat head = quatMul(quatMul(quatFromYaw(0.6f), Quat { std::sin(-0.15f), 0, 0, std::cos(-0.15f) }), Quat { 0, 0, std::sin(0.3f), std::cos(0.3f) });
    const Pose hud = noRollPose(head, { 0, 1.6f, 0 }, 1.5f);
    const Vec3 hx = quatRotate(hud.q, { 1, 0, 0 });
    CHECK_NEAR(hx.y, 0.0, 1e-5);
    const Vec3 hf = quatRotate(hud.q, { 0, 0, -1 }), headF = quatRotate(head, { 0, 0, -1 });
    CHECK_NEAR(dot(hf, headF), 1.0, 1e-5);
    CHECK_NEAR(hud.p.y - 1.6f, 1.5f * headF.y, 1e-5);
}

static void testOrbitView() {
    // A camera 300 units behind its target, facing heading 40 degrees, slightly pitched.
    const float h0 = 40.0f * 3.1415926f / 180.0f;
    const Vec3 target = { 1000.0f, 200.0f, -500.0f };
    const Vec3 eye = { target.x + 300.0f * std::sin(h0), target.y + 80.0f, target.z + 300.0f * std::cos(h0) };
    const Mat4 view = guLookAt(eye, target, { 0, 1, 0 });
    CHECK_NEAR(viewHeading(view), h0, 1e-4);
    const float dist = std::sqrt((eye.x - target.x) * (eye.x - target.x) + (eye.z - target.z) * (eye.z - target.z));
    // Orbit to heading 10 degrees (a 30 degree right turn).
    const float h1 = 10.0f * 3.1415926f / 180.0f;
    float delta = 0.0f;
    const Mat4 v = orbitView(view, h1, dist, &delta);
    CHECK_NEAR(delta, h1 - h0, 1e-5);
    CHECK_NEAR(viewHeading(v), h1, 1e-4);
    // The look-at target stays where it was in view space (straight ahead, same depth).
    const Vec3 a = transformPoint(target, view), b = transformPoint(target, v);
    CHECK_NEAR(a.x, b.x, 1e-2);
    CHECK_NEAR(a.y, b.y, 1e-2);
    CHECK_NEAR(a.z, b.z, 1e-2);
    // And the result is what the game camera would show at the new heading.
    const Vec3 eye1 = { target.x + 300.0f * std::sin(h1), eye.y, target.z + 300.0f * std::cos(h1) };
    CHECK(matNear(v, guLookAt(eye1, target, { 0, 1, 0 }), 2e-3f));
    // At the target heading nothing changes.
    const Mat4 same = orbitView(view, h0, dist, &delta);
    CHECK_NEAR(delta, 0.0, 1e-5);
    CHECK(matNear(same, view, 1e-3f));
    CHECK_NEAR(wrapAngle(3.5f * 3.1415926f), -0.5f * 3.1415926f, 1e-4);
}

static void testHudSolve() {
    // The N64 blender's usual 2D mode: out = a * src + (1 - a) * dst, in the targets' encoding.
    // Rendered over black and over white and solved, every (src, a) comes back, in 8-bit steps.
    auto q = [](float v) { return std::round(v * 255.0f) / 255.0f; };
    int worst = 0;
    for (int ai = 0; ai <= 255; ai += 5) {
        const float a = ai / 255.0f;
        const float srcs[][3] = { { 1, 1, 1 }, { 0, 0, 0 }, { 0.9f, 0.6f, 0.1f }, { 0.2f, 0.3f, 0.95f } };
        for (const auto &src : srcs) {
            float b[3], w[3], c[3], alpha;
            for (int i = 0; i < 3; i++) {
                b[i] = q(a * src[i]);
                w[i] = q(a * src[i] + (1.0f - a));
            }
            hudSolve(b, w, c, alpha);
            CHECK_NEAR(alpha, a, 1.5 / 255.0);
            // Composited over any background the solved pixel matches the game's own blend.
            const float bg[3] = { 0.3f, 0.7f, 0.5f };
            for (int i = 0; i < 3; i++) {
                const float game = a * src[i] + (1.0f - a) * bg[i];
                const float ours = alpha * c[i] + (1.0f - alpha) * bg[i];
                const int err = int(std::lround(std::fabs(game - ours) * 255.0f));
                worst = std::max(worst, err);
            }
        }
    }
    CHECK(worst <= 2);
    // Fully transparent: alpha 0, colour 0. Opaque black text outline: alpha 1, colour 0.
    float c[3], alpha;
    const float zero[3] = { 0, 0, 0 }, one[3] = { 1, 1, 1 };
    hudSolve(zero, one, c, alpha);
    CHECK(alpha == 0.0f && c[0] == 0.0f);
    hudSolve(zero, zero, c, alpha);
    CHECK(alpha == 1.0f && c[0] == 0.0f);
}

static void testCinema() {
    // Screen 3 m ahead, 3.6 m wide, 2.7 m tall; an eye on its axis sees it symmetrically.
    Pose screen;
    screen.p = { 0.0f, 1.6f, -3.0f };
    FovTangents t = screenFov(screen, 3.6f, 2.7f, { 0.0f, 1.6f, 0.0f });
    CHECK_NEAR(t.left, -0.6, 1e-5);
    CHECK_NEAR(t.right, 0.6, 1e-5);
    CHECK_NEAR(t.up, 0.45, 1e-5);
    CHECK_NEAR(t.down, -0.45, 1e-5);
    // A left eye 32 mm left sees the screen shifted right; a turned screen still works.
    t = screenFov(screen, 3.6f, 2.7f, { -0.032f, 1.6f, 0.0f });
    CHECK_NEAR(t.left, (-1.8 + 0.032) / 3.0, 1e-5);
    Pose turned;
    turned.q = quatFromYaw(0.7f);
    turned.p = quatRotate(turned.q, { 0.0f, 0.0f, -3.0f });
    const FovTangents t2 = screenFov(turned, 3.6f, 2.7f, quatRotate(turned.q, { -0.032f, 0.0f, 0.0f }));
    CHECK_NEAR(t2.left, t.left, 1e-5);
    CHECK_NEAR(t2.up, 0.45, 1e-5);

    // The game camera (fov 45, 4:3, near 10) split for the screen.
    const Mat4 proj = guPerspective(45.0f, 4.0f / 3.0f, 10.0f, 6000.0f, 1.0f);
    const FovTangents g = gameFov(proj);
    CHECK_NEAR(g.up, std::tan(22.5 * 3.14159265 / 180.0), 1e-5);
    CHECK_NEAR(g.right, g.up * 4.0 / 3.0, 1e-5);
    const float near = 10.0f;
    const float screenW = 3.6f, ipd = 0.064f;
    const float sep = cinemaSeparation(0.6f * ipd, screenW, g, near);
    const Mat4 view = guLookAt({ 100, 50, 300 }, { 120, 40, 0 }, { 0, 1, 0 });
    const CinemaEye left = cinemaEye(view, g, sep, near, 0), right = cinemaEye(view, g, sep, near, 1);
    // A point's on-screen position (metres from the left edge) for one eye.
    auto onScreen = [&](const CinemaEye &e, const Vec3 &world) {
        const Vec3 c = transformPoint(world, e.view);
        const float tanX = c.x / -c.z;
        return (tanX - e.tan.left) / (e.tan.right - e.tan.left) * screenW;
    };
    // At the convergence distance (the near plane) both eyes agree: zero parallax.
    const Mat4 cam = rigidInverse(view);
    auto ahead = [&](float d) {
        return Vec3 { cam.m[3][0] - cam.m[2][0] * d, cam.m[3][1] - cam.m[2][1] * d, cam.m[3][2] - cam.m[2][2] * d };
    };
    CHECK_NEAR(onScreen(right, ahead(near)) - onScreen(left, ahead(near)), 0.0, 1e-5);
    // Further away: positive (uncrossed) parallax, growing towards 0.6 x the eye distance.
    const float p100 = onScreen(right, ahead(100.0f)) - onScreen(left, ahead(100.0f));
    const float pFar = onScreen(right, ahead(1e7f)) - onScreen(left, ahead(1e7f));
    CHECK(p100 > 0.0f);
    CHECK(pFar > p100);
    CHECK_NEAR(pFar, 0.6 * ipd, 1e-4);
    CHECK(pFar < ipd);  // never divergent
    // Zero depth setting: identical eyes.
    const CinemaEye flat = cinemaEye(view, g, 0.0f, near, 1);
    CHECK(matNear(flat.view, view, 1e-6f));
}

// ---------------------------------------------------------------------------------------------
// Eye size and camera smoothing

static void testEyeSize() {
    // auto: the recommendation scaled up to 2160, or used as it is when it already reaches it.
    EyeSize e = eyeSize(1728, 1728, 4096, 4096, 0.0);
    CHECK(e.width == 2160 && e.height == 2160);
    CHECK_NEAR(e.scale, 1.25, 1e-9);
    e = eyeSize(2160, 2160, 4096, 4096, 0.0);
    CHECK(e.width == 2160 && e.height == 2160);
    CHECK_NEAR(e.scale, 1.0, 0.0);
    e = eyeSize(2592, 2592, 4096, 4096, 0.0);
    CHECK(e.width == 2592 && e.height == 2592);
    e = eyeSize(1728, 1728, 2000, 2000, 0.0);
    CHECK(e.width == 2000 && e.height == 2000);
    e = eyeSize(1600, 1700, 4096, 4096, 0.0);  // the longer side reaches 2160, both even
    CHECK(e.height == 2160 && e.width == 2032);
    // A number multiplies the recommendation, as before (the 2700 that halved the frame rate).
    e = eyeSize(2160, 2160, 4096, 4096, 1.25);
    CHECK(e.width == 2700 && e.height == 2700);
    e = eyeSize(1728, 1728, 4096, 4096, 0.75);
    CHECK(e.width == 1296 && e.height == 1296);
}

static Pose camAt(float x, float y, float z, float yawDeg, float pitchDeg = 0.0f) {
    const float yaw = yawDeg * 3.14159265f / 180.0f, pitch = pitchDeg * 3.14159265f / 180.0f;
    Pose p;
    p.q = quatMul(quatFromYaw(yaw), Quat { std::sin(0.5f * pitch), 0.0f, 0.0f, std::cos(0.5f * pitch) });
    p.p = { x, y, z };
    return p;
}

static bool poseNear(const Pose &a, const Pose &b, float posEps, float angEps) {
    const float dp = std::sqrt((a.p.x - b.p.x) * (a.p.x - b.p.x) + (a.p.y - b.p.y) * (a.p.y - b.p.y) + (a.p.z - b.p.z) * (a.p.z - b.p.z));
    const float da = quatAngle(a.q, b.q);
    if (!(dp <= posEps) || !(da <= angEps)) {
        std::printf("  pose differs by %.6g units, %.6g rad\n", dp, da);
        return false;
    }

    return true;
}

static void testCameraPoseFromView() {
    const Mat4 view = guLookAt({ 100.0f, 50.0f, -30.0f }, { 160.0f, 40.0f, 20.0f }, { 0.0f, 1.0f, 0.0f });
    const Pose cam = cameraPoseFromView(view);
    CHECK(matNear(viewFromCameraPose(cam), view, 2e-4f));
    CHECK_NEAR(cameraYaw(cam), viewHeading(view), 1e-5);
    const Vec3 at = normalize({ 60.0f, -10.0f, 50.0f });
    CHECK_NEAR(cameraPitch(cam), std::asin(at.y), 1e-5);
    // Exp and log are inverse; slerp takes the short way round.
    const Vec3 v = { 0.3f, -0.8f, 0.2f };
    const Vec3 back = quatLog(quatExp(v));
    CHECK_NEAR(back.x, v.x, 1e-5);
    CHECK_NEAR(back.y, v.y, 1e-5);
    CHECK_NEAR(back.z, v.z, 1e-5);
    const Quat a = quatFromYaw(3.0f), b = quatFromYaw(-3.0f);  // 0.28 rad apart across +-pi
    CHECK_NEAR(quatAngle(quatSlerp(a, b, 0.5f), quatFromYaw(3.14159265f)), 0.0, 1e-4);
}

static Mat4 V(const Pose &camera) {
    return viewFromCameraPose(camera);
}

static Pose C(const Mat4 &view) {
    return cameraPoseFromView(view);
}

static void testCameraSmoothingLatency() {
    // Constant velocity: the B-spline is the linear path half a game frame later, exactly.
    // Moving straight ahead without turning, and turning on the spot.
    CameraSmoother moving, turning;
    for (int i = 0; i < 3; i++) {
        moving.addTick(V(camAt(10.0f * float(i), 2.0f, -5.0f, 30.0f)));
        turning.addTick(V(camAt(0.0f, 0.0f, 0.0f, 3.0f * float(i), 1.0f * float(i))));
    }

    for (float w : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f }) {
        CHECK_NEAR(C(moving.spline(w)).p.x, 10.0f + 10.0f * (w - 0.5f), 1e-3);
        CHECK_NEAR(cameraYaw(C(turning.spline(w))) * 180.0f / 3.14159265f, 3.0f + 3.0f * (w - 0.5f), 1e-3);
        CHECK_NEAR(cameraPitch(C(turning.spline(w))) * 180.0f / 3.14159265f, 1.0f + 1.0f * (w - 0.5f), 1e-3);
        CHECK(matNear(moving.sample(w, 1.0f / 90.0f, CameraSmoothing::Low), moving.spline(w), 1e-4f));
        CHECK(matNear(moving.sample(w, 1.0f / 90.0f, CameraSmoothing::Off), moving.linear(w), 1e-4f));
    }

    // Off is RT64's own path: the view's rotation slerped and its translation lerped, so the host's
    // correction inverse(linear) * sample is the identity.
    const Mat4 a = V(camAt(0, 0, 0, 0)), b = V(camAt(30, 0, 10, 40));
    CameraSmoother s;
    s.addTick(a);
    s.addTick(b);
    const Mat4 mid = s.linear(0.5f);
    for (int r = 0; r < 3; r++) {
        CHECK_NEAR(mid.m[3][r], 0.5f * (a.m[3][r] + b.m[3][r]), 1e-3);
    }

    CHECK(matNear(mul(rigidInverse(s.linear(0.3f)), s.sample(0.3f, 0.01f, CameraSmoothing::Off)), Mat4(), 1e-5f));
}

static void testCameraSmoothingContinuity() {
    // Velocity steps at every game frame (DK64's 30 Hz camera): the B-spline is continuous in
    // position and velocity across game frames, where the linear path's velocity jumps.
    CameraSmoother s;
    const float xs[] = { 0, 2, 10, 11, 25, 25, 26, 40, 41, 41 };
    const float yaws[] = { 0, 1, 6, 6, 14, 20, 21, 21, 30, 31 };
    s.addTick(V(camAt(xs[0], 0, 0, yaws[0])));
    float worstJump = 0.0f, worstJumpVel = 0.0f, worstLinearVelJump = 0.0f;
    const float h = 1e-2f;
    auto px = [](const Mat4 &v) { return C(v).p.x; };
    for (int i = 1; i < 10; i++) {
        const Pose endPrev = C(s.spline(1.0f));
        const float velPrev = (px(s.spline(1.0f)) - px(s.spline(1.0f - h))) / h;
        const float linVelPrev = (px(s.linear(1.0f)) - px(s.linear(1.0f - h))) / h;
        s.addTick(V(camAt(xs[i], 0, 0, yaws[i])));
        if (i < 2) {
            continue;  // the history is still filling
        }

        const Pose startNext = C(s.spline(0.0f));
        const float velNext = (px(s.spline(h)) - px(s.spline(0.0f))) / h;
        const float linVelNext = (px(s.linear(h)) - px(s.linear(0.0f))) / h;
        worstJump = std::max(worstJump, std::fabs(endPrev.p.x - startNext.p.x) + quatAngle(endPrev.q, startNext.q));
        worstJumpVel = std::max(worstJumpVel, std::fabs(velPrev - velNext));
        worstLinearVelJump = std::max(worstLinearVelJump, std::fabs(linVelPrev - linVelNext));
    }

    CHECK(worstJump < 1e-3f);
    CHECK(worstJumpVel < 0.6f);          // finite differences over h; the steps are 1-14 units
    CHECK(worstLinearVelJump > 5.0f);    // the linear path does jump
}

static void testCameraSmoothingFollower() {
    // A steady turn at 90 deg/s, 30 Hz game frames, 90 Hz display: the follower settles to a
    // lag of about rate x smoothing time behind the B-spline and never overshoots it.
    for (CameraSmoothing level : { CameraSmoothing::Medium, CameraSmoothing::High }) {
        CameraSmoother s;
        const float rate = 90.0f;  // deg/s
        const double tick = 1.0 / 30.0, period = 1.0 / 90.0;
        int ticks = 0;
        float lag = 0.0f, worstOvershoot = 0.0f;
        for (int f = 0; f < 180; f++) {
            const double t = f * period;
            while (ticks * tick <= t + 1e-9) {
                s.addTick(V(camAt(0, 0, 0, rate * float(ticks * tick))));
                ticks++;
            }

            const float w = float((t - (ticks - 1) * tick) / tick);
            const Pose out = C(s.sample(w, float(period), level));
            const Pose sp = C(s.spline(w));
            lag = wrapAngle(cameraYaw(sp) - cameraYaw(out)) * 180.0f / 3.14159265f;
            worstOvershoot = std::min(worstOvershoot, lag);
        }

        CHECK_NEAR(lag, rate * cameraFollowTime(level), 0.12 * rate * cameraFollowTime(level));
        CHECK(worstOvershoot > -0.05f);
    }

    // Starting and stopping a swing: the follower's angular velocity changes gradually (no step
    // larger than a fraction of the B-spline's), and the camera's position is the B-spline's.
    CameraSmoother s;
    float prevYaw = 0.0f, prevStep = 0.0f, worstChange = 0.0f, worstSplineChange = 0.0f, prevSplineYaw = 0.0f, prevSplineStep = 0.0f;
    float worstPos = 0.0f;
    int ticks = 0;
    for (int f = 0; f < 240; f++) {
        const double t = f / 90.0;
        while (ticks / 30.0 <= t + 1e-9) {
            const double tt = ticks / 30.0;
            const float yaw = (tt < 0.5) ? 0.0f : ((tt < 0.8) ? float((tt - 0.5) * 150.0) : 45.0f);  // a 45 deg swing in 0.3 s
            s.addTick(V(camAt(100.0f + 3.0f * float(ticks), 20.0f, 50.0f, yaw)));
            ticks++;
        }

        const float w = float((t - (ticks - 1) / 30.0) * 30.0);
        const Pose out = C(s.sample(w, 1.0f / 90.0f, CameraSmoothing::Medium));
        const Pose sp = C(s.spline(w));
        const float yaw = cameraYaw(out);
        const float splineYaw = cameraYaw(sp);
        worstPos = std::max(worstPos, std::fabs(out.p.x - sp.p.x) + std::fabs(out.p.y - sp.p.y) + std::fabs(out.p.z - sp.p.z));
        if (f > 1) {
            worstChange = std::max(worstChange, std::fabs(wrapAngle(yaw - prevYaw) - prevStep));
            worstSplineChange = std::max(worstSplineChange, std::fabs(wrapAngle(splineYaw - prevSplineYaw) - prevSplineStep));
        }

        prevStep = wrapAngle(yaw - prevYaw);
        prevSplineStep = wrapAngle(splineYaw - prevSplineYaw);
        prevYaw = yaw;
        prevSplineYaw = splineYaw;
    }

    CHECK(worstChange < 0.6f * worstSplineChange);
    CHECK(worstPos < 0.05f);
    CHECK_NEAR(prevYaw * 180.0f / 3.14159265f, 45.0, 0.05);  // arrives
}

static void testCameraSmoothingCuts() {
    for (CameraSmoothing level : { CameraSmoothing::Low, CameraSmoothing::Medium, CameraSmoothing::High }) {
        CameraSmoother s;
        for (int i = 0; i < 10; i++) {
            s.addTick(V(camAt(5.0f * float(i), 0, 0, 2.0f * float(i))));
            s.sample(0.5f, 1.0f / 90.0f, level);
        }

        // A teleport: the new camera at once, at every weight, with no swoop afterwards.
        const Pose far = camAt(1000.0f, 50.0f, -400.0f, 120.0f);
        CHECK(s.addTick(V(far)));
        CHECK(s.cuts() == 1);
        CHECK(poseNear(C(s.sample(0.0f, 1.0f / 90.0f, level)), far, 2e-3f, 1e-4f));
        CHECK(poseNear(C(s.sample(1.0f, 1.0f / 90.0f, level)), far, 2e-3f, 1e-4f));
        // A camera cut that only turns (a cutscene angle): also a cut.
        const Pose turned = camAt(1000.0f, 50.0f, -400.0f, 200.0f);
        CHECK(s.addTick(V(turned)));
        CHECK(poseNear(C(s.sample(0.3f, 1.0f / 90.0f, level)), turned, 2e-3f, 1e-4f));
        // The host's own reasons (a scene change).
        const Pose same = camAt(1001.0f, 50.0f, -400.0f, 200.0f);
        CHECK(s.addTick(V(same), true));
        CHECK(poseNear(C(s.sample(0.7f, 1.0f / 90.0f, level)), same, 2e-3f, 1e-4f));
        CHECK(s.cuts() == 3);
        // A normal step is not a cut.
        CHECK(!s.addTick(V(camAt(1010.0f, 50.0f, -400.0f, 210.0f))));
    }
}

static void testCameraSmoothingWrap() {
    // Headings across +-180 degrees: every displayed step is small and in the same direction.
    for (CameraSmoothing level : { CameraSmoothing::Off, CameraSmoothing::Low, CameraSmoothing::High }) {
        CameraSmoother s;
        float prev = 0.0f, worst = 0.0f, least = 1e9f;
        int ticks = 0;
        for (int f = 0; f < 90; f++) {
            const double t = f / 90.0;
            while (ticks / 30.0 <= t + 1e-9) {
                s.addTick(V(camAt(0, 0, 0, 160.0f + 2.0f * float(ticks))));  // 160 .. 220 = -140
                ticks++;
            }

            const float w = float((t - (ticks - 1) / 30.0) * 30.0);
            const float yaw = cameraYaw(C(s.sample(w, 1.0f / 90.0f, level)));
            if (f > 6) {
                const float step = wrapAngle(yaw - prev) * 180.0f / 3.14159265f;
                worst = std::max(worst, std::fabs(step));
                least = std::min(least, step);
            }

            prev = yaw;
        }

        CHECK(worst < 1.0f);
        CHECK(least > -0.01f);
        CHECK(s.cuts() == 0);
    }

    // The step statistics see the same: no 360 degree step.
    CameraStepStats stats;
    for (int i = 0; i < 20; i++) {
        stats.add(camAt(float(i), 0, 0, 170.0f + float(i)));
    }

    CHECK(stats.frames == 19);
    CHECK_NEAR(stats.maxAbs[0], 1.0, 1e-3);
    CHECK_NEAR(stats.stepSd(0), 0.0, 1e-3);
    CHECK_NEAR(stats.dMaxAbs[0], 0.0, 1e-3);
    CHECK_NEAR(stats.maxAbs[2], 1.0, 1e-4);
    stats.restart();
    stats.add(camAt(500.0f, 0, 0, 0.0f));  // no step across a cut
    CHECK(stats.frames == 19);
}

static void testSmoothTurn() {
    // The stick curve: nothing inside the 20% deadzone, full speed at full deflection, the power
    // 1.5 in between, symmetric, monotonic.
    CHECK_NEAR(smoothTurnRate(0.0f, 120.0f), 0.0, 0.0);
    CHECK_NEAR(smoothTurnRate(0.19f, 120.0f), 0.0, 0.0);
    CHECK_NEAR(smoothTurnRate(1.0f, 120.0f), 120.0, 1e-3);
    CHECK_NEAR(smoothTurnRate(-1.0f, 120.0f), -120.0, 1e-3);
    CHECK_NEAR(smoothTurnRate(0.6f, 120.0f), 120.0 * std::pow(0.5, 1.5), 1e-3);
    float prev = 0.0f;
    bool monotonic = true;
    for (int i = 0; i <= 100; i++) {
        const float v = smoothTurnRate(float(i) / 100.0f, 90.0f);
        monotonic = monotonic && (v >= prev);
        prev = v;
    }

    CHECK(monotonic);
    CHECK_NEAR(smoothTurnRate(1.0f, 500.0f), 360.0, 1e-3);  // the standard's range, 30-360

    // The mapper: held stick X in smooth mode turns by rate x dt each frame, in stereo play only.
    InputMapper mapper;
    InputConfig config;
    config.turnMode = TurnMode::Smooth;
    config.smoothSpeedDps = 120.0f;
    InputContext context;
    context.stereoPlay = true;
    InputSample sample;
    sample.valid = true;
    sample.rStick[0] = 1.0f;
    float total = 0.0f;
    for (int f = 0; f < 90; f++) {
        total += mapper.update(sample, config, context, f / 90.0, 1.0f / 90.0f).turnDeg;
    }

    CHECK_NEAR(total, 120.0, 0.01);
    context.stereoPlay = false;
    CHECK_NEAR(mapper.update(sample, config, context, 1.0, 1.0f / 90.0f).turnDeg, 0.0, 0.0);
    // Snap: one 45 degree step per push by default.
    InputMapper snapper;
    InputConfig snapConfig;
    context.stereoPlay = true;
    float snapped = 0.0f;
    for (int f = 0; f < 30; f++) {
        snapped += snapper.update(sample, snapConfig, context, f / 90.0, 1.0f / 90.0f).turnDeg;
    }

    CHECK_NEAR(snapped, 45.0, 0.0);

    // The orbit by an explicit angle: the same as orbitView for a small angle, any number of
    // turns, the pivot stays put and the distance to it is kept.
    const Mat4 view = guLookAt({ 100.0f, 30.0f, 200.0f }, { 100.0f, 20.0f, 100.0f }, { 0.0f, 1.0f, 0.0f });
    float delta = 0.0f;
    const Mat4 a = orbitView(view, viewHeading(view) + 0.5f, 100.0f, &delta);
    CHECK_NEAR(delta, 0.5, 1e-5);
    CHECK(matNear(orbitViewBy(view, 0.5f, 100.0f), a, 1e-4f));
    CHECK(matNear(orbitViewBy(view, 0.5f + 2.0f * 3.14159265f, 100.0f), a, 2e-3f));
    const Mat4 far = orbitViewBy(view, 4.0f, 100.0f);  // past pi: no wrap in the other direction
    CHECK_NEAR(wrapAngle(viewHeading(far) - viewHeading(view)), wrapAngle(4.0f), 1e-4);
    const Vec3 pivot = { 100.0f, 30.0f, 100.0f };
    const Vec3 p = viewPosition(far);
    CHECK_NEAR(std::sqrt((p.x - pivot.x) * (p.x - pivot.x) + (p.z - pivot.z) * (p.z - pivot.z)), 100.0, 1e-2);
    CHECK_NEAR(p.y, 30.0, 1e-3);
}

// ---------------------------------------------------------------------------------------------
// Adjustable third-person origin and world-size consistency (production bridge functions).

static void testCameraOrigin() {
    const Vec3 at = { 10, 20, -30 }, eye = { 90, 70, 150 };
    const Mat4 view = guLookAt(eye, at, { 0, 1, 0 });
    const Mat4 same = thirdPersonView(view, at, 0, 0);
    CHECK(std::memcmp(&view, &same, sizeof(view)) == 0);
    const Vec3 low = thirdPersonOrigin(eye, at, 1, 0);
    CHECK_NEAR(low.x, eye.x, 0);
    CHECK_NEAR(low.y, at.y, 0);
    CHECK_NEAR(low.z, eye.z, 0);
    const Vec3 close = thirdPersonOrigin(eye, at, 0, 0.8f);
    CHECK_NEAR(close.x, eye.x + 0.8f * (at.x - eye.x), 1e-5);
    CHECK_NEAR(close.y, eye.y + 0.8f * (at.y - eye.y), 1e-5);
    CHECK_NEAR(close.z, eye.z + 0.8f * (at.z - eye.z), 1e-5);
    CHECK_NEAR(thirdPersonOrigin(eye, at, 1, 0.8f).y, at.y, 1e-5);
    const Vec3 far = thirdPersonOrigin(eye, at, 0, -0.5f);
    CHECK_NEAR(far.x, eye.x - 0.5f * (at.x - eye.x), 1e-5);
    CHECK_NEAR(far.y, eye.y - 0.5f * (at.y - eye.y), 1e-5);
    CHECK_NEAR(far.z, eye.z - 0.5f * (at.z - eye.z), 1e-5);
    const Vec3 high = thirdPersonOrigin(eye, at, -0.5f, 0);
    CHECK_NEAR(high.x, eye.x, 1e-5);
    CHECK_NEAR(high.z, eye.z, 1e-5);
    CHECK(high.y > eye.y);
    const Vec3 fartherHigher = thirdPersonOrigin(eye, at, -0.5f, -0.5f);
    CHECK(fartherHigher.x > eye.x && fartherHigher.z > eye.z && fartherHigher.y > eye.y);
    // Re-aim inherited pitch when lowered; with level_pitch on the wearer still supplies pitch.
    const Mat4 lowered = thirdPersonView(view, at, 0.5f, 0.3f);
    const Vec3 atCamera = transformPoint(at, lowered);
    CHECK_NEAR(atCamera.x, 0, 1e-4);
    CHECK_NEAR(atCamera.y, 0, 1e-4);
    CHECK(atCamera.z < 0);
    CHECK_NEAR(cameraYaw(cameraPoseFromView(lowered)), cameraYaw(cameraPoseFromView(view)), 1e-5);
    CHECK_NEAR(cameraPitch(cameraPoseFromView(levelView(lowered))), 0, 1e-6);
    Pose head = { quatExp({ 0.2f, 0, 0 }), {} };
    CHECK_NEAR(cameraPitch(cameraPoseFromView(eyeView(levelView(lowered), head, 15))), 0.2, 1e-5);

    // A rolled camera (including incidental roll from orientation interpolation) must not
    // gain a yaw step when lowering changes inherited pitch.
    Pose rolled = { quatMul(quatMul(quatFromYaw(0.7f), quatExp({ -0.4f, 0, 0 })), quatExp({ 0, 0, 0.3f })), eye };
    const Vec3 rolledAt = transformPoint({ 0, 0, -150 }, poseMatrix(rolled));
    const Mat4 rolledView = viewFromCameraPose(rolled);
    const Mat4 rolledLow = thirdPersonView(rolledView, rolledAt, 0.6f, 0.3f);
    CHECK_NEAR(viewHeading(rolledLow), viewHeading(rolledView), 1e-5);
    CHECK_NEAR(viewHeading(levelView(rolledLow)), viewHeading(levelView(rolledView)), 1e-5);
    const Vec3 rolledFocus = transformPoint(rolledAt, rolledLow);
    CHECK_NEAR(rolledFocus.x, 0, 1e-4);
    CHECK_NEAR(rolledFocus.y, 0, 1e-4);

    for (Vec3 shift : { Vec3{}, Vec3{ 10000, -3000, 9000 } }) {
        for (Vec3 from : { eye, Vec3{ -90, -70, -150 }, at }) {
            from = { from.x + shift.x, from.y + shift.y, from.z + shift.z };
            const Vec3 target = { at.x + shift.x, at.y + shift.y, at.z + shift.z };
            const Vec3 line = { target.x - from.x, target.y - from.y, target.z - from.z };
            for (float lower : { 0.0f, 0.25f, 0.5f, 1.0f }) {
                float prevDistance = INFINITY;
                for (float closer : { 0.0f, 0.2f, 0.4f, 0.8f, 1.0f }) {
                    const Vec3 origin = thirdPersonOrigin(from, target, lower, closer);
                    const Vec3 remain = { target.x - origin.x, target.y - origin.y, target.z - origin.z };
                    const float distance = dot(remain, remain);
                    CHECK(distance <= prevDistance + 0.1f);
                    prevDistance = distance;
                    CHECK(dot(remain, line) >= -0.01f); // never crosses the target plane
                    CHECK(origin.x >= std::min(from.x, target.x) && origin.x <= std::max(from.x, target.x));
                    CHECK(origin.y >= std::min(from.y, target.y) && origin.y <= std::max(from.y, target.y));
                    CHECK(origin.z >= std::min(from.z, target.z) && origin.z <= std::max(from.z, target.z));
                    const Vec3 actual = viewPosition(thirdPersonView(guLookAt(from, { from.x, from.y, from.z - 1 }, { 0, 1, 0 }), target, lower, closer));
                    CHECK_NEAR(actual.x, origin.x, 0.01);
                    CHECK_NEAR(actual.y, origin.y, 0.01);
                    CHECK_NEAR(actual.z, origin.z, 0.01);
                }
            }
            float prevHeight = INFINITY;
            for (int i = 0; i <= 10; i++) {
                const Vec3 origin = thirdPersonOrigin(from, target, float(i) / 10, 0.3f);
                const float height = std::fabs(origin.y - target.y);
                CHECK(height <= prevHeight + 0.001f);
                prevHeight = height;
            }
        }
    }
}

static void testCameraOriginComposition() {
    CameraSmoother smooth;
    auto tick = [&smooth](float x, bool cut = false) {
        const Vec3 eye = { x, 60, 150 }, at = { x + 10, 20, 0 };
        smooth.addTick(guLookAt(eye, at, { 0, 1, 0 }), cut, &at);
    };
    tick(0); tick(10); tick(25);
    for (CameraSmoothing level : { CameraSmoothing::Off, CameraSmoothing::Low, CameraSmoothing::Medium, CameraSmoothing::High }) {
        for (float w : { 0.0f, 0.25f, 0.5f, 1.0f }) {
            const Mat4 raw = smooth.linear(w), filtered = smooth.sample(w, 1.0f / 120, level);
            const Vec3 focus = smooth.focus(w, level);
            const Mat4 correction = mul(rigidInverse(raw), filtered);
            const Mat4 corrected = mul(raw, correction);
            const Mat4 adjusted = thirdPersonView(corrected, focus, 0.5f, 0.3f);
            const Mat4 offset = mul(rigidInverse(corrected), adjusted);
            CHECK(matNear(mul(corrected, offset), adjusted, 1e-4f));
            for (float angle : { 0.0f, 0.5f, -1.5f, 7.0f }) {
                const Mat4 turned = orbitViewAround(adjusted, angle, focus);
                const Vec3 before = transformPoint(focus, adjusted), after = transformPoint(focus, turned);
                CHECK_NEAR(after.x, before.x, 1e-4);
                CHECK_NEAR(after.y, before.y, 1e-4);
                CHECK_NEAR(after.z, before.z, 1e-4);
                CHECK_NEAR(wrapAngle(viewHeading(turned) - viewHeading(adjusted) - angle), 0, 1e-5);
                for (float units : { 4.0f, 8.0f, 15.0f, 100.0f }) {
                    Pose left = { quatFromYaw(0.2f), { -0.032f, 0.1f, 0 } }, right = left;
                    right.p.x = 0.032f;
                    const Mat4 lv = eyeView(levelView(turned), left, units), rv = eyeView(levelView(turned), right, units);
                    const Vec3 l = viewPosition(lv), r = viewPosition(rv);
                    const Vec3 d = { r.x - l.x, r.y - l.y, r.z - l.z };
                    CHECK_NEAR(std::sqrt(dot(d, d)) / units, 0.064, 2e-5);
                }
            }
            // Exactly the old sequence at defaults, including both eyes and head/recentre pose.
            Pose tracked = { quatMul(quatFromYaw(0.4f), quatExp({ 0.1f, 0, 0 })), { 0.03f, 0.2f, -0.1f } };
            for (bool levelPitch : { false, true }) {
                const Mat4 oldOrbit = orbitViewBy(corrected, 0.7f, 150);
                const Mat4 newOrbit = orbitViewBy(thirdPersonView(corrected, focus, 0, 0), 0.7f, 150);
                const Mat4 oldEye = eyeView(levelPitch ? levelView(oldOrbit) : oldOrbit, tracked, 15);
                const Mat4 newEye = eyeView(levelPitch ? levelView(newOrbit) : newOrbit, tracked, 15);
                CHECK(std::memcmp(&oldEye, &newEye, sizeof(oldEye)) == 0);
            }
        }
    }
    // The new origin inherits the spline's continuous position/velocity at a game tick boundary.
    auto position = [&smooth](float w) {
        return viewPosition(thirdPersonView(smooth.spline(w), smooth.focus(w, CameraSmoothing::Low), 0.5f, 0.3f));
    };
    const Vec3 end = position(1), before = position(0.99f);
    tick(28);
    const Vec3 start = position(0), after = position(0.01f);
    CHECK_NEAR(start.x, end.x, 1e-4);
    CHECK_NEAR((after.x - start.x) / 0.01f, (end.x - before.x) / 0.01f, 0.3);
    // Cut to another region: both camera and focus reset without a swoop.
    tick(5000, true);
    for (float w : { 0.0f, 0.5f, 1.0f }) {
        CHECK_NEAR(position(w).x, 5003, 0.01);
        CHECK_NEAR(smooth.focus(w, CameraSmoothing::High).x, 5010, 0);
    }
    smooth.reset();
    CHECK(!smooth.focusReady());
}

static void testWorldScaleConsumers() {
    const Mat4 gameProj = guPerspective(45, 4.0f / 3, 10, 6000, 1);
    const Mat4 projection = eyeProjection({ -1, 1, 1, -1 }, gameProj);
    const Mat4 view = guLookAt({ 0, 30, 100 }, { 0, 10, 0 }, { 0, 1, 0 });
    for (float units : { 4.0f, 8.0f, 15.0f, 100.0f }) {
        Pose neutral, moved;
        moved.p = { 0.1f, 0.2f, -0.3f };
        const Vec3 a = viewPosition(eyeView(view, neutral, units)), b = viewPosition(eyeView(view, moved, units));
        const Vec3 d = { b.x - a.x, b.y - a.y, b.z - a.z };
        CHECK_NEAR(std::sqrt(dot(d, d)) / units, std::sqrt(0.14), 2e-5);
        // The diagnostic eye_near_m=0 and cinema retain the original depth bits at every scale.
        CHECK_NEAR(projection.m[2][2], gameProj.m[2][2], 0);
        CHECK_NEAR(projection.m[3][2], gameProj.m[3][2], 0);
        const FogRemap fog = fogRemap(gameProj, projection);
        CHECK_NEAR(fog.a, 1, 1e-6); CHECK_NEAR(fog.b, 0, 1e-6);
        CHECK_NEAR(depthToZw(projection, 10), -1, 1e-6);
    }
}

static void testCameraOriginSettings() {
    dk64vr_settings_t s = {};
    dk64vr_settings_defaults(&s);
    CHECK_NEAR(dk64vr_settings_get(&s, "camera_lower"), 0, 0);
    CHECK_NEAR(dk64vr_settings_get(&s, "camera_closer"), 0, 0);
    CHECK_NEAR(dk64vr_settings_dk_height(&s), 1.68, 1e-9);
    for (const char *key : { "camera_lower", "camera_closer" }) {
        CHECK(dk64vr_settings_key(dk64vr_settings_index(key))->type == SFVR_SETTING_FLOAT);
    }
    CHECK_NEAR(dk64vr_settings_key((size_t)dk64vr_settings_index("camera_lower"))->min, -1.0, 0);
    CHECK_NEAR(dk64vr_settings_key((size_t)dk64vr_settings_index("camera_closer"))->min, -1.0, 0);
    CHECK(cameraOffsetCanRenderWithoutNativeCollision(0.0f, 0.0f));
    CHECK(cameraOffsetCanRenderWithoutNativeCollision(0.4f, 0.6f));
    CHECK(!cameraOffsetCanRenderWithoutNativeCollision(-0.4f, 0.6f));
    CHECK(!cameraOffsetCanRenderWithoutNativeCollision(0.4f, -0.6f));
    const char *text = "version = 1\n[vr]\nport_schema = 5\nworld_scale = 8\ncamera_lower = -0.5\ncamera_closer = -0.3\nsnap_angle = 60\nfuture_key = 123\n";
    dk64vr_settings_parse(&s, text, strlen(text));
    for (size_t i = 0; i < dk64vr_settings_count(); i++) s.values[i] = s.file_values[i];
    CHECK_NEAR(dk64vr_settings_get(&s, "camera_lower"), -0.5, 0);
    CHECK_NEAR(dk64vr_settings_get(&s, "camera_closer"), -0.3, 1e-6);
    CHECK_NEAR(dk64vr_settings_dk_height(&s), 3.15, 1e-9);
    CHECK_NEAR(dk64vr_settings_get(&s, "port_schema"), 5, 0); // additive keys: no default migration
    size_t length;
    char *formatted = dk64vr_settings_format(&s, &length);
    CHECK(formatted != nullptr);
    dk64vr_settings_t roundTrip = {};
    dk64vr_settings_defaults(&roundTrip);
    if (formatted) {
        CHECK(std::strstr(formatted, "future_key = 123") != nullptr);
        CHECK(dk64vr_settings_parse(&roundTrip, formatted, length) == 0);
        for (size_t i = 0; i < dk64vr_settings_count(); i++) CHECK_NEAR(s.file_values[i], roundTrip.file_values[i], 1e-6);
    }
    std::free(formatted);
    CHECK(dk64vr_settings_apply_env(&s, [](const char *key) -> const char * {
        if (std::strcmp(key, "DK64_VR_WORLD_SCALE") == 0) return "4";
        if (std::strcmp(key, "DK64_VR_CAMERA_LOWER") == 0) return "0.75";
        if (std::strcmp(key, "DK64_VR_CAMERA_CLOSER") == 0) return "0.6";
        return nullptr;
    }) == 3);
    CHECK_NEAR(dk64vr_settings_get(&s, "world_scale"), 4.0, 0);
    CHECK_NEAR(dk64vr_settings_dk_height(&s), 6.3, 1e-9);
    CHECK_NEAR(dk64vr_settings_get(&s, "camera_lower"), 0.75, 1e-6);
    CHECK_NEAR(dk64vr_settings_get(&s, "camera_closer"), 0.6, 1e-6);
    CHECK_NEAR(s.file_values[dk64vr_settings_index("world_scale")], 8, 0);
    CHECK_NEAR(s.file_values[dk64vr_settings_index("camera_lower")], -0.5, 0);
    CHECK_NEAR(s.file_values[dk64vr_settings_index("camera_closer")], -0.3, 1e-6);
    for (const char *key : { "world_scale", "camera_lower", "camera_closer" }) {
        const sfvr_setting_key *range = dk64vr_settings_key(dk64vr_settings_index(key));
        dk64vr_settings_set(&s, key, -100);
        CHECK_NEAR(dk64vr_settings_get(&s, key), range->min, 1e-6);
        dk64vr_settings_set(&s, key, 1000);
        CHECK_NEAR(dk64vr_settings_get(&s, key), range->max, 1e-6);
    }
    CHECK(dk64vr_settings_apply_env(&s, [](const char *key) -> const char * {
        if (std::strcmp(key, "DK64_VR_WORLD_SCALE") == 0) return "-3";
        if (std::strcmp(key, "DK64_VR_CAMERA_LOWER") == 0) return "2";
        if (std::strcmp(key, "DK64_VR_CAMERA_CLOSER") == 0) return "9";
        return nullptr;
    }) == 3);
    CHECK_NEAR(dk64vr_settings_get(&s, "world_scale"), 3.75, 0);
    CHECK_NEAR(dk64vr_settings_get(&s, "camera_lower"), 1, 0);
    CHECK_NEAR(dk64vr_settings_get(&s, "camera_closer"), 0.8, 1e-6);
    const char *invalid = "[vr]\nport_schema = 5\nworld_scale = 0\ncamera_lower = nan\ncamera_closer = 5\n";
    dk64vr_settings_parse(&s, invalid, strlen(invalid));
    CHECK_NEAR(s.file_values[dk64vr_settings_index("world_scale")], 3.75, 0);
    CHECK_NEAR(s.file_values[dk64vr_settings_index("camera_lower")], 0, 0);
    CHECK_NEAR(s.file_values[dk64vr_settings_index("camera_closer")], 0.8, 1e-6);
    dk64vr_settings_free(&s);
    dk64vr_settings_free(&roundTrip);
}

// ---------------------------------------------------------------------------------------------
// The Menu + View chord (standard.md section 1): resolved before Start, the View short press or
// the recentre can act.

struct ChordRun {
    InputMapper m;
    InputConfig config;
    InputContext context;
    double t = 5.0;
    int fired = 0, starts = 0, toggles = 0, recentres = 0, applies = 0;
    double firstStartS = -1.0, firedS = -1.0;
    void frames(bool menu, bool view, double seconds) {
        const double dt = 1.0 / 72.0;
        for (double e = 0.0; e < seconds - 1e-9; e += dt) {
            InputSample s = sampleIdle();
            s.menu = menu;
            s.view = view;
            const InputResult r = m.update(s, config, context, t, float(dt));
            if (r.vrMenuChord) {
                fired++;
                firedS = t;
            }
            if (r.buttons & n64::Start) {
                starts++;
                if (firstStartS < 0.0) {
                    firstStartS = t;
                }
            }
            toggles += (r.navHeld & nav::ToggleMenu) ? 1 : 0;
            applies += (r.navHeld & nav::Apply) ? 1 : 0;
            recentres += r.recentre ? 1 : 0;
            t += dt;
        }
    }
};

static void testChord() {
    {
        // Menu tapped: Start is held back during the grace, then pulsed for at least 0.1 s.
        ChordRun c;
        c.context.stereoPlay = true;
        c.frames(false, false, 0.1);
        c.frames(true, false, 0.06);
        CHECK(c.starts == 0);
        c.frames(false, false, 0.5);
        CHECK(c.starts >= 7 && c.starts <= 10);  // 0.12 s at 72 Hz
        CHECK(c.fired == 0 && c.toggles == 0);
    }
    {
        // Menu held: Start from 0.15 s on, for as long as it is held.
        ChordRun c;
        c.frames(false, false, 0.1);
        const double down = c.t;
        c.frames(true, false, 1.0);
        CHECK(c.firstStartS >= down + 0.15 - 1e-9 && c.firstStartS < down + 0.17);
        CHECK(c.starts >= 60);
        CHECK(c.fired == 0);
    }
    {
        // Both together for 0.5 s: the chord, once, and nothing else, even held 2 s (past the
        // recentre) and on release.
        ChordRun c;
        c.frames(false, false, 0.1);
        const double down = c.t;
        c.frames(true, true, 2.0);
        c.frames(false, false, 0.5);
        CHECK(c.fired == 1);
        CHECK(c.firedS >= down + 0.5 - 1e-9 && c.firedS < down + 0.52);
        CHECK(c.starts == 0 && c.toggles == 0 && c.recentres == 0);
    }
    {
        // View first, Menu 0.3 s later: the chord 0.5 s after Menu; no recentre at 1 s, no View
        // short press on release, no Start.
        ChordRun c;
        c.frames(false, false, 0.1);
        c.frames(false, true, 0.3);
        const double menuDown = c.t;
        c.frames(true, true, 1.5);
        c.frames(false, false, 0.5);
        CHECK(c.fired == 1 && c.firedS >= menuDown + 0.5 - 1e-9);
        CHECK(c.starts == 0 && c.toggles == 0 && c.recentres == 0);
    }
    {
        // Menu first, View 0.1 s later (inside the grace): the chord, no Start.
        ChordRun c;
        c.frames(false, false, 0.1);
        c.frames(true, false, 0.1);
        c.frames(true, true, 0.7);
        c.frames(false, false, 0.3);
        CHECK(c.fired == 1 && c.starts == 0 && c.toggles == 0);
    }
    {
        // Let go early: nothing at all, also from the button still held.
        ChordRun c;
        c.frames(false, false, 0.1);
        c.frames(true, true, 0.3);
        c.frames(true, false, 1.0);
        c.frames(false, false, 0.3);
        c.frames(false, true, 1.2);  // a later View hold still recentres
        CHECK(c.fired == 0 && c.starts == 0 && c.toggles == 0 && c.recentres == 1);
    }
    {
        // View held past the recentre, then Menu: two separate actions, Start at once.
        ChordRun c;
        c.frames(false, false, 0.1);
        c.frames(false, true, 1.2);
        CHECK(c.recentres == 1);
        const double menuDown = c.t;
        c.frames(true, true, 0.3);
        CHECK(c.fired == 0 && c.firstStartS >= menuDown - 1e-9 && c.firstStartS < menuDown + 0.02);
    }
    {
        // Menu later than the grace: Start already passed, so no chord.
        ChordRun c;
        c.frames(false, false, 0.1);
        c.frames(true, false, 0.3);
        c.frames(true, true, 1.0);
        CHECK(c.fired == 0 && c.starts > 0);
    }
    {
        // In a recomp menu Menu is Apply: held back the same way, and the chord still works.
        ChordRun c;
        c.context.menuOpen = true;
        c.frames(false, false, 0.1);
        c.frames(true, true, 0.6);
        c.frames(false, false, 0.3);
        CHECK(c.fired == 1 && c.applies == 0 && c.toggles == 0);
        c.frames(true, false, 0.3);
        CHECK(c.applies > 0);
    }
    {
        // Buttons held through a reset (focus came back with them down) do nothing.
        ChordRun c;
        c.frames(true, true, 1.0);
        CHECK(c.fired == 0 && c.starts == 0);
    }
    {
        // ignoreHeld: the B that closed a menu never reaches the game as C-right.
        InputMapper m;
        InputConfig config;
        InputContext context;
        InputSample s = sampleIdle();
        s.b = true;
        m.ignoreHeld(s);
        CHECK((m.update(s, config, context, 1.0, 0.014f).buttons & n64::CRight) == 0);
        CHECK((m.update(s, config, context, 1.1, 0.014f).buttons & n64::CRight) == 0);
        s.b = false;
        m.update(s, config, context, 1.2, 0.014f);
        s.b = true;
        CHECK((m.update(s, config, context, 1.3, 0.014f).buttons & n64::CRight) != 0);
    }
}

// ---------------------------------------------------------------------------------------------
// The VR settings menu (src/vr/menu): tiles and rows against the settings registry.

namespace {

// The registry plus keys this build may not have (camera_lower, camera_closer).
class ExtraSettings final : public menu::Settings {
public:
    ExtraSettings(dk64vr_settings_t *s, bool extra) : base(s), extra(extra) {}
    bool has(const char *k) const override { return isExtra(k) ? extra : base.has(k); }
    double get(const char *k) const override { return isExtra(k) ? value(k) : base.get(k); }
    double def(const char *k) const override { return isExtra(k) ? 0.0 : base.def(k); }
    double lo(const char *k) const override { return isExtra(k) ? -1.0 : base.lo(k); }
    double hi(const char *k) const override { return isExtra(k) ? ((std::strcmp(k, "camera_closer") == 0) ? 0.8 : 1.0) : base.hi(k); }
    void set(const char *k, double v) override {
        if (isExtra(k)) {
            (std::strcmp(k, "camera_lower") == 0 ? lower : closer) = v;
        }
        else {
            base.set(k, v);
        }
    }
    menu::StructSettings base;
    bool extra;
    double lower = 0.0, closer = 0.0;

private:
    static bool isExtra(const char *k) { return (std::strcmp(k, "camera_lower") == 0) || (std::strcmp(k, "camera_closer") == 0); }
    double value(const char *k) const { return (std::strcmp(k, "camera_lower") == 0) ? lower : closer; }
};

bool contains(const std::vector<std::string> &v, const char *x) {
    return std::find(v.begin(), v.end(), x) != v.end();
}

menu::Effects press(menu::Model &m, uint32_t bit, double &t) {
    menu::Input in;
    in.nowS = (t += 0.05);
    in.navHeld = bit;
    menu::Effects e = m.step(in, menu::Pointer(), menu::Layout());
    in.nowS = (t += 0.05);
    in.navHeld = 0;
    const menu::Effects e2 = m.step(in, menu::Pointer(), menu::Layout());
    e.close = e.close || e2.close;
    e.recentre = e.recentre || e2.recentre;
    return e;
}

}

static void testWorldSize() {
    // DK64's 25.2-unit Donkey Kong: world_scale 15 makes him 1.68 m ("DK 1.7 m").
    CHECK_NEAR(menu::dkHeightFromScale(15.0), 1.68, 1e-12);
    CHECK(menu::dkHeightText(15.0) == "DK 1.7 m");
    CHECK(menu::dkHeightText(menu::scaleFromDkHeight(0.8)) == "DK 80 cm");
    CHECK(std::string(menu::dkHeightCompare(15.0)) == "About as tall as you");
    CHECK(std::string(menu::dkHeightCompare(menu::scaleFromDkHeight(5.0))) == "Giant");
    // Round trip for every preset; stepping walks them in order in both directions.
    const std::vector<double> p = menu::dkHeightPresets(4.0, 100.0);
    CHECK(p.size() == 16 && p.front() == 0.5 && p.back() == 6.0);
    std::vector<std::string> texts;
    for (size_t i = 0; i < p.size(); i++) {
        const double sc = menu::scaleFromDkHeight(p[i]);
        CHECK_NEAR(menu::dkHeightFromScale(sc), p[i], 1e-12);
        texts.push_back(menu::dkHeightText(sc));
        if (i + 1 < p.size()) {
            CHECK_NEAR(menu::stepWorldScale(sc, 1, 4.0, 100.0), menu::scaleFromDkHeight(p[i + 1]), 1e-9);
        }
        if (i > 0) {
            CHECK_NEAR(menu::stepWorldScale(sc, -1, 4.0, 100.0), menu::scaleFromDkHeight(p[i - 1]), 1e-9);
        }
    }
    std::sort(texts.begin(), texts.end());
    CHECK(std::unique(texts.begin(), texts.end()) == texts.end());  // every step reads differently
    // The registry's range limits the presets: this build's world_scale floor of 5 stops at 5 m.
    CHECK(menu::dkHeightPresets(5.0, 100.0).back() == 5.0);
    const double top = menu::scaleFromDkHeight(5.0);
    CHECK_NEAR(menu::stepWorldScale(top, 1, 5.0, 100.0), top, 0.0);
    // From a value between presets (world_scale 20: 1.26 m) to the neighbours.
    CHECK_NEAR(menu::stepWorldScale(20.0, 1, 5.0, 100.0), menu::scaleFromDkHeight(1.4), 1e-9);
    CHECK_NEAR(menu::stepWorldScale(20.0, -1, 5.0, 100.0), menu::scaleFromDkHeight(1.2), 1e-9);
}

static void testMenuModel() {
    dk64vr_settings_t s;
    std::memset(&s, 0, sizeof(s));
    dk64vr_settings_defaults(&s);
    ExtraSettings access(&s, false);
    menu::Facts facts;
    facts.displayHz = 72.0;
    facts.recommendedEye = 1728;
    menu::Model m(access, facts);
    double t = 10.0;
    m.open(t, 0);

    // C: the approved tiles plus World size, Recentre on top, All VR settings below.
    CHECK(m.screen() == menu::Screen::Quick);
    const std::vector<std::string> tiles = m.visibleRows();
    CHECK(tiles == std::vector<std::string>({ "turn_mode", "snap_angle", "world_size", "camera_smoothing", "hud_distance", "foveation" }));
    CHECK(m.stops().front() == "recentre" && m.stops().back() == "all");
    CHECK(m.focusedAct() == "row:turn_mode");
    const std::string quick = m.render(false);
    CHECK(quick.find("Quick VR settings") != std::string::npos);
    CHECK(quick.find("data-act=\"set:turn_mode:1\"") != std::string::npos);
    CHECK(quick.find("data-act=\"set:turn_mode:2\"") == std::string::npos);  // the tile offers Snap and Smooth
    CHECK(quick.find("DK 1.7 m") != std::string::npos);
    CHECK(quick.find("data-act=\"all\"") != std::string::npos);

    // Tile to key: Right on Turning is Smooth; snap angle then dims and leaves the focus order.
    press(m, nav::Right, t);
    CHECK_NEAR(dk64vr_settings_get(&s, "turn_mode"), 1.0, 0.0);
    CHECK(!contains(m.stops(), "row:snap_angle"));
    CHECK(m.render(false).find("Only with Snap turning.") != std::string::npos);
    press(m, nav::Left, t);
    CHECK_NEAR(dk64vr_settings_get(&s, "turn_mode"), 0.0, 0.0);
    press(m, nav::Down, t);
    CHECK(m.focusedAct() == "row:snap_angle");
    press(m, nav::Right, t);
    CHECK_NEAR(dk64vr_settings_get(&s, "snap_angle"), 45.0, 0.0);
    // World size: Right makes Donkey Kong taller (a smaller world_scale), Left back.
    press(m, nav::Down, t);
    CHECK(m.focusedAct() == "row:world_size");
    press(m, nav::Right, t);
    CHECK_NEAR(dk64vr_settings_get(&s, "world_scale"), menu::scaleFromDkHeight(1.8), 1e-4);
    CHECK(m.valueText("world_size") == "DK 1.8 m");
    press(m, nav::Left, t);
    CHECK_NEAR(dk64vr_settings_get(&s, "world_scale"), 15.0, 1e-4);
    // HUD distance steps by 0.1 m; Foveation by level.
    press(m, nav::Down, t);
    press(m, nav::Down, t);
    CHECK(m.focusedAct() == "row:hud_distance");
    press(m, nav::Right, t);
    CHECK_NEAR(dk64vr_settings_get(&s, "hud_distance"), 1.6, 1e-6);
    press(m, nav::Down, t);
    press(m, nav::Left, t);
    CHECK_NEAR(dk64vr_settings_get(&s, "foveation"), 2.0, 0.0);
    // Every change was recorded for the host, in order.
    CHECK(access.base.changes.size() == 7);
    CHECK(access.base.changes.front().key == "turn_mode" && access.base.changes.back().key == "foveation");

    // Recentre (top button) and close (B on C).
    menu::Effects e = m.act("recentre", t);
    CHECK(e.recentre && !e.close);
    e = press(m, nav::Back, t);
    CHECK(e.close);

    // B from "All VR settings...": Comfort, with world size; camera lower/closer hide until
    // their keys exist.
    m.open(t, 0);
    m.act("all", t);
    CHECK(m.screen() == menu::Screen::Full && m.tab() == menu::Tab::Comfort);
    std::vector<std::string> comfort = m.visibleRows();
    CHECK(contains(comfort, "world_size") && !contains(comfort, "camera_lower") && !contains(comfort, "camera_closer"));
    CHECK(m.focusedAct() == "row:defaults_profile");
    {
        ExtraSettings more(&s, true);
        menu::Model m2(more, facts);
        m2.open(t, 0);
        m2.act("all", t);
        comfort = m2.visibleRows();
        CHECK(contains(comfort, "camera_lower") && contains(comfort, "camera_closer"));
        menu::Pointer p;
        p.on = true;
        p.down = true;
        p.act = "slider:camera_closer";
        p.trackLeft = 100.0f;
        p.trackWidth = 400.0f;
        p.x = 300.0f;
        menu::Input in;
        in.nowS = t + 0.1;
        m2.step(in, p, menu::Layout());
        CHECK_NEAR(more.closer, -0.1, 1e-6);
    }

    // Bumpers cycle the five tabs and wrap; the standard's order.
    const menu::Tab order[] = { menu::Tab::Controls, menu::Tab::Hud, menu::Tab::Display, menu::Tab::Room, menu::Tab::Comfort };
    for (menu::Tab want : order) {
        press(m, nav::TabRight, t);
        CHECK(m.tab() == want);
    }
    press(m, nav::TabLeft, t);
    CHECK(m.tab() == menu::Tab::Room);

    // No diagnostics anywhere; the measured rate on Display.
    for (int tab = 0; tab < int(menu::Tab::Count); tab++) {
        m.showFull(menu::Tab(tab));
        const std::string page = m.render(false);
        CHECK(page.find("diag") == std::string::npos && page.find("dump") == std::string::npos && page.find("timing") == std::string::npos);
        if (menu::Tab(tab) == menu::Tab::Display) {
            CHECK(page.find("72.0 Hz") != std::string::npos);
            CHECK(page.find("per-app setting") != std::string::npos);
        }
    }

    // Defaults: the first press arms, a second within 3 s resets this tab only.
    m.showFull(menu::Tab::Hud);
    m.act("step:hud_distance:1", t);
    CHECK_NEAR(dk64vr_settings_get(&s, "hud_distance"), 1.7, 1e-6);
    m.act("def", t);
    CHECK_NEAR(dk64vr_settings_get(&s, "hud_distance"), 1.7, 1e-6);
    CHECK(m.render(false).find("Reset HUD &amp; menus?") != std::string::npos);
    m.act("def", t + 1.0);
    CHECK_NEAR(dk64vr_settings_get(&s, "hud_distance"), 1.5, 1e-9);
    CHECK_NEAR(dk64vr_settings_get(&s, "snap_angle"), 45.0, 0.0);  // Comfort untouched
    // An armed Defaults lapses after 3 s.
    m.act("step:hud_distance:1", t + 2.0);
    m.act("def", t + 2.0);
    menu::Input idle;
    idle.nowS = t + 5.1;
    m.step(idle, menu::Pointer(), menu::Layout());
    m.act("def", t + 5.2);
    CHECK_NEAR(dk64vr_settings_get(&s, "hud_distance"), 1.6, 1e-6);
    m.act("def", t + 5.3);
    CHECK_NEAR(dk64vr_settings_get(&s, "hud_distance"), 1.5, 1e-9);
    t += 6.0;
    // Comfort's Defaults covers the virtual rows' keys and world_scale.
    const std::vector<std::string> keys = m.tabKeys(menu::Tab::Comfort);
    CHECK(contains(keys, "move_from_view") && contains(keys, "move_relative") && contains(keys, "world_scale")
        && !contains(keys, "hud_distance") && !contains(keys, "defaults_profile"));
    m.showFull(menu::Tab::Comfort);
    m.act("def", t);
    m.act("def", t + 0.1);
    CHECK_NEAR(dk64vr_settings_get(&s, "snap_angle"), 30.0, 0.0);

    // Selecting Frame personal leaves saved values alone, but Comfort reset uses its defaults.
    m.act("set:turn_mode:0", t + 0.2);
    m.act("set:defaults_profile:1", t + 0.3);
    CHECK_NEAR(dk64vr_settings_get(&s, "turn_mode"), 0.0, 0.0);
    CHECK_NEAR(dk64vr_settings_get(&s, "defaults_profile"), 1.0, 0.0);
    dk64vr_settings_set(&s, "smooth_turn_speed", 60.0);
    m.act("def", t + 0.4);
    m.act("def", t + 0.5);
    CHECK_NEAR(dk64vr_settings_get(&s, "defaults_profile"), 1.0, 0.0);
    CHECK_NEAR(dk64vr_settings_get(&s, "turn_mode"), 1.0, 0.0);
    CHECK_NEAR(dk64vr_settings_get(&s, "smooth_turn_speed"), 120.0, 0.0);

    // Dependent rows: the game camera on the right stick disables Turning, with the reason.
    m.act("set:turn_mode:0", t + 0.6);
    m.act("set:right_stick_x:1", t);
    CHECK(m.render(false).find("Right stick is the game camera.") != std::string::npos);
    CHECK(!contains(m.stops(), "row:turn_mode"));
    m.act("set:turn_mode:1", t);
    CHECK_NEAR(dk64vr_settings_get(&s, "turn_mode"), 0.0, 0.0);
    m.act("set:right_stick_x:0", t);

    // "Left stick walks toward": Camera is move_from_view off; a hand is move_relative.
    m.act("set:move_ref:3", t);
    CHECK_NEAR(dk64vr_settings_get(&s, "move_from_view"), 0.0, 0.0);
    m.act("set:move_ref:1", t);
    CHECK_NEAR(dk64vr_settings_get(&s, "move_from_view"), 1.0, 0.0);
    CHECK_NEAR(dk64vr_settings_get(&s, "move_relative"), 1.0, 0.0);

    // Automatic resolution off keeps today's eye size (2160 / 1728 = 1.25), on is "auto".
    m.showFull(menu::Tab::Display);
    m.act("tog:res_auto", t);
    CHECK_NEAR(dk64vr_settings_get(&s, "resolution_scale"), 1.25, 1e-6);
    CHECK(m.render(false).find("2160 \xc3\x97 2160 per eye") != std::string::npos);
    m.act("tog:res_auto", t);
    CHECK_NEAR(dk64vr_settings_get(&s, "resolution_scale"), DK64VR_RESOLUTION_AUTO, 0.0);

    // The pointer: a click on a segment sets it; holding does not repeat; a slider drags.
    m.showFull(menu::Tab::Controls);
    menu::Pointer p;
    p.on = true;
    p.act = "slider:haptics";
    p.trackLeft = 200.0f;
    p.trackWidth = 600.0f;
    p.x = 500.0f;
    p.down = true;
    menu::Input in;
    in.nowS = (t += 0.1);
    m.step(in, p, menu::Layout());
    CHECK_NEAR(dk64vr_settings_get(&s, "haptics"), 0.5, 1e-6);
    p.x = 9999.0f;  // dragged past the end
    in.nowS = (t += 0.1);
    m.step(in, p, menu::Layout());
    CHECK_NEAR(dk64vr_settings_get(&s, "haptics"), 1.0, 1e-6);
    p.down = false;
    m.step(in, p, menu::Layout());
    p.act = "set:dominant_hand:1";
    p.down = true;
    in.nowS = (t += 0.1);
    m.step(in, p, menu::Layout());
    CHECK_NEAR(dk64vr_settings_get(&s, "dominant_hand"), 1.0, 0.0);
    CHECK(!m.padMode());
    in.nowS = (t += 0.1);
    p.act = "set:dominant_hand:0";
    m.step(in, p, menu::Layout());  // still held: no second click
    CHECK_NEAR(dk64vr_settings_get(&s, "dominant_hand"), 1.0, 0.0);

    // Scrolling: the pad focus is kept in view once the render showing it has been laid out.
    {
        dk64vr_settings_t ss;
        std::memset(&ss, 0, sizeof(ss));
        dk64vr_settings_defaults(&ss);
        menu::StructSettings sa(&ss);
        menu::Model sm(sa, facts);
        sm.open(0.0, 0);
        sm.act("all", 0.0);
        const auto layoutNow = [&]() {
            menu::Layout l;
            l.valid = true;
            l.viewHeight = 674.0f;
            l.contentHeight = 12.0f + 13.0f * 120.0f + 24.0f;
            l.focusValid = sm.focusIndex() >= 1;
            l.focusTop = 12.0f + float(sm.focusIndex() - 1) * 120.0f;
            l.focusBottom = l.focusTop + 118.0f;
            return l;
        };
        double tt = 1.0;
        for (int i = 0; i < 9; i++) {
            menu::Input in;
            in.nowS = (tt += 0.05);
            in.navHeld = nav::Down;
            sm.step(in, menu::Pointer(), layoutNow());
            sm.render(false);
            sm.dirty = false;  // the UI drew it
            in.navHeld = 0;
            for (int k = 0; k < 2; k++) {
                in.nowS = (tt += 0.05);
                sm.step(in, menu::Pointer(), layoutNow());
                sm.render(false);
                sm.dirty = false;
            }
            const menu::Layout l = layoutNow();
            if (!(l.focusTop >= sm.scrollPx() - 1.0f && l.focusBottom <= sm.scrollPx() + l.viewHeight + 1.0f)) std::printf("scroll i=%d focus=%d top=%.0f scroll=%.0f\n", i, sm.focusIndex(), l.focusTop, sm.scrollPx());
            CHECK(l.focusTop >= sm.scrollPx() - 1.0f && l.focusBottom <= sm.scrollPx() + l.viewHeight + 1.0f);
        }
        CHECK(sm.scrollPx() > 300.0f);
        CHECK(sm.scrollPx() <= 12.0f + 13.0f * 120.0f + 24.0f - 674.0f + 1e-3f);
    }

    // The host applies and saves the recorded changes: they survive format and parse (lesson 64).
    const char *file = "version = 1\n[vr]\nmy_own_key = 3\n";
    dk64vr_settings_t disk;
    std::memset(&disk, 0, sizeof(disk));
    dk64vr_settings_defaults(&disk);
    dk64vr_settings_parse(&disk, file, std::strlen(file));
    for (const menu::StructSettings::Change &c : access.base.changes) {
        dk64vr_settings_set(&disk, c.key.c_str(), c.value);
    }
    size_t len = 0;
    char *text = dk64vr_settings_format(&disk, &len);
    CHECK(text != nullptr && std::strstr(text, "my_own_key = 3") != nullptr);
    dk64vr_settings_t back;
    std::memset(&back, 0, sizeof(back));
    dk64vr_settings_defaults(&back);
    if (text != nullptr) {
        dk64vr_settings_parse(&back, text, len);
    }
    const char *checked[] = { "haptics", "dominant_hand", "move_relative", "move_from_view", "resolution_scale", "hud_distance", "world_scale", "snap_angle", "foveation" };
    for (const char *k : checked) {
        CHECK_NEAR(back.file_values[dk64vr_settings_index(k)], dk64vr_settings_get(&s, k), 1e-6);
    }
    std::free(text);
    dk64vr_settings_free(&disk);
    dk64vr_settings_free(&back);
}

// ---------------------------------------------------------------------------------------------
// Camera-facing billboards (lib/rt64/src/shared/rt64_vr_billboard.h)

// The game's matrix stack operations on rows (Banjo's mlMtxRotYaw/RotPitch/RotRoll/Scale_xyz:
// each one is applied to the vertex before what is already on the stack).
struct BbMat {
    double m[4][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } };
    void rows(int a, int b, double ca, double sa, double cb, double sb) {
        for (int i = 0; i < 3; i++) {
            const double x = m[a][i], y = m[b][i];
            m[a][i] = x * ca + y * sa;
            m[b][i] = x * cb + y * sb;
        }
    }
    void yaw(double deg) { const double r = deg * M_PI / 180.0; rows(0, 2, std::cos(r), -std::sin(r), std::sin(r), std::cos(r)); }
    void pitch(double deg) { const double r = deg * M_PI / 180.0; rows(1, 2, std::cos(r), std::sin(r), -std::sin(r), std::cos(r)); }
    void roll(double deg) { const double r = deg * M_PI / 180.0; rows(0, 1, std::cos(r), std::sin(r), -std::sin(r), std::cos(r)); }
    void scale(double x, double y, double z) { for (int i = 0; i < 3; i++) { m[0][i] *= x; m[1][i] *= y; m[2][i] *= z; } }
    void translate(double x, double y, double z) { m[3][0] = x; m[3][1] = y; m[3][2] = z; }
    void toFloat(float out[16]) const { for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) out[r * 4 + c] = float(m[r][c]); }
};

static BbMat bbCamera(double yawDeg, double pitchDeg) {
    BbMat c;
    c.yaw(yawDeg);
    c.pitch(pitchDeg);
    return c;
}

static Vec3 bbRow(const float m[16], int r) { return { m[r * 4 + 0], m[r * 4 + 1], m[r * 4 + 2] }; }
static float bbLen(const Vec3 &v) { return std::sqrt(dot(v, v)); }
static float bbDet(const float m[16]) { return dot(cross(bbRow(m, 0), bbRow(m, 1)), bbRow(m, 2)); }

static void testBillboardGameBasis() {
    // The camera basis Banjo builds sprites from (sViewportMatrix: yaw, then pitch): row 2 is the
    // camera's back axis (its look vector is (0,0,-1) through it) and row 0 stays level.
    float m[16];
    bbCamera(30.0, -20.0).toFloat(m);
    CHECK_NEAR(bbRow(m, 0).y, 0.0, 1e-7);
    CHECK_NEAR(bbDet(m), 1.0, 1e-6);
    bbCamera(0.0, -20.0).toFloat(m);
    CHECK_NEAR(bbRow(m, 2).x, 0.0, 1e-7);
    CHECK_NEAR(bbRow(m, 2).y, std::sin(20.0 * M_PI / 180.0), 1e-6);
    CHECK_NEAR(bbRow(m, 2).z, std::cos(20.0 * M_PI / 180.0), 1e-6);
}

static void testBillboardFacesViewpointAlongItsAxis() {
    // A viewpoint on the billboard's own axis: the spherical facing changes nothing.
    BbMat b = bbCamera(-65.0, -25.0);
    b.roll(37.0);
    b.translate(1200.0, 85.0, -3400.0);
    b.scale(-1.5, 1.5, 1.5);
    float m[16], orig[16];
    b.toFloat(m);
    std::memcpy(orig, m, sizeof(m));
    const Vec3 axis = normalize(bbRow(m, 2));
    const float vp[3] = { m[12] + axis.x * 700.0f, m[13] + axis.y * 700.0f, m[14] + axis.z * 700.0f };
    CHECK(RT64::vrFaceBillboard(m, vp, RT64::VrBillboardMode::Spherical));
    for (int i = 0; i < 16; i++) {
        CHECK_NEAR(m[i], orig[i], 2e-5);
    }
}

static void testBillboardCylindrical() {
    // A prop sprite (func_80344138: camera basis, then a mirrored scale), the viewpoint off to the
    // side and above: upright, its normal level towards the viewpoint, position, scale and mirror kept.
    for (double yawDeg : { 0.0, 47.0, 180.0, -120.0 }) {
        for (double pitchDeg : { 0.0, -30.0, 15.0 }) {
            BbMat b = bbCamera(yawDeg, pitchDeg);
            b.translate(-250.0, 40.0, 900.0);
            b.scale(-0.8, 0.8, 0.8);
            float m[16], orig[16];
            b.toFloat(m);
            std::memcpy(orig, m, sizeof(m));
            const float vp[3] = { 100.0f, 400.0f, 300.0f };
            CHECK(RT64::vrFaceBillboard(m, vp, RT64::VrBillboardMode::Cylindrical));
            for (int i = 12; i < 16; i++) {
                CHECK(m[i] == orig[i]);
            }
            CHECK_NEAR(m[4], 0.0, 1e-5);
            CHECK_NEAR(m[5], 0.8, 1e-5);
            CHECK_NEAR(m[6], 0.0, 1e-5);
            const Vec3 back = bbRow(m, 2);
            CHECK_NEAR(back.y, 0.0, 1e-5);
            CHECK_NEAR(bbLen(back), 0.8, 1e-5);
            const Vec3 toView = normalize(Vec3{ vp[0] - m[12], 0.0f, vp[2] - m[14] });
            CHECK_NEAR(dot(normalize(back), toView), 1.0, 1e-5);
            CHECK_NEAR(bbLen(bbRow(m, 0)), 0.8, 1e-5);
            CHECK_NEAR(bbRow(m, 0).y, 0.0, 1e-5);
            CHECK_NEAR(bbDet(m), bbDet(orig), 1e-5);  // still mirrored
        }
    }
}

static void testBillboardSphericalKeepsRoll() {
    // A particle (func_80344424: camera basis, roll, scale): it faces the viewpoint with the same
    // roll relative to the level axis, and the same scale.
    const double rollDeg = 37.0;
    BbMat b = bbCamera(110.0, -18.0);
    b.roll(rollDeg);
    b.translate(500.0, 120.0, -60.0);
    b.scale(2.0, 2.0, 2.0);
    float m[16], orig[16];
    b.toFloat(m);
    std::memcpy(orig, m, sizeof(m));
    const float vp[3] = { -300.0f, 260.0f, 410.0f };
    CHECK(RT64::vrFaceBillboard(m, vp, RT64::VrBillboardMode::Spherical));
    const Vec3 toView = normalize(Vec3{ vp[0] - m[12], vp[1] - m[13], vp[2] - m[14] });
    CHECK_NEAR(dot(normalize(bbRow(m, 2)), toView), 1.0, 1e-6);
    const Vec3 right = normalize(cross(Vec3{ 0.0f, 1.0f, 0.0f }, toView));
    const Vec3 up = cross(toView, right);
    const Vec3 x = bbRow(m, 0);
    CHECK_NEAR(std::atan2(dot(x, up), dot(x, right)) * 180.0 / M_PI, rollDeg, 1e-3);
    for (int r = 0; r < 3; r++) {
        CHECK_NEAR(bbLen(bbRow(m, r)), 2.0, 1e-5);
    }
    CHECK_NEAR(dot(bbRow(m, 0), bbRow(m, 1)), 0.0, 1e-4);
    CHECK_NEAR(bbDet(m), bbDet(orig), 1e-4);
    CHECK(m[12] == orig[12] && m[13] == orig[13] && m[14] == orig[14]);
}

static void testBillboardSideOfGameView() {
    // The case it is for: a billboard 90 degrees to the side of the game camera is edge-on to a
    // viewer at the camera; facing turns it to the viewer.
    BbMat b = bbCamera(0.0, 0.0);  // camera looking down -Z
    b.translate(800.0, 0.0, 0.0);  // to its right
    float m[16];
    b.toFloat(m);
    const float vp[3] = { 0.0f, 0.0f, 0.0f };
    CHECK_NEAR(dot(normalize(bbRow(m, 2)), Vec3{ -1.0f, 0.0f, 0.0f }), 0.0, 1e-6);  // edge-on
    CHECK(RT64::vrFaceBillboard(m, vp, RT64::VrBillboardMode::Cylindrical));
    CHECK_NEAR(dot(normalize(bbRow(m, 2)), Vec3{ -1.0f, 0.0f, 0.0f }), 1.0, 1e-6);
}

static void testBillboardFlat() {
    // A flat billboard (zero Z scale, as DK64's sprites): its normal comes from rows 0 and 1, on
    // the game camera's side; mirroring is kept.
    for (double sx : { 1.25, -1.25 }) {
        BbMat b = bbCamera(35.0, -15.0);
        b.roll(20.0);
        b.translate(400.0, 30.0, -900.0);
        b.scale(sx, 1.25, 0.0);
        float m[16];
        b.toFloat(m);
        const Vec3 camBack = normalize(Vec3{ float(bbCamera(35.0, -15.0).m[2][0]), float(bbCamera(35.0, -15.0).m[2][1]), float(bbCamera(35.0, -15.0).m[2][2]) });
        const float game[3] = { m[12] + camBack.x * 500.0f, m[13] + camBack.y * 500.0f, m[14] + camBack.z * 500.0f };
        // Facing the game camera itself changes nothing.
        float same[16];
        std::memcpy(same, m, sizeof(m));
        CHECK(RT64::vrFaceBillboard(same, game, RT64::VrBillboardMode::Spherical, game));
        for (int i = 0; i < 16; i++) {
            CHECK_NEAR(same[i], m[i], 2e-5);
        }
        const float vp[3] = { -200.0f, 300.0f, 100.0f };
        CHECK(RT64::vrFaceBillboard(m, vp, RT64::VrBillboardMode::Spherical, game));
        const Vec3 toView = normalize(Vec3{ vp[0] - m[12], vp[1] - m[13], vp[2] - m[14] });
        const Vec3 n = cross(bbRow(m, 0), bbRow(m, 1));
        CHECK_NEAR(dot(normalize(n), toView), sx > 0.0 ? 1.0 : -1.0, 1e-5);
        CHECK_NEAR(bbLen(bbRow(m, 2)), 0.0, 1e-6);
        CHECK_NEAR(bbLen(bbRow(m, 0)), 1.25, 1e-5);
    }
}

static void testBillboardPivot() {
    // A model's billboard node (func_803384A8): translate(-c), scale, the camera's yaw, then the
    // node's position p. Turned about c, the node stays at p and faces the viewpoint from there.
    const double c[3] = { 450.0, 330.0, -190.0 };
    BbMat rot = bbCamera(140.0, 0.0);  // yaw only (cylindrical node)
    BbMat m3;
    // M = T(-c) * S * R * T(p) in row-vector order: rows = R scaled, row 3 = p - c * S * R.
    const double s = 1.5;
    const double p[3] = { 2100.0, 400.0, -800.0 };
    for (int r = 0; r < 3; r++) {
        for (int k = 0; k < 3; k++) {
            m3.m[r][k] = rot.m[r][k] * s;
        }
    }
    for (int k = 0; k < 3; k++) {
        m3.m[3][k] = p[k] - (c[0] * m3.m[0][k] + c[1] * m3.m[1][k] + c[2] * m3.m[2][k]);
    }
    float m[16];
    m3.toFloat(m);
    const float pivot[3] = { float(c[0]), float(c[1]), float(c[2]) };
    const float vp[3] = { 2500.0f, 900.0f, 300.0f };
    CHECK(RT64::vrFaceBillboard(m, vp, RT64::VrBillboardMode::Cylindrical, nullptr, pivot));
    const Vec3 at = transformPoint(Vec3{ pivot[0], pivot[1], pivot[2] }, Mat4{ { { m[0], m[1], m[2], m[3] }, { m[4], m[5], m[6], m[7] }, { m[8], m[9], m[10], m[11] }, { m[12], m[13], m[14], m[15] } } });
    CHECK_NEAR(at.x, p[0], 2e-2);
    CHECK_NEAR(at.y, p[1], 2e-2);
    CHECK_NEAR(at.z, p[2], 2e-2);
    const Vec3 toView = normalize(Vec3{ vp[0] - float(p[0]), 0.0f, vp[2] - float(p[2]) });
    CHECK_NEAR(dot(normalize(bbRow(m, 2)), toView), 1.0, 1e-5);
    CHECK_NEAR(bbRow(m, 1).y, 1.5, 1e-5);
    // Without the pivot it would turn about the far-away model origin and leave the node's place.
    float origin[16];
    m3.toFloat(origin);
    CHECK(RT64::vrFaceBillboard(origin, vp, RT64::VrBillboardMode::Cylindrical));
    const Vec3 moved = transformPoint(Vec3{ pivot[0], pivot[1], pivot[2] }, Mat4{ { { origin[0], origin[1], origin[2], origin[3] }, { origin[4], origin[5], origin[6], origin[7] }, { origin[8], origin[9], origin[10], origin[11] }, { origin[12], origin[13], origin[14], origin[15] } } });
    CHECK(std::fabs(moved.x - float(p[0])) + std::fabs(moved.z - float(p[2])) > 100.0f);
}

static void testBillboardMatching() {
    // Matching: upright when the game built it upright (yaw only), complete when it pitched it.
    const float vp[3] = { 100.0f, 500.0f, 300.0f };
    BbMat level = bbCamera(60.0, 0.0);
    level.translate(-200.0, 0.0, -400.0);
    float a[16], b[16];
    level.toFloat(a);
    level.toFloat(b);
    CHECK(RT64::vrFaceBillboard(a, vp, RT64::VrBillboardMode::Matching));
    CHECK(RT64::vrFaceBillboard(b, vp, RT64::VrBillboardMode::Cylindrical));
    for (int i = 0; i < 16; i++) {
        CHECK(a[i] == b[i]);
    }
    BbMat pitched = bbCamera(60.0, -25.0);
    pitched.translate(-200.0, 0.0, -400.0);
    pitched.toFloat(a);
    pitched.toFloat(b);
    CHECK(RT64::vrFaceBillboard(a, vp, RT64::VrBillboardMode::Matching));
    CHECK(RT64::vrFaceBillboard(b, vp, RT64::VrBillboardMode::Spherical));
    for (int i = 0; i < 16; i++) {
        CHECK(a[i] == b[i]);
    }
}

static void testBillboardDegenerate() {
    float m[16], orig[16];
    BbMat b = bbCamera(20.0, -10.0);
    b.translate(5.0, 6.0, 7.0);
    b.toFloat(m);
    std::memcpy(orig, m, sizeof(m));
    const float at[3] = { 5.0f, 6.0f, 7.0f };
    CHECK(!RT64::vrFaceBillboard(m, at, RT64::VrBillboardMode::Spherical));
    CHECK(!RT64::vrFaceBillboard(m, at, RT64::VrBillboardMode::Cylindrical));
    const float above[3] = { 5.0f, 600.0f, 7.0f };
    CHECK(!RT64::vrFaceBillboard(m, above, RT64::VrBillboardMode::Cylindrical));
    CHECK(!RT64::vrFaceBillboard(m, above, RT64::VrBillboardMode::None));
    CHECK(std::memcmp(m, orig, sizeof(m)) == 0);
    // Straight above, spherical keeps the game's level axis.
    CHECK(RT64::vrFaceBillboard(m, above, RT64::VrBillboardMode::Spherical));
    CHECK_NEAR(dot(normalize(bbRow(m, 2)), Vec3{ 0.0f, 1.0f, 0.0f }), 1.0, 1e-6);
    CHECK_NEAR(dot(normalize(bbRow(m, 0)), normalize(bbRow(orig, 0))), 1.0, 1e-5);
    // Built looking straight down: no level axis to recover.
    BbMat down = bbCamera(0.0, -90.0);
    down.toFloat(m);
    std::memcpy(orig, m, sizeof(m));
    const float vp[3] = { 100.0f, 50.0f, 0.0f };
    CHECK(!RT64::vrFaceBillboard(m, vp, RT64::VrBillboardMode::Spherical));
    CHECK(std::memcmp(m, orig, sizeof(m)) == 0);
}

static void testBillboardGroups() {
    const RT64::VrBillboardGroup groups[2] = {
        { 0x40000000u, 0x7FFFFFFFu, RT64::VrBillboardMode::Cylindrical },
        { 0x80000000u, 0xBFFFFFFFu, RT64::VrBillboardMode::Spherical } };
    CHECK(RT64::vrBillboardMode(groups, 2, 0x3FFFFFFFu) == RT64::VrBillboardMode::None);
    CHECK(RT64::vrBillboardMode(groups, 2, 0x40000000u) == RT64::VrBillboardMode::Cylindrical);
    CHECK(RT64::vrBillboardMode(groups, 2, 0x7FFFFFFFu) == RT64::VrBillboardMode::Cylindrical);
    CHECK(RT64::vrBillboardMode(groups, 2, 0x80000000u) == RT64::VrBillboardMode::Spherical);
    CHECK(RT64::vrBillboardMode(groups, 2, 0xC0000000u) == RT64::VrBillboardMode::None);
    CHECK(RT64::vrBillboardMode(groups, 2, 0xFFFFFFFFu) == RT64::VrBillboardMode::None);  // G_EX_ID_AUTO
    CHECK(RT64::vrBillboardMode(groups, 0, 0x40000000u) == RT64::VrBillboardMode::None);
}

static void testBillboardViewpointBothEyes() {
    // Both eyes face one point, midway between them through the same view as the eye views; a
    // head turn about that point leaves it (and so every billboard) where it is.
    Mat4 game = guLookAt({ 300.0f, 200.0f, 900.0f }, { 0.0f, 50.0f, 0.0f }, { 0.0f, 1.0f, 0.0f });
    Pose l, r;
    l.p = { -0.032f, 0.01f, 0.02f };
    r.p = { 0.032f, 0.01f, 0.02f };
    const float ws = 100.0f;
    const Vec3 vp = billboardViewpoint(game, l, r, ws);
    const Vec3 pl = viewPosition(eyeView(game, l, ws)), pr = viewPosition(eyeView(game, r, ws));
    CHECK_NEAR(vp.x, 0.5 * (pl.x + pr.x), 1e-3);
    CHECK_NEAR(vp.y, 0.5 * (pl.y + pr.y), 1e-3);
    CHECK_NEAR(vp.z, 0.5 * (pl.z + pr.z), 1e-3);
    // The head turned 70 degrees and pitched 20 about the point between the eyes.
    const Quat turn = quatMul(quatFromYaw(70.0f * float(M_PI) / 180.0f), Quat{ std::sin(0.5f * 20.0f * float(M_PI) / 180.0f), 0.0f, 0.0f, std::cos(0.5f * 20.0f * float(M_PI) / 180.0f) });
    const Vec3 mid = { 0.0f, 0.01f, 0.02f };
    Pose l2, r2;
    l2.q = r2.q = turn;
    const Vec3 dl = quatRotate(turn, Vec3{ -0.032f, 0.0f, 0.0f }), dr = quatRotate(turn, Vec3{ 0.032f, 0.0f, 0.0f });
    l2.p = { mid.x + dl.x, mid.y + dl.y, mid.z + dl.z };
    r2.p = { mid.x + dr.x, mid.y + dr.y, mid.z + dr.z };
    const Vec3 vp2 = billboardViewpoint(game, l2, r2, ws);
    CHECK_NEAR(vp2.x, vp.x, 1e-3);
    CHECK_NEAR(vp2.y, vp.y, 1e-3);
    CHECK_NEAR(vp2.z, vp.z, 1e-3);
    // Leaning 0.3 m to the right moves it 30 units along the game camera's right axis.
    Pose l3 = l, r3 = r;
    l3.p.x += 0.3f;
    r3.p.x += 0.3f;
    const Vec3 vp3 = billboardViewpoint(game, l3, r3, ws);
    const Mat4 cam = rigidInverse(game);
    const Vec3 rightAxis = { cam.m[0][0], cam.m[0][1], cam.m[0][2] };
    CHECK_NEAR(dot(Vec3{ vp3.x - vp.x, vp3.y - vp.y, vp3.z - vp.z }, rightAxis), 30.0, 1e-2);
}

int main() {
    testEyeProjectionMatchesGuPerspective();
    testEyeProjectionAsymmetricEdges();
    testEyeViewIdentityPose();
    testEyeViewTranslationAndScale();
    testEyeViewYaw();
    testEyeDisparityDirection();
    testLevelView();
    testPoseMath();
    testFogRemap();
    testEyeNearAndFog();
    testEyeNearSettings();
    testSchedulerSteady120();
    testSchedulerSteady90And72();
    testSchedulerJitter();
    testSchedulerTwentyHz();
    testSchedulerDrift();
    testSchedulerStall();
    testSchedulerWeightEdges();
    testSettings();
    testFoveation();
    testActionNamesUnique();
    testBindingPaths();
    testN64Map();
    testStickRelativeToView();
    testTurnAndView();
    testLiveAdjustment();
    testMenuNavigation();
    testMerge();
    testLayerPoses();
    testOrbitView();
    testHudSolve();
    testCinema();
    testEyeSize();
    testCameraPoseFromView();
    testCameraSmoothingLatency();
    testCameraSmoothingContinuity();
    testCameraSmoothingFollower();
    testCameraSmoothingCuts();
    testCameraSmoothingWrap();
    testSmoothTurn();
    testCameraOrigin();
    testCameraOriginComposition();
    testWorldScaleConsumers();
    testCameraOriginSettings();
    testChord();
    testWorldSize();
    testMenuModel();
    testBillboardGameBasis();
    testBillboardFacesViewpointAlongItsAxis();
    testBillboardCylindrical();
    testBillboardSphericalKeepsRoll();
    testBillboardSideOfGameView();
    testBillboardFlat();
    testBillboardPivot();
    testBillboardMatching();
    testBillboardDegenerate();
    testBillboardGroups();
    testBillboardViewpointBothEyes();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
