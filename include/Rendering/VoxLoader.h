#pragma once
#include "Platform/Export.h"
#ifndef VOX_LOADER_H
#define VOX_LOADER_H

#include <vector>
#include <string>
#include <cstdint>
#include <glm/glm.hpp>
#include "ModelLoader.h"

// 简化的VOX文件解析器
namespace VoxFormat {

// VOX文件格式常量
// 文件中存储为 "VOX " (56 4F 58 20)，小端序读取后为 0x20584F56
constexpr uint32_t VOX_MAGIC = 0x20584F56;
constexpr uint32_t VOX_VERSION = 150;

// Chunk 类型 (小端序)
constexpr uint32_t CHUNK_MAIN = 0x4E49414D;  // "NIAM" (MAIN in little-endian)
constexpr uint32_t CHUNK_SIZE = 0x455A4953;  // "EZIS" (SIZE in little-endian)
constexpr uint32_t CHUNK_XYZI = 0x495A5958;  // "IZYX" (XYZI in little-endian)
constexpr uint32_t CHUNK_RGBA = 0x41424752;  // "ABGR" (RGBA in little-endian)
constexpr uint32_t CHUNK_IMAP = 0x50414D49;  // "PAMI" (IMAP in little-endian)
constexpr uint32_t CHUNK_MATT = 0x5454414D;  // "TTAM" (MATT in little-endian)

// 体素数据
struct MIKAN_API Voxel {
    uint8_t x, y, z;
    uint8_t colorIndex;
};

// 调色板颜色 (RGBA)
struct MIKAN_API Color {
    uint8_t r, g, b, a;
};

// 模型数据
struct MIKAN_API Model {
    uint32_t sizeX, sizeY, sizeZ;
    std::vector<Voxel> voxels;
};

// VOX文件数据
struct MIKAN_API VoxData {
    std::vector<Model> models;
    Color palette[256];  // 默认调色板 + 自定义调色板
    bool hasCustomPalette = false;
    
    VoxData();
};

// 加载VOX文件
bool LoadVoxFile(const std::string& path, VoxData& outData);

} // namespace VoxFormat

#endif // VOX_LOADER_H
