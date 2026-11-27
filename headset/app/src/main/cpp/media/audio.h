// Headset audio: PC audio playback and microphone capture (Opus), through AAudio on Android and
// PulseAudio (PipeWire on SteamOS) on Linux.
#pragma once

#ifdef __ANDROID__
#include <aaudio/AAudio.h>
#endif

#include <atomic>
#include <thread>
#include <functional>
#include <mutex>
#include <vector>

#include "rc/audio_codec.h"

namespace rv {

class AudioPlayer {
   public:
    ~AudioPlayer() { Stop(); }
    bool Start(bool lowLatencyPath = false);
    void Stop();
    // AudioPacketHeader + Opus payload from the network thread.
    void PushPacket(const uint8_t* data, size_t size);
    void SetVolume(float v) { m_volume = v; }
    int BufferedMs();

   private:
    bool StartLocked();
    void StopLocked();
#ifdef __ANDROID__
    static aaudio_data_callback_result_t Callback(AAudioStream*, void* user, void* audio, int32_t frames);
    static void ErrorCallback(AAudioStream*, void* user, aaudio_result_t error);
    AAudioStream* m_stream = nullptr;
#else
    void Run();
    void* m_stream = nullptr;  // pa_simple
    std::thread m_thread;
    std::atomic<bool> m_running{false};
#endif
    rc::AudioDecoder m_decoder;
    rc::JitterBuffer m_jitter;
    std::atomic<float> m_volume{1.f};
    std::atomic<bool> m_restart{false};
    bool m_lowLatencyPath = false;
    uint32_t m_lastSeq = 0;
    std::mutex m_mutex;
};

class MicCapture {
   public:
    using PacketSink = std::function<void(const uint8_t* data, size_t size)>;
    ~MicCapture() { Stop(); }
    bool Start(PacketSink sink);
    void Stop();
    bool Running() const { return m_stream != nullptr; }
    void PollRestart();
    void ScheduleRestart() { m_restart.store(true); }
    void SetMuted(bool m) { m_muted = m; }
    void SetGain(float g) { m_gain = g; }  // the microphone volume setting, 0 .. 2

   private:
    // Converts what the microphone delivers to 48 kHz mono and sends it in 10 ms packets.
    void OnSamples(const void* audio, int32_t frames);
#ifdef __ANDROID__
    static aaudio_data_callback_result_t Callback(AAudioStream*, void* user, void* audio, int32_t frames);
    static void ErrorCallback(AAudioStream*, void* user, aaudio_result_t error);
    AAudioStream* m_stream = nullptr;
#else
    void Run();
    void* m_stream = nullptr;  // pa_simple
    std::thread m_thread;
    std::atomic<bool> m_running{false};
#endif
    rc::AudioEncoder m_encoder;
    PacketSink m_sink;
    std::vector<int16_t> m_pending;
    uint32_t m_seq = 0;
    // What the microphone actually delivers (Android may not grant the 48 kHz mono we
    // ask for); converted to 48 kHz mono before encoding.
    int m_rate = 48000;
    int m_channels = 1;
    bool m_float = false;
    double m_resamplePos = 0;
    int16_t m_prevSample = 0;
    std::atomic<bool> m_restart{false};
    int64_t m_lastRestartNs = 0;
    std::atomic<bool> m_muted{false};
    std::atomic<float> m_gain{1.f};
    // Voice cleanup: 80 Hz high-pass filter + adaptive soft-knee noise gate.
    float m_hpfPrevIn = 0.f;
    float m_hpfPrevOut = 0.f;
    float m_noiseFloor = 0.004f;  // ~ -48 dBFS initial estimate
    float m_gateGain = 1.f;
    int m_gateHoldFrames = 0;
};

}  // namespace rv
