// DdsDecoder.h - 内置 DDS 纹理解码（SDL_image 裁剪版无 DDS loader）
// 2026-08：Bistro 场景 DDS 加载失败（SDL Error: Unsupported image format）→ 引擎自实现
// 支持：DXT1(BC1) / DXT3(BC2) / DXT5(BC3) / ATI2(BC5_UNORM, 法线 RG)
// 不支持：BC4 / BC6H / BC7（返回 false，调用方提示转 KTX2——引擎 KTX2/BasisU 路径覆盖 BC7/ASTC）
// DDS 数据为 top-down 行序（与 PNG 一致，无需 Y 翻转）
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace DdsDecoder {

// 解码 DDS 数据到 RGBA8（top-down）。
// 返回 false = 非 DDS / 格式不支持 / 数据损坏。
bool Decode(const uint8_t* data, size_t size, int& outWidth, int& outHeight, std::vector<uint8_t>& outRGBA);

// 支持格式的只读说明（供日志/诊断）
bool IsSupportedFormat(const uint8_t* data, size_t size);

} // namespace DdsDecoder
