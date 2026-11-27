#include "headset.h"

#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

#include <algorithm>
#include <cctype>

#include "pico/pico_extensions.h"
#include "rc/platform.h"

namespace rv {

std::string SystemProperty(const char* name) {
#ifdef __ANDROID__
    char value[PROP_VALUE_MAX] = {};
    __system_property_get(name, value);
    return value;
#else
    (void)name;
    return {};
#endif
}

std::vector<const char*> Headset::Extensions() {
    return {
        // Quest
        XR_FB_BODY_TRACKING_EXTENSION_NAME,
        XR_META_BODY_TRACKING_FULL_BODY_EXTENSION_NAME,
        XR_META_BODY_TRACKING_FIDELITY_EXTENSION_NAME,
        XR_FB_FACE_TRACKING2_EXTENSION_NAME,
        XR_FB_EYE_TRACKING_SOCIAL_EXTENSION_NAME,
        // PICO
        XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME,
        XR_BD_ULTRA_CONTROLLER_INTERACTION_EXTENSION_NAME,
        XR_BD_BODY_TRACKING_EXTENSION_NAME,
        XR_BD_BODY_TRACKING_AUXILIARY_METRICS_EXTENSION_NAME,
        XR_BD_FACIAL_SIMULATION_EXTENSION_NAME,
        kBdMotionTrackingExtension,
        kPicoConfigurationExtension,
        // HTC Vive
        XR_HTC_VIVE_FOCUS3_CONTROLLER_INTERACTION_EXTENSION_NAME,
        XR_HTC_HAND_INTERACTION_EXTENSION_NAME,
        // Play For Dream / YVR
        "XR_YVR_controller_interaction",
        // Android XR
        kAndroidXrMarkerExtension,
        // Any: where the eyes look, for foveated encoding
        XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME,
    };
}

std::vector<ControllerProfile> Headset::Controllers() const {
    // Return standard Touch-compatible layouts in order of preference.
    // XrInput gracefully binds whichever profile the runtime supports.
    return {
        {"/interaction_profiles/oculus/touch_controller", nullptr, true, true},
        {"/interaction_profiles/bytedance/pico_ultra_controller_bd", XR_BD_ULTRA_CONTROLLER_INTERACTION_EXTENSION_NAME, false, true},
        {"/interaction_profiles/bytedance/pico4_controller", XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME, false, true},
        {"/interaction_profiles/bytedance/pico_neo3_controller", XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME, false, true},
        {"/interaction_profiles/htc/vive_focus3_controller", XR_HTC_VIVE_FOCUS3_CONTROLLER_INTERACTION_EXTENSION_NAME, false, true},
        {"/interaction_profiles/yvr/touch_controller_yvr", "XR_YVR_controller_interaction", false, true},
        {"/interaction_profiles/htc/vive_controller", nullptr, false, true},
        {"/interaction_profiles/valve/index_controller", nullptr, false, true},
        {"/interaction_profiles/microsoft/motion_controller", nullptr, false, false},
    };
}

std::unique_ptr<Headset> Headset::Create(XrContext& xr) {
    if (xr.IsCompatPlainOpenXr()) {
        RC_LOGI("compatPlainOpenXr active: forcing generic standard OpenXR headset");
        return std::make_unique<Headset>();
    }
    std::string runtime = xr.RuntimeName();
    std::transform(runtime.begin(), runtime.end(), runtime.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    const auto has = [&](const char* s) { return runtime.find(s) != std::string::npos; };
    if (has("oculus") || has("meta")) return CreateQuestHeadset();
    if (has("pico") || has("bytedance") || xr.HasExtension(XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME)) return CreatePicoHeadset(xr);
    if (has("android") || xr.HasExtension(kAndroidXrMarkerExtension)) return CreateAndroidXrHeadset();
    if (has("htc") || has("vive") || xr.HasExtension(XR_HTC_VIVE_FOCUS3_CONTROLLER_INTERACTION_EXTENSION_NAME)) return CreateHtcHeadset();
    RC_LOGI("Unknown OpenXR runtime \"%s\": plain OpenXR only", xr.RuntimeName().c_str());
    return std::make_unique<Headset>();
}

}  // namespace rv
