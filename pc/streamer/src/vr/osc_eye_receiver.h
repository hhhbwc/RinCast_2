// PC-side localhost UDP/OSC receiver for external eye tracking (e.g. PaperTracker / VRCFT).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "rc/protocol.h"

namespace rcs {

struct OscEyeStatus {
    bool enabled = false;
    bool listening = false;   // UDP socket bound to 127.0.0.1:port
    bool active = false;      // Valid eye-tracking packet received within last 500 ms
    uint16_t port = 9945;
    int bindError = 0;        // WSA error code (0 = ok)
    float rateHz = 0.0f;      // Valid packet rate over the last ~1 s
    float lastNormX = 0.0f;   // Estimated normalized gaze [-1, +1] (+right)
    float lastNormY = 0.0f;   // Estimated normalized gaze [-1, +1] (+up)
    std::string lastAddress;  // Most recent OSC address seen
    uint64_t unrecognizedPackets = 0;
};

struct OscEyeParserState {
    float leftX = 0.0f;
    float leftY = 0.0f;
    float rightX = 0.0f;
    float rightY = 0.0f;
    bool hasLeft = false;
    bool hasRight = false;
    float closedLeft = 0.0f;
    float closedRight = 0.0f;
    float eyesClosed = 0.0f;
    rc::GazePayload lastValidGaze{};
    bool hasValidGaze = false;
    uint32_t lastPaperTrackerSequence = 0;
    bool hasPaperTrackerSequence = false;
    bool isRepeatedGaze = false;
};

class OscEyeReceiver {
   public:
    using GazeCallback = std::function<void(const rc::GazePayload&)>;

    OscEyeReceiver() = default;
    ~OscEyeReceiver();

    OscEyeReceiver(const OscEyeReceiver&) = delete;
    OscEyeReceiver& operator=(const OscEyeReceiver&) = delete;

    void SetCallback(GazeCallback cb);
    void Configure(bool enabled, uint16_t port, bool invertX, bool invertY,
                   float offsetX = 0.0f, float offsetY = 0.0f, float gainX = 1.0f, float gainY = 1.0f);
    void Stop();

    OscEyeStatus Status() const;

    // Pure packet parser exposed for offline unit testing (no socket required).
    static bool ParsePacket(const uint8_t* data, size_t len, bool invertX, bool invertY,
                            OscEyeParserState& state, rc::GazePayload& outGaze,
                            std::string& outLastAddress, float& outNormX, float& outNormY,
                            bool& outUpdatedGaze,
                            float offsetX = 0.0f, float offsetY = 0.0f, float gainX = 1.0f, float gainY = 1.0f);

   private:
    void WorkerLoop(uint16_t port);
    void StopLocked();

    GazeCallback m_callback;
    std::mutex m_cbMutex;

    std::mutex m_lifecycleMutex;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_invertX{false};
    std::atomic<bool> m_invertY{false};
    std::atomic<float> m_offsetX{0.0f};
    std::atomic<float> m_offsetY{0.0f};
    std::atomic<float> m_gainX{1.0f};
    std::atomic<float> m_gainY{1.0f};
    bool m_enabled = false;
    uint16_t m_port = 9945;
    std::thread m_thread;

    mutable std::mutex m_statusMutex;
    OscEyeStatus m_status;
    int64_t m_lastValidNs = 0;
};

}  // namespace rcs
