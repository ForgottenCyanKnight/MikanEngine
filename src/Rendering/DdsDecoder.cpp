// DdsDecoder.cpp - 内置 DDS 纹理解码实现
// 支持 DXT1/DXT3/DXT5 块压缩 + ATI2(BC5) 法线（RG 通道，B 填 128）
// 参考标准 S3TC/BC 解压算法（public domain 级别）

#include "Rendering/DdsDecoder.h"
#include <cstring>

namespace DdsDecoder {

namespace {

constexpr uint32_t kMagic = 0x20534444u; // "DDS "

// DDS_PIXELFORMAT dwFlags
constexpr uint32_t kDdpfFourCC = 0x00000004u;

// DX10 dxgiFormat（BC5_UNORM）
constexpr uint32_t kDxgiBC5Unorm = 77;
constexpr uint32_t kDxgiBC5Snorm = 78;
constexpr uint32_t kDxgiBC4Unorm = 80;

struct DdsHeader {
    uint32_t height = 0;
    uint32_t width = 0;
    uint32_t fourCC = 0;
    uint32_t dx10Format = 0;
    bool hasDx10 = false;
    const uint8_t* pixels = nullptr;
    size_t pixelBytes = 0;
};

bool ParseHeader(const uint8_t* data, size_t size, DdsHeader& h) {
    if (!data || size < 124) return false;
    if (std::memcmp(data, "DDS ", 4) != 0) return false;

    h.height = data[12] | (data[13] << 8) | (data[14] << 16) | (data[15] << 24);
    h.width  = data[16] | (data[17] << 8) | (data[18] << 16) | (data[19] << 24);
    if (h.width == 0 || h.height == 0 || h.width > 16384 || h.height > 16384) return false;

    // DDS_HEADER 124 字节；DDS_PIXELFORMAT @76：dwSize @76、dwFlags @80、dwFourCC @84
    const uint32_t pfFlags = data[80] | (data[81] << 8) | (data[82] << 16) | (data[83] << 24);
    h.fourCC = data[84] | (data[85] << 8) | (data[86] << 16) | (data[87] << 24);

    size_t dataOffset = 128;   // 4(magic "DDS ") + 124(DDS_HEADER)
    if (h.fourCC == 0x30315844u /* "DX10" */) { // DDS_HEADER_DXT10 20 字节：dxgiFormat @128
        if (size < 148) return false;
        h.dx10Format = data[128] | (data[129] << 8) | (data[130] << 16) | (data[131] << 24);
        h.hasDx10 = true;
        dataOffset = 148;   // 4(magic) + 124(header) + 20(DX10)
    } else if ((pfFlags & kDdpfFourCC) == 0) {
        return false; // 未压缩 RGB DDS 暂不支持
    }
    h.pixels = data + dataOffset;
    h.pixelBytes = size - dataOffset;
    return true;
}

inline void ColorBlockColors(uint16_t c0u, uint16_t c1u, uint8_t colors[4][4], bool& useTransparency) {
    const int c0r = (c0u >> 11) & 31, c0g = (c0u >> 5) & 63, c0b = c0u & 31;
    const int c1r = (c1u >> 11) & 31, c1g = (c1u >> 5) & 63, c1b = c1u & 31;
    colors[0][0] = static_cast<uint8_t>((c0r * 255 + 15) / 31);
    colors[0][1] = static_cast<uint8_t>((c0g * 255 + 31) / 63);
    colors[0][2] = static_cast<uint8_t>((c0b * 255 + 15) / 31);
    colors[0][3] = 255;
    colors[1][0] = static_cast<uint8_t>((c1r * 255 + 15) / 31);
    colors[1][1] = static_cast<uint8_t>((c1g * 255 + 31) / 63);
    colors[1][2] = static_cast<uint8_t>((c1b * 255 + 15) / 31);
    colors[1][3] = 255;
    if (c0u > c1u) {
        for (int i = 0; i < 3; i++) {
            colors[2][i] = static_cast<uint8_t>((2 * colors[0][i] + colors[1][i]) / 3);
            colors[3][i] = static_cast<uint8_t>((colors[0][i] + 2 * colors[1][i]) / 3);
        }
        colors[2][3] = colors[3][3] = 255;
        useTransparency = false;
    } else {
        for (int i = 0; i < 3; i++) {
            colors[2][i] = static_cast<uint8_t>((colors[0][i] + colors[1][i]) / 2);
            colors[3][i] = 0;
        }
        colors[2][3] = 255;
        colors[3][3] = 0;
        useTransparency = true;
    }
}

// 生成 DXT5/BC4 的 16 值 alpha 表
inline void AlphaTable(uint8_t a0, uint8_t a1, uint8_t table[8]) {
    table[0] = a0;
    table[1] = a1;
    if (a0 > a1) {
        for (int i = 1; i <= 6; i++) table[i + 1] = static_cast<uint8_t>(((7 - i) * a0 + i * a1) / 7);
    } else {
        for (int i = 1; i <= 4; i++) table[i + 1] = static_cast<uint8_t>(((5 - i) * a0 + i * a1) / 5);
        table[6] = 0;
        table[7] = 255;
    }
}

// 解码一个 4x4 块的 3bit 索引（8 字节：2 端点 + 6 字节索引）
inline void DecodeIndex3(const uint8_t* src, uint8_t outIdx[16]) {
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++) bits |= static_cast<uint64_t>(src[2 + i]) << (8 * i);
    for (int i = 0; i < 16; i++) outIdx[i] = static_cast<uint8_t>((bits >> (3 * i)) & 7);
}

inline void DecodeDxt1(const uint8_t* block, uint8_t* out) {
    const uint16_t c0 = block[0] | (block[1] << 8);
    const uint16_t c1 = block[2] | (block[3] << 8);
    uint8_t colors[4][4];
    bool transp = false;
    ColorBlockColors(c0, c1, colors, transp);
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            const int idx = (block[4 + y] >> (2 * x)) & 3;
            const int o = (y * 4 + x) * 4;
            out[o + 0] = colors[idx][0];
            out[o + 1] = colors[idx][1];
            out[o + 2] = colors[idx][2];
            out[o + 3] = colors[idx][3];
        }
    }
}

