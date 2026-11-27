#include "app.h"
#include "vr/face_share.h"
#include "net/firewall.h"

#include <shellapi.h>
#include <timeapi.h>

#include <chrono>
#include <ctime>
#include <fstream>

#include "audio/audio.h"
#include "input/input_injector.h"
#include "rc/platform.h"
#include "rc/project.h"
#include "rc/protocol.h"
#include "session/client_session.h"
#include "settings.h"
#include "version.h"
#include "vr/steamvr.h"

#pragma comment(lib, "winmm.lib")

namespace rcs {

namespace {

std::mutex g_logFileMutex;
std::ofstream g_logFile;

void FileLogSink(rc::LogLevel level, const char* message) {
    static const char* names[] = {"DEBUG", "INFO ", "WARN ", "ERROR"};
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &t);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
    std::lock_guard lock(g_logFileMutex);
    if (g_logFile) g_logFile << ts << " " << names[int(level)] << " " << message << "\n" << std::flush;
    OutputDebugStringA(message);
    OutputDebugStringA("\n");
}

bool SetStartWithWindows(bool enable) {
    const wchar_t* key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    if (enable) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        return RegWriteString(HKEY_CURRENT_USER, key, L"RinCast Streamer", L"\"" + std::wstring(exe) + L"\" --minimized");
    }
    RegDeleteValue(HKEY_CURRENT_USER, key, L"RinCast Streamer");  // already gone is fine
    return true;
}

}  // namespace

App& App::Get() {
    static App app;
    return app;
}

std::string App::Version() const { return RINCAST_VERSION_STRING; }

bool App::Init() {
    std::error_code ec;
    std::filesystem::create_directories(DataDir() / L"logs", ec);
    const auto logPath = DataDir() / L"logs" / L"streamer.log";
    // Keep one previous log.
    std::filesystem::rename(logPath, DataDir() / L"logs" / L"streamer.previous.log", ec);
    g_logFile.open(logPath, std::ios::out | std::ios::trunc);
    rc::SetLogSink(FileLogSink);
    RC_LOGI("RinCast Streamer %s starting on %s", Version().c_str(), WindowsVersion().c_str());

    timeBeginPeriod(1);
    SettingsStore::Get().Load();
    const Settings settings = SettingsStore::Get().Snapshot();
    SetStartWithWindows(settings.startWithWindows);
    // Default devices a killed streamer left switched to the headset's.
    RunAsync([] { audio::RestoreDefaults(); });

    // GPU & encoder capabilities (the VR bridge device lives on the same adapter).
    if (!m_bridge.Start()) RC_LOGE("VR bridge failed to start");
    auto adapter = PreferredAdapter();
    m_gpu = DescribeAdapter(adapter.Get());
    if (m_bridge.Device()) m_caps = ProbeEncoderCaps(m_bridge.Device(), m_gpu.vendorId);
    RC_LOGI("GPU: %s, encoder %s (H.264 %d, HEVC %d, AV1 %d, 10-bit HEVC %d)", m_gpu.name.c_str(),
            m_caps.backend.c_str(), m_caps.h264, m_caps.hevc, m_caps.av1, m_caps.hevc10);

    m_bridge.SetHapticHandler([this](const rcvr::HapticEvent& e) {
        if (auto s = m_server.ActiveSession()) {
            rc::HapticsPayload p{};
            p.hand = uint8_t(e.hand);
            p.durationS = e.durationS;
            p.frequency = e.frequency;
            p.amplitude = e.amplitude;
            s->Connection()->Send(rc::PacketType::Haptics, &p, sizeof(p));
        }
    });

    m_server.SetApprovalCallback([this](const HelloInfo& hello) {
        json prompt{{"type", "approval"},
                    {"deviceId", hello.deviceId},
                    {"deviceName", hello.deviceName},
                    {"model", hello.model},
                    {"address", hello.address.IpString()}};
        {
            std::lock_guard lock(m_uiMutex);
            m_pendingApproval = prompt;
        }
        PushUi(prompt);
        if (ShowWindowRequest) ShowWindowRequest();
    });
    if (!m_server.Start()) {
        RC_LOGE("Could not open the stream socket");
        return false;
    }
    m_oscEye.SetCallback([this](const rc::GazePayload& g) {
        if (auto session = m_server.ActiveSession()) session->OnExternalGaze(g);
    });
    m_oscEye.Configure(settings.externalEyeTracking, static_cast<uint16_t>(settings.externalEyeOscPort),
                       settings.externalEyeInvertX, settings.externalEyeInvertY,
                       settings.externalEyeOffsetX, settings.externalEyeOffsetY,
                       settings.externalEyeGainX, settings.externalEyeGainY);
    m_cloud.Start(&m_server);
    steamvr::WatchForcedDriver();

    if (settings.autoRegisterSteamVrDriver && steamvr::SteamVrPath()) {
        RunAsync([] { steamvr::RegisterDriver(); });
    }
    return true;
}

