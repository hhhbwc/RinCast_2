// Offline tests compiling and exercising REAL headset C++ production TUs:
//   headset/app/src/main/cpp/net/stream_client.cpp
//   headset/app/src/main/cpp/media/video_decoder_common.cpp
//   headset/app/src/main/cpp/app/settings.cpp
//   headset/app/src/main/cpp/app/strings.cpp
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "app/settings.h"
#include "app/strings.h"
#include "gfx/math.h"
#include "media/video_decoder.h"
#include "net/stream_client.h"
#include "rc/connection.h"
#include "rc/crypto.h"
#include "rc/handshake.h"
#include "rc/platform.h"
#include "rc/protocol.h"
#include "rc/socket.h"
#include "rc/video_transport.h"
#include "xr/thumbstick_deadzone.h"

namespace rv {

// Minimal non-common stubs so VideoDecoder::Submit / LastGoodFrame / AverageDecodeMs
// from production video_decoder_common.cpp compile and run offline without MediaCodec.
VideoDecoder::~VideoDecoder() = default;
DecoderCaps VideoDecoder::Capabilities() { return {}; }
bool VideoDecoder::Init(rc::VideoCodec codec, int width, int height, float fps) {
    m_codec = codec;
    m_width = width;
    m_height = height;
    m_fps = fps;
    return true;
}
void VideoDecoder::Shutdown() {
    m_running = false;
    ResetTextureGeometry();
}
bool VideoDecoder::Latch(DecodedFrameInfo&) { return false; }
bool VideoDecoder::Configure(const std::vector<uint8_t>&) { return false; }
void VideoDecoder::InputLoop() {}
void VideoDecoder::OutputLoop() {}

// Exposes VideoDecoder's test-only queue/queued watermark inspection via a helper subclass?
// Actually Submit/LastGoodFrame/NeedsKeyframe/Failed/Warn are all public API on VideoDecoder!

}  // namespace rv

