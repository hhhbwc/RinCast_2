// NVIDIA NVENC (Video Codec SDK 13 API, loaded dynamically from the driver).
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <vector>

#include "encoder.h"
#include "nvenc/nvEncodeAPI.h"
#include "rc/fovea_qp.h"
#include "rc/fps_ladder.h"
#include "rc/platform.h"
#include "rc/stream_profile.h"

namespace rcs {

namespace {

using CreateInstanceFn = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
using MaxVersionFn = NVENCSTATUS(NVENCAPI*)(uint32_t*);

bool CadenceFillExperiment() {
    static const bool enabled = [] {
        char text[2]{};
        return GetEnvironmentVariableA("RINCAST_CADENCE_FILL", text, sizeof(text)) == 1 && text[0] == '1';
    }();
    return enabled;
}

struct NvencLib {
    HMODULE module = nullptr;
    NV_ENCODE_API_FUNCTION_LIST api{};
    bool ok = false;

    static NvencLib& Get() {
        static NvencLib lib = [] {
            NvencLib l;
            l.module = LoadLibraryW(L"nvEncodeAPI64.dll");
            if (!l.module) return l;
            auto maxVersion = reinterpret_cast<MaxVersionFn>(GetProcAddress(l.module, "NvEncodeAPIGetMaxSupportedVersion"));
            auto create = reinterpret_cast<CreateInstanceFn>(GetProcAddress(l.module, "NvEncodeAPICreateInstance"));
            if (!maxVersion || !create) return l;
            uint32_t version = 0;
            maxVersion(&version);
            const uint32_t required = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
            if (version < required) {
                RC_LOGW("NVENC driver API %u.%u is older than required %u.%u; update the NVIDIA driver", version >> 4,
                        version & 15, NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION);
                return l;
            }
            l.api.version = NV_ENCODE_API_FUNCTION_LIST_VER;
            l.ok = create(&l.api) == NV_ENC_SUCCESS;
            return l;
        }();
        return lib;
    }
};

GUID CodecGuid(rc::VideoCodec codec) {
    switch (codec) {
        case rc::VideoCodec::H264: return NV_ENC_CODEC_H264_GUID;
        case rc::VideoCodec::HEVC: return NV_ENC_CODEC_HEVC_GUID;
        case rc::VideoCodec::AV1: return NV_ENC_CODEC_AV1_GUID;
    }
    return NV_ENC_CODEC_HEVC_GUID;
}

void* OpenSession(ID3D11Device* device) {
    auto& lib = NvencLib::Get();
    if (!lib.ok) return nullptr;
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params{};
    params.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    params.device = device;
    params.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    params.apiVersion = NVENCAPI_VERSION;
    void* encoder = nullptr;
    if (lib.api.nvEncOpenEncodeSessionEx(&params, &encoder) != NV_ENC_SUCCESS) return nullptr;
    return encoder;
}

int GetCap(void* encoder, GUID codec, NV_ENC_CAPS cap) {
    NV_ENC_CAPS_PARAM p{};
    p.version = NV_ENC_CAPS_PARAM_VER;
    p.capsToQuery = cap;
    int value = 0;
    NvencLib::Get().api.nvEncGetEncodeCaps(encoder, codec, &p, &value);
    return value;
}

class NvencEncoder final : public VideoEncoder {
   public:
    ~NvencEncoder() override { Destroy(); }

    const char* Name() const override { return "NVENC"; }
    // The rolling 5 s window the ENC 5s line already summarises - read, never reset here.
    // Invalid NVENC samples never enter this mean; no valid sample => -1 (unknown).
    float AverageQp() const override { return m_qpStats.Mean(); }

    bool Init(ID3D11Device* device, const EncoderConfig& config) override {
        m_config = config;
        m_fillGate.Reset();
        m_device = device;
        auto& api = NvencLib::Get().api;
        m_encoder = OpenSession(device);
        if (!m_encoder) {
            RC_LOGE("NVENC: could not open session");
            return false;
        }
        const GUID codec = CodecGuid(config.codec);
        // Detect GPU architecture for automatic preset optimization
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC desc{};
        bool isModernRtx = false;
        if (device && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) &&
            SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) &&
            SUCCEEDED(adapter->GetDesc(&desc))) {
            const wchar_t* name = desc.Description;
            if (wcsstr(name, L"RTX") != nullptr) {
                isModernRtx = true;
            }
        }
        // Preset and tuning: built-in is the streaming choice.
        // On modern RTX GPUs (RTX 30/40/50 series), auto-upgrade VR preset to P3 (Fast) for higher quality.
        static const GUID* const kPresets[] = {
            nullptr,
            &NV_ENC_PRESET_P1_GUID, &NV_ENC_PRESET_P2_GUID, &NV_ENC_PRESET_P3_GUID,
            &NV_ENC_PRESET_P4_GUID, &NV_ENC_PRESET_P5_GUID, &NV_ENC_PRESET_P6_GUID,
            &NV_ENC_PRESET_P7_GUID,
        };
        const GUID autoPreset = config.desktop ? NV_ENC_PRESET_P4_GUID
                                               : (isModernRtx ? NV_ENC_PRESET_P3_GUID : NV_ENC_PRESET_P1_GUID);
        const GUID preset = config.preset > 0 && config.preset <= 7 ? *kPresets[config.preset] : autoPreset;

        const NV_ENC_TUNING_INFO autoTuning = config.desktop ? NV_ENC_TUNING_INFO_LOW_LATENCY
                                                             : (isModernRtx ? NV_ENC_TUNING_INFO_LOW_LATENCY
                                                                            : NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY);
        const NV_ENC_TUNING_INFO tuning =
            config.tuning == 1   ? NV_ENC_TUNING_INFO_LOW_LATENCY
            : config.tuning == 2 ? NV_ENC_TUNING_INFO_HIGH_QUALITY
                                 : autoTuning;

