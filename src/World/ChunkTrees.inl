namespace {

constexpr int kTreeCellSize = 10;
constexpr int kTreeCrownReach = 5;
constexpr int kTreeMaxHeight = 15;

uint32_t TreeHash(int x, int z, uint32_t salt)
{
    uint32_t hash = (static_cast<uint32_t>(x) * 0x9e3779b1u) ^
                    (static_cast<uint32_t>(z) * 0x85ebca77u) ^ salt;
    hash ^= hash >> 16;
    hash *= 0x7feb352du;
    hash ^= hash >> 15;
    hash *= 0x846ca68bu;
    return hash ^ (hash >> 16);
}

int TreeCell(int coordinate)
{
    return coordinate / kTreeCellSize - (coordinate % kTreeCellSize < 0 ? 1 : 0);
}

float TreeDensity(int x, int z)
{
    static const auto forest = ChunkTerrain::Detail::Fractal(3821, 0.018f, 2);
    return std::clamp(0.44f + 0.95f * forest.GetNoise(float(x), float(z)), 0.04f, 0.94f);
}

uint8_t TreeWood(int x, int z, const ChunkTerrain::Column& column, uint32_t seed)
{
    static const auto groves = ChunkTerrain::Detail::Fractal(7919, 0.0075f, 2);
    const float patch = groves.GetNoise(float(x), float(z));
    const float elevation = glm::smoothstep(118.0f, 176.0f, float(column.surfaceY));
    // Spruce favors upper slopes. Broadleaf groves mix birch and oak at lower elevations.
    const float spruceChance = std::clamp(0.10f + 0.70f * elevation + 0.52f * patch, 0.03f, 0.92f);
    const float birchShare = std::clamp(0.43f - 0.66f * patch - 0.15f * elevation, 0.10f, 0.84f);
    if (float((seed >> 4) & 255u) / 256.0f < spruceChance) return BlockManager::SPRUCE_WOOD;
    if (float((seed >> 16) & 255u) / 256.0f < birchShare) return BlockManager::BIRCH_WOOD;
    return BlockManager::WOOD;
}

void PlaceTreeBlock(Chunk& chunk, int x, int y, int z, uint8_t type)
{
    const int lx = x - chunk.chunkX;
    const int lz = z - chunk.chunkZ;
    if (lx < 0 || lx >= Chunk::SIZE || lz < 0 || lz >= Chunk::SIZE || y < 0 || y >= Chunk::HEIGHT) return;
    auto& block = chunk.blocks[Chunk::index(lx, y, lz)];
    if (block == BlockManager::AIR || (type != BlockManager::LEAVES && block == BlockManager::LEAVES)) {
        block = type;
    }
}

void AddCrownLayer(Chunk& chunk, int centerX, int y, int centerZ, int radius, uint32_t seed)
{
    const int minX = std::max(centerX - radius, chunk.chunkX);
    const int maxX = std::min(centerX + radius, chunk.chunkX + Chunk::SIZE - 1);
    const int minZ = std::max(centerZ - radius, chunk.chunkZ);
    const int maxZ = std::min(centerZ + radius, chunk.chunkZ + Chunk::SIZE - 1);
    const int radiusSquared = radius * radius;
    for (int x = minX; x <= maxX; ++x) {
        for (int z = minZ; z <= maxZ; ++z) {
            const int dx = x - centerX;
            const int dz = z - centerZ;
            const int distanceSquared = dx * dx + dz * dz;
            if (distanceSquared > radiusSquared) continue;
            if (radius > 1 && distanceSquared > radiusSquared - radius &&
                TreeHash(x, z, seed ^ static_cast<uint32_t>(y)) % 8u == 0u) continue;
            PlaceTreeBlock(chunk, x, y, z, BlockManager::LEAVES);
        }
    }
}

void StampTree(Chunk& chunk, int x, int ground, int z, uint8_t wood, uint32_t seed)
{
    const bool conifer = wood == BlockManager::SPRUCE_WOOD;
    const bool birch = wood == BlockManager::BIRCH_WOOD;
    const int trunkHeight = conifer ? 8 + int(seed % 6u) : (birch ? 6 : 5) + int(seed % 5u);
    const int trunkTop = ground + trunkHeight;
    for (int y = ground + 1; y <= trunkTop; ++y) PlaceTreeBlock(chunk, x, y, z, wood);

    if (conifer) {
        const int tip = trunkTop + 2;
        for (int y = ground + 3; y <= tip; ++y) {
            const int fromTip = tip - y;
            int radius = std::min(3, (fromTip + 1) / 2);
            if (fromTip > 3 && fromTip % 3 == 0) --radius;
            AddCrownLayer(chunk, x, y, z, radius, seed);
        }
        return;
    }

    constexpr int kDirections[4][2] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}};
    const int branchCount = trunkHeight >= 7 ? 2 : 1;
    for (int branch = 0; branch < branchCount; ++branch) {
        const auto& direction = kDirections[((seed >> 5) + 2u * branch) & 3u];
        const int branchY = trunkTop - 2 - branch;
        const int branchX = x + 2 * direction[0];
        const int branchZ = z + 2 * direction[1];
        PlaceTreeBlock(chunk, x + direction[0], branchY, z + direction[1], wood);
        PlaceTreeBlock(chunk, branchX, branchY, branchZ, wood);
        PlaceTreeBlock(chunk, branchX, branchY + 1, branchZ, wood);
        AddCrownLayer(chunk, branchX, branchY, branchZ, 1, seed);
        AddCrownLayer(chunk, branchX, branchY + 1, branchZ, 2, seed);
        AddCrownLayer(chunk, branchX, branchY + 2, branchZ, 1, seed);
    }

    constexpr int kBroadRadii[] = {2, 3, 4, 3, 2, 1};
    const int leanX = int((seed >> 12) % 3u) - 1;
    const int leanZ = int((seed >> 16) % 3u) - 1;
    for (int layer = 0; layer < 6; ++layer) {
        const int radius = kBroadRadii[layer] - ((birch || (seed & 4u)) && kBroadRadii[layer] > 2 ? 1 : 0);
        AddCrownLayer(chunk, x + (layer >= 2 ? leanX : 0), trunkTop - 3 + layer,
                      z + (layer >= 2 ? leanZ : 0), radius, seed);
    }
}

