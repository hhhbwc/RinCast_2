#include "video_decoder.h"

#include <GLES2/gl2ext.h>
#include <android/hardware_buffer.h>
#include <media/NdkMediaFormat.h>

#include <cstring>
#include <string>
#include <sys/resource.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include "rc/platform.h"
#include "../platform/jni_bridge.h"

namespace rv {

namespace {

PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC pGetNativeClientBuffer = nullptr;
PFNEGLCREATEIMAGEKHRPROC pCreateImage = nullptr;
PFNEGLDESTROYIMAGEKHRPROC pDestroyImage = nullptr;
PFNGLEGLIMAGETARGETTEXTURE2DOESPROC pImageTargetTexture = nullptr;

void LoadEglProcs() {
    if (pCreateImage) return;
    pGetNativeClientBuffer = reinterpret_cast<PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC>(
        eglGetProcAddress("eglGetNativeClientBufferANDROID"));
    pCreateImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    pDestroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    pImageTargetTexture =
        reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
}

const char* Mime(rc::VideoCodec c) {
    switch (c) {
        case rc::VideoCodec::H264: return "video/avc";
        case rc::VideoCodec::HEVC: return "video/hevc";
        case rc::VideoCodec::AV1: return "video/av01";
    }
    return "video/hevc";
}

// Splits an Annex-B stream into NAL units (start code included).
std::vector<std::pair<size_t, size_t>> SplitNals(const std::vector<uint8_t>& d) {
    std::vector<size_t> starts;
    for (size_t i = 0; i + 3 < d.size(); i++) {
        if (d[i] == 0 && d[i + 1] == 0 && (d[i + 2] == 1 || (d[i + 2] == 0 && d[i + 3] == 1))) {
            starts.push_back(i);
            i += 2;
        }
    }
    std::vector<std::pair<size_t, size_t>> nals;
    for (size_t k = 0; k < starts.size(); k++)
        nals.push_back({starts[k], (k + 1 < starts.size() ? starts[k + 1] : d.size()) - starts[k]});
    return nals;
}

int NalType(const std::vector<uint8_t>& d, size_t start, rc::VideoCodec codec) {
    size_t p = start;
    while (p < d.size() && d[p] == 0) p++;
    p++;  // the 0x01
    if (p >= d.size()) return -1;
    return codec == rc::VideoCodec::H264 ? (d[p] & 0x1F) : ((d[p] >> 1) & 0x3F);
}

}  // namespace

VideoDecoder::~VideoDecoder() { Shutdown(); }

DecoderCaps VideoDecoder::Capabilities() {
    DecoderCaps caps;
    caps.h264 = true;
    caps.hevc = true;
    caps.hevc10 = false;
    caps.av1 = false;
    caps.av110 = false;
    caps.maxWidth = 4096;
    caps.maxHeight = 2160;

    auto jniCaps = Platform::VideoDecoderCaps();
    if (jniCaps.valid) {
        caps.h264 = jniCaps.h264;
        caps.hevc = jniCaps.hevc;
        caps.hevc10 = jniCaps.hevc10;
        caps.av1 = jniCaps.av1;
        caps.av110 = jniCaps.av110;
        caps.maxWidth = jniCaps.maxWidth;
        caps.maxHeight = jniCaps.maxHeight;
        RC_LOGI("Decoder dynamic caps (from MediaCodecList): hevc10=%d av1=%d av110=%d max=%dx%d",
                caps.hevc10, caps.av1, caps.av110, caps.maxWidth, caps.maxHeight);
    } else {
        // Fallback for native/Linux or if JNI query failed
        caps.hevc10 = true;
        if (AMediaCodec* av1 = AMediaCodec_createDecoderByType("video/av01")) {
            char* name = nullptr;
            AMediaCodec_getName(av1, &name);
            const bool software = name && (!strncmp(name, "c2.android.", 11) || !strncmp(name, "OMX.google.", 11));
            caps.av1 = caps.av110 = !software;
            RC_LOGI("AV1 decoder: %s%s", name ? name : "?", software ? " (software, not used)" : "");
            if (name) AMediaCodec_releaseName(av1, name);
            AMediaCodec_delete(av1);
        }
        char device[PROP_VALUE_MAX] = {};
        __system_property_get("ro.product.device", device);
        const bool quest1 = !strcmp(device, "monterey");
        caps.maxWidth = quest1 ? 4096 : 8192;
        caps.maxHeight = quest1 ? 2160 : 4096;
    }

    // ROUND18 L3: report a frame-rate ceiling for devices where we MEASURED a
    // decode wall (our own PICO 4: HEVC 2688x2688 topped out near 45 fps). This is
    // a hint, not a hard cap; the PC only warns. Unknown devices stay at 0 = no
    // report. Detection reuses the same props the XR loader uses (PHOENIX/A8110).
    char model[PROP_VALUE_MAX] = {};
    __system_property_get("ro.product.model", model);
    const bool pico4 = !strcmp(model, "Phoenix") || !strcmp(model, "A8110") || !strcmp(model, "A8110 Pro") ||
                       !strncmp(model, "PICO 4", 6);
    if (pico4) caps.maxFpsAtMaxRes = 45;
    RC_LOGI("Decoder caps: %dx%d, max fps at max res %d", caps.maxWidth, caps.maxHeight, caps.maxFpsAtMaxRes);
    return caps;
}

bool VideoDecoder::Init(rc::VideoCodec codec, int width, int height, float fps) {
    LoadEglProcs();
    ResetTextureGeometry();
    m_codec = codec;
    m_width = width;
    m_height = height;
    m_fps = fps > 1 ? fps : 72;  // ROUND18 L4 fallback
    m_readerMaxImages = 8;
    media_status_t st = AImageReader_newWithUsage(width, height, AIMAGE_FORMAT_PRIVATE,
                                                  AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, 8, &m_reader);
    if (st != AMEDIA_OK || !m_reader) {
        // Fallback to 4 buffers if 8 fails due to memory constraints on certain GPU architectures
        m_readerMaxImages = 4;
        st = AImageReader_newWithUsage(width, height, AIMAGE_FORMAT_PRIVATE,
                                       AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, 4, &m_reader);
    }
    if (st != AMEDIA_OK || !m_reader) {
        RC_LOGE("Decoder %s %dx%d: AImageReader_newWithUsage failed (%d, reader=%p)", Mime(codec), width, height,
                int(st), static_cast<void*>(m_reader));
        m_reader = nullptr;
        m_failed = true;
        return false;
    }
    const media_status_t windowStatus = AImageReader_getWindow(m_reader, &m_window);
    if (windowStatus != AMEDIA_OK || !m_window) {
        RC_LOGE("Decoder %s %dx%d: AImageReader_getWindow failed (%d, window=%p)", Mime(codec), width, height,
                int(windowStatus), static_cast<void*>(m_window));
        AImageReader_delete(m_reader);
        m_reader = nullptr;
        m_window = nullptr;
        m_failed = true;
        return false;
    }
    GLuint texture = 0;
    glGenTextures(1, &texture);
    if (!texture) {
        RC_LOGE("Decoder %s %dx%d: glGenTextures failed", Mime(codec), width, height);
        AImageReader_delete(m_reader);
        m_reader = nullptr;
        m_window = nullptr;
        m_failed = true;
        return false;
    }
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    const float crop[4] = {0, 0, 0, 0};
    PublishTextureGeometry(texture, 0, 0, crop);
    m_running = true;
    m_inputThread = std::thread([this] { InputLoop(); });
    return true;
}

void VideoDecoder::Shutdown() {
    m_running = false;
    m_cv.notify_all();
    if (m_inputThread.joinable()) m_inputThread.join();
    if (m_outputThread.joinable()) m_outputThread.join();
    if (m_codecHandle) {
        AMediaCodec_stop(m_codecHandle);
        AMediaCodec_delete(m_codecHandle);
        m_codecHandle = nullptr;
    }
    m_configured = false;
    ReleaseImage(m_image, m_eglImage);
    m_image = nullptr;
    m_eglImage = EGL_NO_IMAGE_KHR;
    for (auto& [img, egl] : m_retired) ReleaseImage(img, egl);
    m_retired.clear();
    m_held.clear();  // the decoder's own buffers, gone with it
    m_behindSinceNs = m_inReaderSinceNs = 0;
    if (m_reader) {
        AImageReader_delete(m_reader);
        m_reader = nullptr;
        m_window = nullptr;
    }
    auto geometry = Geometry();
    ResetTextureGeometry();
    if (geometry.texture) glDeleteTextures(1, &geometry.texture);
}

bool VideoDecoder::Configure(const std::vector<uint8_t>& keyframe) {
    if (!m_window) {
        RC_LOGE("Decoder %s %dx%d: missing output window", Mime(m_codec), m_width, m_height);
        m_failed = true;
        return false;
    }

    auto tryConfigure = [&](bool safeFallback) -> bool {
        if (m_codecHandle) {
            AMediaCodec_stop(m_codecHandle);
            AMediaCodec_delete(m_codecHandle);
            m_codecHandle = nullptr;
        }
        m_codecHandle = AMediaCodec_createDecoderByType(Mime(m_codec));
        if (!m_codecHandle) {
            RC_LOGE("No decoder for %s", Mime(m_codec));
            return false;
        }
        char* component = nullptr;
        const media_status_t componentStatus = AMediaCodec_getName(m_codecHandle, &component);
        std::string compName = (componentStatus == AMEDIA_OK && component) ? component : "";
        if (component) AMediaCodec_releaseName(m_codecHandle, component);

        std::string compLower = compName;
        std::transform(compLower.begin(), compLower.end(), compLower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        char boardProp[PROP_VALUE_MAX] = {};
        __system_property_get("ro.board.platform", boardProp);
        std::string boardLower = boardProp;
        std::transform(boardLower.begin(), boardLower.end(), boardLower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        const bool isQualcomm = (compLower.find("qti") != std::string::npos || compLower.find("qcom") != std::string::npos);
        const bool isMediatek = (compLower.find("mtk") != std::string::npos || compLower.find("mediatek") != std::string::npos ||
                                 boardLower.rfind("mt", 0) == 0);
        const bool isRockchip = (compLower.find("rk.") != std::string::npos || compLower.find("rockchip") != std::string::npos ||
                                 boardLower.rfind("rk", 0) == 0);
        const bool isLegacyOmx = (compLower.rfind("omx.", 0) == 0);
        const bool isSoftware = (compLower.rfind("c2.android.", 0) == 0 || compLower.rfind("omx.google.", 0) == 0);

        RC_LOGI("Decoder component: %s (attempt: %s, board: %s, qcom:%d mtk:%d rk:%d omx:%d sw:%d)",
                compName.c_str(), safeFallback ? "safe-fallback" : "optimized", boardLower.c_str(),
                isQualcomm, isMediatek, isRockchip, isLegacyOmx, isSoftware);

        AMediaFormat* fmt = AMediaFormat_new();
        if (!fmt) {
            RC_LOGE("Decoder %s %dx%d: AMediaFormat_new failed", Mime(m_codec), m_width, m_height);
            AMediaCodec_delete(m_codecHandle);
            m_codecHandle = nullptr;
            return false;
        }
        AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, Mime(m_codec));
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, m_width);
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, m_height);

        if (!safeFallback) {
            AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_MAX_WIDTH, m_width);
            AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_MAX_HEIGHT, m_height);
            const int64_t rawInputSize = static_cast<int64_t>(m_width) * m_height / 4;
            const int32_t clampedInputSize = static_cast<int32_t>(std::clamp<int64_t>(rawInputSize, 1024 * 1024, 8 * 1024 * 1024));
            AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE, clampedInputSize);
            // Latency over everything else, filtered by SoC/component quirks to avoid
            // configure rejections or frame-order stalls on legacy OMX / Rockchip / MTK decoders.
            AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_PRIORITY, 0);
            AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_FRAME_RATE, int32_t(m_fps + 0.5f));
            if (!isLegacyOmx && !isRockchip && !isSoftware) {
                AMediaFormat_setInt32(fmt, "low-latency", 1);
                const float opRateMul = isMediatek ? 1.0f : 2.0f;
                AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_OPERATING_RATE, int32_t(m_fps * opRateMul + 0.5f));
            }

            if (isQualcomm && !isLegacyOmx && !s_compatPlain) {
                AMediaFormat_setInt32(fmt, "vendor.qti-ext-dec-low-latency.enable", 1);
                AMediaFormat_setInt32(fmt, "vendor.qti-ext-dec-picture-order.enable", 1);
                AMediaFormat_setInt32(fmt, "vendor.qti-ext-dec-info-misr.disable", 1);
                AMediaFormat_setInt32(fmt, "vendor.qti-ext-dec-instant-decode.enable", 1);
                AMediaFormat_setInt32(fmt, "vendor.qti-ext-dec-error-correction.conceal", 1);
            }
        }

        // Parameter sets as codec-specific data (they are also repeated inline).
        if (m_codec != rc::VideoCodec::AV1) {
            std::vector<uint8_t> csd0, csd1;
            for (auto [off, len] : SplitNals(keyframe)) {
                const int t = NalType(keyframe, off, m_codec);
                if (m_codec == rc::VideoCodec::H264) {
                    if (t == 7) csd0.insert(csd0.end(), keyframe.begin() + off, keyframe.begin() + off + len);
                    if (t == 8) csd1.insert(csd1.end(), keyframe.begin() + off, keyframe.begin() + off + len);
                } else if (t == 32 || t == 33 || t == 34) {
                    csd0.insert(csd0.end(), keyframe.begin() + off, keyframe.begin() + off + len);
                }
            }
            if (!csd0.empty()) AMediaFormat_setBuffer(fmt, "csd-0", csd0.data(), csd0.size());
            if (!csd1.empty()) AMediaFormat_setBuffer(fmt, "csd-1", csd1.data(), csd1.size());
        }
        media_status_t st = AMediaCodec_configure(m_codecHandle, fmt, m_window, nullptr, 0);
        AMediaFormat_delete(fmt);
        const media_status_t started = (st == AMEDIA_OK) ? AMediaCodec_start(m_codecHandle) : st;
        if (st != AMEDIA_OK || started != AMEDIA_OK) {
            RC_LOGW("Decoder %s %dx%d: %s (attempt: %s) failed (st=%d started=%d)", Mime(m_codec), m_width, m_height,
                    st != AMEDIA_OK ? "configure" : "start", safeFallback ? "safe-fallback" : "optimized",
                    int(st), int(started));
            AMediaCodec_delete(m_codecHandle);
            m_codecHandle = nullptr;
            return false;
        }
        return true;
    };

    bool ok = tryConfigure(false);
    if (!ok) {
        RC_LOGW("Decoder %s %dx%d: Attempt 1 failed, retrying with Attempt 2 (universal safe degradation)",
                Mime(m_codec), m_width, m_height);
        ok = tryConfigure(true);
    }
    if (!ok) {
        RC_LOGE("Decoder %s %dx%d: Both configuration attempts failed", Mime(m_codec), m_width, m_height);
        m_failed = true;
        return false;
    }
    m_consecutiveInputStalls = 0;
    const int64_t startNs = rc::NowNs();
    m_lastInputQueuedNs.store(0, std::memory_order_relaxed);
    m_lastOutputDecodedNs.store(startNs, std::memory_order_relaxed);
    m_configured = true;
    m_failed = false;  // a later keyframe got it going after all
    m_outputThread = std::thread([this] { OutputLoop(); });
    RC_LOGI("Decoder %s %dx%d started", Mime(m_codec), m_width, m_height);
    return true;
}