        NV_ENC_PRESET_CONFIG presetConfig{};
        presetConfig.version = NV_ENC_PRESET_CONFIG_VER;
        presetConfig.presetCfg.version = NV_ENC_CONFIG_VER;
        if (api.nvEncGetEncodePresetConfigEx(m_encoder, codec, preset, tuning, &presetConfig) != NV_ENC_SUCCESS) {
            RC_LOGE("NVENC: preset query failed (codec unsupported?)");
            return false;
        }
        m_encodeConfig = presetConfig.presetCfg;
        m_encodeConfig.version = NV_ENC_CONFIG_VER;
        m_encodeConfig.gopLength = NVENC_INFINITE_GOPLENGTH;
        m_encodeConfig.frameIntervalP = 1;
        auto& rc = m_encodeConfig.rcParams;
        // CBR spent the whole budget even on a still menu: rate control drove QP down
        // until the frame was big enough, and the needless bytes queued on the way to
        // the headset. VBR with maxBitRate == the ceiling lets a simple frame cost what
        // it costs and still lets a complex one use the full budget. See stream_profile.h.
        m_cappedVbr = config.cappedVbr;
        rc.rateControlMode = m_cappedVbr ? NV_ENC_PARAMS_RC_VBR : NV_ENC_PARAMS_RC_CBR;
        // Keep single-pass by default for VR to prevent 2-3ms extra encode latency on 4K+ dual eye buffers.
        rc.multiPass = config.twoPass ? NV_ENC_TWO_PASS_QUARTER_RESOLUTION : NV_ENC_MULTI_PASS_DISABLED;
        ApplyBitrate(config.bitrateKbps);
        rc.zeroReorderDelay = 1;
        rc.enableAQ = (config.adaptiveQuant || isModernRtx) ? 1 : 0;  // strength 0: the driver picks it
        // The QP floor is the "spend less on a simple picture" half of capped VBR.
        // Rate control cannot lower QP past it, so a still menu stays small even
        // though averageBitRate still offers the whole ceiling to a complex frame.
        const int minQp = config.vbrMinQp < 0 ? int(rc::ProfileParams(config.profile).minQp) : config.vbrMinQp;
        if (m_cappedVbr && minQp > 0) {
            rc.enableMinQP = 1;
            const uint32_t q = std::clamp<uint32_t>(uint32_t(minQp), 1u, 51u);
            rc.minQP.qpInterP = rc.minQP.qpInterB = rc.minQP.qpIntra = q;
        }

        // Loss recovery: keep a few older frames as references so a loss can be answered
        // by invalidating the lost ones instead of sending a keyframe (5-10x the size).
        // H.264 decoders on Android cope badly with the resulting frame-number gaps.
        m_canInvalidate = config.codec != rc::VideoCodec::H264 &&
                          GetCap(m_encoder, codec, NV_ENC_CAPS_SUPPORT_REF_PIC_INVALIDATION) != 0;
        const int engines = GetCap(m_encoder, codec, NV_ENC_CAPS_NUM_ENCODER_ENGINES);
        // Two or more NVENC engines encode one frame in strips together: roughly halves
        // the encode time of large VR frames.
        // Strips cost compression, so not when bits are scarce.
        const bool split = config.splitFrame && engines > 1 && config.bitrateKbps >= 30000 && config.codec != rc::VideoCodec::H264 &&
                           uint64_t(config.width) * config.height >= 2560ull * 1440;
        const bool useSlices = config.sliceCount > 1 && !config.desktop && config.bitrateKbps >= 40000 && !split;
        m_blockSize = config.codec == rc::VideoCodec::H264 ? 16 : config.codec == rc::VideoCodec::HEVC ? 32 : 64;
        if (config.foveated) rc.qpMapMode = NV_ENC_QP_MAP_DELTA;

        const NV_ENC_BIT_DEPTH depth = config.tenBit ? NV_ENC_BIT_DEPTH_10 : NV_ENC_BIT_DEPTH_8;
        switch (config.codec) {
            case rc::VideoCodec::H264: {
                auto& h = m_encodeConfig.encodeCodecConfig.h264Config;
                h.repeatSPSPPS = 1;
                h.idrPeriod = NVENC_INFINITE_GOPLENGTH;
                h.sliceMode = useSlices ? 3 : 0;
                h.sliceModeData = useSlices ? uint32_t(config.sliceCount) : 0;
                h.h264VUIParameters.videoSignalTypePresentFlag = 1;
                h.h264VUIParameters.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED;
                h.h264VUIParameters.videoFullRangeFlag = 0;
                h.h264VUIParameters.colourDescriptionPresentFlag = 1;
                h.h264VUIParameters.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
                h.h264VUIParameters.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
                h.h264VUIParameters.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT709;
                m_encodeConfig.profileGUID = NV_ENC_H264_PROFILE_HIGH_GUID;
                break;
            }
            case rc::VideoCodec::HEVC: {
                auto& h = m_encodeConfig.encodeCodecConfig.hevcConfig;
                h.repeatSPSPPS = 1;
                h.idrPeriod = NVENC_INFINITE_GOPLENGTH;
                h.sliceMode = useSlices ? 3 : 0;
                h.sliceModeData = useSlices ? uint32_t(config.sliceCount) : 0;
                h.inputBitDepth = depth;
                h.outputBitDepth = depth;
                if (config.foveated) h.maxCUSize = NV_ENC_HEVC_CUSIZE_32x32;  // the QP map is per 32x32 CTB
                if (m_canInvalidate) {
                    h.maxNumRefFramesInDPB = kDpbFrames;
                    h.numRefL0 = NV_ENC_NUM_REF_FRAMES_1;
                }
                h.hevcVUIParameters.videoSignalTypePresentFlag = 1;
                h.hevcVUIParameters.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED;
                h.hevcVUIParameters.videoFullRangeFlag = 0;
                h.hevcVUIParameters.colourDescriptionPresentFlag = 1;
                h.hevcVUIParameters.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
                h.hevcVUIParameters.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
                h.hevcVUIParameters.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT709;
                m_encodeConfig.profileGUID = config.tenBit ? NV_ENC_HEVC_PROFILE_MAIN10_GUID : NV_ENC_HEVC_PROFILE_MAIN_GUID;
                break;
            }
            case rc::VideoCodec::AV1: {
                auto& a = m_encodeConfig.encodeCodecConfig.av1Config;
                a.repeatSeqHdr = 1;
                a.idrPeriod = NVENC_INFINITE_GOPLENGTH;
                a.inputBitDepth = depth;
                a.outputBitDepth = depth;
                a.colorPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
                a.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
                a.matrixCoefficients = NV_ENC_VUI_MATRIX_COEFFS_BT709;
                a.colorRange = 0;
                a.chromaFormatIDC = 1;
                if (useSlices) {
                    a.numTileRows = 2;
                    a.numTileColumns = 1;
                }
                if (m_canInvalidate) {
                    a.maxNumRefFramesInDPB = kDpbFrames;
                    a.numFwdRefs = NV_ENC_NUM_REF_FRAMES_1;
                }
                m_encodeConfig.profileGUID = NV_ENC_AV1_PROFILE_MAIN_GUID;
                break;
            }
        }

