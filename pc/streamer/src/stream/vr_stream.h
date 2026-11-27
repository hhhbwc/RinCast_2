// Encodes frames produced through the VR bridge (SteamVR or OpenXR) and
// sends them with their render pose so the headset can reproject.
#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "../encode/color_convert.h"
#include "../encode/encoder.h"
#include "rc/auto_tune.h"
#include "rc/motion_field.h"
#include "rc/platform.h"
#include "rc/protocol.h"
#include "rc/stale_flush.h"  // A72: the flush's chain state lives in the members below
#include "video_sender.h"

namespace rcs {

class VrBridgeHost;

struct VrStreamConfig {
    rc::VideoCodec codec = rc::VideoCodec::HEVC;
    bool tenBit = false;
    uint32_t eyeWidth = 0;
    uint32_t eyeHeight = 0;
    float fps = 72;  // ROUND18 L4 default (was 90)
    uint32_t bitrateKbps = 150000;
    int fecPercent = 5;
    bool adaptive = true;
    rc::StreamProfile profile = rc::StreamProfile::Balanced;  // what to favour (latency / balanced / quality)
    uint32_t startBitrateKbps = 0;  // adaptive: where to start (0 = at bitrateKbps)
    bool pace = false;              // legacy pacing switch (desktop stream); VR uses pacingMode
    int pacingMode = 2;             // 0 auto-resolved by the session, 1 off, 2 follow, 3 burst
    bool foveated = true;
    bool adaptiveQuant = true;
    bool twoPass = false;
    int sliceCount = 1;
    // false = plain CBR: spend the ceiling exactly, even on still pictures. The quality
    // profile's opt-in mode; every other path leaves the encoder on capped VBR.
    bool cappedVbr = true;
    // L17 fill mode (default off): pass through to the encoder; see EncoderConfig.
    bool fillMode = false;
    bool motionField = false;       // for frame generation on the headset
    // Foveated encoding around where the eyes look (see Foveator): the size each eye is
    // encoded at, 0 = off. eyeWidth x eyeHeight stays the size the headset shows.
    uint32_t foveatedEyeWidth = 0;
    uint32_t foveatedEyeHeight = 0;
    float foveaSize = 0.45f;  // the full-detail share of each eye (Foveator)
    // ROUND18 L2: encode resolution as a fraction of the render size. 1.0 = encode
    // exactly what the game renders. Applied to the final encode size (after any
    // foveation squeeze); the headset always stretches the result back to the panel.
    float encodeScale = 1.0f;
};

class VrStream {
   public:
    // state: "waiting" (armed, no frames) / "running"; producer: steamvr/openxr
    using StateHandler = std::function<void(const std::string& state, const std::string& producer, const std::string& app)>;

    VrStream(rc::Connection* connection, VrBridgeHost* bridge, const VrStreamConfig& config, StateHandler onState);
    ~VrStream();

    bool Start();
    void Stop();
    void RequestKeyframe() { m_sender.RequestKeyframe(); }
    // A87/A99: a picture-tier move updates the queue brake, encoder QP floor and
    // profile VBV without rebuilding the stream. The live profile is persistent so an
    // internal encoder rebuild cannot fall back to the profile the session started at.
    void ApplyProfileRung(int rung) {
        rung = rc::ClampProfileId(rung);
        m_liveProfile.store(rung, std::memory_order_relaxed);
        m_pendingProfile.store(rung, std::memory_order_relaxed);
    }
    // The headset left the VR view: nothing is encoded or sent, but the game's frames still
    // count for Running(). Resuming starts with a keyframe.
    void SetPaused(bool paused) {
        if (m_paused.exchange(paused) && !paused) m_sender.RequestKeyframe();
    }
    bool Paused() const { return m_paused; }
    void OnFeedback(const rc::VideoFeedbackPayload& fb) { m_sender.OnFeedback(fb); }
    // Where the eyes look, as the headset tracked it (foveated encoding).
    void SetGaze(const rc::GazePayload& gaze) {
        std::lock_guard lock(m_gazeMutex);
        m_gaze = gaze;
        if (gaze.flags != 0) {
            m_gazeNs = rc::NowNs();
        }
    }
    VideoSenderStats Stats() {
        VideoSenderStats s = m_sender.Stats();
        s.qp = m_statQp.load(std::memory_order_relaxed);
        s.encWidth = m_statEncW.load(std::memory_order_relaxed);
        s.encHeight = m_statEncH.load(std::memory_order_relaxed);
        s.srcWidth = m_statSrcW.load(std::memory_order_relaxed);
        s.srcHeight = m_statSrcH.load(std::memory_order_relaxed);
        s.gameFps = m_lastGameFps.load(std::memory_order_relaxed);
        s.encFps = m_lastEncFps.load(std::memory_order_relaxed);
        s.droppedFps = m_lastDroppedFps.load(std::memory_order_relaxed);
        return s;
    }
    const VrStreamConfig& Config() const { return m_config; }
    bool Running() const { return m_streaming; }