void App::Shutdown() {
    m_oscEye.Stop();
    m_server.Stop();
    m_cloud.Stop();
    m_bridge.Stop();
    audio::RestoreDefaults();
    SetThreadExecutionState(ES_CONTINUOUS);
    timeEndPeriod(1);
    RC_LOGI("RinCast Streamer stopped");
}

void App::RunAsync(std::function<void()> fn) {
    std::thread([fn = std::move(fn)] {
        ScopedCoInit com;
        fn();
    }).detach();
}

void App::SetUiSink(UiSink sink) {
    std::lock_guard lock(m_uiMutex);
    m_uiSink = std::move(sink);
}

void App::PushUi(const json& message) {
    UiSink sink;
    {
        std::lock_guard lock(m_uiMutex);
        sink = m_uiSink;
    }
    if (sink) sink(message.dump());
}

void App::OnSessionStarted(ClientSession* session) {
    m_streaming = true;
    // A19 audit #13: the PC answered the approval prompt, but the wearer was still
    // showing "waiting for approval" with no idea the PC had clicked. The asking
    // device has no session of its own until now, so the answer goes out on the one
    // that just started. (A denial has no session at all - the headset already shows
    // the Reject's "declined" text on its next Hello.)
    bool approved = false;
    {
        std::lock_guard lock(m_uiMutex);
        approved = m_approvedNotice.erase(session->DeviceId()) > 0;
    }
    if (approved)
        session->SendControl({{"type", rc::msg::kNotice},
                              {"level", "info"},
                              {"key", "notice.pcAllowedConnect"},
                              {"text", "This PC allowed the headset to connect."}});
    bool remember = true;
    {
        std::lock_guard lock(m_uiMutex);
        remember = m_noRemember.erase(session->DeviceId()) == 0;
    }
    if (remember) SettingsStore::Get().Update([&](Settings& s) {
        const int64_t now = std::time(nullptr);
        for (auto& d : s.trustedDevices) {
            if (d.id == session->DeviceId()) {
                d.lastSeen = now;
                d.name = session->DeviceName();
                if (!session->DeviceKey().empty()) d.publicKey = session->DeviceKey();
                return;
            }
        }
        // Approved locally (or via the account): remember it.
        s.trustedDevices.push_back({session->DeviceId(), session->DeviceName(), session->Model(), now, now, session->DeviceKey()});
    });
    {
        std::lock_guard lock(m_uiMutex);
        m_pendingApproval = nullptr;
    }
    PushUi({{"type", "session"}, {"connected", true}, {"deviceName", session->DeviceName()}});
    if (SettingsStore::Get().Snapshot().preventSleep)
        SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
}

void App::OnSessionEnded(ClientSession* session, const std::string& reason) {
    m_streaming = false;
    FaceShare::Get().Clear();
    PushUi({{"type", "session"}, {"connected", false}, {"reason", reason}, {"deviceName", session->DeviceName()}});
    SetThreadExecutionState(ES_CONTINUOUS);
}