        m_init = {};
        m_init.version = NV_ENC_INITIALIZE_PARAMS_VER;
        m_init.encodeGUID = codec;
        m_init.presetGUID = preset;
        m_init.tuningInfo = tuning;
        m_init.encodeWidth = m_init.darWidth = m_init.maxEncodeWidth = config.width;
        m_init.encodeHeight = m_init.darHeight = m_init.maxEncodeHeight = config.height;
        m_init.frameRateNum = RatePlan(0).rateNum;
        m_init.frameRateDen = 1000;
        // 启动时用标称分母；运行期由统一事务同步更新分母、码率及 VBV（D-030）。
        m_init.enablePTD = 1;
        m_init.enableEncodeAsync = 0;
        m_init.encodeConfig = &m_encodeConfig;
        m_init.splitEncodeMode = split ? NV_ENC_SPLIT_AUTO_FORCED_MODE : NV_ENC_SPLIT_AUTO_MODE;
        NVENCSTATUS st = api.nvEncInitializeEncoder(m_encoder, &m_init);
        if (st != NV_ENC_SUCCESS && (split || config.foveated)) {
            // Older drivers may reject a combination; the plain configuration still works.
            RC_LOGW("NVENC: initialize with split=%d foveated=%d failed (%d): %s; retrying without", int(split),
                    int(config.foveated), int(st), api.nvEncGetLastErrorString(m_encoder));
            api.nvEncDestroyEncoder(m_encoder);
            m_encoder = OpenSession(device);
            if (!m_encoder) return false;
            m_init.splitEncodeMode = NV_ENC_SPLIT_AUTO_MODE;
            m_encodeConfig.rcParams.qpMapMode = NV_ENC_QP_MAP_DISABLED;
            st = api.nvEncInitializeEncoder(m_encoder, &m_init);
        }
        if (st != NV_ENC_SUCCESS && useSlices) {
            RC_LOGW("NVENC: initialize with %d slices failed (%d): %s; retrying single-slice",
                    config.sliceCount, int(st), api.nvEncGetLastErrorString(m_encoder));
            api.nvEncDestroyEncoder(m_encoder);
            m_encoder = OpenSession(device);
            if (!m_encoder) return false;
            m_encodeConfig.encodeCodecConfig.h264Config.sliceMode = 0;
            m_encodeConfig.encodeCodecConfig.h264Config.sliceModeData = 0;
            m_encodeConfig.encodeCodecConfig.hevcConfig.sliceMode = 0;
            m_encodeConfig.encodeCodecConfig.hevcConfig.sliceModeData = 0;
            m_encodeConfig.encodeCodecConfig.av1Config.numTileRows = 0;
            m_encodeConfig.encodeCodecConfig.av1Config.numTileColumns = 0;
            st = api.nvEncInitializeEncoder(m_encoder, &m_init);
        }
        if (st != NV_ENC_SUCCESS && config.twoPass) {
            RC_LOGW("NVENC: initialize with two passes failed (%d): %s; retrying with one", int(st),
                    api.nvEncGetLastErrorString(m_encoder));
            api.nvEncDestroyEncoder(m_encoder);
            m_encoder = OpenSession(device);
            if (!m_encoder) return false;
            m_encodeConfig.rcParams.multiPass = NV_ENC_MULTI_PASS_DISABLED;
            st = api.nvEncInitializeEncoder(m_encoder, &m_init);
        }
        if (st != NV_ENC_SUCCESS && m_cappedVbr) {
            // A driver that refuses VBR with this preset/codec still works in CBR.
            // Losing the stream over a rate-control mode would be a poor trade.
            RC_LOGW("NVENC: VBR initialize failed (%d): %s; retrying with CBR", int(st),
                    api.nvEncGetLastErrorString(m_encoder));
            api.nvEncDestroyEncoder(m_encoder);
            m_encoder = OpenSession(device);
            if (!m_encoder) return false;
            m_cappedVbr = false;
            m_encodeConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
            m_encodeConfig.rcParams.enableMinQP = 0;
            ApplyBitrate(config.bitrateKbps);
            st = api.nvEncInitializeEncoder(m_encoder, &m_init);
        }
        if (st != NV_ENC_SUCCESS) {
            RC_LOGE("NVENC: initialize failed (%d): %s", int(st), api.nvEncGetLastErrorString(m_encoder));
            return false;
        }
        m_split = m_init.splitEncodeMode == NV_ENC_SPLIT_AUTO_FORCED_MODE;
        m_foveated = m_encodeConfig.rcParams.qpMapMode == NV_ENC_QP_MAP_DELTA;
        NV_ENC_CREATE_BITSTREAM_BUFFER bb{};
        bb.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
        if (api.nvEncCreateBitstreamBuffer(m_encoder, &bb) != NV_ENC_SUCCESS) return false;
        m_bitstream = bb.bitstreamBuffer;
        m_rateState.applied = RatePlan(0);
        const auto budget = m_rateState.applied.budget;
        const std::string qpText = !m_cappedVbr ? std::string("n/a")
                                   : minQp > 0  ? std::to_string(minQp)
                                                : std::string("off");
        const char* presetName = preset == NV_ENC_PRESET_P3_GUID ? ", preset P3 (Fast)"
                                 : preset == NV_ENC_PRESET_P4_GUID ? ", preset P4"
                                 : preset == NV_ENC_PRESET_P1_GUID ? ", preset P1 (Fastest)" : "";
        const int activeSlices =
            config.codec == rc::VideoCodec::H264 && m_encodeConfig.encodeCodecConfig.h264Config.sliceMode == 3
                ? int(m_encodeConfig.encodeCodecConfig.h264Config.sliceModeData)
            : config.codec == rc::VideoCodec::HEVC && m_encodeConfig.encodeCodecConfig.hevcConfig.sliceMode == 3
                ? int(m_encodeConfig.encodeCodecConfig.hevcConfig.sliceModeData)
            : config.codec == rc::VideoCodec::AV1 && m_encodeConfig.encodeCodecConfig.av1Config.numTileRows > 1
                ? int(m_encodeConfig.encodeCodecConfig.av1Config.numTileRows)
                : 1;
        const std::string slicesText = activeSlices > 1 ? (", " + std::to_string(activeSlices) + " slices") : "";
        RC_LOGI("NVENC %s %ux%u @%.0f fps, ceiling %u kbps (%s, avg %u kbps, QP floor %s)%s%s%s%s%s%s%s%s",
                CodecName(config.codec), config.width, config.height, config.fps, config.bitrateKbps,
                m_cappedVbr ? "capped VBR" : "CBR",
                m_cappedVbr ? budget.averageBitrate / 1000 : config.bitrateKbps, qpText.c_str(),
                presetName,
                config.tenBit ? ", 10-bit" : "",
                m_split ? ", split across engines" : "", m_foveated ? ", foveated" : "",
                m_canInvalidate ? ", loss recovery" : "", m_encodeConfig.rcParams.enableAQ ? ", AQ" : "",
                m_encodeConfig.rcParams.multiPass != NV_ENC_MULTI_PASS_DISABLED ? ", two-pass" : "",
                slicesText.c_str());
        return true;
    }

