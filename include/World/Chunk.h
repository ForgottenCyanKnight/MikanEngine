// Chunk.h
#ifndef CHUNK_H
#define CHUNK_H
#pragma once
#include <glm/glm.hpp>
#include <vector>
#include <cstdint>
#include "World/BlockManager.h"
#include "FastNoiseLite.h"
#include "AABB.h"
#include "World/WorldTypes.h"

struct DrawElementsIndirectCommand {
    uint32_t vertexCount;    // 每个实例的顶点数
    uint32_t instanceCount;  // 实例数量
    uint32_t firstIndex;     // 索引缓冲区中的起始索引
    uint32_t baseVertex;     // 顶点缓冲区中的基础顶点
    uint32_t baseInstance;   // 实例数据的基础索引
};

namespace std {
    template<>
    struct hash<std::pair<int, int>> {
        size_t operator()(const pair<int, int>& p) const {
            return hash<long long>()(((long long)p.first << 32) | p.second);
        }
    };

    template<>
    struct equal_to<std::pair<int, int>> {
        bool operator()(const pair<int, int>& a, const pair<int, int>& b) const {
            return a.first == b.first && a.second == b.second;
        }
    };
}


class Chunk {
public:
    static const int SIZE = 16, HEIGHT = 256;
    int lodLevel = 0;

    struct FaceInstance {//区块面实例
        float posX, posY, posZ;  // 世界坐标 (12字节)
        uint32_t packedData;     // 数据包 (4字节)

        FaceInstance(const glm::vec3& pos, uint8_t face, uint8_t type, uint8_t uv, const glm::vec2& s)
            : posX(pos.x), posY(pos.y), posZ(pos.z)
        {
            // 量化尺寸为4位 (0-15对应1-16)
            uint32_t sizeX_quant = static_cast<uint32_t>(s.x - 1.0f) & 0xF;
            uint32_t sizeY_quant = static_cast<uint32_t>(s.y - 1.0f) & 0xF;

            // 打包所有数据到32位字段
            packedData = face
                | (static_cast<uint32_t>(type) << 8)
                | (static_cast<uint32_t>(uv) << 16)
                | (sizeX_quant << 24)
                | (sizeY_quant << 28);
        }
    };
    std::vector<FaceInstance> faceInstances;

    int chunkX, chunkZ; 
    glm::ivec2 GetChunkCoords() const { return { chunkX, chunkZ }; } 
    std::vector<FaceInstance> opaqueFaceInstances;
    std::vector<FaceInstance> transparentFaceInstances;
    std::vector<FaceInstance> alphaFaceInstances;
    std::vector<FaceInstance> plantFaceInstances;

    using TerrainType = ConfigTerrainType;

    enum BiomeType {
        DESERT,
        PLAINS,
        MOUNTAINS,
        SNOWY,
        OCEAN,
        RIVER,
        BEACH,
        LAKE
    };
    static const int SEA_LEVEL = 100;  // 海平面高度

    static inline int index(int x, int y, int z) {
        return x + z * SIZE + y * SIZE * SIZE;
    }

    // 新增：获取区块中心坐标
    glm::vec3 GetCenter() const {
        return glm::vec3(
            chunkX + SIZE / 2.0f,
            HEIGHT / 2.0f,
            chunkZ + SIZE / 2.0f
        );
    }

    Chunk();
    ~Chunk() = default;
    float lastMeshDistance = -1.0f; // 上次生成网格时的距离
    void Generate(int x, int z);
    void Clear();
    bool needsMeshUpdate = true; // 原有标记
    bool dataDirty = true;       // 新增数据变化标记
    uint8_t blocks[SIZE * HEIGHT * SIZE] = { 0 };
    uint8_t lightData[SIZE * HEIGHT * SIZE] = { 0 }; // 新增光照数据数组
    std::array<bool, 4> dirtyEdges = { false, false, false, false }; // 顺序: 东、西、南、北

    DrawElementsIndirectCommand GetDrawCommand() const {
        return {
            4,  // 每个面4个顶点
            visibleOpaqueCount, // 使用全局可见计数
            0,   // 起始索引
            0,   // 基础顶点
            0    // 基础实例
        };
    }

    // 修改可见计数
    uint32_t visibleOpaqueCount = 0;
    uint32_t visibleTransparentCount = 0;
    AABB GetAABB() const {
        glm::vec3 min(chunkX, 0.0f, chunkZ);
        glm::vec3 max(chunkX + SIZE, HEIGHT, chunkZ + SIZE);
        return { min, max };
    }
private:

    BiomeType GetBiomeAt(int worldX, int worldZ, int terrainHeight);
    float sigmoid(float x);
    uint8_t GetSurfaceBlock(BiomeType biome, int x, int y, int z);
    FastNoiseLite treeNoise;
    void GenerateTree(int lx, int lz, int baseHeight);
    void InitializeNoiseGenerators();
    void GenerateFlatTerrain();
    void GeneratePerlinTerrain();
    void GenerateAdvancedTerrain();
    FastNoiseLite noiseGen;
    FastNoiseLite continentNoise;
    FastNoiseLite terrainNoise;
    FastNoiseLite ridgeNoise;
    FastNoiseLite detailNoise;
    FastNoiseLite waterNoise;
    FastNoiseLite lakeNoise;
    float GetContinentNoise(int worldX, int worldZ);
    float GetTerrainNoise(int worldX, int worldY, int worldZ);
    float GetRidgeNoise(int worldX, int worldY, int worldZ);
    float GetDetailNoise(int worldX, int worldY, int worldZ);
};

#endif // CHUNK_H