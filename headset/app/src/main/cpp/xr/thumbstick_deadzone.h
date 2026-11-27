#pragma once

#include <algorithm>
#include <cmath>

#include "../gfx/math.h"

namespace rv {

inline constexpr float kMaxStickDeadzone = 1.0f;
inline constexpr float kMaxStickCalibrationDrift = 0.40f;
inline constexpr float kStickCalibrationMargin = 0.03f;
inline constexpr int kStickCalibrationFrames = 45;

inline float ClampStickDeadzone(float deadzone) {
    return std::isfinite(deadzone) ? std::clamp(deadzone, 0.f, kMaxStickDeadzone) : 0.f;
}

inline double StickMagnitude(Vec2 raw) {
    return std::hypot(static_cast<double>(raw.x), static_cast<double>(raw.y));
}

inline Vec2 ApplyThumbstickDeadzone(Vec2 raw, float deadzone) {
    if (!std::isfinite(raw.x) || !std::isfinite(raw.y)) return {};
    const float d = ClampStickDeadzone(deadzone);
    const double r = StickMagnitude(raw);
    if (r <= 1e-6 || r <= d) return {};
    if (d == 0.f) {
        if (r <= 1.0) return raw;
        const double gain = 1.0 / r;
        return {static_cast<float>(raw.x * gain), static_cast<float>(raw.y * gain)};
    }
    const double scaled = std::min((r - d) / (1.0 - d), 1.0);
    const double gain = scaled / r;
    return {static_cast<float>(raw.x * gain), static_cast<float>(raw.y * gain)};
}

inline float SuggestDeadzoneForDrift(Vec2 raw, float margin = kStickCalibrationMargin) {
    if (!std::isfinite(raw.x) || !std::isfinite(raw.y)) return 0.f;
    const float safeMargin = std::isfinite(margin) ? std::clamp(margin, 0.f, 0.20f) : 0.f;
    const double r = StickMagnitude(raw);
    if (r < 0.005) return 0.f;
    const double target = std::ceil((r + safeMargin) * 100.0 - 1e-9) / 100.0;
    return static_cast<float>(std::clamp(target, 0.0, static_cast<double>(kMaxStickCalibrationDrift)));
}

struct StickDeadzoneCalibration {
    bool active = false;
    bool invalid = false;
    int samples = 0;
    double sumX = 0.0;
    double sumY = 0.0;
    float peakMagnitude = 0.f;

    void Start() {
        active = true;
        invalid = false;
        samples = 0;
        sumX = sumY = 0.0;
        peakMagnitude = 0.f;
    }

    bool Observe(Vec2 raw) {
        if (!active) return false;
        if (!std::isfinite(raw.x) || !std::isfinite(raw.y)) {
            active = false;
            invalid = true;
            return false;
        }
        sumX += raw.x;
        sumY += raw.y;
        peakMagnitude = std::max(peakMagnitude, static_cast<float>(StickMagnitude(raw)));
        if (++samples < kStickCalibrationFrames) return false;
        active = false;
        return true;
    }

    bool CanApply() const {
        if (invalid || samples != kStickCalibrationFrames) return false;
        const double meanMagnitude = std::hypot(sumX / samples, sumY / samples);
        // Peak covers jitter; a large peak-mean gap means the stick was moved, not resting.
        return peakMagnitude + kStickCalibrationMargin <= kMaxStickCalibrationDrift + 1e-6 &&
               peakMagnitude <= meanMagnitude + 0.08 + 1e-6;
    }

    float SuggestedDeadzone() const {
        return CanApply() ? SuggestDeadzoneForDrift({static_cast<float>(peakMagnitude), 0.f}) : 0.f;
    }
};

}  // namespace rv
