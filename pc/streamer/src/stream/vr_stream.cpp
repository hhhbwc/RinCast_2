#include "vr_stream.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include "foveator.h"
#include "motion_estimator.h"
#include "vr_diag_math.h"
#include "rc/fovea_placement.h"
#include "rc/frame_generation.h"
#include "rc/motion_field.h"
#include "rc/stale_flush.h"
#include "rc/tracking_cache.h"

#include <avrt.h>

#include "../vr/vr_bridge_host.h"
#include "rc/platform.h"

namespace rcs {

namespace {

// Rotates a direction seen from `now` into what it was seen as from `before`.
void HeadTurn(const rcvr::BridgePose& before, const rcvr::BridgePose& now, float m[9]) {
    const float* a = before.orientation;  // x, y, z, w; conjugated below
    const float* b = now.orientation;
    const float ax = -a[0], ay = -a[1], az = -a[2], aw = a[3];
    const float x = aw * b[0] + ax * b[3] + ay * b[2] - az * b[1];
    const float y = aw * b[1] - ax * b[2] + ay * b[3] + az * b[0];
    const float z = aw * b[2] + ax * b[1] - ay * b[0] + az * b[3];
    const float w = aw * b[3] - ax * b[0] - ay * b[1] - az * b[2];
    m[0] = 1 - 2 * (y * y + z * z);
    m[1] = 2 * (x * y - z * w);
    m[2] = 2 * (x * z + y * w);
    m[3] = 2 * (x * y + z * w);
    m[4] = 1 - 2 * (x * x + z * z);
    m[5] = 2 * (y * z - x * w);
    m[6] = 2 * (x * z - y * w);
    m[7] = 2 * (y * z + x * w);
    m[8] = 1 - 2 * (x * x + y * y);
}

// For what can go wrong on every frame: each reason at most once every 5 seconds, with how
// often it happened since it was last logged.
void WarnEvery5s(const char* reason) {
    static std::mutex mutex;
    static std::map<const char*, std::pair<int64_t, uint32_t>> seen;  // last logged, count since
    std::lock_guard lock(mutex);
    auto& [last, count] = seen[reason];
    count++;
    const int64_t now = rc::NowNs();
    if (last && now - last < 5'000'000'000LL) return;
    RC_LOGW("%s (%u times)", reason, count);
    last = now;
    count = 0;
}

// A26 (stale-frame flush): how long after the driver's submit the headset expects to
// show the frame, when the frame's exact prediction is unavailable. The same 40 ms
// client_session falls back to when no clock offset is known yet.
constexpr int64_t kStalePredictAheadNs = 40'000'000;
// A26: a prediction further before the driver's submit than this means the clock
// mapping behind it is off (a frame cannot be displayed before it was rendered);
// such a value is not trusted and the predict-ahead estimate is used instead.
constexpr int64_t kStaleClockGuardNs = 5'000'000;

bool AsyncMotionReadback() {
    static const bool enabled = [] {
        char text[2]{};
        return !(GetEnvironmentVariableA("RINCAST_ASYNC_MOTION", text, sizeof(text)) == 1 && text[0] == '0');
    }();
    return enabled;
}

}  // namespace

// The full-density square follows the eyes, but only moves once they look clearly elsewhere:
// while it stays put, the encoder can keep referring to the frames before. Without a recent
// gaze, it sits where each eye's lens is sharpest.
void VrStream::PlaceFovea(const float fovTan[2][4], float start[2][2]) {
    rc::GazePayload g;
    int64_t at = 0;
    {
        std::lock_guard lock(m_gazeMutex);
        g = m_gaze;
        at = m_gazeNs;
    }
    const int64_t now = rc::NowNs();
    const int64_t ageNs = at ? (now - at) : 10'000'000'000LL;
    // 1) Gaze is fresh (< 800 ms): live track the eyes with One-Euro smoothing + CTU macroblock grid alignment.
    // 2) Gaze lost/blinking briefly (800 ms .. 2000 ms): hold last placed fovea, never snap to centre.
    // 3) Gaze lost for a long time (> 2000 ms): gracefully interpolate back to lens centre over 200 ms.
    const bool fresh = ageNs < 800'000'000LL;
    const bool holdGrace = m_foveaPlaced && (ageNs < 2'000'000'000LL);
    const bool tracking = fresh && (g.flags != 0);

    const float eyeW = m_config.eyeWidth > 0 ? float(m_config.eyeWidth) : 2048.0f;
    const float eyeH = m_config.eyeHeight > 0 ? float(m_config.eyeHeight) : 2048.0f;
    const float fSize = m_config.foveaSize;

    if (tracking) {
        m_recenterStartNs = 0;
        m_wasTracking = true;
    } else if (m_wasTracking && !holdGrace) {
        if (m_recenterStartNs == 0) {
            m_recenterStartNs = now;
            std::memcpy(m_recenterFrom, m_foveaStart, sizeof(m_foveaStart));
        }
    }

    for (int eye = 0; eye < 2; eye++) {
        // The eye's own gaze, or the other one's while it blinks.
        const int from = (g.flags & (1 << eye)) ? eye : (g.flags & (1 << (1 - eye))) ? 1 - eye : -1;
        if (fresh && from >= 0) {
            float target[2] = {m_foveaStart[eye][0], m_foveaStart[eye][1]};
            rc::FoveaSquare(fovTan[eye], g.gaze[from], fSize, m_foveaPlaced, target);
            // One-Euro speed-adaptive filter: minCutoff 1.0 Hz (lock fixation noise), beta 0.5 (fast saccades)
            float filteredX = target[0], filteredY = target[1];
            m_foveaFilter[eye].Filter(target[0], target[1], now, 1.0f, 0.5f, 1.0f, filteredX, filteredY);

            // Align foveaStart boundaries to 32px macroblock (CTU) grid
            float pxX = filteredX * eyeW;
            float pxY = filteredY * eyeH;
            float alignedPxX = std::round(pxX / 32.0f) * 32.0f;
            float alignedPxY = std::round(pxY / 32.0f) * 32.0f;
            m_foveaStart[eye][0] = std::clamp(alignedPxX / eyeW, 0.0f, 1.0f - fSize);
            m_foveaStart[eye][1] = std::clamp(alignedPxY / eyeH, 0.0f, 1.0f - fSize);
        } else if (holdGrace) {
            // Blink or brief tracking loss: hold fovea in place to prevent twitching/flicker.
        } else {
            // Lost tracking (> 2000 ms): smooth 200 ms interpolation back to lens centre
            float center[2] = {};
            rc::FoveaSquare(fovTan[eye], nullptr, fSize, false, center);
            if (m_recenterStartNs > 0 && (now - m_recenterStartNs) < 200'000'000LL) {
                const float t = std::clamp(float(double(now - m_recenterStartNs) / 200'000'000.0), 0.0f, 1.0f);
                const float s = t * t * (3.0f - 2.0f * t);  // smoothstep
                m_foveaStart[eye][0] = (1.0f - s) * m_recenterFrom[eye][0] + s * center[0];
                m_foveaStart[eye][1] = (1.0f - s) * m_recenterFrom[eye][1] + s * center[1];
            } else {
                m_foveaStart[eye][0] = center[0];
                m_foveaStart[eye][1] = center[1];
                m_foveaFilter[eye].Reset();
                m_wasTracking = false;
            }
        }
        start[eye][0] = m_foveaStart[eye][0];
        start[eye][1] = m_foveaStart[eye][1];
    }
    m_foveaPlaced = true;
}

VrStream::VrStream(rc::Connection* connection, VrBridgeHost* bridge, const VrStreamConfig& config,
                   StateHandler onState)
    : m_connection(connection),
      m_bridge(bridge),
      m_config(config),
      m_liveProfile(int(config.profile)),
      m_sender(connection, rc::kVrStreamId, config.codec, config.bitrateKbps, config.fecPercent, config.adaptive,
               config.profile),
      m_onState(std::move(onState)),
      m_motionWakeEvent(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {
    if (config.startBitrateKbps) m_sender.StartAt(config.startBitrateKbps);
}

VrStream::~VrStream() {
    Stop();
    if (m_motionWakeEvent) {
        CloseHandle(m_motionWakeEvent);
        m_motionWakeEvent = nullptr;
    }
}


bool VrStream::Start() {
    m_running = true;
    m_sendThread = std::thread([this] { RunSender(); });
    m_encodeThread = std::thread([this] { RunEncoder(); });
    m_thread = std::thread([this] { Run(); });
    return true;
}

void VrStream::Stop() {
    m_running = false;
    // Under the locks: a thread between checking m_running and waiting would miss the wake-up.
    { std::lock_guard lock(m_jobMutex); }
    { std::lock_guard lock(m_sendMutex); }
    { std::lock_guard lock(m_motionSyncMutex); }
    m_jobReady.notify_all();
    m_jobTaken.notify_all();
    m_sendCv.notify_all();
    m_motionSyncCv.notify_all();
    if (m_motionWakeEvent) SetEvent(m_motionWakeEvent);
    if (m_thread.joinable()) m_thread.join();
    if (m_encodeThread.joinable()) m_encodeThread.join();
    if (m_sendThread.joinable()) m_sendThread.join();
}

static const char* ProducerName(rcvr::ProducerKind k) {
    switch (k) {
        case rcvr::ProducerKind::SteamVR: return "steamvr";
        case rcvr::ProducerKind::OpenXR: return "openxr";
        default: return "";
    }
}

void VrStream::LogFrameFlow(int64_t now) {
    // G10.1: a once-a-second line that puts every key quantity on ONE clock. The other
    // logs each run their own 5/10 s window with a different start, so they never line up;
    // this one is for correlating "when it pumped, what were target/queue/half-rate doing".
    if (!m_diagSinceNs) m_diagSinceNs = now;
    if (now - m_diagSinceNs >= 1'000'000'000LL) {
        m_diagSinceNs = now;
        const VideoSenderStats s = Stats();
        // ROUND19 T1: frame-level identity + encode-time distribution for the window just
        // ended (telemetry only; reads the counters the send path keeps above).
        const uint32_t end = m_diagEncIdx.load(std::memory_order_relaxed);
        const uint32_t count = std::min<uint32_t>(m_diagEncCount.exchange(0, std::memory_order_relaxed), uint32_t(kDiagRing));
        std::vector<std::pair<uint32_t, double>> frames;  // (seq, encodeMs), window order
        frames.reserve(count);
        VisitDiagRing(end, count, uint32_t(kDiagRing), [&](uint32_t idx) {
            frames.emplace_back(m_diagSeqRing[idx].load(std::memory_order_relaxed),
                                m_diagEncUs[idx].load(std::memory_order_relaxed) / 1000.0);
        });
        const uint32_t seqMin = m_diagSeqMin.exchange(0, std::memory_order_relaxed);
        const uint32_t seqMax = m_diagSeqMax.exchange(0, std::memory_order_relaxed);
        std::vector<double> encSorted;
        encSorted.reserve(frames.size());
        for (auto& f : frames) encSorted.push_back(f.second);
        std::sort(encSorted.begin(), encSorted.end());
        const double p50 = encSorted.empty() ? 0.0 : encSorted[size_t((encSorted.size() - 1) * 50 / 100)];
        const double p95 = encSorted.empty() ? 0.0 : encSorted[size_t((encSorted.size() - 1) * 95 / 100)];
        const double periodMs = m_config.fps > 0 ? 1000.0 / m_config.fps : 0.0;
        char seqPart[32];
        if (seqMax) snprintf(seqPart, sizeof(seqPart), ", seq %u-%u", seqMin, seqMax);
        else snprintf(seqPart, sizeof(seqPart), ", seq -");
        // A79 phase M (stale-flush attribution, reading only): the send-side pacing
        // backlog and the burst-overrun counter on the same line (the pair that
        // separates "the clock was pushed ahead by debt" from "the socket held the
        // burst back"), plus the in-slot age of the frame now sitting in m_job and in
        // the send slot (-1.0 = the slot was empty at the tick). All are relaxed reads
        // of snapshots their owning threads store at the existing write points; no
        // lock, no new primitive. Appended AFTER the leading fields, which stay
        // byte-for-byte for tools/logpair.py PC_DIAG and analyze_dual.py RE_DIAG1S
        // (neither regex is end-anchored, so an append cannot break them).
        const double backlogMs = double(m_sender.SendBacklogNs()) / 1e6;
        const uint32_t overruns = m_sender.BudgetOverruns();
        const int64_t jobSince = m_jobSinceNs.load(std::memory_order_relaxed);
        const int64_t sendSince = m_sendSinceNs.load(std::memory_order_relaxed);
        const double jobAgeMs = jobSince ? double(now - jobSince) / 1e6 : -1.0;
        const double sendAgeMs = sendSince ? double(now - sendSince) / 1e6 : -1.0;
        // A83 phase M (reading only), appended at the END - logpair.py PC_DIAG :39 and
        // analyze_dual.py RE_DIAG1S :46 are not end-anchored. submitGap = deltas of
        // consecutive driver-written submitPcNs (driver.cpp:846): the producer's cadence,
        // independent of our pull loop, so it alone separates H4a (producer gave ~56/s)
        // from H4b (it gave 90/s, we took 56/s); arrival1s = our side, same window.
        // n/a = no valid sample (submitPcNs == 0 excluded, never a 0 min). Signed: a
        // negative falsifies the cross-process QPC assumption - never abs()'d. %.1f per M4.
        const auto fmtRange = [](char* b, size_t n, const char* name, uint32_t samples, int64_t minNs, int64_t maxNs) {
            if (samples) snprintf(b, n, ", %s %.1f-%.1f ms", name, double(minNs) / 1e6, double(maxNs) / 1e6);
            else snprintf(b, n, ", %s n/a", name);
        };
        char arrPart[64], subPart[64], s2jPart[64];
        fmtRange(arrPart, sizeof(arrPart), "arrival1s", m_cadArrivalSamples, m_cadArrivalMinNs, m_cadArrivalMaxNs);
        fmtRange(subPart, sizeof(subPart), "submitGap", m_cadSubmitSamples, m_cadSubmitMinNs, m_cadSubmitMaxNs);
        if (m_cadSubmitToJobSamples)
            snprintf(s2jPart, sizeof(s2jPart), ", submitToJob avg %.1f/max %.1f ms",
                     double(m_cadSubmitToJobSumNs) / m_cadSubmitToJobSamples / 1e6,
                     double(m_cadSubmitToJobMaxNs) / 1e6);
        else snprintf(s2jPart, sizeof(s2jPart), ", submitToJob n/a");
        const auto fmtAvgMax = [](char* b, size_t n, const char* name, const DiagTiming& t) {
            if (t.samples)
                snprintf(b, n, ", %s avg/max %.1f/%.1f ms", name,
                         double(t.sumNs) / t.samples / 1e6, double(t.maxNs) / 1e6);
            else snprintf(b, n, ", %s n/a", name);
        };
        DiagTiming sendWait;
        const uint32_t sendWaitEnd = m_diagSendWaitIdx.load(std::memory_order_relaxed);
        const uint32_t sendWaitCount =
            std::min<uint32_t>(m_diagSendWaitCount.exchange(0, std::memory_order_relaxed), uint32_t(kDiagRing));
        VisitDiagRing(sendWaitEnd, sendWaitCount, uint32_t(kDiagRing), [&](uint32_t idx) {
            sendWait.Add(int64_t(m_diagSendWaitUs[idx].load(std::memory_order_relaxed)) * 1000);
        });
        char holdPart[96], lockPart[96], convertPart[96], motionPart[96], sendWaitPart[96], jobReplPart[40];
        fmtAvgMax(holdPart, sizeof(holdPart), "hold", m_diagHold);
        fmtAvgMax(lockPart, sizeof(lockPart), "lock", m_diagLock1s);
        fmtAvgMax(convertPart, sizeof(convertPart), "convertBlock", m_diagConvertBlock);
        fmtAvgMax(motionPart, sizeof(motionPart), "motionRead", m_diagMotionRead);
        fmtAvgMax(sendWaitPart, sizeof(sendWaitPart), "sendWait", sendWait);
        snprintf(jobReplPart, sizeof(jobReplPart), ", jobRepl %u", m_diagJobRepl);
        const uint32_t overruns1s = m_diagOverrunsPrimed ? (overruns >= m_diagOverrunsPrev ? overruns - m_diagOverrunsPrev
                                                                                         : overruns)
                                                        : 0;
        m_diagOverrunsPrev = overruns;
        RC_LOGI("rc diag 1s: game %.0f/s enc %.0f/s drop %.0f/s, halfRate %s, target %u kbps, wire %.1f Mbps, "
                "queue %.0f ms, qp %.1f, render %ux%u enc %ux%u%s, enc p50/p95 %.1f/%.1f ms"
                ", sendBacklog %.1f ms, overruns %u, jobAge %.1f ms, sendAge %.1f ms%s%s%s"
                "%s%s%s%s%s%s, overruns1s %u, motionPending %u",
                s.gameFps, s.encFps, s.droppedFps, m_halfRateActive.load(std::memory_order_relaxed) ? "ON" : "OFF",
                m_sender.TargetBitrateKbps(), s.mbps, s.queueMs, s.qp, s.srcWidth, s.srcHeight, s.encWidth, s.encHeight,
                seqPart, p50, p95, backlogMs, overruns, jobAgeMs, sendAgeMs, arrPart, subPart, s2jPart,
                holdPart, lockPart, convertPart, motionPart, jobReplPart, sendWaitPart, overruns1s, m_diagMotionPending);
        m_diagHold = {};
        m_diagLock1s = {};
        m_diagConvertBlock = {};
        m_diagMotionRead = {};
        m_diagMotionPending = 0;
        const uint32_t motionRequests = m_diagMotionRequests.exchange(0, std::memory_order_acq_rel);
        const rc::MotionMailboxStats motion = m_motionResults.Stats();
        const auto motionDelta = [&](auto member) { return motion.*member - m_diagMotionPrev.*member; };
        const bool hasMailboxActivity = rc::MotionMailboxHasActivity(motion, m_diagMotionPrev);
        const double oldestAgeMs = ElapsedPositiveNs(now, motion.oldestEnqueuedNs);
        const double newestAgeMs = ElapsedPositiveNs(now, motion.newestEnqueuedNs);
        if (rc::ShouldLogMotionDiagnostics(m_config.motionField, hasMailboxActivity,
                                          m_motionDiagStateKnown, m_motionDiagEnabled)) {
            RC_LOGI("rc motion 1s: enabled %d, encoded %u, fields %llu, attached %llu, missing %llu, expired %llu, "
                    "evicted %llu, cleared %llu, invalid %llu, pending %llu, oldestAge %.1f ms, newestAge %.1f ms",
                    int(m_config.motionField), motionRequests,
                    static_cast<unsigned long long>(motionDelta(&rc::MotionMailboxStats::produced)),
                    static_cast<unsigned long long>(motionDelta(&rc::MotionMailboxStats::hits)),
                    static_cast<unsigned long long>(motionDelta(&rc::MotionMailboxStats::misses)),
                    static_cast<unsigned long long>(motionDelta(&rc::MotionMailboxStats::expired)),
                    static_cast<unsigned long long>(motionDelta(&rc::MotionMailboxStats::evicted)),
                    static_cast<unsigned long long>(motionDelta(&rc::MotionMailboxStats::cleared)),
                    static_cast<unsigned long long>(motionDelta(&rc::MotionMailboxStats::invalid)),
                    static_cast<unsigned long long>(motion.pending), oldestAgeMs / 1e6, newestAgeMs / 1e6);
        }
        m_motionDiagStateKnown = true;
        m_motionDiagEnabled = m_config.motionField;
        m_diagMotionPrev = motion;
        m_diagJobRepl = 0;
        m_cadArrivalSamples = 0; m_cadArrivalMinNs = m_cadArrivalMaxNs = 0;
        m_cadSubmitSamples = 0; m_cadSubmitMinNs = m_cadSubmitMaxNs = 0;
        m_cadSubmitToJobSamples = 0; m_cadSubmitToJobSumNs = m_cadSubmitToJobMaxNs = 0;
        // ROUND19 T1: sparse slow-frame trace, only under pressure, at most 8 per window.
        if ((s.droppedFps > 0.0 || (periodMs > 0 && p95 > periodMs)) && !frames.empty()) {
            std::vector<std::pair<uint32_t, double>> worst(frames);
            std::sort(worst.begin(), worst.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });
            if (worst.size() > 8) worst.resize(8);
            std::string seqs, encs;
            char part[48];
            for (auto& f : worst) {
                snprintf(part, sizeof(part), "%s%u", seqs.empty() ? "" : ",", f.first);
                seqs += part;
                snprintf(part, sizeof(part), "%s%.1f", encs.empty() ? "" : ",", f.second);
                encs += part;
            }
            RC_LOGI("slow-frames seq=%s enc=%s ms", seqs.c_str(), encs.c_str());
        }
    }

    if (!m_flowSinceNs) m_flowSinceNs = now;
    if (now - m_flowSinceNs < 10'000'000'000LL) return;
    const double secs = double(now - m_flowSinceNs) / 1e9;
    m_flowSinceNs = now;
    const uint32_t given = m_flowGiven.exchange(0), bridge = m_flowReplacedBridge.exchange(0),
                   queue = m_flowReplacedQueue.exchange(0), encoded = m_flowEncoded.exchange(0);
    const uint32_t staleDrops = m_flowStaleDrops.exchange(0);  // A26: the window's flush count
    const int64_t convertNs = m_flowConvertNs.exchange(0), encodeNs = m_flowEncodeNs.exchange(0),
                  heldNs = m_flowHeldNs.exchange(0);
    if (!given) return;
    const uint32_t converted = BoundedConvertedCount(given, bridge);
    // Live rates for the stats path (atomics: Stats() reads from another thread).
    m_lastGameFps.store(double(given) / secs, std::memory_order_relaxed);
    m_lastEncFps.store(double(encoded) / secs, std::memory_order_relaxed);
    m_lastDroppedFps.store(double(queue) / secs, std::memory_order_relaxed);
    // G10.3: arrival-gap and keyed-mutex wait spread, reset each 10 s window.
    const uint32_t gapMin = m_diagGapMin, gapMax = m_diagGapMax;
    const uint32_t lockN = m_diagLockSamples;
    const double lockAvgMs = lockN ? double(m_diagLockSumNs) / lockN / 1e6 : 0.0;
    const double lockMaxMs = double(m_diagLockMaxNs) / 1e6;
    m_diagGapMin = 0;
    m_diagGapMax = 0;
    m_diagLockSamples = 0;
    m_diagLockSumNs = 0;
    m_diagLockMaxNs = 0;
    RC_LOGI("VR frames: %.0f/s from the game, %.0f/s encoded; %.0f/s replaced before conversion, %.0f/s while "
            "the encoder was busy; conversion %.1f ms, encoding %.1f ms, held for the encoder %.1f ms, "
            "arrivalGap %u-%u ms, lockWait avg %.1f/max %.1f ms (%u)",
            given / secs, encoded / secs, bridge / secs, queue / secs,
            converted ? double(convertNs) / converted / 1e6 : 0.0, encoded ? double(encodeNs) / encoded / 1e6 : 0.0,
            converted ? double(heldNs) / converted / 1e6 : 0.0,
            gapMin, gapMax, lockAvgMs, lockMaxMs, lockN);
    // A26: the window's stale-flush count, on its own line and only when it fired -
    // the steady state's log stream stays byte-for-byte as it was.
    if (staleDrops)
        RC_LOGI("VR stale flush: %u frame(s) dropped past their display moment in this 10 s window", staleDrops);
}

void VrStream::WaitForMotionField(uint64_t frameIndex, uint64_t headsetTargetNs) {
    if (!frameIndex || !headsetTargetNs || m_motionResults.HasTimestamp(frameIndex, headsetTargetNs)) return;
    if (!m_motionHasPendingRead.load(std::memory_order_acquire)) return;
    m_motionWaiters.fetch_add(1, std::memory_order_acq_rel);
    if (m_motionWakeEvent) SetEvent(m_motionWakeEvent);
    {
        std::unique_lock lock(m_motionSyncMutex);
        m_motionSyncCv.wait_for(lock, std::chrono::milliseconds(5), [&] {
            return !m_running || m_motionResults.HasTimestamp(frameIndex, headsetTargetNs) ||
                   !m_motionHasPendingRead.load(std::memory_order_acquire);
        });
    }
    m_motionWaiters.fetch_sub(1, std::memory_order_acq_rel);
}

int64_t VrStream::WaitForEncoder(int64_t convertAvgNs, const std::function<bool()>& pollMotion) {
    const int64_t start = rc::NowNs();
    const auto finish = [&] {
        const int64_t heldNs = rc::NowNs() - start;
        m_flowHeldNs += heldNs;
        m_diagHold.Add(heldNs);
        return heldNs;
    };
    {
        std::unique_lock lock(m_jobMutex);
        // The frame before still waits for the encoder: one converted now would only replace it.
        if (m_job) {
            const int64_t waitEndNs = rc::NowNs() + 100'000'000LL;
            while (m_running && m_job) {
                if (rc::NowNs() >= waitEndNs) break;
                bool morePending = false;
                if (pollMotion) {
                    lock.unlock();
                    morePending = pollMotion();
                    lock.lock();
                }
                if (!m_running || !m_job) break;
                const int64_t remNs = waitEndNs - rc::NowNs();
                if (remNs <= 0) break;
                const int sliceMs = morePending ? 1 : int((remNs + 999'999) / 1'000'000);
                m_jobTaken.wait_for(lock, std::chrono::milliseconds(std::max(1, sliceMs)),
                                    [this] { return !m_running || !m_job; });
            }
        }
        if (!m_encoding || m_job) {
            lock.unlock();
            if (pollMotion) (void)pollMotion();
            return finish();
        }
    }
    // The encoder is busy: take the game's next frame so its conversion ends about when the
    // encoder is free. Taken sooner, it would wait for the encoder and arrive older. Meanwhile the
    // frame stays in the bridge, and the driver holds the game until it's taken: the game renders
    // at the rate the encoder keeps up with, not frames that would be thrown away.
    const int64_t readyAt = m_encodeStartNs + m_encodeAvgNs - convertAvgNs - 1'000'000;
    const int64_t sleepUntil = std::min(readyAt, rc::NowNs() + 20'000'000);
    while (m_running) {
        const int64_t remainingNs = sleepUntil - rc::NowNs();
        if (remainingNs < 1'000'000) break;
        const bool morePending = pollMotion && pollMotion();
        const int64_t remAfterNs = sleepUntil - rc::NowNs();
        if (remAfterNs < 1'000'000) break;
        const DWORD sleepMs = DWORD(morePending ? 1 : int(remAfterNs / 1'000'000));
        if (m_motionWakeEvent)
            WaitForSingleObject(m_motionWakeEvent, sleepMs);
        else
            rc::SleepMs(int(sleepMs));
    }
    if (pollMotion) (void)pollMotion();
    return finish();
}

void VrStream::Run() {
    ScopedCoInit com;
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);  // highest MMCSS class
    if (mmcss) AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_HIGH);

    ID3D11Device* device = m_bridge->Device();
    ComPtr<ID3D11DeviceContext> ctx;
    device->GetImmediateContext(&ctx);
    rcvr::Shared* shared = m_bridge->Shared();
    // The conversion queues behind the game's GPU work otherwise; with a busy GPU it waits most
    // of a frame. A relative priority, within this process's class: no privilege needed.
    {
        static std::once_flag logged;
        ComPtr<IDXGIDevice> dxgi;
        const HRESULT hr = SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) ? dxgi->SetGPUThreadPriority(7) : E_NOINTERFACE;
        std::call_once(logged, [hr] { RC_LOGI("VR: GPU priority for conversion %s", SUCCEEDED(hr) ? "raised" : "unchanged (refused)"); });
    }

    // Three YUV targets: one the encoder reads, one waiting for it, one being converted into.
    ColorConverter converters[3];
    Foveator foveator;
    const bool foveated = m_config.foveatedEyeWidth && m_config.foveatedEyeHeight;
    uint32_t srcWidth = 0, srcHeight = 0;  // the frame from the game
    uint32_t encWidth = 0, encHeight = 0;  // what's encoded: smaller when foveated
    bool convertersReady = false;
    rc::HalfRateState halfRateState;
    bool motionSkipPending = false;
    m_sender.SetPacingMode(m_config.pacingMode, 0);
    m_diagOverrunsPrev = m_sender.BudgetOverruns();
    m_diagOverrunsPrimed = true;
    uint64_t lastFrameIndex = 0;
    int64_t lastFrameNs = 0;
    bool streaming = false;
    rcvr::ProducerKind lastProducer = rcvr::ProducerKind::None;
    std::string lastApp;
    MotionEstimator motion;
    const bool asyncMotion = AsyncMotionReadback();
    bool motionReady = false;
    uint64_t lastMotionFrameIndex = 0;
    std::map<ID3D11Texture2D*, ComPtr<ID3D11ShaderResourceView>> motionViews;
    rcvr::BridgePose motionPose{};
    uint64_t motionPreviousTimestamp = 0;
    int64_t motionPrevSubmitNs = 0;
    uint64_t motionLastIntervalNs = 0;
    std::vector<float> motionField;
    // A18: the quantized grid lives on in m_motionGridPrev (the hysteresis state) instead of
    // a local, per the card: the cross-frame state belongs to VrStream.
    uint32_t lastTextureGeneration = m_bridge->TextureGeneration();
    // A61: a producer that restarted starts its frame counter over (driver.cpp resets it
    // per vrserver process) and the textures may be a new generation. Everything derived
    // from the old producer's frames has to go with it - above all lastFrameIndex, which
    // otherwise rejects every new frame as stale for the rest of the session.
    const auto resetProducerCaches = [&] {
        lastFrameIndex = 0;
        m_diagArrivalPrevNs = 0;
        m_cadPrevSubmitNs = 0;  // A83: the next frame becomes the cadence baseline again
        motionViews.clear();  // the SRVs pin the previous generation's textures
        motion.Reset();
        m_motionHasPendingRead.store(false, std::memory_order_release);
        lastMotionFrameIndex = 0;
        motionSkipPending = false;
        m_motionGridPrev.clear();
        m_motionResults.Clear();
        motionPose = rcvr::BridgePose{};
        motionPreviousTimestamp = 0;
        motionPrevSubmitNs = 0;
        motionLastIntervalNs = 0;
    };

    m_onState("waiting", "", "");
    bool firstFrameLogged = false;
    int64_t lastWaitReportNs = rc::NowNs();
    int64_t convertAvgNs = 0;
    // Drain any finished async GPU motion readbacks before RunEncoder reaches AttachMotionField,
    // instead of leaving them unread until the next game frame wakes this loop.
    const auto pollPendingMotion = [&]() -> bool {
        if (!streaming || m_paused || !asyncMotion || !motionReady) return false;
        bool publishedAny = false;
        if (motion.HasPendingRead()) {
            bool slotConsumed = false;
            do {
                rc::MotionReadbackTiming timing;
                slotConsumed = false;
                if (motion.TryRead(ctx.Get(), motionField, timing, slotConsumed)) {
                    const uint64_t publishedNs = uint64_t(rc::NowNs());
                    if (rc::PublishMotionField(m_motionResults, true, timing, uint16_t(motion.GridWidth()),
                                               uint16_t(motion.GridHeight()), uint16_t(MotionEstimator::kBlockSize),
                                               motionField, m_motionGridPrev, publishedNs)) {
                        publishedAny = true;
                    }
                }
            } while (slotConsumed && motion.HasPendingRead());
        }
        const bool stillPending = motion.HasPendingRead();
        m_motionHasPendingRead.store(stillPending, std::memory_order_release);
        if (publishedAny || (!stillPending && m_motionWaiters.load(std::memory_order_acquire) > 0)) {
            std::lock_guard lock(m_motionSyncMutex);
            m_motionSyncCv.notify_all();
        }
        return stillPending;
    };
    while (m_running) {
        const int64_t waitStartNs = rc::NowNs();
        for (;;) {
            const bool wasPending = pollPendingMotion();
            const int64_t elapsedMs = (rc::NowNs() - waitStartNs) / 1'000'000;
            if (!wasPending && elapsedMs >= 50) break;
            const bool waiterWaiting = wasPending && m_motionWaiters.load(std::memory_order_acquire) > 0;
            const DWORD timeoutMs = waiterWaiting ? 0u : (wasPending ? 1u : DWORD(50 - elapsedMs));
            DWORD waitRes = WAIT_TIMEOUT;
            if (timeoutMs == 0u) {
                SwitchToThread();
                waitRes = WaitForSingleObject(m_bridge->FrameEvent(), 0);
            } else {
                HANDLE waitHandles[2] = {m_bridge->FrameEvent(), m_motionWakeEvent};
                const DWORD waitCount = m_motionWakeEvent ? 2u : 1u;
                waitRes = WaitForMultipleObjects(waitCount, waitHandles, FALSE, timeoutMs);
            }
            if (!m_running) break;
            (void)pollPendingMotion();
            if (waitRes == WAIT_OBJECT_0 || !wasPending) break;
            const LONG64 ready = shared->latestReadySlot;
            if (ready >= 0) {
                const int readySlot = int(ready & 0xFF);
                if (readySlot < rcvr::kFrameSlots && shared->slots[readySlot].state == rcvr::kSlotReady) break;
            }
            if (rc::NowNs() - waitStartNs >= 50'000'000LL) break;
        }
        if (!m_running) break;
        // Not before the encoder is about to want it; the frame waits in the bridge, the game with it.
        int64_t frameHoldNs = -1;
        if (streaming && !m_paused) frameHoldNs = WaitForEncoder(convertAvgNs, pollPendingMotion);
        const int64_t now = rc::NowNs();
        // A61: dead-producer recovery and a resolution change both hand out new textures.
        if (const uint32_t generation = m_bridge->TextureGeneration(); generation != lastTextureGeneration) {
            RC_LOGI("VR: bridge textures recreated (generation %u -> %u), restarting frame accounting",
                    lastTextureGeneration, generation);
            lastTextureGeneration = generation;
            resetProducerCaches();
        }
        // No picture: say every 10 s what the bridge looks like, so a log shows whether the
        // driver never sent a frame or frames arrive and get dropped here.
        if (!streaming && now - lastWaitReportNs > 10'000'000'000LL) {
            lastWaitReportNs = now;
            RC_LOGW("VR: no frame sent for 10 s. Producer %s (pid %ld), latest slot %lld, slots %ld/%ld/%ld, "
                    "heartbeat %.1f s ago, encoder %s",
                    ProducerName(m_bridge->ActiveProducer()), long(shared->producerPid), (long long)shared->latestReadySlot,
                    long(shared->slots[0].state), long(shared->slots[1].state), long(shared->slots[2].state),
                    shared->producerHeartbeatNs ? double(rcvr::QpcNowNs() - shared->producerHeartbeatNs) / 1e9 : -1.0,
                    convertersReady ? "ready" : "not created");
        }
        if (streaming && !m_paused) LogFrameFlow(now);

        const rcvr::ProducerKind producer = m_bridge->ActiveProducer();
        const std::string app = m_bridge->ApplicationName();
        // A88: one second was too eager. A tier rebuild (A87 no longer does one),
        // a SteamVR resolution settle or a single slow game frame (the 10:17 log
        // has 801 ms gaps) used to trip "waiting" and send the headset to the
        // desktop view; coming back meant another visible hitch. Three seconds
        // still reports a real dead producer, and the 10 s WARN below is unchanged.
        const bool fresh = now - lastFrameNs < 3'000'000'000LL;
        if (streaming && !fresh) {
            streaming = false;
            m_streaming = false;
            lastWaitReportNs = now;
            m_onState("waiting", ProducerName(producer), app);
        }
        if (producer != lastProducer || app != lastApp) {
            lastProducer = producer;
            lastApp = app;
            m_onState(streaming ? "running" : "waiting", ProducerName(producer), app);
        }

        const LONG64 latest = shared->latestReadySlot;
        if (latest < 0) continue;
        const int slot = int(latest & 0xFF);
        if (slot >= rcvr::kFrameSlots) continue;
        rcvr::FrameSlot& fs = shared->slots[slot];
        if (InterlockedCompareExchange(&fs.state, rcvr::kSlotReading, rcvr::kSlotReady) != rcvr::kSlotReady) continue;
        // A61: a restarted producer's counter runs backwards. Read only after a successful
        // CAS: before it, the slot's index is whatever the last wake already consumed.
        if (lastFrameIndex != 0 && fs.frameIndex <= lastFrameIndex) {
            RC_LOGI("VR: producer restarted (frame %llu -> %llu), restarting frame accounting",
                    (unsigned long long)lastFrameIndex, (unsigned long long)fs.frameIndex);
            resetProducerCaches();
        }
        // Frames the game finished that were replaced before this one was taken.
        if (lastFrameIndex && !m_paused) {
            const uint64_t given = fs.frameIndex - lastFrameIndex;
            m_flowGiven += uint32_t(given);
            m_flowReplacedBridge += uint32_t(given - 1);
            // G10.3: arrival gap distribution. A steady fps with a bimodal gap is
            // hidden by the mean; min..max in the 10 s line makes it visible.
            if (m_diagArrivalPrevNs) {
                const uint32_t gapMs = uint32_t((now - m_diagArrivalPrevNs) / 1'000'000);
                if (m_diagGapMin == 0 || gapMs < m_diagGapMin) m_diagGapMin = gapMs;
                if (gapMs > m_diagGapMax) m_diagGapMax = gapMs;
                // A83 phase M: our-side gap on the 1 s diag window (the 10 s %u-%u above untouched).
                const int64_t gapNs = now - m_diagArrivalPrevNs;
                if (!m_cadArrivalSamples || gapNs < m_cadArrivalMinNs) m_cadArrivalMinNs = gapNs;
                if (!m_cadArrivalSamples || gapNs > m_cadArrivalMaxNs) m_cadArrivalMaxNs = gapNs;
                m_cadArrivalSamples++;
            }
            m_diagArrivalPrevNs = now;
            // A83 phase M: the producer's own cadence - the driver writes submitPcNs
            // (driver.cpp:846) whether or not our loop keeps up, which is why this delta
            // separates H4a/H4b while arrivalGap cannot. submitPcNs == 0 drops the
            // baseline instead of entering the statistics as 0; the delta stays signed.
            if (fs.submitPcNs == 0) {
                m_cadPrevSubmitNs = 0;
            } else {
                if (m_cadPrevSubmitNs) {
                    const int64_t d = fs.submitPcNs - m_cadPrevSubmitNs;
                    if (!m_cadSubmitSamples || d < m_cadSubmitMinNs) m_cadSubmitMinNs = d;
                    if (!m_cadSubmitSamples || d > m_cadSubmitMaxNs) m_cadSubmitMaxNs = d;
                    m_cadSubmitSamples++;
                }
                m_cadPrevSubmitNs = fs.submitPcNs;
            }
        }

        if (m_paused) {
            lastFrameIndex = fs.frameIndex;
            lastFrameNs = now;
            InterlockedExchange(&fs.state, rcvr::kSlotFree);
            motion.Reset();
            lastMotionFrameIndex = 0;
            motionSkipPending = false;
            // A18: the hysteresis state follows the estimator's - the next frame compares
            // against nothing, so every block starts over from still.
            m_motionGridPrev.clear();
            m_motionResults.Clear();
            if (!streaming) {
                streaming = true;
                m_streaming = true;
                m_onState("running", ProducerName(producer), app);
            }
            continue;
        }

        ComPtr<ID3D11Texture2D> texRef = m_bridge->SlotTexture(slot);
        ID3D11Texture2D* tex = texRef.Get();
        if (!tex) {
            WarnEvery5s("VR frame dropped: the frame's texture isn't available");
            InterlockedExchange(&fs.state, rcvr::kSlotFree);
            continue;
        }
        D3D11_TEXTURE2D_DESC desc;
        tex->GetDesc(&desc);
        if (!firstFrameLogged) {
            firstFrameLogged = true;
            RC_LOGI("VR: first frame from %s (%ux%u, format %u)", ProducerName(producer), desc.Width, desc.Height,
                    unsigned(desc.Format));
        }
        if (!convertersReady || srcWidth != (desc.Width & ~1u) || srcHeight != (desc.Height & ~1u)) {
            srcWidth = desc.Width & ~1u;
            srcHeight = desc.Height & ~1u;
            // ROUND18 L2: encWidth/Height are what the ENCODER sees; srcWidth/Height
            // stay the render size (displayed on the headset, and logged separately).
            // encodeScale is applied to the final encode size, after foveation, so a
            // single rule covers both paths. NV12/P010 need even dimensions.
            {
                const uint32_t baseW = foveated ? m_config.foveatedEyeWidth * 2 : srcWidth;
                const uint32_t baseH = foveated ? m_config.foveatedEyeHeight : srcHeight;
                // ROUND18 L2-FIX2: same helper as the foveation/coded-size metadata,
                // so what we tell the headset is byte-for-byte what the encoder sees.
                encWidth = rc::ScaledEncodeSize(baseW, m_config.encodeScale);
                encHeight = rc::ScaledEncodeSize(baseH, m_config.encodeScale);
            }
            m_statSrcW.store(srcWidth, std::memory_order_relaxed);
            m_statSrcH.store(srcHeight, std::memory_order_relaxed);
            convertersReady = false;
            if (foveated && !foveator.Init(device, srcWidth / 2, srcHeight, encWidth / 2, encHeight, m_config.foveaSize, m_config.tenBit)) {
                WarnEvery5s("VR frame dropped: foveated encoding didn't start");
                InterlockedExchange(&fs.state, rcvr::kSlotFree);
                continue;
            }
            bool ok = true;
            for (auto& c : converters) ok = ok && c.Init(device, encWidth, encHeight, m_config.tenBit, true);
            if (!ok) {
                WarnEvery5s("VR frame dropped: the colour converter didn't start");
                InterlockedExchange(&fs.state, rcvr::kSlotFree);
                continue;
            }
            convertersReady = true;
            motionViews.clear();
            motionReady = m_config.motionField && motion.Init(device, srcWidth, srcHeight);
            m_motionHasPendingRead.store(false, std::memory_order_release);
            lastMotionFrameIndex = 0;
            motionSkipPending = false;
            motionPose = rcvr::BridgePose{};
            motionPreviousTimestamp = 0;
            motionPrevSubmitNs = 0;
            motionLastIntervalNs = 0;
            if (m_config.motionField && !motionReady) RC_LOGW("Frame generation: the motion estimator didn't start");
            // A18: a new estimator geometry invalidates the old grid's blocks - the hysteresis
            // state restarts from all-still with it (the next AppendMotion resizes it fresh).
            m_motionGridPrev.clear();
            m_motionResults.Clear();
        }
        ComPtr<IDXGIKeyedMutex> mutex;
        tex->QueryInterface(IID_PPV_ARGS(&mutex));
        // G10.3: time the keyed-mutex acquire (observation only - timeout and the
        // success/failure decision are untouched). A long acquire is the direct sign
        // the driver still holds the shared texture, which used to be invisible.
        const int64_t lockWaitStart = rc::NowNs();
        const bool lockOk = !mutex || rcvr::AcquireKeyed(mutex.Get(), 100);
        // A83 phase M (reading only): this frame's OWN acquire wait, for the flush line -
        // the 10 s avg/max below cannot attribute a single late frame. Timeout (100) and
        // the success decision untouched. -1 = no keyed mutex, printed n/a.
        int64_t frameLockWaitNs = -1;
        if (mutex) {
            const int64_t waited = rc::NowNs() - lockWaitStart;
            frameLockWaitNs = waited;
            ++m_diagLockSamples;
            m_diagLockSumNs += waited;
            if (waited > m_diagLockMaxNs) m_diagLockMaxNs = waited;
            m_diagLock1s.Add(waited);
        }
        if (!lockOk) {
            WarnEvery5s("VR frame dropped: the driver kept the frame locked for over 100 ms");
            InterlockedExchange(&fs.state, rcvr::kSlotFree);
            continue;
        }
        // On a narrow path a clean image at half the frames beats a smeared one at all of
        // them: the headset keeps reprojecting the last frame to the current head pose.
        const uint32_t target = m_sender.TargetBitrateKbps();
        const double bitsPerPixel = target * 1000.0 / (double(encWidth) * encHeight * m_config.fps);
        const bool halfRateBefore = halfRateState.active;
        halfRateState = rc::StepHalfRate(halfRateState, bitsPerPixel);
        const bool halfRate = halfRateState.active;
        if (halfRate != halfRateBefore)
            RC_LOGI("VR half-rate %s: effective target %u kbps, bpp %.4f (%ux%u @ %.0f nominal fps)",
                    halfRate ? "ON" : "OFF", target, bitsPerPixel, encWidth, encHeight, double(m_config.fps));
        // Publish for the 1 s timeline; this is a copy of the existing decision, not a new one.
        m_halfRateActive.store(halfRate, std::memory_order_relaxed);
        if (halfRateState.skipNext) {
            if (asyncMotion) motionSkipPending = true;
            if (mutex) mutex->ReleaseSync(0);
            lastFrameIndex = fs.frameIndex;
            InterlockedExchange(&fs.state, rcvr::kSlotFree);
            continue;
        }

        // A delayed field must stay per-frame; only the deliberate half-rate skip may span two.
        if (asyncMotion && motionReady && lastMotionFrameIndex &&
            rc::ShouldResetMotionPrevious(fs.frameIndex, lastMotionFrameIndex, motionSkipPending)) {
            motion.ResetPrevious();
            lastMotionFrameIndex = 0;
        }
        motionSkipPending = false;

        // A target neither the encoder nor the frame waiting for it uses.
        ColorConverter* converter = nullptr;
        {
            std::lock_guard lock(m_jobMutex);
            for (auto& c : converters)
                if (c.Output() != m_encoding && (!m_job || c.Output() != m_job->yuv.Get())) converter = &c;
        }
        const int64_t convertStart = rc::NowNs();
        bool motionValid = false;
        // A delayed field keeps the source interval it measured, including a half-rate two-frame gap.
        uint64_t motionIntervalNs = 0;
        bool motionQueued = false;
        if (converter && motionReady) {
            ComPtr<ID3D11ShaderResourceView>& view = motionViews[tex];
            if (!view) {
                D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
                sd.Format = desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS   ? DXGI_FORMAT_R8G8B8A8_UNORM
                            : desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS ? DXGI_FORMAT_B8G8R8A8_UNORM
                                                                           : desc.Format;
                sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                sd.Texture2D.MipLevels = 1;
                device->CreateShaderResourceView(tex, &sd, &view);
            }
            if (view) {
                float turn[9];
                HeadTurn(motionPose, fs.renderPose, turn);
                motionIntervalNs = rc::ResolveMotionIntervalNs(fs.headsetTargetNs, motionPreviousTimestamp,
                                                               fs.submitPcNs, motionPrevSubmitNs, motionLastIntervalNs);
                motionValid = motion.Process(ctx.Get(), view.Get(), turn, fs.fovTan);
                if (motionValid && asyncMotion) {
                    const rc::MotionReadbackTiming timing{fs.headsetTargetNs, motionIntervalNs, fs.frameIndex};
                    if (!motion.QueueRead(ctx.Get(), timing))
                        WarnEvery5s("VR motion readback unavailable");
                    else {
                        motionQueued = true;
                        ctx->Flush();
                    }
                }
                motionPose = fs.renderPose;
                lastMotionFrameIndex = fs.frameIndex;
                motionPreviousTimestamp = fs.headsetTargetNs;
                motionPrevSubmitNs = fs.submitPcNs;
                if (motionIntervalNs) motionLastIntervalNs = motionIntervalNs;
            }
        }
        float foveaStart[2][2] = {};
        if (foveated) PlaceFovea(fs.fovTan, foveaStart);
        const bool converted =
            converter && (foveated ? foveator.Squeeze(ctx.Get(), tex, foveaStart) && converter->Convert(ctx.Get(), foveator.Output())
                                   : converter->Convert(ctx.Get(), tex));
        if (mutex) mutex->ReleaseSync(0);
        // The encoder is on another thread: hand the GPU the conversion now.
        ctx->Flush();
        m_motionHasPendingRead.store(motion.HasPendingRead(), std::memory_order_release);
        const int64_t convertBlockNs = rc::NowNs() - convertStart;
        m_diagConvertBlock.Add(convertBlockNs);

        const rcvr::FrameSlot slotCopy = fs;
        InterlockedExchange(&fs.state, rcvr::kSlotFree);
        lastFrameIndex = slotCopy.frameIndex;
        lastFrameNs = rc::NowNs();
        if (!converted) {
            WarnEvery5s("VR frame dropped: colour conversion failed");
            continue;
        }

        // The shared bridge keeps only the newest tracking sample. A rendered frame can
        // carry an older sequence, so recover its target from the bridge's bounded cache.
        int64_t trackedDisplayPcNs = 0;
        bool trackingClockValid = false;
        const bool trackingHit =
            m_bridge->LookupTracking(slotCopy.trackingSequence, trackedDisplayPcNs, trackingClockValid);
        const rc::PredictedDisplay prediction = rc::SelectPredictedDisplay(
            trackingHit, trackedDisplayPcNs, trackingClockValid, slotCopy.submitPcNs,
            kStaleClockGuardNs, kStalePredictAheadNs);

        EncodeJob job;
        job.yuv = converter->Output();
        job.format = converter->Format();
        job.width = encWidth;
        job.height = encHeight;
        job.srcWidth = srcWidth;
        job.srcHeight = srcHeight;
        job.halfRate = halfRate;
        job.foveated = foveated;
        job.motionQueued = motionQueued;
        std::memcpy(job.foveaStart, foveaStart, sizeof(foveaStart));
        job.frameIndex = slotCopy.frameIndex;
        job.headsetTargetNs = slotCopy.headsetTargetNs;
        job.submitPcNs = slotCopy.submitPcNs;
        job.predictedDisplayPcNs = prediction.pcNs;
        // A75: the flush judges both prediction sources with the same grace; the source
        // and cache/clock state travel with the frame only to explain a later drop line.
        job.predictedDisplayExact = prediction.exact;
        job.predictedDisplayClockValid = prediction.clockValid;
        job.predictedDisplayTrackHit = prediction.trackHit;
        std::memcpy(job.renderOrientation, slotCopy.renderPose.orientation, sizeof(job.renderOrientation));
        std::memcpy(job.renderPosition, slotCopy.renderPose.position, sizeof(job.renderPosition));
        std::memcpy(job.fovTan, slotCopy.fovTan, sizeof(job.fovTan));
        // The async path never waits for a field: a miss sends explicit zero motion.
        int64_t frameMotionReadNs = -1;
        if (motionValid) {
            const int64_t motionReadStart = rc::NowNs();
            bool motionRead = false;
            rc::MotionReadbackTiming motionTiming;
            [[maybe_unused]] bool slotConsumed = false;
            if (asyncMotion) {
                motionRead = motion.TryRead(ctx.Get(), motionField, motionTiming, slotConsumed);
                if (!motionRead) ++m_diagMotionPending;
            } else {
                motionRead = motion.Read(ctx.Get(), motionField);
                motionTiming = {slotCopy.headsetTargetNs, motionIntervalNs, slotCopy.frameIndex};
            }
            frameMotionReadNs = rc::NowNs() - motionReadStart;
            m_diagMotionRead.Add(frameMotionReadNs);
            const bool publishedMotion = rc::PublishMotionField(m_motionResults, motionRead, motionTiming, uint16_t(motion.GridWidth()),
                                                                 uint16_t(motion.GridHeight()), uint16_t(MotionEstimator::kBlockSize), motionField,
                                                                 m_motionGridPrev, uint64_t(rc::NowNs()));
            (void)pollPendingMotion();
            if (publishedMotion) {
                std::lock_guard lock(m_motionSyncMutex);
                m_motionSyncCv.notify_all();
            }
        }
        const int64_t convertNs = rc::NowNs() - convertStart;
        m_flowConvertNs += convertNs;
        convertAvgNs = convertAvgNs ? (convertAvgNs * 7 + convertNs) / 8 : convertNs;
        // A83 phase M (reading only): this frame's age over the segment A79's jobAge cannot
        // see - driver submit (driver.cpp:846) until m_job entry. rc::NowNs() and
        // rcvr::QpcNowNs() (vr_bridge.h:231) are the same QPC conversion, so the
        // cross-process subtraction holds; kept signed so a negative falsifies it at
        // runtime. submitPcNs == 0 -> -1 (n/a), out of the statistics.
        const int64_t jobEnterNs = rc::NowNs();
        const int64_t submitToJobNs = slotCopy.submitPcNs ? jobEnterNs - slotCopy.submitPcNs : -1;
        job.submitToJobNs = submitToJobNs;
        job.lockWaitNs = frameLockWaitNs;
        job.holdNs = frameHoldNs;
        job.convertBlockNs = convertBlockNs;
        job.motionReadNs = frameMotionReadNs;
        if (submitToJobNs >= 0) {
            m_cadSubmitToJobSumNs += submitToJobNs;
            if (!m_cadSubmitToJobSamples || submitToJobNs > m_cadSubmitToJobMaxNs)
                m_cadSubmitToJobMaxNs = submitToJobNs;
            m_cadSubmitToJobSamples++;
        }
        {
            std::lock_guard lock(m_jobMutex);
            // The encoder hasn't taken the one before: this newer frame replaces it.
            if (m_job) {
                m_flowReplacedQueue++;
                m_diagJobRepl++;
            }
            m_job = std::move(job);
            // A79 phase M: stamp the slot-entry moment at the slot's only write point
            // (the lock is the existing one; the diag reader never takes it). A83: taken
            // once above, so submitToJob ends exactly where jobAge begins.
            m_jobSinceNs.store(jobEnterNs, std::memory_order_relaxed);
        }
        m_jobReady.notify_one();

        if (!streaming) {
            streaming = true;
            m_streaming = true;
            m_onState("running", ProducerName(m_bridge->ActiveProducer()), m_bridge->ApplicationName());
        }
    }
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

void VrStream::RunEncoder() {
    ScopedCoInit com;
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (mmcss) AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_HIGH);
    // Same relative GPU priority the conversion thread uses: the sampler showed the
    // NVENC engine at 13% while frames waited ~7 ms in queue - the encode thread was
    // last in line for GPU time. Failure here is cosmetic (logged once).
    {
        static std::once_flag logged;
        ComPtr<IDXGIDevice> dxgi;
        const HRESULT hr = SUCCEEDED(m_bridge->Device()->QueryInterface(IID_PPV_ARGS(&dxgi))) ? dxgi->SetGPUThreadPriority(7) : E_NOINTERFACE;
        std::call_once(logged, [hr] { RC_LOGI("VR: GPU priority for encoding %s", SUCCEEDED(hr) ? "raised" : "unchanged (refused)"); });
    }

    ID3D11Device* device = m_bridge->Device();
    rcvr::Shared* shared = m_bridge->Shared();
    std::unique_ptr<VideoEncoder> encoder;
    uint32_t encWidth = 0, encHeight = 0;
    uint32_t srcW = 0, srcH = 0;
    DXGI_FORMAT encFormat = DXGI_FORMAT_UNKNOWN;
    uint32_t appliedKbps = 0;
    rc::StreamProfile appliedProfile = rc::StreamProfile::Balanced;
    int64_t profileAttemptNs = 0;
    EncodedFrame out;
    std::vector<uint8_t> frameMeta;
    while (m_running) {
        EncodeJob job;
        {
            std::unique_lock lock(m_jobMutex);
            m_encoding = nullptr;
            m_jobReady.wait(lock, [this] { return !m_running || m_job.has_value(); });
            if (!m_running) break;
            job = std::move(*m_job);
            m_job.reset();
            m_jobSinceNs.store(0, std::memory_order_relaxed);  // A79 phase M: slot empty again
            m_encoding = job.yuv.Get();
            m_encodeStartNs = rc::NowNs();
        }
        m_jobTaken.notify_one();
        // A62: the NVENC backend caches nvEncRegisterResource per input texture pointer.
        // Those registrations live exactly as long as the converter outputs do, so the
        // encoder must be rebuilt whenever the converters were rebuilt - and they are
        // rebuilt on any SOURCE size change, even when the encoded size stays the same
        // (foveated sizes are derived from the config and round to 16 px). Without this,
        // a rebuilt texture that lands on a freed address is handed to NVENC as its dead
        // predecessor's registration.
        const rc::StreamProfile liveProfile = rc::ProfileFromId(m_liveProfile.load(std::memory_order_relaxed));
        if (!encoder || encWidth != job.width || encHeight != job.height || encFormat != job.format ||
            srcW != job.srcWidth || srcH != job.srcHeight) {
            encWidth = job.width;
            encHeight = job.height;
            encFormat = job.format;
            srcW = job.srcWidth;
            srcH = job.srcHeight;
            EncoderConfig ec;
            ec.codec = m_config.codec;
            ec.width = encWidth;
            ec.height = encHeight;
            ec.fps = m_config.fps;
            ec.bitrateKbps = m_sender.TargetBitrateKbps();
            ec.profile = liveProfile;
            ec.tenBit = m_config.tenBit;
            ec.inputFormat = encFormat;
            // Dual-track foveated encoding: Foveator geometric squeeze reduces edge pixels,
            // while NVENC/AMF QP map simultaneously focuses encoder bit budget onto the fovea.
            ec.foveated = m_config.foveated;
            ec.adaptiveQuant = m_config.adaptiveQuant;
            ec.twoPass = m_config.twoPass;
            ec.sliceCount = (m_config.sliceCount > 1 && ec.bitrateKbps >= 40000) ? m_config.sliceCount : 1;
            ec.cappedVbr = m_config.cappedVbr;
            ec.fillMode = m_config.fillMode;
            encoder = CreateEncoder(device, m_bridge->VendorId(), ec);
            // The stats path reads these from any thread; the encoder is local to
            // RunEncoder, so its QP and size travel out through atomics.
            m_statEncW.store(ec.width, std::memory_order_relaxed);
            m_statEncH.store(ec.height, std::memory_order_relaxed);
            appliedKbps = ec.bitrateKbps;
            appliedProfile = liveProfile;
            m_sender.RequestKeyframe();
            if (!encoder) {
                WarnEvery5s("VR frame dropped: the encoder didn't start");
                rc::SleepMs(500);
                continue;
            }
        }

        // A87/A99: the live rung owns both encoder RC parameters. Apply them together so
        // Quality's deeper VBV cannot lag behind its QP floor. Refusals are retried at
        // most once a second, and an internal rebuild above starts at the same live rung.
        const int64_t profileNow = rc::NowNs();
        if (liveProfile != appliedProfile && (!profileAttemptNs || profileNow - profileAttemptNs >= 1'000'000'000LL)) {
            profileAttemptNs = profileNow;
            if (encoder->SetProfile(liveProfile)) {
                appliedProfile = liveProfile;
            } else {
                const auto q = rc::ProfileParams(liveProfile);
                RC_LOGW("VR: encoder refused profile rung %d (QP floor %u, VBV %u%%); keeping the old profile, no rebuild",
                        m_liveProfile.load(std::memory_order_relaxed), q.minQp, q.vbvFramesPercent);
            }
        }

        // Half rate pays nothing extra here. The encoder tracks the interval between the
        // frames it actually sees (the slow EMA in nvenc_encoder reconfigures frameRateNum
        // to the real rate), so halving the frames already doubles the per-frame budget by
        // itself. Doubling the bitrate as well paid it twice: whenever the guard sat low
        // enough to trip half rate, the encoder produced twice what the pacer was allowed
        // to send (17.5 -> 35 Mb/s against a 26 Mb/s pacer) and the frame rate fell to
        // 29-33 fps. The handoff is bounded anyway - RunEncoder waits for the previous
        // SendJob to be taken - so 2x output would only queue against a 1x wire.
        const uint32_t target = m_sender.TargetBitrateKbps();
        const uint32_t encoderKbps = target;
        if (encoderKbps != appliedKbps) {
            encoder->SetBitrate(encoderKbps);
            appliedKbps = encoderKbps;
        }
        if (m_config.foveated) encoder->SetFoveation(job.fovTan);
        const bool keyframe = m_sender.BeginFrame(*encoder);
        const int64_t encStart = rc::NowNs();
        if (!encoder->Encode(job.yuv.Get(), keyframe, out)) {
            WarnEvery5s("VR frame dropped: encoding failed; restarting the encoder");
            encoder.reset();
            continue;
        }
        const int64_t encEnd = rc::NowNs();
        const bool recovery = m_sender.TakeRecovery();
        m_flowEncoded++;
        m_statQp.store(encoder->AverageQp(), std::memory_order_relaxed);
        m_flowEncodeNs += encEnd - encStart;
        const int64_t encodeAvg = m_encodeAvgNs;
        m_encodeAvgNs = encodeAvg ? (encodeAvg * 7 + (encEnd - encStart)) / 8 : encEnd - encStart;

        rc::VrFrameMetadata meta{};
        meta.trackingTimestampNs = job.headsetTargetNs;
        std::memcpy(meta.renderPose.orientation, job.renderOrientation, sizeof(job.renderOrientation));
        std::memcpy(meta.renderPose.position, job.renderPosition, sizeof(job.renderPosition));
        std::memcpy(meta.fovTan, job.fovTan, sizeof(meta.fovTan));
        meta.ipd = shared->config.ipd;
        meta.eyeWidth = job.srcWidth / 2;
        meta.eyeHeight = job.srcHeight;
        meta.pcSubmitNs = job.submitPcNs;
        meta.pcEncodeStartNs = encStart;
        meta.pcEncodeEndNs = encEnd;
        frameMeta.assign(reinterpret_cast<const uint8_t*>(&meta), reinterpret_cast<const uint8_t*>(&meta) + sizeof(meta));
        if (job.foveated) {
            rc::VrFoveationMetadata fm{};
            std::memcpy(fm.foveaStart, job.foveaStart, sizeof(fm.foveaStart));
            frameMeta.insert(frameMeta.end(), reinterpret_cast<const uint8_t*>(&fm), reinterpret_cast<const uint8_t*>(&fm) + sizeof(fm));
        }
        if (m_config.motionField && job.motionQueued) WaitForMotionField(job.frameIndex, job.headsetTargetNs);
        bool motionRejectedOversize = false;
        rc::AttachMotionField(m_config.motionField, m_motionResults, job.frameIndex, job.headsetTargetNs, frameMeta,
                              &motionRejectedOversize);
        if (motionRejectedOversize)
            WarnEvery5s("VR motion field omitted: metadata exceeds the uint16 wire limit");
        m_diagMotionRequests.fetch_add(1, std::memory_order_release);
        SendJob send;
        send.frame = std::move(out);
        send.meta = std::move(frameMeta);
        send.flags = rc::kVideoHasPose | (m_config.tenBit ? rc::kVideoHdr10 : 0);
        send.encodeMs = rc::NsToMs(encEnd - encStart);
        // A55: the producer counts in 64 bits, the wire's diag seq field is 32 bits by design
        // (it is a wrapping sequence number). Say so instead of letting the compiler warn.
        send.frameIndex = static_cast<uint32_t>(job.frameIndex);  // ROUND19 T1: pass the existing counter through
        send.recovery = recovery;
        send.predictedDisplayPcNs = job.predictedDisplayPcNs;  // A26: the flush gate judges this
        send.predictedDisplayExact = job.predictedDisplayExact;  // A75: the flush log line names it
        send.predictedDisplayClockValid = job.predictedDisplayClockValid;
        send.predictedDisplayTrackHit = job.predictedDisplayTrackHit;
        send.submitToJobNs = job.submitToJobNs;  // A83 phase M: the flush line reports this frame's own upstream ages
        send.lockWaitNs = job.lockWaitNs;
        send.holdNs = job.holdNs;
        send.convertBlockNs = job.convertBlockNs;
        send.motionReadNs = job.motionReadNs;
        {
            // The one before still going out (a paced send): wait, a dropped frame would break
            // the ones referencing it.
            const int64_t sendWaitStart = rc::NowNs();
            std::unique_lock lock(m_sendMutex);
            m_sendCv.wait(lock, [this] { return !m_running || !m_send; });
            if (!m_running) break;
            send.sendWaitNs = rc::NowNs() - sendWaitStart;
            const uint32_t idx = uint32_t(m_diagSendWaitIdx.fetch_add(1, std::memory_order_relaxed) % kDiagRing);
            m_diagSendWaitUs[idx].store(uint32_t(send.sendWaitNs / 1000), std::memory_order_relaxed);
            m_diagSendWaitCount.fetch_add(1, std::memory_order_relaxed);
            m_send = std::move(send);
            // A79 phase M: stamp the send-slot entry moment at the slot's only fill point.
            m_sendSinceNs.store(rc::NowNs(), std::memory_order_relaxed);
        }
        m_sendCv.notify_all();
        out = {};
        frameMeta.clear();
    }
    {
        std::lock_guard lock(m_jobMutex);
        m_encoding = nullptr;
    }
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

void VrStream::RunSender() {
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (mmcss) AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_HIGH);
    bool firstSent = false;
    while (m_running) {
        // A87/A99: VideoSender::m_guard is not locked, so the tier's new queue-brake
        // line is installed here on the sender's own thread. The encoder thread reads
        // the same persistent live rung for QP/VBV; neither path stops the transport.
        const int pendingProfile = m_pendingProfile.exchange(-1, std::memory_order_relaxed);
        if (pendingProfile >= 0) {
            m_sender.SetProfile(rc::ProfileFromId(pendingProfile));
            // A93/A99: the encoder reports the QP/VBV half on its own thread. This
            // line confirms the sender actually consumed the queued brake change.
            RC_LOGI("VR: queue brake retargeted in place to rung %d (target %.0f ms, no stream rebuild)",
                    pendingProfile, rc::ProfileParams(rc::ProfileFromId(pendingProfile)).targetQueueMs);
        }
        SendJob send;
        {
            std::unique_lock lock(m_sendMutex);
            m_sendCv.wait(lock, [this] { return !m_running || m_send.has_value(); });
            if (!m_running) break;
            send = std::move(*m_send);
            m_send.reset();
            m_sendSinceNs.store(0, std::memory_order_relaxed);  // A79 phase M: slot empty again
        }
        m_sendCv.notify_all();
        // A26 (stale-frame flush) + A72 (the chain rule) + A75 (the grace period): a frame
        // whose display moment passed more than rc::kFlushGraceNs ago is late no matter how
        // fast it goes out, and a frame whose references were dropped with it decodes
        // against a stale picture. So: drop the expired one, and while the chain is broken
        // let only repair frames (IDR / recovery) leave - freezing on the last good picture
        // beats one more artifact. The debt wipe, the two throttles and the log line are
        // A26's, unchanged. In the steady state nothing here fires: normal frames' display
        // moment sits in the future, and the 0-8 ms of slip the pre-A75 rule turned into
        // runs of 8/14/16 dropped frames is inside the grace by design.
        const int64_t nowNs = rc::NowNs();
        const rc::FlushGate gate =
            rc::StaleFlushGate(send.frame.keyframe || send.recovery, send.predictedDisplayPcNs,
                               nowNs, m_staleFlush);
        if (!gate.send) {
            m_flowStaleDrops.fetch_add(1, std::memory_order_relaxed);
            if (gate.requestKeyframe) m_sender.RequestKeyframe();
            // The old billing lives in the packetizer and is unknown here; the wipe is
            // unconditional, so the argument is irrelevant (rc/stale_flush.h).
            if (gate.resetDebt) m_sender.ResetPacingDebt(rc::FlushSendDebtNs(0, nowNs));
            m_staleLogCount++;
            if (gate.log) {
                // A72 review: a drop is either late (A26's reason) or held back because the
                // frames it references were dropped with it. Calling the second one "display
                // moment had passed" sends the next reader chasing a clock fault that never
                // happened, and "keyframe requested" was a lie whenever the throttle held.
                const bool late = rc::FrameExpiredForSend(send.predictedDisplayPcNs, nowNs);
                // A75 (TASK-A75-FLUSH-GRACE-2026-11-22): the moment has two sources - the
                // headset's exact target mapped into the PC clock, or the submit + 40 ms
                // predict-ahead estimate - and a "0-8 ms late" batch means a different bug
                // in each. One appended word says which produced the number this line
                // reports; the rule it was judged by, and the grace in it, are the same.
                // A79 phase M (reading only): the same four pacing/age readouts the 1 s
                // diag line carries, measured at the flush instant itself, appended after
                // the leading fields (byte-for-byte preserved; no parser is end-anchored
                // on this line). sendBacklog here reflects the LAST SENT frame's billing
                // anchor - the debt wipe above writes the raw send-thread clock, and the
                // snapshot is deliberately only published at the packetizer's one anchor
                // point (video_transport.cpp), so a wiped backlog reads as what it was.
                const int64_t flushJobSince = m_jobSinceNs.load(std::memory_order_relaxed);
                const int64_t flushSendSince = m_sendSinceNs.load(std::memory_order_relaxed);
                // A83 phase M (reading only): the dropped frame's OWN upstream values -
                // the diag window averages hide single-shot events. n/a: submitPcNs was 0
                // / no keyed mutex. Appended at the END; no parser matches this line.
                const auto fmtMs = [](char* b, size_t n, int64_t v) {
                    if (v >= 0) snprintf(b, n, "%.1f ms", double(v) / 1e6);
                    else snprintf(b, n, "n/a");
                };
                char s2jPart[32], lwPart[32], holdPart[32], convertPart[32], motionPart[32], sendWaitPart[32];
                fmtMs(s2jPart, sizeof(s2jPart), send.submitToJobNs);
                fmtMs(lwPart, sizeof(lwPart), send.lockWaitNs);
                fmtMs(holdPart, sizeof(holdPart), send.holdNs);
                fmtMs(convertPart, sizeof(convertPart), send.convertBlockNs);
                fmtMs(motionPart, sizeof(motionPart), send.motionReadNs);
                fmtMs(sendWaitPart, sizeof(sendWaitPart), send.sendWaitNs);
                RC_LOGI("VR stale flush: dropped %u frame(s) since the last line; last one %s "
                        "(%lld ms from its display moment, %s); pacer debt wiped, repair %s"
                        "; sendBacklog %.1f ms, overruns %u, jobAge %.1f ms, sendAge %.1f ms"
                        ", submitToJob %s, lockWait %s, hold %s, convertBlock %s, motionRead %s, sendWait %s"
                        ", clockValid %s, trackHit %s",
                        m_staleLogCount, late ? "expired" : "held, chain broken",
                        (long long)((nowNs - send.predictedDisplayPcNs) / 1'000'000),
                        send.predictedDisplayExact ? "exact" : "estimated",
                        gate.requestKeyframe ? "requested" : "already pending",
                        double(m_sender.SendBacklogNs()) / 1e6, m_sender.BudgetOverruns(),
                        flushJobSince ? double(nowNs - flushJobSince) / 1e6 : -1.0,
                        flushSendSince ? double(nowNs - flushSendSince) / 1e6 : -1.0,
                        s2jPart, lwPart, holdPart, convertPart, motionPart, sendWaitPart,
                        send.predictedDisplayClockValid ? "yes" : "no",
                        send.predictedDisplayTrackHit ? "yes" : "no");
                m_staleLogCount = 0;
            }
            if (gate.breaker)
                RC_LOGW("VR stale flush circuit breaker: chain broken, %u consecutive frame(s) "
                        "dropped; forcing a repair request instead of sending a broken frame",
                        rc::kStaleFlushBreakerDrops);
            continue;
        }
        // Last line of defence for the wire's uint16 metadata length.  AttachMotionField()
        // already gates the motion payload, but if the protocol ever grows the BASE metadata
        // past 65535 B (motionField off ⇒ no gate runs), uint16_t() would silently truncate
        // and emit a corrupt frame.  Drop it instead and throttle the warning; wire format
        // and byte order are untouched.
        if (send.meta.size() > static_cast<size_t>(std::numeric_limits<uint16_t>::max())) {
            static int64_t s_metaOversizeWarnNs = 0;
            const int64_t warnNow = rc::NowNs();
            if (!s_metaOversizeWarnNs || warnNow - s_metaOversizeWarnNs >= 5'000'000'000LL) {
                s_metaOversizeWarnNs = warnNow;
                RC_LOGW("VR metadata oversize: dropped frame %u (%zu bytes > 65535); "
                        "check base metadata growth / motion gate", send.frameIndex, send.meta.size());
                // A dropped encoded frame breaks the decoder's reference chain, so ask for a
                // repair frame - but once per throttle window, not per dropped frame.  While
                // the base metadata stays oversize every frame is dropped for the same
                // reason, and a per-frame request would force a continuous IDR stream that
                // is thrown away unencoded-to-wire anyway (keyframe storm).
                m_sender.RequestKeyframe();
            }
            continue;
        }
        m_sender.Send(send.frame, send.meta.data(), uint16_t(send.meta.size()), send.flags, send.encodeMs, send.recovery);
        // ROUND19 T1: feed the diag window (telemetry only; relaxed stores, the same cost
        // class as the flow counters). The CAS makes the window's first seq its minimum.
        {
            uint32_t expected = 0;
            m_diagSeqMin.compare_exchange_strong(expected, send.frameIndex, std::memory_order_relaxed);
            m_diagSeqMax.store(send.frameIndex, std::memory_order_relaxed);
            const uint32_t idx = uint32_t(m_diagEncIdx.fetch_add(1, std::memory_order_relaxed) % kDiagRing);
            m_diagEncUs[idx].store(uint32_t(send.encodeMs * 1000.0 + 0.5), std::memory_order_relaxed);
            m_diagSeqRing[idx].store(send.frameIndex, std::memory_order_relaxed);
            m_diagEncCount.fetch_add(1, std::memory_order_relaxed);
        }
        if (!firstSent) {
            firstSent = true;
            RC_LOGI("VR: frames are reaching the headset");
        }
    }
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

}  // namespace rcs
