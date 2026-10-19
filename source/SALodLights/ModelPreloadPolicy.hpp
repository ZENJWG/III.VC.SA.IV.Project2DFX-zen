#pragma once
// GTA SA PC model-preload selection and budgets. No game headers required.
// Category membership follows Project2DFX's existing IDE draw-distance rules.
#include <cstdint>

namespace ModelPreloadPolicy
{
    constexpr int kModelCount = 20000;
    enum Category : std::uint8_t
    {
        Normal = 1 << 0,
        Timed = 1 << 1,
        Vegetation = 1 << 2,
        Generic = 1 << 3,
        Neon = 1 << 4,
        BigBuilding = 1 << 5,
        Animated = 1 << 6,
        Lod = 1 << 7
    };

    inline bool ValidId(int id) noexcept { return id >= 0 && id < kModelCount; }

    // Timed IDE models are recorded via LoadTimeObject, not LoadObject.
    // One model may have multiple categories (e.g. generic + normal).
    inline std::uint8_t ObjectCategories(int id, float originalDistance,
                                         bool isInterior, bool isGeneric,
                                         bool isVegetation) noexcept
    {
        if (!ValidId(id) || isInterior || !(originalDistance > 0.0f))
            return 0;
        std::uint8_t result = 0;
        if (originalDistance > 300.0f)
            result |= Lod;
        else
            result |= Normal;
        if (isGeneric) result |= Generic;
        if (isVegetation) result |= Vegetation;
        return result;
    }

    // Keep the old overload for existing tests and callers.
    inline std::uint8_t ObjectCategories(int id, float originalDistance,
                                         bool isInterior) noexcept
    {
        return ObjectCategories(id, originalDistance, isInterior,
            id >= 615 && id <= 1572, id >= 615 && id <= 792);
    }

    inline std::uint8_t TimeCategories(bool neon) noexcept
    {
        return static_cast<std::uint8_t>(Timed | (neon ? Neon : 0));
    }

    inline std::uint8_t EnabledMask(bool normal, bool timed, bool vegetation,
                                    bool generic, bool neon, bool big,
                                    bool animated = false, bool lod = false) noexcept
    {
        return static_cast<std::uint8_t>(
            (normal ? Normal : 0) | (timed ? Timed : 0) |
            (vegetation ? Vegetation : 0) | (generic ? Generic : 0) |
            (neon ? Neon : 0) | (big ? BigBuilding : 0) |
            (animated ? Animated : 0) | (lod ? Lod : 0));
    }

    inline unsigned ClampLimit(int value, unsigned defaultValue, unsigned hardMax) noexcept
    {
        return value <= 0 ? defaultValue :
            static_cast<unsigned>(value) > hardMax ? hardMax : static_cast<unsigned>(value);
    }
}
