#pragma once
// Optional GTA SA per-model absolute draw distances. Apply after category rules.
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>

namespace SAModelOverrides {
    constexpr int kCount = 20000;
    class Table {
        std::array<float,kCount> distances{};
    public:
        void Reset() noexcept { distances.fill(0); }
        float Apply(int id,float computed) const noexcept {
            if (id < 0 || id >= kCount) return computed;
            return distances[static_cast<size_t>(id)] > 0.0f ?
                distances[static_cast<size_t>(id)] : computed;
        }
        bool Load(const std::string& path) {
            Reset();
            if (path.empty()) return true;
            std::ifstream stream(path);
            if (!stream) return false;
            std::string line;
            while (std::getline(stream,line)) {
                if (line.empty() || line[0]=='#' || line[0]==';') continue;
                int id=-1;
                float distance=0;
                char extra=0;
                // Reject accidental partial parses and NaN/negative settings.
                if (std::sscanf(line.c_str()," %d , %f %c",&id,&distance,&extra)!=2 ||
                    id<0 || id>=kCount || !std::isfinite(distance) || distance<=0)
                    continue;
                distances[static_cast<size_t>(id)]=distance;
            }
            return true;
        }
    };
}