   private:
    struct DiagTiming {
        uint32_t samples = 0;
        int64_t sumNs = 0;
        int64_t maxNs = 0;
        void Add(int64_t ns) {
            samples++;
            sumNs += ns;
            if (ns > maxNs) maxNs = ns;
        }
    };

    // A converted frame waiting for the encoder. Conversion and encoding run on threads of
    // their own: one frame converts while the one before it encodes. Run in sequence, a
    // frame took both times, and every frame the game finished meanwhile was replaced.
    struct EncodeJob {
        ComPtr<ID3D11Texture2D> yuv;  // kept alive here: the converter may move on to a new size
        DXGI_FORMAT format = DXGI_FORMAT_NV12;
        uint32_t width = 0, height = 0;  // encoded size
        uint32_t srcWidth = 0, srcHeight = 0;
        bool halfRate = false;
        bool foveated = false;  // squeezed around the gaze (foveaStart)
        bool motionQueued = false;
        float foveaStart[2][2] = {};
        uint64_t frameIndex = 0;
        uint64_t headsetTargetNs = 0;
        int64_t submitPcNs = 0;
        // A83 phase M (reading only): this frame's own upstream ages, carried to the flush
        // line - the rc diag window averages hide single-shot events (A79 item 4 died on
        // that ranking math). submitToJobNs = driver submit (driver.cpp:846) -> m_job entry,
        // the segment before A79's jobAge begins; lockWaitNs = this frame's keyed-mutex
        // acquire. -1 = n/a (submitPcNs was 0 / no keyed mutex); signed, never abs()'d.
        int64_t submitToJobNs = -1;
        int64_t lockWaitNs = -1;
        int64_t holdNs = -1;
        int64_t convertBlockNs = -1;
        int64_t motionReadNs = -1;
        // A26 (stale-frame flush): this frame's predicted display moment in the PC
        // clock, recovered in Run() from the tracking sample the frame was rendered
        // with (0 = unknown: never expires - see rc/stale_flush.h).
        int64_t predictedDisplayPcNs = 0;
        // A75 (TASK-A75-FLUSH-GRACE-2026-11-22 §2.2): which of the two prediction paths
        // (vr_stream.cpp:581-582) produced that moment - the headset's exact target
        // mapped into the PC clock (true), or the submit + 40 ms predict-ahead estimate
        // (false). It is Run()'s already-computed `flushExact` (vr_stream.cpp:565),
        // carried, not re-read: the flush judges both with the same rule
        // (rc::kFlushGraceNs), and this says in the log which prediction the next
        // "0-8 ms late" burst came from.
        bool predictedDisplayExact = false;
        bool predictedDisplayClockValid = false;
        bool predictedDisplayTrackHit = false;
        float renderOrientation[4] = {}, renderPosition[3] = {};
        float fovTan[2][4] = {};
    };

