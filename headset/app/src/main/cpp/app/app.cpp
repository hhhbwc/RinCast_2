#include "app.h"

#ifdef __ANDROID__
#include <android/log.h>
#include <android_native_app_glue.h>
#include <sys/system_properties.h>
#else
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <cstdlib>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../platform/jni_bridge.h"
#include "../xr/thumbstick_deadzone.h"

#include <net/if.h>
#include "rc/frame_queue_bound.h"  // A23 (TASK-QUEUE-BOUND-2026-11-22): the decode queue bound
#include "rc/hand_skeleton.h"
#include "rc/platform.h"
#include "rc/protocol.h"
#include "rc/vr_queue_policy.h"
#include "stb/stb_image.h"
#include "strings.h"

namespace rv {

namespace screens {
void Build(App& app, WindowManager& wm);
void SetPage(int page);
void ToggleMenu(App& app);
}

static const char* kVersion = RINCAST_VERSION_STRING;

App::~App() { Shutdown(); }

#ifdef __ANDROID__
App::App(android_app* androidApp) : m_android(androidApp), m_assets(androidApp->activity->assetManager) {}

static void OnAppCmd(android_app* app, int32_t cmd) {
    auto* self = static_cast<App*>(app->userData);
    (void)self;
    switch (cmd) {
        case APP_CMD_RESUME: RC_LOGI("onResume"); break;
        case APP_CMD_PAUSE: RC_LOGI("onPause"); break;
        case APP_CMD_DESTROY: RC_LOGI("onDestroy"); break;
        default: break;
    }
}

void App::HandleAndroidEvents() {
    int events;
    android_poll_source* source;
    const int timeout = (!m_xr.Running() && !m_exit) ? 10 : 0;
    while (ALooper_pollOnce(timeout, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0) {
        if (source) source->process(m_android, source);
        if (m_android->destroyRequested) {
            m_exit = true;
            return;
        }
        if (timeout == 0) continue;
        break;
    }
}

void App::Run() {
    // Attached for good: PICO's runtime calls into Java from the app's thread.
    JNIEnv* env = nullptr;
    m_android->activity->vm->AttachCurrentThread(&env, nullptr);
    m_android->userData = this;
    m_android->onAppCmd = OnAppCmd;
    Init();
    while (!m_exit) {
        HandleAndroidEvents();
        if (m_exit) break;
        if (!m_xr.PollEvents()) {
            ANativeActivity_finish(m_android->activity);
            m_exit = true;
            break;
        }
        if (!m_xr.Running()) {
            ReleaseMicInBackground();
            m_music.Update(false);  // headset off or asleep: fade out
            continue;
        }
        Frame();
    }
    Shutdown();
    m_android->activity->vm->DetachCurrentThread();
}

#else
// Linux (the Steam Frame): a plain program that Steam starts. It ends when SteamVR ends the
// session or on SIGINT / SIGTERM.
namespace {
std::atomic<bool> g_quit{false};
void OnSignal(int) { g_quit = true; }
}  // namespace

App::App(android_app* androidApp) : m_android(androidApp) {}

void App::HandleAndroidEvents() {
    if (g_quit) m_exit = true;
}

void App::Run() {
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
    Init();
    while (!m_exit) {
        HandleAndroidEvents();
        if (m_exit) break;
        if (!m_xr.PollEvents()) {
            m_exit = true;
            break;
        }
        if (!m_xr.Running()) {
            ReleaseMicInBackground();
            m_music.Update(false);
            usleep(10'000);
            continue;
        }
        Frame();
    }
    Shutdown();
}
#endif

void App::Init() {
    Platform::Init(m_android);
    m_settingsPath = Platform::FilesDir() + "/settings.json";
    std::error_code firstRunEc;
    const bool firstRun = !std::filesystem::exists(m_settingsPath, firstRunEc);
    m_settings = HeadsetSettings::Load(m_settingsPath);
    m_xr.SetCompatPlainOpenXr(m_settings.compatPlainOpenXr);
    VideoDecoder::SetCompatPlain(m_settings.compatPlainOpenXr);
    SetLanguage(m_settings.language, Platform::SystemLocale());
    {
        rc::crypto::IdentitySeed seed{};
        if (!rc::HexDecode(m_settings.deviceSecret, seed.data(), seed.size())) {
            seed = rc::crypto::NewIdentitySeed();
            m_settings.deviceSecret = rc::HexEncode(seed.data(), seed.size());
            m_settings.Save(m_settingsPath);
        }
        m_client.SetIdentity(seed);
    }
    if (!m_xr.Init(m_android, Headset::Extensions()) || !(m_headset = Headset::Create(m_xr)) ||
        !m_xr.StartSession(m_headset->PreferStageSpace() || m_settings.fixedPlaySpace, m_headset->VelocitiesFromPoses())) {
        RC_LOGE("OpenXR initialization failed");
        m_exit = true;
        return;
    }
    RC_LOGI("Headset family: %s", m_headset->Family());
    const char* rawRenderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    std::string renderer = rawRenderer ? rawRenderer : "";
    std::string rLow = renderer;
    std::transform(rLow.begin(), rLow.end(), rLow.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    std::string platform = SystemProperty("ro.board.platform");
    std::transform(platform.begin(), platform.end(), platform.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    const auto hasR = [&](const char* s) { return rLow.find(s) != std::string::npos; };
    const auto hasP = [&](const char* s) { return platform.find(s) != std::string::npos; };
    const bool lowGpu = hasR("adreno (tm) 5") || hasR("adreno 5") ||
                        hasR("adreno (tm) 61") || hasR("adreno 61") ||
                        hasR("adreno (tm) 62") || hasR("adreno 62") ||
                        hasR("adreno (tm) 63") || hasR("adreno 63") ||
                        hasR("adreno (tm) 64") || hasR("adreno 64") ||
                        hasR("mali-g5") || hasR("mali-g72") || hasR("mali-g76") || hasR("mali-t") ||
                        hasR("powervr") || hasR("swiftshader") ||
                        hasP("msm8998") || hasP("sdm845") || hasP("sda845") ||
                        hasP("rk35") || hasP("mt67");
    const bool highGpu = hasR("adreno (tm) 73") || hasR("adreno (tm) 74") || hasR("adreno (tm) 75") ||
                         hasR("adreno (tm) 76") || hasR("adreno (tm) 8") ||
                         hasR("adreno 73") || hasR("adreno 74") || hasR("adreno 75") || hasR("adreno 8") ||
                         hasR("immortalis-g7") || hasR("immortalis-g9");
    m_gpuTier = lowGpu ? "Low" : highGpu ? "High" : "Mid";

    if (firstRun) {
        if (lowGpu) {
            m_settings.SetVrScale(0.6f);
            m_settings.vrSharpen = 0.0f;
            m_settings.upscaling = false;
            m_settings.frameGeneration = false;
            m_settings.refreshRate = 72.f;
            m_settings.vrBitrateMbps = 60;
        } else if (highGpu) {
            m_settings.SetVrScale(0.9f);
            m_settings.vrSharpen = 0.45f;
            m_settings.refreshRate = 90.f;
        } else {
            m_settings.SetVrScale(0.75f);
            m_settings.vrSharpen = 0.35f;
            m_settings.refreshRate = 72.f;
        }
        RC_LOGI("First-run hardware tier: %s (renderer='%s', platform='%s') -> scale=%.2f sharpen=%.2f fps=%.0f bitrate=%d",
                m_gpuTier.c_str(), renderer.c_str(), platform.c_str(), m_settings.vrScale, m_settings.vrSharpen,
                m_settings.refreshRate, m_settings.vrBitrateMbps);
        SaveSettings();
    }
    // The resolution used to count from the runtime's recommended size; it counts from the
    // panel now. The same pixels as before, at the new percentage.
    if (!m_settings.vrScaleOfPanel) {
        uint32_t fullW = 0, fullH = 0;
        m_headset->FullResolution(m_xr, fullW, fullH);
        m_settings.SetVrScale(m_settings.vrScale * float(m_xr.RecommendedWidth()) / float(std::max(1u, fullW)));
        m_settings.vrScaleOfPanel = true;
        SaveSettings();
    }
    Platform::RequestPermissions(m_headset->StartupPermissions());
    m_input.Init(m_xr, m_headset->Controllers());
    m_sky.Init(m_assets);
    m_music.Load(m_assets);
    m_ui.Init(m_assets);
    m_keyboard.Conf() = m_settings.keyboard;
    kb::Keyboard::Output kbOut;
    kbOut.key = [this](uint16_t vk, bool down) { SendKey(vk, down); };
    kbOut.text = [this](const std::string& t) { SendText(t); };
    kbOut.hide = [this] { ToggleWindow("keyboard", false); };
    kbOut.recenter = [this] {  // back to its spot in front of you, level again
        if (Window* w = m_windows.Find("keyboard")) {
            w->roll = 0;
            w->pose = m_keyboard.PlaceInFront(m_head);
        }
    };
    kbOut.saved = [this] {
        m_settings.keyboard = m_keyboard.Conf();
        SaveSettings();
    };
    if (!m_keyboard.Init(m_assets, kbOut)) RC_LOGE("Keyboard: its assets are missing");
    if (!m_scene.Init(m_xr.IsSrgbSwapchain())) {
        RC_LOGE("Scene renderer initialization failed");
        m_exit = true;
        m_xr.Shutdown();
        return;
    }
    m_windows.Init(&m_ui, &m_scene);
    m_xr.SetRefreshRate(m_settings.refreshRate);
    m_discovery.Start(Platform::DeviceId(), Platform::DeviceModel());
    m_cloud.Start(m_settings.cloudServer, m_settings.cloudRefreshToken, m_settings.cloudUserName,
                  [this](const std::string& refresh, const std::string& name) {
                      std::lock_guard lock(m_netMutex);
                      m_controlQueue.push_back(json{{"type", "_cloudTokens"}, {"refresh", refresh}, {"name", name}});
                  });

    StreamClient::Callbacks cb;
    cb.onControl = [this](const json& m) {
        std::lock_guard lock(m_netMutex);
        m_controlQueue.push_back(m);
    };
    cb.onVideoFrame = [this](rc::ReceivedFrame&& f) { OnVideoFrame(std::move(f)); };
    cb.onAudio = [this](const uint8_t* d, size_t n) {
        if (n > 0 && d[0] == uint8_t(rc::AudioStream::PcToHeadset)) m_audio.PushPacket(d, n);
    };
    cb.onHaptics = [this](const rc::HapticsPayload& h) {
        std::lock_guard lock(m_netMutex);
        m_controlQueue.push_back(json{{"type", "_haptic"}, {"hand", h.hand}, {"duration", h.durationS},
                                      {"frequency", h.frequency}, {"amplitude", h.amplitude}});
    };
    m_client.SetCallbacks(cb);

    CreateWindows();
    m_poseThreadStop = false;
    m_poseThread = std::thread([this] { PoseThread(); });
    m_lastFrameNs = rc::NowNs();
    m_initialized = true;
    RC_LOGI("RinCast %s ready", kVersion);
}

void App::Shutdown() {
    if (!m_initialized) return;
    m_initialized = false;
    {
        std::lock_guard lock(m_poseMutex);
        m_poseThreadStop = true;
        m_heldPose.reset();
    }
    m_poseCv.notify_all();
    if (m_poseThread.joinable()) m_poseThread.join();
    m_client.Disconnect();
    m_discovery.Stop();
    m_cloud.Stop();
    m_audio.Stop();
    m_mic.Stop();
    m_scene.ClearExternal();
    m_decoders.clear();
    for (auto& [id, sc] : m_monitorLayers) m_xr.DestroySwapchain(sc);
    for (auto& [id, sc] : m_uiLayers) m_xr.DestroySwapchain(sc);
    m_xr.DestroySwapchain(m_vrSwapchain);
    m_xr.DestroySwapchain(m_statsLayer);
    m_xr.Shutdown();
    Platform::SetStreamingLocks(false);
}

void App::SaveSettings() { m_settings.Save(m_settingsPath); }

// PC 端改了共享设置后推到一体机：本地回填并保存，但**不回传**（避免 PC↔一体机互相回显成环）。
void App::ApplyPcSettings(const json& info) {
    const json& s = info.value("settings", json::object());
    if (s.empty()) return;
    if (s.contains("codec")) m_settings.codec = s.value("codec", m_settings.codec);
    if (s.contains("tenBit")) m_settings.tenBit = s.value("tenBit", m_settings.tenBit);
    if (s.contains("adaptiveBitrate")) m_settings.adaptiveBitrate = s.value("adaptiveBitrate", m_settings.adaptiveBitrate);
    if (s.contains("desktopFps")) m_settings.desktopFps = s.value("desktopFps", m_settings.desktopFps);
    // A86 (user, 2026-11-23): "streamProfile" is deliberately NOT adopted any more. The
    // PC no longer sends it and this headset no longer shows it; a PC that still does
    // (older build) is ignored rather than writing a value no UI can change back.
    SaveSettings();
}

void App::ApplyStreamSettings() {
    SaveSettings();
    VideoDecoder::SetCompatPlain(m_settings.compatPlainOpenXr);
    m_xr.SetRefreshRate(m_settings.refreshRate);
    // ROUND18 L8-TELEM: the rate the display actually runs at with this config (0: could not read).
    RC_LOGI("Stream config: %.0f Hz asked, %.0f Hz on the display", m_settings.refreshRate, m_xr.RefreshRate());
    m_audio.SetVolume(m_settings.volume);
    m_mic.SetGain(m_settings.micVolume);
    // A new rate gets its own chance before the PC is told the display is elsewhere.
    m_displayHzForPc = 0;
    m_displayOffSince = -1;
    if (m_client.GetState() == StreamClient::State::Connected)
        m_client.SendControl({{"type", rc::msg::kUpdateSettings}, {"settings", StreamSettingsForPc()}});
    if (m_settings.micPassthrough && m_client.GetState() == StreamClient::State::Connected && !m_mic.Running())
        m_mic.Start([this](const uint8_t* d, size_t n) { m_client.SendAudio(d, n); });
    if (!m_settings.micPassthrough) m_mic.Stop();
}

json App::StreamSettingsForPc() {
    json s = m_settings.StreamSettingsJson();
    if (m_displayHzForPc > 0) s["refreshRate"] = m_displayHzForPc;
    // Without eye tracking, foveated encoding keeps a larger square fixed at the lens centre.
    s["eyesTracked"] = GazeSupported();
    // ROUND20 L17: the diagnostics panel's armed overrides ride the same push.
    if (m_forceFoveation != 2) s["gazeFoveation"] = m_forceFoveation == 1;
    // Always sent, 0 included: the PC keeps the last parsed value when the key is absent,
    // so omitting it at 0 would leave the override armed forever (ROUND20 L17 rework R1).
    s["debugEncodeScalePct"] = m_forceEncodeScale;
    return s;
}

// The PC renders at the rate it's told. When the display stays at another one (the runtime
// keeps its own, or lowers it when hot), frames at the wrong rate stutter: some are never
// shown, or some twice. Tell the PC what the display really runs at, and the setting again
// once the display gets there.
void App::FollowDisplayRate() {
    // Without the extension, or when it can't be read, the rate isn't known: leave the setting.
    if (m_time < m_displayCheckAt || !m_xr.Running() || !m_xr.HasExtension(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME)) return;
    m_displayCheckAt = m_time + 1.f;
    const float hz = m_xr.RefreshRate();
    if (hz <= 0) return;
    const bool off = std::fabs(hz - m_settings.refreshRate) > 0.5f;
    if (!off) m_displayOffSince = -1;
    else if (m_displayOffSince < 0) m_displayOffSince = m_time;
    // KeepRefreshRate asks again every two seconds: give it a few tries first.
    const float forPc = off && m_time - m_displayOffSince > 6.f ? hz : off ? m_displayHzForPc : 0;
    if (std::fabs(forPc - m_displayHzForPc) < 0.5f) return;
    m_displayHzForPc = forPc;
    RC_LOGI("The PC renders at %.0f Hz: the display is at %.0f, %.0f asked for", forPc > 0 ? forPc : m_settings.refreshRate,
            hz, m_settings.refreshRate);
    if (m_client.GetState() == StreamClient::State::Connected)
        m_client.SendControl({{"type", rc::msg::kUpdateSettings}, {"settings", StreamSettingsForPc()}});
}

// ---------------------------------------------------------------------------
// Connection management
// ---------------------------------------------------------------------------

void App::SendHello() {
    // (The display description is also sent in client.info once connected.)
}

static json HelloJson(XrContext& xr, const Headset& headset, const json& settings, const FrameState* frame) {
    json fov = json::array();
    float ipd = 0.063f;
    if (frame) {
        for (int e = 0; e < 2; e++) {
            const auto& f = frame->eyes[e].fov;
            fov.push_back({std::tan(-f.angleLeft), std::tan(f.angleRight), std::tan(f.angleUp), std::tan(-f.angleDown)});
        }
        ipd = Length(frame->eyes[0].pose.position - frame->eyes[1].pose.position);
        if (ipd < 0.05f || ipd > 0.08f) ipd = 0.063f;
    } else {
        fov = {{1.376, 0.839, 0.966, 1.191}, {0.839, 1.376, 0.966, 1.191}};
    }
    static const DecoderCaps dec = VideoDecoder::Capabilities();
    json rates = json::array();
    for (float r : xr.RefreshRates()) rates.push_back(r);
    // The size the PC scales the resolution setting by, and tells SteamVR is its 100%.
    uint32_t eyeW = 0, eyeH = 0;
    headset.FullResolution(xr, eyeW, eyeH);
    return {{"deviceId", Platform::DeviceId()},
            {"deviceName", Platform::DeviceModel()},
            {"model", Platform::DeviceModel()},
            {"appVersion", kVersion},
            {"display",
             {{"eyeWidth", eyeW},
              {"eyeHeight", eyeH},
              {"refreshRates", rates},
              {"ipd", ipd},
              {"fov", fov}}},
            {"decoders",
             {{"h264", dec.h264}, {"hevc", dec.hevc}, {"hevc10", dec.hevc10}, {"av1", dec.av1}, {"av110", dec.av110},
              {"maxWidth", dec.maxWidth}, {"maxHeight", dec.maxHeight},
              {"maxFpsAtMaxRes", dec.maxFpsAtMaxRes}}},
            {"settings", settings}};
}

static FrameState g_lastFrame;
static bool g_haveFrame = false;

void App::ConnectLan(const DiscoveredPc& pc) {
    m_haveLast = true;
    m_lastIsCloud = false;
    m_lastLan = pc;
    ConnectTarget t;
    t.name = pc.name;
    t.direct = pc.address;
    t.viaVpn = pc.viaVpn;
    if (pc.viaUsb) t.usbNetwork = m_usbNetwork.handle;
    m_settings.lastPc = pc.serverId;
    // A link-local address means nothing on another day or cable.
    if (!pc.viaUsb) m_settings.lastPcAddress = pc.address.IpString();
    SaveSettings();
    m_client.Connect(t, HelloJson(m_xr, *m_headset, StreamSettingsForPc(), g_haveFrame ? &g_lastFrame : nullptr));
    EnterMode(Mode::Connecting);
}

void App::SetUsbCable(bool on) {
    m_settings.usbCable = on;
    Platform::SetUsbNetwork(on);
    const Platform::UsbNetwork u = Platform::GetUsbNetwork();
    if (on && u.state == Platform::UsbNetwork::NotAllowed) {
        RC_LOGW("USB network rejected; not persisting usbCable=true");
        m_settings.usbCable = false;
    }
    SaveSettings();
    m_usbCheckedNs = 0;
}

void App::UpdateUsbNetwork() {
    const int64_t now = rc::NowNs();
    if (now - m_usbCheckedNs < 1'000'000'000LL) return;
    if (!m_usbCheckedNs && m_settings.usbCable) Platform::SetUsbNetwork(true);  // asked for at start
    m_usbCheckedNs = now;
    const Platform::UsbNetwork u = Platform::GetUsbNetwork();
    if (u.state == Platform::UsbNetwork::NotAllowed && m_settings.usbCable) {
        RC_LOGW("USB network not allowed; reverting usbCable setting");
        m_settings.usbCable = false;
        SaveSettings();
    }
    const bool changed = u.state != m_usbNetwork.state || u.handle != m_usbNetwork.handle || u.interface != m_usbNetwork.interface;
    m_usbNetwork = u;
    if (!changed) return;
    const uint32_t index = u.state == Platform::UsbNetwork::Connected && !u.interface.empty() ? if_nametoindex(u.interface.c_str()) : 0;
    RC_LOGI("USB network: state %d, interface %s (%u), %d kbps", int(u.state), u.interface.c_str(), index, u.kbps);
    m_discovery.SetUsbNetwork(index ? u.handle : 0, index);
}

void App::ConnectCloud(const CloudPc& pc) {
    m_haveLast = true;
    m_lastIsCloud = true;
    m_lastCloud = pc;
    m_settings.lastPc = pc.id;
    SaveSettings();
    EnterMode(Mode::Connecting);
    // If the PC is also on this network, prefer the direct path.
    for (const auto& lan : m_discovery.List()) {
        if (lan.serverId == pc.id) {
            ConnectLan(lan);
            return;
        }
    }
    m_cloud.Connect(pc.id, pc.name, [this](bool ok, ConnectTarget target, std::string error) {
        std::lock_guard lock(m_netMutex);
        if (!ok) {
            m_controlQueue.push_back(json{{"type", "_connectFailed"}, {"error", error}});
            return;
        }
        // Hand the target to the main thread (it owns the client lifecycle).
        json t{{"type", "_cloudTarget"},
               {"name", target.name},
               {"session", target.cloudSession},
               {"secret", target.connectSecret},
               {"token", target.sessionToken}};
        json rendezvous = json::array(), locals = json::array();
        for (const auto& a : target.rendezvous) rendezvous.push_back(a.ToString());
        for (const auto& a : target.pcLocalAddrs) locals.push_back(a.ToString());
        t["rendezvous"] = rendezvous;
        t["locals"] = locals;
        m_controlQueue.push_back(t);
    });
}

// Called from window contents, i.e. while windows are being drawn: tearing the session
// down there would delete monitor windows mid-iteration. Do it at the start of the next frame.
void App::Disconnect() { m_disconnectRequested = true; }

void App::DisconnectNow() {
    m_disconnectRequested = false;
    m_client.Disconnect();
    m_audio.Stop();
    m_mic.Stop();
    Platform::SetStreamingLocks(false);
    {
        std::lock_guard lock(m_decoderMutex);
        m_decoders.clear();
    }
    m_scene.ClearExternal();
    for (auto& [id, ds] : m_desktopStreams) m_windows.Remove("monitor-" + std::to_string(id));
    m_desktopStreams.clear();
    std::erase_if(m_monitorLayers, [&](auto& entry) {
        m_monitorFresh[entry.first] = false;
        return m_xr.DestroySwapchain(entry.second);
    });
    m_xr.DestroySwapchain(m_vrSwapchain);
    m_vrHasFrame = false;
    m_vrState = "idle";
    // A new connection starts from nothing: nothing was left behind to stay away from.
    m_vrLeftByUser = false;
    m_vrLastActive = -1e9f;
    m_serverInfo = json::object();
    // The diagnosis short code lives and dies with the session's server.info (the line above).
    m_client.SetDiagnosis("");
    m_games.clear();
    m_monitorsLaidOut = false;
    m_decoderRetryNs.clear();
    m_motionFrameNs = 0;
    m_motionIntervalSmoothNs = 0;
    m_motionConsistent = false;
    m_vrLatchedDisplayNs = 0;
    EnterMode(Mode::Home);
}

void App::LaunchSteamVr() {
    m_client.SendControl({{"type", rc::msg::kLaunchSteamVr}});
    EnterMode(Mode::LaunchingVr);
}

void App::LaunchGame(const std::string& id) {
    m_client.SendControl({{"type", rc::msg::kLaunchGame}, {"id", id}});
    EnterMode(Mode::LaunchingVr);
}

void App::ExitVr(bool byUser) {
    m_client.SendControl({{"type", rc::msg::kStopVr}});
    // A game that keeps running keeps reporting "running", and a stream restart reports
    // "waiting" and then "running" whenever it happens; none of that is a reason to pull
    // the user back in. When the user asks to leave, stay on the desktop until they ask to
    // go back (the dock's "Back to VR", the Games list and launching SteamVR all go through
    // EnterMode(LaunchingVr) and undo this). Leaving because the game ended is not a choice
    // and pins nothing.
    if (byUser) m_vrLeftByUser = true;
    // TASK-OPLOGIC-FIX-HMD-2026-11-22 §1.2: the pull-back itself is unchanged; leaving because
    // the game ended just says so now, instead of returning to the desktop without a word.
    if (!byUser) PostNotice(Tr("notice.gameStopped"));
    EnterMode(Mode::Desktop);
}

void App::Recenter() {
    LayoutMonitors(true);
    LayoutHome(true);
}

void App::UpdateScreenShape(float previousDistance) {
    const float distance = m_settings.screenDistance;
    for (auto& [id, d] : m_desktopStreams) {
        Window* w = m_windows.Find("monitor-" + std::to_string(id));
        if (!w) continue;
        w->curveRadius = m_settings.curvature > 0.02f ? distance / std::max(0.05f, m_settings.curvature) : 0.f;
        w->layerBacked = m_settings.sharpLayers && (w->curveRadius == 0 || m_xr.CylinderLayers());
        if (distance != previousDistance) {
            // Slide along the line from the head, so the arrangement keeps its shape.
            const Vec3 from = w->pose.position - m_head.position;
            w->pose.position = m_head.position + from * (distance / std::max(0.2f, previousDistance));
            w->widthM *= distance / std::max(0.2f, previousDistance);
        }
    }
}

void App::RetryConnection() {
    if (!m_haveLast) return;
    m_problem.clear();
    if (m_lastIsCloud) ConnectCloud(m_lastCloud);
    else ConnectLan(m_lastLan);
}

// A connection that dropped by itself gets a few chances before the user has to act.
void App::ReconnectTick() {
    if (!Reconnecting() || m_time - m_lastRetry < 4.f) return;
    m_lastRetry = m_time;
    m_retryAttempt++;
    RetryConnection();
}

void App::SetManualPc(const std::string& address) {
    m_settings.manualPc = address;
    SaveSettings();
    rc::NetAddress a;
    if (rc::NetAddress::Parse(address, rc::kDiscoveryPort, a)) m_discovery.AddProbeTarget(a);
}

float App::TimeOnHomeWithoutPcs(bool noPcs) {
    if (!noPcs) m_noPcsSince = -1;
    else if (m_noPcsSince < 0) m_noPcsSince = m_time;
    return noPcs ? m_time - m_noPcsSince : 0;
}

void App::ApplyAccessibility() {
    theme::SetHighContrast(m_settings.highContrast);
    for (const auto& w : m_windows.All()) {
        if (w->kind == WindowKind::Ui && !w->twoHands) {  // (the keyboard has its own size, from its resize corner)
            if (w->baseWidthM <= 0) w->baseWidthM = w->widthM;
            w->widthM = w->baseWidthM * std::min(m_settings.uiScale, 1.3f);
            w->needsRedraw = true;
        }
    }
}

void App::SendKey(uint16_t vk, bool down) { m_client.SendControl({{"type", rc::msg::kKey}, {"vk", vk}, {"down", down}}); }

void App::TogglePcKeyboard() { m_client.SendControl({{"type", rc::msg::kPcKeyboard}}); }

void App::SendText(const std::string& text) { m_client.SendControl({{"type", rc::msg::kText}, {"text", text}}); }

void App::PostNotice(const std::string& text, const std::string& level, const std::string& action) {
    // An error, or one with something to do, stays until it's answered.
    m_notices.push_back({text, level, action, 0, !action.empty() || level == "error" ? 3600.f : level == "warning" ? 10.f : 6.f});
    if (m_settings.readAloud) Platform::Speak(text);
    while (m_notices.size() > 3) m_notices.pop_front();
    ToggleWindow("notices", true);
}

void App::DismissNotice() {
    if (!m_notices.empty()) m_notices.pop_back();
    if (m_notices.empty()) ToggleWindow("notices", false);
}

const Texture& App::EnvironmentThumbnail(const std::string& key) {
    auto it = m_envThumbnails.find(key);
    if (it != m_envThumbnails.end()) return it->second;
    Texture& t = m_envThumbnails[key];
    std::vector<uint8_t> bytes;
    if (m_assets.Read(("ui/env-" + key + ".png").c_str(), bytes)) LoadImageTexture(bytes, 4, true, true, t);
    return t;
}

void App::RunAction(const std::string& action) {
    if (action == "vr.retry") {
        LaunchSteamVr();
    } else if (action == "vr.restartSteamVR") {
        m_client.SendControl({{"type", action}});
        EnterMode(Mode::LaunchingVr);
    } else if (!action.empty()) {
        m_client.SendControl({{"type", action}});
    }
}

void App::RequestGames() {
    m_gamesRequestedAt = m_time;
    m_gamesAnswered = false;
    m_client.SendControl({{"type", rc::msg::kListGames}});
}

void App::RequestGameImage(GameInfo& g) {
    if (g.imageRequested || !g.hasImage) return;
    g.imageRequested = true;
    m_client.SendControl({{"type", "games.image"}, {"id", g.id}});
}

std::string App::VrStateText() const {
    if (m_vrState == "launching") return m_vrProducer == "openxr" ? Tr("vrState.startingGame") : Tr("vrState.startingSteamvr");
    if (m_vrState == "running") return m_vrApp.empty() ? Tr("vrState.running") : m_vrApp;
    if (m_vrState == "error") return Tr("vrState.error");
    return Tr("vrState.ready");
}

bool App::IsVisible(const std::string& id) {
    Window* w = m_windows.Find(id);
    return w && w->visible;
}

void App::ToggleWindow(const std::string& id, bool show) {
    Window* w = m_windows.Find(id);
    if (!w) return;
    if (show && !w->visible) {
        // Re-open in front of the user if it's far behind them.
        const Vec3 toWin = Normalize(w->pose.position - m_head.position);
        // Transient panels always open where the user is looking; the dock, stats and
        // overlays keep their own slot so they never stack on top of each other.
        const bool transient = id == "home" || id == "keyboard" || id == "vr-overlay" || id == "notices" || id == "launch";
        if (transient || !m_placed.count(id) || Dot(toWin, m_head.Forward()) < 0.3f) {
            if (id == "keyboard") {
                w->roll = 0;
                w->pose = m_keyboard.PlaceInFront(m_head);
            }
            else if (id == "dock")
                w->pose = WindowManager::PlaceInFront(m_head, 0.9f, 0, -0.50f, 0);
            else if (id == "stats")
                w->pose = WindowManager::PlaceInFront(m_head, 1.1f, 38.f, 0.18f, 0);
            else if (id == "notices")
                w->pose = WindowManager::PlaceInFront(m_head, 1.0f, 0, 0.40f, 0);  // above the main panel, not over it
            else if (id == "launch")
                w->pose = WindowManager::PlaceInFront(m_head, 1.0f, 0, -0.18f, 0);
            else if (id == "vr-overlay")
                w->pose = WindowManager::PlaceInFront(m_head, 0.95f, 0, -0.12f, 0);
            else
                w->pose = WindowManager::PlaceInFront(m_head, 1.3f, 0, -0.05f, 0);
            m_placed.insert(id);
            w->appear = 0;
        }
    }
    w->visible = show;
    // In VR the controllers go to RinCast instead of the game while it's open, however it closes.
    if (id == "vr-overlay") m_vrOverlay = show && m_mode == Mode::Vr;
}

void App::ShowMonitors(bool show) {
    m_monitorsVisible = show;
    for (auto& [id, ds] : m_desktopStreams)
        if (Window* w = m_windows.Find("monitor-" + std::to_string(id))) w->visible = show && m_mode == Mode::Desktop;
}

// ---------------------------------------------------------------------------
// Modes
// ---------------------------------------------------------------------------

void App::EnterMode(Mode m) {
    if (m == m_mode) return;
    RC_LOGI("Mode %d -> %d", int(m_mode), int(m));
    const Mode prev = m_mode;
    m_mode = m;
    m_modeTime = 0;
    switch (m) {
        case Mode::Home:
            for (auto& w : m_windows.All()) w->visible = false;
            ToggleWindow("home", true);
            m_warp = 0;
            m_arrival = 0;
            break;
        case Mode::Connecting:
            ToggleWindow("home", true);
            break;
        case Mode::Desktop:
            ToggleWindow("home", false);
            ToggleWindow("launch", false);
            ToggleWindow("vr-overlay", false);
            ToggleWindow("dock", true);
            if (m_settings.showStats) ToggleWindow("stats", true);
            m_arrival = 0;
            if (prev == Mode::Vr || prev == Mode::LaunchingVr) {
                m_client.SendControl({{"type", rc::msg::kStartDesktop}});
            }
            ShowMonitors(m_monitorsVisible);
            break;
        case Mode::LaunchingVr:
            // Going back to VR is the user's own doing (deck, Games list, SteamVR): whatever
            // they left before is no longer a reason to stay on the desktop.
            m_vrLeftByUser = false;
            // Everything floats away; hyperspace takes over.
            for (auto& w : m_windows.All())
                if (w->id != "notices") w->visible = false;
            m_tunnelOrientation = Quat::Yaw(std::atan2(-m_head.Forward().x, -m_head.Forward().z));
            m_warpSpeed = 0.15f;
            m_arrival = 0;
            // Wait for a new picture, not the one from when VR was left.
            m_vrHasFrame = false;
            m_motionMoves = false;
            m_vrNoPicture = 0;
            // The desktop isn't shown in VR: stop its streams and free their decoders before the
            // VR decoder starts. A Quest 2 doesn't have the decoding hardware for both at once, and
            // the bandwidth is the game's. Leaving VR starts the desktop again.
            m_client.SendControl({{"type", rc::msg::kStopDesktop}});
            {
                std::lock_guard lock(m_decoderMutex);
                for (const auto& [id, d] : m_desktopStreams) m_decoders.erase(id);
            }
            m_client.SendControl({{"type", rc::msg::kStartVr}});
            ToggleWindow("launch", true);
            break;
        case Mode::Vr:
            for (auto& w : m_windows.All()) w->visible = false;
            // Free bandwidth for the game.
            m_client.SendControl({{"type", rc::msg::kStopDesktop}});
            if (m_settings.vrMenuHints < 3) {
                m_settings.vrMenuHints++;
                SaveSettings();
                PostNotice(Tr("notice.menuHint"), "info");
            }
            break;
    }
}

// Developer hooks for testing without controllers (e.g. in an emulator):
//   adb shell setprop debug.rincast.ui home,keyboard       windows to show (others are hidden)
//   adb shell setprop debug.rincast.tab 2                  page of the main panel
//   adb shell setprop debug.rincast.pc 10.0.2.2            extra PC address to probe
// And for measuring a VR stream's timing against the headset's log ("VR frame timing", "VR
// frame arrival"), changed while it runs:
//   adb shell setprop debug.rincast.prelatch_ms 1          that much extra work before the game's
//                                                          picture is picked up (0 to 5 ms): does
//                                                          the headset wake the app earlier for it?
//   adb shell setprop debug.rincast.slip 1                 ask the PC to let waiting pictures run
//                                                          out (SteamVR games); off by default, as
//                                                          a picture waiting measured faster
//   adb shell setprop debug.rincast.nowait 1                with slip, try no waiting picture even
//                                                          when the arrival margin exceeds 0.75 frame
//   adb shell setprop debug.rincast.pose_hold 0            send poses as soon as they are located,
//                                                          not held back for SteamVR's pickup
// On Linux the same come from the environment: DEBUG_RINCAST_PC, DEBUG_RINCAST_TAB, DEBUG_RINCAST_UI,
// DEBUG_RINCAST_PRELATCH_MS, DEBUG_RINCAST_SLIP, DEBUG_RINCAST_NOWAIT, DEBUG_RINCAST_POSE_HOLD.
static void DebugProperty(const char* name, char (&value)[92]) {
#ifdef __ANDROID__
    __system_property_get(name, value);
#else
    std::string env = name;
    for (char& c : env) c = c == '.' ? '_' : char(std::toupper(static_cast<unsigned char>(c)));
    const char* v = std::getenv(env.c_str());
    std::snprintf(value, sizeof(value), "%s", v ? v : "");
#endif
}

void App::PollDebugProperties() {
    if (m_time - m_lastDebugPoll < 1.f) return;
    m_lastDebugPoll = m_time;
    char value[92] = {};  // PROP_VALUE_MAX
    DebugProperty("debug.rincast.pc", value);
    if (value[0] && m_debugPc != value) {
        m_debugPc = value;
        rc::NetAddress a;
        if (rc::NetAddress::Parse(m_debugPc, rc::kDiscoveryPort, a)) m_discovery.AddProbeTarget(a);
    }
    value[0] = 0;
    DebugProperty("debug.rincast.tab", value);
    if (value[0] && m_debugTab != value) {
        m_debugTab = value;
        screens::SetPage(std::atoi(value));
    }
    value[0] = 0;
    DebugProperty("debug.rincast.ui", value);
    if (value[0] && m_debugUi != value) {
        m_debugUi = value;
        const std::string list = "," + m_debugUi + ",";
        for (const char* id : {"home", "dock", "keyboard", "stats", "vr-overlay"})
            ToggleWindow(id, list.find(std::string(",") + id + ",") != std::string::npos);
        RC_LOGI("debug.rincast.ui = %s", m_debugUi.c_str());
    }
    value[0] = 0;
    DebugProperty("debug.rincast.prelatch_ms", value);
    if (const float ms = std::clamp(float(std::atof(value)), 0.f, 5.f); ms != m_debugPreLatchMs) {
        m_debugPreLatchMs = ms;
        RC_LOGI("debug.rincast.prelatch_ms = %.1f: that much extra work before the game's picture is picked up", ms);
    }
    value[0] = 0;
    DebugProperty("debug.rincast.slip", value);
    if (const bool slip = value[0] == '1'; slip != m_debugSlip) {
        m_debugSlip = slip;
        RC_LOGI("debug.rincast.slip = %d: the PC is %s to let waiting pictures run out", int(slip),
                slip ? "asked" : "never asked");
    }
    value[0] = 0;
    DebugProperty("debug.rincast.nowait", value);
    if (const bool nowait = value[0] == '1'; nowait != m_debugNoWait) {
        m_debugNoWait = nowait;
        RC_LOGI("debug.rincast.nowait = %d: %s", int(nowait),
                nowait ? "the no-waiting-picture experiment is armed; slip and SteamVR are still required"
                       : "the no-waiting-picture experiment is off");
    }
    value[0] = 0;
    DebugProperty("debug.rincast.pose_hold", value);
    if (const bool hold = value[0] != '0'; hold != m_debugPoseHold) {
        m_debugPoseHold = hold;
        m_poseHoldNs = 0;
        RC_LOGI("debug.rincast.pose_hold = %d: poses are %s", int(hold),
                hold ? "held back until just before SteamVR takes them" : "sent as soon as located");
    }
}

void App::UpdateMode(float dt) {
    m_modeTime += dt;
    m_discovery.SetSearching(m_mode == Mode::Home);
    PollDebugProperties();
    UpdateUsbNetwork();
    // Until a PC is connected, and not while it's being reconnected to after a drop: the
    // music would only start to stop again.
    m_music.Update(m_settings.music && (m_mode == Mode::Home || m_mode == Mode::Connecting) && m_xr.Visible() &&
                   !Reconnecting());
    const auto clientState = m_client.GetState();
    switch (m_mode) {
        case Mode::Home:
            ReconnectTick();
            break;
        case Mode::Connecting:
            if (clientState == StreamClient::State::Connected) {
                // Introduce ourselves, then show the desktop.
                json info = HelloJson(m_xr, *m_headset, StreamSettingsForPc(), g_haveFrame ? &g_lastFrame : nullptr);
                info["type"] = rc::msg::kClientInfo;
                m_client.SendControl(info);
                Platform::SetStreamingLocks(true);
                if (m_settings.streamAudio) m_audio.Start(!m_client.IsRemote() && !m_client.IsVpn());
                m_audio.SetVolume(m_settings.volume);
                m_mic.SetGain(m_settings.micVolume);
                if (m_settings.micPassthrough)
                    m_mic.Start([this](const uint8_t* d, size_t n) { m_client.SendAudio(d, n); });
                // Straight into SteamVR when asked, but not on a reconnect: one that comes back
                // while a game runs goes back into it anyway (kVrState), and one that comes back
                // on the desktop stays there.
                const bool launchSteamVr = m_settings.autoLaunchSteamVr && !Reconnecting();
                if (!launchSteamVr) m_client.SendControl({{"type", rc::msg::kStartDesktop}});
                m_problem.clear();
                m_retryUntil = 0;
                m_retryAttempt = 0;
                if (m_settings.uiSounds) Platform::PlaySound(Platform::Sound::Connected);
                if (launchSteamVr)
                    LaunchSteamVr();
                else
                    EnterMode(Mode::Desktop);
            } else if (clientState == StreamClient::State::Failed) {
                m_problem = m_client.StatusText();
                if (m_settings.uiSounds && !Reconnecting()) Platform::PlaySound(Platform::Sound::Problem);
                m_client.Disconnect();
                EnterMode(Mode::Home);
            }
            break;
        case Mode::Desktop:
        case Mode::LaunchingVr:
        case Mode::Vr:
            if (clientState == StreamClient::State::Failed || clientState == StreamClient::State::Idle) {
                const std::string status = m_client.StatusText();
                DisconnectNow();
                // Anything but the PC ending it on purpose is worth another try.
                if (status != "The PC ended the session." && status != Tr("notice.pcEndedSession") && !status.empty() && m_haveLast) {
                    m_problem = status;
                    m_retryUntil = m_time + 20.f;
                    m_retryAttempt = 0;
                    m_lastRetry = m_time - 3.f;  // first try almost at once
                    if (m_settings.uiSounds) Platform::PlaySound(Platform::Sound::Problem);
                } else {
                    PostNotice(status.empty() ? Tr("notice.disconnected") : status, "warning");
                }
                return;
            }
            break;
    }

    if (m_mode == Mode::Desktop || m_mode == Mode::LaunchingVr || m_mode == Mode::Vr) RecoverFailedDecoders();

    // Hyperspace choreography.
    if (m_mode == Mode::LaunchingVr) {
        m_warp = Damp(m_warp, 1.f, 3.5f, dt);
        m_warpSpeed = std::min(1.f, m_warpSpeed + dt * 0.45f);  // accelerate into the jump
        if (m_vrState == "running" && m_vrHasFrame) EnterMode(Mode::Vr);
        // The PC sends the game, but no picture comes out of the decoder: say so, instead of
        // blaming SteamVR when the wait runs out.
        if (m_vrState == "running" && !m_vrHasFrame) m_vrNoPicture += dt;
        std::shared_ptr<VideoDecoder> vrDecoder;
        {
            std::lock_guard lock(m_decoderMutex);
            if (auto it = m_decoders.find(rc::kVrStreamId); it != m_decoders.end()) vrDecoder = it->second;
        }
        if (m_mode == Mode::LaunchingVr && m_vrNoPicture > 0 && (m_vrNoPicture > 15.f || (m_vrNoPicture > 3.f && vrDecoder && vrDecoder->Failed()))) {
            RC_LOGW("VR: the PC has been sending for %.0f s, but no picture could be shown. Decoder: %s, started %d, "
                    "failed %d, %u frames decoded, %dx%d",
                    m_vrNoPicture, vrDecoder ? "present" : "missing", vrDecoder && vrDecoder->Started(),
                    vrDecoder && vrDecoder->Failed(), vrDecoder ? vrDecoder->DecodedFrames() : 0u, m_vrEyeWidth * 2,
                    m_vrEyeHeight);
            PostNotice(Tr("notice.cantShow"), "error", "vr.retry");
            EnterMode(Mode::Desktop);
        } else if (m_mode == Mode::LaunchingVr && (m_vrState == "error" || m_modeTime > 100.f)) {
            PostNotice(m_vrState == "error" ? Tr("notice.vrError") : Tr("notice.vrTimeout"), "error", "vr.retry");
            EnterMode(Mode::Desktop);
        }
    } else if (m_mode == Mode::Vr) {
        // Arrival: a final burst of speed, then fade into the stream.
        m_arrival = std::min(1.f, m_arrival + dt / 0.9f);
        m_warpSpeed = m_arrival < 0.35f ? 1.f + m_arrival * 8.f : Damp(m_warpSpeed, 0.f, 4.f, dt);
        m_warp = 1.f - SmoothStep(0.25f, 1.f, m_arrival);
        // The game closed: back to the desktop after a short grace period.
        if (m_vrState != "running" && m_modeTime > 3.f) {
            static float idle = 0;
            idle += dt;
            if (idle > 2.f) {
                idle = 0;
                ExitVr();
            }
        }
    } else {
        m_warp = Damp(m_warp, 0.f, 6.f, dt);
        m_warpSpeed = Damp(m_warpSpeed, 0.f, 3.f, dt);
    }
    m_warpPhase += m_warpSpeed * dt;

    for (auto& n : m_notices) n.age += dt;
    while (!m_notices.empty() && m_notices.front().age > m_notices.front().life) m_notices.pop_front();
    if (m_notices.empty()) ToggleWindow("notices", false);
}

// ---------------------------------------------------------------------------
// Network
// ---------------------------------------------------------------------------

void App::ProcessNetwork() {
    std::vector<json> queue;
    {
        std::lock_guard lock(m_netMutex);
        queue.swap(m_controlQueue);
    }
    for (const auto& m : queue) HandleControl(m);
}

static bool DecodeImage(const std::string& b64, Texture& out) {
    static const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<uint8_t> bytes;
    uint32_t buf = 0;
    int bits = 0;
    for (char c : b64) {
        const size_t p = chars.find(c);
        if (p == std::string::npos) continue;
        buf = (buf << 6) | uint32_t(p);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bytes.push_back(uint8_t(buf >> bits));
        }
    }
    return !bytes.empty() && LoadImageTexture(bytes, 4, true, true, out);
}

void App::HandleControl(const json& m) {
    const std::string type = m.value("type", "");
    if (type == "_cloudTokens") {
        m_settings.cloudRefreshToken = m.value("refresh", "");
        m_settings.cloudUserName = m.value("name", "");
        SaveSettings();
    } else if (type == "_connectFailed") {
        m_problem = m.value("error", Tr("computers.couldntConnect"));
        EnterMode(Mode::Home);
    } else if (type == "_cloudTarget") {
        ConnectTarget t;
        t.name = m.value("name", "");
        t.cloudSession = m.value("session", "");
        t.connectSecret = m.value("secret", "");
        t.sessionToken = m.value("token", "");
        auto endpoints = [](const json& list, std::vector<rc::NetAddress>& out) {
            for (const auto& e : list) {
                rc::NetAddress a;
                if (e.is_string() && rc::NetAddress::ParseEndpoint(e.get<std::string>(), a)) out.push_back(a);
            }
        };
        endpoints(m.value("rendezvous", json::array()), t.rendezvous);
        endpoints(m.value("locals", json::array()), t.pcLocalAddrs);
        m_client.Connect(t, HelloJson(m_xr, *m_headset, StreamSettingsForPc(), g_haveFrame ? &g_lastFrame : nullptr));
    } else if (type == "_haptic") {
        m_input.Vibrate(m.value("hand", 0), m.value("amplitude", 0.f), std::max(0.02f, m.value("duration", 0.05f)),
                        m.value("frequency", 0.f) > 0 ? m.value("frequency", 0.f) : XR_FREQUENCY_UNSPECIFIED);
    } else if (type == rc::msg::kServerInfo) {
        m_serverInfo = m;
        // TASK-OPLOGIC-FIX-HMD-2026-11-22 §1.3: the PC's optional diagnosis short code (A21
        // adds it PC-side; absent on old PCs) is what connect-failure text keys off of.
        m_client.SetDiagnosis(m.value("diagnosis", ""));
        ApplyPcSettings(m);  // 回填 PC 端改过的可同步设置（不回传）
    } else if (type == rc::msg::kMonitors) {
        m_monitors.clear();
        for (const auto& mon : m.value("monitors", json::array()))
            m_monitors.push_back({mon.value("id", ""), mon.value("name", ""), mon.value("index", 0),
                                  mon.value("width", 0), mon.value("height", 0), mon.value("primary", false)});
    } else if (type == rc::msg::kStreamConfig) {
        if (m.value("kind", "") == "desktop")
            ConfigureDesktopStreams(m.value("streams", json::array()));
        else if (!m.value("streams", json::array()).empty())
            ConfigureVrStream(m["streams"][0]);
    } else if (type == rc::msg::kVrState) {
        const bool wasActive = m_vrState == "launching" || m_vrState == "running";
        m_vrState = m.value("state", "idle");
        m_vrProducer = m.value("producer", "");
        m_vrApp = m.value("app", "");
        if (m.contains("error")) {
            std::string err = m.value("error", "");
            if (m.contains("errorKey")) {
                const std::string ek = m.value("errorKey", "");
                const char* tr = Tr(ek.c_str());
                if (tr != ek) err = tr;
            } else if (err == "SteamVR did not start. Check SteamVR on your PC.") {
                err = Tr("notice.steamVrDidNotStart");
            } else if (err == "SteamVR is not installed on this PC.") {
                err = Tr("notice.steamVrNotInstalled");
            }
            PostNotice(err, "error");
        }
        const bool vrActive = m_vrState == "launching" || m_vrState == "running";
        // A VR app started from the PC: jump in. Only one that starts after VR has been
        // inactive for a while is new; the PC restarts the VR stream whenever a video setting
        // changes (every step of a slider) or SteamVR's resolution moves, and that reports
        // "waiting" and then "running" again within a second or two. Taking those for a new
        // game pulled the user out of the settings and into VR on every change.
        constexpr float kNewVrAppAfter = 10.f;
        const bool newVrApp = vrActive && !wasActive && m_time - m_vrLastActive > kNewVrAppAfter;
        if (vrActive || wasActive) m_vrLastActive = m_time;
        // A new one also ends a deliberate leave: that app is gone.
        if (newVrApp) m_vrLeftByUser = false;
        // Never while one the user deliberately left is still running there.
        if (newVrApp && !m_vrLeftByUser && m_mode == Mode::Desktop) EnterMode(Mode::LaunchingVr);
    } else if (type == rc::msg::kGames) {
        m_gamesAnswered = true;
        std::vector<GameInfo> games;
        for (const auto& g : m.value("games", json::array())) {
            GameInfo gi;
            gi.id = g.value("id", "");
            gi.name = g.value("name", "");
            gi.hasImage = g.value("hasImage", false);
            for (auto& old : m_games)
                if (old.id == gi.id && old.image.id) {
                    gi.image = old.image;
                    gi.imageRequested = true;
                    old.image = {};
                }
            games.push_back(std::move(gi));
        }
        for (auto& old : m_games) old.image.Destroy();
        m_games = std::move(games);
    } else if (type == "games.image") {
        for (auto& g : m_games)
            if (g.id == m.value("id", "") && !g.image.id) DecodeImage(m.value("data", ""), g.image);
    } else if (type == rc::msg::kServerStats) {
        m_serverStats = m;
    } else if (type == rc::msg::kVrPoseWait) {
        OnPoseWait(m);
    } else if (type == rc::msg::kNotice) {
        std::string text = m.value("text", "");
        const std::string action = m.value("action", "");
        if (m.contains("key")) {
            const std::string key = m.value("key", "");
            const char* tr = Tr(key.c_str());
            if (tr != key) text = tr;
        } else if (action == "vr.restartSteamVR" || text == "Restart SteamVR to use the new resolution.") {
            text = Tr("notice.restartSteamVR");
        } else if (text == "H.264 does not support this picture size; the stream switched to HEVC.") {
            text = Tr("notice.h264FallbackHevc");
        } else if (text == "SteamVR did not start. Check SteamVR on your PC.") {
            text = Tr("notice.steamVrDidNotStart");
        } else if (text == "SteamVR is not installed on this PC.") {
            text = Tr("notice.steamVrNotInstalled");
        } else if (text == "Launching game…") {
            text = Tr("notice.launchingGame");
        } else if (text == "Could not launch the game") {
            text = Tr("notice.couldNotLaunchGame");
        } else if (text == "This PC allowed the headset to connect.") {
            text = Tr("notice.pcAllowedConnect");
        } else if (text == "This PC now lets the headset use its mouse and keyboard.") {
            text = Tr("notice.inputAllowed");
        } else if (text == "This PC turned off the headset's mouse and keyboard. You can still watch." ||
                   text == "This PC has the headset's mouse and keyboard turned off. You can still watch.") {
            text = Tr("notice.inputDisabled");
        }
        PostNotice(text, m.value("level", "info"), action);
    }
}

void App::OnVideoFrame(rc::ReceivedFrame&& f) {
    std::lock_guard lock(m_decoderMutex);
    auto it = m_decoders.find(f.streamId);
    if (it == m_decoders.end() || !it->second) return;
    if (it->second->Failed()) {
        m_refChainBroken[f.streamId] = true;
        return;
    }
    if (it->second->Codec() != f.codec) return;
    // A23 (queue bound) + A46 (TASK-RECOVERY-GUARD): judge the queue age at the decode
    // submission point (the Submit call below). A frame that queued longer than
    // rc::kQueueDropMs from its arrival (completeNs) to here is dropped: a momentary
    // freeze recovered by a keyframe beats half a second of queued latency.
    // A46: the recovery IDR is exempt - it is far larger than a P frame, so it queues
    // longer. Dropping it here threw away the very frame that rebuilds the chain,
    // which is why cap100 sessions never recovered (11-13%).
    const bool isKeyframe = (f.flags & rc::kVideoKeyframe) != 0;
    const int64_t nowNs = rc::NowNs();
    if (rc::FrameQueueExpired(nowNs, f.completeNs, isKeyframe)) {
        m_queueDrops++;
        m_queueDropStreak++;
        // Persistent PER-STREAM break state: a drop breaks the chain no matter how many
        // small P frames slip through the gate afterwards. The first break requests a
        // keyframe immediately - no longer gated on the streak reaching 5.
        const bool firstBreak = !m_refChainBroken[f.streamId];
        m_refChainBroken[f.streamId] = true;
        if (firstBreak || rc::QueueDropRequestKeyframe(m_queueDropStreak))
            m_client.RequestKeyframe(f.streamId);
        if (nowNs - m_queueDropLogNs >= 5'000'000'000LL) {
            m_queueDropLogNs = nowNs;
            RC_LOGW("Decode queue overflow: %u frame(s) dropped after >%d ms queued so far "
                    "(keyframe first + every %u drops, streak %u)",
                    m_queueDrops, int(rc::kQueueDropMs), rc::kQueueDropKeyframeEvery, m_queueDropStreak);
        }
        return;
    }
    // A frame passed the gate: the request *cadence* starts over, but "broken" does not -
    // only a real keyframe reaching Submit clears that. The old code cleared here, so a
    // 1-4 frame drop streak interleaved with passing frames never asked for a keyframe.
    m_queueDropStreak = 0;
    it->second->Submit(f.frameIndex, isKeyframe, (f.flags & rc::kVideoRecovery) != 0,
                       std::move(f.bitstream), std::move(f.metadata), f.completeNs);
    m_client.SetLastGoodFrame(f.streamId, it->second->LastGoodFrame());
    if (isKeyframe) m_refChainBroken[f.streamId] = false;
    // While broken, actively re-request on every passing frame. NeedsKeyframe() is the
    // decoder's one-shot gap flag (kept as double insurance); OR-ing the persistent
    // state means recovery no longer depends on that flag being re-armed.
    const bool decoderNeedsKeyframe = it->second->NeedsKeyframe();
    if (m_refChainBroken[f.streamId] || decoderNeedsKeyframe)
        m_client.RequestKeyframe(f.streamId);
}

void App::RecoverFailedDecoders() {
    struct FailedDecoder {
        uint8_t id;
        std::shared_ptr<VideoDecoder> decoder;
    };
    std::vector<FailedDecoder> failed;
    const int64_t now = rc::NowNs();
    {
        std::lock_guard lock(m_decoderMutex);
        for (const auto& [id, decoder] : m_decoders) {
            if (!decoder || !decoder->Failed()) continue;
            auto& retryAt = m_decoderRetryNs[id];
            if (retryAt && now < retryAt) continue;
            retryAt = now + 5'000'000'000LL;
            failed.push_back({id, decoder});
        }
    }
    for (const auto& entry : failed) {
        const uint8_t id = entry.id;
        RC_LOGW("Decoder stream %u failed; rebuilding it and requesting a keyframe", id);
        auto replacement = std::make_unique<VideoDecoder>();
        if (id == rc::kVrStreamId) {
            replacement->SetDecodeSampleSink([this](double ms) {
                m_decSamples.Push(float(ms));
                m_client.FeedDecodeSample(ms);
            });
        }
        if (!replacement->Init(entry.decoder->Codec(), entry.decoder->Width(), entry.decoder->Height(), entry.decoder->Fps())) {
            RC_LOGE("Decoder stream %u recovery init failed; retrying in 5 s", id);
            continue;
        }
        std::shared_ptr<VideoDecoder> decoder = std::move(replacement);
        {
            std::lock_guard lock(m_decoderMutex);
            auto it = m_decoders.find(id);
            if (it == m_decoders.end() || it->second != entry.decoder) continue;
            m_decoders[id] = decoder;
            m_refChainBroken[id] = true;
        }
        m_client.ResetStream(id);
        m_client.RequestKeyframe(id);
        if (id == rc::kVrStreamId) {
            m_scene.ClearExternal();
            m_vrHasFrame = m_vrLatched = m_motionMoves = m_motionConsistent = false;
            m_vrLatchNs = 0;
            m_vrLatchedDisplayNs = 0;
            m_vrNoPicture = 0;
            m_genQuota = {};
            m_decSamples.Reset();
        } else {
            m_monitorFresh[id] = false;
            if (Window* w = m_windows.Find("monitor-" + std::to_string(id))) w->videoReady = false;
        }
        RC_LOGI("Decoder stream %u recovered; waiting for a keyframe", id);
    }
}

void App::ConfigureDesktopStreams(const json& streams) {
    std::map<uint8_t, DesktopStreamInfo> next;
    for (const auto& s : streams) {
        DesktopStreamInfo d;
        d.streamId = s.value("streamId", uint8_t(1));
        d.monitorId = s.value("monitorId", "");
        d.name = s.value("name", "");
        d.width = s.value("width", 1920);
        d.height = s.value("height", 1080);
        d.codec = rc::VideoCodec(s.value("codec", 1));
        d.fps = s.value("fps", 60.f);
        next[d.streamId] = d;
    }
    // A new stream counts its frames from the start and can't continue the old one's picture,
    // so even with the same size the decoder and frame tracking have to start over too.
    for (auto& [id, d] : next) {
        auto dec = std::make_unique<VideoDecoder>();
        dec->Init(d.codec, d.width, d.height, d.fps);
        {
            std::lock_guard lock(m_decoderMutex);
            m_decoders[id] = std::move(dec);
            m_decoderRetryNs.erase(id);
        }
        m_client.ResetStream(id);
        if (m_monitorLayers.count(id)) {
            m_monitorFresh[id] = false;
            if (m_xr.DestroySwapchain(m_monitorLayers[id])) m_monitorLayers.erase(id);
        }
        Window* w = m_windows.Create("monitor-" + std::to_string(id), WindowKind::Video);
        w->title = d.name;
        w->aspect = float(d.height) / float(d.width);
        w->minWidthM = 0.4f;
        w->maxWidthM = 6.f;
        w->videoReady = false;
    }
    for (auto& [id, d] : m_desktopStreams) {
        if (!next.count(id)) {
            m_windows.Remove("monitor-" + std::to_string(id));
            std::lock_guard lock(m_decoderMutex);
            m_decoders.erase(id);
            if (m_monitorLayers.count(id)) {
                m_monitorFresh[id] = false;
                if (m_xr.DestroySwapchain(m_monitorLayers[id])) m_monitorLayers.erase(id);
            }
        }
    }
    m_desktopStreams = next;
    LayoutMonitors(!m_monitorsLaidOut);
    m_monitorsLaidOut = true;
    ShowMonitors(m_monitorsVisible);
}

void App::ConfigureVrStream(const json& s) {
    const int eyeW = s.value("eyeWidth", 0), eyeH = s.value("eyeHeight", 0);
    const auto codec = rc::VideoCodec(s.value("codec", 1));
    if (eyeW <= 0 || eyeH <= 0) return;
    m_vrEyeWidth = eyeW;
    m_vrEyeHeight = eyeH;
    m_vrCodec = codec;
    m_linkPolicy = {};      // VD-OBSERVE: a fresh stream starts judging from scratch
    m_linkPolicyTicks = 0;
    m_genQuota = {};
    m_genQuotaAnnounced = false;
    // Foveated: the frames come smaller, and are stretched back to eyeWidth x eyeHeight.
    int codedW = eyeW, codedH = eyeH;
    const json f = s.value("foveation", json());
    m_vrFoveated = false;
    if (f.is_object()) {
        const int fw = f.value("eyeWidth", 0), fh = f.value("eyeHeight", 0);
        const float size = f.value("size", 0.f);
        if (fw > 0 && fh > 0 && fw <= eyeW && fh <= eyeH && size > 0.05f && size < 1.f) {
            codedW = fw;
            codedH = fh;
            m_vrFoveated = true;
            m_vrFoveation = {};
            m_vrFoveation.size = size;
            m_vrFoveation.density[0] = (float(fw) / eyeW - size) / (1 - size);
            m_vrFoveation.density[1] = (float(fh) / eyeH - size) / (1 - size);
            for (int eye = 0; eye < 2; eye++) m_vrFoveation.start[eye][0] = m_vrFoveation.start[eye][1] = (1 - size) / 2;
            RC_LOGI("VR stream: foveated, %dx%d per eye shown at %dx%d", fw, fh, eyeW, eyeH);
        }
    }
    // ROUND18 L2-FIX2: a plain (un-foveated) stream is also encoded below the render
    // size now. Prefer the PC's reported per-eye coded size so the decoder matches the
    // real frame; an old PC omits these keys, so fall back to the render size. Gated
    // on !m_vrFoveated so a valid foveation block above keeps sole ownership.
    if (!m_vrFoveated) {
        const int cw = s.value("codedWidth", 0), ch = s.value("codedHeight", 0);
        if (cw > 0 && ch > 0 && cw <= eyeW && ch <= eyeH) {
            codedW = cw;
            codedH = ch;
        }
    }
    auto dec = std::make_unique<VideoDecoder>();
    // R3: per-frame decode costs feed the diagnostics quantiles (set before Init starts the threads).
    // TASK-DEC-PERCENTILE-2026-11-20 §1.3: the same per-frame cost lands in the app's own
    // sampler, whose percentiles the diag line reads (the client's copy stays switch-gated).
    dec->SetDecodeSampleSink([this](double ms) {
        m_decSamples.Push(float(ms));
        m_client.FeedDecodeSample(ms);
    });
    dec->Init(codec, codedW * 2, codedH, s.value("fps", 72.f));  // ROUND18 L4 fallback
    {
        std::lock_guard lock(m_decoderMutex);
        m_decoders[rc::kVrStreamId] = std::move(dec);
        m_decoderRetryNs.erase(rc::kVrStreamId);
    }
    m_scene.ClearExternal();
    m_client.ResetStream(rc::kVrStreamId);
    CreateVrSwapchain();
    m_vrHasFrame = false;
    m_vrLatchedDisplayNs = 0;
    m_motionMoves = false;
    m_motionConsistent = false;
    m_motionFrameNs = 0;
    m_motionIntervalSmoothNs = 0;
    // A new stream starts with nothing waiting and nothing known about its timing.
    m_behindSinceNs = m_slipUntilNs = m_slipRetryNs = m_queueKeptUntilNs = m_queuePolicyLogNs = 0;
    m_slips.clear();
    m_arrivalSpread.clear();
    m_queueWanted = 1;
}

void App::CreateVrSwapchain() {
    // Upscaling only has a job while the stream is below the headset's own resolution.
    const float resolvedScale = rc::ResolveVrUpscale(m_settings.upscaling, m_xr.RecommendedWidth(), m_vrEyeWidth,
                                                    m_scene.UpscalerReady());
    if (m_vrSwapchain.handle && !m_xr.DestroySwapchain(m_vrSwapchain)) return;
    const float s = resolvedScale;
    if (!m_xr.CreateSwapchain(int(m_vrEyeWidth * s + 0.5f) * 2, int(m_vrEyeHeight * s + 0.5f), GL_SRGB8_ALPHA8, 1,
                              false, m_vrSwapchain, 1, "vr")) {
        m_vrHasFrame = false;
        RC_LOGE("VR swapchain creation failed; a decoded frame will retry it");
        return;
    }
    m_vrUpscaleSetting = m_settings.upscaling;
    m_vrUpscaled = resolvedScale > 1.f;
    // ROUND18 L8-TELEM: the queue depth the runtime gave the stream's swapchain; read it
    // against the submit->lit time in the diag line.
    RC_LOGI("VR swapchain: %u images, %dx%d", m_xr.SwapchainImages(), m_vrSwapchain.width, m_vrSwapchain.height);
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void App::LayoutHome(bool force) {
    Window* home = m_windows.Find("home");
    if (home && (force || home->appear < 0.01f)) home->pose = WindowManager::PlaceInFront(m_head, 1.3f, 0, -0.05f);
}

void App::LayoutMonitors(bool force) {
    if (!force || m_desktopStreams.empty()) return;
    // Arrange monitors on an arc around the user, primary in the middle.
    std::vector<uint8_t> order;
    for (auto& [id, d] : m_desktopStreams) order.push_back(id);
    const float distance = m_settings.screenDistance;
    const float width = 1.45f * distance / 1.35f;
    const float gapM = 0.06f;
    // A portrait monitor is a landscape one turned on its side: as tall as the others are
    // wide, not as wide (that made a 16:10 one 2.3 m tall).
    std::vector<float> widths(order.size(), width);
    float total = gapM * (float(order.size()) - 1);
    for (size_t i = 0; i < order.size(); i++) {
        if (Window* w = m_windows.Find("monitor-" + std::to_string(order[i])); w && w->aspect > 1.f)
            widths[i] = width / w->aspect;
        total += widths[i];
    }
    float along = -total / 2;  // metres along the arc, left edge of the next monitor
    for (size_t i = 0; i < order.size(); i++) {
        Window* w = m_windows.Find("monitor-" + std::to_string(order[i]));
        const float centre = along + widths[i] / 2;
        along += widths[i] + gapM;
        if (!w) continue;
        w->widthM = widths[i];
        w->curveRadius = m_settings.curvature > 0.02f ? distance / std::max(0.05f, m_settings.curvature) : 0.f;
        w->layerBacked = m_settings.sharpLayers && (w->curveRadius == 0 || m_xr.CylinderLayers());
        w->pose = WindowManager::PlaceInFront(m_head, distance, centre / distance * 180.f / kPi, 0.05f);
    }
    if (Window* dock = m_windows.Find("dock")) {
        dock->pose = WindowManager::PlaceInFront(m_head, 0.9f, 0, -0.50f, 0);
        m_placed.insert("dock");
    }
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

// The headset's microphone while RinCast is in the background (the Quest's home, another app,
// the headset asleep). The connection to the PC lives on its own thread, so before, a session
// kept recording there until it ended. Android lets one app at a time record well: a Quest Pro
// gave the next app that opened the microphone, Virtual Desktop say, a much quieter one until
// the headset restarted. Let go of it, and take it again when RinCast is back in front.
void App::ReleaseMicInBackground() {
    if (!m_mic.Running()) return;
    m_mic.Stop();
    m_micReleased = true;
    RC_LOGI("Microphone released while RinCast is in the background");
}

void App::Frame() {
    const int64_t now = rc::NowNs();
    if (m_micReleased) {
        m_micReleased = false;
        if (m_settings.micPassthrough && m_client.GetState() == StreamClient::State::Connected)
            m_mic.Start([this](const uint8_t* d, size_t n) { m_client.SendAudio(d, n); });
    } else if (m_settings.micPassthrough && m_client.GetState() == StreamClient::State::Connected) {
        m_mic.PollRestart();
    }
    const float dt = std::clamp(float(now - m_lastFrameNs) / 1e9f, 0.001f, 0.1f);
    m_lastFrameNs = now;
    m_time += dt;

    FrameState frame;
    // Lighthouse trackers calibrated against the headset (Space Calibrator) need a space that
    // stays put; the local floor moves whenever the headset recenters.
    m_xr.SetPreferStage(m_headset->PreferStageSpace() || m_settings.fixedPlaySpace);
    m_xr.KeepRefreshRate();
    FollowDisplayRate();
    if (!m_xr.BeginFrame(frame)) return;
    const int64_t wokeNs = rc::NowNs();
    g_lastFrame = frame;
    g_haveFrame = true;
    const bool firstFrame = m_head.position.y == 0 && m_head.position.x == 0;
    m_head = frame.head;
    if (firstFrame) LayoutHome(true);
    if (m_xr.SpaceGeneration() != m_spaceGeneration) {
        m_spaceGeneration = m_xr.SpaceGeneration();
        Recenter();
    }

    m_input.Update(frame.predictedDisplayTime);
    ProcessNetwork();
    if (m_problem != m_spokenProblem) {
        m_spokenProblem = m_problem;
        if (m_settings.readAloud && !m_problem.empty()) Platform::Speak("Connection problem. " + m_problem);
    }
    if (m_disconnectRequested) DisconnectNow();
    UpdateMode(dt);
    UpdateInput(frame, dt);
    if (m_client.GetState() == StreamClient::State::Connected) {
        UpdateTracking(frame);
    } else {
        // Neither is any use without a PC, and hand tracking runs slower with simultaneous on.
        m_headset->StopBody();
        m_input.SetSimultaneous(false);
    }
    // xrEndFrame discards every layer on these frames; drawing into them only burns CPU/GPU time.
    if (!frame.shouldRender) {
        m_vrLatched = false;
        m_vrLatchNs = 0;
        m_displayDeltaApplied = 0;
        m_xr.EndFrame(frame, m_underlays, false, m_overlays, false, 0);
        m_submitNs = rc::NowNs();
        return;
    }
    // The monitors' pictures, for what is drawn with them below. The game's picture waits for
    // the late latch: nothing drawn in between needs it.
    m_vrLatched = false;
    m_vrLatchNs = 0;
    LatchVideo(frame, false);

    // Environment / passthrough.
    const Environment env = EnvironmentFromKey(m_settings.environment);
    m_xr.SetPassthrough(env == Environment::Passthrough && m_mode != Mode::Vr);

    // During a game with nothing on top, drawing an empty eye layer over the stream would
    // only cost GPU time and heat. The performance overlay alone doesn't need it either: it
    // goes to the compositor as a layer of its own, redrawn when its numbers change. Drawing
    // both eye images every frame for it took enough GPU time that the headset started its
    // frames earlier, which added to the very latency it shows.
    Window* stats = m_windows.Find("stats");
    bool anyWindow = false;
    for (const auto& w : m_windows.All())
        if (w.get() != stats) anyWindow = anyWindow || w->visible || w->appear > 0.01f;
    const bool vrClear = m_mode == Mode::Vr && m_vrHasFrame && !m_vrOverlay && m_warp <= 0.001f && !anyWindow;
    const bool statsShown = stats && (stats->visible || stats->appear > 0.01f);
    const bool statsLayer = vrClear && statsShown && stats->visible && stats->appear > 0.98f;
    UpdateStatsLayer(stats, statsLayer, dt);
    const bool uiLayer = !(vrClear && (!statsShown || statsLayer));
    m_keyboard.SetVisible(IsVisible("keyboard"));
    m_keyboard.SetDirectVk(m_client.GetState() == StreamClient::State::Connected && m_mode == Mode::Desktop);
    if (Window* kbw = m_windows.Find("keyboard"); kbw && kbw->visible) m_keyboard.Size(*kbw);
    m_windows.RenderCanvases(dt, m_time);
    UpdateUiLayers();
    if (frame.shouldRender && uiLayer) {
        RenderEyes(frame, dt);
        // The GPU starts on the eye images now instead of when the frame is handed in, so the
        // game's picture, drawn last, isn't queued behind work that could already be done.
        glFlush();
    }
    SteerVrTiming(frame);
    // Late latch: the game's picture is picked up here and nowhere earlier, as the last thing
    // before the frame is handed in, and frames decoded while we rendered still make it into
    // this frame's compositor layers (VR stream, monitor layers) instead of waiting a whole
    // frame. The headset starts our frame just early enough for everything from here on to be
    // done in time: whatever sits between this and xrEndFrame is time a picture waits.
    // A test of exactly that (debug.rincast.prelatch_ms): work added before the pickup. If
    // "woken ... before the display" in the log grows by as much, the headset counts all of
    // the frame, and what runs before the pickup today is worth moving out of it.
    if (m_debugPreLatchMs > 0 && m_mode == Mode::Vr) {
        const int64_t until = rc::NowNs() + int64_t(m_debugPreLatchMs * 1e6f);
        while (rc::NowNs() < until) {
        }
    }
    m_lateLatchNs = rc::NowNs();
    LatchVideo(frame, true);
    {
        const double lead = double(m_xr.ToMonotonicNs(frame.predictedDisplayTime) - m_lateLatchNs);
        if (lead > 0 && lead < 1e8) m_latchLeadNs = m_latchLeadNs > 0 ? m_latchLeadNs * 0.95 + lead * 0.05 : lead;
    }
    // A decoded frame earns a fresh budget for the empty display slots that follow it.
    // DecideFrameGeneration spends that budget only when a moved frame can actually be drawn.
    if (m_mode == Mode::Vr && m_settings.frameGeneration && !m_vrLatched) GenerateFrame(frame);
    BuildLayers(frame);
    const bool alpha = m_xr.PassthroughActive() || !m_underlays.empty();
    // ROUND19 E1 probe: only VR frames may ask for an earlier display slot; every other commit
    // passes 0, which multiplies out to the untouched displayTime.
    m_displayDeltaApplied = m_mode == Mode::Vr ? m_displayDeltaFile : 0;
    m_xr.EndFrame(frame, m_underlays, alpha, m_overlays, uiLayer, m_displayDeltaApplied);
    m_submitNs = rc::NowNs();  // ROUND18 L8-TELEM: everything from here to the display time belongs to the runtime
    LogVrTiming(frame, wokeNs);
}

#ifndef GL_TIME_ELAPSED_EXT
#define GL_TIME_ELAPSED_EXT 0x88BF
#endif
#ifndef GL_GPU_DISJOINT_EXT
#define GL_GPU_DISJOINT_EXT 0x8FBB
#endif

// The GPU's time for the game's picture, measured on one frame at a time: a result is read
// when it is ready, a frame or two later, and never waited for.
void App::TimeVrDraw(bool begin) {
    if (!m_vrTimerChecked) {
        m_vrTimerChecked = true;
        const char* extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
        if (extensions && std::strstr(extensions, "GL_EXT_disjoint_timer_query")) glGenQueries(1, &m_vrTimer);
    }
    if (!m_vrTimer) return;
    if (begin) {
        if (m_vrTimerPending) {
            GLuint available = 0;
            glGetQueryObjectuiv(m_vrTimer, GL_QUERY_RESULT_AVAILABLE, &available);
            if (!available) return;
            m_vrTimerPending = false;
            GLuint ns = 0;
            glGetQueryObjectuiv(m_vrTimer, GL_QUERY_RESULT, &ns);
            GLint disjoint = 0;
            glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
            if (!disjoint) {
                m_timingGpuNs += double(ns);
                m_timingGpuFrames++;
            }
        }
        glBeginQuery(GL_TIME_ELAPSED_EXT, m_vrTimer);
        m_vrTimerRunning = true;
    } else if (m_vrTimerRunning) {
        glEndQuery(GL_TIME_ELAPSED_EXT);
        m_vrTimerRunning = false;
        m_vrTimerPending = true;
    }
}

void App::LogVrTiming(const FrameState& frame, int64_t wokeNs) {
    const int64_t now = rc::NowNs();
    if (m_mode != Mode::Vr) {
        m_timingLogNs = now;
        m_timingFrames = m_timingGpuFrames = 0;
        m_timingWakeLeadNs = m_timingLatchNs = m_timingSubmitNs = m_timingGpuNs = 0;
        m_submitLitSumMs = 0;
        m_submitLitFrames = 0;
        m_flowDisplays = m_flowShown = m_flowLate = 0;
        m_diagFlowDisplays = m_diagFlowShown = m_diagFlowLate = 0;
        m_flowGenerated = 0;
        m_generatedFramesWindow = 0;
        m_genFramesPrevSlot = m_generatedFrames;
        m_flowDecodedStart = 0;
        m_flowDecodeMaxMs = 0;
        m_generationWindow.Reset();
        m_generationDiag.ResetWindow();
        m_generationDiag.last = {};
        m_latchDiag.ResetWindow();
        m_zeroSampleCount = 0;
        m_motionPackets = m_motionPacketFailures = 0;
        m_motionFailureCounts[0] = m_motionFailureCounts[1] = m_motionFailureCounts[2] = m_motionFailureCounts[3] = 0;
        for (uint32_t& count : m_motionHeaderFailures) count = 0;
        m_motionFailure = 0;
        m_vrMetadataBytes = m_motionMetadataBytes = 0;
        m_lastMotionHeader = {};
        m_generatedLogNs = now;
        m_decSamples.Reset();  // TASK-DEC-PERCENTILE-2026-11-20 §1.3: same boundary as the flow window
        m_flowSlackMs.clear();
        m_flowWaiting[0] = m_flowWaiting[1] = m_flowWaiting[2] = 0;
        m_flowSlips = 0;
        return;
    }
    if (m_vrLatchNs) {
        m_timingWakeLeadNs += double(m_xr.ToMonotonicNs(frame.predictedDisplayTime) - wokeNs);
        m_timingLatchNs += double(m_vrLatchNs - wokeNs);
        m_timingSubmitNs += double(now - m_vrLatchNs);
        m_timingFrames++;
        // ROUND18 L8-TELEM: the runtime's own share, xrEndFrame's return to this frame's display
        // time. Same drop bounds as AddStages: a small negative is a clock offset, beyond that
        // the reading is worthless.
        const double submitLitMs = double(m_xr.ToMonotonicNs(frame.predictedDisplayTime) - m_submitNs) / 1e6;
        if (submitLitMs >= 0 && submitLitMs < 1000) {
            m_submitLitSumMs += submitLitMs;
            m_submitLitFrames++;
        } else if (submitLitMs < 0 && submitLitMs > -2) {
            m_submitLitFrames++;
        }
    }
    m_flowDisplays++;
    m_diagFlowDisplays++;
    if (m_vrLatched) {
        m_flowShown++;
        m_diagFlowShown++;
    }
    // TASK-POLICY-FIX-TESTS-2026-11-20 §B: a slot with no new picture where
    // GenerateFrame still drew is an extrapolated one. m_generatedFrames moves
    // at most once per slot and only in GenerateFrame, so a change since the
    // previous slot means this slot drew - counted here, in the same window as
    // the other buckets, instead of reading m_generatedFrames across its own
    // (5 s, separately clocked) reset inside GenerateFrame.
    else if (m_generatedFrames != m_genFramesPrevSlot) m_flowGenerated++;
    m_genFramesPrevSlot = m_generatedFrames;
    const int64_t flowWindowStartNs = m_timingLogNs;
    if (now - flowWindowStartNs < 5'000'000'000LL) return;
    m_timingLogNs = now;
    if (now - m_generatedLogNs >= 5'000'000'000LL) {
        const double secs = double(now - m_generatedLogNs) / 1e9;
        const double flowSecs = double(now - flowWindowStartNs) / 1e9;
        const double shownFps = flowSecs > 0 ? double(m_flowShown) / flowSecs : 0.0;
        double streamFps = 0;
        if (m_serverStats.contains("vr")) streamFps = m_serverStats["vr"].value("fps", 0.0);
        const auto& stats = m_generationWindow;
        const uint32_t rejected = stats.attempts - stats.generated;
        const auto reason = [&](rc::GenerationRejectReason r) { return stats.rejected[size_t(r)]; };
        RC_LOGI("Frame generation: %.0f/s generated, %.0f/s shown of %.0f Hz, stream %.0f fps, %.0f%% skipped "
                "(cap %.1f, field: %d moving, mean %d, spread %d, far %d, coherent %d, global %d); "
                "attempts %u, quota-blocked %u, rejected %u, reasons inconsistent %u no-motion %u missing-frame %u "
                "missing-swapchain %u invalid-timing %u invalid-field %u amount-small %u amount-cap %u texture %u draw %u; "
                "motion packets %u, failures %u, last-failure %d",
                stats.GeneratedFps(secs), shownFps, m_xr.RefreshRate(), streamFps,
                stats.attempts ? 100.0 * rejected / double(stats.attempts) : 0.0,
                m_lastGenCap, m_motionGateMoving, m_motionGateMean, m_motionGateSpread, m_motionGateFar,
                m_motionGateCoherent, m_motionGateGlobal, stats.attempts, stats.quotaBlocked, rejected,
                reason(rc::GenerationRejectReason::InconsistentMotion), reason(rc::GenerationRejectReason::NoMotion),
                reason(rc::GenerationRejectReason::MissingFrame), reason(rc::GenerationRejectReason::MissingSwapchain),
                reason(rc::GenerationRejectReason::InvalidMotionTiming), reason(rc::GenerationRejectReason::InvalidField),
                reason(rc::GenerationRejectReason::AmountTooSmall), reason(rc::GenerationRejectReason::AmountAboveCap),
                reason(rc::GenerationRejectReason::TextureUnavailable), reason(rc::GenerationRejectReason::DrawFailed),
                m_motionPackets, m_motionPacketFailures, m_motionFailure);
        const auto& diag = m_generationDiag;
        const auto& latchDiag = m_latchDiag;
        const char* lastPath = diag.last.path == rc::GenerationPath::Kept ? "kept"
                                 : diag.last.path == rc::GenerationPath::External ? "external" : "none";
        const double motionAgeMs = m_vrMeta.trackingTimestampNs
                                        ? double(now - int64_t(m_vrMeta.trackingTimestampNs)) / 1e6
                                        : 0.0;
        RC_LOGI("VR generation diagnostics: calls %u, attempts %u, quota-blocked %u, generated %u, acquire-failed %u, "
                "draw-failed %u, release-failed %u; state enabled %d, latched %d, hasFrame %d, swapchain %d, "
                "outstanding %d, output %dx%d, field %dx%d block %d, metadata %zu bytes, motionMetadata %zu bytes, "
                "motionGrid %ux%u block %u previous %llu, motionAge %.1f ms, "
                "moves %d, consistent %d, externalReady %d, upscaled %d; last reason %s, path %s, amount %.3f, cap %.2f; "
                "latch no-image %u, metadata-missing %u, swapchain-missing %u, acquire-failed %u, release-failed %u; "
                "motion failures short-header %u, invalid-header %u, invalid-runs %u; "
                "header width-zero %u, height-zero %u, block-zero %u, timestamp-nonadvancing %u, unknown %u; "
                "last-header-failure track %llu prev %llu grid %ux%u block %u",
                diag.calls, diag.attempts, diag.quotaBlocked, diag.generated, diag.acquireFailed, diag.drawFailed,
                diag.releaseFailed, int(m_settings.frameGeneration), int(m_vrLatched), int(m_vrHasFrame),
                int(m_vrSwapchain.handle != 0), int(m_vrSwapchain.imageAcquired), m_vrSwapchain.width, m_vrSwapchain.height,
                m_motionWidth, m_motionHeight, m_motionBlock, m_vrMetadataBytes, m_motionMetadataBytes,
                m_lastMotionHeader.gridWidth, m_lastMotionHeader.gridHeight, m_lastMotionHeader.blockSize,
                static_cast<unsigned long long>(m_lastMotionHeader.previousTimestampNs), motionAgeMs, int(m_motionMoves),
                int(m_motionConsistent), int(m_scene.ExternalGenerationReady()), int(m_vrUpscaled),
                rc::GenerationRejectReasonName(diag.last.reason), lastPath, diag.last.amount, diag.last.cap,
                latchDiag.noImage, latchDiag.metadataMissing, latchDiag.swapchainMissing, latchDiag.acquireFailed,
                latchDiag.releaseFailed, m_motionFailureCounts[1], m_motionFailureCounts[2], m_motionFailureCounts[3],
                m_motionHeaderFailures[0], m_motionHeaderFailures[1], m_motionHeaderFailures[2],
                m_motionHeaderFailures[3], m_motionHeaderFailures[4],
                static_cast<unsigned long long>(m_lastMotionFailureTrackingNs),
                static_cast<unsigned long long>(m_lastMotionFailureHeader.previousTimestampNs),
                m_lastMotionFailureHeader.gridWidth, m_lastMotionFailureHeader.gridHeight,
                m_lastMotionFailureHeader.blockSize);
        // Separate line: fields/order of the two lines above are untouched.  Only in a
        // zero-generation window.  mB=0 means "no motion field" and ALONE cannot separate
        // mailbox miss / PC oversize rollback / PC motion disabled — correlate ts (the
        // headsetTargetNs the PC wrote into VrFrameMetadata) with the PC's own frame records.
        // f is the wire sequence only: it is NOT the PC producer's counter.
        // Worth=false, timing/field, consistency and clock/quota gating are unambiguous
        // from reason+flags alone.  seq orders the samples inside this window.
        if (stats.attempts > 0 && stats.generated == 0 && m_zeroSampleCount > 0) {
            char line[1024] = "VR gen zero-window trace:";
            size_t used = std::strlen(line);
            for (uint8_t i = 0; i < m_zeroSampleCount; ++i) {
                const ZeroGenSample& s = m_zeroSamples[i];
                int n = std::snprintf(line + used, sizeof(line) - used,
                        " [f=%u seq=%u ts=%llu r=%s fl=0x%x mB=%u since=%.1f sm=%.1f amt=%.2f cap=%.2f]",
                        s.frameIndex, s.localSeq, static_cast<unsigned long long>(s.trackingTimestampNs),
                        rc::GenerationRejectReasonName(rc::GenerationRejectReason(s.reason)),
                        unsigned(s.flags), unsigned(s.motionBytes), s.sinceMs, s.smoothMs, s.amount, s.cap);
                if (n <= 0 || size_t(n) >= sizeof(line) - used) break;
                used += size_t(n);
            }
            RC_LOGI("%s", line);
        }
        m_generationWindow.Reset();
        m_generationDiag.ResetWindow();
        m_latchDiag.ResetWindow();
        m_zeroSampleCount = 0;
        m_motionPackets = m_motionPacketFailures = 0;
        m_motionFailureCounts[0] = m_motionFailureCounts[1] = m_motionFailureCounts[2] = m_motionFailureCounts[3] = 0;
        for (uint32_t& count : m_motionHeaderFailures) count = 0;
        m_motionFailure = 0;
        m_generatedLogNs = now;
    }
    uint32_t decoded = 0;
    {
        std::lock_guard lock(m_decoderMutex);
        auto it = m_decoders.find(rc::kVrStreamId);
        if (it != m_decoders.end() && it->second) decoded = it->second->DecodedFrames();
    }
    if (m_flowDisplays && decoded >= m_flowDecodedStart && m_flowDecodedStart) {
        // Decoded but never shown: skipped to catch up (see VideoDecoder::Latch). A display
        // without a new picture either got an extrapolated frame or repeats the last one.
        // TASK-POLICY-FIX-TESTS-2026-11-20 §B: the buckets are disjoint and sum to the
        // window - shown (a newly latched decoded picture was drawn) + generated (an
        // extrapolated frame was drawn) + repeated (a pure reprojection of the last
        // picture) == displays.
        const int n = int(decoded - m_flowDecodedStart);
        RC_LOGI("VR frame flow over %d displays: %d decoded, %d shown, %d generated, %d never shown, %d repeated, "
                "%d decoded after their latch; slowest decode %.1f ms",
                m_flowDisplays, n, m_flowShown, m_flowGenerated, std::max(0, n - m_flowShown),
                m_flowDisplays - m_flowShown - m_flowGenerated, m_flowLate, m_flowDecodeMaxMs);
        // How far ahead poses are predicted, and where that time goes (the last second's stages).
        std::string stages;
        for (int i = 0; i < kStageCount; i++) {
            if (m_stats.stageMs[i] < 0) continue;
            char part[48];
            snprintf(part, sizeof(part), "%s%s %.1f", stages.empty() ? "" : ", ", kLatencyStages[i].key, m_stats.stageMs[i]);
            stages += part;
        }
        RC_LOGI("VR prediction: %.1f ms past the display time, %.1f ms from the pose to its display; %s",
                m_pipelineLatencyMs, m_stats.latencyMs, stages.empty() ? "stages not measured yet" : stages.c_str());
    }
    if (m_flowSlackMs.size() >= 20) {
        // When frames were decoded against the late latch: what the margin has to cover. Folded
        // into one display period, from 20% of one early to 80% late against the target, so a
        // frame later than that reads as early; "after it" below is the part that can be told.
        std::sort(m_flowSlackMs.begin(), m_flowSlackMs.end());
        const auto at = [&](double part) { return m_flowSlackMs[size_t(part * double(m_flowSlackMs.size() - 1))]; };
        RC_LOGI("VR frame arrival: decoded %.1f ms before the latch in the middle, 5%% of frames under %.1f ms, 1%% under "
                "%.1f ms, the latest %.1f ms (below 0: after it); margin %.1f ms, %s; taken with 0, 1, 2 newer pictures "
                "waiting: %d, %d, %d; the PC let them run out %d times",
                at(0.5), at(0.05), at(0.01), m_flowSlackMs.front(), m_phaseMarginNs / 1e6,
                m_queueWanted ? "one picture may wait" : "no picture meant to wait", m_flowWaiting[0],
                m_flowWaiting[1], m_flowWaiting[2], m_flowSlips);
    }
    if (now - m_poseWaitAtNs < 5'000'000'000LL)
        RC_LOGI("VR poses: held back %.1f ms before sending; on the PC they waited %.1f ms for SteamVR to take "
                "them, the 2%% that waited least %.1f ms (below 0: missed one)",
                m_poseHoldNs / 1e6, double(m_poseWaitNs) / 1e6, double(m_poseWaitLowNs) / 1e6);
    m_flowSlackMs.clear();
    m_flowWaiting[0] = m_flowWaiting[1] = m_flowWaiting[2] = 0;
    m_flowSlips = 0;
    m_flowDecodedStart = decoded;
    m_flowDisplays = m_flowShown = m_flowLate = 0;
    m_flowGenerated = 0;
    m_flowDecodeMaxMs = 0;
    m_decSamples.Reset();  // TASK-DEC-PERCENTILE-2026-11-20 §1.3: the sampler's window ends with the flow window's
    if (m_timingFrames) {
        const double n = m_timingFrames * 1e6;
        RC_LOGI("VR frame timing at %.0f Hz: woken %.1f ms before the display, the picture picked up %.2f ms after "
                "waking and handed in %.2f ms later, drawn in %.2f ms of GPU time (%d measured)",
                m_xr.RefreshRate(), m_timingWakeLeadNs / n, m_timingLatchNs / n, m_timingSubmitNs / n,
                m_timingGpuFrames ? m_timingGpuNs / (m_timingGpuFrames * 1e6) : 0.0, m_timingGpuFrames);
    }
    m_timingFrames = m_timingGpuFrames = 0;
    m_timingWakeLeadNs = m_timingLatchNs = m_timingSubmitNs = m_timingGpuNs = 0;
}

void App::StartStickCalibration() {
    const auto available = [&](int hand) {
        const HandInput& in = m_input.Hand(hand);
        return in.thumbstickActive && !in.isHand;
    };
    const bool linked = m_settings.linkStickDeadzone;
    if ((linked && (!available(0) || !available(1))) || (!linked && !available(0) && !available(1))) {
        PostNotice(Tr("ctrl.calibrationUnavailable"), "warning");
        return;
    }
    for (int hand = 0; hand < 2; hand++) {
        m_stickCalibration[hand] = {};
        if (available(hand)) m_stickCalibration[hand].Start();
    }
}

void App::CancelStickCalibration() {
    for (auto& calibration : m_stickCalibration) calibration.active = false;
}

bool App::StickCalibrationActive() const {
    return m_stickCalibration[0].active || m_stickCalibration[1].active;
}

bool App::StickCalibrationActive(int hand) const {
    return hand >= 0 && hand < 2 && m_stickCalibration[hand].active;
}

float App::StickCalibrationProgress() const {
    return std::max(StickCalibrationProgress(0), StickCalibrationProgress(1));
}

float App::StickCalibrationProgress(int hand) const {
    return hand >= 0 && hand < 2 ? std::clamp(m_stickCalibration[hand].samples / float(kStickCalibrationFrames), 0.f, 1.f) : 0.f;
}

void App::UpdateStickCalibration() {
    const auto available = [&](int hand) {
        const HandInput& in = m_input.Hand(hand);
        return in.thumbstickActive && !in.isHand;
    };
    bool changed = false;
    bool failed = false;
    bool succeeded = false;
    const bool linked = m_settings.linkStickDeadzone;
    for (int hand = 0; hand < 2; hand++) {
        auto& calibration = m_stickCalibration[hand];
        if (!calibration.active) continue;
        if (!available(hand)) {
            calibration = {};
            failed = true;
            if (linked) break;
            continue;
        }
        calibration.Observe(m_input.Hand(hand).thumbstick);
        if (calibration.active) continue;
        if (!calibration.CanApply()) {
            calibration = {};
            failed = true;
            if (linked) break;
            continue;
        }
        if (linked) {
            succeeded = true;
            continue;
        }
        const float suggested = calibration.SuggestedDeadzone();
        float& setting = hand == 0 ? m_settings.leftStickDeadzone : m_settings.rightStickDeadzone;
        if (setting != suggested) {
            setting = suggested;
            changed = true;
        }
        calibration = {};
        succeeded = true;
    }
    if (linked && failed) {
        for (auto& calibration : m_stickCalibration) calibration = {};
        PostNotice(Tr("ctrl.calibrationFailed"), "warning");
        return;
    }
    if (linked && StickCalibrationActive()) return;
    if (linked && succeeded) {
        const float linkedDeadzone = std::max(m_stickCalibration[0].SuggestedDeadzone(), m_stickCalibration[1].SuggestedDeadzone());
        if (m_settings.leftStickDeadzone != linkedDeadzone || m_settings.rightStickDeadzone != linkedDeadzone) {
            m_settings.leftStickDeadzone = m_settings.rightStickDeadzone = linkedDeadzone;
            changed = true;
        }
        for (auto& calibration : m_stickCalibration) calibration = {};
    }
    if (changed) SaveSettings();
    if (failed) PostNotice(Tr("ctrl.calibrationFailed"), "warning");
    if (succeeded) PostNotice(Tr("ctrl.calibrationDone"));
}

void App::UpdateInput(const FrameState& frame, float dt) {
    UpdateStickCalibration();
    const bool uiActive = m_mode != Mode::Vr || m_vrOverlay;
    for (int h = 0; h < 2; h++) {
        const HandInput& in = m_input.Hand(h);
        auto& r = m_rays[h];
        const bool was = r.triggerDown;
        r.valid = uiActive && in.active && in.aimValid;
        r.origin = in.aim.position;
        // Steady the pointer when it's nearly still (more so for tracked hands), without
        // lag when it really moves.
        {
            const Vec3 target = in.aim.Forward();
            Vec3& smooth = m_smoothDir[h];
            const Vec3 delta = target - smooth;
            const float moved = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
            const float follow = std::clamp((in.isHand ? 0.12f : 0.3f) + moved * (in.isHand ? 18.f : 40.f), 0.f, 1.f);
            smooth = r.valid ? smooth + delta * follow : target;
            const float len = std::sqrt(smooth.x * smooth.x + smooth.y * smooth.y + smooth.z * smooth.z);
            r.dir = len > 0.001f ? smooth * (1.f / len) : target;
        }
        r.triggerDown = was ? in.trigger > 0.35f : in.trigger > 0.65f;
        r.triggerPressed = r.triggerDown && !was;
        r.triggerReleased = !r.triggerDown && was;
        const Vec2 uiStick = ApplyThumbstickDeadzone(in.thumbstick, m_settings.StickDeadzone(h));
        const float uiMag = static_cast<float>(StickMagnitude(uiStick));
        r.scroll = uiStick.y * SmoothStep(0.12f, 0.20f, uiMag) * m_settings.scrollSpeed;
        r.push = uiStick.y;

        // Left menu: in VR mode, long-press (0.6 s) opens RinCast's overlay so short/double
        // presses pass cleanly to SteamVR without hijacking SteamVR's menu button.
        if (m_mode == Mode::Vr) {
            if (in.menu) {
                m_handState[h].menuHeld += dt;
                if (!m_handState[h].menuLongFired && m_handState[h].menuHeld >= 0.6f) {
                    m_handState[h].menuLongFired = true;
                    ToggleWindow("vr-overlay", !m_vrOverlay);
                }
            } else {
                m_handState[h].menuHeld = 0;
                m_handState[h].menuLongFired = false;
            }
        } else {
            m_handState[h].menuHeld = 0;
            m_handState[h].menuLongFired = false;
            if (in.menu && !m_handState[h].menu) {
                int64_t now = rc::NowNs();
                const bool twice = now - m_lastMenuPressNs < 450'000'000LL;
                if (m_mode == Mode::Desktop) {
                    if (twice && VrRunning()) {
                        ToggleWindow("dock");  // undo the first press
                        ToggleWindow("vr-overlay", !IsVisible("vr-overlay"));
                        now = 0;  // a third press starts over
                    } else {
                        ToggleWindow("dock");
                    }
                } else {
                    screens::ToggleMenu(*this);
                }
                m_lastMenuPressNs = now;
            }
        }
        m_handState[h].menu = in.menu;
    }
    // Both grips held: the keyboard, without hunting for the dock's button first.
    {
        const HandInput& left = m_input.Hand(0);
        const HandInput& right = m_input.Hand(1);
        const bool both = left.active && right.active && left.squeeze > 0.7f && right.squeeze > 0.7f;
        const float before = m_gripsHeld;
        m_gripsHeld = both ? m_gripsHeld + dt : 0.f;
        if (before < 0.5f && m_gripsHeld >= 0.5f && m_mode == Mode::Desktop) {
            if (m_settings.usePcKeyboard && m_client.GetState() == StreamClient::State::Connected) TogglePcKeyboard();
            else ToggleWindow("keyboard", !IsVisible("keyboard"));
        }
        // L3 + R3 held 0.15 s: toggle performance stats overlay in VR or Desktop.
        const bool bothSticks = left.active && right.active && left.thumbstickClick && right.thumbstickClick;
        const float prevSticks = m_sticksHeld;
        m_sticksHeld = bothSticks ? m_sticksHeld + dt : 0.f;
        if (prevSticks < 0.15f && m_sticksHeld >= 0.15f && (m_mode == Mode::Vr || m_mode == Mode::Desktop)) {
            m_settings.showStats = !m_settings.showStats;
            ToggleWindow("stats", m_settings.showStats);
            SaveSettings();
        }
        // Desktop mode: long-press R3 (0.6 s) to recenter screens.
        const bool r3Only = m_mode == Mode::Desktop && right.active && right.thumbstickClick &&
                            !(left.active && left.thumbstickClick);
        const float prevR3 = m_r3Held;
        m_r3Held = r3Only ? m_r3Held + dt : 0.f;
        if (prevR3 < 0.6f && m_r3Held >= 0.6f) Recenter();
    }
    // One-handed route to the same menu for the right hand: hold A and B.
    {
        const HandInput& right = m_input.Hand(1);
        const bool held = right.active && right.primary && right.secondary;
        const float before = m_abHeld;
        m_abHeld = held ? m_abHeld + dt : 0.f;
        if (before < 0.6f && m_abHeld >= 0.6f) {
            if (m_mode == Mode::Vr) {
                ToggleWindow("vr-overlay", !m_vrOverlay);
            } else if (m_mode == Mode::Desktop) {
                ToggleWindow("dock");
            } else {
                screens::ToggleMenu(*this);
            }
        }
    }

    m_windows.Update(m_rays, m_head, dt, m_time, m_hits);
    if (m_settings.haptics && m_windows.ConsumeHaptic()) m_input.Vibrate(m_windows.HapticHand(), 0.15f, 0.015f);
    if (m_windows.UiContext().ConsumeHoverChange() && m_settings.haptics) m_input.Vibrate(m_windows.HapticHand(), 0.06f, 0.008f);
    if (const std::string speech = m_windows.UiContext().ConsumeSpeech(); m_settings.readAloud && !speech.empty()) Platform::Speak(speech);
    const bool uiClicked = m_windows.UiContext().ConsumeClick();
    if (uiClicked && m_settings.uiSounds) Platform::PlaySound(Platform::Sound::Click);
    if (m_settings.haptics && uiClicked)
        m_input.Vibrate(m_windows.HapticHand(), 0.35f, 0.025f);

    // ---- PC mouse from laser over a monitor ----
    if (m_mode != Mode::Desktop || m_windows.Dragging()) return;
    int mouseHand = -1;
    if (m_settings.desktopInput) {
        const auto hitsVideo = [&](int h) {
            const WindowHit& hit = m_hits[h];
            return hit.window && hit.window->kind == WindowKind::Video && hit.zone == HitZone::Content;
        };
        const int prev = std::clamp(m_lastMouseHand, 0, 1);
        const int other = 1 - prev;
        const HandInput& otherIn = m_input.Hand(other);
        const bool otherActing = m_rays[other].triggerDown || otherIn.primary || otherIn.secondary;
        if (hitsVideo(prev) && (!otherActing || !hitsVideo(other))) {
            mouseHand = prev;
        } else {
            for (int h = 1; h >= 0; h--) {
                if (hitsVideo(h) && (mouseHand < 0 || m_rays[h].triggerDown)) mouseHand = h;
            }
        }
        if (mouseHand >= 0) m_lastMouseHand = mouseHand;
    }
    if (mouseHand < 0) {
        // Release all held mouse buttons when the ray leaves the monitors or input is disabled.
        for (int h = 0; h < 2; h++) {
            if (m_handState[h].trigger) m_client.SendControl({{"type", rc::msg::kMouseButton}, {"button", 0}, {"down", false}});
            if (m_handState[h].secondary) m_client.SendControl({{"type", rc::msg::kMouseButton}, {"button", 1}, {"down", false}});
            if (m_handState[h].stickClick) m_client.SendControl({{"type", rc::msg::kMouseButton}, {"button", 2}, {"down", false}});
            m_handState[h].trigger = false;
            m_handState[h].secondary = false;
            m_handState[h].stickClick = false;
            m_handState[h].clickLocked = false;
        }
        return;
    }
    const WindowHit& hit = m_hits[mouseHand];
    const int streamId = std::stoi(hit.window->id.substr(8));
    const HandInput& in = m_input.Hand(mouseHand);
    auto& st = m_handState[mouseHand];
    float sendU = hit.uv.x;
    float sendV = hit.uv.y;
    if (!in.isHand) {
        if (m_rays[mouseHand].triggerPressed) {
            st.clickLocked = true;
            st.clickLockTime = 0.f;
            st.downU = sendU;
            st.downV = sendV;
        }
        if (st.clickLocked && m_rays[mouseHand].triggerDown) {
            st.clickLockTime += dt;
            const float du = sendU - st.downU;
            const float dv = sendV - st.downV;
            if (st.clickLockTime < 0.12f && (du * du + dv * dv) < (0.008f * 0.008f)) {
                sendU = st.downU;
                sendV = st.downV;
            } else {
                st.clickLocked = false;
            }
        } else if (!m_rays[mouseHand].triggerDown) {
            st.clickLocked = false;
        }
    }
    rc::PointerPayload p{};
    p.monitorStreamId = uint8_t(streamId);
    p.visible = 1;
    p.u = sendU;
    p.v = sendV;
    p.inputSeq = ++m_pointerSeq;
    m_client.SendPointer(p);
    auto button = [&](bool now, bool& prev, int index) {
        if (now != prev) {
            m_client.SendControl({{"type", rc::msg::kMouseButton}, {"button", index}, {"down", now}});
            if (now && m_settings.haptics) m_input.Vibrate(mouseHand, 0.25f, 0.02f);
            prev = now;
        }
    };
    if (in.isHand) {
        const bool down = m_rays[mouseHand].triggerDown;
        using Pinch = HandState::Pinch;
        if (down && !st.trigger) {
            st.pinch = Pinch::Undecided;
            st.pinchTime = 0;
            st.pinchStartV = st.pinchLastV = hit.uv.y;
        }
        const auto click = [&](int index) {
            m_client.SendControl({{"type", rc::msg::kMouseButton}, {"button", index}, {"down", true}});
            m_client.SendControl({{"type", rc::msg::kMouseButton}, {"button", index}, {"down", false}});
            if (m_settings.haptics) m_input.Vibrate(mouseHand, 0.25f, 0.02f);
        };
        if (down) {
            st.pinchTime += dt;
            if (st.pinch == Pinch::Undecided && std::fabs(hit.uv.y - st.pinchStartV) > 0.02f) st.pinch = Pinch::Scrolling;
            if (st.pinch == Pinch::Scrolling) {
                // The page follows the hand, as on a touch screen.
                m_client.SendControl({{"type", rc::msg::kMouseWheel}, {"dx", 0.f}, {"dy", (hit.uv.y - st.pinchLastV) * 24.f * m_settings.scrollSpeed}});
                st.pinchLastV = hit.uv.y;
            } else if (st.pinch == Pinch::Undecided && st.pinchTime > 0.6f) {
                click(1);
                st.pinch = Pinch::Done;
            }
        } else if (st.trigger && st.pinch == Pinch::Undecided) {
            click(0);
        }
        st.trigger = down;
        (void)frame;
        return;
    }
    button(m_rays[mouseHand].triggerDown, st.trigger, 0);
    // B or Y right-clicks, a moment later: holding A and B opens the menu instead.
    if (in.secondary && in.primary) st.chord = true;
    if (in.secondary) {
        st.secondaryHeld += dt;
    } else if (st.secondaryHeld > 0 && !st.secondary && !st.chord) {
        // A quick tap, over before the wait ended: still a click.
        button(true, st.secondary, 1);
    }
    button(in.secondary && !st.chord && (st.secondary || st.secondaryHeld > 0.1f), st.secondary, 1);
    if (!in.secondary) {
        st.secondaryHeld = 0;
        if (!in.primary) st.chord = false;
    }
    button(in.thumbstickClick, st.stickClick, 2);
    // Smooth scrolling: thumbstick -> wheel notches.
    const Vec2 deskStick = ApplyThumbstickDeadzone(in.thumbstick, m_settings.StickDeadzone(mouseHand));
    const float deskMag = static_cast<float>(StickMagnitude(deskStick));
    const float sy = deskStick.y * SmoothStep(0.15f, 0.25f, deskMag);
    const float sx = deskStick.x * SmoothStep(0.45f, 0.55f, deskMag);
    if (sy != 0 || sx != 0)
        m_client.SendControl({{"type", rc::msg::kMouseWheel},
                              {"dx", sx * dt * 8.f * m_settings.scrollSpeed},
                              {"dy", sy * dt * 12.f * m_settings.scrollSpeed}});
    (void)frame;
}

static void PutHead(rc::TrackingPayload& t, const Pose& p, const Vec3& lin, const Vec3& ang) {
    t.hmd.orientation[0] = p.orientation.x;
    t.hmd.orientation[1] = p.orientation.y;
    t.hmd.orientation[2] = p.orientation.z;
    t.hmd.orientation[3] = p.orientation.w;
    t.hmd.position[0] = p.position.x;
    t.hmd.position[1] = p.position.y;
    t.hmd.position[2] = p.position.z;
    std::memcpy(t.hmdLinearVelocity, &lin, sizeof(float) * 3);
    std::memcpy(t.hmdAngularVelocity, &ang, sizeof(float) * 3);
}

template <typename Controller>
static void PutGrip(Controller& c, const Pose& g, const Vec3& lin, const Vec3& ang) {
    c.pose.orientation[0] = g.orientation.x;
    c.pose.orientation[1] = g.orientation.y;
    c.pose.orientation[2] = g.orientation.z;
    c.pose.orientation[3] = g.orientation.w;
    c.pose.position[0] = g.position.x;
    c.pose.position[1] = g.position.y;
    c.pose.position[2] = g.position.z;
    std::memcpy(c.linearVelocity, &lin, sizeof(float) * 3);
    std::memcpy(c.angularVelocity, &ang, sizeof(float) * 3);
}

void App::PoseThread() {
    std::unique_lock lock(m_poseMutex);
    while (!m_poseThreadStop) {
        if (!m_heldPose) {
            m_poseCv.wait(lock);
            continue;
        }
        const int64_t wait = m_heldPose->sendAtNs - rc::NowNs();
        if (wait > 0) {
            m_poseCv.wait_for(lock, std::chrono::nanoseconds(wait));
            continue;
        }
        HeldPose p = *m_heldPose;
        m_heldPose.reset();
        lock.unlock();
        SendPose(p);
        lock.lock();
    }
}

// Head and controllers located again for the same display time, now that it is later: what
// the PC renders with is that much fresher, and predicted that much less far ahead.
void App::SendPose(HeldPose& p) {
    rc::TrackingPayload& t = p.t;
    if (p.relocateHead || p.relocateGrip[0] || p.relocateGrip[1]) {
        t.sampleTimeNs = uint64_t(rc::NowNs());
        Pose pose;
        Vec3 lin, ang;
        if (p.relocateHead && m_xr.LocateSpace(m_xr.ViewSpace(), p.target, pose, &lin, &ang)) PutHead(t, pose, lin, ang);
        for (int h = 0; h < 2; h++)
            if (p.relocateGrip[h] && m_input.LocateGrip(h, p.target, pose, &lin, &ang)) PutGrip(t.controllers[h], pose, lin, ang);
    }
    m_client.SendTracking(t, p.hasGaze ? &p.gaze : nullptr);
    std::lock_guard lock(m_trackingSentMutex);
    m_trackingSent[t.targetDisplayTimeNs] = int64_t(t.sampleTimeNs);
    while (m_trackingSent.size() > 512) m_trackingSent.erase(m_trackingSent.begin());
}

// The PC's report of how long poses waited there for SteamVR. Held back by that much, they
// would arrive just in time; the 2% that waited least are kept 1 ms early, as Wi-Fi delays
// some packets more than others, and one that misses leaves the game a pose a frame old.
void App::OnPoseWait(const json& m) {
    const int64_t period = m.value("periodNs", int64_t(0));
    m_poseWaitNs = m.value("waitNs", int64_t(0));
    m_poseWaitLowNs = m.value("lowNs", int64_t(0));
    m_poseWaitAtNs = rc::NowNs();
    if (!m_debugPoseHold || period <= 0) {
        m_poseHoldNs = 0;
        return;
    }
    // Later in steps, earlier at once. Each report already counts the hold it was sent with.
    const double move = double(m_poseWaitLowNs) - 1e6;
    m_poseHoldNs = std::clamp(m_poseHoldNs + (move > 0 ? 0.5 * move : move), 0.0, 0.7 * double(period));
}

void App::UpdateTracking(const FrameState& frame) {
    // Predict to when the PC's frame for this pose will reach the display.
    const XrTime target = frame.predictedDisplayTime + XrTime(m_pipelineLatencyMs * 1e6);
    HeldPose held;
    held.target = target;
    rc::TrackingPayload& t = held.t;
    t.sampleTimeNs = uint64_t(rc::NowNs());
    t.targetDisplayTimeNs = uint64_t(m_xr.ToMonotonicNs(target));
    Pose head;
    Vec3 lin, ang;
    if (m_xr.LocateSpace(m_xr.ViewSpace(), target, head, &lin, &ang)) {
        t.flags |= rc::kTrackHmdValid;
        PutHead(t, head, lin, ang);
        held.relocateHead = true;
    }
    HandInput predicted[2];
    m_input.Locate(target, predicted);
    for (int h = 0; h < 2; h++) {
        const HandInput& in = m_input.Hand(h);
        auto& c = t.controllers[h];
        if (!in.active) {
            // A controller held still goes to sleep after a while, and stops being tracked. Told
            // so, SteamVR took it away and games lost it (out of view) until it was moved again.
            // It stays where it was last seen instead, as it does in the headset's own home.
            if (m_haveControllerPose[h]) {
                c.flags = rc::kCtrlConnected | rc::kCtrlPoseValid;
                c.pose = m_controllerPose[h];
            }
            continue;
        }
        c.flags = rc::kCtrlConnected;
        rc::Pose joints[rc::kHandJointCount];
        const auto sendBones = [&] {
            rc::HandSkeletonPayload bones{};
            bones.hand = uint8_t(h);
            rc::HandJointsToBones(h, joints, bones.bones);
            m_client.SendHandSkeleton(bones);
        };
        const bool playingWithHand = in.isHand && m_settings.handsInGames && m_mode == Mode::Vr && m_input.LocateHandJoints(h, target, joints);
        // Only the fingers matter here: the bones are placed relative to the palm, so a slower
        // hand can't shake against the controller. The hand runs at about 30 Hz in this mode,
        // so don't ask it to predict as far ahead as the controller.
        if (in.held && m_input.LocateHandJoints(h, frame.predictedDisplayTime, joints)) sendBones();
        if (playingWithHand) {
            c.flags |= rc::kCtrlPoseValid;
            c.pose = rc::HandDevicePose(h, joints[0]);
            sendBones();
        } else if (predicted[h].gripValid) {
            c.flags |= rc::kCtrlPoseValid;
            PutGrip(c, predicted[h].grip, predicted[h].gripLinearVelocity, predicted[h].gripAngularVelocity);
            if (!in.isHand) {
                m_controllerPose[h] = c.pose;
                m_haveControllerPose[h] = true;
                held.relocateGrip[h] = true;
            }
        } else if (!in.isHand && m_haveControllerPose[h]) {
            c.flags |= rc::kCtrlPoseValid;  // tracking lost for a moment: where it was last seen
            c.pose = m_controllerPose[h];
        }
        if (in.isHand) c.flags |= rc::kCtrlHandTracked;
        // Don't leak clicks on the desktop or the overlay into a game still running behind them.
        const bool gameInput = m_mode == Mode::Vr && !m_vrOverlay;
        if (gameInput) {
            uint32_t b = 0;
            if (h == 0) {
                if (in.primary) b |= rc::kBtnX;
                if (in.secondary) b |= rc::kBtnY;
                if (in.touchPrimary) b |= rc::kTouchX;
                if (in.touchSecondary) b |= rc::kTouchY;
                if (in.menu) b |= rc::kBtnMenu;
            } else {
                const bool menuChord = in.primary && in.secondary;  // hold both: RinCast's menu
                if (in.primary && !menuChord) b |= rc::kBtnA;
                if (in.secondary && !menuChord) b |= rc::kBtnB;
                if (in.touchPrimary) b |= rc::kTouchA;
                if (in.touchSecondary) b |= rc::kTouchB;
            }
            if (in.thumbstickClick) b |= rc::kBtnThumbstick;
            if (in.touchThumbstick) b |= rc::kTouchThumbstick;
            if (in.touchTrigger) b |= rc::kTouchTrigger;
            if (in.touchThumbrest) b |= rc::kTouchThumbrest;
            if (in.trigger > 0.85f) b |= rc::kBtnTrigger;
            if (in.squeeze > 0.85f) b |= rc::kBtnGrip;
            c.buttons = b;
            c.trigger = in.trigger;
            c.grip = in.squeeze;
            const Vec2 stick = ApplyThumbstickDeadzone(in.thumbstick, m_settings.StickDeadzone(h));
            c.thumbstick[0] = stick.x;
            c.thumbstick[1] = stick.y;
            if (playingWithHand) {
                // A fist closes the fingers that would otherwise read as pinches.
                c.grip = rc::HandGrip(joints);
                if (c.grip > 0.85f) c.buttons |= rc::kBtnGrip;
                if (c.grip < 0.5f) {
                    if (in.pinchMiddle > 0.85f) c.buttons |= h == 0 ? rc::kBtnX : rc::kBtnA;
                    if (in.pinchRing > 0.85f) c.buttons |= h == 0 ? rc::kBtnY : rc::kBtnB;
                }
            }
        }
    }
    // Where the eyes look, for foveated encoding. Now rather than predicted: eyes jump, and the
    // PC keeps every pixel of a square around it.
    const bool foveate = m_settings.gazeFoveation && m_mode == Mode::Vr && GazeSupported();
    if (!foveate) {
        m_headset->StopGaze();
    } else if (!m_gazePermissionAsked) {
        m_gazePermissionAsked = true;
        Platform::RequestPermissions(m_headset->GazePermissions());
    }
    held.hasGaze = foveate && ReadGaze(frame.predictedDisplayTime, held.gaze);
    // Held back only while the PC says how long poses wait there (SteamVR games); otherwise
    // sent at once, as it always was.
    const int64_t now = rc::NowNs();
    if (!m_debugPoseHold || m_mode != Mode::Vr || now - m_poseWaitAtNs > 3'000'000'000LL) m_poseHoldNs = 0;
    const bool hold = m_poseHoldNs >= 300'000;
    held.sendAtNs = int64_t(t.sampleTimeNs) + int64_t(m_poseHoldNs);
    std::optional<HeldPose> previous;
    {
        std::lock_guard lock(m_poseMutex);
        previous = std::exchange(m_heldPose, std::nullopt);
        if (hold) m_heldPose = held;
    }
    if (previous) SendPose(*previous);  // this frame came before the last one's packet went: now
    if (hold) {
        m_poseCv.notify_one();
    } else {
        held.relocateHead = held.relocateGrip[0] = held.relocateGrip[1] = false;  // just located
        SendPose(held);
    }
    UpdateBody(frame);
    UpdateFace(frame);
}

bool App::ReadGaze(XrTime time, rc::GazePayload& out) {
    if (m_headset->GazeSupported(m_xr)) return m_headset->ReadGaze(m_xr, time, out);
    float d[3];
    if (!m_input.LocateGaze(time, d)) return false;
    // One gaze for both eyes: plenty to place the sharp square.
    out.flags = rc::kGazeLeftValid | rc::kGazeRightValid;
    for (int eye = 0; eye < 2; eye++) std::memcpy(out.gaze[eye], d, sizeof(d));
    return true;
}

void App::UpdateBody(const FrameState& frame) {
    const bool body = m_settings.bodyTracking && m_mode == Mode::Vr && m_headset->BodySupported(m_xr);
    // Stop body tracking before simultaneous tracking starts, and the other way round.
    if (!body) m_headset->StopBody();
    m_input.SetSimultaneous(m_settings.fingersWithControllers && m_mode == Mode::Vr &&
                            !(m_settings.bodyTracking && m_headset->BodyExcludesFingers()));
    if (!body) return;
    if (!m_bodyPermissionAsked) {
        m_bodyPermissionAsked = true;
        Platform::RequestPermissions(m_headset->BodyPermissions());
    }
    rc::BodyTrackingPayload b{};
    // For the current frame rather than the stream's prediction: body estimates are the
    // headset's own best guess already, and fail when asked far ahead.
    if (m_headset->LocateBody(m_xr, frame.predictedDisplayTime, b)) m_client.SendBody(b);
}

void App::UpdateFace(const FrameState& frame) {
    const bool face = m_settings.faceTracking && m_mode == Mode::Vr && m_headset->FaceSupported(m_xr);
    if (!face) {
        m_headset->StopFace();
        return;
    }
    if (!m_facePermissionAsked) {
        m_facePermissionAsked = true;
        Platform::RequestPermissions(m_headset->FacePermissions());
    }
    rc::FaceTrackingPayload f{};
    if (m_headset->ReadFace(m_xr, frame.predictedDisplayTime, f)) m_client.SendFace(f);
}

void App::ReadMotion(const std::vector<uint8_t>& metadata) {
    m_motionMoves = false;
    m_motionConsistent = false;
    m_motionFrameNs = 0;
    m_motionFailure = 0;
    m_motionMetadataBytes = metadata.size();
    m_motionGateMoving = m_motionGateMean = m_motionGateSpread = m_motionGateFar = 0;
    m_motionGateCoherent = m_motionGateGlobal = 0;
    if (!m_settings.frameGeneration) return;
    ++m_motionPackets;
    rc::HeadsetMotionState state{m_motionMoves, m_motionConsistent, m_motionWidth, m_motionHeight,
                                 m_motionBlock, m_motionFrameNs, m_motionIntervalSmoothNs,
                                 std::move(m_motionGrid)};
    const rc::MotionDecodeResult res =
        rc::DecodeVrMotionMetadata(true, m_vrFoveated, m_vrMeta.trackingTimestampNs, metadata, state);
    m_motionGrid = std::move(state.grid);
    if (res.failureStage != 1) {
        const rc::MotionHeader h = res.header;
        m_lastMotionHeader = h;
    }
    if (res.failureStage != 0) {
        ++m_motionPacketFailures;
        if (res.failureStage == 1) ++m_motionFailureCounts[1];
        if (res.failureStage == 2) {
            ++m_motionFailureCounts[2];
            m_lastMotionFailureHeader = res.header;
            m_lastMotionFailureTrackingNs = m_vrMeta.trackingTimestampNs;
            if (res.headerFailure & rc::kMotionHeaderZeroWidth) ++m_motionHeaderFailures[0];
            if (res.headerFailure & rc::kMotionHeaderZeroHeight) ++m_motionHeaderFailures[1];
            if (res.headerFailure & rc::kMotionHeaderZeroBlock) ++m_motionHeaderFailures[2];
            if (res.headerFailure & rc::kMotionHeaderNonAdvancingTimestamp) ++m_motionHeaderFailures[3];
            if (res.headerFailure & uint8_t(~rc::kMotionHeaderFailureMask)) ++m_motionHeaderFailures[4];
        }
        else if (res.failureStage == 3) ++m_motionFailureCounts[3];
        m_motionFailure = res.failureStage;
        return;
    }
    m_motionWidth = state.width;
    m_motionHeight = state.height;
    m_motionBlock = state.block;
    m_motionFrameNs = state.frameNs;
    m_motionIntervalSmoothNs = state.intervalSmoothNs;
    m_motionConsistent = state.consistent;
    m_motionMoves = state.moves;
    m_motionGateMoving = res.gate.moving;
    m_motionGateMean = res.gate.mean;
    m_motionGateSpread = res.gate.spread;
    m_motionGateFar = res.gate.farRejected;
    m_motionGateCoherent = res.gate.coherent;
    m_motionGateGlobal = res.gate.global;
    if (res.uploadMotion) m_scene.UploadMotion(m_motionGrid.data(), m_motionWidth, m_motionHeight);
}

// No new frame for this display: move the moving things on from the last picture. The layer
// keeps the last frame's pose, so the compositor still turns it with the head.
void App::GenerateFrame(const FrameState& frame) {
    ++m_generationDiag.calls;
    rc::GenerationInput in;
    in.enabled = m_settings.frameGeneration;
    in.vr = m_mode == Mode::Vr;
    in.latched = m_vrLatched;
    in.hasFrame = m_vrHasFrame;
    in.swapchain = m_vrSwapchain.handle != 0;
    in.motionMoves = m_motionMoves;
    in.motionConsistent = m_motionConsistent;
    in.motionFrameNs = m_motionFrameNs;
    in.sinceFrameNs = rc::ElapsedSinceLatchedFrameNs(m_xr.ToMonotonicNs(frame.predictedDisplayTime), m_vrLatchedDisplayNs,
                                                     int64_t(m_vrMeta.trackingTimestampNs));
    in.displayNs = frame.period > 0 ? double(frame.period) : 1e9 / 90.0;
    in.motionIntervalSmoothNs = m_motionIntervalSmoothNs;
    in.upscaled = m_vrUpscaled;
    in.externalReady = m_scene.ExternalGenerationReady();
    const float fieldW = rc::MotionFieldExtent(m_motionWidth, m_motionBlock);
    const float fieldH = rc::MotionFieldExtent(m_motionHeight, m_motionBlock);
    in.fieldWidth = fieldW;
    in.fieldHeight = fieldH;
    // Forced cadence only below ~60 fps at 72 Hz: above that the real frames already cover the
    // display slots, and a forced frame would just take quota from a healthy stream.
    in.force = rc::ShouldForceFrameGeneration(m_motionIntervalSmoothNs, m_motionFrameNs, in.displayNs);
    // Both DrawKeptMoved (un-upscaled m_gsrTexture) and DrawExternalMoved sample the
    // source-resolution VR picture, not the upscaled swapchain target.
    const uint32_t pictureW = m_vrEyeWidth > 0 ? uint32_t(m_vrEyeWidth) * 2u : uint32_t(m_vrSwapchain.width);
    const uint32_t pictureH = m_vrEyeHeight > 0 ? uint32_t(m_vrEyeHeight) : uint32_t(m_vrSwapchain.height);
    const float fieldU = rc::MotionFieldUvScale(m_motionWidth, m_motionBlock, pictureW);
    const float fieldV = rc::MotionFieldUvScale(m_motionHeight, m_motionBlock, pictureH);
    const rc::GenerationDecision decision = rc::DecideFrameGeneration(m_genQuota, in);
    if (decision.attempt || decision.quotaBlocked) {
        m_generationDiag.last = decision;
        m_lastGenCap = decision.cap;
    }
    if (decision.attempt) ++m_generationDiag.attempts;
    if (decision.quotaBlocked) ++m_generationDiag.quotaBlocked;
    if (decision.attempt) m_genStats.Attempt();
    if (!decision.draw) {
        // Zero-generation causal sample (fixed size, in place — no per-frame allocation).
        if (m_zeroSampleCount < sizeof(m_zeroSamples) / sizeof(m_zeroSamples[0])) {
            ZeroGenSample& s = m_zeroSamples[m_zeroSampleCount++];
            // Correlate with the PC by trackingTimestampNs, the one value both ends copy
            // verbatim (PC meta.trackingTimestampNs = job.headsetTargetNs).  The wire seq is
            // NOT the PC's producer counter: VideoSender::Send assigns ++m_frameIndex and
            // ignores SendJob::frameIndex, so every stale/oversize drop drifts the two
            // permanently.  Keep the seq for ordering, use ts for cross-end alignment.
            s.frameIndex = m_vrFrameIndex;
            s.trackingTimestampNs = m_vrMeta.trackingTimestampNs;
            s.localSeq = ++m_zeroGenFrameIndex;
            s.reason = uint8_t(decision.reason);
            s.flags = uint8_t((in.motionMoves ? 1 : 0) | (in.motionConsistent ? 2 : 0) |
                              (m_motionMetadataBytes > 0 ? 4 : 0) | (!decision.quotaBlocked ? 8 : 0));
            s.motionBytes = uint16_t(std::min<size_t>(m_motionMetadataBytes, 65535u));
            s.sinceMs = float(in.sinceFrameNs / 1e6);
            s.smoothMs = float(in.motionIntervalSmoothNs / 1e6);
            s.amount = decision.amount;
            s.cap = decision.cap;
        }
        m_generationWindow.Record(decision);
        return;
    }
    const GLuint fbo = m_xr.AcquireImage(m_vrSwapchain);
    if (!fbo) {
        ++m_generationDiag.acquireFailed;
        m_vrHasFrame = false;
        m_generationWindow.Record(decision, false);
        return;
    }
    bool drew = false;
    if (decision.path == rc::GenerationPath::Kept) {
        drew = m_scene.DrawKeptMoved(fbo, m_vrSwapchain.width, m_vrSwapchain.height, decision.scaleX, decision.scaleY, fieldU,
                                     fieldV);
    } else if (decision.path == rc::GenerationPath::External) {
        drew = m_scene.DrawExternalMoved(fbo, m_vrSwapchain.width, m_vrSwapchain.height, decision.scaleX, decision.scaleY, fieldU,
                                         fieldV);
    }
    const bool released = m_xr.ReleaseImage(m_vrSwapchain);
    if (!released) ++m_generationDiag.releaseFailed;
    if (!drew) ++m_generationDiag.drawFailed;
    drew = released && drew;
    m_generationWindow.Record(decision, drew);
    if (!drew) {
        m_vrHasFrame = false;
        return;
    }
    glFlush();  // as for a new frame: the GPU starts now
    m_generatedFrames++;
    m_generatedFramesWindow++;
    ++m_generationDiag.generated;
    m_genStats.Generated();
}

void App::LatchVideo(const FrameState& frame, bool late) {
    // Latching waits for swapchain images and blits: keep the network thread out of that.
    // (Only this thread removes decoders, so the last reference still drops on the GL thread.)
    std::map<uint8_t, std::shared_ptr<VideoDecoder>> decoders;
    {
        std::lock_guard lock(m_decoderMutex);
        decoders = m_decoders;
    }
    for (auto& [id, dec] : decoders) {
        if (!dec) continue;
        // Taken early, a frame would only sit in its layer while the menus are drawn, and one
        // decoded in that time would wait for the next display.
        if (id == rc::kVrStreamId && !late) continue;
        DecodedFrameInfo info;
        if (!dec->Latch(info)) {
            if (id == rc::kVrStreamId) {
                ++m_latchDiag.noImage;
                m_behindSinceNs = 0;  // no picture to take: none waited behind one either
            }
            continue;
        }
        const int64_t latchNs = rc::NowNs();
        const auto texture = dec->Geometry();
        if (id == rc::kVrStreamId) {
            m_vrMetadataBytes = info.metadata.size();
            m_vrLatchNs = latchNs;
            const bool haveMeta = info.metadata.size() >= sizeof(rc::VrFrameMetadata);
            const bool haveFoveation = !m_vrFoveated || info.metadata.size() >= sizeof(rc::VrFrameMetadata) + sizeof(rc::VrFoveationMetadata);
            if (!haveMeta || !haveFoveation) {
                ++m_latchDiag.metadataMissing;
                m_vrHasFrame = m_vrLatched = m_motionMoves = m_motionConsistent = false;
                m_vrLatchNs = m_vrLatchedDisplayNs = 0;
                if (m_settings.frameGeneration) {
                    ++m_motionPackets;
                    ++m_motionPacketFailures;
                    ++m_motionFailureCounts[1];
                    m_motionFailure = 1;
                }
                continue;
            }
            rc::VrFrameMetadata nextMeta;
            std::memcpy(&nextMeta, info.metadata.data(), sizeof(nextMeta));
            rc::VrFoveationMetadata nextFoveation{};
            if (m_vrFoveated)
                std::memcpy(&nextFoveation, info.metadata.data() + sizeof(rc::VrFrameMetadata), sizeof(nextFoveation));
            if (!m_vrSwapchain.handle || m_settings.upscaling != m_vrUpscaleSetting) CreateVrSwapchain();
            if (!m_vrSwapchain.handle) {
                ++m_latchDiag.swapchainMissing;
                m_vrHasFrame = m_vrLatched = false;
                m_vrLatchNs = m_vrLatchedDisplayNs = 0;
                continue;
            }
            const GLuint fbo = m_xr.AcquireImage(m_vrSwapchain);
            if (!fbo) {
                ++m_latchDiag.acquireFailed;
                m_vrHasFrame = m_vrLatched = false;
                m_vrLatchNs = m_vrLatchedDisplayNs = 0;
                continue;
            }
            m_vrMeta = nextMeta;
            if (m_vrFoveated) std::memcpy(m_vrFoveation.start, nextFoveation.foveaStart, sizeof(nextFoveation.foveaStart));
            const VideoFoveation* foveation = m_vrFoveated ? &m_vrFoveation : nullptr;
            TimeVrDraw(true);
            const float effectiveSharpen = (!m_vrUpscaled && !m_xr.LayerSettings() && (m_stats.mbps == 0 || m_stats.mbps >= 35))
                                               ? std::clamp(m_settings.vrSharpen, 0.0f, 1.0f)
                                               : 0.0f;
            const float effectiveVibrance = std::clamp(m_settings.vrVibrance, 0.0f, 0.5f);
            const float effectiveDarkGamma = std::clamp(m_settings.vrDarkGamma, 0.0f, 0.35f);
            m_scene.SetVrImageAdjust(effectiveSharpen, effectiveVibrance, effectiveDarkGamma);
            m_scene.SetFoveaDebug(m_forceFoveaDebug == 1 || (m_forceFoveaDebug == 2 && m_settings.foveaDebug));
            if (m_vrUpscaled) {
                m_scene.KeepVideo(texture.texture, texture.crop, m_vrEyeWidth * 2, m_vrEyeHeight, foveation);
                m_scene.DrawKeptUpscaled(fbo, m_vrSwapchain.width, m_vrSwapchain.height);
            } else {
                m_scene.BlitExternal(fbo, texture.texture, m_vrSwapchain.width, m_vrSwapchain.height, texture.crop, 0, 0,
                                     1, 1, foveation, effectiveSharpen, effectiveVibrance, effectiveDarkGamma,
                                     texture.width, texture.height);
            }
            TimeVrDraw(false);
            if (!m_xr.ReleaseImage(m_vrSwapchain)) {
                ++m_latchDiag.releaseFailed;
                m_vrHasFrame = m_vrLatched = false;
                m_vrLatchNs = m_vrLatchedDisplayNs = 0;
                continue;
            }
            if (!m_vrUpscaled && m_settings.frameGeneration)
                m_scene.RememberExternal(texture.texture, texture.width, texture.height, texture.crop, foveation);
            // The GPU starts on the picture now, while the rest of the frame is put
            // together, not when the frame is handed in.
            glFlush();
            const int64_t displayNs = m_xr.ToMonotonicNs(frame.predictedDisplayTime);
            m_vrHasFrame = true;
            m_vrLatched = true;
            m_vrFrameIndex = info.frameIndex;
            m_vrLatchedDisplayNs = displayNs;
            // Each decoded frame owns a fresh budget for the empty display slots that follow it.
            // A sudden drop from 72 to 30 fps must land on the wide budget immediately: the 0.8/0.2
            // EMA would otherwise keep the old, short interval for another 4-5 frames and starve
            // exactly the slots this change exists to fill.
            ReadMotion(info.metadata);
            const double slotIntervalNs =
                std::max(m_motionIntervalSmoothNs, double(m_motionFrameNs > 0 ? m_motionFrameNs : 0));
            const int generationSlots = rc::FrameGenerationSlots(slotIntervalNs, double(frame.period));
            m_genQuota.Arm(m_settings.frameGeneration, generationSlots);
            if (m_settings.frameGeneration && !m_genQuotaAnnounced) {
                m_genQuotaAnnounced = true;
                RC_LOGI("Frame generation quota armed: %d slots per decoded frame", generationSlots);
            }
            m_vrFrames++;
            // ROUND20 T4: the decoded-frame seq range this diag window covers (telemetry
            // only; the PC logs its producer's own seq so logpair can pair the ends).
            if (!m_seqWindowMin) m_seqWindowMin = m_vrFrames;
            m_seqWindowMax = m_vrFrames;
            // Latency feedback: the frame was rendered for `trackingTimestampNs` and will be
            // shown at this frame's display time; steer our prediction toward reality.
            const double errorMs = double(displayNs - int64_t(m_vrMeta.trackingTimestampNs)) / 1e6;
            if (std::fabs(errorMs) < 200) m_pipelineLatencyMs = std::clamp(m_pipelineLatencyMs + errorMs * 0.05, 15.0, 120.0);
            int64_t sampledNs = 0;
            {
                std::lock_guard lock(m_trackingSentMutex);
                auto it = m_trackingSent.find(m_vrMeta.trackingTimestampNs);
                if (it != m_trackingSent.end()) sampledNs = it->second;
            }
            // A22 (TASK-LATENCY-EMA): the panels' EMA takes at most one latency sample per
            // display frame. Pose age comes first (what VD's overlay shows); a desktop latch's
            // transport age only fills the slot when no pose age landed this frame, and frames
            // with neither leave the EMA alone. The 1 s window sums below stay raw.
            if (sampledNs) m_frameLatencyMs = double(displayNs - sampledNs) / 1e6;
            // TASK-LATENCY-METER-FIX-2026-11-22 §1.1: the total and the stages count the
            // same frames under the same conditions, so the stages add up to the total
            // instead of falling short of it. Frames before the clocks are matched are
            // left out of both (the first seconds of a session only).
            if (sampledNs && haveMeta && info.receivedNs && info.decodedNs && m_client.HasClockOffset()) {
                // PC times onto the headset's clock. How the pose's trip to the PC and the
                // game share the time between them isn't measured: the trip counts as half
                // the round trip.
                m_latencySum += double(displayNs - sampledNs) / 1e6;
                m_latencyCount++;
                const int64_t offset = m_client.ClockOffsetNs();
                const double up = m_client.Stats().rttMs / 2;
                double s[kStageCount] = {-1, -1, -1, -1, -1, -1, -1, -1, -1};
                s[kStageUplink] = up;
                s[kStageGame] = double(m_vrMeta.pcSubmitNs - offset - sampledNs) / 1e6 - up;
                s[kStageEncoderWait] = double(m_vrMeta.pcEncodeStartNs - m_vrMeta.pcSubmitNs) / 1e6;
                s[kStageEncode] = double(m_vrMeta.pcEncodeEndNs - m_vrMeta.pcEncodeStartNs) / 1e6;
                s[kStageDownlink] = double(info.receivedNs - (m_vrMeta.pcEncodeEndNs - offset)) / 1e6;
                s[kStageDecode] = double(info.decodedNs - info.receivedNs) / 1e6;
                s[kStageNextFrame] = double(latchNs - info.decodedNs) / 1e6;
                s[kStageCompositor] = double(displayNs - latchNs) / 1e6;
                AddStages(s);
            }
            // Where this frame landed against the moment it should have been ready: the late
            // latch, less a margin. Folded into one display period, since a frame that just
            // missed it is the same story as one a whole period too early. Not while the PC
            // moves its frames to let waiting pictures run out: they land all over the period.
            if (m_latchLeadNs > 0 && info.decodedNs && frame.period > 0 && latchNs >= m_slipUntilNs) {
                const double period = double(frame.period);
                const double target = double(displayNs) - m_latchLeadNs - m_phaseMarginNs;
                double e = std::fmod(double(info.decodedNs) - target, period);
                if (e >= period / 2) e -= period;
                if (e < -period / 2) e += period;
                m_phaseErrors.push_back(e);
                if (e > m_phaseMarginNs) {
                    m_flowLate++;
                    m_diagFlowLate++;
                }
                // For the log, folded the way SteerVrTiming folds lateness (frames run late far
                // more than early): up to 80% of a period late, up to 20% early.
                const double late = std::fmod(e + 1.2 * period, period) - 0.2 * period;
                m_flowSlackMs.push_back(float((m_phaseMarginNs - late) / 1e6));
            }
            CatchUpVr(*dec, info.waiting, frame);
        } else {
            Window* w = m_windows.Find("monitor-" + std::to_string(id));
            if (!w) continue;
            w->videoTexture = texture.texture;
            std::memcpy(w->videoCrop, texture.crop, sizeof(w->videoCrop));
            if (w->layerBacked) {
                auto& sc = m_monitorLayers[id];
                if (!sc.handle)
                    m_xr.CreateSwapchain(dec->Width(), dec->Height(), GL_SRGB8_ALPHA8, 1, false, sc, 1,
                                         ("monitor-" + std::to_string(id)).c_str());
                if (sc.handle) {
                    m_monitorFresh[id] = false;
                    w->videoReady = false;
                    const GLuint fbo = m_xr.AcquireImage(sc);
                    if (fbo) {
                        m_scene.BlitExternal(fbo, texture.texture, sc.width, sc.height, texture.crop);
                        if (m_xr.ReleaseImage(sc)) {
                            m_monitorFresh[id] = true;
                            w->videoReady = true;
                        }
                    }
                }
            } else {
                w->videoReady = true;
            }
            if (info.receivedNs) {
                const int64_t displayNs = m_xr.ToMonotonicNs(frame.predictedDisplayTime);
                rc::DesktopFrameMetadata dm{};
                if (info.metadata.size() >= sizeof(dm) && info.decodedNs && m_client.HasClockOffset()) {
                    // From the PC's capture to the headset's display, as the protocol defines it.
                    std::memcpy(&dm, info.metadata.data(), sizeof(dm));
                    const int64_t offset = m_client.ClockOffsetNs();
                    const double transportMs = double(displayNs - (dm.pcCaptureNs - offset)) / 1e6;
                    m_latencySum += transportMs;
                    // A22 (TASK-LATENCY-EMA): transport age only fills the EMA's slot when no
                    // pose age landed this display frame (the VR latch's pose age wins).
                    if (m_frameLatencyMs < 0) m_frameLatencyMs = transportMs;
                    double s[kStageCount] = {-1, -1, -1, -1, -1, -1, -1, -1, -1};
                    s[kStageCapture] = double(dm.pcEncodeEndNs - dm.pcCaptureNs) / 1e6;
                    s[kStageDownlink] = double(info.receivedNs - (dm.pcEncodeEndNs - offset)) / 1e6;
                    s[kStageDecode] = double(info.decodedNs - info.receivedNs) / 1e6;
                    s[kStageNextFrame] = double(latchNs - info.decodedNs) / 1e6;
                    s[kStageCompositor] = double(displayNs - latchNs) / 1e6;
                    AddStages(s);
                } else {
                    // Before the clocks are matched: the headset's part only.
                    m_latencySum += double(displayNs - info.receivedNs) / 1e6;
                }
                m_latencyCount++;
            }
        }
        m_genStats.source += id == rc::kVrStreamId || m_mode != Mode::Vr ? 1 : 0;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // A22 (TASK-LATENCY-EMA): commit the display frame's one staged latency sample to the
    // panels' EMA - the same lerp VD's overlay uses (PerformanceOverlay.cs:386: alpha 0.02,
    // a ~50-frame time constant), so spikes smooth into the trend. No 0.999 s cap: that cap
    // is an artifact of VD's seconds-normalized VRLatency, ours is plain milliseconds. VD
    // also floors its total at the runtime's motion-to-photon minimum, which OpenXR gives us
    // no equivalent API to query - and the pose age already contains the trip to the PC, so
    // that floor is not reproduced. Only the late latch commits: both latches of a display
    // frame stage into the same slot, so the EMA still moves at most once per display frame.
    if (late && m_frameLatencyMs >= 0) {
        m_latencyEma = m_latencyEma < 0 ? m_frameLatencyMs : m_latencyEma + 0.02 * (m_frameLatencyMs - m_latencyEma);
        m_frameLatencyMs = -1;
    }

    // Once per second: statistics for the overlay and the PC.
    const int64_t now = rc::NowNs();
    if (now - m_statsWindowStart > 1'000'000'000LL) {
        const double secs = double(now - m_statsWindowStart) / 1e9;
        m_statsWindowStart = now;
        // ROUND19 E1 probe: the displayTime delta switch, polled here once a second (no per-frame
        // IO). Missing, empty or unparsable file reads as 0, i.e. the untouched behavior.
        {
            int probeDelta = 0;
            std::FILE* f = std::fopen((Platform::FilesDir() + "/display_delta.txt").c_str(), "rb");
            if (f) {
                char buf[32] = {};
                const size_t got = std::fread(buf, 1, sizeof(buf) - 1, f);
                std::fclose(f);
                char* end = nullptr;
                const long parsed = std::strtol(buf, &end, 10);
                if (got && end != buf) probeDelta = int(std::clamp(parsed, -3L, 0L));
            }
            if (probeDelta != m_displayDeltaFile) {
                m_displayDeltaFile = probeDelta;
                RC_LOGI("DisplayTime probe: delta=%d frames", probeDelta);
            }
        }
        // ROUND20 L17: the diagnostics switch panel - four files/ switches polled once a
        // second (no per-frame IO), logged only on change, effects applied here. The
        // foveation/scale overrides ride the next settings push; the forced refresh rate
        // is re-asserted every second because KeepRefreshRate re-asks for the setting's
        // own rate every two.
        {
            auto readSwitch = [](const char* name) -> long {
                std::FILE* f = std::fopen((Platform::FilesDir() + ("/" + std::string(name))).c_str(), "rb");
                if (!f) return -1;  // missing file: caller decides the default
                char buf[32] = {};
                const size_t got = std::fread(buf, 1, sizeof(buf) - 1, f);
                std::fclose(f);
                char* end = nullptr;
                const long parsed = std::strtol(buf, &end, 10);
                return (got && end != buf) ? parsed : -1;
            };
            {
                const long parsed = readSwitch("force_encode_scale.txt");
                const int v = (parsed >= 50 && parsed <= 100) ? int(parsed) : 0;
                if (v != m_forceEncodeScale) {
                    m_forceEncodeScale = v;
                    RC_LOGI("Debug force_encode_scale: %d (0 off, else the encode scale percent)", v);
                    m_client.SendControl({{"type", rc::msg::kUpdateSettings}, {"settings", StreamSettingsForPc()}});
                }
            }
            {
                const long parsed = readSwitch("force_foveation.txt");
                const int v = (parsed == 0 || parsed == 1) ? int(parsed) : 2;
                if (v != m_forceFoveation) {
                    m_forceFoveation = v;
                    RC_LOGI("Debug force_foveation: %d (0 off, 1 on, 2 follow settings)", v);
                    m_client.SendControl({{"type", rc::msg::kUpdateSettings}, {"settings", StreamSettingsForPc()}});
                }
            }
            {
                const long parsed = readSwitch("force_fovea_debug.txt");
                const int v = (parsed == 0 || parsed == 1) ? int(parsed) : 2;
                if (v != m_forceFoveaDebug) {
                    m_forceFoveaDebug = v;
                    RC_LOGI("Debug force_fovea_debug: %d (0 off, 1 on, 2 follow settings)", v);
                }
            }
            {
                const long parsed = readSwitch("force_refresh.txt");
                const int v = (parsed == 72 || parsed == 90) ? int(parsed) : 0;
                if (v != m_forceRefresh) {
                    m_forceRefresh = v;
                    RC_LOGI("Debug force_refresh: %d (0 off, else forced Hz)", v);
                    m_xr.SetRefreshRate(v ? float(v) : m_settings.refreshRate);
                } else if (v) {
                    m_xr.SetRefreshRate(float(v));  // keep winning over KeepRefreshRate
                }
            }
            {
                const long parsed = readSwitch("decode_stats.txt");
                const bool v = parsed == 1;
                if (v != m_decodeQuantiles) {
                    m_decodeQuantiles = v;
                    m_client.SetDecodeQuantiles(v);
                    RC_LOGI("Debug decode_stats: %d (decode p50/p95 in the diag line)", v ? 1 : 0);
                }
            }
        }
        const bool vr = m_mode == Mode::Vr || m_mode == Mode::LaunchingVr;
        m_stats.fps = m_genStats.source / secs / std::max<size_t>(1, vr ? 1 : m_desktopStreams.size());
        m_stats.generatedFps = m_genStats.GeneratedFps(secs);
        m_stats.generatedRejectedFps = m_genStats.RejectedFps(secs);
        // VD-OBSERVE (TASK 2026-11-20 §2.1): the pace policy, ticked at 1 Hz here rather than in
        // GenerateFrame(): that one only runs with frame generation on, which is off by default,
        // so its log would never be seen - exactly the state we want to judge first.
        // Observation only: nothing below changes drawing, decoding or requests.
        // recvFps (m_stats.fps) is the count of decoded frames newly latched per second -
        // generated frames do NOT count in it (they go to m_generatedFrames, see :2023/:2157).
        if (vr && m_xr.RefreshRate() > 0) {
            rc::LinkPolicyInput policyIn;
            policyIn.targetFps = m_xr.RefreshRate();
            policyIn.gameFps = m_serverStats.contains("vr") ? float(m_serverStats["vr"].value("gameFps", 0.0)) : 0.f;
            policyIn.recvFps = float(m_stats.fps);
            policyIn.aer = false;  // reserved: no AER source on this build
            const rc::PaceMode pace = rc::UpdateLinkPolicy(m_linkPolicy, policyIn, now);
            if (++m_linkPolicyTicks % 5 == 0) {
                double encFps = 0, dropped = 0;
                if (m_serverStats.contains("vr")) {
                    encFps = m_serverStats["vr"].value("encFps", 0.0);
                    dropped = m_serverStats["vr"].value("droppedFps", 0.0);
                }
                RC_LOGI("Link policy: %s (%s), game %.0f/%.1f fps (raw/smoothed), shown %.1f, target %.0f Hz, "
                        "enc %.0f fps, dropped %.0f/s",
                        rc::PaceModeName(pace), rc::PaceReasonName(m_linkPolicy.lastReason),
                        policyIn.gameFps, m_linkPolicy.smoothedGameFps, m_stats.fps, policyIn.targetFps,
                        encFps, dropped);
            }
        }
        m_stats.latencyMs = m_latencyCount ? m_latencySum / m_latencyCount : 0;
        m_genStats.Reset();
        m_latencySum = 0;
        m_latencyCount = 0;
        json stages = json::object();
        for (int i = 0; i < kStageCount; i++) {
            m_stats.stageMs[i] = m_stageFrames[i] ? m_stageSum[i] / m_stageFrames[i] : -1;
            if (m_stats.stageMs[i] >= 0) stages[kLatencyStages[i].key] = std::round(m_stats.stageMs[i] * 10) / 10;
            m_stageSum[i] = 0;
            m_stageFrames[i] = 0;
        }
        // ROUND18 L8-TELEM: the window's average submit->lit, -1 when no frame was measured.
        const double submitLitMs = m_submitLitFrames ? m_submitLitSumMs / m_submitLitFrames : -1.0;
        m_submitLitSumMs = 0;
        m_submitLitFrames = 0;
        m_stats.rttMs = m_client.Stats().rttMs;
        double decode = 0;
        for (auto& [id, dec] : decoders) {
            if (!dec) continue;
            const double d = dec->AverageDecodeMs();
            const double slowest = dec->TakeMaxDecodeMs();
            if (id == rc::kVrStreamId) m_flowDecodeMaxMs = std::max(m_flowDecodeMaxMs, slowest);
            m_client.ReportDecode(id, d, m_stats.latencyMs);
            if ((id == rc::kVrStreamId) == vr) decode = std::max(decode, d);
        }
        m_stats.decodeMs = decode;
        // ROUND18 L5: stale decoded frames dropped in this 1 s window (output-side
        // protection, see VideoDecoder::HandOver / Latch). Counter is cumulative,
        // so report the delta; a decoder swap can only lower it, guard against wrap.
        {
            uint32_t skippedNow = 0;
            for (auto& [id, dec] : decoders)
                if (dec) skippedNow += dec->SkippedFrames();
            m_statsDropStale = skippedNow >= m_statsSkippedPrev ? skippedNow - m_statsSkippedPrev : 0;
            m_statsSkippedPrev = skippedNow;
        }
        if (vr && m_serverStats.contains("vr")) {
            m_stats.mbps = m_serverStats["vr"].value("mbps", 0.0);
        } else if (m_serverStats.contains("desktop")) {
            double mbps = 0;
            for (const auto& d : m_serverStats["desktop"]) mbps += d.value("mbps", 0.0);
            m_stats.mbps = mbps;
        }
        const char* names[] = {"H.264", "HEVC", "AV1"};
        m_stats.codec = vr ? names[int(m_vrCodec) % 3]
                           : (m_desktopStreams.empty() ? "" : names[int(m_desktopStreams.begin()->second.codec) % 3]);
        m_stats.width = vr ? m_vrEyeWidth : (m_desktopStreams.empty() ? 0 : m_desktopStreams.begin()->second.width);
        m_stats.height = vr ? m_vrEyeHeight : (m_desktopStreams.empty() ? 0 : m_desktopStreams.begin()->second.height);

        // Diagnostics only: the headset's display side on a one-second clock, so the
        // "rc vr rx 1s" receive line and this line can be read against the PC's
        // "rc diag 1s" at the same second. All values are computed above for the overlay.
        if (vr) {
            char seqPart[32];
            if (m_seqWindowMin) snprintf(seqPart, sizeof(seqPart), ", seq %u-%u", m_seqWindowMin, m_seqWindowMax);
            else snprintf(seqPart, sizeof(seqPart), ", seq -");
            // TASK-DEC-PERCENTILE-2026-11-20 §1.3.3: the quantiles come from the app's own
            // per-frame sampler now, so the field carries data without the debug switch.
            // Format string untouched.
            RC_LOGI("rc vr diag 1s: %d shown, %d repeated, %d late of %d displays (flow window), %d generated, "
                    "%u stale dropped; "
                    "latency %.1f ms, pipeline %.1f ms, decode %.1f ms, rtt %.1f ms, stages %s, submit->lit %.1f ms, "
                    "dt-probe %d%s, fes %d, ffov %d, fref %d, dec p50/p95 %.1f/%.1f",
                    m_diagFlowShown,
                    std::max(0, m_diagFlowDisplays - m_diagFlowShown - int(m_generatedFramesWindow)),
                    m_diagFlowLate, m_diagFlowDisplays, m_generatedFramesWindow,
                    m_statsDropStale,
                    m_stats.latencyMs, m_pipelineLatencyMs, m_stats.decodeMs, m_stats.rttMs,
                    stages.empty() ? "NA" : stages.dump().c_str(), submitLitMs, m_displayDeltaApplied, seqPart,
                    m_forceEncodeScale, m_forceFoveation, m_forceRefresh, m_decSamples.Percentile(50.f),
                    m_decSamples.Percentile(95.f));
            m_diagFlowDisplays = m_diagFlowShown = m_diagFlowLate = 0;
            m_generatedFramesWindow = 0;
            // TASK-DEC-PERCENTILE-2026-11-20 §1.3.4: the APK-forensics grep anchor and device
            // cross-check line - the same decode_stats switch as the panel above gates it, so
            // the default log volume is unchanged.
            if (m_decodeQuantiles)
                RC_LOGI("decode p50=%.1f p95=%.1f (n=%d)", m_decSamples.Percentile(50.f),
                        m_decSamples.Percentile(95.f), m_decSamples.Count());
            m_seqWindowMin = 0;
            m_seqWindowMax = 0;
            // TASK-A80 M2: the runtime's measured view fov (XrContext::BeginFrame ->
            // frame.eyes[e].fov) next to the fovTan the PC told us to declare
            // (m_vrMeta.fovTan, what BuildLayers turns into m_vrViews[e].fov), index by
            // index, on the existing 1 s diag beat - no new timer. Readings only: neither
            // array is touched here, and the layer is built elsewhere.
            // Both sides print as POSITIVE magnitudes in vr_bridge.h's order
            // (left, right, up, down), so a match is a plain number-for-number read.
            // It settles the card's Q1: if the runtime's angleUp lines up with meta[3]
            // rather than meta[2], the contract is inverted and the evidence is on the
            // device instead of in a reading of the code. Q2 rides along: the printed
            // degrees show whether either side is stuck on whole numbers.
            {
                static const char* const kIdxName[4] = {"left", "right", "up", "down"};
                const double kRad = 180.0 / 3.14159265358979323846;
                char cmp[1000];
                int used = snprintf(cmp, sizeof(cmp), "rc fov cmp 1s:");
                for (int eye = 0; eye < 2; eye++) {
                    if (used <= 0 || used >= int(sizeof(cmp))) break;
                    const float* t = m_vrMeta.fovTan[eye];
                    const XrFovf& f = frame.eyes[eye].fov;
                    // XrFovf carries the sign the projection needs; the metadata is
                    // unsigned tangents, so compare magnitudes.
                    const double rt[4] = {std::fabs(double(f.angleLeft)), std::fabs(double(f.angleRight)),
                                          std::fabs(double(f.angleUp)), std::fabs(double(f.angleDown))};
                    const double md[4] = {std::atan(double(t[0])) * kRad, std::atan(double(t[1])) * kRad,
                                          std::atan(double(t[2])) * kRad, std::atan(double(t[3])) * kRad};
                    int worst = 0;
                    for (int k = 1; k < 4; k++)
                        if (std::fabs(rt[k] - md[k]) > std::fabs(rt[worst] - md[worst])) worst = k;
                    const int n = snprintf(
                        cmp + used, sizeof(cmp) - used,
                        " e%d rtDeg %.9g %.9g %.9g %.9g (rad %.9g %.9g %.9g %.9g) metaTan %.9g %.9g %.9g %.9g"
                        " metaDeg %.9g %.9g %.9g %.9g maxdev %s %.9g deg;",
                        eye, rt[0], rt[1], rt[2], rt[3], double(f.angleLeft), double(f.angleRight),
                        double(f.angleUp), double(f.angleDown), t[0], t[1], t[2], t[3], md[0], md[1], md[2], md[3],
                        kIdxName[worst], rt[worst] - md[worst]);
                    if (n > 0) used += n;
                }
                RC_LOGI("%s", cmp);
            }
        }
        m_client.SendControl({{"type", rc::msg::kStats},
                              {"stats", {{"fps", m_stats.fps}, {"latencyMs", m_stats.latencyMs}, {"decodeMs", m_stats.decodeMs}, {"latencyStages", stages},
                                         {"debugFes", m_forceEncodeScale}, {"debugFfov", m_forceFoveation}, {"debugFref", m_forceRefresh},
                                         {"debugDec", m_decodeQuantiles ? 1 : 0}}}});
    }
}

// The PC paces SteamVR with a vsync of its own, at the headset's rate but not in step with
// its display: frames finished at any point of the headset's frame and waited up to a whole
// one to be shown. Twice a second, tell the PC how far to move its vsync so they finish just
// before the late latch, with a margin wide enough for how much they vary.
void App::SteerVrTiming(const FrameState& frame) {
    const int64_t now = rc::NowNs();
    if (m_mode != Mode::Vr || frame.period <= 0) {
        m_phaseErrors.clear();
        m_arrivalSpread.clear();
        m_queueWanted = 1;
        m_phaseSentNs = now;
        return;
    }
    // While the PC moves its frames later (CatchUpVr), there is nothing to steer by.
    if (now < m_slipUntilNs) {
        m_phaseErrors.clear();
        m_phaseSentNs = now;
        return;
    }
    if (now - m_phaseSentNs < 500'000'000LL) return;
    m_phaseSentNs = now;
    if (m_phaseErrors.size() < 10) {
        m_phaseErrors.clear();
        return;
    }
    // Averaged as angles on the period's circle, or early and late arrivals near the wrap
    // point would cancel out.
    const double period = double(frame.period), k = 6.283185307 / period;
    double sinSum = 0, cosSum = 0;
    for (double e : m_phaseErrors) {
        sinSum += std::sin(e * k);
        cosSum += std::cos(e * k);
    }
    const double mean = std::atan2(sinSum, cosSum) / k;
    double spread = 0;
    for (double e : m_phaseErrors) {
        double d = std::fmod(e - mean + 1.5 * period, period) - period / 2;
        spread += d * d;
    }
    spread = std::sqrt(spread / double(m_phaseErrors.size()));
    // How much later than the average each frame came, kept for five seconds. Frames run late
    // far more than early (everything on the way can only add time), so one that seems most
    // of a period early counts as the rest of it late: -20% to +80% of a period.
    for (double e : m_phaseErrors) {
        const double late = std::fmod(e - mean + 1.2 * period, period) - 0.2 * period;
        m_arrivalSpread.push_back({now, float(late)});
    }
    while (!m_arrivalSpread.empty() && now - m_arrivalSpread.front().first > 5'000'000'000LL) m_arrivalSpread.pop_front();
    m_phaseErrors.clear();
    // The margin that all but one of those frames would have made. If it stays well under a
    // period, it replaces the picture kept waiting: frames then reach the display that much
    // after decoding, not that plus a whole frame. Otherwise one picture waiting is what covers
    // the late ones, with the narrow margin it always had. A PC that can't let waiting
    // pictures run out keeps the timing it had.
    float latest = -1e30f, second = -1e30f;
    for (const auto& [when, late] : m_arrivalSpread) {
        if (late > latest) {
            second = latest;
            latest = late;
        } else if (late > second) {
            second = late;
        }
    }
    const bool known = m_arrivalSpread.size() >= 100 && now - m_arrivalSpread.front().first > 2'000'000'000LL;
    const double needed = double(second) + 0.5e6;
    const double widest = 0.8 * period;
    // Once it fits it may grow to the widest; to start fitting it has to be clearly narrower.
    const bool pcSlips = PcSlips();
    const int wanted = rc::SelectVrQueueWanted(known, pcSlips, m_debugNoWait, frame.period, needed,
                                               m_queueWanted, now, m_queueKeptUntilNs);
    if (wanted != m_queueWanted || now - m_queuePolicyLogNs >= 5'000'000'000LL) {
        RC_LOGI("VR queue policy: wanted %d, known %d, pcSlips %d, nowait %d, needed %.1f ms, limit %.1f ms, "
                "kept %.0f ms",
                wanted, int(known), int(pcSlips), int(m_debugNoWait), needed / 1e6,
                rc::VrQueueLimitNs(frame.period, m_queueWanted) / 1e6,
                double(std::max<int64_t>(0, m_queueKeptUntilNs - now)) / 1e6);
        m_queuePolicyLogNs = now;
    }
    m_queueWanted = wanted;
    if (m_queueWanted == 0) {
        // Wider at once, narrower slowly: a margin too narrow costs a repeated picture, one
        // too wide a millisecond.
        const double margin = std::clamp(needed, 2e6, widest);
        m_phaseMarginNs += (margin - m_phaseMarginNs) * (margin > m_phaseMarginNs ? 0.5 : 0.15);
    } else {
        const double margin = std::clamp(2.5 * spread, 2e6, 0.4 * period);
        m_phaseMarginNs = m_phaseMarginNs * 0.7 + margin * 0.3;
    }
    m_client.SendControl({{"type", rc::msg::kVrPhase}, {"shiftNs", int64_t(-mean)}});
}

// A newer picture waiting at display after display: the stream runs a frame behind. With a
// margin wide enough for how arrivals vary (m_queueWanted 0) that frame is time lost, and with
// two waiting one of them always is. The decoder gets rid of them by skipping (VideoDecoder::
// Latch), which shows, and the next late frame then repeats a picture: about once a second on
// Wi-Fi. A PC that can is asked instead to make its frames a little later for a moment, each a
// few percent after the one before: the display takes the waiting ones faster than new ones
// come, every frame is shown, and the timing ends up where it was, one display earlier. The
// numbers here were chosen on a model of this (frames arriving with Wi-Fi's kind of variation):
// about the latency the skipping gave, with a fifth of the repeated pictures and no skipped ones.
void App::CatchUpVr(VideoDecoder& decoder, int waiting, const FrameState& frame) {
    const int64_t now = rc::NowNs();
    m_flowWaiting[std::clamp(waiting, 0, 2)]++;
    if (!PcSlips() || frame.period <= 0) return;  // the decoder's own way
    if (now < m_slipUntilNs) {
        m_slipLeast = std::min(m_slipLeast, waiting);
        decoder.HoldCatchUp(m_slipUntilNs + 100'000'000LL);
        return;
    }
    if (m_slipUntilNs) {
        // It has had its time. Never fewer waiting than before: moving SteamVR's vsync didn't
        // move this game's frames, so the decoder's own way it is, for a while.
        m_slipUntilNs = 0;
        m_behindSinceNs = 0;
        if (std::min(m_slipLeast, waiting) >= m_slipFrom) {
            m_slipRetryNs = now + 60'000'000'000LL;
            RC_LOGW("VR frames: still %d waiting after the PC was asked to let them run out; skipping them instead "
                    "for the next minute", waiting);
        }
    }
    if (waiting == 0) {
        m_behindSinceNs = 0;
        return;
    }
    if (now < m_slipRetryNs) return;
    // Either the one waiting is wanted, or the PC is about to be asked: no skipping.
    decoder.HoldCatchUp(now + 1'000'000'000LL);
    // Looked at 150 ms at a time: more waiting than wanted at four displays in five. Not at
    // every one, or a single late frame would start the count again, as it did for the decoder.
    if (!m_behindSinceNs) {
        m_behindSinceNs = now;
        m_behindLeast = waiting;
        m_behindLatches = m_behindOver = 0;
    }
    m_behindLeast = std::min(m_behindLeast, waiting);
    m_behindLatches++;
    if (waiting > m_queueWanted) m_behindOver++;
    if (now - m_behindSinceNs < 150'000'000LL) return;
    const int over = m_behindOver, latches = m_behindLatches, least = m_behindLeast;
    m_behindSinceNs = 0;  // the next 150 ms count anew
    if (latches < 5 || over * 5 < latches * 4) return;
    // With no picture meant to wait, each time one does again is a frame that came later than
    // the margin, and a repeated picture. More than five times in 15 s is more often than the
    // margin was sized for (a decode that takes two frames' time looks early in the spread, not
    // late): one stays waiting for half a minute, which covers those.
    while (!m_slips.empty() && now - m_slips.front() > 15'000'000'000LL) m_slips.pop_front();
    if (m_queueWanted == 0) {
        if (m_slips.size() >= 5) {
            m_queueKeptUntilNs = now + 30'000'000'000LL;
            m_queueWanted = 1;
            m_slips.clear();
            RC_LOGI("VR frames: a picture waited again six times in 15 s after running out; one is kept waiting for 30 s");
            return;
        }
        m_slips.push_back(now);
    }
    const int excess = std::max(1, least - m_queueWanted);
    const int64_t slip = int64_t(excess) * frame.period;
    // The time the PC takes for it, and for the frames made since to get here.
    const int64_t takes = int64_t(double(slip) / rc::kVrPhaseSlipRate) + 300'000'000LL;
    m_client.SendControl({{"type", rc::msg::kVrPhase}, {"slipNs", slip}});
    m_slipUntilNs = now + takes;
    m_slipFrom = std::max(least, m_queueWanted + 1);
    m_slipLeast = m_slipFrom;
    m_flowSlips++;
    decoder.HoldCatchUp(m_slipUntilNs + 100'000'000LL);
    RC_LOGI("VR frames: %d waiting at %d of %d displays (%d wanted); the PC makes its frames %.1f ms later over the "
            "next %.0f ms",
            m_slipFrom, over, latches, m_queueWanted, double(slip) / 1e6, double(takes) / 1e6);
}

void App::AddStages(const double (&ms)[kStageCount]) {
    // TASK-LATENCY-METER-FIX-2026-11-22 §1.2: no sample is dropped any more. The old
    // filter threw away whole frames' stages (below -2 or at 1000 and up), so a clock
    // offset error pushing the biggest stage (the game) below -2 removed it from the
    // sum and the stages fell visibly short of the total. Every stage now counts every
    // frame: negatives as zero, extremes capped at a second (see AddStageSamples).
    AddStageSamples(m_stageSum, m_stageFrames, ms);
}

void App::RenderEyes(const FrameState& frame, float dt) {
    const Environment env = EnvironmentFromKey(m_settings.environment);
    const bool inVr = m_mode == Mode::Vr;
    m_sky.Prepare(inVr ? Environment::Passthrough : env, m_time);
    m_windows.SortForDrawing(frame.head.position);
    for (int eye = 0; eye < 2; eye++) {
        if (!m_xr.BindEye(eye)) continue;
        glClearColor(0, 0, 0, 0);
        glClearDepthf(1.f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        const EyeView& v = frame.eyes[eye];
        const Mat4 viewProj = v.projection * v.view;

        // Environment (or hyperspace). In VR the game stream is underneath; we only
        // draw the fading tunnel on top of it.
        const Environment drawEnv = inVr ? Environment::Passthrough : env;
        m_sky.Draw(v.projection, v.pose.orientation, drawEnv, m_time, m_settings.skyBrightness, m_warp, m_warpPhase,
                   m_tunnelOrientation);

        m_scene.SetViewProjection(viewProj, v.pose.position);
        m_windows.Draw(m_scene);

        // Lasers.
        for (int h = 0; h < 2; h++) {
            const auto& r = m_rays[h];
            if (!r.valid) continue;
            const WindowHit& hit = m_hits[h];
            const float len = hit.window ? hit.distance : 2.5f;
            const Vec3 end = r.origin + r.dir * len;
            const float press = r.triggerDown ? 1.f : 0.f;
            const Color start = Color(1.f, 1.f, 1.f, 0.0f);
            const Color tip = Mix(Color(1.f, 1.f, 1.f, hit.window ? 0.6f : 0.3f), Color(0.42f, 0.7f, 1.f, 0.9f), press);
            m_scene.Beam(r.origin + r.dir * 0.03f, end, 0.0035f, start, tip);
            m_scene.ControllerOrb(m_input.Hand(h).aim, Color(1, 1, 1, 0.65f));
            if (hit.window) {
                const bool onMonitor = hit.window->kind == WindowKind::Video && hit.zone == HitZone::Content;
                // The PC cursor is part of the video; keep our dot subtle there.
                m_scene.Dot(hit.point, hit.normal, onMonitor ? 0.004f : 0.007f,
                            Color(1, 1, 1, onMonitor ? 0.35f : 0.95f));
            }
        }
    }
    (void)dt;
}

void App::UpdateStatsLayer(Window* stats, bool active, float dt) {
    if (stats) stats->canvasLayer = active && m_statsLayer.handle;
    m_statsLayerActive = active && m_statsLayer.handle && m_statsDrawnNs;
    if (!active) {
        m_statsDrawnNs = 0;  // the next time, draw at once
        return;
    }
    const int w = int(stats->canvasW * stats->rtScale), h = int(stats->canvasH * stats->rtScale);
    if (m_statsLayer.handle && (m_statsLayer.width != w || m_statsLayer.height != h)) {
        if (!m_xr.DestroySwapchain(m_statsLayer)) {
            stats->canvasLayer = false;
            m_statsLayerActive = false;
            return;
        }
    }
    if (!m_statsLayer.handle && !m_xr.CreateSwapchain(w, h, GL_SRGB8_ALPHA8, 1, false, m_statsLayer, 1, "stats")) return;
    stats->canvasLayer = true;
    // Its numbers change once a second.
    const int64_t now = rc::NowNs();
    if (m_statsDrawnNs && now - m_statsDrawnNs < 250'000'000LL) return;
    const GLuint fbo = m_xr.AcquireImage(m_statsLayer);
    if (!fbo) {
        stats->canvasLayer = false;
        m_statsLayerActive = false;
        return;
    }
    m_windows.RenderCanvasTo(*stats, fbo, w, h, dt, m_time);
    if (!m_xr.ReleaseImage(m_statsLayer)) {
        stats->canvasLayer = false;
        m_statsLayerActive = false;
        return;
    }
    m_statsDrawnNs = now;
    m_statsLayerActive = true;
}

void App::UpdateUiLayers() {
    // The headset composes up to 16 layers; the game, monitors, the overlay, passthrough and
    // the eye images come first.
    constexpr int kMaxUiLayers = 6;
    int used = 0;
    for (const auto& wp : m_windows.All()) {
        Window& w = *wp;
        if (w.kind != WindowKind::Ui) continue;
        // While it fades in or out, it's drawn into the eye images as before.
        // Layered from the first frame it is visible: drawn into the eye images while it faded,
        // a window was soft until it became a layer, then popped sharp. The fade is the layer's.
        const bool layer = m_settings.sharpLayers && !w.canvasLayer && !w.removed && w.visible && w.appear > 0.01f &&
                           w.curveRadius == 0 && w.rt.fbo && used < kMaxUiLayers;
        w.layerBacked = false;
        if (!layer) {
            w.layerCurrent = false;
            continue;
        }
        XrSwapchainGl& sc = m_uiLayers[w.id];
        const int width = w.rt.color.width, height = w.rt.color.height;
        if (sc.handle && (sc.width != width || sc.height != height)) {
            if (!m_xr.DestroySwapchain(sc)) continue;
        }
        if (!sc.handle) {
            w.layerCurrent = false;
            if (!m_xr.CreateSwapchain(width, height, GL_SRGB8_ALPHA8, 1, false, sc, 1, w.id.c_str())) continue;
        }
        // A failed release leaves the runtime image acquired. Retry that ownership hand-back
        // before drawing or reusing this layer; never submit it while it is still acquired.
        if (sc.imageAcquired && !m_xr.ReleaseImage(sc)) {
            w.layerCurrent = false;
            continue;
        }
        // A layer shows the image last released into it: an unchanged canvas needn't be copied
        // again every frame.
        if (w.layerCurrent && !w.canvasRedrawn && !sc.imageAcquired) {
            w.layerBacked = true;
            used++;
            continue;
        }
        // Pixel for pixel: the canvas was just drawn at this size. While it fades in or out, the
        // canvas goes in at that opacity instead, so the window is sharp from its first frame.
        const GLuint fbo = m_xr.AcquireImage(sc);
        if (!fbo) {
            w.layerCurrent = false;
            continue;
        }
        if (w.appear < 0.995f) {
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            m_scene.Blit2DAlpha(w.rt.color.id, width, height, w.appear);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        } else {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, w.rt.fbo);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
            glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }
        if (!m_xr.ReleaseImage(sc)) {
            w.layerCurrent = false;
            continue;
        }
        w.layerBacked = true;
        w.layerCurrent = w.appear >= 0.995f;
        used++;
    }
    // Windows that are gone or hidden give their images back.
    for (auto it = m_uiLayers.begin(); it != m_uiLayers.end();) {
        const Window* w = m_windows.Find(it->first);
        if (!w || w->removed || (!w->visible && w->appear < 0.01f)) {
            if (!m_xr.DestroySwapchain(it->second)) {
                ++it;
                continue;
            }
            it = m_uiLayers.erase(it);
        } else {
            ++it;
        }
    }
}

void App::BuildLayers(const FrameState& frame) {
    m_underlays.clear();
    m_overlays.clear();
    m_quadLayers.clear();
    m_cylinderLayers.clear();
    m_layerSettings.clear();
    // Room for every layer: m_underlays points into these.
    m_quadLayers.reserve(32);
    m_cylinderLayers.reserve(32);
    m_layerSettings.reserve(32);

    // VR stream: the PC frame with the exact pose it was rendered for; the Quest
    // compositor reprojects it to the current head pose.
    if (m_mode == Mode::Vr && m_vrHasFrame && m_vrSwapchain.handle && !m_vrSwapchain.imageAcquired) {
        const Pose head{{m_vrMeta.renderPose.orientation[0], m_vrMeta.renderPose.orientation[1],
                         m_vrMeta.renderPose.orientation[2], m_vrMeta.renderPose.orientation[3]},
                        {m_vrMeta.renderPose.position[0], m_vrMeta.renderPose.position[1], m_vrMeta.renderPose.position[2]}};
        const float ipd = m_vrMeta.ipd > 0.04f ? m_vrMeta.ipd : 0.063f;
        for (int eye = 0; eye < 2; eye++) {
            XrCompositionLayerProjectionView& v = m_vrViews[eye];
            v = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            const Pose eyePose = head * Pose{{}, {eye == 0 ? -ipd / 2 : ipd / 2, 0, 0}};
            v.pose = ToXr(eyePose);
            const float* t = m_vrMeta.fovTan[eye];
            v.fov = {-std::atan(t[0]), std::atan(t[1]), std::atan(t[2]), -std::atan(t[3])};
            v.subImage.swapchain = m_vrSwapchain.handle;
            v.subImage.imageRect.offset = {eye * m_vrSwapchain.width / 2, 0};
            v.subImage.imageRect.extent = {m_vrSwapchain.width / 2, m_vrSwapchain.height};
        }
        m_vrLayer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        m_vrLayer.space = m_xr.AppSpace();
        m_vrLayer.viewCount = 2;
        m_vrLayer.views = m_vrViews;
        // Sharpening shows up compression artifacts, so only with bits to spare.
        if (m_xr.LayerSettings() && m_stats.mbps >= 40 && m_settings.vrSharpen > 0.001f) {
            XrCompositionLayerSettingsFB s{XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB};
            s.layerFlags = XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SHARPENING_BIT_FB;
            m_layerSettings.push_back(s);
            m_vrLayer.next = &m_layerSettings.back();
        }
        m_underlays.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&m_vrLayer));
    }

    if (m_statsLayerActive) {
        if (Window* w = m_windows.Find("stats")) {
            const void* next = nullptr;
            if (m_xr.LayerSettings()) {
                XrCompositionLayerSettingsFB s{XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB};
                s.layerFlags = XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SUPER_SAMPLING_BIT_FB;
                m_layerSettings.push_back(s);
                next = &m_layerSettings.back();
            }
            XrCompositionLayerQuad q{XR_TYPE_COMPOSITION_LAYER_QUAD};
            q.next = next;
            // The canvas is premultiplied, and its rounded corners are see-through.
            q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            q.space = m_xr.AppSpace();
            q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            q.subImage.swapchain = m_statsLayer.handle;
            q.subImage.imageRect.extent = {m_statsLayer.width, m_statsLayer.height};
            q.pose = ToXr(w->pose);
            q.size = {w->widthM, w->HeightM()};
            m_quadLayers.push_back(q);
            m_overlays.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&m_quadLayers.back()));
        }
    }

    // Monitors and UI windows as compositor layers (sharp text): underneath the projection
    // layer, which punches matching holes. Back to front like the windows, so where two overlap
    // the nearer one is on top.
    for (Window* w : m_windows.DrawOrder()) {
        if (w->kind == WindowKind::Ui) {
            const auto it = m_uiLayers.find(w->id);
            if (!w->layerBacked || it == m_uiLayers.end() || !it->second.handle || it->second.imageAcquired) continue;
            const void* next = nullptr;
            if (m_xr.LayerSettings()) {
                XrCompositionLayerSettingsFB s{XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB};
                s.layerFlags = XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SUPER_SAMPLING_BIT_FB;
                m_layerSettings.push_back(s);
                next = &m_layerSettings.back();
            }
            XrCompositionLayerQuad q{XR_TYPE_COMPOSITION_LAYER_QUAD};
            q.next = next;
            // Premultiplied, with see-through rounded corners (as the overlay above).
            q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            q.space = m_xr.AppSpace();
            q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            q.subImage.swapchain = it->second.handle;
            q.subImage.imageRect.extent = {it->second.width, it->second.height};
            q.pose = ToXr(w->pose);
            q.size = {w->widthM, w->HeightM()};
            m_quadLayers.push_back(q);
            m_underlays.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&m_quadLayers.back()));
            continue;
        }
        if (m_mode == Mode::Desktop) {
            if (w->id.rfind("monitor-", 0) != 0) continue;
            const auto it = m_monitorLayers.find(uint8_t(std::atoi(w->id.c_str() + 8)));
            if (it == m_monitorLayers.end()) continue;
            const uint8_t id = it->first;
            XrSwapchainGl& sc = it->second;
            if (!sc.handle || sc.imageAcquired || !w->layerBacked || !w->visible || w->appear < 0.98f || !m_monitorFresh[id]) continue;
            const void* next = nullptr;
            if (m_xr.LayerSettings()) {
                XrCompositionLayerSettingsFB s{XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB};
                s.layerFlags = XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SUPER_SAMPLING_BIT_FB |
                               XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SHARPENING_BIT_FB;
                m_layerSettings.push_back(s);
                next = &m_layerSettings.back();
            }
            if (w->curveRadius > 0) {
                XrCompositionLayerCylinderKHR c{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
                c.next = next;
                c.space = m_xr.AppSpace();
                c.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                c.subImage.swapchain = sc.handle;
                c.subImage.imageRect.extent = {sc.width, sc.height};
                // Cylinder centre sits `radius` in front of the window surface.
                c.pose = ToXr({w->pose.orientation, w->pose.Transform({0, 0, w->curveRadius})});
                c.radius = w->curveRadius;
                c.centralAngle = w->widthM / w->curveRadius;
                c.aspectRatio = w->widthM / w->HeightM();
                m_cylinderLayers.push_back(c);
                m_underlays.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&m_cylinderLayers.back()));
            } else {
                XrCompositionLayerQuad q{XR_TYPE_COMPOSITION_LAYER_QUAD};
                q.next = next;
                q.space = m_xr.AppSpace();
                q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                q.subImage.swapchain = sc.handle;
                q.subImage.imageRect.extent = {sc.width, sc.height};
                q.pose = ToXr(w->pose);
                q.size = {w->widthM, w->HeightM()};
                m_quadLayers.push_back(q);
                m_underlays.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&m_quadLayers.back()));
            }
        }
    }
    (void)frame;
}

void App::CreateWindows() {
    screens::Build(*this, m_windows);
    ApplyAccessibility();
    if (!m_settings.manualPc.empty()) SetManualPc(m_settings.manualPc);
    rc::NetAddress lastAddress;
    if (rc::NetAddress::Parse(m_settings.lastPcAddress, rc::kDiscoveryPort, lastAddress)) m_discovery.AddProbeTarget(lastAddress);
}

std::string App::CompatibilityDiagnostic() const {
    const char* loader = xrloader::Name();
    const char* family = m_headset ? m_headset->Family() : "?";
    return rc::StrFormat("%s | %s | %s | Tier:%s",
                         family, loader ? loader : "OpenXR",
                         m_xr.IsSrgbSwapchain() ? "sRGB" : "RGBA8",
                         m_gpuTier.empty() ? "Mid" : m_gpuTier.c_str());
}

}  // namespace rv

// ---------------------------------------------------------------------------

#ifdef __ANDROID__
void android_main(android_app* app) {
    rv::App rincast(app);
    rincast.Run();
}
#else
int main() {
    rv::App rincast(nullptr);
    rincast.Run();
    return 0;
}
#endif
