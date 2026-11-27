// Offline tests compiling and exercising REAL PC streamer production TUs:
//   pc/streamer/src/settings.cpp
//   pc/streamer/src/util.cpp
//   pc/streamer/src/vr/vr_bridge_host.cpp
//   pc/streamer/src/gpu.cpp
//   pc/streamer/src/encode/color_convert.cpp
//   pc/streamer/src/stream/foveator.cpp
//   pc/streamer/src/stream/video_sender.cpp
//
// All stateful resources (RINCAST_DATA_DIR and rcvr::BridgeNames) are strictly isolated per
// test process so this binary NEVER touches live %LOCALAPPDATA%\RinCast or Local\RinCastVRBridge.v1.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "encode/color_convert.h"
#include "encode/encoder.h"
#include "vr/osc_eye_receiver.h"
#include "gpu.h"
#include "nlohmann/json.hpp"
#include "rc/connection.h"
#include "rc/crypto.h"
#include "rc/platform.h"
#include "rc/protocol.h"
#include "rc/socket.h"
#include "rc/video_transport.h"
#include "settings.h"
#include "stream/foveator.h"
#include "stream/stream_config.h"
#include "stream/video_sender.h"
#include "stream/vr_diag_math.h"
#include "util.h"
#include "vr/vr_bridge_host.h"
#include "vr_bridge.h"
#include "layer_reproj.h"

using Microsoft::WRL::ComPtr;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d: %s\n", __func__, __LINE__, #cond);          \
            g_failures++;                                                        \
        }                                                                        \
    } while (0)

void InitIsolatedDataDir() {
    const std::filesystem::path isolated =
        std::filesystem::temp_directory_path() /
        (L"rc-streamer-test-data-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(isolated, ec);
    std::filesystem::create_directories(isolated, ec);
    SetEnvironmentVariableW(L"RINCAST_DATA_DIR", isolated.wstring().c_str());
    const std::filesystem::path resolved = rcs::DataDir();
    CHECK(resolved == isolated);
}

void TestPcSettingsAndPersistence() {
    rcs::SettingsStore& store = rcs::SettingsStore::Get();
    // Write a crafted legacy + out-of-range settings file into isolated DataDir().
    const std::filesystem::path file = rcs::DataDir() / L"settings.json";
    {
        std::ofstream f(file);
        f << R"({
            "serverId": "test-server-id",
            "pcName": "IsolatedTestPC",
            "streamProfile": 2,
            "autoTune": false,
            "vrBitrateMbps": 9999,
            "desktopBitrateMbps": 1,
            "fecPercent": 99,
            "encodeScalePct": 25,
            "codec": "bogus",
            "openxrRuntime": "invalid",
            "audioDevices": "bogus",
            "trustedDevices": [{"id":"dev1","name":"Quest 3","publicKey":"abcd"}]
        })";
    }
    store.Load();
    const rcs::Settings s = store.Snapshot();
    CHECK(s.serverId == "test-server-id");
    CHECK(s.pcName == "IsolatedTestPC");
    CHECK(s.vrBitrateMbps == 500);       // clamped [10, 500]
    CHECK(s.desktopBitrateMbps == 5);    // clamped [5, 200]
    CHECK(s.fecPercent == 25);           // clamped [0, 25]
    CHECK(s.encodeScalePct == 50);       // positive clamped [50, 100]
    CHECK(s.codec == "auto");
    CHECK(s.openxrRuntime == "steamvr");
    CHECK(s.audioDevices == "auto");
    CHECK(s.streamProfile == 1);         // A86: ignored from disk, starts on Balanced (1)
    CHECK(s.trustedDevices.size() == 1 && s.trustedDevices[0].id == "dev1");

    // Atomic Update + ToJson invariants: streamProfile / autoTune must NOT be serialized.
    store.Update([](rcs::Settings& mut) {
        mut.vrBitrateMbps = 180;
        mut.encodeScalePct = 0;  // 0 preserves "follow profile / auto"
        mut.qualityProfileCbr = true;
    });
    const nlohmann::json serialized = store.Snapshot().ToJson(true);
    CHECK(!serialized.contains("streamProfile"));
    CHECK(!serialized.contains("autoTune"));
    CHECK(!serialized.contains("cloudRefreshTokenProtected"));
    CHECK(serialized["settingsSchema"] == rcs::kSettingsSchema);

    // Reload and confirm persistence.
    store.Load();
    CHECK(store.Snapshot().vrBitrateMbps == 180);
    CHECK(store.Snapshot().encodeScalePct == 0);
    CHECK(store.Snapshot().qualityProfileCbr == true);

    // Corrupt JSON must not crash Load().
    {
        std::ofstream f(file);
        f << "{not valid json";
    }
    store.Load();
    CHECK(store.Snapshot().vrBitrateMbps == 180);
    std::printf("streamer settings: clamping, A86 schema isolation, persistence, corrupt-guard ok\n");
}

void TestPcSettingsMalformedAndIsolated() {
    // Verify FromJson rejects non-object JSON cleanly without throwing or mutating defaults.
    const rcs::Settings defaults{};
    for (const nlohmann::json& nonObj :
         {nlohmann::json(nullptr), nlohmann::json::array({1, 2, "bad"}), nlohmann::json(123)}) {
        rcs::Settings s{};
        bool threw = false;
        try {
            s.FromJson(nonObj);
        } catch (const std::exception& e) {
            std::printf("TestPcSettingsMalformedAndIsolated: non-object FromJson threw: %s\n", e.what());
            threw = true;
        } catch (...) {
            threw = true;
        }
        CHECK(!threw);
        CHECK(s.vrBitrateMbps == defaults.vrBitrateMbps);
        CHECK(s.codec == defaults.codec);
        CHECK(s.trustedDevices.empty());
    }

    // Malformed trustedDevices must not throw or skip clamping; valid dev-ok must survive with safe defaults.
    {
        rcs::Settings s{};
        bool threw = false;
        const nlohmann::json malformed = nlohmann::json::parse(R"({
            "vrBitrateMbps": 9999,
            "trustedDevices": [
                null,
                42,
                {"id": 123},
                {"id": "dev-ok", "name": "PICO", "firstSeen": "nan", "lastSeen": 777}
            ]
        })");
        try {
            s.FromJson(malformed);
        } catch (const std::exception& e) {
            std::printf("TestPcSettingsMalformedAndIsolated: malformed trustedDevices threw: %s\n", e.what());
            threw = true;
        } catch (...) {
            threw = true;
        }
        CHECK(!threw);
        CHECK(s.vrBitrateMbps == 500);
        CHECK(s.trustedDevices.size() == 1);
        if (s.trustedDevices.size() == 1) {
            CHECK(s.trustedDevices[0].id == "dev-ok");
            CHECK(s.trustedDevices[0].name == "PICO");
            CHECK(s.trustedDevices[0].model.empty());
            CHECK(s.trustedDevices[0].firstSeen == 0);
            CHECK(s.trustedDevices[0].lastSeen == 777);
            CHECK(s.trustedDevices[0].publicKey.empty());
        }

        // Non-array trustedDevices must also be ignored safely without throwing or clearing existing devices.
        bool nonArrayThrew = false;
        try {
            s.FromJson(nlohmann::json::parse(R"({"trustedDevices": 123})"));
        } catch (...) {
            nonArrayThrew = true;
        }
        CHECK(!nonArrayThrew);
        CHECK(s.trustedDevices.size() == 1);
    }

    // SettingsStore::Load() inside isolated RINCAST_DATA_DIR with extreme scalars + malformed devices + corrupt JSON.
    const std::filesystem::path dataDir = rcs::DataDir();
    CHECK(dataDir.wstring().find(L"rc-streamer-test-data-") != std::wstring::npos);
    const std::filesystem::path file = dataDir / L"settings.json";
    rcs::SettingsStore& store = rcs::SettingsStore::Get();
    {
        std::ofstream f(file);
        f << R"({
            "vrBitrateMbps": -9999,
            "desktopBitrateMbps": 999999,
            "fecPercent": -10,
            "encodeScalePct": 999,
            "codec": 12345,
            "openxrRuntime": null,
            "audioDevices": [],
            "trustedDevices": [
                null,
                42,
                {"id": 123},
                {"id": "dev-ok", "name": "PICO", "firstSeen": "nan", "lastSeen": 777}
            ]
        })";
    }
    store.Load();
    const rcs::Settings loaded = store.Snapshot();
    CHECK(!loaded.serverId.empty());
    CHECK(!loaded.pcName.empty());
    CHECK(loaded.vrBitrateMbps == 10);
    CHECK(loaded.desktopBitrateMbps == 200);
    CHECK(loaded.fecPercent == 0);
    CHECK(loaded.encodeScalePct == 100);
    CHECK(loaded.codec == "auto");
    CHECK(loaded.openxrRuntime == "steamvr");
    CHECK(loaded.audioDevices == "auto");
    CHECK(loaded.trustedDevices.size() == 1);
    if (loaded.trustedDevices.size() == 1) {
        CHECK(loaded.trustedDevices[0].id == "dev-ok");
        CHECK(loaded.trustedDevices[0].firstSeen == 0);
        CHECK(loaded.trustedDevices[0].lastSeen == 777);
    }

    // Non-object & corrupt JSON written to isolated settings.json must not crash or corrupt valid state.
    for (const char* raw : {"null", "[]", "123", "{corrupt"}) {
        {
            std::ofstream f(file);
            f << raw;
        }
        store.Load();
        CHECK(store.Snapshot().vrBitrateMbps == 10);
        CHECK(store.Snapshot().trustedDevices.size() == 1);
    }
    std::printf("streamer settings: malformed trustedDevices, non-object JSON, extreme Load() ok\n");
}

