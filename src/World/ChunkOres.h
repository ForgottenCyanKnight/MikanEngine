#pragma once

#include "World/Chunk.h"

#include <algorithm>
#include <cstdint>

namespace ChunkOres {
namespace Detail {

constexpr int kCellSize = 16;
constexpr int kVeinReach = 5;
constexpr int kMinOreY = 6;
constexpr int kMaxOreY = 120;
constexpr int kSurfaceCover = 4;

struct OreBand {
    uint8_t material;
    int minY;
    int maxY;
    int weight;
};

constexpr OreBand kOreBands[] = {
    {BlockManager::COAL_ORE, 39, 118, 34},
    {BlockManager::COPPER_ORE, 35, 106, 22},
    {BlockManager::IRON_ORE, 17, 102, 32},
    {BlockManager::SULPHUR_ORE, 14, 59, 8},
    {BlockManager::DIAMOND_ORE, 8, 31, 4},
    {BlockManager::GERMANIUM_ORE, 12, 43, 5},
    {BlockManager::SALTPETER_ORE, 49, 99, 7},
};

inline int CellAt(int position)
{
    return position / kCellSize - (position % kCellSize < 0 ? 1 : 0);
}

inline uint32_t Hash(int x, int y, int z, uint32_t salt)
{
    uint32_t hash = (static_cast<uint32_t>(x) * 0x9e3779b1u) ^
                    (static_cast<uint32_t>(y) * 0xc2b2ae35u) ^
                    (static_cast<uint32_t>(z) * 0x85ebca77u) ^ salt;
    hash ^= hash >> 16;
    hash *= 0x7feb352du;
    hash ^= hash >> 15;
    hash *= 0x846ca68bu;
    return hash ^ (hash >> 16);
}

inline uint8_t ChooseOre(int y, uint32_t seed)
{
    // A cell can stay unmineralized even when only a rare deep ore is eligible.
    int totalWeight = 24;
    for (const auto& band : kOreBands) {
        if (band.minY <= y && y <= band.maxY) totalWeight += band.weight;
    }

    int choice = static_cast<int>(seed % totalWeight);
    for (const auto& band : kOreBands) {
        if (y < band.minY || y > band.maxY) continue;
        if (choice < band.weight) return band.material;
        choice -= band.weight;
    }
    return BlockManager::AIR;
}

inline bool IsHostRock(uint8_t block)
{
    return block == BlockManager::STONE || block == BlockManager::LIMESTONE ||
           block == BlockManager::BASALT || block == BlockManager::SANDSTONE || block == BlockManager::MARBLE;
}

inline void StampVein(Chunk& chunk, glm::ivec3 center, int maxY, uint32_t seed, uint8_t ore)
{
    constexpr float diagonal = 0.70710678f;
    constexpr glm::vec2 headings[] = {{1.0f, 0.0f}, {diagonal, diagonal}, {0.0f, 1.0f}, {-diagonal, diagonal}};
    const glm::vec2 heading = headings[(seed >> 16) & 3u];
    const float inverseLength = 1.0f / (2.75f + 0.55f * static_cast<float>((seed >> 18) & 3u));
    const float inverseWidth = 1.0f / (1.20f + 0.35f * static_cast<float>((seed >> 20) & 3u));
    const float inverseHeight = 1.0f / (1.00f + 0.35f * static_cast<float>((seed >> 22) & 3u));

    center.x -= chunk.chunkX;
    center.z -= chunk.chunkZ;
    const int minX = std::max(0, center.x - kVeinReach);
    const int maxX = std::min(Chunk::SIZE - 1, center.x + kVeinReach);
    const int minZ = std::max(0, center.z - kVeinReach);
    const int maxZ = std::min(Chunk::SIZE - 1, center.z + kVeinReach);
    const int minY = std::max(kMinOreY, center.y - 3);
    const int lastY = std::min(maxY, center.y + 3);
    for (int y = minY; y <= lastY; ++y) {
        const float vertical = static_cast<float>(y - center.y) * inverseHeight;
        for (int z = minZ; z <= maxZ; ++z) {
            for (int x = minX; x <= maxX; ++x) {
                const float dx = static_cast<float>(x - center.x);
                const float dz = static_cast<float>(z - center.z);
                const float along = (heading.x * dx + heading.y * dz) * inverseLength;
                const float across = (heading.x * dz - heading.y * dx) * inverseWidth;
                const float distanceSquared = along * along + across * across + vertical * vertical;
                if (distanceSquared > 1.12f) continue;
                if (distanceSquared > 0.78f) {
                    const uint32_t noise = Hash(chunk.chunkX + x, y, chunk.chunkZ + z, seed) & 255u;
                    if (distanceSquared > 0.78f + 0.34f * (static_cast<float>(noise) / 255.0f)) continue;
                }

                auto& block = chunk.blocks[Chunk::index(x, y, z)];
                const auto cover = chunk.blocks[Chunk::index(x, y + kSurfaceCover, z)];
                if (IsHostRock(block) && cover != BlockManager::AIR && cover != BlockManager::WATER) block = ore;
            }
        }
    }
}

} // namespace Detail

inline void Stamp(Chunk& chunk, int maxSurfaceY)
{
    using namespace Detail;
    const int maxY = std::min(kMaxOreY, maxSurfaceY - kSurfaceCover);
    if (maxY < kMinOreY) return;

    // Revisit world-space vein seeds within reach of this chunk. The same
    // cell ordering resolves overlaps on both sides of a chunk boundary.
    const int firstCellX = CellAt(chunk.chunkX - kVeinReach);
    const int lastCellX = CellAt(chunk.chunkX + Chunk::SIZE - 1 + kVeinReach);
    const int firstCellZ = CellAt(chunk.chunkZ - kVeinReach);
    const int lastCellZ = CellAt(chunk.chunkZ + Chunk::SIZE - 1 + kVeinReach);
    for (int cx = firstCellX; cx <= lastCellX; ++cx) {
        for (int cy = CellAt(kMinOreY - 3); cy <= CellAt(maxY + 3); ++cy) {
            for (int cz = firstCellZ; cz <= lastCellZ; ++cz) {
                const uint32_t seed = Hash(cx, cy, cz, 0x668a3e21u);
                if ((seed & 3u) >= 2u) continue;

                const glm::ivec3 center(cx * kCellSize + static_cast<int>((seed >> 2) & 15u),
                                        cy * kCellSize + static_cast<int>((seed >> 6) & 15u),
                                        cz * kCellSize + static_cast<int>((seed >> 10) & 15u));
                const uint8_t ore = ChooseOre(center.y, Hash(cx, cy, cz, 0x19c43f47u));
                if (ore != BlockManager::AIR) StampVein(chunk, center, maxY, seed, ore);
            }
        }
    }
}

} // namespace ChunkOres
