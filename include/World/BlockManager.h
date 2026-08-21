#ifndef BLOCK_MANAGER_H
#define BLOCK_MANAGER_H
#pragma once
#include <glm/glm.hpp>
#include <unordered_map>
#include <string>
#include <fstream>
#include <sstream>
#include <vector>
#include <array>
#include <bitset>
struct BlockType {
    std::string name;
    bool isTransparent;
    bool isSolid;
    int frontTexIndex;   // +Z (0)
    int backTexIndex;    // -Z (1)
    int leftTexIndex;    // -X (2)
    int rightTexIndex;   // +X (3)
    int topTexIndex;     // +Y (4)
    int bottomTexIndex;  // -Y (5)
    float hardness;
    int dropId;
    bool emitsLight;
    int lightLevel;
    int blockType;       // 0: 立方体, 1: 植物十字交叉, 2: 模型
};

class BlockManager {
private:
    static BlockManager* instance;
    std::unordered_map<int, BlockType> blockTypes;
    std::array<BlockType, 256> fastblockTypes; // 假设ID范围0-255
    std::bitset<256> transparentBlocks;

    BlockManager();
    void LoadFromCSV(const std::string& filePath);
    BlockType ParseCSVRow(const std::vector<std::string>& row);

public:
    bool IsTransparent(int id) const {
        if (id < 0 || id >= 256) return false;
        return transparentBlocks.test(id);
    }
    BlockManager(const BlockManager&) = delete;
    BlockManager& operator=(const BlockManager&) = delete;
    const std::string& GetBlockName(int id) const;
    static BlockManager& GetInstance();
    void RegisterBlock(int id, const BlockType& type);
    const BlockType& GetBlockType(int id) const;

    static const int BLOCK_TYPE_CUBE = 0;      // 标准立方体
    static const int BLOCK_TYPE_PLANT = 1;     // 植物十字交叉
    static const int BLOCK_TYPE_MODEL = 2;     // 自定义模型
    int GetBlockRenderType(int id) const {
        return GetBlockType(id).blockType;
    }
    const BlockType* GetBlockTypesArray() const {
        return fastblockTypes.data();
    }
    // 预定义方块ID常量
    static const int AIR = 0;
    static const int STONE = 1;
    static const int DIRT = 2;
    static const int GRASSBLOCK = 3;
    static const int WOOD = 4;
    static const int LEAVES = 5;
    static const int GLASS = 6;
    static const int SNOW = 7;
    static const int SAND = 8;
    static const int BRICK = 9;
    static const int PLANK = 10;
    static const int LIGHT = 11;
    static const int WATER = 12;
    static const int ROSE = 13;
    static const int GRASS = 14;
};

#endif // BLOCK_MANAGER_H