    bool Encode(ID3D11Texture2D* input, bool forceKeyframe, EncodedFrame& out) override {
        TrackFrameInterval();
        auto& api = NvencLib::Get().api;
        NV_ENC_REGISTERED_PTR reg = Register(input);
        if (!reg) return false;
        NV_ENC_MAP_INPUT_RESOURCE map{};
        map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
        map.registeredResource = reg;
        if (api.nvEncMapInputResource(m_encoder, &map) != NV_ENC_SUCCESS) return false;

        NV_ENC_PIC_PARAMS pic{};
        pic.version = NV_ENC_PIC_PARAMS_VER;
        pic.inputWidth = m_config.width;
        pic.inputHeight = m_config.height;
        pic.inputPitch = 0;
        pic.inputBuffer = map.mappedResource;
        pic.bufferFmt = map.mappedBufferFmt;
        pic.outputBitstream = m_bitstream;
        pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
        const uint64_t ts = m_frameCount++;
        pic.inputTimeStamp = ts;
        const bool idr = forceKeyframe || ts == 0;
        if (idr) pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
        if (m_foveated && !m_qpMap.empty()) {
            pic.qpDeltaMap = m_qpMap.data();
            pic.qpDeltaMapSize = uint32_t(m_qpMap.size());
        }
        const int64_t submitStart = rc::NowNs();
        NVENCSTATUS st = api.nvEncEncodePicture(m_encoder, &pic);
        const int64_t submitEnd = rc::NowNs();
        bool ok = st == NV_ENC_SUCCESS;
        if (ok) {
            NV_ENC_LOCK_BITSTREAM lock{};
            lock.version = NV_ENC_LOCK_BITSTREAM_VER;
            lock.outputBitstream = m_bitstream;
            lock.doNotWait = 0;
            if (api.nvEncLockBitstream(m_encoder, &lock) == NV_ENC_SUCCESS) {
                const int64_t lockEnd = rc::NowNs();
                m_statSubmitNs += submitEnd - submitStart;
                m_statGpuWaitNs += lockEnd - submitEnd;
                const auto* p = static_cast<const uint8_t*>(lock.bitstreamBufferPtr);
                out.data.assign(p, p + lock.bitstreamSizeInBytes);
                out.keyframe = lock.pictureType == NV_ENC_PIC_TYPE_IDR || lock.pictureType == NV_ENC_PIC_TYPE_I;
                out.timestamp = ts;
                // An intra frame (also what NVENC produces when an invalidation left
                // nothing to reference) resets what older frames can be recovered from.
                if (out.keyframe) m_lastIntraTs = ts;
                {
                    const int64_t now = rc::NowNs();
                    if (!m_statStartNs) m_statStartNs = now;
                    m_statFrames++;
                    m_qpStats.Add(lock.frameAvgQP, rc::MaxValidQp(int(m_config.codec)));
                    m_statBytes += lock.bitstreamSizeInBytes;
                    if (m_statFrames == 1 || lock.bitstreamSizeInBytes < m_statBytesMin) m_statBytesMin = lock.bitstreamSizeInBytes;
                    if (m_statFrames == 1 || lock.bitstreamSizeInBytes > m_statBytesMax) m_statBytesMax = lock.bitstreamSizeInBytes;
                    const int64_t elapsed = now - m_statStartNs;
                    if (elapsed >= 5 * 1000 * 1000 * 1000LL) {
                        const double secs = double(elapsed) / 1e9;
                        // qp is the mean of VALID samples only (-1.0 = no valid sample),
                        // so a garbage NVENC field can no longer drag it into the billions.
                        // The trailing fields are additive: log parsers key on the leading
                        // fixed-width part; diagnostics get the raw rejected value and the
                        // min..max spread (a steady mean can hide quality pumping).
                        // stream= tells the two encoder instances apart (VR and desktop can
                        // both print this prefix); it reuses the existing config.desktop flag.
                        RC_LOGI("ENC 5s: %.0f fps, qp %.1f, %.1f KB/frame, %.1f Mbps (ceiling %u), submit %.1f ms, gpu wait %.1f ms, qpValid %u/%u, qpFirstBad %u, qpRange %u-%u, kbRange %.1f-%.1f, stream %s",
                                m_statFrames / secs, m_qpStats.Mean(),
                                double(m_statBytes) / m_statFrames / 1024.0,
                                double(m_statBytes) * 8.0 / secs / 1e6, m_config.bitrateKbps / 1000,
                                double(m_statSubmitNs) / m_statFrames / 1e6,
                                double(m_statGpuWaitNs) / m_statFrames / 1e6,
                                m_qpStats.valid, m_qpStats.valid + m_qpStats.rejected,
                                m_qpStats.firstBadRaw, m_qpStats.min, m_qpStats.max,
                                double(m_statBytesMin) / 1024.0, double(m_statBytesMax) / 1024.0,
                                m_config.desktop ? "desktop" : "vr");
                        m_statStartNs = now;
                        m_statFrames = 0;
                        m_qpStats.Reset();
                        m_statBytes = 0;
                        m_statBytesMin = 0;
                        m_statBytesMax = 0;
                        m_statSubmitNs = 0;
                        m_statGpuWaitNs = 0;
                    }
                }
                api.nvEncUnlockBitstream(m_encoder, m_bitstream);
            } else {
                ok = false;
            }
        } else {
            RC_LOGW("NVENC: encode failed (%d)", int(st));
        }
        api.nvEncUnmapInputResource(m_encoder, map.mappedResource);
        return ok;
    }