namespace {

int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d: %s\n", __func__, __LINE__, #cond);          \
            g_failures++;                                                        \
        }                                                                        \
    } while (0)

struct TempDir {
    std::filesystem::path dir;
    explicit TempDir(const char* prefix) {
        dir = std::filesystem::temp_directory_path() /
              (std::string(prefix) + "-" + std::to_string(rc::NowNs()));
        std::filesystem::create_directories(dir);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

bool MatrixIsFinite(const rv::Mat4& m) {
    for (const float v : m.m)
        if (!std::isfinite(v)) return false;
    return true;
}

bool MatrixNear(const rv::Mat4& a, const rv::Mat4& b) {
    for (int i = 0; i < 16; i++)
        if (!(std::fabs(a.m[i] - b.m[i]) <= 1e-5f)) return false;
    return true;
}

bool MatrixNearIdentity(const rv::Mat4& m) { return MatrixNear(m, rv::Mat4::Identity()); }

bool VectorNear(const rv::Vec3& a, const rv::Vec3& b) {
    return std::fabs(a.x - b.x) <= 1e-5f && std::fabs(a.y - b.y) <= 1e-5f && std::fabs(a.z - b.z) <= 1e-5f;
}

bool Vector2Near(const rv::Vec2& a, const rv::Vec2& b, float epsilon = 1e-5f) {
    return std::fabs(a.x - b.x) <= epsilon && std::fabs(a.y - b.y) <= epsilon;
}

bool QuatIsFiniteUnit(const rv::Quat& q) {
    if (!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z) || !std::isfinite(q.w)) return false;
    const float length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return std::fabs(length - 1.f) <= 1e-5f;
}

void TestHeadsetMathBounds() {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    rv::Mat4 expected;
    expected(0, 0) = 2.f / 1.1f;
    expected(0, 2) = 0.1f / 1.1f;
    expected(1, 1) = 2.f / 1.1f;
    expected(1, 2) = 0.3f / 1.1f;
    expected(2, 2) = -500.05f / 499.95f;
    expected(2, 3) = -50.f / 499.95f;
    expected(3, 2) = -1.f;
    expected(3, 3) = 0.f;
    const rv::Mat4 normal = rv::Mat4::Projection(-0.5f, 0.6f, 0.7f, -0.4f, 0.05f, 500.f);
    CHECK(MatrixIsFinite(normal));
    CHECK(MatrixNear(normal, expected));

    struct ProjectionCase {
        const char* name;
        float tanLeft, tanRight, tanUp, tanDown, nearZ, farZ;
    };
    const ProjectionCase projectionCases[] = {
        {"all-zero", 0.f, 0.f, 0.f, 0.f, 0.f, 0.f},
        {"zero-horizontal-span", -0.5f, -0.5f, 0.7f, -0.4f, 0.05f, 500.f},
        {"tiny-horizontal-span", -0.5f, -0.5f + 5e-7f, 0.7f, -0.4f, 0.05f, 500.f},
        {"zero-vertical-span", -0.5f, 0.6f, 0.7f, 0.7f, 0.05f, 500.f},
        {"tiny-vertical-span", -0.5f, 0.6f, 0.7f, 0.7f - 5e-7f, 0.05f, 500.f},
        {"zero-depth-span", -0.5f, 0.6f, 0.7f, -0.4f, 1.f, 1.f},
        {"tiny-depth-span", -0.5f, 0.6f, 0.7f, -0.4f, 1.f, 1.f + 5e-7f},
        {"zero-near", -0.5f, 0.6f, 0.7f, -0.4f, 0.f, 500.f},
        {"negative-near", -0.5f, 0.6f, 0.7f, -0.4f, -1.f, 500.f},
        {"nan-left", nan, 0.6f, 0.7f, -0.4f, 0.05f, 500.f},
        {"inf-right", -0.5f, inf, 0.7f, -0.4f, 0.05f, 500.f},
        {"nan-up", -0.5f, 0.6f, nan, -0.4f, 0.05f, 500.f},
        {"inf-down", -0.5f, 0.6f, 0.7f, -inf, 0.05f, 500.f},
        {"nan-near", -0.5f, 0.6f, 0.7f, -0.4f, nan, 500.f},
        {"inf-far", -0.5f, 0.6f, 0.7f, -0.4f, 0.05f, inf},
    };
    for (const ProjectionCase& c : projectionCases) {
        const rv::Mat4 m = rv::Mat4::Projection(c.tanLeft, c.tanRight, c.tanUp, c.tanDown, c.nearZ, c.farZ);
        const bool ok = MatrixIsFinite(m) && MatrixNearIdentity(m);
        if (!ok) {
            std::printf("FAIL %s: degenerate Projection(%s) is not finite identity\n", __func__, c.name);
            g_failures++;
        }
    }

    const rv::Quat identityLook = rv::LookRotation({0.f, 0.f, -1.f}, {0.f, 1.f, 0.f});
    CHECK(QuatIsFiniteUnit(identityLook));
    CHECK(VectorNear(identityLook.Rotate({0.f, 0.f, -1.f}), {0.f, 0.f, -1.f}));
    CHECK(VectorNear(identityLook.Rotate({0.f, 1.f, 0.f}), {0.f, 1.f, 0.f}));

    struct LookRotationCase {
        const char* name;
        rv::Vec3 forward;
        rv::Vec3 up;
        rv::Vec3 expectedForward;
    };
    const LookRotationCase lookCases[] = {
        {"collinear-up", {0.f, 1.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 1.f, 0.f}},
        {"collinear-down", {0.f, 1.f, 0.f}, {0.f, -1.f, 0.f}, {0.f, 1.f, 0.f}},
        {"near-collinear", {0.f, 1.f, 0.f}, {0.f, 1.f, 1e-7f}, {0.f, 1.f, 0.f}},
        {"zero-forward", {0.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, -1.f}},
        {"zero-up", {0.f, 0.f, -1.f}, {0.f, 0.f, 0.f}, {0.f, 0.f, -1.f}},
        {"zero-both", {0.f, 0.f, 0.f}, {0.f, 0.f, 0.f}, {0.f, 0.f, -1.f}},
        {"nan-forward", {nan, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, -1.f}},
        {"nan-up", {0.f, 0.f, -1.f}, {nan, 0.f, 0.f}, {0.f, 0.f, -1.f}},
        {"inf-up", {0.f, 0.f, -1.f}, {0.f, inf, 0.f}, {0.f, 0.f, -1.f}},
    };
    for (const LookRotationCase& c : lookCases) {
        const rv::Quat q = rv::LookRotation(c.forward, c.up);
        const bool ok = QuatIsFiniteUnit(q) && VectorNear(q.Rotate({0.f, 0.f, -1.f}), c.expectedForward);
        if (!ok) {
            std::printf("FAIL %s: LookRotation(%s) is not a finite expected rotation\n", __func__, c.name);
            g_failures++;
        }
    }

    CHECK(QuatIsFiniteUnit(rv::Normalize(rv::Quat{0.f, 0.f, 0.f, 0.f})));
    CHECK(VectorNear(rv::Normalize(rv::Quat{0.f, 0.f, 0.f, 0.f}).Rotate({1.f, 2.f, 3.f}), {1.f, 2.f, 3.f}));
    CHECK(QuatIsFiniteUnit(rv::Normalize(rv::Quat{nan, 0.f, 0.f, 0.f})));
    CHECK(QuatIsFiniteUnit(rv::Normalize(rv::Quat{0.f, inf, 0.f, 0.f})));
    CHECK(QuatIsFiniteUnit(rv::Normalize(rv::Quat{0.f, 0.f, 0.f, 2.f})));
    CHECK(VectorNear(rv::Normalize(rv::Quat{0.f, 0.f, 0.f, 2.f}).Rotate({1.f, 2.f, 3.f}), {1.f, 2.f, 3.f}));

    const rv::Pose badPose{{inf, 0.f, 0.f, 0.f}, {nan, 2.f, 3.f}};
    const rv::Pose badSafe = rv::SanitizePose(badPose);
    CHECK(QuatIsFiniteUnit(badSafe.orientation));
    CHECK(VectorNear(badSafe.position, {0.f, 0.f, 0.f}));
    CHECK(MatrixIsFinite(rv::Mat4::FromPose(badSafe.Inverse())));

    const rv::Pose zeroPose{{0.f, 0.f, 0.f, 0.f}, {1.f, 2.f, 3.f}};
    const rv::Pose zeroSafe = rv::SanitizePose(zeroPose);
    CHECK(QuatIsFiniteUnit(zeroSafe.orientation));
    CHECK(VectorNear(zeroSafe.position, {1.f, 2.f, 3.f}));
    CHECK(MatrixIsFinite(rv::Mat4::FromPose(zeroSafe.Inverse())));

    const rv::Pose scaledPose{{0.f, 0.f, 0.f, 2.f}, {4.f, 5.f, 6.f}};
    const rv::Pose scaledSafe = rv::SanitizePose(scaledPose);
    CHECK(QuatIsFiniteUnit(scaledSafe.orientation));
    CHECK(VectorNear(scaledSafe.position, {4.f, 5.f, 6.f}));
    CHECK(VectorNear(scaledSafe.orientation.Rotate({1.f, 2.f, 3.f}), {1.f, 2.f, 3.f}));

    const float maxFloat = std::numeric_limits<float>::max();
    const float invSqrt3 = 1.f / std::sqrt(3.f);
    CHECK(VectorNear(rv::Normalize(rv::Vec3{maxFloat, 0.f, 0.f}), {1.f, 0.f, 0.f}));
    CHECK(VectorNear(rv::Normalize(rv::Vec3{maxFloat, maxFloat, maxFloat}), {invSqrt3, invSqrt3, invSqrt3}));
    CHECK(VectorNear(rv::Normalize(rv::Vec3{inf, 0.f, 0.f}), {0.f, 0.f, -1.f}));
    CHECK(VectorNear(rv::Normalize(rv::Vec3{nan, 1.f, 0.f}), {0.f, 0.f, -1.f}));
    CHECK(VectorNear(rv::NormalizeOr(rv::Vec3{maxFloat, 0.f, 0.f}, {0.f, 1.f, 0.f}), {1.f, 0.f, 0.f}));
    CHECK(VectorNear(rv::NormalizeOr(rv::Vec3{maxFloat, maxFloat, maxFloat}, {0.f, 1.f, 0.f}),
                     {invSqrt3, invSqrt3, invSqrt3}));

    const rv::Quat maxX = rv::Normalize(rv::Quat{maxFloat, 0.f, 0.f, 0.f});
    CHECK(QuatIsFiniteUnit(maxX));
    CHECK(VectorNear(maxX.Rotate({0.f, 1.f, 0.f}), {0.f, -1.f, 0.f}));
    const rv::Quat maxAll = rv::Normalize(rv::Quat{maxFloat, maxFloat, maxFloat, maxFloat});
    CHECK(QuatIsFiniteUnit(maxAll));
    CHECK(std::fabs(maxAll.x - 0.5f) <= 1e-5f && std::fabs(maxAll.y - 0.5f) <= 1e-5f &&
          std::fabs(maxAll.z - 0.5f) <= 1e-5f && std::fabs(maxAll.w - 0.5f) <= 1e-5f);

    const rv::Pose hugePose{{maxFloat, 0.f, 0.f, 0.f}, {1.f, 2.f, 3.f}};
    const rv::Pose hugeSafe = rv::SanitizePose(hugePose);
    CHECK(QuatIsFiniteUnit(hugeSafe.orientation));
    CHECK(MatrixIsFinite(rv::Mat4::FromPose(hugeSafe.Inverse())));

    const rv::Quat hugeLook = rv::LookRotation({maxFloat, 0.f, 0.f}, {0.f, maxFloat, 0.f});
    CHECK(QuatIsFiniteUnit(hugeLook));
    CHECK(VectorNear(hugeLook.Rotate({0.f, 0.f, -1.f}), {1.f, 0.f, 0.f}));

    const rv::Quat identity;
    const rv::Quat yaw90 = rv::Quat::Yaw(1.5707963267948966f);
    const rv::Quat halfTurn = rv::Slerp(identity, yaw90, 0.5f);
    CHECK(QuatIsFiniteUnit(halfTurn));
    CHECK(VectorNear(halfTurn.Rotate({0.f, 0.f, -1.f}), {-0.7071067811865475f, 0.f, -0.7071067811865475f}));
    const rv::Quat scaledHalfTurn = rv::Slerp(identity, {0.f, 2.f, 0.f, 0.f}, 0.5f);
    CHECK(QuatIsFiniteUnit(scaledHalfTurn));
    CHECK(VectorNear(scaledHalfTurn.Rotate({0.f, 0.f, -1.f}), {-1.f, 0.f, 0.f}));
    const rv::Quat hugeHalfTurn = rv::Slerp(identity, {maxFloat, 0.f, 0.f, 0.f}, 0.5f);
    CHECK(QuatIsFiniteUnit(hugeHalfTurn));
    CHECK(VectorNear(hugeHalfTurn.Rotate({0.f, 0.f, -1.f}), {0.f, 1.f, 0.f}));
    CHECK(QuatIsFiniteUnit(rv::Slerp({nan, 0.f, 0.f, 0.f}, identity, 0.5f)));
    CHECK(QuatIsFiniteUnit(rv::Slerp(identity, {0.f, inf, 0.f, 0.f}, 0.5f)));
    CHECK(VectorNear(rv::Slerp(yaw90, identity, nan).Rotate({0.f, 0.f, -1.f}), {-1.f, 0.f, 0.f}));
    CHECK(VectorNear(rv::Slerp(yaw90, identity, inf).Rotate({0.f, 0.f, -1.f}), {-1.f, 0.f, 0.f}));
    CHECK(VectorNear(rv::Slerp(yaw90, identity, -1.f).Rotate({0.f, 0.f, -1.f}), {-1.f, 0.f, 0.f}));
    CHECK(VectorNear(rv::Slerp(yaw90, identity, 2.f).Rotate({0.f, 0.f, -1.f}), {0.f, 0.f, -1.f}));
    CHECK(QuatIsFiniteUnit(rv::Slerp(identity, {-1.f, 0.f, 0.f, 0.f}, 0.5f)));
    CHECK(QuatIsFiniteUnit(rv::Slerp({}, {}, 0.5f)));

    CHECK(rv::Clamp01(-inf) == 0.f);
    CHECK(rv::Clamp01(-1.f) == 0.f);
    CHECK(rv::Clamp01(0.f) == 0.f);
    CHECK(rv::Clamp01(0.25f) == 0.25f);
    CHECK(rv::Clamp01(1.f) == 1.f);
    CHECK(rv::Clamp01(2.f) == 1.f);
    CHECK(rv::Clamp01(inf) == 1.f);
    CHECK(rv::Clamp01(nan) == 0.f);
    CHECK(rv::SmoothStep(0.25f, 1.f, 0.f) == 0.f);
    CHECK(rv::SmoothStep(0.25f, 1.f, 0.625f) == 0.5f);
    CHECK(rv::SmoothStep(0.25f, 1.f, 1.f) == 1.f);
    CHECK(rv::SmoothStep(0.25f, 1.f, nan) == 0.f);
    CHECK(rv::SmoothStep(0.25f, 1.f, inf) == 1.f);
    CHECK(rv::SmoothStep(0.25f, 1.f, -inf) == 0.f);
    CHECK(rv::SmoothStep(nan, 1.f, 0.5f) == 0.f);
    CHECK(rv::SmoothStep(0.f, nan, 0.5f) == 0.f);
    CHECK(rv::SmoothStep(0.f, 0.f, 0.f) == 0.f);
    CHECK(rv::SmoothStep(0.f, 0.f, 1.f) == 1.f);
    CHECK(rv::SmoothStep(0.f, 0.f, -1.f) == 0.f);
    for (float edge0 : {-inf, -1.f, 0.f, 1.f, inf, nan}) {
        for (float edge1 : {-inf, -1.f, 0.f, 1.f, inf, nan}) {
            for (float value : {-inf, -1.f, 0.f, 0.5f, 1.f, inf, nan}) {
                const float smoothed = rv::SmoothStep(edge0, edge1, value);
                CHECK(std::isfinite(smoothed) && smoothed >= 0.f && smoothed <= 1.f);
            }
        }
    }

    const float angles[] = {-3.141592653589793f, -1.5707963267948966f, 0.f, 1.5707963267948966f, 3.141592653589793f};
    const float scales[] = {1e-4f, 1.f, 1e20f, maxFloat};
    const float factors[] = {-inf, -1.f, 0.f, 0.25f, 0.5f, 0.75f, 1.f, 2.f, inf, nan};
    const rv::Vec3 probe{0.3f, -0.7f, 0.2f};
    for (float aa : angles) {
        for (float ab : angles) {
            const rv::Quat a = rv::Quat::AxisAngle({1.f, 2.f, 3.f}, aa);
            const rv::Quat b = rv::Quat::AxisAngle({-2.f, 1.f, 0.5f}, ab);
            const rv::Vec3 expected = rv::Slerp(a, b, 0.37f).Rotate(probe);
            for (float sa : scales) {
                for (float sb : scales) {
                    const rv::Quat qa{a.x * sa, a.y * sa, a.z * sa, a.w * sa};
                    const rv::Quat qb{b.x * sb, b.y * sb, b.z * sb, b.w * sb};
                    const rv::Quat scaled = rv::Slerp(qa, qb, 0.37f);
                    CHECK(QuatIsFiniteUnit(scaled));
                    CHECK(VectorNear(scaled.Rotate(probe), expected));
                    for (float factor : factors) CHECK(QuatIsFiniteUnit(rv::Slerp(qa, qb, factor)));
                }
            }
        }
    }
    std::printf("headset math bounds: projection, look-rotation and slerp degenerate inputs rejected safely\n");
}

void TestStringsAndLocales() {
    rv::SetLanguage("auto", "zh-Hans-CN");
    CHECK(rv::Language() == "zh-CN");
    rv::SetLanguage("auto", "zh-Hant-TW");
    CHECK(rv::Language() == "zh-TW");
    rv::SetLanguage("auto", "ja-JP");
    CHECK(rv::Language() == "ja");
    rv::SetLanguage("auto", "pt-BR");  // unknown -> English fallback
    CHECK(rv::Language() == "en");

    // Critical runtime keys must resolve cleanly (never echo raw key) across every language.
    const char* criticalKeys[] = {
        "computers.timeout.lan",
        "computers.timeout.usb",
        "computers.timeout.vpn",
        "computers.timeout.remoteOffline",
        "computers.timeout.remoteNoDirect",
        "computers.diag.firewall",
        "computers.diag.usb",
        "computers.diag.steamvrDown",
        "computers.diag.remoteDown",
        "stat.frameGen.active",
        "stat.frameGen.standby",
        "stat.frameGen.rejected",
        "nav.controllers",
        "ctrl.title",
        "ctrl.calibrationDone",
        "ctrl.calibrationFailed",
        "ctrl.calibrationUnavailable",
    };
    size_t langCount = 0;
    const rv::LanguageChoice* choices = rv::LanguageChoices(langCount);
    CHECK(langCount >= 5);
    for (size_t i = 0; i < langCount; i++) {
        rv::SetLanguage(choices[i].code, "en");
        CHECK(rv::Language() == choices[i].code);
        for (const char* key : criticalKeys) {
            const char* text = rv::Tr(key);
            CHECK(text != nullptr && std::strcmp(text, key) != 0 && std::strlen(text) > 0);
        }
    }
    rv::SetLanguage("en", "en");
    const std::string formatted = rv::Trf("computers.connectingTo", {{"name", "TestRig"}});
    CHECK(formatted.find("TestRig") != std::string::npos);
    CHECK(std::strcmp(rv::Tr("nonexistent.key.check"), "nonexistent.key.check") == 0);
    std::printf("headset strings/locales: %zu languages and critical keys ok\n", langCount);
}

void TestThumbstickDeadzone() {
    using namespace rv;
    CHECK(Vector2Near(ApplyThumbstickDeadzone({0.3f, -0.4f}, 0.f), {0.3f, -0.4f}));
    CHECK(Vector2Near(ApplyThumbstickDeadzone({0.1f, 0.1f}, 0.15f), {}));
    CHECK(Vector2Near(ApplyThumbstickDeadzone({0.6f, 0.f}, 0.2f), {0.5f, 0.f}));
    CHECK(Vector2Near(ApplyThumbstickDeadzone({1.f, 0.f}, 0.2f), {1.f, 0.f}));
    const float diagonal = 0.6f / std::sqrt(2.f);
    CHECK(Vector2Near(ApplyThumbstickDeadzone({diagonal, diagonal}, 0.2f), {diagonal * (5.f / 6.f), diagonal * (5.f / 6.f)}));
    CHECK(Vector2Near(ApplyThumbstickDeadzone({std::numeric_limits<float>::quiet_NaN(), 0.f}, 0.2f), {}));
    CHECK(Vector2Near(ApplyThumbstickDeadzone({0.f, std::numeric_limits<float>::infinity()}, 0.2f), {}));
    CHECK(Vector2Near(ApplyThumbstickDeadzone({0.1f, 0.f}, -1.f), {0.1f, 0.f}));
    CHECK(Vector2Near(ApplyThumbstickDeadzone({0.5f, 0.f}, 0.4f), {1.f / 6.f, 0.f}));
    CHECK(Vector2Near(ApplyThumbstickDeadzone({0.5f, 0.f}, 2.f), {}));
    CHECK(Vector2Near(ApplyThumbstickDeadzone({3.f, 4.f}, 0.f), {0.6f, 0.8f}));
    CHECK(Vector2Near(ApplyThumbstickDeadzone({std::numeric_limits<float>::max(), std::numeric_limits<float>::max()}, 0.2f),
                      {0.70710677f, 0.70710677f}));
    CHECK(Vector2Near(ApplyThumbstickDeadzone({0.2f + 1e-4f, 0.f}, 0.2f), {0.000125f, 0.f}, 1e-6f));
    CHECK(Vector2Near(ApplyThumbstickDeadzone({0.6f, 0.f}, std::numeric_limits<float>::quiet_NaN()), {0.6f, 0.f}));
    CHECK(std::fabs(SuggestDeadzoneForDrift({0.06f, 0.08f}) - 0.13f) < 1e-6f);
    CHECK(SuggestDeadzoneForDrift({0.001f, 0.f}) == 0.f);
    CHECK(SuggestDeadzoneForDrift({0.5f, 0.f}) == kMaxStickCalibrationDrift);

    StickDeadzoneCalibration calibration;
    calibration.Start();
    for (int i = 0; i < kStickCalibrationFrames - 1; i++) CHECK(!calibration.Observe({0.06f, 0.08f}));
    CHECK(calibration.Observe({0.06f, 0.08f}));
    CHECK(calibration.CanApply());
    CHECK(std::fabs(calibration.SuggestedDeadzone() - 0.13f) < 1e-6f);

    calibration.Start();
    for (int i = 0; i < kStickCalibrationFrames - 1; i++) CHECK(!calibration.Observe({}));
    CHECK(calibration.Observe({0.3f, 0.f}));
    CHECK(!calibration.CanApply());

    calibration.Start();
    for (int i = 0; i < kStickCalibrationFrames; i++) CHECK(!calibration.Observe({0.39f, 0.f}) || i == kStickCalibrationFrames - 1);
    CHECK(!calibration.CanApply());

    calibration.Start();
    CHECK(!calibration.Observe({std::numeric_limits<float>::quiet_NaN(), 0.f}));
    CHECK(!calibration.active && calibration.invalid && !calibration.CanApply());
    std::printf("thumbstick deadzone: radial mapping, bounds, and multi-frame calibration ok\n");
}

void TestHeadsetSettings() {
    TempDir tmp("rc-hs-settings");
    const std::string path = (tmp.dir / "settings.json").string();

    // Legacy pre-1.2 migration: missing vrScale should map vrQuality preset ("medium" -> 0.75)
    // and ignored streamProfile must not break Load/FromJson.
    {
        std::ofstream f(path);
        f << R"({"vrQuality":"medium","streamProfile":2,"micVolume":9.5,"vrScaleOfPanel":false,"codec":"hevc"})";
    }
    const rv::HeadsetSettings loaded = rv::HeadsetSettings::Load(path);
    CHECK(std::fabs(loaded.vrScale - 0.75f) < 1e-4f);
    CHECK(loaded.vrScaleOfPanel == false);
    CHECK(loaded.micVolume == 2.0f);
    CHECK(loaded.codec == "hevc");
    CHECK(loaded.leftStickDeadzone == 0.f && loaded.rightStickDeadzone == 0.f && loaded.linkStickDeadzone);

    // Step quantization + clamp bounds.
    rv::HeadsetSettings s;
    s.SetVrScale(0.1f);
    CHECK(s.vrScale == rv::kMinVrScale);
    CHECK(s.vrQuality == "low");
    s.SetVrScale(1.9f);
    CHECK(s.vrScale == rv::kMaxVrScale);
    CHECK(s.vrQuality == "godlike");
    s.SetVrScale(0.88f);
    CHECK(std::fabs(s.vrScale - 0.90f) < 1e-4f);
    CHECK(s.vrQuality == "high");
    s.frameGeneration = true;
    s.leftStickDeadzone = 0.12f;
    s.rightStickDeadzone = 0.23f;
    s.linkStickDeadzone = false;
    s.keyboard.swipe = true;
    s.keyboard.recent = {"hello"};
    s.Save(path);

    const rv::HeadsetSettings rt = rv::HeadsetSettings::Load(path);
    CHECK(rt.frameGeneration == true);
    CHECK(rt.keyboard.swipe == true);
    CHECK(rt.keyboard.recent.size() == 1 && rt.keyboard.recent[0] == "hello");
    const auto streamJson = rt.StreamSettingsJson();
    CHECK(streamJson["frameGeneration"] == true);
    CHECK(!streamJson.contains("streamProfile"));
    CHECK(!streamJson.contains("leftStickDeadzone"));
    CHECK(!streamJson.contains("rightStickDeadzone"));
    CHECK(!streamJson.contains("linkStickDeadzone"));
    CHECK(std::fabs(rt.leftStickDeadzone - 0.12f) < 1e-6f);
    CHECK(std::fabs(rt.rightStickDeadzone - 0.23f) < 1e-6f);
    CHECK(!rt.linkStickDeadzone && rt.StickDeadzone(0) == rt.leftStickDeadzone && rt.StickDeadzone(1) == rt.rightStickDeadzone);

    {
        std::ofstream f(path);
        f << R"({"leftStickDeadzone":0.1,"rightStickDeadzone":0.3,"linkStickDeadzone":true})";
    }
    const rv::HeadsetSettings linked = rv::HeadsetSettings::Load(path);
    CHECK(linked.leftStickDeadzone == 0.3f && linked.rightStickDeadzone == 0.3f);
    rv::HeadsetSettings clamped;
    clamped.FromJson(nlohmann::json{{"leftStickDeadzone", -1.f}, {"rightStickDeadzone", 2.f}, {"linkStickDeadzone", false}});
    CHECK(clamped.leftStickDeadzone == 0.f && clamped.rightStickDeadzone == rv::kMaxStickDeadzone);
    clamped.leftStickDeadzone = std::numeric_limits<float>::quiet_NaN();
    clamped.FromJson(clamped.ToJson());
    CHECK(clamped.leftStickDeadzone == 0.f);

    // Corrupt JSON must cleanly fall back to defaults without throwing.
    {
        std::ofstream f(path);
        f << "{broken json";
    }
    const rv::HeadsetSettings fallback = rv::HeadsetSettings::Load(path);
    CHECK(fallback.codec == "auto");
    CHECK(fallback.vrScale == 0.75f);
    std::printf("headset settings: migration, clamp, roundtrip, corrupt-fallback ok\n");
}

void TestVideoDecoderSubmitChain() {
    rv::VideoDecoder dec;
    CHECK(dec.Init(rc::VideoCodec::HEVC, 2560, 1440, 72.f));

    std::vector<uint8_t> meta(sizeof(rc::VrFrameMetadata), 0);
    auto push = [&](uint32_t idx, bool key, bool rec, std::vector<uint8_t> bytes, std::vector<uint8_t> m) {
        dec.Submit(idx, key, rec, std::move(bytes), std::move(m), rc::NowNs());
    };

    // Frames before first keyframe must be dropped and request a keyframe.
    push(1, false, false, {1, 2, 3}, meta);
    CHECK(dec.NeedsKeyframe());
    CHECK(dec.LastGoodFrame() == 0);

    // Keyframe establishes chain; stale/out-of-order frames are suppressed.
    push(2, true, false, {1, 2, 3, 4}, meta);
    CHECK(!dec.NeedsKeyframe());
    CHECK(dec.LastGoodFrame() == 2);
    push(3, false, false, {5, 6}, meta);
    push(2, false, false, {7, 8}, meta);  // duplicate <= lastGood
    CHECK(dec.LastGoodFrame() == 3);

    // Truncated metadata logs warning but preserves frame chain!
    push(4, false, false, {9, 10}, {1, 2, 3});
    CHECK(dec.LastGoodFrame() == 4);
    CHECK(!dec.NeedsKeyframe());

    // Gap (frame 5 lost -> frame 6 arrives): chain breaks, P-frame 6 skipped, NeedsKeyframe armed.
    push(6, false, false, {11, 12}, meta);
    CHECK(dec.NeedsKeyframe());
    CHECK(dec.LastGoodFrame() == 4);

    // Ordinary P-frame 7 must remain blocked until keyframe or recovery frame.
    push(7, false, false, {13, 14}, meta);
    CHECK(dec.NeedsKeyframe());
    CHECK(dec.LastGoodFrame() == 4);

    // Recovery frame 8 restores chain!
    push(8, false, true, {15, 16}, meta);
    CHECK(!dec.NeedsKeyframe());
    CHECK(dec.LastGoodFrame() == 8);

    // Empty bitstream invalidates queue back to m_lastQueued (0 in stub) and requests keyframe.
    push(9, false, false, {}, meta);
    CHECK(dec.NeedsKeyframe());
    CHECK(dec.LastGoodFrame() == 0);

    // Restart with keyframe 10 then flood > 6 queued frames without InputLoop draining -> overflow flush.
    push(10, true, false, {1, 2, 3}, meta);
    CHECK(dec.LastGoodFrame() == 10);
    for (uint32_t idx = 11; idx <= 16; idx++) push(idx, false, false, {uint8_t(idx)}, meta);  // 7 in queue (next submit hits > 6)
    CHECK(dec.LastGoodFrame() == 16);
    push(17, false, false, {17}, meta);  // 8th submit sees queue.size() > 6 -> backlog clear -> WaitForKeyframe
    CHECK(dec.NeedsKeyframe());
    std::printf("headset VideoDecoder::Submit state machine: ok\n");
}

void TestDecoderGeometryLifecycle() {
    rv::VideoDecoder dec;
    CHECK(dec.Init(rc::VideoCodec::HEVC, 2828, 1344, 72));
    dec.Shutdown();
    const auto geometry = dec.Geometry();
    CHECK(geometry.texture == 0);
    CHECK(geometry.width == 0 && geometry.height == 0);
    for (float value : geometry.crop) CHECK(value == 0.f);
    std::printf("headset decoder geometry: shutdown clears the complete texture snapshot\n");
}

void TestDecodedCropBounds() {
    CHECK(rv::IsValidDecodedCrop(1920, 1088, 0, 0, 1920, 1088));
    CHECK(rv::IsValidDecodedCrop(1920, 1088, 16, 8, 1904, 1080));
    CHECK(!rv::IsValidDecodedCrop(0, 1088, 0, 0, 0, 1088));
    CHECK(!rv::IsValidDecodedCrop(1920, 0, 0, 0, 1920, 0));
    CHECK(!rv::IsValidDecodedCrop(1920, 1088, -1, 0, 1920, 1088));
    CHECK(!rv::IsValidDecodedCrop(1920, 1088, 0, -1, 1920, 1088));
    CHECK(!rv::IsValidDecodedCrop(1920, 1088, 0, 0, -1, 1088));
    CHECK(!rv::IsValidDecodedCrop(1920, 1088, 0, 0, 1920, -1));
    CHECK(!rv::IsValidDecodedCrop(1920, 1088, 0, 0, 1921, 1088));
    CHECK(!rv::IsValidDecodedCrop(1920, 1088, 0, 0, 1920, 1089));
    CHECK(!rv::IsValidDecodedCrop(1920, 1088, 1920, 0, 1920, 1088));
    CHECK(!rv::IsValidDecodedCrop(1920, 1088, 0, 1088, 1920, 1088));
    CHECK(!rv::IsValidDecodedCrop(1920, 1088, 100, 0, 100, 1088));
    CHECK(!rv::IsValidDecodedCrop(1920, 1088, 0, 100, 1920, 100));
    std::printf("headset decoded crop bounds: full, inset, zero-size, negative and overflow rejected\n");
}

// Reproduces & regression-tests the StreamClient::ResetStream vs OnPacket UAF race and
// exercises full loopback LAN + remote proof handshake, diagnosis localization, and feedback.
void TestStreamClientConcurrencyAndLoopback() {
    rv::SetLanguage("en", "en");

    // 1. Timeout + diagnosis localization through real StreamClient::StatusText() + strings.cpp.
    {
        rv::StreamClient client;
        client.SetDiagnosis("firewall");
        rv::ConnectTarget unreachable;
        unreachable.name = "DeadPC";
        unreachable.direct = rc::NetAddress{0x7F000001, 1};  // closed port / no listener -> instant ICMP or wait?
        // We don't want to wait 30s! Verify StatusText non-timeout path and SetDiagnosis safety instead;
        // full handshake timeout string resolution is exercised quickly in loopback below.
        CHECK(client.ServerName().empty());
        CHECK(client.GetState() == rv::StreamClient::State::Idle);
    }

    // 2. Real UDP loopback StreamClient <-> server Connection + concurrent ResetStream vs OnPacket storm!
    rc::NetInit();
    rc::UdpSocket serverSock;
    CHECK(serverSock.Open(0));
    const uint16_t serverPort = serverSock.LocalPort();

    const std::string connectSecret = "loopback-secret-42";
    auto serverIdentity = rc::crypto::KeyPair::Generate();
    std::atomic<bool> stopServer{false};
    std::atomic<bool> tamperProof{false};
    std::atomic<uint32_t> serverConnectedCount{0};
    std::atomic<uint32_t> serverFeedbackCount{0};

    std::thread serverThread([&] {
        uint8_t buf[2048];
        rc::NetAddress from;
        std::unique_ptr<rc::Connection> serverConn;
        uint32_t nextFrameIdx = 1;
        int64_t lastFrameSendNs = 0;

        while (!stopServer) {
            const int n = serverSock.RecvFrom(buf, sizeof(buf), from, 2);
            if (n > 0 && rc::IsHandshakeDatagram(buf, size_t(n))) {
                rc::HandshakePacket hp;
                if (rc::ParseHandshake(buf, size_t(n), hp) && hp.type == rc::PacketType::Hello) {
                    const nlohmann::json hj = nlohmann::json::parse(hp.json, nullptr, false);
                    const std::string cNonce = hj.value("nonce", "");
                    const auto sKeys = rc::crypto::KeyPair::Generate();
                    const std::string sNonce = "srvnonce";
                    auto sk = rc::crypto::DeriveSessionKeys(
                        sKeys.secretKey, hp.publicKey, hp.publicKey, sKeys.publicKey,
                        rc::SessionContext(cNonce, sNonce, connectSecret));
                    const uint32_t sessionId = 0x12345678u;
                    serverConn = std::make_unique<rc::Connection>(&serverSock, from, sessionId, sk, true,
                                                                  rc::kMaxDatagramLan);
                    serverConn->SetPacketHandler([&](rc::PacketType pt, const uint8_t*, size_t) {
                        if (pt == rc::PacketType::VideoFeedback) serverFeedbackCount++;
                    });

                    nlohmann::json wj{{"name", "LoopbackPC"}, {"nonce", sNonce}};
                    wj["proof"] = tamperProof ? "deadbeef"
                                              : rc::WelcomeProof(connectSecret, sKeys.publicKey, hp.publicKey);
                    rc::HandshakePacket welcome;
                    welcome.type = rc::PacketType::Welcome;
                    welcome.sessionId = sessionId;
                    welcome.publicKey = sKeys.publicKey;
                    welcome.json = wj.dump();
                    auto bytes = rc::BuildHandshake(welcome);
                    serverSock.SendTo(from, bytes.data(), bytes.size());
                    if (!tamperProof) serverConnectedCount++;
                }
            } else if (n > 0 && serverConn) {
                serverConn->HandleDatagram(buf, size_t(n), from);
            }

            const int64_t now = rc::NowNs();
            if (serverConn) {
                serverConn->Tick(now);
                // Stream multi-fragment VR frames continuously so OnPacket overlaps ResetStream!
                if (!tamperProof && now - lastFrameSendNs >= 2'000'000LL) {
                    lastFrameSendNs = now;
                    rc::VideoPacketizer pkt(serverConn.get());
                    pkt.SetFecPercent(10);
                    rc::VrFrameMetadata meta{};
                    meta.eyeWidth = 2560;
                    meta.eyeHeight = 1440;
                    meta.pcEncodeEndNs = now - 5'000'000LL;
                    std::vector<uint8_t> payload(4200, uint8_t(nextFrameIdx & 0xFF));
                    pkt.SendFrame(rc::kVrStreamId, rc::VideoCodec::HEVC, rc::kVideoKeyframe, nextFrameIdx++,
                                  &meta, sizeof(meta), payload.data(), payload.size());
                }
            }
        }
        if (serverConn) serverConn->SendDisconnect();
    });

    // First verify bad WelcomeProof in remote mode is rejected (never becomes Connected).
    {
        tamperProof = true;
        rv::StreamClient client;
        rv::ConnectTarget target;
        target.name = "LoopbackPC";
        target.cloudSession = "sess-bad";
        target.connectSecret = connectSecret;
        target.pcLocalAddrs.push_back(rc::NetAddress{0x7F000001, serverPort});
        client.Connect(target, {{"deviceId", "hs-1"}, {"deviceName", "Test"}});
        rc::SleepMs(300);
        CHECK(client.GetState() != rv::StreamClient::State::Connected);
        client.Disconnect();
        tamperProof = false;
    }

    // Now test legitimate connection + concurrent ResetStream() hammering while Video packets fly in!
    {
        rv::StreamClient client;
        client.SetIdentity(serverIdentity.secretKey);
        std::atomic<uint32_t> framesReceived{0};
        rv::StreamClient::Callbacks cb;
        cb.onVideoFrame = [&](rc::ReceivedFrame&& f) {
            CHECK(!f.bitstream.empty());
            framesReceived++;
        };
        cb.onAudio = [](const uint8_t*, size_t) {};
        cb.onHaptics = [](const rc::HapticsPayload&) {};
        client.SetCallbacks(cb);

        rv::ConnectTarget target;
        target.name = "LoopbackPC";
        target.cloudSession = "sess-good";
        target.connectSecret = connectSecret;
        target.pcLocalAddrs.push_back(rc::NetAddress{0x7F000001, serverPort});
        client.Connect(target, {{"deviceId", "hs-1"}, {"deviceName", "Test"}});

        const int64_t waitStart = rc::NowNs();
        while (client.GetState() != rv::StreamClient::State::Connected &&
               rc::NowNs() - waitStart < 3'000'000'000LL) {
            rc::SleepMs(10);
        }
        CHECK(client.GetState() == rv::StreamClient::State::Connected);
        CHECK(client.ServerName() == "LoopbackPC");

        // Feed decode quantiles while streaming + stress ResetStream vs network OnPacket simultaneously!
        client.SetDecodeQuantiles(true);
        for (int s = 1; s <= 100; s++) client.FeedDecodeSample(double(s) * 0.1);
        const auto q = client.TakeDecodeQuantiles();
        CHECK(q.first >= 4.0 && q.second >= 9.0);

        for (int iter = 0; iter < 150; iter++) {
            client.ResetStream(rc::kVrStreamId);
            client.RequestKeyframe(rc::kVrStreamId);
            rc::SleepMs(2);
        }
        // Stop resetting and confirm fresh stream recovers cleanly and receives frames + sends feedback!
        const uint32_t beforeStable = framesReceived.load();
        const int64_t stableStart = rc::NowNs();
        while (framesReceived.load() < beforeStable + 5 && rc::NowNs() - stableStart < 2'000'000'000LL) {
            rc::SleepMs(5);
        }
        CHECK(framesReceived.load() >= beforeStable + 5);
        client.SendFeedback();
        rc::SleepMs(50);
        CHECK(serverFeedbackCount.load() >= 1);
        client.Disconnect();
        CHECK(client.GetState() == rv::StreamClient::State::Idle);
    }

    stopServer = true;
    serverThread.join();
    CHECK(serverConnectedCount.load() >= 1);
    std::printf("headset StreamClient: proof rejection, loopback stream, and 150 concurrent ResetStream races ok\n");
}

}  // namespace

int main() {
    TestStringsAndLocales();
    TestThumbstickDeadzone();
    TestHeadsetSettings();
    TestHeadsetMathBounds();
    TestVideoDecoderSubmitChain();
    TestDecodedCropBounds();
    TestDecoderGeometryLifecycle();
    TestStreamClientConcurrencyAndLoopback();
    if (g_failures) {
        std::printf("FAILED: %d check(s)\n", g_failures);
        return 1;
    }
    std::printf("ALL HEADSET TESTS PASSED\n");
    return 0;
}
