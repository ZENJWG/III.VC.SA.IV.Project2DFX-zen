#pragma once
// GTA SA distant-traffic projected-headlight geometry policy.
// Pure helpers stay independent of RenderWare and plugin-sdk for regression tests.
#include <cmath>
#include <cstdint>

namespace ProjectedHeadlightPolicy {
    constexpr int Rows = 5;
    constexpr int Corners = 4; // left beam L/R, right beam L/R
    constexpr float SurfaceTolerance = 1.60f; // reject roofs, lower decks, invalid roads
    constexpr float MaxStepHeight = 1.00f;     // stop at abrupt crests/steps
    constexpr float MaxCrossHeight = .65f;     // reject edges crossing walls/curbs
    constexpr float GeometryOffset = .065f;   // mitigate Z fighting
    constexpr float RowStep = 2.75f;
    constexpr float FirstStep = 2.8f;

    inline bool AcceptHeight(float hit, float predicted) noexcept {
        return std::isfinite(hit) && std::isfinite(predicted) &&
            std::abs(hit - predicted) <= SurfaceTolerance;
    }
    inline bool AcceptCross(float a, float b) noexcept {
        return std::isfinite(a) && std::isfinite(b) &&
            std::abs(a - b) <= MaxCrossHeight;
    }
    inline bool AcceptStep(float previous, float next) noexcept {
        return std::isfinite(previous) && std::isfinite(next) &&
            std::abs(previous - next) <= MaxStepHeight;
    }
    inline bool AcceptPose(float distanceSq2d, float directionDot) noexcept {
        return std::isfinite(distanceSq2d) && distanceSq2d <= 2.25f &&
            std::isfinite(directionDot) && directionDot >= .985f;
    }
    inline float RowStrength(unsigned row) noexcept {
        if (row >= static_cast<unsigned>(Rows)) return 0.0f;
        const float t = static_cast<float>(row) / float(Rows - 1);
        return (1.0f - t) * (1.0f - .32f * t);
    }
    inline unsigned char Alpha(float light, unsigned row, bool edge) noexcept {
        if (!std::isfinite(light) || light <= 0.0f) return 0;
        // Deliberately subtle: additive bloom would overwhelm SkyGFX's timecycle.
        const float strength = (light > 1.0f ? 1.0f : light) *
            RowStrength(row) * (edge ? .04f : .30f);
        const float a = 175.0f * strength;
        return static_cast<unsigned char>(a > 255.0f ? 255.0f : a);
    }
}
