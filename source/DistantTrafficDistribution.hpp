#pragma once
// GTA SA distant-traffic distribution. Pure math: no RenderWare, GPU or game memory.
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace DistantTrafficDistribution {
inline float Clamp01(float x) {
    return std::isfinite(x) ? (std::clamp)(x, 0.0f, 1.0f) : 0.0f;
}
inline std::uint32_t Mix(std::uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    return x ^ (x >> 16);
}
inline float Random01(std::uint32_t x) {
    return static_cast<float>(Mix(x) >> 8) / 16777216.0f;
}
inline float AreaNoise(int x, int y) {
    const std::uint32_t a = static_cast<std::uint32_t>(x);
    const std::uint32_t b = static_cast<std::uint32_t>(y);
    return Random01(a * 0x9e3779b9u ^ b * 0x85ebca6bu ^ 0x7c4a7f29u);
}
// Bilinear, smooth spatial patches (not random changes per frame or per-car).
// Density differences extend across several neighboring streets.
inline float Neighborhood(float x, float y) {
    if (!std::isfinite(x) || !std::isfinite(y) ||
        std::abs(x) > 1000000.0f || std::abs(y) > 1000000.0f) return 0.5f;
    constexpr float scale = 210.0f;
    const float fx = std::floor(x / scale), fy = std::floor(y / scale);
    const int ix = static_cast<int>(fx), iy = static_cast<int>(fy);
    auto smooth = [](float t) { return t * t * (3.0f - 2.0f * t); };
    const float u = smooth(x / scale - fx), v = smooth(y / scale - fy);
    const float a = AreaNoise(ix, iy) * (1.0f - u) + AreaNoise(ix + 1, iy) * u;
    const float b = AreaNoise(ix, iy + 1) * (1.0f - u) + AreaNoise(ix + 1, iy + 1) * u;
    return a * (1.0f - v) + b * v;
}
inline unsigned LocalLimit(unsigned base, float x, float y, float variation) {
    if (!base || Clamp01(variation) <= 0.0f) return base;
    // Traffic occasionally clusters on arterials, but residential blocks remain quiet.
    const float factor = 1.0f + Clamp01(variation) * (Neighborhood(x, y) * 1.65f - 0.90f);
    const float value = static_cast<float>(base) * factor;
    return static_cast<unsigned>((std::max)(1.0f, std::floor(value + 0.5f)));
}
inline float TargetGap(float speed, bool water, std::uint32_t random, float variation) {
    speed = std::isfinite(speed) ? (std::clamp)(speed, 1.0f, 45.0f) : 16.0f;
    const float original = water ? 90.0f : (std::max)(28.0f, speed * 2.0f);
    const float varied = water ? (65.0f + 55.0f * Random01(random)) :
        (std::max)(12.0f, speed * (0.72f + 1.55f * Random01(random)));
    return original + (varied - original) * Clamp01(variation);
}
// Do not impose the same exclusion circle on vehicles in *different* lanes.
// Keep a physical clearance at junctions and safe longitudinal headway on
// the same lane. The 2D vectors are unit-length driving directions.
inline bool BlocksSpawn(float dx, float dy, float dz, float dirX, float dirY,
                        float otherX, float otherY, float gap, bool water) {
    if (std::abs(dz) >= 4.0f) return false;
    const float d2 = dx*dx + dy*dy;
    if (water) return d2 < gap*gap;
    if (d2 < 5.0f*5.0f) return true;
    const float heading = dirX*otherX + dirY*otherY;
    if (heading > 0.65f) {
        const float lateral = std::abs(dx*dirY - dy*dirX);
        const float along = dx*dirX + dy*dirY;
        return lateral < 3.3f && std::abs(along) < gap;
    }
    // Crossing roads must not overlap at junctions.
    return heading > -0.65f && d2 < 10.0f*10.0f;
}
} // namespace DistantTrafficDistribution
