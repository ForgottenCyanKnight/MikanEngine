#include "Rendering/VoxelTexture3DCache.h"
#include "EngineGlobal.h"
#include <fstream>
#include <iostream>
#include <filesystem>
#include <chrono>
#include <unordered_set>

namespace VoxelCache {

Texture3DCacheGenerator::Texture3DCacheGenerator() {
}

Texture3DCacheGenerator::~Texture3DCacheGenerator() {
}

std::string Texture3DCacheGenerator::GetCachePath(const std::string& sourcePath) {
    if (sourcePath.empty()) {
        return "";
    }
    
    // 获取 vox 文件所在目录
    std::filesystem::path path(sourcePath);
    std::filesystem::path parentDir = path.parent_path();
    std::string stem = path.stem().string();
    
    // 在 Android 上使用 assets 目录（只读）
#ifdef ANDROID_BUILD
    // Android 只读取 assets 中的缓存，不生成新缓存
    std::filesystem::path cacheDir = parentDir / "texture3d";
    std::string cacheFile = stem + "_texture3d.bin";
    return (cacheDir / cacheFile).string();
#else
    // 在 vox 文件同级目录下的 texture3d 子目录
    std::filesystem::path cacheDir = parentDir / "texture3d";
    
    // 确保目录存在
    std::filesystem::create_directories(cacheDir);
    
    // 缓存文件名：{modelname}_texture3d.bin
    std::string cacheFile = stem + "_texture3d.bin";
    
    return (cacheDir / cacheFile).string();
#endif
}

size_t Texture3DCacheGenerator::EstimateCacheSize(uint32_t sizeX, uint32_t sizeY, uint32_t sizeZ) {
    // Header: 80 bytes
    // Palette: 256 * 4 = 1024 bytes (最多)
    // Voxel data: sizeX * sizeY * sizeZ bytes (R8 format)
    return 80 + 1024 + (size_t)sizeX * sizeY * sizeZ;
}

bool Texture3DCacheGenerator::GenerateCache(const VoxFormat::VoxData& voxData,
                                           const std::string& sourcePath,
                                           const std::string& cachePath) {
    if (voxData.models.empty()) {
        std::cerr << "[Texture3DCache] No models in vox data" << std::endl;
        return false;
    }
    
    const auto& model = voxData.models[0];
    uint32_t sizeX = model.sizeX;
    uint32_t sizeY = model.sizeY;
    uint32_t sizeZ = model.sizeZ;
    
    std::cout << "[Texture3DCache] Generating cache for " << sizeX << "x" 
              << sizeY << "x" << sizeZ << " grid..." << std::endl;
    
    // 1. 优化调色板（移除未使用的颜色）
    std::vector<VoxFormat::Color> optimizedPalette;
    std::vector<uint8_t> colorRemap;
    OptimizePalette(voxData, optimizedPalette, colorRemap);
    
    std::cout << "[Texture3DCache] Optimized palette: " << optimizedPalette.size() 
              << " colors (from 256)" << std::endl;
    
    // 2. 构建 3D 网格数据（R8 格式）
    std::vector<uint8_t> voxelGrid;
    BuildVoxelGrid(voxData, voxelGrid, sizeX, sizeY, sizeZ);
    
    std::cout << "[Texture3DCache] Voxel grid built: " << voxelGrid.size() 
              << " bytes" << std::endl;
    
    // 3. 压缩体素数据（可选，这里先不压缩）
    // std::vector<uint8_t> compressedData;
    // CompressVoxelData(voxelGrid, compressedData);
    
    // 4. 写入缓存文件
    std::ofstream file(cachePath, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "[Texture3DCache] Failed to create cache file: " 
                  << cachePath << std::endl;
        return false;
    }
    
    // 4.1 写入文件头
    Texture3DCacheHeader header = {};
    header.magic = CACHE_MAGIC;
    header.version = CACHE_VERSION;
    header.sizeX = sizeX;
    header.sizeY = sizeY;
    header.sizeZ = sizeZ;
    header.voxelCount = model.voxels.size();
    header.paletteSize = optimizedPalette.size() * sizeof(VoxFormat::Color);
    header.dataSize = voxelGrid.size();
    
    // 获取源文件时间戳
    if (!sourcePath.empty()) {
        try {
            auto fileTime = std::filesystem::last_write_time(sourcePath);
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                fileTime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
            header.timestamp = sctp.time_since_epoch().count();
        } catch (...) {
            header.timestamp = 0;
        }
    }
    
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    
    // 4.2 写入调色板
    file.write(reinterpret_cast<const char*>(optimizedPalette.data()), 
               header.paletteSize);
    
    // 4.3 写入体素数据
    file.write(reinterpret_cast<const char*>(voxelGrid.data()), 
               header.dataSize);
    
    file.close();
    
    size_t fileSize = 80 + header.paletteSize + header.dataSize;
    std::cout << "[Texture3DCache] Cache file created: " << cachePath << std::endl;
    std::cout << "[Texture3DCache] File size: " << fileSize << " bytes (" 
              << (fileSize / 1024.0) << " KB)" << std::endl;
    
    return true;
}

bool Texture3DCacheGenerator::LoadCache(const std::string& cachePath,
                                       std::vector<uint8_t>& outVoxelData,
                                       std::vector<VoxFormat::Color>& outPalette,
                                       uint32_t& outSizeX, uint32_t& outSizeY, uint32_t& outSizeZ) {
    std::ifstream file(cachePath, std::ios::binary);
    if (!file.is_open()) {
        std::cout << "[Texture3DCache] Cache file not found: " << cachePath << std::endl;
        return false;
    }
    
    // 1. 读取文件头
    Texture3DCacheHeader header;
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    
    if (header.magic != CACHE_MAGIC) {
        std::cerr << "[Texture3DCache] Invalid cache magic: " << header.magic 
                  << " (expected " << CACHE_MAGIC << ")" << std::endl;
        return false;
    }
    
    if (header.version != CACHE_VERSION) {
        std::cerr << "[Texture3DCache] Unsupported cache version: " << header.version 
                  << " (expected " << CACHE_VERSION << ")" << std::endl;
        return false;
    }
    
    outSizeX = header.sizeX;
    outSizeY = header.sizeY;
    outSizeZ = header.sizeZ;
    
    std::cout << "[Texture3DCache] Loading cache: " << outSizeX << "x" 
              << outSizeY << "x" << outSizeZ << std::endl;
    
    // 2. 读取调色板
    size_t paletteCount = header.paletteSize / sizeof(VoxFormat::Color);
    outPalette.resize(paletteCount);
    file.read(reinterpret_cast<char*>(outPalette.data()), header.paletteSize);
    
    // 3. 读取体素数据
    outVoxelData.resize(header.dataSize);
    file.read(reinterpret_cast<char*>(outVoxelData.data()), header.dataSize);
    
    file.close();
    
    std::cout << "[Texture3DCache] Cache loaded successfully!" << std::endl;
    std::cout << "  Palette: " << paletteCount << " colors" << std::endl;
    std::cout << "  Voxel data: " << header.dataSize << " bytes" << std::endl;
    
    return true;
}

bool Texture3DCacheGenerator::IsCacheValid(const std::string& cachePath, 
                                          const std::string& sourcePath) {
    // 1. 检查缓存文件是否存在
    if (!std::filesystem::exists(cachePath)) {
        std::cout << "[Texture3DCache] Cache file does not exist" << std::endl;
        return false;
    }
    
    // 2. 检查源文件是否存在
    if (!std::filesystem::exists(sourcePath)) {
        std::cout << "[Texture3DCache] Source file does not exist" << std::endl;
        return false;
    }
    
    // 3. 读取缓存文件头
    std::ifstream file(cachePath, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }
    
    Texture3DCacheHeader header;
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    file.close();
    
    if (header.magic != CACHE_MAGIC) {
        return false;
    }
    
    // 4. 比较时间戳
    try {
        auto sourceTime = std::filesystem::last_write_time(sourcePath);
        auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            sourceTime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
        uint64_t sourceTimestamp = sctp.time_since_epoch().count();
        
        if (header.timestamp != sourceTimestamp) {
            std::cout << "[Texture3DCache] Cache outdated (source file modified)" << std::endl;
            return false;
        }
    } catch (...) {
        return false;
    }
    
    return true;
}

void Texture3DCacheGenerator::BuildVoxelGrid(const VoxFormat::VoxData& voxData,
                                            std::vector<uint8_t>& gridData,
                                            uint32_t sizeX, uint32_t sizeY, uint32_t sizeZ) {
    // 初始化网格（0 表示空）
    size_t totalSize = (size_t)sizeX * sizeY * sizeZ;
    gridData.assign(totalSize, 0);
    
    if (voxData.models.empty()) {
        return;
    }
    
    const auto& model = voxData.models[0];
    
    // 填充体素数据
    for (const auto& voxel : model.voxels) {
        if (voxel.x < sizeX && voxel.y < sizeY && voxel.z < sizeZ) {
            size_t index = (size_t)voxel.z * sizeY * sizeX + 
                          (size_t)voxel.y * sizeX + 
                          (size_t)voxel.x;
            
            // 存储调色板索引（1-255，0 保留为空）
            gridData[index] = voxel.colorIndex;
        }
    }
}

void Texture3DCacheGenerator::OptimizePalette(const VoxFormat::VoxData& voxData,
                                             std::vector<VoxFormat::Color>& outPalette,
                                             std::vector<uint8_t>& outRemap) {
    // 1. 收集所有使用的颜色索引
    std::unordered_set<uint8_t> usedColors;
    
    for (const auto& model : voxData.models) {
        for (const auto& voxel : model.voxels) {
            usedColors.insert(voxel.colorIndex);
        }
    }
    
    // 2. 创建新的紧凑调色板
    outPalette.clear();
    outPalette.reserve(usedColors.size());
    outRemap.assign(256, 0);
    
    // 索引 0 保留为空（黑色透明）
    VoxFormat::Color emptyColor = {0, 0, 0, 0};
    outPalette.push_back(emptyColor);
    
    uint8_t newIndex = 1;
    for (uint8_t oldIndex : usedColors) {
        if (oldIndex == 0) continue;  // 跳过空颜色
        
        outPalette.push_back(voxData.palette[oldIndex]);
        outRemap[oldIndex] = newIndex;
        newIndex++;
    }
    
    std::cout << "[Texture3DCache] Palette optimized: " << outPalette.size() 
              << " colors used" << std::endl;
}

void Texture3DCacheGenerator::CompressVoxelData(const std::vector<uint8_t>& input,
                                               std::vector<uint8_t>& output) {
    // TODO: 实现游程编码（RLE）压缩
    // 对于稀疏体素场景，可以显著减少文件大小
    output = input;  // 暂时不压缩
}

void Texture3DCacheGenerator::DecompressVoxelData(const std::vector<uint8_t>& input,
                                                 std::vector<uint8_t>& output,
                                                 size_t expectedSize) {
    // TODO: 实现 RLE 解压
    output = input;
    output.resize(expectedSize);
}

} // namespace VoxelCache