void VideoDecoder::InputLoop() {
    setpriority(PRIO_PROCESS, gettid(), -10);  // best effort
    while (m_running) {
        Pending p;
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait_for(lock, std::chrono::milliseconds(20), [&] { return !m_running || !m_queue.empty(); });
            if (!m_running) break;
            if (m_queue.empty()) continue;
            p = std::move(m_queue.front());
            m_queue.pop_front();
        }
        if (!m_running) break;
        if (!m_configured) {
            if (!p.keyframe || !Configure(p.data)) continue;
        }
        ssize_t index = -1;
        for (int attempt = 0; attempt < 50 && m_running && index < 0; attempt++) {
            index = AMediaCodec_dequeueInputBuffer(m_codecHandle, 2000);
            if (index < 0 && index != AMEDIACODEC_INFO_TRY_AGAIN_LATER) break;
        }
        if (!m_running) break;
        if (index < 0 && index != AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
            Fail("dequeueInputBuffer returned an error",
                 rc::StrFormat("status=%lld frame=%u data=%zu", static_cast<long long>(index), p.frameIndex,
                               p.data.size())
                     .c_str());
            break;
        }
        if (index < 0) {
            m_consecutiveInputStalls++;
            Warn("no free input buffer for 100 ms, the decoder is falling behind",
                 rc::StrFormat("status=%lld frame=%u data=%zu attempts=50 timeoutMs=100 stalls=%d",
                               static_cast<long long>(index), p.frameIndex, p.data.size(), m_consecutiveInputStalls)
                     .c_str());
            if (m_consecutiveInputStalls >= 15) {
                Fail("decoder stalled (consecutive input timeouts)",
                     rc::StrFormat("stalls=%d frame=%u", m_consecutiveInputStalls, p.frameIndex).c_str());
                break;
            }
            // This frame never reaches the decoder, so the chain breaks here.
            std::lock_guard lock(m_mutex);
            m_queue.clear();
            m_seenKeyframe = false;
            m_lastFrameIndex = m_lastQueued;
            m_needKeyframe = true;
            continue;
        }
        size_t capacity = 0;
        uint8_t* buf = AMediaCodec_getInputBuffer(m_codecHandle, size_t(index), &capacity);
        const size_t n = std::min(capacity, p.data.size());
        if (!buf || capacity == 0 || p.data.size() > capacity) {
            Warn(!buf || capacity == 0 ? "queue-in empty input buffer" : "queue-in bitstream exceeds input buffer",
                 rc::StrFormat("frame=%u index=%zu buffer=%p capacity=%zu data=%zu copied=%zu", p.frameIndex,
                               size_t(index), static_cast<void*>(buf), capacity, p.data.size(), n)
                     .c_str());
            const media_status_t returnStatus =
                AMediaCodec_queueInputBuffer(m_codecHandle, size_t(index), 0, 0, uint64_t(p.frameIndex), 0);
            if (returnStatus != AMEDIA_OK)
                Fail("queueInputBuffer empty-buffer return failed",
                     rc::StrFormat("status=%d frame=%u index=%zu", int(returnStatus), p.frameIndex, size_t(index)).c_str());
            std::lock_guard lock(m_mutex);
            m_queue.clear();
            m_seenKeyframe = false;
            m_lastFrameIndex = m_lastQueued;
            m_needKeyframe = true;
            continue;
        }
        if (n > 0) std::memcpy(buf, p.data.data(), n);
        const media_status_t queueStatus =
            AMediaCodec_queueInputBuffer(m_codecHandle, size_t(index), 0, n, uint64_t(p.frameIndex), 0);
        if (queueStatus != AMEDIA_OK) {
            Fail("queueInputBuffer failed",
                 rc::StrFormat("status=%d frame=%u index=%zu offset=0 size=%zu capacity=%zu data=%zu pts=%u",
                               int(queueStatus), p.frameIndex, size_t(index), n, capacity, p.data.size(), p.frameIndex)
                     .c_str());
            break;
        }
        m_consecutiveInputStalls = 0;
        const int64_t queuedNs = rc::NowNs();
        m_lastInputQueuedNs.store(queuedNs, std::memory_order_relaxed);
        {
            std::lock_guard lock(m_statsMutex);
            m_inputTimes[int64_t(p.frameIndex)] = queuedNs;
            while (m_inputTimes.size() > 64) m_inputTimes.erase(m_inputTimes.begin());
        }
        m_lastQueued = p.frameIndex;
    }
}

