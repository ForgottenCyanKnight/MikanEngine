#pragma once

#include "ChunkTerrain.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ChunkCaves {
namespace Detail {

constexpr int kCellSize = 64;
constexpr int kWaterBuffer = 2;
constexpr int kFloor = 4;

inline int CellAt(int coordinate) {
    return coordinate / kCellSize - (coordinate % kCellSize < 0);
}

inline uint32_t CaveHash(int x, int z) {
    uint32_t hash = static_cast<uint32_t>(x) * 0x85ebca77u ^
                    static_cast<uint32_t>(z) * 0x9e3779b1u ^ 0xaf3e52b9u;
    hash = (hash ^ (hash >> 16)) * 0x7feb352du;
    hash = (hash ^ (hash >> 15)) * 0x846ca68bu;
    return hash ^ (hash >> 16);
}

inline glm::vec3 Anchor(int cellX, int cellZ, uint32_t seed) {
    const int x = cellX * kCellSize + 25 + static_cast<int>(seed % 15);
    const int z = cellZ * kCellSize + 25 + static_cast<int>((seed >> 7) % 15);
    const int cover = 24 + static_cast<int>((seed >> 16) % 16);
    const int y = std::clamp(ChunkTerrain::Sample(x, z).surfaceY - cover, 20, 123);
    return {x + 0.5f, y + 0.5f, z + 0.5f};
}

class Carver {
public:
    using Columns = ChunkTerrain::Column[Chunk::SIZE + 2][Chunk::SIZE + 2];

    Carver(Chunk& chunk, const Columns& columns) : chunk(chunk), columns(columns) {}

    void Capsule(glm::vec3 from, glm::vec3 to, float radius, int roof) {
        const int minX = std::max(0, static_cast<int>(std::floor(std::min(from.x, to.x) - radius)) - chunk.chunkX);
        const int maxX = std::min(Chunk::SIZE - 1,
            static_cast<int>(std::floor(std::max(from.x, to.x) + radius)) - chunk.chunkX);
        const int minZ = std::max(0, static_cast<int>(std::floor(std::min(from.z, to.z) - radius)) - chunk.chunkZ);
        const int maxZ = std::min(Chunk::SIZE - 1,
            static_cast<int>(std::floor(std::max(from.z, to.z) + radius)) - chunk.chunkZ);
        if (minX > maxX || minZ > maxZ) return;
        PrepareDryColumns();

        const glm::vec3 axis = to - from;
        const glm::vec2 horizontal(axis.x, axis.z);
        const float horizontalLength = glm::length(horizontal);
        const float horizontalInvSquared = horizontalLength == 0.0f ? 0.0f
            : 1.0f / (horizontalLength * horizontalLength);
        const float axisInvSquared = 1.0f / std::max(glm::dot(axis, axis), 0.001f);
        const float verticalReach = radius + std::abs(axis.y) *
            (horizontalLength == 0.0f ? 1.0f : std::min(1.0f, radius / horizontalLength));
        const float radiusSquared = radius * radius;

        for (int x = minX; x <= maxX; ++x) {
            for (int z = minZ; z <= maxZ; ++z) {
                if (!dry[x][z]) continue;
                const glm::vec2 offset(chunk.chunkX + x + 0.5f - from.x, chunk.chunkZ + z + 0.5f - from.z);
                const float projected = std::clamp(glm::dot(offset, horizontal) * horizontalInvSquared, 0.0f, 1.0f);
                const glm::vec2 horizontalDelta = offset - projected * horizontal;
                if (glm::dot(horizontalDelta, horizontalDelta) >= radiusSquared) continue;

                const float centreY = from.y + projected * axis.y;
                const int lower = std::max(kFloor, static_cast<int>(std::floor(centreY - verticalReach)));
                const int upper = std::min(columns[x + 1][z + 1].surfaceY - roof,
                    static_cast<int>(std::floor(centreY + verticalReach)));
                for (int y = lower; y <= upper; ++y) {
                    const glm::vec3 point(offset.x, y + 0.5f - from.y, offset.y);
                    const float along = std::clamp(glm::dot(point, axis) * axisInvSquared, 0.0f, 1.0f);
                    const glm::vec3 delta = point - along * axis;
                    if (glm::dot(delta, delta) < radiusSquared) {
                        chunk.blocks[Chunk::index(x, y, z)] = BlockManager::AIR;
                    }
                }
            }
        }
    }

private:
    void PrepareDryColumns() {
        if (dryPrepared) return;
        dryPrepared = true;
        bool wet[Chunk::SIZE + 2 * kWaterBuffer][Chunk::SIZE + 2 * kWaterBuffer];
        for (int x = -kWaterBuffer; x < Chunk::SIZE + kWaterBuffer; ++x) {
            for (int z = -kWaterBuffer; z < Chunk::SIZE + kWaterBuffer; ++z) {
                const bool inHalo = x >= -1 && x <= Chunk::SIZE && z >= -1 && z <= Chunk::SIZE;
                wet[x + kWaterBuffer][z + kWaterBuffer] = (inHalo ? columns[x + 1][z + 1]
                    : ChunkTerrain::Sample(chunk.chunkX + x, chunk.chunkZ + z)).waterY >= 0;
            }
        }
        for (int x = 0; x < Chunk::SIZE; ++x) {
            for (int z = 0; z < Chunk::SIZE; ++z) {
                bool clearOfWater = true;
                for (int dx = 0; dx <= 2 * kWaterBuffer && clearOfWater; ++dx) {
                    for (int dz = 0; dz <= 2 * kWaterBuffer && clearOfWater; ++dz) {
                        clearOfWater = !wet[x + dx][z + dz];
                    }
                }
                dry[x][z] = clearOfWater;
            }
        }
    }