inline void DecodeDxt3(const uint8_t* block, uint8_t* out) {
    // alpha: 高 8 字节（每像素 4bit）
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            const int o = (y * 4 + x) * 4;
            const int nib = (block[y * 2 + x / 2] >> (4 * (x % 2))) & 0xF;
            out[o + 3] = static_cast<uint8_t>(nib * 17);
        }
    }
    // 颜色块按 DXT1（c0 > c1 恒真——DXT3 颜色块按 DXT1 规则但不透明）
    const uint16_t c0 = block[8] | (block[9] << 8);
    const uint16_t c1 = block[10] | (block[11] << 8);
    uint8_t colors[4][4];
    bool transp = false;
    ColorBlockColors(c0, c1, colors, transp);
    if (c0 <= c1) { // DXT3 无透明规则：强制 4 色（c0>c1 语义）
        for (int i = 0; i < 3; i++) {
            colors[2][i] = static_cast<uint8_t>((2 * colors[0][i] + colors[1][i]) / 3);
            colors[3][i] = static_cast<uint8_t>((colors[0][i] + 2 * colors[1][i]) / 3);
        }
        colors[2][3] = colors[3][3] = 255;
    }
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            const int idx = (block[12 + y] >> (2 * x)) & 3;
            const int o = (y * 4 + x) * 4;
            out[o + 0] = colors[idx][0];
            out[o + 1] = colors[idx][1];
            out[o + 2] = colors[idx][2];
            // alpha 已写
        }
    }
}

inline void DecodeDxt5(const uint8_t* block, uint8_t* out) {
    uint8_t table[8];
    AlphaTable(block[0], block[1], table);
    uint8_t idx[16];
    DecodeIndex3(block, idx);
    for (int i = 0; i < 16; i++) out[i * 4 + 3] = table[idx[i]];
    const uint16_t c0 = block[8] | (block[9] << 8);
    const uint16_t c1 = block[10] | (block[11] << 8);
    uint8_t colors[4][4];
    bool transp = false;
    ColorBlockColors(c0, c1, colors, transp);
    if (c0 <= c1) { // DXT5 颜色块恒 4 色
        for (int i = 0; i < 3; i++) {
            colors[2][i] = static_cast<uint8_t>((2 * colors[0][i] + colors[1][i]) / 3);
            colors[3][i] = static_cast<uint8_t>((colors[0][i] + 2 * colors[1][i]) / 3);
        }
        colors[2][3] = colors[3][3] = 255;
    }
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            const int idx = (block[12 + y] >> (2 * x)) & 3;
            const int o = (y * 4 + x) * 4;
            out[o + 0] = colors[idx][0];
            out[o + 1] = colors[idx][1];
            out[o + 2] = colors[idx][2];
        }
    }
}