void TestVrBridgeHostAndClient() {
    const std::wstring pid = std::to_wstring(GetCurrentProcessId());
    const std::wstring shmName = L"Local\\RinCastVRBridge.Test." + pid;
    const std::wstring frameEvtName = L"Local\\RinCastVRFrameReady.Test." + pid;
    const std::wstring trackEvtName = L"Local\\RinCastVRTracking.Test." + pid;
    const rcvr::BridgeNames names{shmName.c_str(), frameEvtName.c_str(), trackEvtName.c_str()};

    rcs::VrBridgeHost host;
    CHECK(host.Start(names));
    CHECK(host.Device() != nullptr);

    rcvr::BridgeClient client;
    CHECK(client.Open(names));
    CHECK(client.StreamerAlive());

    rcvr::DisplayConfig cfg{};
    cfg.eyeWidth = 1024;
    cfg.eyeHeight = 1024;
    cfg.refreshRate = 90.f;
    cfg.ipd = 0.063f;
    cfg.fovTan[0][0] = cfg.fovTan[0][1] = cfg.fovTan[0][2] = cfg.fovTan[0][3] = 1.0f;
    cfg.fovTan[1][0] = cfg.fovTan[1][1] = cfg.fovTan[1][2] = cfg.fovTan[1][3] = 1.0f;
    CHECK(!host.Configure(cfg));
    CHECK(host.TextureGeneration() == 1);
    CHECK(host.SlotTexture(0) != nullptr && host.SlotTexture(2) != nullptr);

    // Bump resolution -> textures regenerate & configGeneration increments.
    const LONG genBefore = client.shared()->configGeneration;
    cfg.eyeWidth = 1280;
    cfg.eyeHeight = 1280;
    host.Configure(cfg);
    CHECK(host.TextureGeneration() == 2);
    CHECK(client.shared()->configGeneration > genBefore);

    // Toggle 10-bit without changing resolution/refresh/FOV -> textures regenerate & configGeneration increments.
    const LONG genBefore10 = client.shared()->configGeneration;
    host.Configure(cfg, true);
    CHECK(host.TextureGeneration() == 3);
    CHECK(client.shared()->textureFormat == DXGI_FORMAT_R10G10B10A2_TYPELESS);
    CHECK(client.shared()->configGeneration > genBefore10);

    // Concurrent seqlock stress: writer bumps monotonically coherent sequence/position while reader checks zero tears.
    std::atomic<bool> stopSeqlock{false};
    std::atomic<uint64_t> reads{0};
    std::thread reader([&] {
        rcvr::TrackingSnapshot snap{};
        while (!stopSeqlock) {
            if (rcvr::ReadTracking(client.shared(), snap)) {
                CHECK(std::fabs(snap.hmd.position[0] - float(snap.sequence)) < 0.01f);
                CHECK(snap.headsetTargetNs == snap.sequence * 1000ULL);
                reads++;
            }
        }
    });
    for (uint64_t seq = 1; seq <= 25000; seq++) {
        rcvr::TrackingSnapshot s{};
        s.sequence = seq;
        s.receivedPcNs = rcvr::QpcNowNs();
        s.targetDisplayPcNs = s.receivedPcNs + 40'000'000;
        s.headsetTargetNs = seq * 1000ULL;
        s.flags = 1;
        s.hmd.position[0] = float(seq);
        host.PublishTracking(s, (seq % 5) != 0);
    }
    stopSeqlock = true;
    reader.join();
    CHECK(reads.load() >= 100);

    // LookupTracking cache hit & clockValid echo.
    int64_t targetNs = 0;
    bool clockValid = false;
    CHECK(host.LookupTracking(24999, targetNs, clockValid));
    CHECK(clockValid == true);
    CHECK(host.LookupTracking(25000, targetNs, clockValid));
    CHECK(clockValid == false);

    // Test concurrent SlipVsync vs HeartbeatLoop (regression guard against lost RMW updates!).
    const int64_t epochBefore = client.shared()->vsyncEpochNs;
    std::thread slipA([&] {
        for (int i = 0; i < 40; i++) {
            host.SlipVsync(100'000);  // 40 * 0.1 ms = 4 ms
            rc::SleepMs(1);
        }
    });
    std::thread slipB([&] {
        for (int i = 0; i < 40; i++) {
            host.SlipVsync(100'000);  // + 4 ms -> total 8 ms (< 50 ms cap)
            rc::SleepMs(1);
        }
    });
    slipA.join();
    slipB.join();
    // Wait long enough for HeartbeatLoop to drain 8 ms at kVrPhaseSlipRate (0.03 -> ~267 ms).
    rc::SleepMs(480);
    const int64_t epochAfter = client.shared()->vsyncEpochNs;
    const double slippedMs = double(epochAfter - epochBefore) / 1e6;
    CHECK(slippedMs >= 7.2 && slippedMs <= 8.8);

    // Haptics ring > kHapticsRing (32) wrap test.
    std::atomic<uint32_t> hapticsSeen{0};
    std::atomic<uint32_t> lastSeq{0};
    host.SetHapticHandler([&](const rcvr::HapticEvent& e) {
        CHECK(e.sequence > lastSeq.load());
        lastSeq = e.sequence;
        hapticsSeen++;
    });
    for (int i = 0; i < 48; i++) {
        client.PushHaptic(uint32_t(i & 1), 0.02f, 160.f, 0.5f);
        rc::SleepMs(2);
    }
    rc::SleepMs(40);
    CHECK(hapticsSeen.load() >= 45);

    // Slot acquire / publish / release lifecycle.
    const int slot = client.AcquireSlot();
    CHECK(slot >= 0);
    client.shared()->slots[slot].frameIndex = 77;
    client.PublishSlot(slot);
    CHECK(client.Untaken(slot, 77));
    InterlockedExchange(&client.shared()->slots[slot].state, rcvr::kSlotFree);
    CHECK(!client.Untaken(slot, 77));

    client.Close();
    host.Stop();
    std::printf("streamer VrBridgeHost + BridgeClient: seqlock, lookup, slip CAS (%.1f ms), haptics wrap ok\n",
                slippedMs);
}

void TestVrBridgeHostHapticsRaceAndBurst() {
    const std::wstring pid = std::to_wstring(GetCurrentProcessId());
    const std::wstring shmName = L"Local\\RinCastVRBridge.HapticsTest." + pid;
    const std::wstring frameEvtName = L"Local\\RinCastVRFrameReady.HapticsTest." + pid;
    const std::wstring trackEvtName = L"Local\\RinCastVRTracking.HapticsTest." + pid;
    const rcvr::BridgeNames names{shmName.c_str(), frameEvtName.c_str(), trackEvtName.c_str()};

    rcs::VrBridgeHost host;
    CHECK(host.Start(names));
    rcvr::Shared* shared = host.Shared();
    CHECK(shared != nullptr);

    rcvr::BridgeClient client;
    CHECK(client.Open(names));

    std::mutex rxMutex;
    std::vector<rcvr::HapticEvent> rx;
    host.SetHapticHandler([&](const rcvr::HapticEvent& e) {
        std::lock_guard lock(rxMutex);
        rx.push_back(e);
    });

    auto waitForSeq = [&](uint32_t targetSeq, int timeoutMs, rcvr::HapticEvent& out) {
        const int64_t deadline = rcvr::QpcNowNs() + int64_t(timeoutMs) * 1'000'000LL;
        while (rcvr::QpcNowNs() < deadline) {
            {
                std::lock_guard lock(rxMutex);
                for (const auto& ev : rx) {
                    if (ev.sequence == targetSeq) {
                        out = ev;
                        return true;
                    }
                }
            }
            rc::SleepMs(2);
        }
        return false;
    };

    // 1. Split-write race: reserve slot, sleep 4 ms (and ensure HeartbeatLoop polls at least once)
    //    BEFORE writing fields + MemoryBarrier() + e.sequence = index + 1.
    {
        // Enable small vsync drift so HeartbeatLoop updates vsyncEpochNs every poll cycle.
        host.ShiftVsync(5'000'000);
        rc::SleepMs(2);

        const LONG idx = InterlockedIncrement(&shared->hapticsWrite) - 1;
        const int64_t tStart = rcvr::QpcNowNs();
        int64_t prevEpoch = shared->vsyncEpochNs;
        int epochChanges = 0;
        const int64_t pollDeadline = tStart + 250'000'000LL;
        while ((epochChanges < 2 || (rcvr::QpcNowNs() - tStart) < 4'000'000LL) &&
               rcvr::QpcNowNs() < pollDeadline) {
            rc::SleepMs(1);
            const int64_t curEpoch = shared->vsyncEpochNs;
            if (curEpoch != prevEpoch) {
                prevEpoch = curEpoch;
                epochChanges++;
            }
        }
        CHECK(epochChanges >= 2);

        rcvr::HapticEvent& e = shared->haptics[uint32_t(idx) % uint32_t(rcvr::kHapticsRing)];
        e.hand = 1;
        e.durationS = 0.125f;
        e.frequency = 240.0f;
        e.amplitude = 0.875f;
        MemoryBarrier();
        e.sequence = uint32_t(idx + 1);

        rcvr::HapticEvent got{};
        const bool delivered = waitForSeq(uint32_t(idx + 1), 200, got);
        CHECK(delivered);
        if (delivered) {
            CHECK(got.sequence == uint32_t(idx + 1));
            CHECK(got.hand == 1u);
            CHECK(std::fabs(got.durationS - 0.125f) < 1e-5f);
            CHECK(std::fabs(got.frequency - 240.0f) < 1e-3f);
            CHECK(std::fabs(got.amplitude - 0.875f) < 1e-5f);
        }
    }

    // 2a. Abandoned/dead producer slot (> 65 ms without publishing e.sequence) must not stall subsequent events.
    {
        const LONG abandonedIdx = InterlockedIncrement(&shared->hapticsWrite) - 1;
        rc::SleepMs(75);
        client.PushHaptic(0, 0.05f, 180.0f, 0.65f);

        rcvr::HapticEvent gotValid{};
        const bool validDelivered = waitForSeq(uint32_t(abandonedIdx + 2), 250, gotValid);
        CHECK(validDelivered);
        if (validDelivered) {
            CHECK(gotValid.hand == 0u);
            CHECK(std::fabs(gotValid.durationS - 0.05f) < 1e-5f);
            CHECK(std::fabs(gotValid.frequency - 180.0f) < 1e-3f);
            CHECK(std::fabs(gotValid.amplitude - 0.65f) < 1e-5f);
        }
        bool abandonedDelivered = false;
        {
            std::lock_guard lock(rxMutex);
            for (const auto& ev : rx) {
                if (ev.sequence == uint32_t(abandonedIdx + 1)) abandonedDelivered = true;
            }
        }
        CHECK(!abandonedDelivered);
    }

    // 2b. Abandoned slot followed immediately by a valid event must unblock once wait timeout expires.
    {
        const LONG abandonedIdx2 = InterlockedIncrement(&shared->hapticsWrite) - 1;
        client.PushHaptic(1, 0.075f, 220.0f, 0.45f);

        rcvr::HapticEvent gotValid2{};
        const bool validDelivered2 = waitForSeq(uint32_t(abandonedIdx2 + 2), 350, gotValid2);
        CHECK(validDelivered2);
        if (validDelivered2) {
            CHECK(gotValid2.hand == 1u);
            CHECK(std::fabs(gotValid2.durationS - 0.075f) < 1e-5f);
            CHECK(std::fabs(gotValid2.frequency - 220.0f) < 1e-3f);
            CHECK(std::fabs(gotValid2.amplitude - 0.45f) < 1e-5f);
        }
        bool abandonedDelivered2 = false;
        {
            std::lock_guard lock(rxMutex);
            for (const auto& ev : rx) {
                if (ev.sequence == uint32_t(abandonedIdx2 + 1)) abandonedDelivered2 = true;
            }
        }
        CHECK(!abandonedDelivered2);
    }

    // 3. High-concurrency wrap/burst preserves valid monotonic sequence & untorn payload.
    {
        std::atomic<uint32_t> prevBurstSeq{0};
        std::atomic<uint32_t> burstCount{0};
        std::atomic<uint32_t> tornErrors{0};
        const uint32_t baseSeq = uint32_t(shared->hapticsWrite);
        host.SetHapticHandler([&](const rcvr::HapticEvent& e) {
            const uint32_t prev = prevBurstSeq.exchange(e.sequence);
            if (e.sequence == 0 || e.sequence <= baseSeq || e.sequence <= prev) {
                tornErrors++;
            }
            if (e.hand == 0) {
                if (std::fabs(e.durationS - 0.01f) > 1e-5f || std::fabs(e.frequency - 100.0f) > 1e-3f ||
                    std::fabs(e.amplitude - 0.25f) > 1e-5f) {
                    tornErrors++;
                }
            } else if (e.hand == 1) {
                if (std::fabs(e.durationS - 0.03f) > 1e-5f || std::fabs(e.frequency - 300.0f) > 1e-3f ||
                    std::fabs(e.amplitude - 0.75f) > 1e-5f) {
                    tornErrors++;
                }
            } else {
                tornErrors++;
            }
            burstCount++;
        });

        std::vector<std::thread> producers;
        for (int t = 0; t < 4; t++) {
            producers.emplace_back([&, t] {
                for (int i = 0; i < 120; i++) {
                    if (((t + i) & 1) == 0) {
                        client.PushHaptic(0, 0.01f, 100.0f, 0.25f);
                    } else {
                        client.PushHaptic(1, 0.03f, 300.0f, 0.75f);
                    }
                }
            });
        }
        for (auto& p : producers) p.join();
        rc::SleepMs(10);
        client.PushHaptic(1, 0.03f, 300.0f, 0.75f);
        const uint32_t expectedFinalSeq = uint32_t(shared->hapticsWrite);
        const int64_t burstDeadline = rcvr::QpcNowNs() + 300'000'000LL;
        while (prevBurstSeq.load() < expectedFinalSeq && rcvr::QpcNowNs() < burstDeadline) {
            rc::SleepMs(2);
        }
        CHECK(tornErrors.load() == 0);
        CHECK(burstCount.load() >= 1);
        CHECK(prevBurstSeq.load() == expectedFinalSeq);
    }

    // 4. Clean Start() reset after Stop() re-initializes read cursor & delivers sequence == 1.
    client.Close();
    host.Stop();
    CHECK(host.Start(names));
    CHECK(client.Open(names));
    {
        std::lock_guard lock(rxMutex);
        rx.clear();
    }
    host.SetHapticHandler([&](const rcvr::HapticEvent& e) {
        std::lock_guard lock(rxMutex);
        rx.push_back(e);
    });
    client.PushHaptic(0, 0.04f, 150.0f, 0.5f);
    rcvr::HapticEvent restarted{};
    CHECK(waitForSeq(1u, 200, restarted));
    CHECK(restarted.sequence == 1u);

    client.Close();
    host.Stop();
    std::printf("streamer VrBridgeHost haptics race, abandoned-slot, burst & restart ok\n");
}

static ComPtr<ID3D11Device> CreateHardwareOrWarpDevice(ComPtr<ID3D11DeviceContext>& ctx) {
    auto adapter = rcs::PreferredAdapter();
    if (adapter) {
        if (auto dev = rcs::CreateDevice(adapter.Get(), &ctx)) return dev;
    }
    const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    ComPtr<ID3D11Device> dev;
    if (SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, &fl, 1,
                                    D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
        return dev;
    }
    return nullptr;
}

void TestRealGpuFoveatorAndColorConverter() {
    ComPtr<ID3D11DeviceContext> ctx;
    auto dev = CreateHardwareOrWarpDevice(ctx);
    CHECK(dev != nullptr);
    if (!dev) return;

    constexpr uint32_t eyeW = 512, eyeH = 512, fullW = eyeW * 2;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = fullW;
    td.Height = eyeH;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    // Paint left half mid-gray (128,128,128) and right half bright green (0,255,0).
    std::vector<uint8_t> pixels(size_t(fullW) * eyeH * 4);
    for (uint32_t y = 0; y < eyeH; y++) {
        for (uint32_t x = 0; x < fullW; x++) {
            uint8_t* px = &pixels[(size_t(y) * fullW + x) * 4];
            if (x < eyeW) {
                px[0] = 128; px[1] = 128; px[2] = 128; px[3] = 255;
            } else {
                px[0] = 0; px[1] = 255; px[2] = 0; px[3] = 255;
            }
        }
    }
    D3D11_SUBRESOURCE_DATA init{pixels.data(), fullW * 4, 0};
    ComPtr<ID3D11Texture2D> srcTex;
    CHECK(SUCCEEDED(dev->CreateTexture2D(&td, &init, &srcTex)));

    // 1. Test Foveator::Squeeze output geometry and center color preservation.
    rcs::Foveator fov;
    const uint32_t fovEyeW = rcs::Foveator::EncodedSize(eyeW, 0.45f);
    const uint32_t fovEyeH = rcs::Foveator::EncodedSize(eyeH, 0.45f);
    CHECK(fovEyeW > 0 && fovEyeW < eyeW);
    CHECK(fov.Init(dev.Get(), eyeW, eyeH, fovEyeW, fovEyeH, 0.45f));
    const float starts[2][2] = {{0.275f, 0.275f}, {0.275f, 0.275f}};
    CHECK(fov.Squeeze(ctx.Get(), srcTex.Get(), starts));

    D3D11_TEXTURE2D_DESC fovDesc{};
    fov.Output()->GetDesc(&fovDesc);
    fovDesc.Usage = D3D11_USAGE_STAGING;
    fovDesc.BindFlags = 0;
    fovDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> fovStaging;
    CHECK(SUCCEEDED(dev->CreateTexture2D(&fovDesc, nullptr, &fovStaging)));
    ctx->CopyResource(fovStaging.Get(), fov.Output());
    D3D11_MAPPED_SUBRESOURCE fovMap{};
    CHECK(SUCCEEDED(ctx->Map(fovStaging.Get(), 0, D3D11_MAP_READ, 0, &fovMap)));
    const auto* fovRow = static_cast<const uint8_t*>(fovMap.pData) + size_t(fovEyeH / 2) * fovMap.RowPitch;
    const uint8_t* leftSeam = fovRow + size_t(fovEyeW - 1) * 4;
    const uint8_t* rightSeam = fovRow + size_t(fovEyeW) * 4;
    CHECK(leftSeam[0] == 128 && leftSeam[1] == 128 && leftSeam[2] == 128);
    CHECK(rightSeam[0] == 0 && rightSeam[1] == 255 && rightSeam[2] == 0);
    ctx->Unmap(fovStaging.Get(), 0);

    auto verifyColorConvert = [&](bool tenBit) {
        rcs::ColorConverter conv;
        const uint32_t cw = fovEyeW * 2, ch = fovEyeH;
        CHECK(conv.Init(dev.Get(), cw, ch, tenBit));
        CHECK(conv.Convert(ctx.Get(), fov.Output()));

        D3D11_TEXTURE2D_DESC yd{};
        conv.Output()->GetDesc(&yd);
        CHECK(yd.Format == (tenBit ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12));
        yd.Usage = D3D11_USAGE_STAGING;
        yd.BindFlags = 0;
        yd.MiscFlags = 0;
        yd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        CHECK(SUCCEEDED(dev->CreateTexture2D(&yd, nullptr, &staging)));
        ctx->CopyResource(staging.Get(), conv.Output());
        ctx->Flush();

        D3D11_MAPPED_SUBRESOURCE map{};
        CHECK(SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map)));
        // BT.709 limited range expected:
        // Left eye gray (128/255 = 0.502): Y ~ 16 + 0.502*219 = 126
        // Right eye pure green (0,1,0): Y ~ 16 + 0.7152*219 = 172
        auto sampleY = [&](uint32_t x, uint32_t y) -> int {
            const uint8_t* row = static_cast<const uint8_t*>(map.pData) + size_t(y) * map.RowPitch;
            if (!tenBit) return row[x];
            const uint16_t* row16 = reinterpret_cast<const uint16_t*>(row);
            return int(row16[x] >> 8);
        };
        auto sampleUV = [&](uint32_t x, uint32_t y) -> std::pair<int, int> {
            const uint8_t* uvBase = static_cast<const uint8_t*>(map.pData) + size_t(ch + y / 2) * map.RowPitch;
            if (!tenBit) {
                return {uvBase[(x & ~1u)], uvBase[(x & ~1u) + 1]};
            }
            const uint16_t* uv16 = reinterpret_cast<const uint16_t*>(uvBase);
            return {int(uv16[(x & ~1u)] >> 8), int(uv16[(x & ~1u) + 1] >> 8)};
        };
        const int leftY = sampleY(fovEyeW / 2, fovEyeH / 2);
        const int rightY = sampleY(fovEyeW + fovEyeW / 2, fovEyeH / 2);
        const auto [leftU, leftV] = sampleUV(fovEyeW / 2, fovEyeH / 2);
        const auto [rightU, rightV] = sampleUV(fovEyeW + fovEyeW / 2, fovEyeH / 2);
        ctx->Unmap(staging.Get(), 0);

        CHECK(leftY >= 120 && leftY <= 132);
        CHECK(leftU >= 124 && leftU <= 132 && leftV >= 124 && leftV <= 132);  // neutral gray
        CHECK(rightY >= 166 && rightY <= 178);
        CHECK(rightU < 60 && rightV < 60);  // green chroma in BT.709 has low U and low V
    };

    verifyColorConvert(false);  // NV12
    verifyColorConvert(true);   // P010
    std::printf("streamer D3D11 Foveator + ColorConverter (NV12 & P010): ok\n");
}

