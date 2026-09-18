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

// ===== PNG 编码（显式保存地形笔刷产物用）=====
// bundled zlib 只带 inflate 没有 deflate，因此编码走 stored-block（无压缩）
// zlib 流——输出是完全合法的 PNG，本 Loader 与任何标准 PNG 解码器都能读回。
//
// 灰度 16-bit（color type 0 / bit depth 16），samples 宿端序、行主序、顶左原点，
// 与 LoadPng16 输出同约定，所以「保存 → 重载」逐字节等价。
MIKAN_API bool SavePng16(const std::string& filePath,
                         uint32_t width, uint32_t height,
                         const uint16_t* samples,
                         std::string* errorMessage = nullptr);

// 灰度 8-bit（channels=1，草密度图用）或 RGBA 8-bit（channels=4，控制图用）；
// pixels 行主序、顶左原点。
MIKAN_API bool SavePng8(const std::string& filePath,
                        uint32_t width, uint32_t height, uint32_t channels,
                        const uint8_t* pixels,
                        std::string* errorMessage = nullptr);

} // namespace HeightmapLoader
