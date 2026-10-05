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

#include "dk64vr_input.h"
#include "dk64vr_math.h"
#include "dk64vr_sched.h"
#include "dk64vr_settings.h"
#include "dk64vr_log.h"

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
    CHECK_NEAR(dk64vr_settings_get(&s, "resolution_scale"), 1.0, 1e-9);
    CHECK_NEAR(dk64vr_settings_get(&s, "world_scale"), 15.0, 0.0);
    CHECK_NEAR(dk64vr_settings_get(&s, "hud_distance"), 1.5, 1e-9);
    CHECK_NEAR(dk64vr_settings_get(&s, "turn_mode"), 0.0, 0.0);
    CHECK_NEAR(dk64vr_settings_get(&s, "snap_angle"), 30.0, 0.0);
    CHECK_NEAR(dk64vr_settings_get(&s, "right_stick_x"), 0.0, 0.0);
    CHECK_NEAR(dk64vr_settings_get(&s, "cutscenes"), 0.0, 0.0);
    CHECK_NEAR(dk64vr_settings_get(&s, "level_pitch"), 1.0, 0.0);

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
    CHECK_NEAR(s.file_values[dk64vr_settings_index("resolution_scale")], 1.0, 1e-9);

    // Round trip: what format() writes parses back to the same values with nothing to repair.
    size_t len = 0;
    char *out = dk64vr_settings_format(&s, &len);
    CHECK(out != nullptr);
    if (out != nullptr) {
        CHECK(std::strstr(out, "my_own_key = 3") != nullptr);
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

    // Schema 2 migration (lesson 79): old defaults in a schema-1 file move to the new ones once,
    // values the user chose stay, and a schema-2 file is left alone.
    const char *old =
        "version = 1\n"
        "[vr]\n"
        "world_scale = 25\n"
        "resolution_scale = 1.25\n"
        "refresh_rate = 90\n";
    CHECK(dk64vr_settings_parse(&s, old, std::strlen(old)) > 0);
    CHECK_NEAR(s.file_values[dk64vr_settings_index("world_scale")], 15.0, 0.0);
    CHECK_NEAR(s.file_values[dk64vr_settings_index("resolution_scale")], 1.0, 1e-9);
    CHECK_NEAR(s.file_values[dk64vr_settings_index("refresh_rate")], 90.0, 0.0);
    CHECK_NEAR(s.file_values[dk64vr_settings_index("port_schema")], 3.0, 0.0);
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
    // The Frame is asymmetric: these must be on the hand the Frame has them on.
    const ProfileBindings &frame = profileAt(0);
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
    InputResult r = m.update(s, config, context, 1.0, 1.0f / 72.0f);
    // DK64 Recompiled's defaults on an Xbox-layout pad (src/game/config.cpp set_control_defaults).
    CHECK(r.buttons == (n64::A | n64::B | n64::CLeft | n64::CRight | n64::Start | n64::CDown | n64::CUp | n64::L | n64::DUp | n64::DLeft | n64::Z | n64::R));
    // Trigger hysteresis: 0.4 keeps Z held, 0.3 releases it, 0.45 does not re-press.
    s = sampleIdle();
    s.lTrigger = 0.4f;
    r = m.update(s, config, context, 1.02, 1.0f / 72.0f);
    CHECK((r.buttons & n64::Z) != 0);
    s.lTrigger = 0.3f;
    r = m.update(s, config, context, 1.04, 1.0f / 72.0f);
    CHECK((r.buttons & n64::Z) == 0);
    s.lTrigger = 0.45f;
    r = m.update(s, config, context, 1.06, 1.0f / 72.0f);
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
    CHECK_NEAR(m.update(s, config, context, t += 0.014, 0.014f).turnDeg, 30.0, 1e-5);
    CHECK_NEAR(m.update(s, config, context, t += 0.014, 0.014f).turnDeg, 0.0, 1e-5);
    s.rStick[0] = 0.1f;
    m.update(s, config, context, t += 0.014, 0.014f);
    s.rStick[0] = -0.9f;
    CHECK_NEAR(m.update(s, config, context, t += 0.014, 0.014f).turnDeg, -30.0, 1e-5);
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
    testSchedulerSteady120();
    testSchedulerSteady90And72();
    testSchedulerJitter();
    testSchedulerTwentyHz();
    testSchedulerDrift();
    testSchedulerStall();
    testSchedulerWeightEdges();
    testSettings();
    testActionNamesUnique();
    testBindingPaths();
    testN64Map();
    testStickRelativeToView();
    testTurnAndView();
    testMenuNavigation();
    testMerge();
    testLayerPoses();
    testOrbitView();
    testHudSolve();
    testCinema();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
