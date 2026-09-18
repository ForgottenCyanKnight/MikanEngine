#include "Rendering/HeightmapLoader.h"

#include "Core/EngineConfig.h"

#include <SDL3/SDL_iostream.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

#include "zlib/zlib.h"

namespace {

constexpr uint8_t kPngSignature[8] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a
};

constexpr uint32_t kMaxDimension = 32768;

void SetError(std::string* errorMessage, const char* message) {
    if (errorMessage) {
        *errorMessage = message ? message : "unknown heightmap error";
    }
}

uint32_t ReadBE32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

uint8_t PaethPredictor(uint8_t left, uint8_t above, uint8_t upperLeft) {
    const int p = static_cast<int>(left) + static_cast<int>(above) - static_cast<int>(upperLeft);
    const int pa = std::abs(p - static_cast<int>(left));
    const int pb = std::abs(p - static_cast<int>(above));
    const int pc = std::abs(p - static_cast<int>(upperLeft));
    if (pa <= pb && pa <= pc) return left;
    if (pb <= pc) return above;
    return upperLeft;
}

bool ReadFileBytes(const std::string& filePath,
                   std::vector<uint8_t>& bytes,
                   std::string* errorMessage) {
    const std::string resolvedPath = EngineConfig::ResolvePlatformPath(filePath);
    SDL_IOStream* io = SDL_IOFromFile(resolvedPath.c_str(), "rb");
    if (!io) {
        SetError(errorMessage, SDL_GetError());
        return false;
    }

    const Sint64 size = SDL_GetIOSize(io);
    if (size <= 0 || static_cast<uint64_t>(size) > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        SDL_CloseIO(io);
        SetError(errorMessage, "empty or invalid PNG file size");
        return false;
    }

    bytes.resize(static_cast<size_t>(size));
    const size_t read = SDL_ReadIO(io, bytes.data(), bytes.size());
    SDL_CloseIO(io);
    if (read != bytes.size()) {
        bytes.clear();
        SetError(errorMessage, "failed to read PNG file");
        return false;
    }
    return true;
}

bool IsChunk(const uint8_t* type, char a, char b, char c, char d) {
    return type[0] == static_cast<uint8_t>(a) &&
           type[1] == static_cast<uint8_t>(b) &&
           type[2] == static_cast<uint8_t>(c) &&
           type[3] == static_cast<uint8_t>(d);
}

} // namespace