void TestRealGpuSharedTypelessVrTexture() {
    ComPtr<ID3D11DeviceContext> ctx;
    auto adapter = rcs::PreferredAdapter();
    auto dev = adapter ? rcs::CreateDevice(adapter.Get(), &ctx) : ComPtr<ID3D11Device>{};
    CHECK(dev != nullptr);
    if (!dev) return;

    constexpr UINT width = 1024, height = 512;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R10G10B10A2_TYPELESS;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    ComPtr<ID3D11Texture2D> source;
    CHECK(SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &source)));

    ComPtr<IDXGIResource1> resource;
    CHECK(SUCCEEDED(source.As(&resource)));
    HANDLE handle = nullptr;
    CHECK(SUCCEEDED(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                                L"Local\\RinCastTestTypeless", &handle)));
    ComPtr<ID3D11Device1> device1;
    ComPtr<ID3D11Texture2D> opened;
    if (SUCCEEDED(dev.As(&device1)) && handle) {
        CHECK(SUCCEEDED(device1->OpenSharedResource1(handle, IID_PPV_ARGS(&opened))));
    }
    CloseHandle(handle);

    ComPtr<IDXGIKeyedMutex> mutex;
    source.As(&mutex);
    CHECK(mutex != nullptr);
    if (mutex) CHECK(SUCCEEDED(mutex->AcquireSync(0, 100)));

    rcs::Foveator fov;
    CHECK(fov.Init(dev.Get(), width / 2, height, width / 2, height, 0.45f, true));
    const float starts[2][2] = {{0.275f, 0.275f}, {0.275f, 0.275f}};
    CHECK(fov.Squeeze(ctx.Get(), source.Get(), starts));
    rcs::ColorConverter conv;
    CHECK(conv.Init(dev.Get(), width, height, true, true));
    CHECK(conv.Convert(ctx.Get(), fov.Output()));
    if (mutex) CHECK(SUCCEEDED(mutex->ReleaseSync(0)));
}

void TestRealGpuFoveatorHalfTexel() {
    ComPtr<ID3D11DeviceContext> ctx;
    auto dev = CreateHardwareOrWarpDevice(ctx);
    CHECK(dev != nullptr);
    if (!dev) return;
    constexpr uint32_t eyeW = 512, eyeH = 8, fullW = eyeW * 2;
    std::vector<uint8_t> pixels(size_t(fullW) * eyeH * 4, 0);
    for (uint32_t y = 0; y < eyeH; y++) {
        for (uint32_t x = 0; x < fullW; x++) {
            uint8_t value = 255;
            if (x == eyeW - 2) value = 200;
            if (x == eyeW - 1) value = 40;
            if (x == eyeW) value = 240;
            if (x == eyeW + 1) value = 80;
            uint8_t* px = &pixels[(size_t(y) * fullW + x) * 4];
            px[0] = px[1] = px[2] = value;
            px[3] = 255;
        }
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = fullW;
    td.Height = eyeH;
    td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{pixels.data(), fullW * 4, 0};
    ComPtr<ID3D11Texture2D> source;
    CHECK(SUCCEEDED(dev->CreateTexture2D(&td, &init, &source)));
    rcs::Foveator fov;
    CHECK(fov.Init(dev.Get(), eyeW, eyeH, eyeW, eyeH, 0.f));
    const float starts[2][2] = {};
    CHECK(fov.Squeeze(ctx.Get(), source.Get(), starts));
    D3D11_TEXTURE2D_DESC desc{};
    fov.Output()->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    CHECK(SUCCEEDED(dev->CreateTexture2D(&desc, nullptr, &staging)));
    ctx->CopyResource(staging.Get(), fov.Output());
    D3D11_MAPPED_SUBRESOURCE map{};
    CHECK(SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map)));
    const auto* row = static_cast<const uint8_t*>(map.pData) + size_t(eyeH / 2) * map.RowPitch;
    const int left = row[size_t(eyeW - 1) * 4];
    const int right = row[size_t(eyeW) * 4];
    ctx->Unmap(staging.Get(), 0);
    CHECK(std::abs(left - 60) <= 8);
    CHECK(std::abs(right - 215) <= 8);
    if (std::abs(left - 60) > 8 || std::abs(right - 215) > 8)
        std::printf("FAIL: foveator eye edges were %d/%d, expected 60/215 within 8\n", left, right);
    else std::printf("PASS: real D3D11 foveator preserves both half-texel eye edges\n");
}

void TestVrDiagMath() {
    // 1. VisitDiagRing: uint32_t wraparound (end=3, count=8, cap=64 -> 59..63, 0..2).
    std::vector<uint32_t> wrapped;
    rcs::VisitDiagRing(3u, 8u, 64u, [&](uint32_t idx) { wrapped.push_back(idx); });
    CHECK((wrapped == std::vector<uint32_t>{59u, 60u, 61u, 62u, 63u, 0u, 1u, 2u}));

    // Edge cases: count == 0, capacity == 0, count > capacity clamped to capacity.
    std::vector<uint32_t> emptyVisits;
    rcs::VisitDiagRing(10u, 0u, 64u, [&](uint32_t idx) { emptyVisits.push_back(idx); });
    rcs::VisitDiagRing(10u, 4u, 0u, [&](uint32_t idx) { emptyVisits.push_back(idx); });
    CHECK(emptyVisits.empty());

    std::vector<uint32_t> clamped;
    rcs::VisitDiagRing(2u, 10u, 4u, [&](uint32_t idx) { clamped.push_back(idx); });
    CHECK((clamped == std::vector<uint32_t>{2u, 3u, 0u, 1u}));

    // 2. ElapsedPositiveNs: preserves precision above 2^53 and guards clock skew/zero.
    const uint64_t sinceBig = uint64_t(1e16);
    const int64_t nowBig = int64_t(sinceBig + 1500ULL);
    CHECK(rcs::ElapsedPositiveNs(nowBig, sinceBig) == 1500.0);
    CHECK(rcs::ElapsedPositiveNs(int64_t(sinceBig + 1ULL), sinceBig) == 1.0);
    CHECK(rcs::ElapsedPositiveNs(int64_t(sinceBig - 10ULL), sinceBig) == 0.0);
    CHECK(rcs::ElapsedPositiveNs(1000, 2000ULL) == 0.0);
    CHECK(rcs::ElapsedPositiveNs(1000, 0ULL) == 0.0);
    CHECK(rcs::ElapsedPositiveNs(0, 100ULL) == 0.0);
    CHECK(rcs::ElapsedPositiveNs(-500, 100ULL) == 0.0);

    // 3. BoundedConvertedCount: prevents uint32_t underflow when bridge > given.
    CHECK(rcs::BoundedConvertedCount(5u, 6u) == 0u);
    CHECK(rcs::BoundedConvertedCount(0u, 1u) == 0u);
    CHECK(rcs::BoundedConvertedCount(5u, 5u) == 0u);
    CHECK(rcs::BoundedConvertedCount(10u, 3u) == 7u);
    std::printf("streamer vr_diag_math (ring wrap, 64-bit ns, bounded count): ok\n");
}

class MockEncoder final : public rcs::VideoEncoder {
   public:
    bool Init(ID3D11Device* /*device*/, const rcs::EncoderConfig& config) override {
        m_config = config;
        lastBitrateKbps = config.bitrateKbps;
        return true;
    }
    bool Encode(ID3D11Texture2D* /*input*/, bool forceKeyframe, rcs::EncodedFrame& out) override {
        out.keyframe = forceKeyframe;
        out.timestamp = ++nextTimestamp;
        out.data.assign(framePayloadBytes, uint8_t(out.timestamp & 0xFF));
        encodedCount++;
        return true;
    }
    void SetBitrate(uint32_t kbps) override {
        lastBitrateKbps = kbps;
        bitrateUpdates.push_back(kbps);
    }
    const char* Name() const override { return "MockEncoder"; }
    bool InvalidateAfter(uint64_t timestamp) override {
        invalidatedTimestamps.push_back(timestamp);
        return allowInvalidate;
    }

    size_t framePayloadBytes = 4096;
    uint64_t nextTimestamp = 0;
    uint32_t encodedCount = 0;
    uint32_t lastBitrateKbps = 0;
    bool allowInvalidate = false;
    std::vector<uint32_t> bitrateUpdates;
    std::vector<uint64_t> invalidatedTimestamps;
};

struct SenderLoopbackPair {
    struct FrameWireStats {
        uint32_t dataFrags = 0;
        uint32_t parityFrags = 0;
        uint16_t declaredCount = 0;
        std::set<uint8_t> fecGroups;
    };

    rc::UdpSocket tx, rx;
    std::unique_ptr<rc::Connection> sendConn, recvConn;
    std::unique_ptr<rc::VideoReassembler> reasm;
    std::thread reader;
    std::atomic<bool> stop{false};
    std::mutex mutex;
    std::map<uint32_t, FrameWireStats> wireStats;
    std::vector<rc::ReceivedFrame> frames;