    void SetBitrate(uint32_t kbps) override {
        // 与下一帧的新鲜 fps 观测合并，避免同一帧先改码率、再改 fps 的两次 API 调用。
        m_requestedKbps = kbps;
    }

    bool SetProfile(rc::StreamProfile profile) override {
        if (!m_encoder || !m_cappedVbr || !m_encodeConfig.rcParams.enableMinQP) return false;
        const auto q = rc::ProfileParams(profile);
        const int64_t now = rc::NowNs();
        const auto plan = RatePlan(profile, now);
        auto candidateConfig = m_encodeConfig;
        const uint32_t qp = std::clamp(q.minQp, 1u, 51u);
        candidateConfig.rcParams.minQP.qpInterP = qp;
        candidateConfig.rcParams.minQP.qpInterB = qp;
        candidateConfig.rcParams.minQP.qpIntra = qp;
        ApplyBudget(candidateConfig, plan);
        candidateConfig.rcParams.vbvInitialDelay =
            rc::ClampVbvInitialDelay(candidateConfig.rcParams.vbvInitialDelay, plan.budget.vbvBufferSize);
        auto candidateInit = m_init;
        candidateInit.frameRateNum = plan.rateNum;
        candidateInit.frameRateDen = 1000;
        candidateInit.encodeConfig = &candidateConfig;
        const bool ok = rc::ApplyEncoderRateTransaction(m_rateState, plan, now, [&] {
            NV_ENC_RECONFIGURE_PARAMS re{};
            re.version = NV_ENC_RECONFIGURE_PARAMS_VER;
            re.reInitEncodeParams = candidateInit;
            return NvencLib::Get().api.nvEncReconfigureEncoder(m_encoder, &re) == NV_ENC_SUCCESS;
        });
        if (!ok) {
            RC_LOGW("NVENC: QP-floor reconfigure to %u refused (%s); keeping profile %d and its old VBV",
                    qp, NvencLib::Get().api.nvEncGetLastErrorString(m_encoder), int(m_config.profile));
            return false;
        }
        m_config.profile = profile;
        m_config.bitrateKbps = plan.requestedKbps;
        m_encodeConfig = candidateConfig;
        m_init = candidateInit;
        m_init.encodeConfig = &m_encodeConfig;
        RC_LOGI("NVENC: QP floor retuned to %u in place (no session reset), profile %d, VBV %.2f Mbit",
                qp, int(profile), double(plan.budget.vbvBufferSize) / 1e6);
        return true;
    }

    bool InvalidateAfter(uint64_t timestamp) override {
        if (!m_encoder || !m_canInvalidate || m_frameCount == 0) return false;
        // The good frame must still be in the DPB, and not from before the last intra frame.
        if (timestamp < m_lastIntraTs || timestamp >= m_frameCount || m_frameCount - 1 - timestamp >= kDpbFrames)
            return false;
        auto& api = NvencLib::Get().api;
        for (uint64_t t = timestamp + 1; t < m_frameCount; t++)
            if (api.nvEncInvalidateRefFrames(m_encoder, t) != NV_ENC_SUCCESS) return false;
        return true;
    }

    void SetFoveation(const float fovTan[2][4]) override {
        if (!m_foveated || std::memcmp(fovTan, m_fov, sizeof(m_fov)) == 0) return;
        std::memcpy(m_fov, fovTan, sizeof(m_fov));
        // QP offset per block from the angle to the eye's optical axis: full quality
        // within ~25 degrees (the sharp part of the lens), ramping to +kMaxDelta (about
        // half the bits; AV1 counts in finer quantizer steps) at 50 degrees and beyond.
        // Rate control keeps the frame size, so the bits saved at the edges sharpen the
        // centre instead.
        const int maxDelta = m_config.codec == rc::VideoCodec::AV1 ? 24 : 6;
        const uint32_t bw = (m_config.width + m_blockSize - 1) / m_blockSize;
        const uint32_t bh = (m_config.height + m_blockSize - 1) / m_blockSize;
        const float eyeW = m_config.width / 2.f;
        m_qpMap.assign(size_t(bw) * bh, 0);
        for (uint32_t by = 0; by < bh; by++) {
            for (uint32_t bx = 0; bx < bw; bx++) {
                const float x = (bx + 0.5f) * m_blockSize, y = (by + 0.5f) * m_blockSize;
                const int eye = x < eyeW ? 0 : 1;
                const float* f = fovTan[eye];
                const float u = std::clamp((x - eye * eyeW) / eyeW, 0.f, 1.f);
                const float v = std::clamp(y / float(m_config.height), 0.f, 1.f);
                const float tx = -f[0] + u * (f[0] + f[1]);
                const float ty = f[2] - v * (f[2] + f[3]);
                const float deg = std::atan(std::sqrt(tx * tx + ty * ty)) * 57.2958f;
                m_qpMap[size_t(by) * bw + bx] = int8_t(rc::FoveaQpDelta(deg, maxDelta));
            }
        }
    }

