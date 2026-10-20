#pragma once
// GTA SA streaming budget decisions, independent of game headers.
#include <algorithm>
#include <cstdint>

namespace StreamingBudget {
    constexpr std::uint64_t MiB = 1024ull * 1024ull;
    inline bool CanRequest(std::uint32_t used, std::uint32_t available,
                           std::uint32_t baseline, std::uint32_t outstanding,
                           int budgetMB, int freeReserveMB) noexcept {
        if (outstanding >= 16 || used >= available) return false;
        const auto reserve = static_cast<std::uint64_t>(
            std::clamp(freeReserveMB, 0, 1024)) * MiB;
        if (static_cast<std::uint64_t>(available - used) <= reserve) return false;
        const auto budget = static_cast<std::uint64_t>(
            std::clamp(budgetMB, 0, 1024)) * MiB;
        if (!budget) return false;
        return used <= baseline ||
               static_cast<std::uint64_t>(used - baseline) < budget;
    }
}