namespace HeightmapLoader {

bool LoadPng16(const std::string& filePath,
               HeightmapPixels16& out,
               std::string* errorMessage) {
    out.Clear();
    if (errorMessage) errorMessage->clear();

    std::vector<uint8_t> fileBytes;
    if (!ReadFileBytes(filePath, fileBytes, errorMessage)) {
        return false;
    }

    if (fileBytes.size() < sizeof(kPngSignature) ||
        std::memcmp(fileBytes.data(), kPngSignature, sizeof(kPngSignature)) != 0) {
        SetError(errorMessage, "not a PNG file");
        return false;
    }

    uint32_t width = 0;
    uint32_t height = 0;
    uint8_t bitDepth = 0;
    uint8_t colorType = 0;
    uint8_t compressionMethod = 0;
    uint8_t filterMethod = 0;
    uint8_t interlaceMethod = 0;
    bool hasIHDR = false;
    bool hasIEND = false;
    std::vector<uint8_t> compressed;

    size_t offset = sizeof(kPngSignature);
    while (offset < fileBytes.size()) {
        if (fileBytes.size() - offset < 12) {
            SetError(errorMessage, "truncated PNG chunk header");
            return false;
        }

        const uint32_t chunkLength = ReadBE32(fileBytes.data() + offset);
        offset += 4;
        const uint8_t* chunkType = fileBytes.data() + offset;
        offset += 4;

        if (static_cast<size_t>(chunkLength) > fileBytes.size() - offset - 4) {
            SetError(errorMessage, "truncated PNG chunk data");
            return false;
        }

        const uint8_t* chunkData = fileBytes.data() + offset;
        offset += static_cast<size_t>(chunkLength);
        const uint32_t storedCrc = ReadBE32(fileBytes.data() + offset);
        offset += 4;

        uLong computedCrc = crc32(0L, Z_NULL, 0);
        computedCrc = crc32(computedCrc, chunkType, 4);
        if (chunkLength > 0) {
            computedCrc = crc32(computedCrc, chunkData, static_cast<uInt>(chunkLength));
        }
        if (static_cast<uint32_t>(computedCrc) != storedCrc) {
            SetError(errorMessage, "PNG chunk CRC mismatch");
            return false;
        }

        if (IsChunk(chunkType, 'I', 'H', 'D', 'R')) {
            if (hasIHDR || chunkLength != 13) {
                SetError(errorMessage, "invalid PNG IHDR");
                return false;
            }
            width = ReadBE32(chunkData + 0);
            height = ReadBE32(chunkData + 4);
            bitDepth = chunkData[8];
            colorType = chunkData[9];
            compressionMethod = chunkData[10];
            filterMethod = chunkData[11];
            interlaceMethod = chunkData[12];
            hasIHDR = true;
        } else if (IsChunk(chunkType, 'I', 'D', 'A', 'T')) {
            if (chunkLength > 0) {
                const size_t oldSize = compressed.size();
                compressed.resize(oldSize + static_cast<size_t>(chunkLength));
                std::memcpy(compressed.data() + oldSize, chunkData, chunkLength);
            }
        } else if (IsChunk(chunkType, 'I', 'E', 'N', 'D')) {
            if (chunkLength != 0) {
                SetError(errorMessage, "invalid PNG IEND");
                return false;
            }
            hasIEND = true;
            break;
        }
    }

    if (!hasIHDR || !hasIEND || compressed.empty()) {
        SetError(errorMessage, "PNG is missing IHDR, IDAT, or IEND");
        return false;
    }
    if (width == 0 || height == 0 || width > kMaxDimension || height > kMaxDimension) {
        SetError(errorMessage, "PNG dimensions are invalid or too large");
        return false;
    }
    if (bitDepth != 16) {
        SetError(errorMessage, "heightmap PNG must use 16-bit samples");
        return false;
    }
    if (colorType != 0 && colorType != 4) {
        SetError(errorMessage, "heightmap PNG must be grayscale or grayscale+alpha");
        return false;
    }
    if (compressionMethod != 0 || filterMethod != 0) {
        SetError(errorMessage, "unsupported PNG compression or filter method");
        return false;
    }
    if (interlaceMethod != 0) {
        SetError(errorMessage, "interlaced PNG heightmaps are not supported yet");
        return false;
    }
    if (compressed.size() > static_cast<size_t>(std::numeric_limits<uInt>::max())) {
        SetError(errorMessage, "PNG compressed data is too large");
        return false;
    }

    const size_t channels = colorType == 4 ? 2u : 1u;
    const size_t bytesPerPixel = channels * sizeof(uint16_t);
    if (width > std::numeric_limits<size_t>::max() / bytesPerPixel) {
        SetError(errorMessage, "PNG row size overflow");
        return false;
    }
    const size_t rowBytes = static_cast<size_t>(width) * bytesPerPixel;
    if (rowBytes > std::numeric_limits<size_t>::max() - 1 ||
        static_cast<size_t>(height) > std::numeric_limits<size_t>::max() / (rowBytes + 1)) {
        SetError(errorMessage, "PNG image size overflow");
        return false;
    }
    const size_t inflatedSize = (rowBytes + 1) * static_cast<size_t>(height);
    if (inflatedSize > static_cast<size_t>(std::numeric_limits<uInt>::max())) {
        SetError(errorMessage, "PNG scanline data is too large for the bundled zlib build");
        return false;
    }

    std::vector<uint8_t> inflated(inflatedSize);
    z_stream stream{};
    stream.next_in = compressed.data();
    stream.avail_in = static_cast<uInt>(compressed.size());
    stream.next_out = inflated.data();
    stream.avail_out = static_cast<uInt>(inflated.size());

    int zResult = inflateInit(&stream);
    if (zResult != Z_OK) {
        SetError(errorMessage, "failed to initialize PNG decompressor");
        return false;
    }

    do {
        zResult = inflate(&stream, Z_NO_FLUSH);
        if (zResult == Z_STREAM_END) break;
        if (zResult != Z_OK || stream.avail_out == 0) {
            inflateEnd(&stream);
            SetError(errorMessage, "failed to decompress PNG image data");
            return false;
        }
    } while (stream.avail_in > 0);

    const bool inflateOk = zResult == Z_STREAM_END &&
                           stream.total_out == static_cast<uLong>(inflatedSize);
    inflateEnd(&stream);
    if (!inflateOk) {
        SetError(errorMessage, "PNG decompressed size does not match its dimensions");
        return false;
    }

    out.width = width;
    out.height = height;
    out.samples.resize(static_cast<size_t>(width) * static_cast<size_t>(height));

    std::vector<uint8_t> previousRow(rowBytes, 0);
    std::vector<uint8_t> currentRow(rowBytes, 0);
    size_t scanlineOffset = 0;
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t filter = inflated[scanlineOffset++];
        const uint8_t* filteredRow = inflated.data() + scanlineOffset;
        scanlineOffset += rowBytes;

        for (size_t i = 0; i < rowBytes; ++i) {
            const uint8_t left = i >= bytesPerPixel ? currentRow[i - bytesPerPixel] : 0;
            const uint8_t above = previousRow[i];
            const uint8_t upperLeft = i >= bytesPerPixel ? previousRow[i - bytesPerPixel] : 0;
            uint8_t predictor = 0;
            switch (filter) {
            case 0:
                predictor = 0;
                break;
            case 1:
                predictor = left;
                break;
            case 2:
                predictor = above;
                break;
            case 3:
                predictor = static_cast<uint8_t>((static_cast<unsigned>(left) + static_cast<unsigned>(above)) / 2u);
                break;
            case 4:
                predictor = PaethPredictor(left, above, upperLeft);
                break;
            default:
                SetError(errorMessage, "unsupported PNG scanline filter");
                out.Clear();
                return false;
            }
            currentRow[i] = static_cast<uint8_t>(filteredRow[i] + predictor);
        }

        for (uint32_t x = 0; x < width; ++x) {
            const size_t sampleOffset = static_cast<size_t>(x) * bytesPerPixel;
            const uint16_t sample = static_cast<uint16_t>(
                (static_cast<uint16_t>(currentRow[sampleOffset]) << 8) |
                static_cast<uint16_t>(currentRow[sampleOffset + 1]));
            out.samples[static_cast<size_t>(y) * width + x] = sample;
        }

        std::swap(previousRow, currentRow);
    }

