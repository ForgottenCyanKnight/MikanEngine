// Chunk.cpp
#include "World/Chunk.h"
#include "ChunkTerrain.h"
#include "ChunkCaves.h"
#include "ChunkGeology.h"
#include "ChunkOres.h"
#include "ChunkTrees.inl"
#include "World/BlockManager.h"
#include <algorithm>
#include <cmath>

Chunk::Chunk() {
    Clear();
}

void Chunk::Generate(int x, int z) {
    chunkX = x;
    chunkZ = z;
    Clear();

    TerrainType type = GetWorldConfig().terrainType;

    switch (type) {
    case FLAT: GenerateFlatTerrain(); break;
    case PERLIN: GeneratePerlinTerrain(); break;
    case ADVANCED: GenerateAdvancedTerrain(); break;
    }
}

namespace {

uint32_t PlantHash(int x, int z) {
    uint32_t bits = static_cast<uint32_t>(x) * 0x9e3779b9u ^ static_cast<uint32_t>(z) * 0x85ebca6bu;
    bits += 0x68bc21ebu;
    bits = (bits ^ (bits >> 16)) * 0x7feb352du;
    bits = (bits ^ (bits >> 15)) * 0x846ca68bu;
    return bits ^ (bits >> 16);
}

} // namespace

void Chunk::GenerateAdvancedTerrain() {
    // One column of padding keeps slope materials continuous at chunk edges.
    ChunkTerrain::Column columns[SIZE + 2][SIZE + 2];
    int commonRockHeight = ChunkGeology::kFoundationHeight;
    int maxSurfaceY = 0;
    for (int x = -1; x <= SIZE; ++x) {
        for (int z = -1; z <= SIZE; ++z) {
            columns[x + 1][z + 1] = ChunkTerrain::Sample(chunkX + x, chunkZ + z);
            if (x >= 0 && x < SIZE && z >= 0 && z < SIZE) {
                commonRockHeight = std::min(commonRockHeight, columns[x + 1][z + 1].surfaceY - 4);
                maxSurfaceY = std::max(maxSurfaceY, columns[x + 1][z + 1].surfaceY);
            }
        }
    }
    commonRockHeight = std::max(commonRockHeight, 0);
    std::fill_n(blocks, commonRockHeight * SIZE * SIZE, BlockManager::STONE);

    for (int x = 0; x < SIZE; ++x) {
        for (int z = 0; z < SIZE; ++z) {
            const auto& column = columns[x + 1][z + 1];
            const int dx = columns[x + 2][z + 1].surfaceY - columns[x][z + 1].surfaceY;
            const int dz = columns[x + 1][z + 2].surfaceY - columns[x + 1][z].surfaceY;
            const bool steep = dx * dx + dz * dz > 16;
            const bool submerged = column.waterY > column.surfaceY;
            const bool sandy = submerged || column.biome == BEACH || column.biome == DESERT;
            const bool rocky = steep && (!sandy || column.waterY - column.surfaceY > 7);
            const int soilStart = rocky ? column.surfaceY : std::max(0, column.surfaceY - (sandy ? 3 : 4));
            const auto geology = ChunkGeology::Sample(chunkX + x, chunkZ + z);
            const uint8_t soil = sandy ? ChunkGeology::ShoreMaterial(column, geology.sediment) : BlockManager::DIRT;
            const uint8_t snow = column.surfaceY > 215 && geology.sediment < -0.21f
                ? BlockManager::ICE : BlockManager::SNOW;
            const uint8_t surface = rocky ? geology.RockAt(column.surfaceY) : sandy ? soil
                : column.biome == SNOWY ? snow : BlockManager::GRASSBLOCK;

            int rockY = commonRockHeight;
            for (const auto& band : geology.bands) {
                const int endY = std::min(soilStart, band.endY);
                for (; rockY < endY; ++rockY) blocks[index(x, rockY, z)] = band.material;
                if (rockY == soilStart) break;
            }
            for (int y = soilStart; y < column.surfaceY; ++y) {
                blocks[index(x, y, z)] = soil;
            }
            blocks[index(x, column.surfaceY, z)] = surface;
            for (int y = column.surfaceY + 1; y <= column.waterY; ++y) {
                blocks[index(x, y, z)] = BlockManager::WATER;
            }
        }
    }

    ChunkOres::Stamp(*this, maxSurfaceY);
    ChunkCaves::Carve(*this, columns);
    GenerateAdvancedTrees(*this, columns);
    for (int x = 0; x < SIZE; ++x) {
        for (int z = 0; z < SIZE; ++z) {
            const int ground = columns[x + 1][z + 1].surfaceY;
            if (blocks[index(x, ground, z)] != BlockManager::GRASSBLOCK) continue;
            auto& plant = blocks[index(x, ground + 1, z)];
            if (plant != BlockManager::AIR) continue;
            const uint32_t chance = PlantHash(chunkX + x, chunkZ + z) % 100;
            if (chance < 2) plant = BlockManager::ROSE;
            else if (chance < 20) plant = BlockManager::GRASS;
        }
    }
}

void Chunk::GenerateFlatTerrain() {
    constexpr int baseHeight = FLAT_SURFACE_Y - 1;

    std::fill_n(blocks, SIZE * HEIGHT * SIZE, BlockManager::AIR);

    for (int y = 0; y < baseHeight - 5; y++) {
        for (int lx = 0; lx < SIZE; lx++) {
            for (int lz = 0; lz < SIZE; lz++) {
                blocks[index(lx, y, lz)] = BlockManager::STONE;
            }
        }
    }

    for (int y = baseHeight - 5; y < baseHeight; y++) {
        for (int lx = 0; lx < SIZE; lx++) {
            for (int lz = 0; lz < SIZE; lz++) {
                blocks[index(lx, y, lz)] = BlockManager::DIRT;
            }
        }
    }

    for (int lx = 0; lx < SIZE; lx++) {
        for (int lz = 0; lz < SIZE; lz++) {
            blocks[index(lx, baseHeight, lz)] = BlockManager::GRASSBLOCK;
        }
    }
}


void Chunk::GeneratePerlinTerrain() {
    const int baseHeight = HEIGHT / 2;

    for (int lx = 0; lx < SIZE; lx++) {
        for (int lz = 0; lz < SIZE; lz++) {
            int gx = chunkX + lx;
            int gz = chunkZ + lz;

            float noise = noiseGen.GetNoise((float)gx, (float)gz);

            float normalizedNoise = (noise + 1.0f) * 0.5f;
            int terrainHeight = static_cast<int>(normalizedNoise * 30) + baseHeight - 15;

            terrainHeight = std::max(0, std::min(terrainHeight, HEIGHT - 1));

            for (int y = 0; y < HEIGHT; y++) {
                int idx = index(lx, y, lz);

                if (y < terrainHeight - 5) {
                    blocks[idx] = BlockManager::STONE;
                }
                else if (y < terrainHeight) {
                    blocks[idx] = BlockManager::DIRT;
                }
                else if (y == terrainHeight) {
                    blocks[idx] = BlockManager::GRASSBLOCK;
                }
                else {
                    blocks[idx] = BlockManager::AIR;
                }
            }
        }
    }
}


void Chunk::Clear() {
    std::fill_n(blocks, SIZE * HEIGHT * SIZE, 0);
    faceInstances.clear();
    needsMeshUpdate = true;
}