void VideoDecoder::OutputLoop() {
    setpriority(PRIO_PROCESS, gettid(), -10);  // best effort
    while (m_running) {
        AMediaCodecBufferInfo info;
        const ssize_t index = AMediaCodec_dequeueOutputBuffer(m_codecHandle, &info, 10000);
        if (index >= 0) {
            const int64_t now = rc::NowNs();
            m_lastOutputDecodedNs.store(now, std::memory_order_relaxed);
            double frameDecodeMs = -1;
            {
                std::lock_guard lock(m_statsMutex);
                auto it = m_inputTimes.find(info.presentationTimeUs);
                if (it != m_inputTimes.end()) {
                    frameDecodeMs = rc::NsToMs(now - it->second);
                    m_decodeMsSum += frameDecodeMs;
                    m_decodeMsMax = std::max(m_decodeMsMax, frameDecodeMs);
                    m_decodeCount++;
                    m_inputTimes.erase(it);
                }
            }
            // R3: the per-frame cost feeds the diagnostics quantiles (outside the stats lock).
            if (frameDecodeMs >= 0 && m_decodeSampleSink) m_decodeSampleSink(frameDecodeMs);
            {
                std::lock_guard lock(m_mutex);
                auto it = m_infos.find(info.presentationTimeUs);
                if (it != m_infos.end()) it->second.decodedNs = now;
            }
            if (info.size != 0) {
                std::lock_guard lock(m_heldMutex);
                m_held.push_back(size_t(index));
            } else {
                const media_status_t releaseStatus =
                    AMediaCodec_releaseOutputBuffer(m_codecHandle, size_t(index), false);
                if (releaseStatus != AMEDIA_OK) {
                    Fail("releaseOutputBuffer failed",
                         rc::StrFormat("status=%d index=%lld pts=%llu size=%zu flags=%u render=0", int(releaseStatus),
                                       static_cast<long long>(index),
                                       static_cast<unsigned long long>(info.presentationTimeUs), info.size,
                                       uint32_t(info.flags))
                             .c_str());
                    break;
                }
            }
            if (!m_decoded++) RC_LOGI("Decoder %s %dx%d: first frame decoded", Mime(m_codec), m_width, m_height);
        } else if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat* f = AMediaCodec_getOutputFormat(m_codecHandle);
            if (!f) {
                Warn("AMediaCodec_getOutputFormat returned null");
            } else {
                const char* formatText = AMediaFormat_toString(f);
                RC_LOGI("Decoder output format: %s", formatText ? formatText : "?");
                AMediaFormat_delete(f);
            }
        } else if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
            const int64_t now = rc::NowNs();
            const int64_t lastIn = m_lastInputQueuedNs.load(std::memory_order_relaxed);
            const int64_t lastOut = m_lastOutputDecodedNs.load(std::memory_order_relaxed);
            if (lastIn > 0 && lastOut > 0 && (now - lastIn) < 500'000'000LL && (now - lastOut) > 2'000'000'000LL) {
                Fail("decoder stalled (no output frames for 2s)",
                     rc::StrFormat("queuedFrame=%u decoded=%u", m_lastQueued.load(), m_decoded.load()).c_str());
                break;
            }
        } else if (index != AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
            Fail("dequeueOutputBuffer returned an error",
                 rc::StrFormat("status=%lld queuedFrame=%u", static_cast<long long>(index), m_lastQueued.load()).c_str());
            break;
        }
        std::lock_guard lock(m_heldMutex);
        HandOver();
    }
}

