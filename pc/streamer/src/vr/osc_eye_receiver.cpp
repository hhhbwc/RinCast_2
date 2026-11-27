#include "vr/osc_eye_receiver.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "rc/platform.h"
#include "rc/socket.h"

namespace rcs {

namespace {

constexpr int kMaxBundleDepth = 4;
constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
constexpr float kBlinkHoldThreshold = 0.85f;
constexpr int64_t kActiveTimeoutNs = 500'000'000LL;
constexpr int64_t kSubscribeRenewNs = 1'000'000'000LL;

// PaperTracker gaze export v1: argument-free OSC messages sent to 127.0.0.1:<port>
constexpr uint8_t kSubscribeMsg[36] = {
    '/', 'p', 'a', 'p', 'e', 'r', 't', 'r', 'a', 'c', 'k', 'e', 'r', '/', 'g', 'a', 'z', 'e',
    '/', 'v', '1', '/', 's', 'u', 'b', 's', 'c', 'r', 'i', 'b', 'e', '\0', ',', '\0', '\0', '\0'};
constexpr uint8_t kUnsubscribeMsg[40] = {
    '/', 'p', 'a', 'p', 'e', 'r', 't', 'r', 'a', 'c', 'k', 'e', 'r', '/', 'g', 'a', 'z', 'e',
    '/', 'v', '1', '/', 'u', 'n', 's', 'u', 'b', 's', 'c', 'r', 'i', 'b', 'e', '\0', '\0', '\0',
    ',', '\0', '\0', '\0'};

uint32_t ReadBe32(const uint8_t* p) {
    uint32_t v = 0;
    std::memcpy(&v, p, 4);
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) | ((v & 0x00FF0000u) >> 8) |
           ((v & 0xFF000000u) >> 24);
}

uint64_t ReadBe64(const uint8_t* p) {
    const uint64_t hi = ReadBe32(p);
    const uint64_t lo = ReadBe32(p + 4);
    return (hi << 32) | lo;
}

float ReadBeFloat(const uint8_t* p) {
    const uint32_t bits = ReadBe32(p);
    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

double ReadBeDouble(const uint8_t* p) {
    const uint64_t bits = ReadBe64(p);
    double d = 0.0;
    std::memcpy(&d, &bits, sizeof(d));
    return d;
}

bool ReadOscString(const uint8_t* data, size_t len, size_t& offset, std::string& out) {
    if (offset >= len) return false;
    const size_t start = offset;
    size_t nul = start;
    while (nul < len && data[nul] != '\0') ++nul;
    if (nul >= len) return false;
    out.assign(reinterpret_cast<const char*>(data + start), nul - start);
    const size_t withNul = (nul - start) + 1;
    const size_t padded = (withNul + 3u) & ~size_t(3u);
    if (start + padded > len) return false;
    offset = start + padded;
    return true;
}

bool PitchYawToVec(float pitchDeg, float yawDeg, bool invertX, bool invertY, float out[3]) {
    if (!std::isfinite(pitchDeg) || !std::isfinite(yawDeg)) return false;
    yawDeg = std::clamp(yawDeg, -55.0f, 55.0f);
    pitchDeg = std::clamp(pitchDeg, -45.0f, 45.0f);
    if (invertX) yawDeg = -yawDeg;
    if (invertY) pitchDeg = -pitchDeg;
    const float yr = yawDeg * kDegToRad;
    const float pr = pitchDeg * kDegToRad;
    const float x = std::cos(pr) * std::sin(yr);
    const float y = -std::sin(pr);
    const float z = -std::cos(pr) * std::cos(yr);
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || z >= -0.2f) return false;
    out[0] = x;
    out[1] = y;
    out[2] = z;
    return true;
}

bool NormalizeGazeVec(float x, float y, float z, bool invertX, bool invertY, float out[3]) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;
    const float len = std::sqrt(x * x + y * y + z * z);
    if (!(len > 1e-4f)) return false;
    x /= len;
    y /= len;
    z /= len;
    if (z > 0.2f) z = -std::fabs(z);
    if (invertX) x = -x;
    if (invertY) y = -y;
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || z >= -0.2f) return false;
    out[0] = x;
    out[1] = y;
    out[2] = z;
    return true;
}

