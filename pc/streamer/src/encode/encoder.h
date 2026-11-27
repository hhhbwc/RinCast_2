// Hardware video encoder abstraction (NVENC, AMF, Media Foundation).
#pragma once

#include <d3d11_4.h>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "../util.h"
#include "rc/protocol.h"
#include "rc/stream_profile.h"

namespace rcs {

struct EncoderConfig {
    rc::VideoCodec codec = rc::VideoCodec::HEVC;
    uint32_t width = 0;
    uint32_t height = 0;
    float fps = 72;  // ROUND18 L4 default (was 90)
    uint32_t bitrateKbps = 100000;
    bool tenBit = false;
    // Desktop content: favour sharpness (more bits for I-frames, slower preset).
    bool desktop = false;
    DXGI_FORMAT inputFormat = DXGI_FORMAT_B8G8R8A8_UNORM;  // or R8G8B8A8_UNORM
    // VR: spend fewer bits towards the lens edges, where the Quest's optics blur the
    // image anyway (needs SetFoveation; NVENC only).
    bool foveated = false;
    // Use every NVENC engine for one frame on GPUs that have more than one (HEVC/AV1).
    bool splitFrame = true;
    // Number of horizontal slices per frame (1 = single slice; 4 = parallel decode).
    int sliceCount = 1;
    // More bits for flat areas (skies, walls), where blocking shows first; fewer for busy
    // detail that hides it. NVENC and AMF.
    bool adaptiveQuant = true;
    // A quick first pass over the frame before the real one, so rate control knows where
    // the detail is. Costs encode time. NVENC and AMF.
    bool twoPass = false;
    // What to favour: latency / balanced / picture quality. Decides the encoder's
    // spending policy (capped VBR) and, on the sender side, the queue budget.
    rc::StreamProfile profile = rc::StreamProfile::Balanced;
    // CBR is kept only for A/B benchmarks and as a fallback when the driver says
    // it has no VBR. Production uses capped VBR, where bitrateKbps is a ceiling.
    bool cappedVbr = true;
    // L17 fill mode: opt-in frame-rate-rung follow-down (floor 45). Default off:
    // the frozen D-034 rate plan runs unchanged. See rc/fps_ladder.h.
    bool fillMode = false;
    // QP floor: -1 = take the profile's value, 0 = off, >0 = override. This is
    // what makes a still picture cheap without starving a complex one.
    int vbrMinQp = -1;
    // Bench-only overrides: 0 = the built-in choice (VR = P1 + ultra-low-latency,
    // desktop = P4 + low-latency), so the streaming paths always stay 0 and behave
    // exactly as before. 1..7 = NVENC preset P1..P7 (slower = better quality);
    // tuning 1 = low latency, 2 = high quality. encode-bench sets these to price a
    // slower preset against the frame budget; production code never does.
    int preset = 0;
    int tuning = 0;
    // 0 = use the profile's value. Lets the benchmark answer "what if the average
    // target stayed at the ceiling and only the QP floor did the saving?".
    uint32_t vbrAveragePercent = 0;
};

struct EncodedFrame {
    std::vector<uint8_t> data;
    bool keyframe = false;
    uint64_t timestamp = 0;  // encoder-side id, for InvalidateAfter
};

// The numbers the Media Foundation backend writes into its codec API and media type
// (mf_encoder.cpp Init). Derived here, verbatim, so the offline config sweep
// (TASK-COMBO-SWEEP-2026-11-21 §1.8) can construct them for every switch combination
// without a device or an MFT.
struct MfEncoderParams {
    uint32_t meanBitRate = 0;   // CODECAPI_AVEncCommonMeanBitRate / MF_MT_AVG_BITRATE
    uint32_t bufferSize = 0;    // CODECAPI_AVEncCommonBufferSize (one frame at the rate; desktop 2 frames)
    uint32_t frameRateNum = 0;  // MF_MT_FRAME_RATE numerator (x1000; denominator fixed 1000)
};

inline MfEncoderParams DeriveMfParams(const EncoderConfig& c) {
    MfEncoderParams p;
    // A47: keep a 64-bit intermediate so an abnormal ceiling cannot wrap before
    // narrowing into the MF UINT32 rate field (callers validate <= 4,000,000 kbps).
    p.meanBitRate = uint32_t(uint64_t(c.bitrateKbps) * 1000);
    // A47: CODECAPI_AVEncCommonBufferSize is BYTES per frame, not bits. The old
    // kbps*1000/fps was bits/frame -> an 8x too-large VBV buffer (8 frames instead
    // of 1, 16 in desktop mode), raising rate-control latency and burst jitter.
    // safeFps removes the fps<=0 divide-by-zero -> inf/uint32 UB.
    const double safeFps = c.fps > 0.0f ? double(c.fps) : 72.0;
    p.bufferSize = uint32_t(double(c.bitrateKbps) * 1000.0 / 8.0 / safeFps * (c.desktop ? 2.0 : 1.0));
    // A47: round instead of truncate, so 59.94 fps -> 59940 (was 59939).
    p.frameRateNum = uint32_t(std::lround(double(c.fps) * 1000.0));
    return p;
}

class VideoEncoder {
   public:
    virtual ~VideoEncoder() = default;
    virtual bool Init(ID3D11Device* device, const EncoderConfig& config) = 0;
    // Encodes `input` (same size/format as configured). Synchronous: returns
    // once the bitstream is available.
    virtual bool Encode(ID3D11Texture2D* input, bool forceKeyframe, EncodedFrame& out) = 0;
    virtual void SetBitrate(uint32_t kbps) = 0;
    // Retune both the QP floor and the profile's VBV on the live session. False means
    // this backend cannot apply the profile in place; the caller keeps streaming and
    // reports the refusal. A later internal rebuild still uses the caller's live profile.
    virtual bool SetProfile(rc::StreamProfile /*profile*/) { return false; }
    virtual const char* Name() const = 0;
    // The QP the last finished 5 s window actually ran at, -1 when unknown. Lets the
    // UI show "was the ceiling spent, and at what quality" without opening logs.
    virtual float AverageQp() const { return -1.0f; }
    // Loss recovery without a keyframe: marks every frame encoded after `timestamp`
    // as unusable, so the next frame only references `timestamp` or older. Returns
    // false if that isn't possible (the caller then forces a keyframe).
    virtual bool InvalidateAfter(uint64_t /*timestamp*/) { return false; }
    // Side-by-side VR eyes: per-eye left, right, up, down tangents of the field of view.
    virtual void SetFoveation(const float /*fovTan*/[2][4]) {}
    // Dual-track dynamic foveation on an already Foveator-squeezed frame: maps the full-density
    // rectangle [a, a + foveaSize] into encoded-frame CTU coordinates and scales delta-QP by bitrate.
    virtual void SetDynamicFoveation(const float /*foveaStart*/[2][2], float /*foveaSize*/,
                                     float /*densityX*/, float /*densityY*/, uint32_t /*bitrateKbps*/) {}
    const EncoderConfig& Config() const { return m_config; }