    void SetDynamicFoveation(const float foveaStart[2][2], float foveaSize,
                             float densityX, float densityY, uint32_t bitrateKbps) override {
        if (!m_foveated || m_config.width == 0 || m_config.height == 0) return;
        // Bitrate-adaptive delta-QP: strongest in 40..100 Mbps bit-starved regime,
        // tapered in 100..160 Mbps, disabled above 160 Mbps to avoid dark-gradient banding.
        const int qpScale = m_config.codec == rc::VideoCodec::AV1 ? 4 : 1;
        int centerDelta = 0, edgeDelta = 0;
        if (bitrateKbps <= 100000) {
            centerDelta = -2 * qpScale;
            edgeDelta = 5 * qpScale;
        } else if (bitrateKbps <= 160000) {
            centerDelta = -1 * qpScale;
            edgeDelta = 3 * qpScale;
        }
        if (m_dynFoveaValid && std::memcmp(foveaStart, m_dynFoveaStart, sizeof(m_dynFoveaStart)) == 0 &&
            m_dynFoveaSize == foveaSize && m_dynDensityX == densityX && m_dynDensityY == densityY &&
            m_dynCenterDelta == centerDelta && m_dynEdgeDelta == edgeDelta) {
            return;
        }
        m_dynFoveaValid = true;
        std::memcpy(m_dynFoveaStart, foveaStart, sizeof(m_dynFoveaStart));
        m_dynFoveaSize = foveaSize;
        m_dynDensityX = densityX;
        m_dynDensityY = densityY;
        m_dynCenterDelta = centerDelta;
        m_dynEdgeDelta = edgeDelta;

        const uint32_t bw = (m_config.width + m_blockSize - 1) / m_blockSize;
        const uint32_t bh = (m_config.height + m_blockSize - 1) / m_blockSize;
        m_qpMap.assign(size_t(bw) * bh, 0);
        if (centerDelta == 0 && edgeDelta == 0) return;

        const float F = std::clamp(foveaSize, 0.05f, 0.95f);
        const float sx = std::clamp(densityX, 1e-3f, 1.0f);
        const float sy = std::clamp(densityY, 1e-3f, 1.0f);
        const float denomX = F + (1.0f - F) * sx;
        const float denomY = F + (1.0f - F) * sy;
        const float eyeW = m_config.width / 2.0f;
        const float eyeH = float(m_config.height);
        // Smooth 2.5-CTU transition skirt outside the fovea box prevents sharp blocking seams.
        const float skirt = std::max(0.04f, 2.5f * float(m_blockSize) / std::max(1.0f, eyeW));
        float eStart[2][2], eEnd[2][2];
        for (int eye = 0; eye < 2; eye++) {
            eStart[eye][0] = (foveaStart[eye][0] * sx) / denomX;
            eEnd[eye][0] = (foveaStart[eye][0] * sx + F) / denomX;
            eStart[eye][1] = (foveaStart[eye][1] * sy) / denomY;
            eEnd[eye][1] = (foveaStart[eye][1] * sy + F) / denomY;
        }
        for (uint32_t by = 0; by < bh; by++) {
            const float ey = std::clamp(((by + 0.5f) * m_blockSize) / eyeH, 0.0f, 1.0f);
            for (uint32_t bx = 0; bx < bw; bx++) {
                const float x = (bx + 0.5f) * m_blockSize;
                const int eye = x < eyeW ? 0 : 1;
                const float ex = std::clamp((x - eye * eyeW) / eyeW, 0.0f, 1.0f);
                const float dx = ex < eStart[eye][0] ? (eStart[eye][0] - ex)
                               : ex > eEnd[eye][0]   ? (ex - eEnd[eye][0])
                                                     : 0.0f;
                const float dy = ey < eStart[eye][1] ? (eStart[eye][1] - ey)
                               : ey > eEnd[eye][1]   ? (ey - eEnd[eye][1])
                                                     : 0.0f;
                const float dist = std::sqrt(dx * dx + dy * dy);
                const float t = std::clamp(dist / skirt, 0.0f, 1.0f);
                const float s = t * t * (3.0f - 2.0f * t);
                const float qpf = float(centerDelta) + s * float(edgeDelta - centerDelta);
                m_qpMap[size_t(by) * bw + bx] = int8_t(std::clamp(int(std::lround(qpf)), -51, 51));
            }
        }
    }

   private:
    static constexpr uint32_t kDpbFrames = 8;
    // The old CBR behaviour, kept for --cbr A/B runs and as a driver fallback:
    // average == maximum, and 1.5 frames of VBV.
    static constexpr uint32_t kLegacyVbvFramesPercent = 150;
    rc::EncoderRatePlan RatePlan(int64_t now) const { return RatePlan(m_config.profile, now); }

    rc::EncoderRatePlan RatePlan(rc::StreamProfile profile, int64_t now) const {
        const auto q = rc::ProfileParams(profile);
        const uint32_t avgPct = m_config.vbrAveragePercent ? m_config.vbrAveragePercent : q.vbrAveragePercent;
        const bool startup = !m_streamStartNs || now - m_streamStartNs < rc::kRateStartupNs;
        const bool cadenceFill = CadenceFillExperiment() && m_cappedVbr &&
                                 profile == rc::StreamProfile::Quality && !m_config.desktop &&
                                 !startup && m_config.fps > 0;
        // L17 fill branch gate: pressure only (fast EMA below 85% of nominal, VR, not
        // startup); the gate owns the 5 s hold, 30 s cooldown and the only-down latch.
        // The pure planner still does the rung math; VBV and scaled stay frozen D-034.
        const double realFps = m_rateTracker.RealFps(m_config.fps);
        const bool pressure = !cadenceFill && m_config.fillMode && !m_config.desktop && !startup &&
                              m_config.fps > 0 && realFps < double(m_config.fps) * 0.85;
        const bool fillOn = m_fillGate.Update(pressure, now);
        return rc::PlanEncoderRate(m_requestedKbps, m_config.fps, m_config.desktop, startup, m_rateTracker,
                                   m_rateState.applied.rateNum, m_cappedVbr ? avgPct : 100,
                                   m_cappedVbr ? q.vbvFramesPercent : kLegacyVbvFramesPercent,
                                   fillOn && !cadenceFill, cadenceFill);
    }

