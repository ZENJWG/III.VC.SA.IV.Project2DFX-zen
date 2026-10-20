#pragma once
// GTA SA adaptive far-clip policy: engine-independent and regression-testable.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cmath>

namespace AdaptiveFarClip {
    template<std::size_t N = 60>
    struct Controller {
        static_assert(N >= 2);
        std::array<std::int64_t, N> timestamps{};
        std::size_t count = 0, next = 0;
        std::int64_t lastAdjust = 0;
        float value = 1.0f;

        void Reset(float initial) noexcept {
            timestamps.fill(0);
            count = next = 0;
            lastAdjust = 0;
            value = initial;
        }
        float Observe(std::int64_t now, std::int64_t ticksPerSecond,
                      float targetFPS, float minValue, float maxValue) noexcept {
            if (ticksPerSecond <= 0 || !std::isfinite(minValue) ||
                !std::isfinite(maxValue) || minValue <= 0 || maxValue < minValue)
                return value;
            if (count && now <= timestamps[(next + N - 1) % N]) {
                Reset(std::clamp(value, minValue, maxValue));
            }
            if (count && now - timestamps[(next + N - 1) % N] > ticksPerSecond * 2) {
                Reset(std::clamp(value, minValue, maxValue)); // pause/alt-tab
            }
            timestamps[next] = now;
            next = (next + 1) % N;
            count = (std::min)(count + 1, N);
            if (count < 2 || !std::isfinite(targetFPS) || targetFPS <= 0)
                return value;
            const auto first = timestamps[(next + N - count) % N];
            const auto elapsed = now - first;
            if (elapsed <= 0) return value;
            const double fps = static_cast<double>(count - 1) *
                static_cast<double>(ticksPerSecond) / static_cast<double>(elapsed);
            // Require a settled window; use wall time, not call count, for smoothness.
            if (!lastAdjust) { lastAdjust = now; return value; }
            const double dt = static_cast<double>(now - lastAdjust) /
                static_cast<double>(ticksPerSecond);
            if (dt < 0.25) return value;
            lastAdjust = now;
            const float band = (std::max)(1.0f, targetFPS * 0.035f);
            const float delta = (maxValue - minValue) * 0.12f *
                static_cast<float>((std::min)(dt, 1.0));
            if (fps < targetFPS - band) value -= delta;
            else if (fps > targetFPS + band) value += delta;
            value = (std::clamp)(value, minValue, maxValue);
            return value;
        }
    };
}
