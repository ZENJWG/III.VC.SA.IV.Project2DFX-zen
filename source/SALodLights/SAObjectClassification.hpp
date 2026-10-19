#pragma once
// GTA SA IDE classification - computed at load time only (not per frame).
// Numeric ranges are backwards-compatible fallbacks; IDE path/name classification
// covers additional vegetation and generic models outside those ranges.
#include <string_view>

namespace SAObjectClassification
{
    inline bool IsInterior(int id) noexcept
    {
        if (id < 0) return false;
        if (id >= 910 && id <= 955) return true;
        if (id >= 1700 && id <= 1776) return true;
        if (id >= 2360 && id <= 2872) return true;
        if (id >= 2880 && id <= 2882) return true;
        if (id >= 13590 && id <= 13667) return true;
        if (id >= 14383 && id <= 14495) return true;
        if (id >= 14497 && id <= 14528) return true;
        if (id >= 14530 && id <= 14540) return true;
        if (id >= 14542 && id <= 14554) return true;
        if (id == 14556) return true;
        if (id >= 14558 && id <= 14614) return true;
        if (id >= 14616 && id <= 14630) return true;
        if (id >= 14632 && id <= 14633) return true;
        if (id >= 14635 && id <= 14641) return true;
        if (id == 14643) return true;
        if (id >= 14650 && id <= 14657) return true;
        if (id >= 14660 && id <= 14695) return true;
        if (id >= 14699 && id <= 14728) return true;
        if (id >= 14735 && id <= 14762) return true;
        if (id >= 14764 && id <= 14765) return true;
        if (id >= 14770 && id <= 14856) return true;
        if (id >= 14858 && id <= 14883) return true;
        if (id >= 14885 && id <= 14898) return true;
        if (id >= 14900 && id <= 14903) return true;
        if (id >= 15025 && id <= 15043) return true;
        if (id >= 15046 && id <= 15064) return true;
        if (id >= 18000 && id <= 18036) return true;
        if (id >= 18038 && id <= 18075) return true;
        if (id >= 18077 && id <= 18086) return true;
        if (id >= 18088 && id <= 18092) return true;
        if (id >= 18094 && id <= 18095) return true;
        if (id >= 18098 && id <= 18101) return true;
        if (id >= 18104 && id <= 18105) return true;
        if (id == 18109) return true;
        if (id == 18112) return true;

        if (id >= 1327 && id <= 1572) return true; //dynamic2.ide
        return false;
    }

    inline char Lower(char c) noexcept
    {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
    }

    inline bool EqualNoCase(std::string_view a, std::string_view b) noexcept
    {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (Lower(a[i]) != Lower(b[i])) return false;
        return true;
    }

    inline bool BeginsNoCase(std::string_view a, std::string_view b) noexcept
    {
        if (a.size() < b.size()) return false;
        return EqualNoCase(a.substr(0, b.size()), b);
    }

    inline bool ContainsNoCase(std::string_view a, std::string_view needle) noexcept
    {
        if (needle.empty() || needle.size() > a.size()) return false;
        for (size_t i = 0; i + needle.size() <= a.size(); ++i)
            if (EqualNoCase(a.substr(i, needle.size()), needle)) return true;
        return false;
    }

    inline std::string_view BaseName(const char* path) noexcept
    {
        if (!path) return {};
        std::string_view value{path};
        const auto pos = value.find_last_of("/\\\\");
        return pos == value.npos ? value : value.substr(pos + 1);
    }

    inline bool VegetationIDE(const char* path) noexcept
    {
        const auto name = BaseName(path);
        return EqualNoCase(name, "veg.ide") ||
               EqualNoCase(name, "vegepart.ide") ||
               EqualNoCase(name, "vegetation.ide") ||
               EqualNoCase(name, "trees.ide") ||
               EqualNoCase(name, "plants.ide") ||
               EqualNoCase(name, "plant.ide");
    }

    inline bool GenericIDE(const char* path) noexcept
    {
        const auto name = BaseName(path);
        return EqualNoCase(name, "generic.ide") ||
               EqualNoCase(name, "dynamic.ide") ||
               EqualNoCase(name, "dynamic2.ide");
    }

    inline bool VegetationModelName(const char* name) noexcept
    {
        if (!name) return false;
        const std::string_view value{name};
        // Avoid matching "street" for "tree"; use token boundaries.
        constexpr std::string_view tokens[] = {
            "tree", "palm", "bush", "grass", "plant", "cact",
            "shrub", "hedge", "fern", "flower", "weed", "veg",
            "pine", "leaf", "oak", "cypress", "willow", "maple",
            "poplar", "cedar", "elm", "fir"
        };
        for (const auto token : tokens)
        {
            if (BeginsNoCase(value, token)) return true;
            // Common GTA SA model naming convention: xx_tree_xx
            for (size_t i = 1; i + token.size() <= value.size(); ++i)
                if ((value[i - 1] == '_' || value[i - 1] == '-') &&
                    EqualNoCase(value.substr(i, token.size()), token))
                    return true;
        }
        return false;
    }

    struct Kind
    {
        bool generic = false;
        bool vegetation = false;
    };

    inline Kind Classify(int id, const char* name, const char* idePath) noexcept
    {
        return {
            (id >= 615 && id <= 1572) || GenericIDE(idePath),
            (id >= 615 && id <= 792) ||
                VegetationIDE(idePath) || VegetationModelName(name)
        };
    }
}