    // An encoded frame waiting to be packetized and sent. On a thread of its own, so the
    // encoder starts on the next frame while this one goes out (paced, on internet paths).
    struct SendJob {
        EncodedFrame frame;
        std::vector<uint8_t> meta;
        uint8_t flags = 0;
        double encodeMs = 0;
        bool recovery = false;
        uint32_t frameIndex = 0;  // ROUND19 T1: the producer's frame counter, for the diag seq field
        int64_t predictedDisplayPcNs = 0;  // A26: carried from the EncodeJob for the flush gate
        bool predictedDisplayExact = false;  // A75 §2.2: carried too - names the source in the flush log line
        bool predictedDisplayClockValid = false;
        bool predictedDisplayTrackHit = false;
        int64_t submitToJobNs = -1;  // A83 phase M: carried for the flush line's per-frame upstream age
        int64_t lockWaitNs = -1;     // A83 phase M: carried for the flush line's per-frame lock wait
        int64_t holdNs = -1;
        int64_t convertBlockNs = -1;
        int64_t motionReadNs = -1;
        int64_t sendWaitNs = -1;
    };

    void Run();
    void RunEncoder();
    void RunSender();
    // Waits until the encoder is about to want the next frame (see Run).
    int64_t WaitForEncoder(int64_t convertAvgNs, const std::function<bool()>& pollMotion = {});
    // Wakes Run() to drain finished async motion readbacks for an encoded frame within a tight cap.
    void WaitForMotionField(uint64_t frameIndex, uint64_t headsetTargetNs);
    // Where frames went, every 10 s while streaming (see Run).
    void LogFrameFlow(int64_t now);
    // Where each eye's full-density square starts for this frame, from the gaze.
    void PlaceFovea(const float fovTan[2][4], float start[2][2]);