    Chunk& chunk;
    const Columns& columns;
    bool dry[Chunk::SIZE][Chunk::SIZE];
    bool dryPrepared = false;
};

inline void Passage(Carver& carver, glm::vec3 from, glm::vec3 to, uint32_t seed) {
    const glm::vec2 direction = glm::normalize(glm::vec2(to.x - from.x, to.z - from.z));
    const float bend = static_cast<float>(static_cast<int>((seed >> 8) % 17) - 8);
    const float rise = static_cast<float>(static_cast<int>((seed >> 20) % 11) - 5);
    const glm::vec3 midpoint = 0.5f * (from + to) + glm::vec3(-direction.y * bend, rise, direction.x * bend);
    const float radius = 2.7f + static_cast<float>((seed >> 26) & 7u) * 0.2f;
    carver.Capsule(from, midpoint, radius, 6);
    carver.Capsule(midpoint, to, radius + 0.25f, 6);
}

inline void Entrance(Carver& carver, const glm::vec3& anchor, uint32_t seed) {
    constexpr glm::ivec2 directions[] = {{1, 0}, {1, 1}, {0, 1}, {-1, 1},
                                         {-1, 0}, {-1, -1}, {0, -1}, {1, -1}};
    for (int i = 0; i < 8; ++i) {
        const auto direction = directions[((seed >> 9) + i) & 7u];
        for (int reach : {21, 16}) {
            const int x = static_cast<int>(std::floor(anchor.x)) + direction.x * reach;
            const int z = static_cast<int>(std::floor(anchor.z)) + direction.y * reach;
            const auto rock = ChunkTerrain::Sample(x, z);
            if (rock.waterY >= 0) continue;
            const glm::ivec2 front = direction.x != 0 ? glm::ivec2(direction.x, 0) : direction;
            const auto outside = ChunkTerrain::Sample(x + front.x, z + front.y);
            // Open the mouth through a dry slope while leaving a roof above it.
            if (outside.waterY >= 0 || outside.surfaceY < Chunk::SEA_LEVEL + 10
                || rock.surfaceY - outside.surfaceY < 4) continue;
            const glm::vec3 opening(x + 0.5f, outside.surfaceY + 2.5f, z + 0.5f);
            if (std::abs(opening.y - anchor.y) > 40.0f) continue;
            carver.Capsule(anchor, opening, 3.25f, 2);
            return;
        }
    }
}

} // namespace Detail

inline void Carve(Chunk& chunk, const Detail::Carver::Columns& columns) {
    using namespace Detail;
    Carver carver(chunk, columns);
    const int firstX = CellAt(chunk.chunkX);
    const int lastX = CellAt(chunk.chunkX + Chunk::SIZE - 1);
    const int firstZ = CellAt(chunk.chunkZ);
    const int lastZ = CellAt(chunk.chunkZ + Chunk::SIZE - 1);

    // Outgoing passages meet the same jittered anchors in every chunk. The
    // one-cell apron reaches paths that enter from the west or north.
    for (int cellX = firstX - 1; cellX <= lastX; ++cellX) {
        for (int cellZ = firstZ - 1; cellZ <= lastZ; ++cellZ) {
            const bool inX = cellX >= firstX;
            const bool inZ = cellZ >= firstZ;
            if (!inX && !inZ) continue;

            const uint32_t seed = CaveHash(cellX, cellZ);
            const glm::vec3 anchor = Anchor(cellX, cellZ, seed);
            if (inZ && (seed & 3u) != 0u) {
                Passage(carver, anchor, Anchor(cellX + 1, cellZ, CaveHash(cellX + 1, cellZ)), seed);
            }
            if (inX && ((seed >> 3) & 3u) != 0u) {
                Passage(carver, anchor, Anchor(cellX, cellZ + 1, CaveHash(cellX, cellZ + 1)), seed ^ 0x73f29321u);
            }
            if (inX && inZ) {
                if (((seed >> 5) & 3u) == 0u) {
                    carver.Capsule(anchor, anchor, 5.5f + static_cast<float>((seed >> 22) & 3u), 6);
                }
                if (((seed >> 14) & 3u) != 0u) Entrance(carver, anchor, seed);
            }
        }
    }
}

} // namespace ChunkCaves
