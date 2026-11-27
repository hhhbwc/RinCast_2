#include "xr_loader.h"

#include <dlfcn.h>
#include <vector>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#else
#include <cstdlib>
#endif

#include <cstring>
#include <string>

#include "rc/platform.h"

#define RV_XR_DEFINE(name) PFN_##name name = nullptr;
RV_XR_DEFINE(xrGetInstanceProcAddr)
RV_XR_GLOBAL_FUNCTIONS(RV_XR_DEFINE)
RV_XR_INSTANCE_FUNCTIONS(RV_XR_DEFINE)
#undef RV_XR_DEFINE

namespace rv::xrloader {

namespace {

// Packaged in the APK (see CMakeLists.txt).
constexpr const char* kKhronos = "libopenxr_loader.so";
constexpr const char* kPico = "libopenxr_loader_pico.so";
constexpr const char* kQuest1 = "libopenxr_loader_quest1.so";  // only in the Quest 1 build
constexpr const char* kAndroidXr = "libopenxr.google.so";
std::string g_name = kKhronos;
void* g_lib = nullptr;

#ifdef __ANDROID__
std::string Property(const char* name) {
    char value[PROP_VALUE_MAX] = {};
    __system_property_get(name, value);
    return value;
}
#else
std::string Property(const char*) { return {}; }  // not a PICO or a Quest 1
#endif

template <typename T>
bool Get(XrInstance instance, const char* name, T& out) {
    PFN_xrVoidFunction f = nullptr;
    if (XR_FAILED(xrGetInstanceProcAddr(instance, name, &f)) || !f) {
        RC_LOGE("OpenXR: %s not found in %s", name, g_name.c_str());
        return false;
    }
    out = reinterpret_cast<T>(f);
    return true;
}

// PICO's loader on PICO headsets older than the PICO 4 Ultra.
[[maybe_unused]] bool UsePicoLoader() {
    // The headsets that shipped before PICO OS 5.9 brought the standard loader (as in ALVR).
    // PICO's own loader still works on them after updates, so these always get it; the PICO 4
    // Ultra and anything newer use the standard one. Chinese domestic units report codenames
    // instead of marketing names: the PICO 4 is "Phoenix" (model A8110).
    const std::string manufacturer = Property("ro.product.manufacturer");
    const std::string brand = Property("ro.product.brand");
    const bool picoBrand = (strcasecmp(manufacturer.c_str(), "pico") == 0 ||
                            strcasecmp(manufacturer.c_str(), "bytedance") == 0 ||
                            strcasecmp(brand.c_str(), "pico") == 0 ||
                            strcasecmp(brand.c_str(), "bytedance") == 0);
    if (!picoBrand) return false;
    const std::string model = Property("ro.product.model");
    const std::string product = Property("ro.product.name");
    RC_LOGI("PICO headset: manufacturer '%s', brand '%s', model '%s', product '%s'",
            manufacturer.c_str(), brand.c_str(), model.c_str(), product.c_str());
    if (model == "Pico Neo 3" || model == "Pico Neo3 Link") return true;
    if (model == "A8110" || model == "A8110 Pro") return true;
    return product == "PICO 4" || product == "PICO 4 Pro" || product == "PICO 4 Enterprise" || product == "PICO G3" ||
           product == "Phoenix" || product == "Phoenix Pro";
}

// Meta's loader on the Quest 1, whose Horizon OS stopped before the standard loader worked there.
[[maybe_unused]] bool IsQuest1() {
    return strcasecmp(Property("ro.product.manufacturer").c_str(), "oculus") == 0 &&
           Property("ro.product.device") == "monterey";
}

std::vector<std::string> BuildCandidates() {
    std::vector<std::string> candidates;
#ifndef __ANDROID__
    if (const char* env = std::getenv("RINCAST_OPENXR_LOADER"); env && *env) candidates.push_back(env);
    candidates.push_back("libopenxr_loader.so.1");
    candidates.push_back("libopenxr_loader.so");
    const std::string home = std::getenv("HOME") ? std::getenv("HOME") : "";
    for (const char* arch : {"linuxarm64", "linux64"}) {
        candidates.push_back(std::string("/opt/steamvr/bin/") + arch + "/libopenxr_loader.so");
        for (const char* steam : {"/.local/share/Steam", "/.steam/steam"})
            candidates.push_back(home + steam + "/steamapps/common/SteamVR/bin/" + arch + "/libopenxr_loader.so");
    }
#else
    if (IsQuest1()) {
        candidates.push_back(kQuest1);
        candidates.push_back(kKhronos);
        candidates.push_back("/system/lib64/libopenxr_loader.so");
        candidates.push_back("/vendor/lib64/libopenxr_loader.so");
    } else if (UsePicoLoader()) {
        candidates.push_back(kPico);
        candidates.push_back(kKhronos);
        candidates.push_back(kAndroidXr);
        candidates.push_back("/system/lib64/libopenxr_loader.so");
        candidates.push_back("/vendor/lib64/libopenxr_loader.so");
    } else {
        candidates.push_back(kKhronos);
        candidates.push_back(kPico);
        candidates.push_back(kAndroidXr);
        candidates.push_back("/system/lib64/libopenxr_loader.so");
        candidates.push_back("/vendor/lib64/libopenxr_loader.so");
    }
#endif
    return candidates;
}

const std::vector<std::string>& GetCandidates() {
    static const std::vector<std::string> s_candidates = BuildCandidates();
    return s_candidates;
}

}  // namespace

void Unload() {
    if (g_lib) {
        dlclose(g_lib);
        g_lib = nullptr;
    }
    xrGetInstanceProcAddr = nullptr;
#define RV_XR_CLEAR(name) name = nullptr;
    RV_XR_GLOBAL_FUNCTIONS(RV_XR_CLEAR)
    RV_XR_INSTANCE_FUNCTIONS(RV_XR_CLEAR)
#undef RV_XR_CLEAR
}

size_t CandidateCount() {
    return GetCandidates().size();
}

bool LoadCandidate(size_t index) {
    Unload();
    const auto& candidates = GetCandidates();
    if (index >= candidates.size()) return false;
    const std::string& target = candidates[index];
    g_lib = dlopen(target.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!g_lib) {
        RC_LOGW("OpenXR candidate [%zu/%zu] %s dlopen failed: %s",
                index + 1, candidates.size(), target.c_str(), dlerror());
        return false;
    }
    xrGetInstanceProcAddr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(dlsym(g_lib, "xrGetInstanceProcAddr"));
    if (!xrGetInstanceProcAddr) {
        RC_LOGW("OpenXR: %s has no xrGetInstanceProcAddr", target.c_str());
        Unload();
        return false;
    }
    g_name = target;
    RC_LOGI("OpenXR loader loaded: %s (candidate %zu/%zu)", g_name.c_str(), index + 1, candidates.size());
    bool ok = true;
#define RV_XR_GET(name) ok = Get(XR_NULL_HANDLE, #name, name) && ok;
    RV_XR_GLOBAL_FUNCTIONS(RV_XR_GET)
#undef RV_XR_GET
    if (!ok) {
        RC_LOGW("OpenXR candidate %s missing required global functions", g_name.c_str());
        Unload();
        return false;
    }
    return true;
}

bool Load() {
    if (xrGetInstanceProcAddr) return true;
    const size_t count = CandidateCount();
    for (size_t i = 0; i < count; i++) {
        if (LoadCandidate(i)) return true;
    }
    RC_LOGE("OpenXR: all loader candidates failed to load");
    return false;
}

bool LoadInstance(XrInstance instance) {
    bool ok = true;
#undef RV_XR_GET
#define RV_XR_GET(name) ok = Get(instance, #name, name) && ok;
    RV_XR_INSTANCE_FUNCTIONS(RV_XR_GET)
#undef RV_XR_GET
    return ok;
}

const char* Name() { return g_name.c_str(); }

}  // namespace rv::xrloader