    rc::Connection* m_connection;
    VrBridgeHost* m_bridge;
    VrStreamConfig m_config;
    VideoSender m_sender;
    StateHandler m_onState;
    std::thread m_thread;
    std::thread m_encodeThread;
    std::thread m_sendThread;
    std::mutex m_sendMutex;
    std::condition_variable m_sendCv;  // a frame to send, or room for one
    std::optional<SendJob> m_send;     // one at most: frames are never dropped, the encoder waits
                                       // (A26: expired ones are dropped at the take-off - RunSender)
    std::mutex m_jobMutex;
    std::condition_variable m_jobReady;
    std::condition_variable m_jobTaken;
    std::optional<EncodeJob> m_job;  // the newest converted frame, not yet taken by the encoder
    // A79 phase M (stale-flush attribution, reading only): when the frame now sitting in
    // m_job / m_send entered its slot (NowNs; 0 = slot empty). Each is stored at the
    // slot's existing write point, under the lock that already guards it (Run stores
    // m_jobSinceNs, RunEncoder clears it when it takes the job; RunEncoder stores
    // m_sendSinceNs, RunSender clears it when it takes the frame), and read relaxed by
    // the once-a-second diag tick and the flush line WITHOUT any lock - no new
    // synchronisation. A read racing a handoff is off by at most one slot occupancy,
    // which is below the resolution these ages are judged at.
    std::atomic<int64_t> m_jobSinceNs{0};
    std::atomic<int64_t> m_sendSinceNs{0};
    ID3D11Texture2D* m_encoding = nullptr;  // the YUV target the encoder is reading
    // When the encoder started its frame and how long a frame usually takes it: the conversion
    // thread takes the game's next frame just in time for the encoder (see Run).
    std::atomic<int64_t> m_encodeStartNs{0}, m_encodeAvgNs{0};
    // Live encoder truth for the stats path (the encoder itself is local to RunEncoder).
    std::atomic<float> m_statQp{-1.0f};
    std::atomic<uint32_t> m_statEncW{0}, m_statEncH{0};
    std::atomic<uint32_t> m_statSrcW{0}, m_statSrcH{0};  // render size before encodeScale (ROUND18 L2)
    std::atomic<double> m_lastGameFps{0}, m_lastEncFps{0}, m_lastDroppedFps{0};
    // Frame flow since the last report: given by the game, replaced in the bridge before
    // conversion, replaced while waiting for the encoder, encoded; conversion and encoding time,
    // and how long the game's frames were held back so the encoder could keep up.
    std::atomic<uint32_t> m_flowGiven{0}, m_flowReplacedBridge{0}, m_flowReplacedQueue{0}, m_flowEncoded{0};
    std::atomic<int64_t> m_flowConvertNs{0}, m_flowEncodeNs{0}, m_flowHeldNs{0};
    // A26 (stale-frame flush): frames RunSender dropped because their predicted display
    // moment had passed (rc/stale_flush.h). Its own family, not folded into
    // m_flowReplacedQueue: that one means "replaced while the encoder was busy", and the
    // 10 s flow line reports this one separately when it is non-zero.
    std::atomic<uint32_t> m_flowStaleDrops{0};
    // A26/A72: RunSender-thread-only flush state - the keyframe-request (300 ms) and log
    // (1 s) throttle stamps, the broken-chain flag and the consecutive-drop streak the
    // A46 breaker counts (rc::StaleFlushState, rc/stale_flush.h), plus the drop count
    // since the last throttled log line.
    rc::StaleFlushState m_staleFlush;
    uint32_t m_staleLogCount = 0;
    int64_t m_flowSinceNs = 0;
    // ROUND19 T1: the diag window's encode-time distribution and frame-seq range. A small
    // lock-free ring the sender thread writes (two relaxed stores per frame, the same cost
    // class as the flow counters above); the once-a-second diag tick reads and resets it.
    static constexpr size_t kDiagRing = 512;
    std::atomic<uint32_t> m_diagSeqMin{0}, m_diagSeqMax{0};   // 0 = none seen this window
    std::atomic<uint32_t> m_diagEncIdx{0};                    // ring cursor, monotonic
    std::atomic<uint32_t> m_diagEncCount{0};                  // entries since the last diag tick
    std::array<std::atomic<uint32_t>, kDiagRing> m_diagEncUs{};   // encode cost, microseconds
    std::array<std::atomic<uint32_t>, kDiagRing> m_diagSeqRing{}; // the seq of each entry
    // G10.1 diagnostics (observation only). halfRate lives in Run(); publish it so the
    // once-a-second timeline can show whether frame skipping coincided with the swings.
    std::atomic<bool> m_halfRateActive{false};
    int64_t m_diagSinceNs = 0;          // the 1 s unified timeline
    int64_t m_diagArrivalPrevNs = 0;    // last game frame we saw, for arrival jitter
    uint32_t m_diagGapMin = 0, m_diagGapMax = 0;  // arrival gaps (ms) in this window
    uint32_t m_diagLockSamples = 0;
    int64_t m_diagLockSumNs = 0, m_diagLockMaxNs = 0;  // keyed-mutex acquire wait
    DiagTiming m_diagHold, m_diagLock1s, m_diagConvertBlock, m_diagMotionRead;
    uint32_t m_diagJobRepl = 0;
    uint32_t m_diagMotionPending = 0;
    std::atomic<uint32_t> m_diagMotionRequests{0};
    rc::MotionMailboxStats m_diagMotionPrev;
    bool m_motionDiagStateKnown = false;
    bool m_motionDiagEnabled = false;
    std::atomic<uint32_t> m_diagSendWaitIdx{0};
    std::atomic<uint32_t> m_diagSendWaitCount{0};
    std::array<std::atomic<uint32_t>, kDiagRing> m_diagSendWaitUs{};
    uint32_t m_diagOverrunsPrev = 0;
    bool m_diagOverrunsPrimed = false;
    // A83 phase M (reading only): the 1 s window behind the fields appended to `rc diag
    // 1s`. submitGap = deltas of consecutive driver-written submitPcNs (driver.cpp:846),
    // the producer's cadence - measured without our pull loop, so it alone separates "the
    // game gave 56/s" from "it gave 90/s and we took 56/s"; arrival1s = our side, same
    // window. Run()-thread-only like m_diagGap*/m_diagLock* above - no new primitive.
    // Signed on purpose: a negative submitGap falsifies the cross-process QPC assumption.
    int64_t m_cadPrevSubmitNs = 0;  // last frame's submitPcNs (0 = no baseline yet)
    uint32_t m_cadSubmitSamples = 0, m_cadArrivalSamples = 0, m_cadSubmitToJobSamples = 0;
    int64_t m_cadSubmitMinNs = 0, m_cadSubmitMaxNs = 0;
    int64_t m_cadArrivalMinNs = 0, m_cadArrivalMaxNs = 0;
    int64_t m_cadSubmitToJobSumNs = 0, m_cadSubmitToJobMaxNs = 0;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_streaming{false};
    std::atomic<bool> m_paused{false};
    // A87/A99: the current session rung is the single source for sender, NVENC and
    // every encoder rebuild. Written on the session thread, read by both stream threads.
    std::atomic<int> m_liveProfile{1};
    // -1 = no pending change; otherwise the rung for RunSender to set on its own
    // VideoSender (m_guard is not thread-safe). Same writer, different consumer.
    std::atomic<int> m_pendingProfile{-1};
    std::mutex m_gazeMutex;
    rc::GazePayload m_gaze{};
    int64_t m_gazeNs = 0;
    struct OneEuroFilter2D {
        float x = 0.5f, y = 0.5f;
        float dx = 0.0f, dy = 0.0f;
        float lastSpeed = 0.0f;
        int64_t lastNs = 0;
        bool initialized = false;

