#pragma once

#include "Platform/Export.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// CPU-side 16-bit height samples. The sample order is row-major and keeps the
// source image's top-left origin; Vulkan upload code is responsible for any
// render-space Y flip.
struct MIKAN_API HeightmapPixels16 {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint16_t> samples;

    void Clear() {
        width = 0;
        height = 0;
        samples.clear();
    }

    bool IsValid() const {
        return width > 0 && height > 0 &&
               samples.size() == static_cast<size_t>(width) * static_cast<size_t>(height);
    }
};

namespace HeightmapLoader {

// Loads a non-interlaced 16-bit grayscale PNG (color type 0) or grayscale+alpha
// PNG (color type 4). Samples are returned as the original big-endian PNG
// values converted to host-endian uint16_t, with no normalization or gamma
// conversion. Rows retain the PNG top-left origin.
MIKAN_API bool LoadPng16(const std::string& filePath,
                         HeightmapPixels16& out,
                         std::string* errorMessage = nullptr);

} // namespace HeightmapLoader