// PaperTracker gaze export v1: tangent-plane angles (tan(yaw), tan(pitch), -1), yaw right, pitch up.
bool TangentAnglesToVec(float yawDeg, float pitchDeg, bool invertX, bool invertY, float out[3]) {
    if (!std::isfinite(yawDeg) || !std::isfinite(pitchDeg) || std::fabs(yawDeg) >= 89.0f ||
        std::fabs(pitchDeg) >= 89.0f) {
        return false;
    }
    const float yd = std::clamp(invertX ? -yawDeg : yawDeg, -55.0f, 55.0f) * kDegToRad;
    const float pd = std::clamp(invertY ? -pitchDeg : pitchDeg, -45.0f, 45.0f) * kDegToRad;
    return NormalizeGazeVec(std::tan(yd), std::tan(pd), -1.0f, false, false, out);
}

void UpdateNormFromGaze(const rc::GazePayload& g, float& outNormX, float& outNormY) {
    float sx = 0.0f, sy = 0.0f;
    int n = 0;
    if (g.flags & rc::kGazeLeftValid) {
        sx += g.gaze[0][0];
        sy += g.gaze[0][1];
        ++n;
    }
    if (g.flags & rc::kGazeRightValid) {
        sx += g.gaze[1][0];
        sy += g.gaze[1][1];
        ++n;
    }
    if (n > 0) {
        const float avgX = sx / float(n);
        const float avgY = sy / float(n);
        outNormX = std::clamp(avgX / 0.70710678f, -1.0f, 1.0f);
        outNormY = std::clamp(avgY / 0.57357644f, -1.0f, 1.0f);
    }
}

