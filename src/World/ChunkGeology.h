#pragma once

#include "ChunkTerrain.h"

#include <array>

namespace ChunkGeology {

constexpr int kFoundationHeight = 16;

struct RockBand {
    int endY;
    uint8_t material;
};

struct Column {
    std::array<RockBand, 4> bands;
    float sediment;

    uint8_t RockAt(int y) const {
        for (const auto& band : bands) {
            if (y < band.endY) return band.material;
        }
        return bands.back().material;
    }
};

inline Column Sample(int worldX, int worldZ) {
    static const auto provinceNoise = ChunkTerrain::Detail::Fractal(2863, 0.0038f, 2);
    static const auto beddingNoise = ChunkTerrain::Detail::Fractal(6679, 0.009f, 2);
    static const auto sedimentNoise = ChunkTerrain::Detail::Fractal(2181, 0.031f, 2);
    const float x = static_cast<float>(worldX);
    const float z = static_cast<float>(worldZ);
    const float bedding = beddingNoise.GetNoise(x, z);
    const float province = provinceNoise.GetNoise(x + 42.0f * bedding, z - 29.0f * bedding);
    const int basementTop = 41 + static_cast<int>(17.0f * bedding);
    const int contact = 104 + static_cast<int>(29.0f * province + 17.0f * bedding);
    const int seamTop = contact + 6 + static_cast<int>(3.0f * bedding);

    // Rock provinces span many chunks. Tilted contacts expose the layers on
    // hillsides while keeping soil-covered areas continuous.
    const uint8_t lower = province > 0.12f ? BlockManager::BASALT
        : province < -0.18f ? BlockManager::SANDSTONE : BlockManager::LIMESTONE;
    const uint8_t seam = province > 0.03f ? BlockManager::MARBLE : BlockManager::LIMESTONE;
    const uint8_t upper = province > 0.30f ? BlockManager::BASALT
        : province < -0.14f ? BlockManager::SANDSTONE : BlockManager::STONE;
    return {{{{basementTop, BlockManager::STONE}, {contact, lower},
              {seamTop, seam}, {Chunk::HEIGHT, upper}}}, sedimentNoise.GetNoise(x, z)};
}

inline uint8_t ShoreMaterial(const ChunkTerrain::Column& terrain, float sediment) {
    if (terrain.biome == Chunk::RIVER) {
        return sediment > -0.08f ? BlockManager::GRAVEL : BlockManager::SAND;
    }
    const int depth = terrain.waterY - terrain.surfaceY;
    if (terrain.biome == Chunk::LAKE && depth >= 3 && sediment < 0.12f) {
        return BlockManager::CLAY;
    }
    return sediment > 0.29f ? BlockManager::GRAVEL : BlockManager::SAND;
}

} // namespace ChunkGeology
