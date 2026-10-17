#pragma once
#include <array>
#include <cstdint>
#include <limits>
#include <algorithm>

namespace ViewportResolution {
struct Preset { int width; int height; const char* label; };
inline constexpr std::array<Preset, 7> Presets{{
    {640, 360, "640 x 360"}, {960, 540, "960 x 540"},
    {1280, 720, "1280 x 720"}, {1600, 900, "1600 x 900"},
    {1920, 1080, "1920 x 1080"}, {2560, 1440, "2560 x 1440"},
    {3840, 2160, "3840 x 2160"}
}};
inline const Preset& Closest(int width, int height) {
    width = std::clamp(width, 0, 16384);
    height = std::clamp(height, 0, 16384);
    const Preset* selected = &Presets[4];
    auto distance = std::numeric_limits<std::int64_t>::max();
    for (const auto& preset : Presets) {
        const auto dx = std::int64_t(width) - preset.width;
        const auto dy = std::int64_t(height) - preset.height;
        const auto candidate = dx * dx + dy * dy;
        if (candidate < distance) { selected = &preset; distance = candidate; }
    }
    return *selected;
}
inline bool NeedsRebuild(unsigned currentWidth, unsigned currentHeight, int width, int height) {
    return currentWidth != unsigned(width) || currentHeight != unsigned(height);
}
} // namespace ViewportResolution