    bool Open(size_t maxDatagram = rc::kMaxDatagramLan) {
        rc::NetInit();
        if (!tx.Open(0) || !rx.Open(0)) return false;
        tx.SetBuffers(8 << 20, 1 << 20);
        rx.SetBuffers(1 << 20, 8 << 20);
        auto client = rc::crypto::KeyPair::Generate();
        auto server = rc::crypto::KeyPair::Generate();
        const std::string ctx = "r1-sender";
        auto ck = rc::crypto::DeriveSessionKeys(client.secretKey, server.publicKey, client.publicKey,
                                                server.publicKey, ctx);
        auto sk = rc::crypto::DeriveSessionKeys(server.secretKey, client.publicKey, client.publicKey,
                                                server.publicKey, ctx);
        sendConn = std::make_unique<rc::Connection>(&tx, rc::NetAddress{0x7F000001, rx.LocalPort()}, 77, sk, true,
                                                    maxDatagram);
        recvConn = std::make_unique<rc::Connection>(&rx, rc::NetAddress{0x7F000001, tx.LocalPort()}, 77, ck, false,
                                                    maxDatagram);
        reasm = std::make_unique<rc::VideoReassembler>([this](rc::ReceivedFrame&& f) {
            std::lock_guard lock(mutex);
            frames.push_back(std::move(f));
        });
        recvConn->SetPacketHandler([this](rc::PacketType t, const uint8_t* d, size_t n) {
            if (t == rc::PacketType::Video && n >= sizeof(rc::VideoFragmentHeader)) {
                rc::VideoFragmentHeader h{};
                std::memcpy(&h, d, sizeof(h));
                {
                    std::lock_guard lock(mutex);
                    auto& ws = wireStats[h.frameIndex];
                    ws.dataFrags++;
                    ws.declaredCount = h.fragmentCount;
                    ws.fecGroups.insert(h.fecGroupSize);
                }
                reasm->HandleFragment(d, n);
            } else if (t == rc::PacketType::VideoParity && n >= sizeof(rc::VideoParityHeader)) {
                rc::VideoParityHeader ph{};
                std::memcpy(&ph, d, sizeof(ph));
                {
                    std::lock_guard lock(mutex);
                    auto& ws = wireStats[ph.frameIndex];
                    ws.parityFrags++;
                    ws.fecGroups.insert(ph.fecGroupSize);
                }
                reasm->HandleParity(d, n);
            }
        });
        return true;
    }

    void Start() {
        reader = std::thread([this] {
            std::vector<uint8_t> buf(2048);
            rc::NetAddress from;
            while (!stop.load()) {
                const int n = rx.RecvFrom(buf.data(), buf.size(), from, 10);
                if (n <= 0) {
                    recvConn->Tick(rc::NowNs());
                    continue;
                }
                recvConn->HandleDatagram(buf.data(), size_t(n), from);
            }
        });
    }

    bool WaitForAll(uint64_t expectedPackets, size_t expectedFrames, int timeoutMs = 2000) {
        const int64_t deadline = rc::NowNs() + int64_t(timeoutMs) * 1'000'000LL;
        while (rc::NowNs() < deadline) {
            const uint64_t gotPkts = recvConn->Stats().packetsReceived;
            size_t gotFrames = 0;
            {
                std::lock_guard lock(mutex);
                gotFrames = frames.size();
            }
            if (gotPkts >= expectedPackets && gotFrames >= expectedFrames) return true;
            rc::SleepMs(5);
        }
        return false;
    }

    void Stop() {
        stop = true;
        if (reader.joinable()) reader.join();
    }

    ~SenderLoopbackPair() { Stop(); }
};

