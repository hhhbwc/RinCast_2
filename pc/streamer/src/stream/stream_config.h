// The settings -> stream-config synthesis, exposed as pure functions
// (TASK-COMBO-SWEEP-2026-11-21). The bodies are moved VERBATIM out of
// ClientSession::StartVr / StartDesktop / ChooseCodec / RunAutoTune: no store,
// no bridge, no logging, no device - the session keeps the I/O and the logs,
// everything that decides the config lives here so the offline core tests can
// sweep every switch combination. Header-only: no link dependency for the
// test target, exactly like the constexpr helpers in settings.h.
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

#include "../settings.h"
#include "../session/client_session.h"  // ClientCaps, ClientStreamSettings
#include "desktop_stream.h"
#include "foveator.h"
#include "rc/stream_profile.h"
#include "vr_stream.h"

namespace rcs {

// Moved verbatim from client_session.cpp's anonymous namespace so both the
// session and the offline sweep share one definition.
inline uint32_t Align(uint32_t v, uint32_t a) { return std::max(a, (v + a / 2) / a * a); }

inline float QualityScale(const std::string& q) {
    if (q == "low") return 0.6f;
    if (q == "medium") return 0.75f;
    if (q == "ultra") return 1.0f;
    if (q == "godlike") return 1.2f;
    return 0.9f;  // high
}

// The streamer-side codec resolution, moved verbatim from ClientSession::ChooseCodec.
// The caller keeps providing the live caps snapshots and the resolved preference
// (a "auto" headset choice resolves against the PC's saved codec before this call).
inline rc::VideoCodec ChooseStreamCodec(const EncoderCaps& enc, const ClientCaps& dec, bool tenBit,
                                        const std::string& pref) {
    auto ok = [&](rc::VideoCodec c) {
        switch (c) {
            case rc::VideoCodec::H264: return enc.h264 && dec.h264 && !tenBit;
            case rc::VideoCodec::HEVC: return enc.hevc && dec.hevc && (!tenBit || (enc.hevc10 && dec.hevc10));
            case rc::VideoCodec::AV1: return enc.av1 && dec.av1 && (!tenBit || (enc.av110 && dec.av110));
        }
        return false;
    };
    if (pref == "av1" && ok(rc::VideoCodec::AV1)) return rc::VideoCodec::AV1;
    if (pref == "h264" && (enc.h264 && dec.h264)) return rc::VideoCodec::H264;
    if (pref == "hevc" && ok(rc::VideoCodec::HEVC)) return rc::VideoCodec::HEVC;
    // Auto: HEVC is the best latency/quality trade-off on Quest; AV1 when HEVC is missing.
    if (ok(rc::VideoCodec::HEVC)) return rc::VideoCodec::HEVC;
    if (ok(rc::VideoCodec::AV1)) return rc::VideoCodec::AV1;
    return rc::VideoCodec::H264;
}

// What StartVr synthesizes. The coded sizes are what the kStreamConfig message
// tells the headset (ROUND18 L2-FIX2): one value feeds the foveation metadata,
// the coded-size hint and the log.
struct VrStreamBuild {
    VrStreamConfig cfg;
    uint32_t codedEyeWidth = 0;   // encoded PER-EYE width
    uint32_t codedEyeHeight = 0;  // encoded PER-EYE height
    // TASK-CAPS-GUARD: true when the resolved H264 stream exceeded NVENC's 4096 px
    // full-frame width and was rebuilt on HEVC; h264RejectedWidth is the full-frame
    // width the H264 build would have produced (the session logs it).
    bool h264WidthFallback = false;
    uint32_t h264RejectedWidth = 0;
};

// Moved verbatim from ClientSession::StartVr (the VrStreamConfig fill block).
// `autoscalePct` is the L7 ladder's current Pct() (the session reads it under its
// mutex); `match` is StartVr's own st.matchSteamVr && !slowPath.
inline VrStreamBuild BuildVrStreamConfig(const Settings& pc, const ClientStreamSettings& st,
                                         rc::VideoCodec codec, bool slowPath, bool match, int autoscalePct,
                                         uint32_t eyeWidth, uint32_t eyeHeight, float fps) {
    VrStreamBuild out;
    VrStreamConfig& cfg = out.cfg;
    cfg.codec = codec;
    cfg.tenBit = st.tenBit && codec != rc::VideoCodec::H264;
    cfg.eyeWidth = eyeWidth;
    cfg.eyeHeight = eyeHeight;
    cfg.fps = fps;
    cfg.bitrateKbps = uint32_t(rcs::EffectiveVrBitrate(pc.vrBitrateMbps, st.vrBitrateMbps, pc.pcBitrateWins) * 1000);
    if (slowPath) cfg.bitrateKbps = std::min<uint32_t>(cfg.bitrateKbps, 60000);
    cfg.fecPercent = slowPath ? std::max(pc.fecPercent, 12) : pc.fecPercent;
    // PLAN-pacing-fix WP2b: the legacy "pace = true" made the pacer follow the guard's
    // cap, which self-locked. The user's ceiling drives the pacer now, and a frame
    // leaves within its own frame period. A remote path keeps the legacy.
    if (slowPath || pc.videoPacingMode == 2) cfg.pacingMode = 2;
    else if (pc.videoPacingMode == 1) cfg.pacingMode = 1;
    else cfg.pacingMode = 3;  // burst: the LAN default
    cfg.pace = cfg.pacingMode != 1;
    cfg.adaptive = slowPath || (st.adaptiveBitrate && pc.adaptiveBitrate);
    cfg.profile = rc::ProfileFromId(pc.streamProfile);
    // ROUND18 L2: the user's encode scale overrides the profile's value (both 1.0
    // today). 0 = follow the profile. The percentage is stored as an int for the UI.
    cfg.encodeScale = pc.encodeScalePct > 0 ? rc::ClampEncodeScale(pc.encodeScalePct / 100.0f)
                                            : rc::ProfileParams(cfg.profile).encodeScale;
    // ROUND19 T3: the autoscale ladder multiplies the base above (never raises it);
    // the level survives until the user's manual reset. The frame size itself still
    // comes out of ScaledEncodeSize, the one formula shared with the foveation config.
    if (pc.l7Autoscale) {
        cfg.encodeScale = rc::ClampEncodeScale(cfg.encodeScale * autoscalePct / 100.0f);
    }
    // ROUND20 L17: the headset's force_encode_scale debug switch overrides the composed
    // scale when armed (a diagnostics switch, not a user setting).
    if (st.debugEncodeScalePct > 0)
        cfg.encodeScale = rc::ClampEncodeScale(st.debugEncodeScalePct / 100.0f);
    if (slowPath) cfg.startBitrateKbps = std::min<uint32_t>(cfg.bitrateKbps, 12000);
    // QP map is the user's own switch, just like two-pass. A profile no longer
    // turns it on behind their back. The encoder runs it only when the headset's
    // geometric squeeze is off (vr_stream.cpp gates it on !job.foveated) - the two
    // cannot share a frame - so the page says so when that squeeze is on.
    cfg.foveated = pc.foveatedEncoding;
    cfg.adaptiveQuant = pc.adaptiveQuantization;
    // Two-pass is the user's own switch, never forced by a profile. The Best-picture
    // profile used to turn it on by itself (and the page greyed it out), which
    // surprised users who had deliberately left it off; the user decides.
    cfg.twoPass = pc.twoPassEncoding;
    cfg.sliceCount = (pc.vrSlicedEncoding && cfg.bitrateKbps >= 40000)
                         ? (codec == rc::VideoCodec::AV1 ? 2 : 4)
                         : 1;
    // A86b: CBR is a plain independent opt-in switch, never a side effect of the
    // tier auto happens to be on (that coupling oscillated - see settings.h).
    cfg.cappedVbr = !rcs::QualityProfileCbr(pc.qualityProfileCbr);
    cfg.fillMode = pc.fillMode;
    cfg.motionField = st.frameGeneration;
    // ROUND18 L2-FIX2: the real encoded PER-EYE size after encodeScale. One value
    // feeds the foveation metadata, the coded-size hint and the log, so the headset
    // can never undo foveation with a different density than the encoder used.
    // Start from the plain (un-foveated) base; the foveated block overrides it.
    uint32_t codedEyeW = rc::ScaledEncodeSize(cfg.eyeWidth * 2, cfg.encodeScale) / 2;
    uint32_t codedEyeH = rc::ScaledEncodeSize(cfg.eyeHeight, cfg.encodeScale);
    const bool externalEyes = pc.externalEyeTracking;
    const bool effectiveFoveate = st.gazeFoveation || externalEyes;
    const bool effectiveEyesTracked = st.eyesTracked || externalEyes;
    if (effectiveFoveate && cfg.encodeScale >= 1.0f) {
        const float defaultFovea = effectiveEyesTracked ? Foveator::kFoveaSize : Foveator::kFixedFoveaSize;
        cfg.foveaSize = pc.foveaSize > 0.05f ? pc.foveaSize : defaultFovea;
        // When the user lets the profiles drive video settings, each one floors the
        // full-density centre at its own share (latency 0.6, balanced 0.7, quality 0.8)
        // and only ever raises; matched-SteamVR sizes and the slow internet path pick
        // their own. This runs BEFORE the EncodedSize calls, so the sizes encoded - and
        // the share sent to the headset - come from the same value.
        // A86b: foveation no longer follows a (now invisible) rung.
        cfg.foveaSize = QualityProfileFoveaFor(cfg.foveaSize);
        const float periphery = pc.foveaPeriphery > 0.05f ? pc.foveaPeriphery : 0.50f;
        cfg.foveatedEyeWidth = Foveator::EncodedSize(cfg.eyeWidth, cfg.foveaSize, periphery);
        cfg.foveatedEyeHeight = Foveator::EncodedSize(cfg.eyeHeight, cfg.foveaSize, periphery);
        codedEyeW = rc::ScaledEncodeSize(cfg.foveatedEyeWidth * 2, cfg.encodeScale) / 2;
        codedEyeH = rc::ScaledEncodeSize(cfg.foveatedEyeHeight, cfg.encodeScale);
    }
    // (a scaled frame cannot carry foveation: the sizes above already hold the plain
    // scaled size and foveatedEyeWidth/Height stay 0 - vr_stream.cpp's else branch.)
    out.codedEyeWidth = codedEyeW;
    out.codedEyeHeight = codedEyeH;
    return out;
}

// TASK-CAPS-GUARD: NVENC's H264 tops out at a 4096 px full-frame width - larger
// frames fail encoder init ("Frame Dimension greater than the maximum supported
// value", measured on the A14 matrix; the MF fallback fails the same way), so a
// session built on such a config never comes up. When the synthesized H264 config
// would exceed it, re-resolve the codec exactly like ChooseCodec's caps fallback
// (HEVC next, the same ok(HEVC) rule) and rebuild from the same inputs: HEVC's own
// limit is 8192, so the tier the user asked for survives. Nothing else moves - the
// rebuild re-derives every field with the rule it always used, tenBit included.
// When HEVC itself is not resolvable (caps / tenBit), the config is returned as-is
// and the encoder init reports the real problem; the fallback only ever acts on a
// path that would otherwise have failed. The caller logs it (this header stays
// log-free), hence h264WidthFallback / h264RejectedWidth.
inline VrStreamBuild BuildVrStreamConfigGuarded(const Settings& pc, const ClientStreamSettings& st,
                                                rc::VideoCodec codec, bool slowPath, bool match, int autoscalePct,
                                                uint32_t eyeWidth, uint32_t eyeHeight, float fps,
                                                const EncoderCaps& enc, const ClientCaps& dec) {
    VrStreamBuild out = BuildVrStreamConfig(pc, st, codec, slowPath, match, autoscalePct, eyeWidth, eyeHeight, fps);
    const uint32_t fullWidth = out.codedEyeWidth * 2;  // the side-by-side frame the encoder is asked for
    if (out.cfg.codec == rc::VideoCodec::H264 && fullWidth > 4096 && enc.hevc && dec.hevc &&
        (!st.tenBit || (enc.hevc10 && dec.hevc10))) {
        out = BuildVrStreamConfig(pc, st, rc::VideoCodec::HEVC, slowPath, match, autoscalePct, eyeWidth, eyeHeight, fps);
        out.h264WidthFallback = true;
        out.h264RejectedWidth = fullWidth;
    }
    return out;
}

// The desktop stream's fps: the headset's pick, else the PC's, else the panel
// refresh; clamped. Moved verbatim from ClientSession::StartDesktop.
inline float ResolveDesktopFps(const Settings& pc, const ClientStreamSettings& st) {
    float fps = st.desktopFps > 0 ? float(st.desktopFps) : (pc.desktopFps > 0 ? float(pc.desktopFps) : st.refreshRate);
    return std::clamp(fps, 30.f, 120.f);
}

// One monitor's desktop stream config. Moved verbatim from ClientSession::StartDesktop
// (the per-monitor fill; the monitor selection, stream ids and stream creation stay
// in the session). `vrInFlight` is the session's m_vr != nullptr, `monitorCount` the
// number of selected monitors (the slow-path start bitrate splits across them).
inline DesktopStreamConfig BuildDesktopStreamConfig(const Settings& pc, const ClientStreamSettings& st,
                                                    rc::VideoCodec codec, bool slowPath, bool vrInFlight,
                                                    const MonitorInfo& mon, double totalPixels, int monitorCount,
                                                    const ClientCaps& caps) {
    const int totalMbps = st.desktopBitrateMbps > 0 ? st.desktopBitrateMbps : pc.desktopBitrateMbps;
    const uint32_t maxW = std::min<uint32_t>(caps.maxDecodeWidth, codec == rc::VideoCodec::H264 ? 4096 : 8192);
    const uint32_t maxH = std::min<uint32_t>(caps.maxDecodeHeight, codec == rc::VideoCodec::H264 ? 4096 : 8192);
    DesktopStreamConfig cfg;
    cfg.cheapEncode = vrInFlight;  // VR in flight: the desktop skips two-pass
    cfg.codec = codec;
    cfg.tenBit = st.tenBit && codec != rc::VideoCodec::H264;
    double w = mon.width, h = mon.height;
    double scale = 1.0;
    if (pc.desktopMaxHeight > 0 && h > pc.desktopMaxHeight) scale = pc.desktopMaxHeight / h;
    scale = std::min({scale, maxW / w, maxH / h});
    cfg.width = Align(uint32_t(w * scale), 2);
    cfg.height = Align(uint32_t(h * scale), 2);
    cfg.fps = ResolveDesktopFps(pc, st);
    cfg.bitrateKbps = uint32_t(totalMbps * 1000.0 * (double(mon.width) * mon.height / totalPixels));
    cfg.bitrateKbps = std::max<uint32_t>(cfg.bitrateKbps, 5000);
    // Internet paths lose packets and have no retransmissions: recover more of them locally.
    cfg.fecPercent = slowPath ? std::max(pc.fecPercent, 12) : pc.fecPercent;
    // The desktop stream keeps the legacy pacing for now (PLAN-pacing-fix §7).
    cfg.pace = true;
    cfg.adaptiveQuant = pc.adaptiveQuantization;
    // Over the internet a fixed bitrate only fills queues or loses frames: always adapt.
    cfg.adaptive = slowPath || (st.adaptiveBitrate && pc.adaptiveBitrate);
    cfg.profile = rc::ProfileFromId(pc.streamProfile);
    // Over the internet the upload is often far below the LAN bitrate: start low and
    // let the bitrate control find what the path carries, instead of filling queues.
    if (slowPath)
        cfg.startBitrateKbps = std::min<uint32_t>(cfg.bitrateKbps, std::max<uint32_t>(3000, 8000 / uint32_t(monitorCount)));
    return cfg;
}

// The decision layer's per-session input, moved verbatim from
// ClientSession::RunAutoTune (the AutoTuneInput fill).
//
// A84b §3 (TASK-A84b): the `pcOwnsProfile` parameter this took is GONE. It used to
// pin in.profileId to 0 whenever the PC did not own the synced streamProfile key,
// which is PLAN-AUTO-TUNE's "a profile the headset owns is not auto's to move".
// A84 made that guard both unnecessary and wrong, and passing it `true` to keep the
// build compiling left a parameter nothing could turn off - the third state the
// card forbids. The evidence for deleting it rather than restoring the guard:
//   - the guard protected the headset's VALUE of the synced key, and auto can no
//     longer write that key at all: both profile actions move session state only
//     (ClientSession::m_autoProfileEff) and rebuild through OnStreamProfileChanged,
//     which only enqueues a restart (client_session.cpp). Whatever the headset last
//     sent stays in the settings file untouched;
//   - the headset's choice is not ignored either - it IS the input now. The PC
//     adopts a headset-owned streamProfile into its settings
//     (AdoptHeadsetOwnedSettings), and RunAutoTune feeds that same number as
//     AutoTuneInput::profileFloorId, so the headset's pick is the floor auto works
//     above and must climb back to;
//   - restoring the old form would switch the feature off on exactly the machine it
//     is for (the headset slider is the tuning surface since A85, so the PC does not
//     own the key there), and it would corrupt the rung: in.profileId is the RUNG
//     the live stream is built at, and 0 is a lie about it whenever the stream runs
//     at 1 or 2 - the climb would then step up from 0 and the cut would land on a
//     rung the stream is not at.
inline rc::AutoTuneInput BuildAutoTuneInput(const Settings& pc, const ClientStreamSettings& st,
                                             const VrStreamConfig& built) {
    rc::AutoTuneInput in;
    in.cbrOn = rcs::QualityProfileCbr(pc.qualityProfileCbr);
    const bool squeeze = built.foveatedEyeWidth > 0 && built.foveatedEyeHeight > 0;
    in.foveaOn = squeeze;
    // External eyes already provide geometric foveation, but their QP Map path is
    // mutually exclusive; only headset gaze can make AutoTune's QP Map action valid.
    in.foveaPossible = st.gazeFoveation && !pc.externalEyeTracking;
    // A84b §3 above: `pc` is the EFFECTIVE snapshot (see ClientSession::RunAutoTune),
    // so this is the rung the live streams are built at - the only thing auto may move.
    in.profileId = pc.streamProfile;
    return in;
}

}  // namespace rcs
