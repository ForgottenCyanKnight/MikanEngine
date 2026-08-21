// WorldTypes.h - 体素世界配置类型
// 从旧版 (D:\mikan engine) EngineConfig.h 提取的与体素世界相关的部分，
// 避免与 Vulkan 版 EngineConfig 命名冲突。
#pragma once
#include <string>

enum ConfigTerrainType {
    FLAT,
    PERLIN,
    ADVANCED
};

// 体素世界全局配置（对应旧版 engineConfig 中世界相关字段）
struct WorldConfig {
    std::string AssetPath = "";   // 资源根目录（BlockManager 加载 blocks.csv 用）
    ConfigTerrainType terrainType = ADVANCED;
    int renderDistance = 6;       // 世界渲染半径（chunk 数）
};

// 全局世界配置（外部可设置）
WorldConfig& GetWorldConfig();