void TestVideoSenderLiveAndFeedback() {
    const size_t maxHdr = std::max(sizeof(rc::VideoFragmentHeader), sizeof(rc::VideoParityHeader));
    rc::VrFrameMetadata meta{};

    // (a) Burst mode (paceMode=3) + capKbps > 0: WireCeiling triggers MockEncoder::SetBitrate,
    //     BeginFrame recovery uses InvalidateAfter, and concurrent OnFeedback FEC switches
    //     produce per-frame datagram counts self-consistent with a single FEC snapshot.
    {
        SenderLoopbackPair loop;
        CHECK(loop.Open());
        loop.Start();

        const size_t fragPayload = loop.sendConn->MaxPayload() - maxHdr;
        const size_t dataFragsPerFrame = 20;
        const size_t encBytes = dataFragsPerFrame * fragPayload - 2 - sizeof(meta);

        MockEncoder enc;
        rcs::EncoderConfig ec{};
        ec.bitrateKbps = 100'000;
        CHECK(enc.Init(nullptr, ec));
        enc.framePayloadBytes = encBytes;
        enc.allowInvalidate = true;

        rcs::VideoSender sender(loop.sendConn.get(), rc::kVrStreamId, rc::VideoCodec::HEVC, 100'000,
                                /*fecPercent=*/5, /*adaptive=*/true);
        sender.SetPacingMode(3, 1'000'000);
        CHECK(sender.TargetBitrateKbps() == 100'000);

        uint32_t appliedKbps = ec.bitrateKbps;
        auto pushFrame = [&](bool expectKeyframe, bool expectRecovery) {
            const uint32_t target = sender.TargetBitrateKbps();
            if (target != appliedKbps) {
                enc.SetBitrate(target);
                appliedKbps = target;
            }
            const bool kf = sender.BeginFrame(enc);
            CHECK(kf == expectKeyframe);
            rcs::EncodedFrame ef{};
            CHECK(enc.Encode(nullptr, kf, ef));
            const bool rec = sender.TakeRecovery();
            CHECK(rec == expectRecovery);
            sender.Send(ef, &meta, sizeof(meta), 0, 1.0, rec);
        };

        CHECK(sender.WantsFrame() == true);
        pushFrame(true, false);  // frame 1 (keyframe)
        const uint32_t targetAfterFrame1 = sender.TargetBitrateKbps();
        CHECK(targetAfterFrame1 < 100'000 && targetAfterFrame1 >= 50'000);

        CHECK(sender.WantsFrame() == false);
        pushFrame(false, false);  // frame 2 -> triggers MockEncoder::SetBitrate via WireCeiling
        CHECK(!enc.bitrateUpdates.empty());
        CHECK(enc.lastBitrateKbps == targetAfterFrame1);
        pushFrame(false, false);  // frame 3

        // Loss recovery request pointing at frame 2 -> BeginFrame uses InvalidateAfter(2) -> kVideoRecovery.
        rc::VideoFeedbackPayload recFb{};
        recFb.streamId = rc::kVrStreamId;
        recFb.requestKeyframe = 1;
        recFb.lastCompleteFrame = 3;
        recFb.lastGoodFrame = 2;
        recFb.queueDelayMs = 2.0f;
        recFb.fragmentsReceived = 100;
        sender.OnFeedback(recFb);
        CHECK(sender.WantsFrame() == true);
        pushFrame(false, true);  // frame 4 (recovery)
        CHECK(enc.invalidatedTimestamps.size() == 1 && enc.invalidatedTimestamps[0] == 2);

        CHECK(loop.WaitForAll(loop.sendConn->Stats().packetsSent, 4, 1500));
        {
            std::lock_guard lock(loop.mutex);
            CHECK(loop.frames.size() == 4);
            if (loop.frames.size() == 4) {
                CHECK((loop.frames[0].flags & rc::kVideoKeyframe) != 0);
                CHECK((loop.frames[3].flags & rc::kVideoRecovery) != 0);
                CHECK(loop.frames[3].bitstream.size() == encBytes);
            }
        }

        // Concurrent Send() + OnFeedback() switching FEC 5% -> 8% -> 12% -> 5%.
        std::thread fbThread([&] {
            rc::SleepMs(15);
            for (int i = 0; i < 2; i++) {
                rc::VideoFeedbackPayload fb{};
                fb.streamId = rc::kVrStreamId;
                fb.queueDelayMs = 2.0f;
                fb.fragmentsReceived = 75;
                fb.fragmentsLost = 25;
                sender.OnFeedback(fb);
            }
            rc::SleepMs(15);
            for (int i = 0; i < 2; i++) {
                rc::VideoFeedbackPayload fb{};
                fb.streamId = rc::kVrStreamId;
                fb.queueDelayMs = 2.0f;
                fb.fragmentsReceived = 50;
                fb.fragmentsLost = 50;
                sender.OnFeedback(fb);
            }
            rc::SleepMs(25);
            for (int i = 0; i < 12; i++) {
                rc::VideoFeedbackPayload fb{};
                fb.streamId = rc::kVrStreamId;
                fb.queueDelayMs = 2.0f;
                fb.fragmentsReceived = 100;
                fb.fragmentsLost = 0;
                sender.OnFeedback(fb);
                rc::SleepMs(2);
            }
        });

        constexpr int kConcFrames = 36;
        for (int i = 0; i < kConcFrames; i++) {
            pushFrame(false, false);
        }
        fbThread.join();

        const uint64_t totalSentPkts = loop.sendConn->Stats().packetsSent;
        CHECK(loop.WaitForAll(totalSentPkts, 4 + kConcFrames, 2000));
        CHECK(loop.recvConn->Stats().packetsReceived == totalSentPkts);
        CHECK(enc.bitrateUpdates.size() >= 2);
        CHECK(sender.Stats().lastFrameNs > 0);

        std::set<uint8_t> observedGroups;
        {
            std::lock_guard lock(loop.mutex);
            CHECK(loop.frames.size() == 4 + kConcFrames);
            for (uint32_t fi = 5; fi <= 4 + kConcFrames; fi++) {
                auto it = loop.wireStats.find(fi);
                CHECK(it != loop.wireStats.end());
                if (it != loop.wireStats.end()) {
                    const auto& ws = it->second;
                    CHECK(ws.fecGroups.size() == 1);
                    CHECK(ws.dataFrags == dataFragsPerFrame);
                    if (!ws.fecGroups.empty()) {
                        const uint8_t grp = *ws.fecGroups.begin();
                        observedGroups.insert(grp);
                        CHECK(ws.parityFrags == rc::VideoParityFragmentCount(dataFragsPerFrame, grp));
                    }
                }
            }
        }
        CHECK(observedGroups.count(20) == 1);  // base 5% FEC
        CHECK(observedGroups.count(8) == 1);   // escalated 12% FEC
        loop.Stop();
    }

    // (b) T-58 oversize (>2048 fragments) emits 0 datagrams and never pollutes WireCeiling with 0/enc.
    {
        SenderLoopbackPair smallLoop;
        CHECK(smallLoop.Open(256));
        smallLoop.Start();

        const size_t fragPayload = smallLoop.sendConn->MaxPayload() - maxHdr;
        rcs::VideoSender sender(smallLoop.sendConn.get(), rc::kVrStreamId, rc::VideoCodec::HEVC, 100'000,
                                /*fecPercent=*/20, /*adaptive=*/false);
        sender.SetPacingMode(3, 1'000'000);

        const size_t normalEncBytes = 40 * fragPayload - 2 - sizeof(meta);
        rcs::EncodedFrame normalFrame{};
        normalFrame.data.assign(normalEncBytes, 0x3A);
        for (int i = 0; i < 3; i++) {
            normalFrame.keyframe = (i == 0);
            normalFrame.timestamp = uint64_t(i + 1);
            sender.Send(normalFrame, &meta, sizeof(meta), 0, 1.0, false);
        }
        const uint32_t wireCapBefore = sender.TargetBitrateKbps();
        CHECK(wireCapBefore >= 50'000 && wireCapBefore < 90'000);
        const uint64_t packetsBefore = smallLoop.sendConn->Stats().packetsSent;
        const uint64_t bytesBefore = smallLoop.sendConn->Stats().bytesSent;

        // 2049 fragments > rc::kMaxFragmentsPerFrame (2048).
        const size_t oversizeEncBytes = size_t(rc::kMaxFragmentsPerFrame) * fragPayload + 1 - 2 - sizeof(meta);
        rcs::EncodedFrame oversizeFrame{};
        oversizeFrame.keyframe = true;
        oversizeFrame.timestamp = 99;
        oversizeFrame.data.assign(oversizeEncBytes, 0x7F);
        sender.Send(oversizeFrame, &meta, sizeof(meta), 0, 1.0, false);

        CHECK(smallLoop.sendConn->Stats().packetsSent == packetsBefore);
        CHECK(smallLoop.sendConn->Stats().bytesSent == bytesBefore);
        CHECK(sender.TargetBitrateKbps() == wireCapBefore);

        // Send one more normal frame to confirm trailing WireCeiling window was not diluted by 0/enc.
        normalFrame.keyframe = false;
        normalFrame.timestamp = 100;
        sender.Send(normalFrame, &meta, sizeof(meta), 0, 1.0, false);
        CHECK(smallLoop.WaitForAll(smallLoop.sendConn->Stats().packetsSent, 4, 1500));
        CHECK(sender.TargetBitrateKbps() == wireCapBefore);
        smallLoop.Stop();
    }

    // (c) OnFeedback dynamic FEC ladder (4%/10% & fallback) + adaptive=false vs adaptive=true comparison.
    {
        SenderLoopbackPair loopC;
        CHECK(loopC.Open());
        loopC.Start();
        const size_t fragPayload = loopC.sendConn->MaxPayload() - maxHdr;
        const size_t probeEncBytes = 20 * fragPayload - 2 - sizeof(meta);
        rcs::EncodedFrame probeFrame{};
        probeFrame.data.assign(probeEncBytes, 0x22);

        rcs::VideoSender fecSender(loopC.sendConn.get(), rc::kVrStreamId, rc::VideoCodec::HEVC, 100'000,
                                   /*fecPercent=*/5, /*adaptive=*/true);
        fecSender.SetPacingMode(1, 0);

        auto sendProbeDelta = [&]() -> uint64_t {
            const uint64_t before = loopC.sendConn->Stats().packetsSent;
            probeFrame.timestamp++;
            fecSender.Send(probeFrame, &meta, sizeof(meta), 0, 0.5, false);
            return loopC.sendConn->Stats().packetsSent - before;
        };

        CHECK(sendProbeDelta() == 21);  // base 5% -> group 20 -> 20 + 1

        auto sendLossFb = [&](uint32_t rxFrags, uint32_t lostFrags) {
            rc::VideoFeedbackPayload fb{};
            fb.streamId = rc::kVrStreamId;
            fb.queueDelayMs = 2.0f;
            fb.fragmentsReceived = rxFrags;
            fb.fragmentsLost = lostFrags;
            fecSender.OnFeedback(fb);
        };

        // 0 * 0.8 + 0.25 * 0.2 = 0.05 (> 0.04) -> fec=8 -> group 12 -> 20 + 2 = 22
        sendLossFb(75, 25);
        CHECK(std::fabs(fecSender.Stats().lossAvg - 0.05) < 1e-6);
        CHECK(sendProbeDelta() == 22);

        // 0.05 * 0.8 + 0.25 * 0.2 = 0.09 (<= 0.10) -> stays fec=8 -> 22
        sendLossFb(75, 25);
        CHECK(std::fabs(fecSender.Stats().lossAvg - 0.09) < 1e-6);
        CHECK(sendProbeDelta() == 22);

        // 0.09 * 0.8 + 0.50 * 0.2 = 0.172 (> 0.10) -> fec=12 -> group 8 -> 20 + 3 = 23
        sendLossFb(50, 50);
        CHECK(std::fabs(fecSender.Stats().lossAvg - 0.172) < 1e-6);
        CHECK(sendProbeDelta() == 23);

        // 7 clean reports: 0.172 * 0.8^7 = 0.036071 <= 0.04 -> falls back to base 5% -> 21
        for (int i = 0; i < 7; i++) sendLossFb(100, 0);
        CHECK(fecSender.Stats().lossAvg <= 0.04);
        CHECK(sendProbeDelta() == 21);

        // Dual-mode comparison: adaptive=false vs adaptive=true.
        rcs::VideoSender fixedSender(loopC.sendConn.get(), rc::kVrStreamId, rc::VideoCodec::HEVC, 80'000, 5,
                                     /*adaptive=*/false);
        rcs::VideoSender adaptSender(loopC.sendConn.get(), rc::kVrStreamId, rc::VideoCodec::HEVC, 80'000, 5,
                                     /*adaptive=*/true);
        fixedSender.StartAt(20'000);  // no-op for adaptive=false
        adaptSender.StartAt(20'000);
        CHECK(fixedSender.TargetBitrateKbps() == 80'000);

        rc::VideoFeedbackPayload calmFb{};
        calmFb.streamId = rc::kVrStreamId;
        calmFb.queueDelayMs = 2.0f;
        calmFb.fragmentsReceived = 100;
        fixedSender.OnFeedback(calmFb);
        CHECK(fixedSender.TargetBitrateKbps() == 80'000);

        fixedSender.SetMaxBitrate(95'000);
        adaptSender.SetMaxBitrate(95'000);
        CHECK(fixedSender.TargetBitrateKbps() == 95'000);  // adaptive=false raises m_targetKbps directly
        CHECK(adaptSender.TargetBitrateKbps() == 80'000);  // adaptive=true keeps initial m_targetKbps when raising max

        fixedSender.SetMaxBitrate(60'000);
        adaptSender.SetMaxBitrate(60'000);
        CHECK(fixedSender.TargetBitrateKbps() == 60'000);
        CHECK(adaptSender.TargetBitrateKbps() == 60'000);

        // adaptive=false still protects against real congestion via QueueGuard & LossBrake.
        rc::VideoFeedbackPayload congestedFb{};
        congestedFb.streamId = rc::kVrStreamId;
        congestedFb.queueDelayMs = 200.0f;  // EMA 2*0.6 + 200*0.4 = 81.2 ms > dipAt (57 ms) -> 90% dip
        congestedFb.fragmentsReceived = 100;
        fixedSender.OnFeedback(congestedFb);
        CHECK(fixedSender.TargetBitrateKbps() == 54'000);  // QueueGuard 90% dip (54,000 of 60,000)

        rc::VideoFeedbackPayload heavyLossFb{};
        heavyLossFb.streamId = rc::kVrStreamId;
        heavyLossFb.queueDelayMs = 2.0f;
        heavyLossFb.fragmentsReceived = 50;
        heavyLossFb.fragmentsLost = 50;
        fixedSender.OnFeedback(heavyLossFb);
        CHECK(fixedSender.TargetBitrateKbps() == 42'000);  // LossBrake heavy cut (70% of 60,000)
        loopC.Stop();
    }

    // (d) Adversarial malformed/non-finite/overflow OnFeedback inputs must not poison state.
    {
        SenderLoopbackPair loopD;
        CHECK(loopD.Open());
        loopD.Start();
        const size_t fragPayload = loopD.sendConn->MaxPayload() - maxHdr;
        rcs::EncodedFrame probeFrame{};
        probeFrame.data.assign(20 * fragPayload - 2 - sizeof(meta), 0x11);

        const float kInf = std::numeric_limits<float>::infinity();
        const float kNaN = std::numeric_limits<float>::quiet_NaN();

        // d1: Fresh sender receives +inf queueDelayMs, NaN decodeMs, -inf networkMs, negative totalLatencyMs,
        //     and uint32_t overflow fragment counts (0xFFFFFFFFu + 2u == 1u if 32-bit).
        rcs::VideoSender freshSender(loopD.sendConn.get(), rc::kVrStreamId, rc::VideoCodec::HEVC, 100'000,
                                     /*fecPercent=*/5, /*adaptive=*/true);
        freshSender.SetPacingMode(1, 0);

        rc::VideoFeedbackPayload badFb{};
        badFb.streamId = rc::kVrStreamId;
        badFb.queueDelayMs = kInf;
        badFb.decodeMs = kNaN;
        badFb.networkMs = -kInf;
        badFb.totalLatencyMs = -5.0f;
        badFb.fragmentsReceived = 0xFFFFFFFFu;
        badFb.fragmentsLost = 2u;
        freshSender.OnFeedback(badFb);

        const rcs::VideoSenderStats fs = freshSender.Stats();
        CHECK(std::isfinite(fs.queueMs) && fs.queueMs == -1.0);
        CHECK(fs.lastFeedbackNs == 0);
        CHECK(std::isfinite(fs.clientDecodeMs) && fs.clientDecodeMs == 0.0);
        CHECK(std::isfinite(fs.clientLatencyMs) && fs.clientLatencyMs == 0.0);
        CHECK(std::isfinite(fs.lossAvg) && fs.lossAvg >= 0.0 && fs.lossAvg < 0.001);
        CHECK(freshSender.TargetBitrateKbps() == 100'000);

        // d2: Warm sender with valid baseline must preserve EMA and recover cleanly after +inf / NaN / -inf / overflow.
        rcs::VideoSender warmSender(loopD.sendConn.get(), rc::kVrStreamId, rc::VideoCodec::HEVC, 100'000,
                                    /*fecPercent=*/5, /*adaptive=*/true);
        warmSender.SetPacingMode(1, 0);

        rc::VideoFeedbackPayload goodFb{};
        goodFb.streamId = rc::kVrStreamId;
        goodFb.queueDelayMs = 4.0f;
        goodFb.decodeMs = 5.0f;
        goodFb.networkMs = 1.0f;
        goodFb.totalLatencyMs = 18.0f;
        goodFb.fragmentsReceived = 100;
        warmSender.OnFeedback(goodFb);
        CHECK(std::fabs(warmSender.Stats().queueMs - 4.0) < 1e-6);
        CHECK(warmSender.TargetBitrateKbps() == 100'000);

        rc::VideoFeedbackPayload infFb = badFb;
        infFb.decodeMs = kInf;
        infFb.totalLatencyMs = kNaN;
        warmSender.OnFeedback(infFb);
        {
            const auto ws = warmSender.Stats();
            CHECK(std::isfinite(ws.queueMs) && std::fabs(ws.queueMs - 4.0) < 1e-6);
            CHECK(std::isfinite(ws.clientDecodeMs) && ws.clientDecodeMs == 0.0);
            CHECK(std::isfinite(ws.clientLatencyMs) && ws.clientLatencyMs == 0.0);
            CHECK(std::isfinite(ws.lossAvg) && ws.lossAvg >= 0.0 && ws.lossAvg < 0.001);
            CHECK(warmSender.TargetBitrateKbps() == 100'000);
        }

        rc::VideoFeedbackPayload nanQueueFb = goodFb;
        nanQueueFb.queueDelayMs = kNaN;
        warmSender.OnFeedback(nanQueueFb);
        nanQueueFb.queueDelayMs = -kInf;
        warmSender.OnFeedback(nanQueueFb);
        CHECK(std::fabs(warmSender.Stats().queueMs - 4.0) < 1e-6);
        CHECK(warmSender.TargetBitrateKbps() == 100'000);

        // Subsequent valid report must advance queueMs EMA normally (4.0 * 0.6 + 2.0 * 0.4 = 3.2) and keep FEC 5%.
        rc::VideoFeedbackPayload recoverFb{};
        recoverFb.streamId = rc::kVrStreamId;
        recoverFb.queueDelayMs = 2.0f;
        recoverFb.decodeMs = 6.0f;
        recoverFb.networkMs = 1.5f;
        recoverFb.totalLatencyMs = 20.0f;
        recoverFb.fragmentsReceived = 100;
        warmSender.OnFeedback(recoverFb);
        {
            const auto ws = warmSender.Stats();
            CHECK(std::isfinite(ws.queueMs) && std::fabs(ws.queueMs - 3.2) < 1e-5);
            CHECK(std::fabs(ws.clientDecodeMs - 6.0) < 1e-5);
            CHECK(std::fabs(ws.clientLatencyMs - 20.0) < 1e-5);
            CHECK(warmSender.TargetBitrateKbps() == 100'000);
        }

        const uint64_t pktsBeforeProbe = loopD.sendConn->Stats().packetsSent;
        probeFrame.timestamp = 1;
        warmSender.Send(probeFrame, &meta, sizeof(meta), 0, 0.5, false);
        CHECK((loopD.sendConn->Stats().packetsSent - pktsBeforeProbe) == 21);  // still base 5% FEC, not 12%
        loopD.Stop();
    }

    std::printf("streamer VideoSender (burst WireCeiling, T-58 oversize, FEC ladder, dual-mode, adversarial feedback): ok\n");
}

static vr::HmdMatrix34_t MakeMatrixFromSnapshot(const rcvr::TrackingSnapshot& t) {
    const double qx = t.hmd.orientation[0];
    const double qy = t.hmd.orientation[1];
    const double qz = t.hmd.orientation[2];
    const double qw = t.hmd.orientation[3];
    const double pos[3] = {t.hmd.position[0], t.hmd.position[1], t.hmd.position[2]};

    vr::HmdMatrix34_t m{};
    m.m[0][0] = float(1.0 - 2.0 * (qy * qy + qz * qz));
    m.m[0][1] = float(2.0 * (qx * qy - qz * qw));
    m.m[0][2] = float(2.0 * (qx * qz + qy * qw));
    m.m[0][3] = float(pos[0]);

    m.m[1][0] = float(2.0 * (qx * qy + qz * qw));
    m.m[1][1] = float(1.0 - 2.0 * (qx * qx + qz * qz));
    m.m[1][2] = float(2.0 * (qy * qz - qx * qw));
    m.m[1][3] = float(pos[1]);

    m.m[2][0] = float(2.0 * (qx * qz - qy * qw));
    m.m[2][1] = float(2.0 * (qy * qz + qx * qw));
    m.m[2][2] = float(1.0 - 2.0 * (qx * qx + qy * qy));
    m.m[2][3] = float(pos[2]);

    return m;
}

static rcvr::TrackingSnapshot MakeSnapshot(uint64_t seq, float yawRad, float pitchRad = 0.0f, float px = 0.0f, float py = 0.0f, float pz = 0.0f) {
    rcvr::TrackingSnapshot s{};
    s.sequence = seq;
    s.hmd.position[0] = px;
    s.hmd.position[1] = py;
    s.hmd.position[2] = pz;

    const float cy = std::cos(yawRad * 0.5f);
    const float sy = std::sin(yawRad * 0.5f);
    const float cp = std::cos(pitchRad * 0.5f);
    const float sp = std::sin(pitchRad * 0.5f);

    s.hmd.orientation[0] = sp * cy;
    s.hmd.orientation[1] = cp * sy;
    s.hmd.orientation[2] = -sp * sy;
    s.hmd.orientation[3] = cp * cy;
    return s;
}

void TestLayerReprojection() {
    // 1. 单层直通测试：layers.size() == 1 时，targetIdx == 0，ComputeLayerReproj 返回 false 且矩阵为单位阵。
    {
        rcvr::TrackingSnapshot s = MakeSnapshot(100, 0.05f, 0.02f, 0.1f, 0.2f, 0.3f);
        std::deque<rcvr::TrackingSnapshot> history = {s};
        vr::HmdMatrix34_t pose0 = MakeMatrixFromSnapshot(s);
        std::vector<vr::HmdMatrix34_t> layers = {pose0};

        auto res = rcvr::SelectCompositeTargetLayer(layers, history);
        CHECK(res.targetIdx == 0);
        CHECK(res.targetTracking.sequence == 100);

        float reproj[9];
        bool needReproj = rcvr::ComputeLayerReproj(layers[0], res.targetPose, reproj);
        CHECK(!needReproj);
        CHECK(std::fabs(reproj[0] - 1.0f) < 1e-6f);
        CHECK(std::fabs(reproj[4] - 1.0f) < 1e-6f);
        CHECK(std::fabs(reproj[8] - 1.0f) < 1e-6f);
        CHECK(std::fabs(reproj[1]) < 1e-6f && std::fabs(reproj[2]) < 1e-6f && std::fabs(reproj[3]) < 1e-6f);
        CHECK(std::fabs(reproj[5]) < 1e-6f && std::fabs(reproj[6]) < 1e-6f && std::fabs(reproj[7]) < 1e-6f);
    }

    // 2. 多层选最新层测试：layers[0] 匹配 history 中旧样本 seq=10，layers[1] 匹配新样本 seq=12
    //    → 选中 targetIdx == 1；layers[1] 直通（false），layers[0] 开启重投影（true）。
    {
        rcvr::TrackingSnapshot s10 = MakeSnapshot(10, 0.02f, 0.0f, 0.0f, 0.0f, 0.0f);
        rcvr::TrackingSnapshot s12 = MakeSnapshot(12, 0.08f, 0.0f, 0.0f, 0.0f, 0.0f);
        std::deque<rcvr::TrackingSnapshot> history = {s10, s12};
        vr::HmdMatrix34_t pose0 = MakeMatrixFromSnapshot(s10);
        vr::HmdMatrix34_t pose1 = MakeMatrixFromSnapshot(s12);
        std::vector<vr::HmdMatrix34_t> layers = {pose0, pose1};

        auto res = rcvr::SelectCompositeTargetLayer(layers, history);
        CHECK(res.targetIdx == 1);
        CHECK(res.targetTracking.sequence == 12);

        float reproj1[9];
        bool needReproj1 = rcvr::ComputeLayerReproj(layers[1], res.targetPose, reproj1);
        CHECK(!needReproj1);

        float reproj0[9];
        bool needReproj0 = rcvr::ComputeLayerReproj(layers[0], res.targetPose, reproj0);
        CHECK(needReproj0);
    }

    // 3. 游戏停帧掉出历史窗口测试：layers[0] 姿态已不在 32 条 history 内（matched == false），
    //    layers[1] 匹配 seq=50 → 仍稳定选中 targetIdx == 1，不乱跳。
    {
        std::deque<rcvr::TrackingSnapshot> history;
        for (uint64_t seq = 20; seq <= 51; seq++) {
            history.push_back(MakeSnapshot(seq, float(seq) * 0.001f, 0.0f, 0.0f, 0.0f, 0.0f));
        }
        CHECK(history.size() == 32);

        rcvr::TrackingSnapshot sOld = MakeSnapshot(5, 0.5f, 0.0f, 10.0f, 0.0f, 0.0f);
        vr::HmdMatrix34_t pose0 = MakeMatrixFromSnapshot(sOld);
        vr::HmdMatrix34_t pose1 = MakeMatrixFromSnapshot(history[30]); // seq=50

        std::vector<vr::HmdMatrix34_t> layers = {pose0, pose1};
        auto res = rcvr::SelectCompositeTargetLayer(layers, history);
        CHECK(res.targetIdx == 1);
        CHECK(res.targetTracking.sequence == 50);

        std::vector<vr::HmdMatrix34_t> layersBothDropped = {pose0, pose0};
        auto resDropped = rcvr::SelectCompositeTargetLayer(layersBothDropped, history);
        CHECK(resDropped.targetIdx == 1);
        CHECK(resDropped.targetTracking.sequence == 51);
    }

    // 4. 旋转方向符号测试：
    //    头显向右转（绕 +Y 顺时针 -theta）：ReprojectEyeUv(0.5, 0.5, ...) 输出 outU > 0.5（采样旧帧右侧）、outV ≈ 0.5。
    //    头显向上抬（绕 +X 逆时针 +theta）：输出 outV < 0.5（采样旧帧上方）、outU ≈ 0.5。
    //    大角度转出视野边界时返回 false（触发越界填黑/透明）。
    {
        const float fovTan[4] = {1.0f, 1.0f, 1.0f, 1.0f};

        // 头显向右转
        rcvr::TrackingSnapshot sLayer = MakeSnapshot(1, 0.0f, 0.0f);
        rcvr::TrackingSnapshot sTargetRight = MakeSnapshot(2, -0.1f, 0.0f);
        vr::HmdMatrix34_t mLayer = MakeMatrixFromSnapshot(sLayer);
        vr::HmdMatrix34_t mTargetRight = MakeMatrixFromSnapshot(sTargetRight);

        float reprojRight[9];
        bool enRight = rcvr::ComputeLayerReproj(mLayer, mTargetRight, reprojRight);
        CHECK(enRight);

        float outU = 0.0f, outV = 0.0f;
        bool okRight = rcvr::ReprojectEyeUv(0.5f, 0.5f, fovTan, reprojRight, outU, outV);
        CHECK(okRight);
        CHECK(outU > 0.5f);
        CHECK(std::fabs(outV - 0.5f) < 1e-4f);

        // 头显向上抬
        rcvr::TrackingSnapshot sTargetUp = MakeSnapshot(3, 0.0f, 0.1f);
        vr::HmdMatrix34_t mTargetUp = MakeMatrixFromSnapshot(sTargetUp);

        float reprojUp[9];
        bool enUp = rcvr::ComputeLayerReproj(mLayer, mTargetUp, reprojUp);
        CHECK(enUp);

        bool okUp = rcvr::ReprojectEyeUv(0.5f, 0.5f, fovTan, reprojUp, outU, outV);
        CHECK(okUp);
        CHECK(outV < 0.5f);
        CHECK(std::fabs(outU - 0.5f) < 1e-4f);

        // 大角度转出视野边界
        rcvr::TrackingSnapshot sTargetHuge = MakeSnapshot(4, -1.2f, 0.0f);
        vr::HmdMatrix34_t mTargetHuge = MakeMatrixFromSnapshot(sTargetHuge);
        float reprojHuge[9];
        rcvr::ComputeLayerReproj(mLayer, mTargetHuge, reprojHuge);
        bool okHuge = rcvr::ReprojectEyeUv(0.5f, 0.5f, fovTan, reprojHuge, outU, outV);
        CHECK(!okHuge);
    }

    // 5. 非对称 FOV + OpenGL 翻转 bounds 测试：
    //    使用非对称 fovTan = {1.0, 1.2, 0.97, 1.43} 与 vMin=1.0, vMax=0.0，
    //    验证零旋转恒等映射及小角度旋转映射数值精确。
    {
        const float fovTan[4] = {1.0f, 1.2f, 0.97f, 1.43f};
        const float identityM[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};

        for (float u : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
            for (float v : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
                float outU = 0.0f, outV = 0.0f;
                bool ok = rcvr::ReprojectEyeUv(u, v, fovTan, identityM, outU, outV);
                CHECK(ok);
                CHECK(std::fabs(outU - u) < 1e-6f);
                CHECK(std::fabs(outV - v) < 1e-6f);

                float texV = (1.0f - outV);
                CHECK(std::fabs(texV - (1.0f - v)) < 1e-6f);
            }
        }

        rcvr::TrackingSnapshot sLayer = MakeSnapshot(1, 0.0f, 0.0f);
        rcvr::TrackingSnapshot sTarget = MakeSnapshot(2, -0.01f, 0.0f);
        vr::HmdMatrix34_t mLayer = MakeMatrixFromSnapshot(sLayer);
        vr::HmdMatrix34_t mTarget = MakeMatrixFromSnapshot(sTarget);
        float reproj[9];
        rcvr::ComputeLayerReproj(mLayer, mTarget, reproj);

        float outU = 0.0f, outV = 0.0f;
        bool ok = rcvr::ReprojectEyeUv(0.5f, 0.5f, fovTan, reproj, outU, outV);
        CHECK(ok);
        CHECK(outU > 0.5f && outU < 0.6f);
        CHECK(std::fabs(outV - 0.5f) < 1e-3f);
    }

    // 6. HLSL 编译门禁：用 D3DCompile 实际编译更新后的 kBlitShader（vs_5_0 和 ps_5_0），
    //    要求返回 S_OK 且无任何编译错误或导数控制流警告。
    {
        ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;
        HRESULT hrVs = D3DCompile(rcvr::kBlitShader, std::strlen(rcvr::kBlitShader), "kBlitShader", nullptr, nullptr,
                                  "vs", "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS, 0, &vsBlob, &errBlob);
        if (FAILED(hrVs) && errBlob) {
            std::printf("VS compile error: %s\n", (char*)errBlob->GetBufferPointer());
        }
        CHECK(SUCCEEDED(hrVs));
        CHECK(vsBlob != nullptr);

        errBlob.Reset();
        HRESULT hrPs = D3DCompile(rcvr::kBlitShader, std::strlen(rcvr::kBlitShader), "kBlitShader", nullptr, nullptr,
                                  "ps", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS, 0, &psBlob, &errBlob);
        if (FAILED(hrPs) && errBlob) {
            std::printf("PS compile error: %s\n", (char*)errBlob->GetBufferPointer());
        }
        CHECK(SUCCEEDED(hrPs));
        CHECK(psBlob != nullptr);
    }

    std::printf("layer reprojection (single passthrough, multi latest, drop history, rotation signs, asymmetric fov/gl, D3DCompile): ok\n");
}

void Test10BitPipelineDitherStereoAndSrgb() {
    ComPtr<ID3D11DeviceContext> ctx;
    auto dev = CreateHardwareOrWarpDevice(ctx);
    CHECK(dev != nullptr);
    if (!dev) return;

    // 1. Foveator 10-bit (R10G10B10A2_UNORM) -> ColorConverter P010
    constexpr uint32_t eyeW = 128, eyeH = 64, fullW = eyeW * 2;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = fullW;
    td.Height = eyeH;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    auto pack10 = [](uint32_t r, uint32_t g, uint32_t b, uint32_t a) -> uint32_t {
        return (a << 30) | (b << 20) | (g << 10) | r;
    };
    std::vector<uint32_t> px10(size_t(fullW) * eyeH);
    for (uint32_t y = 0; y < eyeH; y++) {
        for (uint32_t x = 0; x < fullW; x++) {
            px10[size_t(y) * fullW + x] = (x < eyeW) ? pack10(512, 512, 512, 3) : pack10(0, 1023, 0, 3);
        }
    }
    D3D11_SUBRESOURCE_DATA init10{px10.data(), fullW * 4, 0};
    ComPtr<ID3D11Texture2D> src10;
    CHECK(SUCCEEDED(dev->CreateTexture2D(&td, &init10, &src10)));

    rcs::Foveator fov10;
    const uint32_t fovEyeW = rcs::Foveator::EncodedSize(eyeW, 0.45f);
    const uint32_t fovEyeH = rcs::Foveator::EncodedSize(eyeH, 0.45f);
    CHECK(fov10.Init(dev.Get(), eyeW, eyeH, fovEyeW, fovEyeH, 0.45f, true));
    D3D11_TEXTURE2D_DESC fovDesc{};
    fov10.Output()->GetDesc(&fovDesc);
    CHECK(fovDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM);
    const float starts[2][2] = {{0.275f, 0.275f}, {0.275f, 0.275f}};
    CHECK(fov10.Squeeze(ctx.Get(), src10.Get(), starts));

    rcs::ColorConverter conv10;
    CHECK(conv10.Init(dev.Get(), fovEyeW * 2, fovEyeH, true, true));
    CHECK(conv10.Convert(ctx.Get(), fov10.Output()));
    D3D11_TEXTURE2D_DESC p010Desc{};
    conv10.Output()->GetDesc(&p010Desc);
    CHECK(p010Desc.Format == DXGI_FORMAT_P010);

    // 2. ColorConverter 8-bit + dithering + stereo UV eye-seam clamp
    rcs::ColorConverter conv8;
    CHECK(conv8.Init(dev.Get(), fullW, eyeH, false, true));
    CHECK(conv8.Convert(ctx.Get(), src10.Get()));
    D3D11_TEXTURE2D_DESC nv12Desc{};
    conv8.Output()->GetDesc(&nv12Desc);
    nv12Desc.Usage = D3D11_USAGE_STAGING;
    nv12Desc.BindFlags = 0;
    nv12Desc.MiscFlags = 0;
    nv12Desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging8;
    CHECK(SUCCEEDED(dev->CreateTexture2D(&nv12Desc, nullptr, &staging8)));
    ctx->CopyResource(staging8.Get(), conv8.Output());
    ctx->Flush();
    D3D11_MAPPED_SUBRESOURCE map8{};
    CHECK(SUCCEEDED(ctx->Map(staging8.Get(), 0, D3D11_MAP_READ, 0, &map8)));
    const uint8_t* uvRow = static_cast<const uint8_t*>(map8.pData) + size_t(eyeH + eyeH / 4) * map8.RowPitch;
    // Last chroma pair of left eye (must stay neutral gray ~128, not bled by right eye green)
    const int leftEdgeU = uvRow[(eyeW - 2)];
    const int leftEdgeV = uvRow[(eyeW - 2) + 1];
    // First chroma pair of right eye (must stay green < 60, not bled by left eye gray)
    const int rightEdgeU = uvRow[eyeW];
    const int rightEdgeV = uvRow[eyeW + 1];
    ctx->Unmap(staging8.Get(), 0);
    if (!(leftEdgeU >= 124 && leftEdgeU <= 132 && leftEdgeV >= 124 && leftEdgeV <= 132 &&
          rightEdgeU < 60 && rightEdgeV < 60)) {
        std::printf("DEBUG UV seam: left=(%d,%d) right=(%d,%d)\n", leftEdgeU, leftEdgeV, rightEdgeU, rightEdgeV);
    }
    CHECK(leftEdgeU >= 124 && leftEdgeU <= 132 && leftEdgeV >= 124 && leftEdgeV <= 132);
    CHECK(rightEdgeU < 60 && rightEdgeV < 60);

    // 3. kBlitShader on R10G10B10A2_UNORM with reproj2.w = 1.0 (LinearToSrgb)
    ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;
    CHECK(SUCCEEDED(D3DCompile(rcvr::kBlitShader, std::strlen(rcvr::kBlitShader), "kBlitShader", nullptr, nullptr,
                               "vs", "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS, 0, &vsBlob, &errBlob)));
    CHECK(SUCCEEDED(D3DCompile(rcvr::kBlitShader, std::strlen(rcvr::kBlitShader), "kBlitShader", nullptr, nullptr,
                               "ps", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS, 0, &psBlob, &errBlob)));
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    CHECK(SUCCEEDED(dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs)));
    CHECK(SUCCEEDED(dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps)));

    D3D11_TEXTURE2D_DESC ftd{};
    ftd.Width = 4; ftd.Height = 4; ftd.MipLevels = ftd.ArraySize = 1;
    ftd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    ftd.SampleDesc.Count = 1;
    ftd.Usage = D3D11_USAGE_DEFAULT;
    ftd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    // Float16(0.2) = 0x3266
    std::vector<uint16_t> f16(4 * 4 * 4, 0x3266);
    D3D11_SUBRESOURCE_DATA fInit{f16.data(), 4 * 8, 0};
    ComPtr<ID3D11Texture2D> fSrc;
    CHECK(SUCCEEDED(dev->CreateTexture2D(&ftd, &fInit, &fSrc)));
    ComPtr<ID3D11ShaderResourceView> fSrv;
    CHECK(SUCCEEDED(dev->CreateShaderResourceView(fSrc.Get(), nullptr, &fSrv)));

    D3D11_TEXTURE2D_DESC otd{};
    otd.Width = 4; otd.Height = 4; otd.MipLevels = otd.ArraySize = 1;
    otd.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    otd.SampleDesc.Count = 1;
    otd.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> oDst;
    CHECK(SUCCEEDED(dev->CreateTexture2D(&otd, nullptr, &oDst)));
    ComPtr<ID3D11RenderTargetView> oRtv;
    CHECK(SUCCEEDED(dev->CreateRenderTargetView(oDst.Get(), nullptr, &oRtv)));

    rcvr::LayerConstants lc{};
    lc.rect[2] = 1.0f; lc.rect[3] = 1.0f;
    lc.reproj0[0] = 1.0f; lc.reproj1[1] = 1.0f; lc.reproj2[2] = 1.0f; lc.reproj2[3] = 1.0f;
    D3D11_BUFFER_DESC cbd{sizeof(lc), D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    D3D11_SUBRESOURCE_DATA cInit{&lc, 0, 0};
    ComPtr<ID3D11Buffer> cb;
    CHECK(SUCCEEDED(dev->CreateBuffer(&cbd, &cInit, &cb)));
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ComPtr<ID3D11SamplerState> samp;
    CHECK(SUCCEEDED(dev->CreateSamplerState(&sd, &samp)));

    D3D11_VIEWPORT vp{0, 0, 4, 4, 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->OMSetRenderTargets(1, oRtv.GetAddressOf(), nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs.Get(), nullptr, 0);
    ctx->PSSetShader(ps.Get(), nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, cb.GetAddressOf());
    ctx->PSSetShaderResources(0, 1, fSrv.GetAddressOf());
    ctx->PSSetSamplers(0, 1, samp.GetAddressOf());
    ctx->Draw(3, 0);

    otd.Usage = D3D11_USAGE_STAGING;
    otd.BindFlags = 0;
    otd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> oStg;
    CHECK(SUCCEEDED(dev->CreateTexture2D(&otd, nullptr, &oStg)));
    ctx->CopyResource(oStg.Get(), oDst.Get());
    D3D11_MAPPED_SUBRESOURCE oMap{};
    CHECK(SUCCEEDED(ctx->Map(oStg.Get(), 0, D3D11_MAP_READ, 0, &oMap)));
    const uint32_t outPx = *static_cast<const uint32_t*>(oMap.pData);
    ctx->Unmap(oStg.Get(), 0);
    const float outR = float(outPx & 0x3FFu) / 1023.0f;
    const float expectedSrgb = 1.055f * std::pow(0.2f, 1.0f / 2.4f) - 0.055f;
    CHECK(std::fabs(outR - expectedSrgb) < 2e-3f);
    std::printf("streamer D3D11 10-bit pipeline, stereo UV clamp, LinearToSrgb (< 2e-3): ok\n");
}

void TestSlicedEncodingConfigAndNvencIdr() {
    // 1. Config synthesis gating: >= 40 Mbps selects 4 HEVC or 2 AV1 slices; otherwise 1.
    {
        rcs::Settings pc;
        rcs::ClientStreamSettings st;
        pc.vrBitrateMbps = 100;
        pc.pcBitrateWins = true;
        pc.vrSlicedEncoding = false;
        auto bOff = rcs::BuildVrStreamConfig(pc, st, rc::VideoCodec::HEVC, false, false, 100, 1280, 1280, 72.f);
        CHECK(bOff.cfg.sliceCount == 1);

        pc.vrSlicedEncoding = true;
        auto bHevc = rcs::BuildVrStreamConfig(pc, st, rc::VideoCodec::HEVC, false, false, 100, 1280, 1280, 72.f);
        CHECK(bHevc.cfg.sliceCount == 4);

        auto bAv1 = rcs::BuildVrStreamConfig(pc, st, rc::VideoCodec::AV1, false, false, 100, 1280, 1280, 72.f);
        CHECK(bAv1.cfg.sliceCount == 2);

        pc.vrBitrateMbps = 30;
        auto bLow = rcs::BuildVrStreamConfig(pc, st, rc::VideoCodec::HEVC, false, false, 100, 1280, 1280, 72.f);
        CHECK(bLow.cfg.sliceCount == 1);
    }

    // 2. Real GPU NVENC 4-slice HEVC IDR NAL verification (if NVIDIA hardware adapter is present).
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return;
    ComPtr<IDXGIAdapter1> nvAdapter;
    for (UINT i = 0;; i++) {
        ComPtr<IDXGIAdapter1> a;
        if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{};
        a->GetDesc1(&desc);
        if (desc.VendorId == 0x10DE && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            nvAdapter = a;
            break;
        }
    }
    if (!nvAdapter) {
        std::printf("streamer sliced encoding (config ok, NVENC hardware test skipped: no NVIDIA GPU)\n");
        return;
    }
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    if (FAILED(D3D11CreateDevice(nvAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
                                 D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
        return;
    }

    constexpr uint32_t kW = 1280, kH = 720;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = kW; td.Height = kH; td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    std::vector<uint8_t> rgba(size_t(kW) * kH * 4, 128);
    D3D11_SUBRESOURCE_DATA init{rgba.data(), kW * 4, 0};
    ComPtr<ID3D11Texture2D> srcTex;
    CHECK(SUCCEEDED(dev->CreateTexture2D(&td, &init, &srcTex)));

    rcs::ColorConverter conv;
    CHECK(conv.Init(dev.Get(), kW, kH, false, false));
    CHECK(conv.Convert(ctx.Get(), srcTex.Get()));
    ctx->Flush();

    auto countHevcIdrSlices = [](const std::vector<uint8_t>& bs) -> std::pair<int, int> {
        int vclSlices = 0, paramSets = 0;
        for (size_t i = 0; i + 4 < bs.size(); i++) {
            size_t hdr = 0;
            if (bs[i] == 0 && bs[i + 1] == 0 && bs[i + 2] == 1) hdr = i + 3;
            else if (i + 5 < bs.size() && bs[i] == 0 && bs[i + 1] == 0 && bs[i + 2] == 0 && bs[i + 3] == 1) hdr = i + 4;
            if (!hdr) continue;
            const int nalType = (bs[hdr] >> 1) & 0x3F;
            if (nalType >= 32 && nalType <= 34) paramSets++;
            if (nalType == 19 || nalType == 20) vclSlices++;
            i = hdr;
        }
        return {paramSets, vclSlices};
    };

    {
        rcs::EncoderConfig ec;
        ec.codec = rc::VideoCodec::HEVC;
        ec.width = kW; ec.height = kH; ec.fps = 72.f;
        ec.bitrateKbps = 80000;
        ec.inputFormat = DXGI_FORMAT_NV12;
        ec.sliceCount = 4;
        ec.splitFrame = false;
        auto enc = rcs::CreateEncoder(dev.Get(), 0x10DE, ec);
        CHECK(enc != nullptr);
        if (enc) {
            rcs::EncodedFrame frame;
            CHECK(enc->Encode(conv.Output(), true, frame));
            CHECK(frame.keyframe);
            const auto [paramSets, slices] = countHevcIdrSlices(frame.data);
            CHECK(paramSets >= 3);
            CHECK(slices == 4);
        }
    }
    {
        // < 40 Mbps must fall back to single slice even if sliceCount == 4 is passed.
        rcs::EncoderConfig ec;
        ec.codec = rc::VideoCodec::HEVC;
        ec.width = kW; ec.height = kH; ec.fps = 72.f;
        ec.bitrateKbps = 30000;
        ec.inputFormat = DXGI_FORMAT_NV12;
        ec.sliceCount = 4;
        ec.splitFrame = false;
        auto enc = rcs::CreateEncoder(dev.Get(), 0x10DE, ec);
        CHECK(enc != nullptr);
        if (enc) {
            rcs::EncodedFrame frame;
            CHECK(enc->Encode(conv.Output(), true, frame));
            const auto [paramSets, slices] = countHevcIdrSlices(frame.data);
            CHECK(paramSets >= 3);
            CHECK(slices == 1);
        }
    }
    {
        if (!rcs::ProbeEncoderCaps(dev.Get(), 0x10DE).av1) {
            std::printf("streamer sliced encoding (config + NVENC HEVC checks ok, AV1 hardware test skipped: encoder reports no AV1)\n");
            return;
        }
        rcs::EncoderConfig ec;
        ec.codec = rc::VideoCodec::AV1;
        ec.width = kW; ec.height = kH; ec.fps = 72.f;
        ec.bitrateKbps = 80000;
        ec.inputFormat = DXGI_FORMAT_NV12;
        ec.sliceCount = 2;
        ec.splitFrame = false;
        auto enc = rcs::CreateEncoder(dev.Get(), 0x10DE, ec);
        CHECK(enc != nullptr);
        if (enc) {
            rcs::EncodedFrame frame;
            CHECK(enc->Encode(conv.Output(), true, frame));
            CHECK(frame.keyframe && !frame.data.empty());
        }
    }
    std::printf("streamer sliced encoding (config + NVENC 4-slice HEVC IDR NAL, AV1 encode & <40M single-slice fallback): ok\n");
}

static std::vector<uint8_t> BuildOscMsg(const std::string& addr, const std::string& tags, const std::vector<float>& floats) {
    auto pushStr = [](std::vector<uint8_t>& out, const std::string& s) {
        out.insert(out.end(), s.begin(), s.end());
        out.push_back(0);
        while (out.size() % 4 != 0) out.push_back(0);
    };
    auto pushBe32 = [](std::vector<uint8_t>& out, uint32_t v) {
        out.push_back(uint8_t((v >> 24) & 0xFF));
        out.push_back(uint8_t((v >> 16) & 0xFF));
        out.push_back(uint8_t((v >> 8) & 0xFF));
        out.push_back(uint8_t(v & 0xFF));
    };
    std::vector<uint8_t> msg;
    pushStr(msg, addr);
    pushStr(msg, tags);
    for (float f : floats) {
        uint32_t bits = 0;
        std::memcpy(&bits, &f, 4);
        pushBe32(msg, bits);
    }
    return msg;
}

static std::vector<uint8_t> BuildOscBundle(const std::vector<std::vector<uint8_t>>& elems) {
    std::vector<uint8_t> b = {'#', 'b', 'u', 'n', 'd', 'l', 'e', 0, 0, 0, 0, 0, 0, 0, 0, 1};
    for (const auto& e : elems) {
        const uint32_t sz = uint32_t(e.size());
        b.push_back(uint8_t((sz >> 24) & 0xFF));
        b.push_back(uint8_t((sz >> 16) & 0xFF));
        b.push_back(uint8_t((sz >> 8) & 0xFF));
        b.push_back(uint8_t(sz & 0xFF));
        b.insert(b.end(), e.begin(), e.end());
    }
    return b;
}

void TestOscEyeReceiverAndSettings() {
    // 1. Settings persistence & port clamp
    {
        rcs::Settings def;
        CHECK(!def.externalEyeTracking);
        CHECK(def.externalEyeOscPort == 9945);
        CHECK(!def.externalEyeInvertX);
        CHECK(!def.externalEyeInvertY);

        rcs::Settings s;
        s.FromJson(rcs::json{{"externalEyeTracking", true},
                             {"externalEyeOscPort", 0},
                             {"externalEyeInvertX", true},
                             {"externalEyeInvertY", true}});
        CHECK(s.externalEyeTracking);
        CHECK(s.externalEyeOscPort == 1024);
        CHECK(s.externalEyeInvertX);
        CHECK(s.externalEyeInvertY);

        s.FromJson(rcs::json{{"externalEyeOscPort", 99999}});
        CHECK(s.externalEyeOscPort == 65535);

        rcs::ClientStreamSettings st;
        st.gazeFoveation = true;
        rcs::Settings externalEyes;
        externalEyes.externalEyeTracking = true;
        auto externalInput = rcs::BuildAutoTuneInput(externalEyes, st, {});
        CHECK(!externalInput.foveaOn);
        CHECK(!externalInput.foveaPossible);

        rcs::Settings headsetEyes;
        auto headsetInput = rcs::BuildAutoTuneInput(headsetEyes, st, {});
        CHECK(headsetInput.foveaPossible);
    }

    // 2. OSC 1.0 coordinate conversion + invertX/Y
    {
        rcs::OscEyeParserState state;
        rc::GazePayload g{};
        std::string lastAddr;
        float nx = 0.0f, ny = 0.0f;
        bool updated = false;

        auto msg0 = BuildOscMsg("/tracking/eye/CenterPitchYaw", ",ff", {0.0f, 0.0f});
        CHECK(rcs::OscEyeReceiver::ParsePacket(msg0.data(), msg0.size(), false, false, state, g, lastAddr, nx, ny, updated));
        CHECK(updated);
        CHECK(std::fabs(g.gaze[0][0]) < 1e-5f && std::fabs(g.gaze[0][1]) < 1e-5f && std::fabs(g.gaze[0][2] + 1.0f) < 1e-5f);

        auto msgYaw = BuildOscMsg("/tracking/eye/LeftRightPitchYaw", ",ffff", {0.0f, 30.0f, 0.0f, 30.0f});
        CHECK(rcs::OscEyeReceiver::ParsePacket(msgYaw.data(), msgYaw.size(), false, false, state, g, lastAddr, nx, ny, updated));
        CHECK(updated && g.gaze[0][0] > 0.4f && g.gaze[0][2] < -0.5f);
        CHECK(rcs::OscEyeReceiver::ParsePacket(msgYaw.data(), msgYaw.size(), true, false, state, g, lastAddr, nx, ny, updated));
        CHECK(updated && g.gaze[0][0] < -0.4f);

        auto msgPitch = BuildOscMsg("/tracking/eye/CenterPitchYaw", ",ff", {20.0f, 0.0f});
        CHECK(rcs::OscEyeReceiver::ParsePacket(msgPitch.data(), msgPitch.size(), false, false, state, g, lastAddr, nx, ny, updated));
        CHECK(updated && g.gaze[0][1] < -0.3f);
        CHECK(rcs::OscEyeReceiver::ParsePacket(msgPitch.data(), msgPitch.size(), false, true, state, g, lastAddr, nx, ny, updated));
        CHECK(updated && g.gaze[0][1] > 0.3f);
    }

    // 3. LeftRightVec (+z flip), FT/v2 params, #bundle, blink hold, and malformed/NaN guards
    {
        rcs::OscEyeParserState state;
        rc::GazePayload g{};
        std::string lastAddr;
        float nx = 0.0f, ny = 0.0f;
        bool updated = false;

        auto vecMsg = BuildOscMsg("/tracking/eye/LeftRightVec", ",ffffff", {0.0f, 0.0f, 1.0f, 0.1f, 0.0f, 0.99f});
        CHECK(rcs::OscEyeReceiver::ParsePacket(vecMsg.data(), vecMsg.size(), false, false, state, g, lastAddr, nx, ny, updated));
        CHECK(updated && g.gaze[0][2] < -0.9f && g.gaze[1][2] < -0.9f);

        auto blinkOn = BuildOscMsg("/tracking/eye/EyesClosedAmount", ",f", {0.95f});
        CHECK(rcs::OscEyeReceiver::ParsePacket(blinkOn.data(), blinkOn.size(), false, false, state, g, lastAddr, nx, ny, updated));
        CHECK(!updated);
        auto heldGaze = BuildOscMsg("/tracking/eye/CenterPitchYaw", ",ff", {-30.0f, -30.0f});
        CHECK(rcs::OscEyeReceiver::ParsePacket(heldGaze.data(), heldGaze.size(), false, false, state, g, lastAddr, nx, ny, updated));
        CHECK(updated);  // held gaze must remain a live external signal
        CHECK(g.gaze[0][2] < -0.9f && std::fabs(g.gaze[0][0]) < 1e-5f &&
              std::fabs(g.gaze[1][0] - 0.1f) < 0.02f);
        CHECK(nx > 0.0f && std::fabs(ny) < 1e-5f);

        auto blinkOff = BuildOscMsg("/tracking/eye/EyesClosedAmount", ",f", {0.0f});
        CHECK(rcs::OscEyeReceiver::ParsePacket(blinkOff.data(), blinkOff.size(), false, false, state, g, lastAddr, nx, ny, updated));
        auto b = BuildOscBundle({
            BuildOscMsg("/avatar/parameters/FT/v2/EyeLeftX", ",f", {0.5f}),
            BuildOscMsg("/avatar/parameters/FT/v2/EyeLeftY", ",f", {0.5f}),
        });
        CHECK(rcs::OscEyeReceiver::ParsePacket(b.data(), b.size(), false, false, state, g, lastAddr, nx, ny, updated));
        CHECK(updated && g.gaze[0][0] > 0.2f && g.gaze[0][1] > 0.1f);

        // Malformed / truncated / NaN
        CHECK(!rcs::OscEyeReceiver::ParsePacket(nullptr, 0, false, false, state, g, lastAddr, nx, ny, updated));
        const uint8_t noNul[] = {'/', 't', 'e', 's', 't', 'a', 'b', 'c'};
        CHECK(!rcs::OscEyeReceiver::ParsePacket(noNul, sizeof(noNul), false, false, state, g, lastAddr, nx, ny, updated));
        auto nanMsg = BuildOscMsg("/tracking/eye/CenterPitchYaw", ",ff", {std::numeric_limits<float>::quiet_NaN(), 0.0f});
        CHECK(!rcs::OscEyeReceiver::ParsePacket(nanMsg.data(), nanMsg.size(), false, false, state, g, lastAddr, nx, ny, updated));
    }

    // 4. PaperTracker native 72-byte /papertracker/gaze/v1 (,iiiffffff)
    {
        auto buildPtV1 = [](uint32_t seq, uint32_t flags, int32_t ageUs, float ly, float lp, float ry, float rp, float fy, float fp) {
            std::vector<uint8_t> p = {'/', 'p', 'a', 'p', 'e', 'r', 't', 'r', 'a', 'c', 'k', 'e', 'r', '/', 'g', 'a', 'z', 'e',
                                      '/', 'v', '1', 0, 0, 0, ',', 'i', 'i', 'i', 'f', 'f', 'f', 'f', 'f', 'f', 0, 0};
            auto pushBe32 = [&](uint32_t v) {
                p.push_back(uint8_t((v >> 24) & 0xFF));
                p.push_back(uint8_t((v >> 16) & 0xFF));
                p.push_back(uint8_t((v >> 8) & 0xFF));
                p.push_back(uint8_t(v & 0xFF));
            };
            auto pushF = [&](float f) {
                uint32_t b = 0;
                std::memcpy(&b, &f, 4);
                pushBe32(b);
            };
            pushBe32(seq);
            pushBe32(flags);
            pushBe32(uint32_t(ageUs));
            pushF(ly); pushF(lp); pushF(ry); pushF(rp); pushF(fy); pushF(fp);
            return p;
        };
        rcs::OscEyeParserState state;
        rc::GazePayload g{};
        std::string lastAddr;
        float nx = 0.0f, ny = 0.0f;
        bool updated = false;
        auto pt1 = buildPtV1(1, 0x3u, 500, 30.0f, 20.0f, 30.0f, 20.0f, 30.0f, 20.0f);
        CHECK(pt1.size() == 72);
        CHECK(rcs::OscEyeReceiver::ParsePacket(pt1.data(), pt1.size(), false, false, state, g, lastAddr, nx, ny, updated));
        CHECK(updated && lastAddr == "/papertracker/gaze/v1");
        CHECK(g.gaze[0][0] > 0.4f && g.gaze[0][1] > 0.25f && g.gaze[0][2] < -0.5f);  // yaw right (+x), pitch up (+y)

        auto ptRepeated = buildPtV1(1, 0x3u, 500, -30.0f, -20.0f, -30.0f, -20.0f, -30.0f, -20.0f);
        CHECK(rcs::OscEyeReceiver::ParsePacket(ptRepeated.data(), ptRepeated.size(), false, false, state, g, lastAddr, nx, ny, updated));
        CHECK(!updated);
        CHECK(state.isRepeatedGaze);
        CHECK(g.gaze[0][0] > 0.4f && g.gaze[0][1] > 0.25f);

        // Test offset and gain adjustment
        {
            rcs::OscEyeParserState offsetState;
            rc::GazePayload og{};
            std::string oAddr;
            float onx = 0.f, ony = 0.f;
            bool oUpdated = false;
            auto ptOffset = buildPtV1(10, 0x3u, 500, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
            CHECK(rcs::OscEyeReceiver::ParsePacket(ptOffset.data(), ptOffset.size(), false, false, offsetState, og, oAddr, onx, ony, oUpdated,
                                                   5.0f, -3.0f, 1.2f, 1.0f));
            CHECK(oUpdated);
            CHECK(og.gaze[0][0] > 0.05f);  // yaw adjusted to +6.0 deg -> gaze[0][0] > 0
        }

        // Fused-only fallback when left/right valid bits are 0
        auto pt2 = buildPtV1(2, 0x4u, 500, 0.0f, 0.0f, 0.0f, 0.0f, -25.0f, -15.0f);
        CHECK(rcs::OscEyeReceiver::ParsePacket(pt2.data(), pt2.size(), false, false, state, g, lastAddr, nx, ny, updated));
        CHECK(updated && g.gaze[0][0] < -0.3f && g.gaze[0][1] < -0.2f);

        auto ptBlinkOn = BuildOscMsg("/tracking/eye/EyesClosedAmount", ",f", {0.95f});
        CHECK(rcs::OscEyeReceiver::ParsePacket(ptBlinkOn.data(), ptBlinkOn.size(), false, false, state, g, lastAddr, nx, ny, updated));
        CHECK(!updated);
        auto ptDuringBlink = buildPtV1(3, 0x3u, 500, 30.0f, 20.0f, 30.0f, 20.0f, 30.0f, 20.0f);
        CHECK(rcs::OscEyeReceiver::ParsePacket(ptDuringBlink.data(), ptDuringBlink.size(), false, false, state, g, lastAddr, nx, ny, updated));
        CHECK(updated);
        CHECK(g.gaze[0][0] < -0.3f && g.gaze[0][1] < -0.2f);  // native gaze must also hold the last valid blink gaze

        auto ptBlinkOff = BuildOscMsg("/tracking/eye/EyesClosedAmount", ",f", {0.0f});
        CHECK(rcs::OscEyeReceiver::ParsePacket(ptBlinkOff.data(), ptBlinkOff.size(), false, false, state, g, lastAddr, nx, ny, updated));
        auto ptAfterBlink = buildPtV1(4, 0x3u, 500, 30.0f, 20.0f, 30.0f, 20.0f, 30.0f, 20.0f);
        CHECK(rcs::OscEyeReceiver::ParsePacket(ptAfterBlink.data(), ptAfterBlink.size(), false, false, state, g, lastAddr, nx, ny, updated));
        CHECK(updated && g.gaze[0][0] > 0.4f && g.gaze[0][1] > 0.25f);

        rcs::OscEyeParserState zeroSequenceState;
        auto ptZeroFirst = buildPtV1(0, 0x3u, 500, 30.0f, 20.0f, 30.0f, 20.0f, 30.0f, 20.0f);
        CHECK(rcs::OscEyeReceiver::ParsePacket(ptZeroFirst.data(), ptZeroFirst.size(), false, false, zeroSequenceState, g, lastAddr, nx, ny, updated));
        CHECK(updated);
        auto ptZeroSecond = buildPtV1(0, 0x3u, 500, -30.0f, -20.0f, -30.0f, -20.0f, -30.0f, -20.0f);
        CHECK(rcs::OscEyeReceiver::ParsePacket(ptZeroSecond.data(), ptZeroSecond.size(), false, false, zeroSequenceState, g, lastAddr, nx, ny, updated));
        CHECK(updated && g.gaze[0][0] < -0.3f && g.gaze[0][1] < -0.2f);
    }
    std::printf("streamer OSC eye receiver (OSC 1.0, #bundle, blink hold, malformed guards, port clamp): ok\n");
}

}  // namespace

int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    InitIsolatedDataDir();
    TestPcSettingsAndPersistence();
    TestPcSettingsMalformedAndIsolated();
    TestVrBridgeHostAndClient();
    TestVrBridgeHostHapticsRaceAndBurst();
    TestRealGpuFoveatorAndColorConverter();
    TestRealGpuSharedTypelessVrTexture();
    TestRealGpuFoveatorHalfTexel();
    TestVrDiagMath();
    TestVideoSenderLiveAndFeedback();
    TestLayerReprojection();
    Test10BitPipelineDitherStereoAndSrgb();
    TestSlicedEncodingConfigAndNvencIdr();
    TestOscEyeReceiverAndSettings();
    CoUninitialize();

    std::error_code ec;
    std::filesystem::remove_all(rcs::DataDir(), ec);
    if (g_failures) {
        std::printf("FAILED: %d check(s)\n", g_failures);
        return 1;
    }
    std::printf("ALL STREAMER TESTS PASSED\n");
    return 0;
}
