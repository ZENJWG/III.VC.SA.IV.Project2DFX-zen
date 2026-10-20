#pragma once
// GTA SA IDE draw-distance policy. Pure functions for isolated regression testing.
// Settings: 0 = disabled, (0,10] = multiplier, >10 = absolute distance.
#include <cmath>

namespace DrawDistancePolicy
{
    inline float Apply(float original, float setting, bool lod = false) noexcept
    {
        if (!std::isfinite(original) || original <= 0.0f ||
            !std::isfinite(setting) || setting <= 0.0f)
            return original;
        if (setting <= 10.0f)
            return original * setting;
        // LODs should never shrink when given an absolute override.
        return lod ? (original < setting ? setting : original) : setting;
    }

    // Big-building instances are identified by CEntity::m_bIsBIGBuilding,
    // not by IDE draw-distance thresholds or name prefixes. A model is updated
    // once when CEntity::SetupBigBuilding is called.
    inline float BigBuilding(float original, float setting) noexcept
    {
        const float distance = Apply(original, setting, true);
        // Mod INI values can be arbitrarily large. Never feed infinity into
        // the game renderer's distance/streaming calculations.
        return std::isfinite(distance) ? distance : original;
    }

    // Entity-based LODs can have an IDE radius <= 300, so also extend them
    // when vanilla SetupBigBuilding marks the actual entity as a LOD.
    inline float BigBuildingCombined(float original, float current,
                                    float lodSetting, float bigSetting) noexcept
    {
        float best = std::isfinite(current) && current > 0.0f ? current : original;
        const float lod = Apply(original, lodSetting, true);
        const float big = BigBuilding(original, bigSetting);
        if (std::isfinite(lod) && lod > best) best = lod;
        if (std::isfinite(big) && big > best) best = big;
        return best;
    }

    inline bool IsNeon(const char* name) noexcept
    {
        if (!name) return false;
        // Bounded to the GTA SA IDE model-name buffer (23 chars + terminator).
        for (unsigned i = 0; i < 24 && name[i]; ++i)
        {
            auto lower = [](char c) noexcept -> char
            {
                return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
            };
            if (i + 4 <= 24 && name[i + 1] && name[i + 2] && name[i + 3] &&
                lower(name[i]) == 'n' && lower(name[i+1]) == 'e' &&
                lower(name[i+2]) == 'o' && lower(name[i+3]) == 'n')
                return true;
        }
        return false;
    }

    inline float Timed(float original, const char* name, float timed, float neon) noexcept
    {
        // Neon overrides timed when both settings are enabled; otherwise fall back
        // to the timed setting. Never multiply by zero for a neon-only configuration.
        const float setting = IsNeon(name) && neon > 0.0f ? neon : timed;
        return Apply(original, setting);
    }

    // Select using parsed model metadata rather than just hard-coded ID ranges.
    // Never shrink an IDE distance when multiple controls overlap.
    inline float ObjectTagged(float original, bool interior, bool isGeneric,
                              bool isVegetation, float lod, float generic,
                              float allNormal, float vegetation) noexcept
    {
        if (interior || !std::isfinite(original) || original <= 0.0f)
            return original;
        float best = original;
        if (original > 300.0f)
        {
            // A long-range vegetation/generic IDE entry still belongs to
            // those categories; the old threshold short-circuit missed it.
            best = Apply(original, lod, true);
            if (!std::isfinite(best) || best < original) best = original;
        }
        const auto extend = [&](float setting) noexcept
        {
            const float candidate = Apply(original, setting);
            if (std::isfinite(candidate) && candidate > best)
                best = candidate;
        };
        if (original <= 300.0f) extend(allNormal);
        if (isGeneric) extend(generic);
        if (isVegetation) extend(vegetation);
        return best;
    }

    // Normal-world scanning must include explicit per-model overrides, even
    // for interiors excluded from category-wide draw-distance adjustments.
    inline bool NeedsNormalScanExtension(float original, float adjusted,
                                         bool excludedInterior) noexcept
    {
        return std::isfinite(original) && original > 0.0f &&
            original <= 300.0f && std::isfinite(adjusted) &&
            adjusted > 0.0f && (!excludedInterior || adjusted > original);
    }

    // Legacy convenience API used by compatibility tests and external integrations.
    inline float Object(float original, int id, bool interior, float lod,
                        float generic, float allNormal, float vegetation) noexcept
    {
        return ObjectTagged(original, interior,
            id >= 615 && id <= 1572, id >= 615 && id <= 792,
            lod, generic, allNormal, vegetation);
    }

    inline float Animated(float original, float setting) noexcept
    {
        const float candidate = Apply(original, setting);
        return std::isfinite(candidate) && candidate > original ? candidate : original;
    }
}