void GenerateAdvancedTrees(Chunk& chunk, const ChunkTerrain::Column (&columns)[Chunk::SIZE + 2][Chunk::SIZE + 2])
{
    const int minX = chunk.chunkX - kTreeCrownReach;
    const int maxX = chunk.chunkX + Chunk::SIZE - 1 + kTreeCrownReach;
    const int minZ = chunk.chunkZ - kTreeCrownReach;
    const int maxZ = chunk.chunkZ + Chunk::SIZE - 1 + kTreeCrownReach;
    const auto sample = [&](int x, int z) {
        const int lx = x - chunk.chunkX + 1;
        const int lz = z - chunk.chunkZ + 1;
        if (lx >= 0 && lx < Chunk::SIZE + 2 && lz >= 0 && lz < Chunk::SIZE + 2) return columns[lx][lz];
        return ChunkTerrain::Sample(x, z);
    };

    // Each cell owns at most one anchor. Keeping it away from cell edges
    // leaves at least six blocks between trunks, more than two branch reaches.
    for (int cellX = TreeCell(minX); cellX <= TreeCell(maxX); ++cellX) {
        for (int cellZ = TreeCell(minZ); cellZ <= TreeCell(maxZ); ++cellZ) {
            const uint32_t seed = TreeHash(cellX, cellZ, 0x2c1b3c6du);
            const int x = cellX * kTreeCellSize + 2 + int(seed % 5u);
            const int z = cellZ * kTreeCellSize + 2 + int((seed >> 12) % 5u);
            if (x < minX || x > maxX || z < minZ || z > maxZ) continue;
            if (float(seed >> 24) / 256.0f >= TreeDensity(x, z)) continue;

            const auto column = sample(x, z);
            if (column.waterY >= 0 || column.surfaceY + kTreeMaxHeight >= Chunk::HEIGHT ||
                (column.biome != Chunk::PLAINS && column.biome != Chunk::MOUNTAINS)) continue;
            const auto level = [&](int neighborX, int neighborZ) {
                const auto adjacent = sample(neighborX, neighborZ);
                return adjacent.waterY < 0 && std::abs(adjacent.surfaceY - column.surfaceY) <= 2;
            };
            if (!level(x - 1, z) || !level(x + 1, z) || !level(x, z - 1) || !level(x, z + 1)) continue;

            StampTree(chunk, x, column.surfaceY, z, TreeWood(x, z, column, seed), seed);
        }
    }
}

} // namespace
