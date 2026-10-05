// Host unit tests for the DK64 VR bridge: the display-time scheduler, the eye projection and
// view, level pitch, the fog remap and the settings file. No window, GPU or runtime.
//
//   tools/docker/build-linux.sh vrtest     (or any C++17 compiler, see src/vr/tests/CMakeLists.txt)
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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
    CHECK_NEAR(dk64vr_settings_get(&s, "refresh_rate"), 120.0, 0.0);
    CHECK_NEAR(dk64vr_settings_get(&s, "resolution_scale"), 1.25, 1e-9);
    CHECK_NEAR(dk64vr_settings_get(&s, "world_scale"), 25.0, 0.0);
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
    CHECK_NEAR(s.file_values[dk64vr_settings_index("resolution_scale")], 1.25, 1e-9);

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
    dk64vr_settings_free(&s);
    dk64vr_log_set_sink(nullptr);
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
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
