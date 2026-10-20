#pragma once
// Numerically safe ground-plane interpolation for the shared distant-car renderer.
// Reject near-vertical planes and malformed collision normals before division.
#include <cmath>

namespace SafeGroundProjection {
    inline bool Height(float groundHeight, float nx, float ny, float nz,
                       float dx, float dy, float* outHeight) noexcept {
        if (!outHeight || !std::isfinite(groundHeight) ||
            !std::isfinite(nx) || !std::isfinite(ny) || !std::isfinite(nz) ||
            !std::isfinite(dx) || !std::isfinite(dy) || std::abs(nz) < 0.1f)
            return false;
        const float height = groundHeight - (nx * dx + ny * dy) / nz;
        if (!std::isfinite(height)) return false;
        *outHeight = height;
        return true;
    }
}