void App::OnVrStateChanged(const std::string& state, const std::string& producer, const std::string& app) {
    PushUi({{"type", "vrState"}, {"state", state}, {"producer", producer}, {"app", app}});
}

json App::SettingsForUi() {
    static std::mutex snapshotMutex;
    static uint64_t revision = 0;
    std::lock_guard lock(snapshotMutex);
    auto session = m_server.ActiveSession();
    json out = session ? session->SyncedSettingsForUi() : SettingsStore::Get().Snapshot().ToJson(true);
    // WebView snapshots can finish delivery out of order; this number never goes on the headset wire.
    out["_revision"] = ++revision;
    return out;
}

json App::StateJson() {
    const Settings s = SettingsStore::Get().Snapshot();
    const auto drv = audio::QueryDriver(audio::DeviceSet::RinCast);
    const auto steam = audio::QueryDriver(audio::DeviceSet::Steam);
    const auto route = audio::ResolveRoute(s.audioDevices);
    const auto svr = steamvr::SteamVrPath();
    json vpnAddresses = json::array();
    for (const auto& itf : rc::ListInterfaces())
        if (itf.isVpn) vpnAddresses.push_back({{"name", itf.name}, {"address", rc::NetAddress{itf.ip, 0}.IpString()}});
    json state{
        {"type", "state"},
        {"version", Version()},
        {"downloadUrl", rc::kDownloadUrl},
        {"downloadSite", rc::kDownloadSite},
        {"settings", SettingsForUi()},
        {"cloud", m_cloud.StatusJson()},
        {"system",
         {{"gpu", m_gpu.name},
          {"os", WindowsVersion()},
          {"encoder",
           {{"backend", m_caps.backend}, {"h264", m_caps.h264}, {"hevc", m_caps.hevc}, {"av1", m_caps.av1},
            {"hevc10", m_caps.hevc10}, {"hwFallback", m_caps.hwFallback}}},
          {"steamvr",
           {{"installed", svr.has_value()}, {"running", steamvr::IsRunning()},
            {"driverRegistered", steamvr::IsDriverRegistered()}}},
          {"openxr",
           {{"rincastAvailable", openxr::IsRinCastAvailable()}, {"rincastActive", openxr::IsRinCastActive()}, {"steamvrActive", openxr::IsSteamVrActive()},
            {"active", Narrow(openxr::ActiveRuntime())}}},
          {"audioDriver",
           {{"speaker", drv.speaker}, {"microphone", drv.microphone}, {"microphoneFeed", drv.microphoneFeed},
            {"steam", {{"speaker", steam.speaker}, {"microphone", steam.microphone && steam.microphoneFeed}}},
            {"active", {{"speaker", route.speakerSet}, {"microphone", route.micSet}}}}},
          {"addresses", m_cloud.LanAddresses()},
          {"vpnAddresses", vpnAddresses},
          {"firewall", FirewallJson()},
          {"elevated", IsElevated()},
          {"inputBlocked", input::InputBlocked()},
          {"port", m_server.Port()}}},
    };
    if (auto session = m_server.ActiveSession()) {
        state["session"] = session->StatusJson();
    } else {
        state["session"] = nullptr;
    }
    std::lock_guard lock(m_uiMutex);
    state["pendingApproval"] = m_pendingApproval;
    const OscEyeStatus osc = m_oscEye.Status();
    state["oscEye"] = {{"enabled", osc.enabled},
                       {"listening", osc.listening},
                       {"active", osc.active},
                       {"port", osc.port},
                       {"bindError", osc.bindError},
                       {"rateHz", osc.rateHz},
                       {"lastNormX", osc.lastNormX},
                       {"lastNormY", osc.lastNormY},
                       {"lastAddress", osc.lastAddress},
                       {"unrecognizedPackets", osc.unrecognizedPackets}};
    return state;
}