// Decoded pictures go to the image reader one at a time, in order, each once the one before
// was taken. The reader keeps only the newest picture not yet taken: handed over as decoded,
// two finished between the same two displays (one a little late, the next on time) lost the
// late one, and the display before repeated its picture. A skip and a stutter together,
// several times a second on Wi-Fi. Now the newer one waits in the decoder for the next display.
// That keeps the stream a frame behind until a display comes without a new picture; Latch
// skips one to catch up when that lasts. Called with m_heldMutex held.
void VideoDecoder::HandOver() {
    const int64_t now = rc::NowNs();
    // A picture handed over but never taken (the reader dropped it): don't wait for it forever.
    if (m_inReaderSinceNs && now - m_inReaderSinceNs > 100'000'000LL) m_inReaderSinceNs = 0;
    if (m_inReaderSinceNs || m_held.empty()) return;
    // ROUND18 L5: the depth at which the oldest decoded pictures are dropped.
    // Configurable; default 2 = the behaviour before this field. Dropping from the
    // OUTPUT side (already-decoded pictures) cannot break the reference chain, so
    // it never smears the picture - the cost is only a skipped/held frame.
    while (m_held.size() > StaleDropThreshold()) {
        const size_t index = m_held.front();
        const media_status_t releaseStatus = AMediaCodec_releaseOutputBuffer(m_codecHandle, index, false);
        if (releaseStatus != AMEDIA_OK) {
            Fail("releaseOutputBuffer failed",
                 rc::StrFormat("status=%d index=%zu render=0 held=%zu staleDropThreshold=%u", int(releaseStatus), index,
                               m_held.size(), StaleDropThreshold())
                     .c_str());
            return;
        }
        m_held.pop_front();
        m_skipped++;
    }
    const size_t index = m_held.front();
    const media_status_t releaseStatus = AMediaCodec_releaseOutputBuffer(m_codecHandle, index, true);
    if (releaseStatus != AMEDIA_OK) {
        Fail("releaseOutputBuffer failed",
             rc::StrFormat("status=%d index=%zu render=1 held=%zu staleDropThreshold=%u", int(releaseStatus), index,
                           m_held.size(), StaleDropThreshold())
                 .c_str());
        return;
    }
    m_held.pop_front();
    m_inReaderSinceNs = now;
}

