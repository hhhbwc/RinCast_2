// HTC Vive standalone headsets (VIVE Focus 3, VIVE XR Elite, VIVE Focus Vision).
// Uses HTC's Focus 3 controller interaction profile, standard OpenXR hand tracking,
// and XR_EXT_eye_gaze_interaction for eye-tracked models (Focus Vision / Focus 3 Eye Tracker).
#include "../headset.h"

#include <algorithm>
#include <cctype>

#include "rc/platform.h"

namespace rv {

namespace {

class HtcHeadset : public Headset {
   public:
    const char* Family() const override { return "HTC Vive"; }
    const char* SettingsName() const override { return "VIVE settings"; }

    bool PanelSize(uint32_t& width, uint32_t& height) const override {
        std::string model = SystemProperty("ro.product.model");
        std::string device = SystemProperty("ro.product.device");
        std::transform(model.begin(), model.end(), model.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        std::transform(device.begin(), device.end(), device.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        const auto has = [&](const char* s) { return model.find(s) != std::string::npos || device.find(s) != std::string::npos; };
        if (has("focus 3") || has("focus3") || has("focus vision") || has("vrg_focus")) {
            width = 2448, height = 2448;
            return true;
        }
        if (has("xr elite") || has("xrelite") || has("xr_elite") || has("flow")) {
            width = 1920, height = 1920;
            return true;
        }
        return false;
    }

    std::vector<ControllerProfile> Controllers() const override {
        return {
            {"/interaction_profiles/htc/vive_focus3_controller", XR_HTC_VIVE_FOCUS3_CONTROLLER_INTERACTION_EXTENSION_NAME, false, true},
            {"/interaction_profiles/oculus/touch_controller", nullptr, true, true},
            {"/interaction_profiles/htc/vive_controller", nullptr, false, true},
            {"/interaction_profiles/valve/index_controller", nullptr, false, true},
            {"/interaction_profiles/microsoft/motion_controller", nullptr, false, false},
        };
    }

    std::vector<std::string> StartupPermissions() const override { return {"android.permission.HAND_TRACKING"}; }
    std::vector<std::string> GazePermissions() const override { return {"android.permission.EYE_TRACKING_FINE"}; }
};

}  // namespace

std::unique_ptr<Headset> CreateHtcHeadset() { return std::make_unique<HtcHeadset>(); }

}  // namespace rv