        void Reset() {
            initialized = false;
            dx = dy = 0.0f;
            lastSpeed = 0.0f;
            lastNs = 0;
        }

        void Filter(float inX, float inY, int64_t nowNs, float minCutoffHz, float beta, float dCutoffHz,
                    float& outX, float& outY) {
            if (!initialized) {
                x = inX;
                y = inY;
                dx = dy = 0.0f;
                lastNs = nowNs;
                initialized = true;
                outX = x;
                outY = y;
                return;
            }
            const float dt = float(double(nowNs - lastNs) / 1e9);
            lastNs = nowNs;
            if (dt <= 0.0001f || dt > 0.5f) {
                x = inX;
                y = inY;
                dx = dy = 0.0f;
                outX = x;
                outY = y;
                return;
            }
            auto alpha = [](float rate, float cutoff) {
                const float tau = 1.0f / (2.0f * 3.14159265f * cutoff);
                return 1.0f / (1.0f + tau * rate);
            };
            const float rate = 1.0f / dt;
            const float alphaD = alpha(rate, dCutoffHz);
            const float curDx = (inX - x) / dt;
            const float curDy = (inY - y) / dt;
            dx = alphaD * curDx + (1.0f - alphaD) * dx;
            dy = alphaD * curDy + (1.0f - alphaD) * dy;
            const float speed = std::sqrt(dx * dx + dy * dy);
            lastSpeed = speed;

            const float cutoff = minCutoffHz + beta * speed;
            const float alphaPos = alpha(rate, cutoff);
            x = alphaPos * inX + (1.0f - alphaPos) * x;
            y = alphaPos * inY + (1.0f - alphaPos) * y;
            outX = x;
            outY = y;
        }
    };

    OneEuroFilter2D m_foveaFilter[2];
    float m_alignedEncPx[2][2] = {{-1.f, -1.f}, {-1.f, -1.f}};
    int64_t m_recenterStartNs = 0;
    float m_recenterFrom[2][2] = {};
    bool m_wasTracking = false;

    float m_foveaStart[2][2] = {};  // encoding thread only
    bool m_foveaPlaced = false;
    // A18: the motion grid of the frame before - the state of the quantizer's Schmidt
    // hysteresis (rc::ApplyMotionHysteresis reads it as "was this block moving", writes the
    // new values back). Run()'s thread only; empty, i.e. all still, on the first frame and
    // whenever the estimator resets. The change's trade lives with the thresholds in
    // rc/motion_field.h.
    std::vector<int8_t> m_motionGridPrev;
    rc::MotionFieldMailbox m_motionResults;
    HANDLE m_motionWakeEvent = nullptr;
    std::mutex m_motionSyncMutex;
    std::condition_variable m_motionSyncCv;
    std::atomic<uint32_t> m_motionWaiters{0};
    std::atomic<bool> m_motionHasPendingRead{false};
};

}  // namespace rcs