// A network can change at any time, and with it what the firewall applies.
void App::CheckFirewall() {
    if (m_firewallChecking.exchange(true)) return;
    RunAsync([this] {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const firewall::Status now = firewall::Query(exe);
        const json before = FirewallJson();
        {
            std::lock_guard lock(m_firewallMutex);
            m_firewall = now;
        }
        m_firewallChecking = false;
        const json after = FirewallJson();
        if (after != before) PushUi({{"type", "firewall"}, {"firewall", after}});
    });
}

json App::FirewallJson() {
    std::lock_guard lock(m_firewallMutex);
    return {{"otherNetworksOpen", m_firewall.otherNetworksOpen},
            {"publicInUse", m_firewall.publicInUse},
            {"publicAllowed", m_firewall.publicAllowed},
            {"publicUnguarded", m_firewall.publicUnguarded},
            {"usbAdapter", m_firewall.usbAdapter},
            {"usbOpen", m_firewall.usbOpen}};
}

void App::Tick() {
    const bool blocked = input::InputBlocked();
    if (blocked != m_lastInputBlocked) {
        m_lastInputBlocked = blocked;
        PushUi({{"type", "inputState"}, {"blocked", blocked}});
    }
    if (m_firewallTicks-- <= 0) {
        m_firewallTicks = 10;
        CheckFirewall();
    }
    // The headset gave up while the prompt was open: nothing is left to answer.
    std::string asking;
    {
        std::lock_guard lock(m_uiMutex);
        if (m_pendingApproval.is_object()) asking = m_pendingApproval.value("deviceId", "");
    }
    if (!asking.empty() && !m_server.StillAsking(asking, 10'000'000'000LL)) {
        {
            std::lock_guard lock(m_uiMutex);
            m_pendingApproval = nullptr;
            m_approvedNotice.erase(asking);  // nobody will start a session for it (A19 #13)
        }
        PushUi({{"type", "approvalEnded"}});
    }
    if (auto session = m_server.ActiveSession()) {
        PushUi({{"type", "stats"}, {"session", session->StatusJson()}});
        if (SettingsStore::Get().Snapshot().preventSleep)
            SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
    }
    const OscEyeStatus osc = m_oscEye.Status();
    if (osc.enabled) {
        PushUi({{"type", "oscEye"},
                {"oscEye",
                 {{"enabled", osc.enabled},
                  {"listening", osc.listening},
                  {"active", osc.active},
                  {"port", osc.port},
                  {"bindError", osc.bindError},
                  {"rateHz", osc.rateHz},
                  {"lastNormX", osc.lastNormX},
                  {"lastNormY", osc.lastNormY},
                  {"lastAddress", osc.lastAddress},
                  {"unrecognizedPackets", osc.unrecognizedPackets}}}});
    }
}

std::string App::ApplySettingSideEffects(const json& changed) {
    std::string error;
    if (changed.contains("startWithWindows") && !SetStartWithWindows(changed["startWithWindows"].get<bool>())) {
        error = "Windows didn't accept this change.";
        SettingsStore::Get().Update([](Settings& s) { s.startWithWindows = !s.startWithWindows; });
    }
    if (changed.contains("pcName") || changed.contains("allowRemote") || changed.contains("cloudUrl")) m_cloud.Wake();
    if (changed.contains("externalEyeTracking") || changed.contains("externalEyeOscPort") ||
        changed.contains("externalEyeInvertX") || changed.contains("externalEyeInvertY") ||
        changed.contains("externalEyeOffsetX") || changed.contains("externalEyeOffsetY") ||
        changed.contains("externalEyeGainX") || changed.contains("externalEyeGainY")) {
        const Settings s = SettingsStore::Get().Snapshot();
        m_oscEye.Configure(s.externalEyeTracking, static_cast<uint16_t>(s.externalEyeOscPort),
                           s.externalEyeInvertX, s.externalEyeInvertY,
                           s.externalEyeOffsetX, s.externalEyeOffsetY,
                           s.externalEyeGainX, s.externalEyeGainY);
    }
    if (auto session = m_server.ActiveSession()) {
        session->OnSettingsChanged(changed);
        // PC 视频参数在建流时读取；复用 session worker，只重建已有流。
        // 同一批设置只排入一次重建，避免 profile 与其他视频键重复触发。
        if (changed.contains("streamProfile") || changed.contains("codec") || changed.contains("tenBit") ||
            changed.contains("vrBitrateMbps") || changed.contains("pcBitrateWins") ||
            changed.contains("desktopBitrateMbps") || changed.contains("adaptiveBitrate") ||
            changed.contains("videoPacingMode") || changed.contains("qualityProfileCbr") ||
            changed.contains("fillMode") ||
            changed.contains("fecPercent") ||
            changed.contains("foveatedEncoding") || changed.contains("foveaSize") ||
            changed.contains("foveaPeriphery") || changed.contains("externalEyeTracking") ||
            changed.contains("adaptiveQuantization") ||
            changed.contains("vrSlicedEncoding") ||
            changed.contains("twoPassEncoding") || changed.contains("encodeScalePct") ||
            changed.contains("l7Autoscale") ||
            changed.contains("desktopMaxHeight") || changed.contains("desktopFps"))
            session->OnStreamProfileChanged();
    }
    return error;
}

void App::HandleUiMessage(const std::string& text) {
    json m;
    try {
        m = json::parse(text);
    } catch (...) {
        return;
    }
    const std::string type = m.value("type", "");
    const std::string requestId = m.value("requestId", "");
    auto reply = [this, requestId](json payload) {
        payload["type"] = "reply";
        payload["requestId"] = requestId;
        PushUi(payload);
    };

    if (type == "getState") {
        json s = StateJson();
        s["requestId"] = requestId;
        PushUi(s);
    } else if (type == "setSettings") {
        json changes = m.value("settings", json::object());
        if (changes.value("externalEyeTracking", false)) changes["foveatedEncoding"] = false;
        else if (changes.value("foveatedEncoding", false)) changes["externalEyeTracking"] = false;
        auto save = [&] {
            SettingsStore::Get().Update([&](Settings& s) {
                json merged = s.ToJson(false);
                for (auto& [k, v] : changes.items()) {
                    if (k == "serverId" || k == "cloudRefreshTokenProtected" || k == "trustedDevices") continue;
                    merged[k] = v;
                }
                s.FromJson(merged);
            });
        };
        if (auto session = m_server.ActiveSession()) session->OwnSyncedSettings(changes, save);
        else save();
        const std::string error = ApplySettingSideEffects(changes);
        // Same overlay as StateJson: headset-owned keys must not snap back to the
        // PC's saved values just because the user toggled an unrelated setting.
        reply({{"ok", error.empty()}, {"error", error}, {"settings", SettingsForUi()}});
    } else if (type == "autoscaleReset") {
        // ROUND19 T3: the user's manual reset - the ladder's only way back to 100 %.
        if (auto session = m_server.ActiveSession()) {
            session->ResetAutoscale();
            reply({{"ok", true}});
        } else {
            reply({{"ok", false}, {"error", "no active session"}});
        }
    } else if (type == "approve" || type == "deny") {
        const std::string id = m.value("deviceId", "");
        // A19 audit #13: the headset learns the result through the session that
        // starts right after an approval (see OnSessionStarted). Mark it before the
        // approval opens the door, so the session cannot race us. A denial keeps no
        // session to talk on - the headset already shows the Reject's "declined"
        // text on its next Hello, so nothing is sent for it here.
        {
            std::lock_guard lock(m_uiMutex);
            if (type == "approve") m_approvedNotice.insert(id);
            m_pendingApproval = nullptr;
        }
        if (type == "approve")
            m_server.Approve(id, m.value("remember", true));
        else
            m_server.Deny(id);
        std::lock_guard lock(m_uiMutex);
        if (type == "approve" && !m.value("remember", true)) m_noRemember.insert(id);
    } else if (type == "restartElevated") {
        if (IsElevated()) reply({{"ok", false}, {"error", "already"}});
        else if (!RestartElevated()) reply({{"ok", false}, {"error", "cancelled"}});
        else {
            reply({{"ok", true}});
            if (QuitRequest) QuitRequest();
        }
    } else if (type == "disconnect") {
        RunAsync([this] { m_server.DisconnectActive("disconnected_by_pc"); });
    } else if (type == "removeTrusted") {
        const std::string id = m.value("deviceId", "");
        SettingsStore::Get().Update([&](Settings& s) {
            std::erase_if(s.trustedDevices, [&](const TrustedDevice& d) { return d.id == id; });
        });
        reply({{"ok", true}});
    } else if (type == "signIn" || type == "register") {
        const std::string email = m.value("email", ""), password = m.value("password", ""), name = m.value("name", "");
        RunAsync([=, this] {
            const std::string err =
                type == "signIn" ? m_cloud.SignIn(email, password) : m_cloud.Register(name, email, password);
            reply({{"ok", err.empty()}, {"error", err}, {"cloud", m_cloud.StatusJson()}});
        });
    } else if (type == "signOut") {
        RunAsync([=, this] {
            m_cloud.SignOut();
            reply({{"ok", true}, {"cloud", m_cloud.StatusJson()}});
        });
    } else if (type == "approveCode") {
        const std::string code = m.value("code", "");
        RunAsync([=, this] {
            const std::string err = m_cloud.ApproveHeadsetCode(code);
            reply({{"ok", err.empty()}, {"error", err}});
        });
    } else if (type == "registerSteamVrDriver") {
        RunAsync([=] { reply({{"ok", steamvr::RegisterDriver()}}); });
    } else if (type == "setOpenXrRuntime") {
        const std::string runtime = m.value("runtime", "steamvr");
        RunAsync([=] {
            const bool ok = openxr::SetActiveRuntime(runtime);
            if (ok) SettingsStore::Get().Update([&](Settings& s) { s.openxrRuntime = runtime; });
            reply({{"ok", ok}});
        });
    } else if (type == "setFirewall") {
        const bool allowPublic = m.value("allowPublic", false);
        RunAsync([=] {
            wchar_t exe[MAX_PATH];
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            firewall::Set(exe, allowPublic);
            // What counts is what Windows says now, not what was asked for.
            const firewall::Status now = firewall::Query(exe);
            {
                std::lock_guard lock(m_firewallMutex);
                m_firewall = now;
            }
            reply({{"ok", now.otherNetworksOpen && now.publicAllowed == allowPublic}});
        });
    } else if (type == "allowUsbCable") {
        std::string adapter;
        {
            std::lock_guard lock(m_firewallMutex);
            adapter = m_firewall.usbAdapter;
        }
        RunAsync([=] {
            wchar_t exe[MAX_PATH];
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            firewall::AllowUsbAdapter(exe, adapter);
            const firewall::Status now = firewall::Query(exe);
            {
                std::lock_guard lock(m_firewallMutex);
                m_firewall = now;
            }
            reply({{"ok", now.usbOpen}});
        });
    } else if (type == "launchSteamVR") {
        RunAsync([] { steamvr::Launch(); });
    } else if (type == "restartSteamVR") {
        RunAsync([] { steamvr::Restart(); });
    } else if (type == "openLogs") {
        ShellExecuteW(nullptr, L"open", (DataDir() / L"logs").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else if (type == "openUrl") {
        const std::string url = m.value("url", "");
        if (url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0)
            ShellExecuteW(nullptr, L"open", Widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

}  // namespace rcs
