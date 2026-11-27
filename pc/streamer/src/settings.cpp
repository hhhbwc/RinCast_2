#include "settings.h"

#include <algorithm>
#include <functional>

#include "rc/platform.h"

namespace rcs {

json Settings::ToJson(bool forUi) const {
    json devices = json::array();
    for (const auto& d : trustedDevices)
        devices.push_back({{"id", d.id}, {"name", d.name}, {"model", d.model}, {"firstSeen", d.firstSeen},
                           {"lastSeen", d.lastSeen}, {"publicKey", d.publicKey}});
    json j{
        {"serverId", serverId},
        {"pcName", pcName},
        {"startWithWindows", startWithWindows},
        {"startMinimized", startMinimized},
        {"preventSleep", preventSleep},
        {"requireApproval", requireApproval},
        {"desktopInput", desktopInput},
        {"trustedDevices", devices},
        {"codec", codec},
        {"tenBit", tenBit},
        {"vrBitrateMbps", vrBitrateMbps},
        {"desktopBitrateMbps", desktopBitrateMbps},
        {"adaptiveBitrate", adaptiveBitrate},
        // A86 (user, 2026-11-23): the picture tier is no longer a user choice - the
        // automatic tuner owns it end to end. StreamProfile is not persisted at all
        // now: FromJson ignores the key, so a file written by an older build is read
        // with the fixed start rung and the stale value is simply dropped on the next
        // save. The field itself stays (Settings::streamProfile) because EffectiveSettings
        // overwrites it with the live rung for the stream builders; see client_session.cpp.
        // A86b: profileControlsVideo was removed with the tier slider - CBR is
        // now a plain independent switch (qualityProfileCbr).
        {"videoPacingMode", videoPacingMode},
        {"qualityProfileCbr", qualityProfileCbr},
        {"encodeScalePct", encodeScalePct},
        {"l7Autoscale", l7Autoscale},
        // A86b: autoTune is deliberately NOT serialized. With no tier slider left,
        // an old file that still says "autoTune": false would silently pin the
        // machine to one rung with no UI anywhere to turn it back on.
        {"fillMode", fillMode},
        {"pcBitrateWins", pcBitrateWins},
        {"fecPercent", fecPercent},
        {"foveatedEncoding", foveatedEncoding},
        {"externalEyeTracking", externalEyeTracking},
        {"adaptiveQuantization", adaptiveQuantization},
        {"twoPassEncoding", twoPassEncoding},
        {"vrSlicedEncoding", vrSlicedEncoding},
        {"desktopMaxHeight", desktopMaxHeight},
        {"desktopFps", desktopFps},
        {"streamAudio", streamAudio},
        {"micPassthrough", micPassthrough},
        {"switchDefaultAudio", switchDefaultAudio},
        {"playOnPcToo", playOnPcToo},
        {"audioDevices", audioDevices},
        {"openxrRuntime", openxrRuntime},
        {"autoRegisterSteamVrDriver", autoRegisterSteamVrDriver},
        {"externalEyeOscPort", externalEyeOscPort},
        {"externalEyeInvertX", externalEyeInvertX},
        {"externalEyeInvertY", externalEyeInvertY},
        {"externalEyeOffsetX", externalEyeOffsetX},
        {"externalEyeOffsetY", externalEyeOffsetY},
        {"externalEyeGainX", externalEyeGainX},
        {"externalEyeGainY", externalEyeGainY},
        {"foveaSize", foveaSize > 0.05f ? foveaSize : 0.40f},
        {"foveaPeriphery", foveaPeriphery > 0.05f ? foveaPeriphery : 0.50f},
        {"cloudUrl", cloudUrl},
        {"allowRemote", allowRemote},
        {"cloudUserName", cloudUserName},
        {"cloudEmail", cloudEmail},
        // A84: the stamp every save carries, so FromJson can tell "this file was
        // written when autoTune still defaulted to off" from "the user chose it".
        // Written as the constant (not a field) so the UI's read-modify-write round
        // trip in app.cpp (merged = ToJson, patch, FromJson) always re-stamps.
        {"settingsSchema", kSettingsSchema},
    };
    if (!forUi) j["cloudRefreshTokenProtected"] = cloudRefreshTokenProtected;
    return j;
}

void Settings::FromJson(const json& j) {
    if (!j.is_object()) return;
    auto get = [&](const char* key, auto& field) {
        if (j.contains(key) && !j[key].is_null()) {
            try {
                field = j[key].get<std::decay_t<decltype(field)>>();
            } catch (...) {
            }
        }
    };
    get("serverId", serverId);
    get("pcName", pcName);
    get("startWithWindows", startWithWindows);
    get("startMinimized", startMinimized);
    get("preventSleep", preventSleep);
    get("requireApproval", requireApproval);
    get("desktopInput", desktopInput);
    get("codec", codec);
    get("tenBit", tenBit);
    get("vrSlicedEncoding", vrSlicedEncoding);
    get("vrBitrateMbps", vrBitrateMbps);
    get("desktopBitrateMbps", desktopBitrateMbps);
    get("adaptiveBitrate", adaptiveBitrate);
    // A86: "streamProfile" is deliberately NOT read - see the note in ToJson. An old
    // settings file keeps its key, but it can no longer pin the tuner's start rung.
    // A86b: "profileControlsVideo" is not read; use qualityProfileCbr directly.
    get("videoPacingMode", videoPacingMode);
    get("qualityProfileCbr", qualityProfileCbr);
    get("encodeScalePct", encodeScalePct);
    get("l7Autoscale", l7Autoscale);
    // A86b: "autoTune" is not read - see ToJson. The field keeps its true default.
    get("fillMode", fillMode);
    get("pcBitrateWins", pcBitrateWins);
    get("fecPercent", fecPercent);
    get("foveatedEncoding", foveatedEncoding);
    get("externalEyeTracking", externalEyeTracking);
    get("adaptiveQuantization", adaptiveQuantization);
    get("twoPassEncoding", twoPassEncoding);
    get("desktopMaxHeight", desktopMaxHeight);
    get("desktopFps", desktopFps);
    get("streamAudio", streamAudio);
    get("micPassthrough", micPassthrough);
    get("switchDefaultAudio", switchDefaultAudio);
    get("playOnPcToo", playOnPcToo);
    get("audioDevices", audioDevices);
    get("openxrRuntime", openxrRuntime);
    get("autoRegisterSteamVrDriver", autoRegisterSteamVrDriver);
    get("externalEyeOscPort", externalEyeOscPort);
    get("externalEyeInvertX", externalEyeInvertX);
    get("externalEyeInvertY", externalEyeInvertY);
    get("externalEyeOffsetX", externalEyeOffsetX);
    get("externalEyeOffsetY", externalEyeOffsetY);
    get("externalEyeGainX", externalEyeGainX);
    get("externalEyeGainY", externalEyeGainY);
    get("foveaSize", foveaSize);
    get("foveaPeriphery", foveaPeriphery);
    get("cloudUrl", cloudUrl);
    get("allowRemote", allowRemote);
    get("cloudRefreshTokenProtected", cloudRefreshTokenProtected);
    get("cloudUserName", cloudUserName);
    get("cloudEmail", cloudEmail);
    // A86b: the autoTune schema migration is gone with the persisted key. There is
    // no file state to migrate any more - the tuner is always on, and the only way
    // to force a rung is a process-lifetime environment variable (see app.cpp).
    if (const auto it = j.find("trustedDevices"); it != j.end() && it->is_array()) {
        trustedDevices.clear();
        for (const auto& d : *it) {
            if (!d.is_object()) continue;
            TrustedDevice td{};
            td.firstSeen = 0;
            td.lastSeen = 0;
            auto getStr = [&](const char* key, std::string& out) {
                if (const auto f = d.find(key); f != d.end() && f->is_string()) {
                    try {
                        out = f->get<std::string>();
                    } catch (...) {
                    }
                }
            };
            auto getI64 = [&](const char* key, int64_t& out) {
                if (const auto f = d.find(key); f != d.end() && f->is_number_integer()) {
                    try {
                        out = f->get<int64_t>();
                    } catch (...) {
                    }
                }
            };
            getStr("id", td.id);
            getStr("name", td.name);
            getStr("model", td.model);
            getI64("firstSeen", td.firstSeen);
            getI64("lastSeen", td.lastSeen);
            getStr("publicKey", td.publicKey);
            if (!td.id.empty()) trustedDevices.push_back(std::move(td));
        }
    }
    // Clamp to sane ranges.
    vrBitrateMbps = std::clamp(vrBitrateMbps, 10, 500);
    desktopBitrateMbps = std::clamp(desktopBitrateMbps, 5, 200);
    fecPercent = std::clamp(fecPercent, 0, 25);
    externalEyeOscPort = std::clamp(externalEyeOscPort, 1024, 65535);
    externalEyeOffsetX = std::clamp(externalEyeOffsetX, -20.0f, 20.0f);
    externalEyeOffsetY = std::clamp(externalEyeOffsetY, -20.0f, 20.0f);
    externalEyeGainX = std::clamp(externalEyeGainX, 0.2f, 3.0f);
    externalEyeGainY = std::clamp(externalEyeGainY, 0.2f, 3.0f);
    if (foveaSize > 1.0f) foveaSize /= 100.0f;
    if (foveaPeriphery > 1.0f) foveaPeriphery /= 100.0f;
    if (foveaSize > 0.01f) foveaSize = std::clamp(foveaSize, 0.10f, 0.80f);
    if (foveaPeriphery > 0.01f) foveaPeriphery = std::clamp(foveaPeriphery, 0.10f, 0.80f);
    if (externalEyeTracking && foveatedEncoding) foveatedEncoding = false;
    streamProfile = std::clamp(streamProfile, 0, 2);
    // 0 = follow the profile; otherwise clamp to the supported range.
    encodeScalePct = encodeScalePct > 0 ? std::clamp(encodeScalePct, 50, 100) : 0;
    if (codec != "auto" && codec != "h264" && codec != "hevc" && codec != "av1") codec = "auto";
    if (openxrRuntime != "rincast" && openxrRuntime != "steamvr") openxrRuntime = "steamvr";
    if (audioDevices != "auto" && audioDevices != "rincast" && audioDevices != "steam" && audioDevices != "system")
        audioDevices = "auto";
}

SettingsStore& SettingsStore::Get() {
    static SettingsStore store;
    return store;
}

void SettingsStore::Load() {
    std::lock_guard lock(m_mutex);
    std::vector<uint8_t> bytes;
    if (ReadFileBytes(DataDir() / L"settings.json", bytes)) {
        try {
            m_settings.FromJson(json::parse(bytes.begin(), bytes.end()));
        } catch (const std::exception& e) {
            RC_LOGW("settings.json is corrupt (%s), using defaults", e.what());
        }
    }
    bool dirty = false;
    if (m_settings.serverId.empty()) {
        uint8_t id[12];
        rc::RandomBytes(id, sizeof(id));
        m_settings.serverId = rc::HexEncode(id, sizeof(id));
        dirty = true;
    }
    if (m_settings.pcName.empty()) {
        m_settings.pcName = ComputerName();
        dirty = true;
    }
    if (dirty) SaveLocked();
}

Settings SettingsStore::Snapshot() const {
    std::lock_guard lock(m_mutex);
    return m_settings;
}

void SettingsStore::Update(const std::function<void(Settings&)>& mutate) {
    std::lock_guard lock(m_mutex);
    mutate(m_settings);
    SaveLocked();
}

void SettingsStore::SaveLocked() {
    if (!WriteFileAtomic(DataDir() / L"settings.json", m_settings.ToJson(false).dump(2)))
        RC_LOGW("Failed to save settings");
}

}  // namespace rcs
