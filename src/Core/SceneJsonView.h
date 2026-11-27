#pragma once

#include "json.hpp"
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace ECS {

struct SceneJsonView {
    const nlohmann::json& value;

    SceneJsonView Find(std::string_view key) const {
        const auto found = value.find(key);
        static const nlohmann::json missing;
        return {found != value.end() ? *found : missing};
    }

    std::string String(std::string_view key) const {
        const auto& field = Find(key).value;
        return field.is_string() ? field.get<std::string>() : std::string{};
    }

    bool Boolean(std::string_view key) const {
        const auto& field = Find(key).value;
        if (field.is_boolean()) return field.get<bool>();
        return field.is_string() && field == "true";
    }

    template<typename T>
    T Number(std::string_view key, T fallback) const {
        const auto& field = Find(key).value;
        if (!field.is_number()) return fallback;
        const double value = field.get<double>();
        if (value < std::numeric_limits<T>::lowest() || value > std::numeric_limits<T>::max()) {
            return fallback;
        }
        return static_cast<T>(value);
    }

    std::vector<float> FloatArray(std::string_view key) const {
        const auto& field = Find(key).value;
        if (!field.is_array()) return {};
        std::vector<float> result;
        result.reserve(field.size());
        for (const auto& element : field) {
            if (!element.is_number()) return {};
            result.push_back(element.get<float>());
        }
        return result;
    }

    // Legacy codecs still apply their original scalar defaults and bounds.
    std::string Token(std::string_view key) const {
        const auto& field = Find(key).value;
        return field.is_null() ? std::string{} : field.dump();
    }
};

} // namespace ECS
