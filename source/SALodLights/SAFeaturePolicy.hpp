#pragma once
// Pure, platform-independent GTA SA feature rules.  No RenderWare/D3D calls.
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace SAFeaturePolicy {
inline float ClampFinite(float value, float low, float high, float fallback) {
    return std::isfinite(value) ? (std::max)(low,(std::min)(high,value)) : fallback;
}
inline float Lerp(float a, float b, float t) {
    return a + (b-a)*(std::max)(0.0f,(std::min)(1.0f,t));
}
// Periodic profiles, blending across both dawn and the midnight boundary.
// Rush peaks at 07:30 and 17:30; night traffic is quiet but never disappears.
inline float TrafficMultiplier(float hour, float rush, float night) {
    if (!std::isfinite(hour)) return 1.0f;
    hour = std::fmod(hour,24.0f);
    if (hour < 0.0f) hour += 24.0f;
    rush = ClampFinite(rush,0.0f,3.0f,1.25f);
    night = ClampFinite(night,0.0f,2.0f,0.65f);
    struct Key { float hour, mult; };
    const Key keys[] = { {0.0f,night},{5.0f,night},{6.5f,1.0f},
        {7.5f,rush},{9.5f,1.0f},{15.5f,1.0f},{17.5f,rush},
        {19.5f,1.0f},{22.5f,night},{24.0f,night} };
    for (unsigned i=1;i<sizeof(keys)/sizeof(keys[0]);++i)
        if (hour <= keys[i].hour) {
            const float t=(hour-keys[i-1].hour)/(keys[i].hour-keys[i-1].hour);
            return Lerp(keys[i-1].mult,keys[i].mult,t);
        }
    return night;
}
// GTA SA distant vehicle and boat navigation coronas are night lights, not
// substitutes for visible vehicle geometry in broad daylight. Smooth dawn/
// dusk ramps avoid popping when the game clock advances.
inline float NightVehicleLightFactor(float hour) {
    if (!std::isfinite(hour)) return 0.0f;
    hour = std::fmod(hour, 24.0f);
    if (hour < 0.0f) hour += 24.0f;
    auto smooth = [](float t) {
        t = (std::clamp)(t, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };
    const float daybreak = smooth((hour - 5.5f) / 1.5f);
    const float sunset = smooth((20.5f - hour) / 2.0f);
    return 1.0f - daybreak * sunset;
}
// Weighted fade for light intensity in rain and fog, using game's own weather
// values; SkyGFX's shader / timecycle is left untouched.
inline float WeatherLight(float rain, float fog) {
    rain=ClampFinite(rain,0.0f,1.0f,0.0f);
    fog=ClampFinite(fog,0.0f,1.0f,0.0f);
    return ClampFinite(1.0f + 0.08f*rain - 0.12f*fog,0.75f,1.10f,1.0f);
}
// CPU-side atmospheric visibility for both RenderWare and D3D9 custom shaders.
// Native GTA/SkyGFX fog may be applied by a different shader pipeline, so
// unlit 2DFX meshes *and* additive coronas need their own alpha envelope.
// Full visibility near the viewer, smooth fade in fog, zero after the fog end.
inline float TrafficFogVisibility(float distance, float cameraFogStart,
                                  float cameraFar, float weatherFog,
                                  float heavyFogLimit, float maxVisibleDistance = 1250.0f) {
    if (!std::isfinite(distance) || distance < 0.0f) return 0.0f;
    // Even if a graphics mod supplies no usable far plane, enforce the
    // user-requested visibility cap rather than exposing traffic indefinitely.
    if (!std::isfinite(cameraFar) || cameraFar <= 0.0f) {
        if (!std::isfinite(maxVisibleDistance) || maxVisibleDistance <= 0.0f)
            return 0.0f;
        const float cap = (std::clamp)(maxVisibleDistance, 300.0f, 10000.0f);
        return ClampFinite((cap - distance) / (cap * 0.30f),
                           0.0f, 1.0f, 0.0f);
    }
    const float end = cameraFar;
    float start = std::isfinite(cameraFogStart) ?
        (std::clamp)(cameraFogStart, 0.0f, end) : end;
    // Do not divide by zero if a graphics mod provides equal fog/far planes.
    float cameraVisibility = 1.0f;
    if (start < end - 1.0f)
        cameraVisibility = ClampFinite((end - distance) / (end - start), 0.0f, 1.0f, 0.0f);

    const float fog = ClampFinite(weatherFog, 0.0f, 1.0f, 0.0f);
    const float heavyEnd = ClampFinite(heavyFogLimit, 200.0f, 2500.0f, 650.0f);
    const float blend = fog * fog * (3.0f - 2.0f * fog);
    const float weatherEnd = Lerp(end, (std::min)(end, heavyEnd), blend);
    // Smooth transition: there are no hard cutoff lines across the map.
    const float weatherStart = weatherEnd * 0.55f;
    const float weatherVisibility = weatherEnd > weatherStart + 1.0f ?
        ClampFinite((weatherEnd - distance) / (weatherEnd - weatherStart),
                    0.0f, 1.0f, 0.0f) : (distance < weatherEnd ? 1.0f : 0.0f);
    float viewVisibility = 1.0f;
    if (std::isfinite(maxVisibleDistance) && maxVisibleDistance > 0.0f) {
        const float cap = (std::clamp)(maxVisibleDistance, 300.0f, 10000.0f);
        const float fadeFrom = cap * 0.70f;
        viewVisibility = ClampFinite((cap - distance) / (cap - fadeFrom),
                                     0.0f, 1.0f, 0.0f);
    }
    return (std::min)(cameraVisibility, (std::min)(weatherVisibility, viewVisibility));
}

// Keep the path graph warm slightly beyond visible traffic, without
// simulating thousands of completely fog-hidden cars across the entire map.
inline float TrafficSimulationRange(float farClip, float weatherFog,
                                   float heavyFogLimit, float maxVisibleDistance) {
    if (!std::isfinite(farClip) || farClip <= 0.0f) return 0.0f;
    float end = farClip;
    if (std::isfinite(maxVisibleDistance) && maxVisibleDistance > 0.0f)
        end = (std::min)(end, maxVisibleDistance);
    const float fog = ClampFinite(weatherFog, 0.0f, 1.0f, 0.0f);
    const float blend = fog * fog * (3.0f - 2.0f * fog);
    const float weatherEnd = Lerp(farClip,
        (std::min)(farClip, ClampFinite(heavyFogLimit, 200.0f, 2500.0f, 650.0f)), blend);
    end = (std::min)(end, weatherEnd);
    return (std::min)(farClip, (std::max)(350.0f, end + 250.0f));
}

// Preserve approximate cars per square kilometre when fog reduces the
// simulated radius. Without this, 1000 cars that once covered a 4-km view
// are squeezed into a small clear circle around the viewer.
inline float TrafficPopulationScale(float simulatedRange, float worldRange) {
    if (!std::isfinite(simulatedRange) || !std::isfinite(worldRange) ||
        simulatedRange <= 0.0f || worldRange <= 0.0f) return 0.0f;
    const float ratio = (std::clamp)(simulatedRange / worldRange, 0.0f, 1.0f);
    return (std::max)(0.08f, ratio * ratio);
}

enum class Tier { Full, Low, Impostor };
inline Tier DetailTier(float distance, float mediumStart, float farStart,
                       bool enabled) {
    if (!enabled || !std::isfinite(distance)) return Tier::Full;
    mediumStart=ClampFinite(mediumStart,150.0f,5000.0f,350.0f);
    farStart=ClampFinite(farStart,mediumStart+40.0f,10000.0f,700.0f);
    if (distance >= farStart) return Tier::Impostor;
    return distance >= mediumStart ? Tier::Low : Tier::Full;
}
inline float DetailFade(float distance, float cutoff, float band) {
    if (!std::isfinite(distance)) return 0.0f;
    band=ClampFinite(band,5.0f,200.0f,40.0f);
    return ClampFinite((cutoff-distance)/band,0.0f,1.0f,0.0f);
}
// In daylight, never replace a distant body with a headlight/taillight
// sprite. Nighttime impostors still work at the same configured distance.
inline float NightVehicleImpostorMix(float distance, float cutoff,
                                    float band, float nightFactor) {
    const float mix = 1.0f - DetailFade(distance, cutoff, band);
    return ClampFinite(mix * nightFactor, 0.0f, 1.0f, 0.0f);
}
// Stable vehicle silhouette choice: one car keeps its proportions when moving.
// Variation uses one shared mesh and per-instance world transforms; no new
// textures, assets, shader constants, or D3D9 device calls.
struct Silhouette { float width, length, height; };
inline unsigned Variant(std::uint32_t id) {
    id ^= id >> 16; id *= 0x7feb352du; id ^= id >> 15;
    return static_cast<unsigned>(id % 4u);
}
inline Silhouette Shape(std::uint32_t id, bool enabled) {
    if (!enabled) return {1.0f,1.0f,1.0f};
    constexpr Silhouette shapes[] = {
        {1.0f,1.0f,1.0f},      // sedan
        {0.86f,0.83f,0.92f},  // compact
        {1.10f,1.06f,1.22f},  // SUV
        {1.03f,1.20f,1.07f}   // longbody
    };
    return shapes[Variant(id)];
}
// Models should be requested *ahead* of their normal LOD eligibility band,
// not pinned or rendered twice. Model streaming remains GTA's responsibility.
inline bool ShouldPrefetchLOD(float cameraDistance, float modelDistance,
                              float transitionBand) {
    if (!std::isfinite(cameraDistance) || cameraDistance < 0 ||
        !std::isfinite(modelDistance) || modelDistance <= 0) return false;
    transitionBand=ClampFinite(transitionBand,10.0f,500.0f,80.0f);
    return cameraDistance <= modelDistance + transitionBand &&
           cameraDistance >= (std::max)(0.0f,modelDistance-transitionBand);
}
} // namespace SAFeaturePolicy