    static void ApplyBudget(NV_ENC_CONFIG& config, const rc::EncoderRatePlan& plan) {
        auto& params = config.rcParams;
        params.averageBitRate = plan.budget.averageBitrate;
        params.maxBitRate = plan.budget.maxBitrate;
        params.vbvBufferSize = plan.budget.vbvBufferSize;
    }

    // 仅初始化使用；运行期必须经过完整候选事务，不能提前修改已提交快照。
    void ApplyBitrate(uint32_t kbps) {
        m_requestedKbps = kbps;
        const auto plan = RatePlan(0);
        ApplyBudget(m_encodeConfig, plan);
        m_encodeConfig.rcParams.vbvInitialDelay = plan.budget.vbvBufferSize;
    }

    void ReconfigureRate(int64_t now) {
        if (!m_requestedKbps) return;  // 还没有有效请求，不下发 0 码率
        const auto plan = RatePlan(now);
        if (!m_rateState.ShouldAttempt(plan, now)) return;
        auto candidateConfig = m_encodeConfig;
        auto candidateInit = m_init;
        ApplyBudget(candidateConfig, plan);
        // 降码率或升档都会缩小 VBV；旧的 initial delay 若大于新桶，驱动拒收整次重配置，
        // 请求就会永远重试失败、码率再也降不下来。夹到桶内，仍不重新灌满。
        candidateConfig.rcParams.vbvInitialDelay =
            rc::ClampVbvInitialDelay(candidateConfig.rcParams.vbvInitialDelay, plan.budget.vbvBufferSize);
        candidateInit.frameRateNum = plan.rateNum;
        candidateInit.frameRateDen = 1000;
        candidateInit.encodeConfig = &candidateConfig;
        // nvEncReconfigureEncoder 是编码侧唯一没有耗时仪表的阻塞点：它落在 submitStart
        // 之前，ENC 5s 的 submit/gpu wait 都看不见它，而一次单发的慢调用可以被按帧统计的
        // enc p95 排名数学藏住。ApplyEncoderRateTransaction 恰好调用 lambda 一次，所以
        // 这里量的就是这一次 nvEncReconfigureEncoder 的墙钟耗时。
        double reconfigMs = 0.0;
        const bool ok = rc::ApplyEncoderRateTransaction(m_rateState, plan, now, [&] {
            NV_ENC_RECONFIGURE_PARAMS re{};
            re.version = NV_ENC_RECONFIGURE_PARAMS_VER;
            re.reInitEncodeParams = candidateInit;
            // resetEncoder/forceIDR 为零；保留已提交的 vbvInitialDelay，不重新灌满桶。
            const int64_t reconfigStart = rc::NowNs();
            const bool done = NvencLib::Get().api.nvEncReconfigureEncoder(m_encoder, &re) == NV_ENC_SUCCESS;
            reconfigMs = double(rc::NowNs() - reconfigStart) / 1e6;
            return done;
        });
        if (!ok) {
            // 失败的重配置同样可能很慢，所以失败分支也带耗时（行末追加，前导字段不变）。
            RC_LOGW("NVENC: rate reconfigure failed; request retained for throttled retry, reconfig %.1f ms",
                    reconfigMs);
            return;
        }
        m_encodeConfig = candidateConfig;
        m_init = candidateInit;
        m_init.encodeConfig = &m_encodeConfig;
        m_config.bitrateKbps = plan.requestedKbps;
        // 语义澄清（字段名是历史日志锚，不许改）：`X fps measured` 不是产出率，是
        // RateTracker::RealFps() = 1e9 / fastIntervalNs（fps_ladder.h），即 Encode 入口
        // 间隔的快速 EMA。偏低 ⇒ 存在约 1e9/X ns 的入口空隙（上游停顿），不能读成
        // 「只编出 X 帧」；产出率看同窗的 rc diag 1s enc/s 与 ENC 5s 行。
        RC_LOGI("NVENC: %.1f fps measured, encoder rate %.1f fps, max %u kbps, VBV %.2f Mbit, reconfig %.1f ms",
                m_rateTracker.RealFps(m_config.fps), double(plan.rateNum) / 1000.0,
                plan.budget.maxBitrate / 1000, double(plan.budget.vbvBufferSize) / 1e6, reconfigMs);
    }

    void TrackFrameInterval() {
        const int64_t now = rc::NowNs();
        if (!m_streamStartNs) m_streamStartNs = now;
        if (!m_config.desktop && m_lastEncodeNs) m_rateTracker.Observe(double(now - m_lastEncodeNs));
        m_lastEncodeNs = now;
        // 即使外层 appliedKbps 已去重，失败的请求仍能在编码线程重试；桌面不参与 fps 观测。
        if (m_encoder) ReconfigureRate(now);
    }