bool HandleOscMessage(const uint8_t* data, size_t len, bool invertX, bool invertY,
                      OscEyeParserState& state, rc::GazePayload& outGaze,
                      std::string& outLastAddress, float& outNormX, float& outNormY,
                      bool& outUpdatedGaze) {
    size_t offset = 0;
    std::string address;
    if (!ReadOscString(data, len, offset, address)) return false;
    if (address.empty() || address[0] != '/') return false;

    std::string typeTag;
    if (!ReadOscString(data, len, offset, typeTag)) return false;
    if (typeTag.empty() || typeTag[0] != ',') return false;

    float args[10] = {};
    int argCount = 0;
    for (size_t i = 1; i < typeTag.size(); ++i) {
        const char tag = typeTag[i];
        if (tag == 'f') {
            if (offset + 4 > len) return false;
            if (argCount < 10) args[argCount++] = ReadBeFloat(data + offset);
            offset += 4;
        } else if (tag == 'd') {
            if (offset + 8 > len) return false;
            if (argCount < 10) args[argCount++] = static_cast<float>(ReadBeDouble(data + offset));
            offset += 8;
        } else if (tag == 'i') {
            if (offset + 4 > len) return false;
            const int32_t iv = static_cast<int32_t>(ReadBe32(data + offset));
            if (argCount < 10) args[argCount++] = static_cast<float>(iv);
            offset += 4;
        } else if (tag == 'T') {
            if (argCount < 10) args[argCount++] = 1.0f;
        } else if (tag == 'F') {
            if (argCount < 10) args[argCount++] = 0.0f;
        } else {
            // Unsupported type tag: safely ignore this message.
            return false;
        }
    }

    outLastAddress = address;

    const bool eyesBlinking = state.eyesClosed > kBlinkHoldThreshold ||
                              (state.closedLeft > kBlinkHoldThreshold && state.closedRight > kBlinkHoldThreshold);

    auto emitGaze = [&](const rc::GazePayload& g) -> bool {
        if (eyesBlinking && state.hasValidGaze) {
            outGaze = state.lastValidGaze;
            UpdateNormFromGaze(outGaze, outNormX, outNormY);
            outUpdatedGaze = true;
            return true;
        }
        state.lastValidGaze = g;
        state.hasValidGaze = true;
        outGaze = g;
        UpdateNormFromGaze(g, outNormX, outNormY);
        outUpdatedGaze = true;
        return true;
    };

    // PaperTracker native gaze export v1: /papertracker/gaze/v1 ,iiiffffff
    if (address == "/papertracker/gaze/v1" && typeTag == ",iiiffffff" && len == 72) {
        const uint32_t sequence = ReadBe32(data + 36);
        // Zero is also used by exporters that do not provide frame numbering.
        if (sequence != 0) {
            if (state.hasPaperTrackerSequence && sequence == state.lastPaperTrackerSequence) return true;
            state.lastPaperTrackerSequence = sequence;
            state.hasPaperTrackerSequence = true;
        } else {
            state.hasPaperTrackerSequence = false;
        }
        const uint32_t ptFlags = ReadBe32(data + 36 + 4);
        const bool leftValid = (ptFlags & 1u) != 0;
        const bool rightValid = (ptFlags & 2u) != 0;
        const bool fusedValid = (ptFlags & 4u) != 0;
        rc::GazePayload g{};
        if (leftValid && TangentAnglesToVec(args[3], args[4], invertX, invertY, g.gaze[0])) {
            g.flags |= rc::kGazeLeftValid;
        } else if (fusedValid && TangentAnglesToVec(args[7], args[8], invertX, invertY, g.gaze[0])) {
            g.flags |= rc::kGazeLeftValid;
        }
        if (rightValid && TangentAnglesToVec(args[5], args[6], invertX, invertY, g.gaze[1])) {
            g.flags |= rc::kGazeRightValid;
        } else if (fusedValid && TangentAnglesToVec(args[7], args[8], invertX, invertY, g.gaze[1])) {
            g.flags |= rc::kGazeRightValid;
        }
        if (g.flags == 0) return false;
        return emitGaze(g);
    }

    // Blink protection updates
    if (address == "/tracking/eye/EyesClosedAmount" && argCount >= 1) {
        if (std::isfinite(args[0])) state.eyesClosed = std::clamp(args[0], 0.0f, 1.0f);
        return true;
    }
    if (address == "/avatar/parameters/FT/v2/EyeClosedLeft" && argCount >= 1) {
        if (std::isfinite(args[0])) state.closedLeft = std::clamp(args[0], 0.0f, 1.0f);
        return true;
    }
    if (address == "/avatar/parameters/FT/v2/EyeClosedRight" && argCount >= 1) {
        if (std::isfinite(args[0])) state.closedRight = std::clamp(args[0], 0.0f, 1.0f);
        return true;
    }

    if (address == "/tracking/eye/LeftRightVec" && argCount >= 6) {
        rc::GazePayload g{};
        if (!NormalizeGazeVec(args[0], args[1], args[2], invertX, invertY, g.gaze[0])) return false;
        if (!NormalizeGazeVec(args[3], args[4], args[5], invertX, invertY, g.gaze[1])) return false;
        g.flags = rc::kGazeLeftValid | rc::kGazeRightValid;
        return emitGaze(g);
    }

    if ((address == "/tracking/eye/CenterVec" || address == "/tracking/eye/CenterVecFull") && argCount >= 3) {
        rc::GazePayload g{};
        if (!NormalizeGazeVec(args[0], args[1], args[2], invertX, invertY, g.gaze[0])) return false;
        std::memcpy(g.gaze[1], g.gaze[0], sizeof(g.gaze[0]));
        g.flags = rc::kGazeLeftValid | rc::kGazeRightValid;
        return emitGaze(g);
    }

    if (address == "/tracking/eye/LeftRightPitchYaw" && argCount >= 4) {
        rc::GazePayload g{};
        if (!PitchYawToVec(args[0], args[1], invertX, invertY, g.gaze[0])) return false;
        if (!PitchYawToVec(args[2], args[3], invertX, invertY, g.gaze[1])) return false;
        g.flags = rc::kGazeLeftValid | rc::kGazeRightValid;
        return emitGaze(g);
    }

    if (address == "/tracking/eye/CenterPitchYaw" && argCount >= 2) {
        rc::GazePayload g{};
        if (!PitchYawToVec(args[0], args[1], invertX, invertY, g.gaze[0])) return false;
        std::memcpy(g.gaze[1], g.gaze[0], sizeof(g.gaze[0]));
        g.flags = rc::kGazeLeftValid | rc::kGazeRightValid;
        return emitGaze(g);
    }

    bool updatedParam = false;
    if (address == "/avatar/parameters/FT/v2/EyeLeftX" && argCount >= 1 && std::isfinite(args[0])) {
        state.leftX = std::clamp(args[0], -1.0f, 1.0f);
        state.hasLeft = true;
        updatedParam = true;
    } else if (address == "/avatar/parameters/FT/v2/EyeLeftY" && argCount >= 1 && std::isfinite(args[0])) {
        state.leftY = std::clamp(args[0], -1.0f, 1.0f);
        state.hasLeft = true;
        updatedParam = true;
    } else if (address == "/avatar/parameters/FT/v2/EyeRightX" && argCount >= 1 && std::isfinite(args[0])) {
        state.rightX = std::clamp(args[0], -1.0f, 1.0f);
        state.hasRight = true;
        updatedParam = true;
    } else if (address == "/avatar/parameters/FT/v2/EyeRightY" && argCount >= 1 && std::isfinite(args[0])) {
        state.rightY = std::clamp(args[0], -1.0f, 1.0f);
        state.hasRight = true;
        updatedParam = true;
    } else if (address == "/avatar/parameters/EyeX" && argCount >= 1 && std::isfinite(args[0])) {
        state.leftX = state.rightX = std::clamp(args[0], -1.0f, 1.0f);
        state.hasLeft = state.hasRight = true;
        updatedParam = true;
    } else if (address == "/avatar/parameters/EyeY" && argCount >= 1 && std::isfinite(args[0])) {
        state.leftY = state.rightY = std::clamp(args[0], -1.0f, 1.0f);
        state.hasLeft = state.hasRight = true;
        updatedParam = true;
    }

    if (updatedParam) {
        rc::GazePayload g{};
        const float lx = state.hasLeft ? state.leftX : state.rightX;
        const float ly = state.hasLeft ? state.leftY : state.rightY;
        const float rx = state.hasRight ? state.rightX : state.leftX;
        const float ry = state.hasRight ? state.rightY : state.leftY;
        // Normalized [-1, 1] (+Y up) -> VRChat pitch/yaw (+pitch down), so negate Y for pitch.
        if (!PitchYawToVec(-ly * 35.0f, lx * 45.0f, invertX, invertY, g.gaze[0])) return false;
        if (!PitchYawToVec(-ry * 35.0f, rx * 45.0f, invertX, invertY, g.gaze[1])) return false;
        g.flags = rc::kGazeLeftValid | rc::kGazeRightValid;
        return emitGaze(g);
    }

    return false;
}

