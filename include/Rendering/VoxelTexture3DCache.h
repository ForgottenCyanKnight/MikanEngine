#pragma once
#include "Platform/Export.h"
#ifndef VOXEL_TEXTURE3D_CACHE_H
#define VOXEL_TEXTURE3D_CACHE_H

#include "VoxLoader.h"
#include <string>
#include <vector>
#include <cstdint>

namespace VoxelCache {

// Texture3D 缓存文件头
struct MIKAN_API Texture3DCacheHeader {
    uint32_t magic;             // 'T3DC' = 0x43443354
    uint32_t version;           // 缓存版本
    uint32_t sizeX, sizeY, sizeZ; // 体素网格尺寸
    uint32_t voxelCount;        // 有效体素数量
    uint32_t paletteSize;       // 调色板大小 (字节)
    uint64_t dataSize;          // 体素数据大小 (字节)
    uint64_t timestamp;         // 源文件时间戳
    uint8_t reserved[32];       // 保留字段
};

static_assert(sizeof(Texture3DCacheHeader) == 80, "Texture3DCacheHeader size mismatch");

// 缓存格式版本
constexpr uint32_t CACHE_VERSION = 1;
constexpr uint32_t CACHE_MAGIC = 0x43443354;  // "T3DC" (little-endian)

// Texture3D 缓存生成器
class MIKAN_API Texture3DCacheGenerator {
public:
    Texture3DCacheGenerator();
    ~Texture3DCacheGenerator();
    
    // 从 VoxData 生成 Texture3D 缓存
    bool GenerateCache(const VoxFormat::VoxData& voxData, 
                      const std::string& sourcePath,
                      const std::string& cachePath);
    
    // 加载已有的缓存
    bool LoadCache(const std::string& cachePath,
                  std::vector<uint8_t>& outVoxelData,
                  std::vector<VoxFormat::Color>& outPalette,
                  uint32_t& outSizeX, uint32_t& outSizeY, uint32_t& outSizeZ);
    
    // 检查缓存是否有效
    bool IsCacheValid(const std::string& cachePath, const std::string& sourcePath);
    
    // 获取缓存文件路径
    static std::string GetCachePath(const std::string& sourcePath);
    
    // 估算缓存文件大小
    static size_t EstimateCacheSize(uint32_t sizeX, uint32_t sizeY, uint32_t sizeZ);
    
private:
    // 构建 3D 纹理数据（R8 格式：调色板索引）
    void BuildVoxelGrid(const VoxFormat::VoxData& voxData,
                       std::vector<uint8_t>& gridData,
                       uint32_t sizeX, uint32_t sizeY, uint32_t sizeZ);
    
    // 优化调色板（移除未使用的颜色）
    void OptimizePalette(const VoxFormat::VoxData& voxData,
                        std::vector<VoxFormat::Color>& outPalette,
                        std::vector<uint8_t>& outRemap);
    
    // 压缩体素数据（游程编码）
    void CompressVoxelData(const std::vector<uint8_t>& input,
                          std::vector<uint8_t>& output);
    
    // 解压体素数据
    void DecompressVoxelData(const std::vector<uint8_t>& input,
                            std::vector<uint8_t>& output,
                            size_t expectedSize);
};

} // namespace VoxelCache

#endif // VOXEL_TEXTURE3D_CACHE_H