   protected:
    EncoderConfig m_config;
};

struct EncoderCaps {
    std::string backend;  // nvenc | amf | mf
    bool h264 = false;
    bool hevc = false;
    bool av1 = false;
    bool hevc10 = false;
    bool av110 = false;
    uint32_t maxWidth = 4096;
    uint32_t maxHeight = 4096;
    // A52: the adapter has a hardware encoder but it could not be opened (old driver, encoder
    // busy, GPU in a VM). We still stream on Media Foundation, but the user needs to know why
    // it feels slow - this is the difference between "my PC is weak" and "my driver is old".
    bool hwFallback = false;
};

// Probes what the adapter's hardware encoder supports.
EncoderCaps ProbeEncoderCaps(ID3D11Device* device, UINT vendorId);

// Creates the best encoder for the adapter: NVENC on NVIDIA, AMF on AMD,
// Media Foundation (Intel QSV and fallback) otherwise.
std::unique_ptr<VideoEncoder> CreateEncoder(ID3D11Device* device, UINT vendorId, const EncoderConfig& config);

std::unique_ptr<VideoEncoder> CreateNvencEncoder();
std::unique_ptr<VideoEncoder> CreateAmfEncoder();
std::unique_ptr<VideoEncoder> CreateMfEncoder();
bool NvencProbe(ID3D11Device* device, EncoderCaps& caps);
bool AmfProbe(ID3D11Device* device, EncoderCaps& caps);
bool MfProbe(ID3D11Device* device, EncoderCaps& caps);

const char* CodecName(rc::VideoCodec codec);

}  // namespace rcs