    return out.IsValid();
}

// ===== PNG 编码 =====

namespace {

void WriteBE32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value >> 24));
    out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
}

void AppendChunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    WriteBE32(out, static_cast<uint32_t>(data.size()));
    const uint8_t* typeBytes = reinterpret_cast<const uint8_t*>(type);
    out.insert(out.end(), typeBytes, typeBytes + 4);
    out.insert(out.end(), data.begin(), data.end());
    uLong crc = crc32(0L, typeBytes, 4);
    if (!data.empty()) {
        crc = crc32(crc, data.data(), static_cast<uInt>(data.size()));
    }
    WriteBE32(out, static_cast<uint32_t>(crc));
}

// 组装 PNG：签名 + IHDR + IDAT(stored-block zlib) + IEND。
// scanlines 已含每行行首 filter 字节（调用方统一写 0）。
bool EncodePng(uint32_t width, uint32_t height, uint8_t bitDepth, uint8_t colorType,
               const std::vector<uint8_t>& scanlines, std::vector<uint8_t>& out,
               std::string* errorMessage) {
    out.clear();
    static const uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    out.insert(out.end(), kSignature, kSignature + sizeof(kSignature));

    std::vector<uint8_t> ihdr;
    WriteBE32(ihdr, width);
    WriteBE32(ihdr, height);
    ihdr.push_back(bitDepth);
    ihdr.push_back(colorType);
    ihdr.push_back(0); // compression: deflate
    ihdr.push_back(0); // filter: adaptive
    ihdr.push_back(0); // interlace: none
    AppendChunk(out, "IHDR", ihdr);

    // zlib 流头（0x78 0x01：无预设字典、最快），数据按 stored deflate 块直排。
    std::vector<uint8_t> idat;
    idat.push_back(0x78);
    idat.push_back(0x01);

    uint32_t adler = adler32(0L, Z_NULL, 0);
    const size_t rawSize = scanlines.size();
    for (size_t offset = 0; offset < rawSize || offset == 0; offset += 65535) {
        const size_t block = std::min<size_t>(65535, rawSize - offset);
        const bool finalBlock = offset + block >= rawSize;
        idat.push_back(static_cast<uint8_t>(finalBlock ? 1 : 0));
        const uint16_t len = static_cast<uint16_t>(block);
        idat.push_back(static_cast<uint8_t>(len & 0xFF));
        idat.push_back(static_cast<uint8_t>(len >> 8));
        idat.push_back(static_cast<uint8_t>(~len & 0xFF));
        idat.push_back(static_cast<uint8_t>((~len >> 8) & 0xFF));
        if (block > 0) {
            idat.insert(idat.end(), scanlines.begin() + static_cast<ptrdiff_t>(offset),
                        scanlines.begin() + static_cast<ptrdiff_t>(offset + block));
            adler = adler32(adler, scanlines.data() + offset, static_cast<uInt>(block));
        }
        if (rawSize == 0) {
            break; // 空输入也至少写一个空块，保证流合法。
        }
    }
    WriteBE32(idat, adler);
    AppendChunk(out, "IDAT", idat);

    static const std::vector<uint8_t> kEmpty;
    AppendChunk(out, "IEND", kEmpty);
    if (errorMessage) errorMessage->clear();
    return true;
}