// 单通道 BC4 块（供 ATI2 复用）：返回 16 个通道值
inline void DecodeBc4(const uint8_t* block, uint8_t out[16]) {
    uint8_t table[8];
    AlphaTable(block[0], block[1], table);
    uint8_t idx[16];
    DecodeIndex3(block, idx);
    for (int i = 0; i < 16; i++) out[i] = table[idx[i]];
}

// ATI2(BC5)：两个 BC4 块（R 通道 + G 通道）；B=128（法线 z 由 shader 重建），A=255
inline void DecodeAti2(const uint8_t* block, uint8_t* out) {
    uint8_t red[16], green[16];
    DecodeBc4(block, red);
    DecodeBc4(block + 8, green);
    for (int i = 0; i < 16; i++) {
        out[i * 4 + 0] = red[i];
        out[i * 4 + 1] = green[i];
        out[i * 4 + 2] = 128;
        out[i * 4 + 3] = 255;
    }
}

enum class Fmt { Dxt1, Dxt3, Dxt5, Ati2, Unsupported };

Fmt DetectFormat(const DdsHeader& h) {
    if (h.hasDx10) {
        switch (h.dx10Format) {
            case kDxgiBC5Unorm:
            case kDxgiBC5Snorm: return Fmt::Ati2;
            default: return Fmt::Unsupported; // BC4/BC6H/BC7 暂不支持
        }
    }
    switch (h.fourCC) {
        case 0x31545844u: return Fmt::Dxt1; // DXT1
        case 0x33545844u: return Fmt::Dxt3; // DXT3
        case 0x35545844u: return Fmt::Dxt5; // DXT5
        case 0x32495441u: return Fmt::Ati2; // ATI2
        default: return Fmt::Unsupported;
    }
}

int BytesPerBlock(Fmt f) {
    switch (f) {
        case Fmt::Dxt1: return 8;
        case Fmt::Dxt3:
        case Fmt::Dxt5:
        case Fmt::Ati2: return 16;
        default: return 0;
    }
}

} // namespace

bool Decode(const uint8_t* data, size_t size, int& outWidth, int& outHeight, std::vector<uint8_t>& outRGBA) {
    DdsHeader h;
    if (!ParseHeader(data, size, h)) return false;

    const Fmt fmt = DetectFormat(h);
    if (fmt == Fmt::Unsupported) return false;

    const int w = static_cast<int>(h.width), hgt = static_cast<int>(h.height);
    outRGBA.assign(static_cast<size_t>(w) * hgt * 4, 0);
    outWidth = w;
    outHeight = hgt;

    const int blocksX = (w + 3) / 4, blocksY = (hgt + 3) / 4;
    const int bpp = BytesPerBlock(fmt);
    const size_t needed = static_cast<size_t>(blocksX) * blocksY * bpp;
    if (h.pixelBytes < needed) return false;

    for (int by = 0; by < blocksY; by++) {
        for (int bx = 0; bx < blocksX; bx++) {
            const uint8_t* block = h.pixels + (static_cast<size_t>(by) * blocksX + bx) * bpp;
            uint8_t blockRGBA[16 * 4];
            switch (fmt) {
                case Fmt::Dxt1: DecodeDxt1(block, blockRGBA); break;
                case Fmt::Dxt3: DecodeDxt3(block, blockRGBA); break;
                case Fmt::Dxt5: DecodeDxt5(block, blockRGBA); break;
                case Fmt::Ati2: DecodeAti2(block, blockRGBA); break;
                default: return false;
            }
            for (int py = 0; py < 4; py++) {
                const int srcY = by * 4 + py;
                if (srcY >= hgt) continue;
                for (int px = 0; px < 4; px++) {
                    const int srcX = bx * 4 + px;
                    if (srcX >= w) continue;
                    const int so = (py * 4 + px) * 4;
                    const int doff = (static_cast<size_t>(srcY) * w + srcX) * 4;
                    outRGBA[doff + 0] = blockRGBA[so + 0];
                    outRGBA[doff + 1] = blockRGBA[so + 1];
                    outRGBA[doff + 2] = blockRGBA[so + 2];
                    outRGBA[doff + 3] = blockRGBA[so + 3];
                }
            }
        }
    }
    return true;
}

bool IsSupportedFormat(const uint8_t* data, size_t size) {
    DdsHeader h;
    if (!ParseHeader(data, size, h)) return false;
    return DetectFormat(h) != Fmt::Unsupported;
}

} // namespace DdsDecoder
