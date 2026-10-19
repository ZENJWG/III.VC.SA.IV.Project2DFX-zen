#pragma once

#include <cmath>

// The LOD corona renderer always submits rotation=0 sprites. This cheap
// bounding-box rejection matches its four-corner reject check, including
// negative half-extents (the original accepted them).
// Non-finite inputs remain "not rejected", just as in the old comparisons.
namespace CoronaRenderMath
{
    inline bool OutsideViewport(float x, float y, float halfWidth, float halfHeight,
                                float viewportWidth, float viewportHeight) noexcept
    {
        const float w = std::abs(halfWidth);
        const float h = std::abs(halfHeight);
        return x + w < 0.0f || y + h < 0.0f ||
               x - w > viewportWidth || y - h > viewportHeight;
    }
}