bool WriteFileBytes(const std::string& filePath, const std::vector<uint8_t>& bytes,
                    std::string* errorMessage) {
    // 注意：写盘不做 ResolvePlatformPath——产物路径由调用方（编辑器持久化
    // 钩子）解析为绝对路径，笔刷保存必须写进工程目录而不是引擎目录。
    SDL_IOStream* io = SDL_IOFromFile(filePath.c_str(), "wb");
    if (!io) {
        if (errorMessage) {
            *errorMessage = std::string("SDL_IOFromFile(wb) failed: ") + SDL_GetError();
        }
        return false;
    }
    const size_t written = bytes.empty()
        ? 0u
        : SDL_WriteIO(io, bytes.data(), bytes.size());
    const bool ok = written == bytes.size();
    SDL_CloseIO(io);
    if (!ok && errorMessage) {
        *errorMessage = "SDL_WriteIO wrote fewer bytes than expected";
    }
    return ok;
}

} // namespace

bool SavePng16(const std::string& filePath,
               uint32_t width, uint32_t height,
               const uint16_t* samples,
               std::string* errorMessage) {
    if (errorMessage) errorMessage->clear();
    if (width == 0 || height == 0 || samples == nullptr) {
        if (errorMessage) *errorMessage = "SavePng16: empty image";
        return false;
    }

    // 每行 = filter 字节 + width × 2 字节大端样本。
    std::vector<uint8_t> scanlines;
    scanlines.resize(static_cast<size_t>(height) * (1 + static_cast<size_t>(width) * 2));
    size_t cursor = 0;
    for (uint32_t y = 0; y < height; ++y) {
        scanlines[cursor++] = 0; // filter none
        const uint16_t* row = samples + static_cast<size_t>(y) * width;
        for (uint32_t x = 0; x < width; ++x) {
            scanlines[cursor++] = static_cast<uint8_t>(row[x] >> 8);
            scanlines[cursor++] = static_cast<uint8_t>(row[x] & 0xFF);
        }
    }

    std::vector<uint8_t> file;
    if (!EncodePng(width, height, 16, 0, scanlines, file, errorMessage) ||
        !WriteFileBytes(filePath, file, errorMessage)) {
        return false;
    }
    if (errorMessage) errorMessage->clear();
    return true;
}

bool SavePng8(const std::string& filePath,
              uint32_t width, uint32_t height, uint32_t channels,
              const uint8_t* pixels,
              std::string* errorMessage) {
    if (errorMessage) errorMessage->clear();
    if (width == 0 || height == 0 || pixels == nullptr ||
        (channels != 1 && channels != 4)) {
        if (errorMessage) *errorMessage = "SavePng8: unsupported image or channel count";
        return false;
    }

    std::vector<uint8_t> scanlines;
    scanlines.resize(static_cast<size_t>(height) *
                     (1 + static_cast<size_t>(width) * channels));
    size_t cursor = 0;
    for (uint32_t y = 0; y < height; ++y) {
        scanlines[cursor++] = 0; // filter none
        const uint8_t* row = pixels + static_cast<size_t>(y) * width * channels;
        std::memcpy(scanlines.data() + cursor, row, static_cast<size_t>(width) * channels);
        cursor += static_cast<size_t>(width) * channels;
    }

    const uint8_t colorType = channels == 1 ? 0 : 6;
    std::vector<uint8_t> file;
    if (!EncodePng(width, height, 8, colorType, scanlines, file, errorMessage) ||
        !WriteFileBytes(filePath, file, errorMessage)) {
        return false;
    }
    if (errorMessage) errorMessage->clear();
    return true;
}

} // namespace HeightmapLoader