bool ParsePacketDepth(const uint8_t* data, size_t len, int depth, bool invertX, bool invertY,
                      OscEyeParserState& state, rc::GazePayload& outGaze,
                      std::string& outLastAddress, float& outNormX, float& outNormY,
                      bool& outUpdatedGaze) {
    if (!data || len == 0 || depth > kMaxBundleDepth) return false;
    static constexpr char kBundleTag[8] = {'#', 'b', 'u', 'n', 'd', 'l', 'e', '\0'};
    if (len >= 16 && std::memcmp(data, kBundleTag, 8) == 0) {
        size_t offset = 16;  // skip "#bundle\0" + 8-byte timetag
        bool anyOk = false;
        while (offset + 4 <= len) {
            const uint32_t elemSize = ReadBe32(data + offset);
            offset += 4;
            if (elemSize == 0 || offset + elemSize > len) break;
            if (ParsePacketDepth(data + offset, elemSize, depth + 1, invertX, invertY, state,
                                 outGaze, outLastAddress, outNormX, outNormY, outUpdatedGaze)) {
                anyOk = true;
            }
            offset += elemSize;
        }
        return anyOk;
    }
    return HandleOscMessage(data, len, invertX, invertY, state, outGaze, outLastAddress,
                            outNormX, outNormY, outUpdatedGaze);
}

}  // namespace

OscEyeReceiver::~OscEyeReceiver() { Stop(); }

void OscEyeReceiver::SetCallback(GazeCallback cb) {
    std::lock_guard lock(m_cbMutex);
    m_callback = std::move(cb);
}

void OscEyeReceiver::Configure(bool enabled, uint16_t port, bool invertX, bool invertY) {
    std::lock_guard lifecycleLock(m_lifecycleMutex);
    m_invertX.store(invertX, std::memory_order_relaxed);
    m_invertY.store(invertY, std::memory_order_relaxed);
    if (enabled == m_enabled && (!enabled || port == m_port)) return;

    StopLocked();
    m_enabled = enabled;
    m_port = port;
    {
        std::lock_guard lock(m_statusMutex);
        m_status = {};
        m_status.enabled = enabled;
        m_status.port = port;
        m_lastValidNs = 0;
    }
    if (!enabled) return;

    m_running.store(true, std::memory_order_release);
    m_thread = std::thread([this, port] { WorkerLoop(port); });
}

void OscEyeReceiver::Stop() {
    std::lock_guard lifecycleLock(m_lifecycleMutex);
    StopLocked();
}

void OscEyeReceiver::StopLocked() {
    m_running.store(false, std::memory_order_release);
    if (m_thread.joinable()) m_thread.join();
    m_enabled = false;
    std::lock_guard lock(m_statusMutex);
    m_status.enabled = false;
    m_status.listening = false;
    m_status.active = false;
    m_status.rateHz = 0.0f;
}

OscEyeStatus OscEyeReceiver::Status() const {
    std::lock_guard lock(m_statusMutex);
    OscEyeStatus s = m_status;
    if (s.active && (rc::NowNs() - m_lastValidNs) > kActiveTimeoutNs) {
        s.active = false;
        s.rateHz = 0.0f;
    }
    return s;
}

bool OscEyeReceiver::ParsePacket(const uint8_t* data, size_t len, bool invertX, bool invertY,
                                 OscEyeParserState& state, rc::GazePayload& outGaze,
                                 std::string& outLastAddress, float& outNormX, float& outNormY,
                                 bool& outUpdatedGaze) {
    outUpdatedGaze = false;
    return ParsePacketDepth(data, len, 0, invertX, invertY, state, outGaze, outLastAddress,
                            outNormX, outNormY, outUpdatedGaze);
}