void VideoDecoder::ReleaseImage(AImage* image, EGLImageKHR egl) {
    if (egl != EGL_NO_IMAGE_KHR && pDestroyImage) {
        if (const EGLDisplay display = eglGetCurrentDisplay(); display != EGL_NO_DISPLAY)
            pDestroyImage(display, egl);
    }
    if (image) AImage_delete(image);
}

bool VideoDecoder::Latch(DecodedFrameInfo& out) {
    if (!m_reader) return false;
    AImage* image = nullptr;
    const media_status_t acquireStatus = AImageReader_acquireLatestImage(m_reader, &image);
    if (acquireStatus != AMEDIA_OK) {
        // No new picture is the normal result between decoder outputs, not a warning.
        const int acquireCode = int(acquireStatus);
        if (acquireCode != int(AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE) &&
            acquireCode != int(AMEDIACODEC_INFO_TRY_AGAIN_LATER))
            Warn("acquireLatestImage failed",
                 rc::StrFormat("status=%d image=%p", int(acquireStatus), static_cast<void*>(image)).c_str());
        return false;
    }
    if (!image) return false;
    int waiting = 0;
    {
        // Room in the reader again: the next picture the decoder holds goes in (see HandOver).
        std::lock_guard lock(m_heldMutex);
        m_inReaderSinceNs = 0;
        // A newer picture already decoded while this one is shown: the stream runs a frame
        // behind. After 500 ms of that at every display, skip a picture to catch up: the next
        // display then shows one decoded after it (no repeat, as one is decoded before each).
        // Not while that is held off (HoldCatchUp).
        const int64_t now = rc::NowNs();
        waiting = int(m_held.size());
        if (m_held.empty()) {
            m_behindSinceNs = 0;
        } else if (!m_behindSinceNs) {
            m_behindSinceNs = now;
        } else if (now - m_behindSinceNs > 500'000'000LL && now >= m_catchUpHeldUntilNs) {
            const size_t dropped = m_held.size();
            while (!m_held.empty()) {
                const size_t index = m_held.front();
                const media_status_t releaseStatus =
                    AMediaCodec_releaseOutputBuffer(m_codecHandle, index, false);
                if (releaseStatus != AMEDIA_OK) {
                    Fail("releaseOutputBuffer failed",
                         rc::StrFormat("status=%d index=%zu render=0 dropped=%zu held=%zu",
                                       int(releaseStatus), index, dropped, m_held.size())
                             .c_str());
                    break;
                }
                m_held.pop_front();
                m_skipped++;
            }
            m_behindSinceNs = 0;
        }
        if (!Failed()) HandOver();
    }
    if (Failed()) {
        ReleaseImage(image, nullptr);
        return false;
    }
    AHardwareBuffer* hb = nullptr;
    const media_status_t bufferStatus = AImage_getHardwareBuffer(image, &hb);
    if (bufferStatus != AMEDIA_OK || !hb)
        Warn("a decoded picture has no hardware buffer to show",
             rc::StrFormat("status=%d eglError=0x%x buffer=%p image=%p", int(bufferStatus),
                           static_cast<unsigned>(eglGetError()), static_cast<void*>(hb), static_cast<void*>(image))
                 .c_str());
    if (!pCreateImage || !pGetNativeClientBuffer || !pImageTargetTexture)
        Warn("EGL import procedure unavailable",
             rc::StrFormat("eglError=0x%x createImage=%d nativeClientBuffer=%d imageTargetTexture=%d",
                           static_cast<unsigned>(eglGetError()), int(pCreateImage != nullptr),
                           int(pGetNativeClientBuffer != nullptr), int(pImageTargetTexture != nullptr))
                 .c_str());
    if (!hb || !pCreateImage || !pGetNativeClientBuffer || !pImageTargetTexture) {
        AImage_delete(image);
        return false;
    }
    AHardwareBuffer_Desc desc{};
    AHardwareBuffer_describe(hb, &desc);
    if (!desc.width || !desc.height) {
        Warn("decoded image has an empty hardware buffer",
             rc::StrFormat("width=%u height=%u", desc.width, desc.height).c_str());
        AImage_delete(image);
        return false;
    }
    AImageCropRect crop{0, 0, int32_t(desc.width), int32_t(desc.height)};
    const media_status_t cropStatus = AImage_getCropRect(image, &crop);
    if (cropStatus != AMEDIA_OK ||
        !IsValidDecodedCrop(desc.width, desc.height, crop.left, crop.top, crop.right, crop.bottom)) {
        Warn("decoded image crop invalid",
             rc::StrFormat("status=%d width=%u height=%u crop=%d,%d-%d,%d", int(cropStatus), desc.width, desc.height,
                           crop.left, crop.top, crop.right, crop.bottom)
                 .c_str());
        crop = {0, 0, int32_t(desc.width), int32_t(desc.height)};
    }
    const float W = float(desc.width), H = float(desc.height);
    // Crop edges beside padding carry half-texel insets; publish them only after EGL import succeeds.
    const float x0 = (crop.left + (crop.left > 0 ? 0.5f : 0.f)) / W;
    const float y0 = (crop.top + (crop.top > 0 ? 0.5f : 0.f)) / H;
    const float x1 = (crop.right - (uint32_t(crop.right) < desc.width ? 0.5f : 0.f)) / W;
    const float y1 = (crop.bottom - (uint32_t(crop.bottom) < desc.height ? 0.5f : 0.f)) / H;
    const float nextCrop[4] = {x0, y0, x1 - x0, y1 - y0};
    const EGLDisplay display = eglGetCurrentDisplay();
    EGLClientBuffer clientBuffer = display != EGL_NO_DISPLAY ? pGetNativeClientBuffer(hb) : nullptr;
    if (!clientBuffer) {
        Warn("decoded picture had no EGL client buffer",
             rc::StrFormat("display=%d width=%d height=%d buffer=%p", int(display != EGL_NO_DISPLAY), m_width,
                           m_height, static_cast<void*>(hb))
                 .c_str());
        AImage_delete(image);
        return false;
    }
    const EGLint attribs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    EGLImageKHR egl = pCreateImage(display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, clientBuffer, attribs);
    if (egl == EGL_NO_IMAGE_KHR) {
        Warn("couldn't turn a decoded picture into a texture",
             rc::StrFormat("eglError=0x%x width=%d height=%d buffer=%p", static_cast<unsigned>(eglGetError()),
                           m_width, m_height, static_cast<void*>(hb))
                 .c_str());
        AImage_delete(image);
        return false;
    }
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, m_texture);
    pImageTargetTexture(GL_TEXTURE_EXTERNAL_OES, egl);
    const auto geometry = Geometry();
    if (geometry.width != int(desc.width) || geometry.height != int(desc.height) ||
        std::memcmp(nextCrop, geometry.crop, sizeof(nextCrop)) != 0) {
        RC_LOGI("Decoder output %ux%u, visible %d,%d-%d,%d", desc.width, desc.height, crop.left, crop.top,
                crop.right, crop.bottom);
    }
    PublishTextureGeometry(Texture(), int(desc.width), int(desc.height), nextCrop);

    // Keep the previous images alive briefly (GPU may still read them); shrink retired depth
    // when AImageReader fell back to 4 buffers to avoid starving the decoder output surface.
    const size_t maxRetired = m_readerMaxImages <= 4 ? 1u : 2u;
    if (m_image) m_retired.push_back({m_image, m_eglImage});
    while (m_retired.size() > maxRetired) {
        ReleaseImage(m_retired.front().first, m_retired.front().second);
        m_retired.pop_front();
    }
    m_image = image;
    m_eglImage = egl;

    int64_t ts = 0;
    const media_status_t timestampStatus = AImage_getTimestamp(image, &ts);
    const int64_t pts = ts / 1000;
    if (timestampStatus != AMEDIA_OK)
        Warn("AImage_getTimestamp failed",
             rc::StrFormat("status=%d timestamp=%lld pts=%lld width=%u height=%u", int(timestampStatus),
                           static_cast<long long>(ts), static_cast<long long>(pts), desc.width, desc.height)
                 .c_str());
    std::lock_guard lock(m_mutex);
    auto it = m_infos.find(pts);
    if (it != m_infos.end()) {
        out = it->second;
        m_infos.erase(m_infos.begin(), std::next(it));
    } else {
        Warn("metadata missing for decoded frame",
             rc::StrFormat("pts=%lld timestamp=%lld queuedFrame=%u width=%u height=%u", static_cast<long long>(pts),
                           static_cast<long long>(ts), m_lastQueued.load(), desc.width, desc.height)
                 .c_str());
        out = {};
        out.frameIndex = uint32_t(pts);
    }
    out.waiting = waiting;
    return true;
}

}  // namespace rv