    // The caller guarantees that an encoder outlives only the input textures it was
    // created for: rebuilding the textures rebuilds the encoder (VrStream::RunEncoder).
    // With a bare-pointer cache that contract is what rules out an ABA hit - a new
    // texture at a freed address would otherwise be taken for the dead one.
    NV_ENC_REGISTERED_PTR Register(ID3D11Texture2D* tex) {
        if (auto it = m_registered.find(tex); it != m_registered.end()) return it->second;
        if (m_registered.size() >= 8) UnregisterAll();
        D3D11_TEXTURE2D_DESC desc;
        tex->GetDesc(&desc);
        NV_ENC_REGISTER_RESOURCE r{};
        r.version = NV_ENC_REGISTER_RESOURCE_VER;
        r.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
        r.resourceToRegister = tex;
        r.width = desc.Width;
        r.height = desc.Height;
        r.pitch = 0;
        r.bufferUsage = NV_ENC_INPUT_IMAGE;
        switch (desc.Format) {
            case DXGI_FORMAT_NV12: r.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12; break;
            case DXGI_FORMAT_P010: r.bufferFormat = NV_ENC_BUFFER_FORMAT_YUV420_10BIT; break;
            case DXGI_FORMAT_R8G8B8A8_UNORM: r.bufferFormat = NV_ENC_BUFFER_FORMAT_ABGR; break;
            default: r.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB; break;
        }
        if (NvencLib::Get().api.nvEncRegisterResource(m_encoder, &r) != NV_ENC_SUCCESS) {
            RC_LOGE("NVENC: register resource failed");
            return nullptr;
        }
        m_registered[tex] = r.registeredResource;
        return r.registeredResource;
    }

    void UnregisterAll() {
        for (auto& [tex, reg] : m_registered) NvencLib::Get().api.nvEncUnregisterResource(m_encoder, reg);
        m_registered.clear();
    }

    void Destroy() {
        if (!m_encoder) return;
        auto& api = NvencLib::Get().api;
        // Flush.
        NV_ENC_PIC_PARAMS eos{};
        eos.version = NV_ENC_PIC_PARAMS_VER;
        eos.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
        api.nvEncEncodePicture(m_encoder, &eos);
        UnregisterAll();
        if (m_bitstream) api.nvEncDestroyBitstreamBuffer(m_encoder, m_bitstream);
        api.nvEncDestroyEncoder(m_encoder);
        m_encoder = nullptr;
    }

    ComPtr<ID3D11Device> m_device;
    void* m_encoder = nullptr;
    NV_ENC_OUTPUT_PTR m_bitstream = nullptr;
    NV_ENC_CONFIG m_encodeConfig{};
    NV_ENC_INITIALIZE_PARAMS m_init{};
    std::map<ID3D11Texture2D*, NV_ENC_REGISTERED_PTR> m_registered;
    uint64_t m_frameCount = 0;
    // 仅编码线程访问：请求、观测与最后一次成功提交的硬件计划分开保存。
    int64_t m_lastEncodeNs = 0;
    int64_t m_streamStartNs = 0;
    uint32_t m_requestedKbps = 0;
    rc::EncoderRateTracker m_rateTracker;
    rc::EncoderRateState m_rateState;
    // L17 fill-mode hysteresis, encode thread only: 5 s sustained pressure before the
    // rung may follow down, 30 s cooldown, latched on - a manual toggle climbs back.
    mutable rc::FillModeGate m_fillGate;
    uint64_t m_lastIntraTs = 0;

    // Encoder-side truth, summarised every 5 s from the lock path below: the QP the
    // rate control actually ran and the bytes that actually came out, so "why didn't
    // it spend the ceiling" stops being a guess. Encoding thread only - no locks.
    int64_t m_statStartNs = 0;
    uint32_t m_statFrames = 0;
    rc::QpStats m_qpStats;
    uint64_t m_statBytes = 0;
    // Per-frame byte spread in the same 5 s window: a steady mean with a wide
    // min..max is the signature of frame-to-frame quality pumping.
    uint32_t m_statBytesMin = 0, m_statBytesMax = 0;
    // Where the encode time actually goes: nvEncEncodePicture (the submit) versus
    // nvEncLockBitstream (waiting for the GPU to finish). The gap is the diagnosis
    // for "the engine is at 13% but the frame took 10 ms".
    int64_t m_statSubmitNs = 0;
    int64_t m_statGpuWaitNs = 0;
    bool m_canInvalidate = false;
    bool m_split = false;
    bool m_foveated = false;
    bool m_cappedVbr = true;  // false after a driver refused VBR, or with --cbr
    uint32_t m_blockSize = 32;
    float m_fov[2][4] = {};
    bool m_dynFoveaValid = false;
    float m_dynFoveaStart[2][2] = {};
    float m_dynFoveaSize = 0.0f;
    float m_dynDensityX = 0.0f;
    float m_dynDensityY = 0.0f;
    int m_dynCenterDelta = 0;
    int m_dynEdgeDelta = 0;
    std::vector<int8_t> m_qpMap;
};

}  // namespace

bool NvencProbe(ID3D11Device* device, EncoderCaps& caps) {
    void* enc = OpenSession(device);
    if (!enc) return false;
    auto& api = NvencLib::Get().api;
    uint32_t count = 0;
    api.nvEncGetEncodeGUIDCount(enc, &count);
    std::vector<GUID> guids(count);
    api.nvEncGetEncodeGUIDs(enc, guids.data(), count, &count);
    caps.backend = "nvenc";
    for (const auto& g : guids) {
        if (g == NV_ENC_CODEC_H264_GUID) caps.h264 = true;
        if (g == NV_ENC_CODEC_HEVC_GUID) {
            caps.hevc = true;
            caps.hevc10 = GetCap(enc, g, NV_ENC_CAPS_SUPPORT_10BIT_ENCODE) != 0;
            caps.maxWidth = uint32_t(GetCap(enc, g, NV_ENC_CAPS_WIDTH_MAX));
            caps.maxHeight = uint32_t(GetCap(enc, g, NV_ENC_CAPS_HEIGHT_MAX));
        }
        if (g == NV_ENC_CODEC_AV1_GUID) {
            caps.av1 = true;
            caps.av110 = GetCap(enc, g, NV_ENC_CAPS_SUPPORT_10BIT_ENCODE) != 0;
        }
    }
    api.nvEncDestroyEncoder(enc);
    return caps.h264 || caps.hevc;
}

std::unique_ptr<VideoEncoder> CreateNvencEncoder() { return std::make_unique<NvencEncoder>(); }

}  // namespace rcs