void OscEyeReceiver::WorkerLoop(uint16_t port) {
    rc::NetInit();
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        const int err = WSAGetLastError();
        RC_LOGW("OSC eye receiver: socket() failed (%d)", err);
        std::lock_guard lock(m_statusMutex);
        m_status.listening = false;
        m_status.bindError = err;
        return;
    }

    DWORD timeoutMs = 200;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));

    // Bind an ephemeral loopback port so we can subscribe to PaperTracker (which owns 127.0.0.1:port).
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    local.sin_port = 0;
    if (bind(s, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR) {
        const int err = WSAGetLastError();
        RC_LOGW("OSC eye receiver: bind loopback failed (%d)", err);
        closesocket(s);
        std::lock_guard lock(m_statusMutex);
        m_status.listening = false;
        m_status.bindError = err;
        return;
    }

    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    target.sin_port = htons(port);

    {
        std::lock_guard lock(m_statusMutex);
        m_status.listening = true;
        m_status.bindError = 0;
    }
    RC_LOGI("OSC eye receiver subscribing to PaperTracker at 127.0.0.1:%u", unsigned(port));

    OscEyeParserState parserState;
    uint8_t buf[4096];
    int64_t rateWindowStartNs = rc::NowNs();
    int64_t nextSubscribeNs = 0;
    uint32_t ratePackets = 0;
    float currentRateHz = 0.0f;

    while (m_running.load(std::memory_order_acquire)) {
        const int64_t nowBefore = rc::NowNs();
        if (nowBefore >= nextSubscribeNs) {
            sendto(s, reinterpret_cast<const char*>(kSubscribeMsg), sizeof(kSubscribeMsg), 0,
                   reinterpret_cast<const sockaddr*>(&target), sizeof(target));
            nextSubscribeNs = nowBefore + kSubscribeRenewNs;
        }

        const int n = recvfrom(s, reinterpret_cast<char*>(buf), sizeof(buf), 0, nullptr, nullptr);
        const int64_t nowNs = rc::NowNs();
        if (nowNs - rateWindowStartNs >= 1'000'000'000LL) {
            const double sec = double(nowNs - rateWindowStartNs) / 1e9;
            currentRateHz = sec > 0.0 ? float(double(ratePackets) / sec) : 0.0f;
            ratePackets = 0;
            rateWindowStartNs = nowNs;
            std::lock_guard lock(m_statusMutex);
            m_status.rateHz = currentRateHz;
        }
        if (n == SOCKET_ERROR) {
            const int err = WSAGetLastError();
            if (err == WSAECONNRESET) {
                std::lock_guard lock(m_statusMutex);
                m_status.active = false;
                m_status.rateHz = 0.0f;
            }
            continue;
        }
        if (n <= 0) continue;

        rc::GazePayload gaze{};
        std::string lastAddr;
        float normX = 0.0f, normY = 0.0f;
        bool updatedGaze = false;
        const bool ok = ParsePacket(buf, size_t(n),
                                    m_invertX.load(std::memory_order_relaxed),
                                    m_invertY.load(std::memory_order_relaxed),
                                    parserState, gaze, lastAddr, normX, normY, updatedGaze);
        if (ok && updatedGaze) {
            ++ratePackets;
            GazeCallback cb;
            {
                std::lock_guard lock(m_cbMutex);
                cb = m_callback;
            }
            if (cb) cb(gaze);
            std::lock_guard lock(m_statusMutex);
            m_status.active = true;
            m_status.lastNormX = normX;
            m_status.lastNormY = normY;
            if (!lastAddr.empty()) m_status.lastAddress = lastAddr;
            m_lastValidNs = nowNs;
        } else if (!ok) {
            std::lock_guard lock(m_statusMutex);
            ++m_status.unrecognizedPackets;
            if (!lastAddr.empty()) m_status.lastAddress = lastAddr;
        }
    }

    // Unsubscribe cleanly from the sole owning thread right before closing the socket:
    // guarantees unsubscribe always follows the last subscribe and prevents WSAENOTSOCK.
    if (port != 0) {
        sendto(s, reinterpret_cast<const char*>(kUnsubscribeMsg), sizeof(kUnsubscribeMsg), 0,
               reinterpret_cast<const sockaddr*>(&target), sizeof(target));
    }
    closesocket(s);
    RC_LOGI("OSC eye receiver stopped");
}

}  // namespace rcs
