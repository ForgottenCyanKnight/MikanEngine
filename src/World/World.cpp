#include "World/World.h"
#include <algorithm>
#include <iostream>

// 体素世界全局配置（WorldSystem 在创建世界时设置）
WorldConfig& GetWorldConfig() {
    static WorldConfig config;
    return config;
}

World::World(int radius) : renderRadius(radius) {
    CalculateMaxUpdatesPerFrame();
    unsigned int numThreads = std::max(1u, std::thread::hardware_concurrency() - 1);
    StartWorkerThreads(numThreads);
}

void World::CalculateMaxUpdatesPerFrame() {
    unsigned int coreCount = std::thread::hardware_concurrency();
    if (coreCount <= 4) {
        maxUpdatesPerFrame = 2; 
    }
    else if (coreCount <= 8) {
        maxUpdatesPerFrame = 4;  
    }
    else {
        maxUpdatesPerFrame = coreCount / 2;  
    }

    maxUpdatesPerFrame = std::max(1, maxUpdatesPerFrame);
}

World::~World() {
    StopWorkerThreads();
}

World::HitResult World::RayCast(const glm::vec3& start, const glm::vec3& direction, float maxDistance) {
    HitResult result;
    glm::vec3 rayDir = glm::normalize(direction);
    glm::vec3 rayStep(1.0f / fabs(rayDir.x), 1.0f / fabs(rayDir.y), 1.0f / fabs(rayDir.z));
    glm::ivec3 mapPos(floor(start.x), floor(start.y), floor(start.z));
    glm::vec3 deltaDist;
    glm::ivec3 step;

    int currentCx = std::numeric_limits<int>::max();
    int currentCz = std::numeric_limits<int>::max();
    Chunk* currentChunk = nullptr;

    for (int i = 0; i < 3; i++) {
        if (rayDir[i] < 0) {
            step[i] = -1;
            deltaDist[i] = (start[i] - mapPos[i]) * rayStep[i];
        }
        else {
            step[i] = 1;
            deltaDist[i] = (mapPos[i] + 1.0f - start[i]) * rayStep[i];
        }
    }

    float maxDist = 0.0f;
    glm::ivec3 lastPos = mapPos;

    while (maxDist < maxDistance) {
        uint8_t blockType = 0;
        constexpr int SIZE = Chunk::SIZE;
        constexpr int SIZE_MASK = SIZE - 1;

        if (mapPos.y >= 0 && mapPos.y < Chunk::HEIGHT) {
            int cx = (mapPos.x >= 0) ? (mapPos.x >> 4) : ((mapPos.x + 1) / SIZE - 1);
            int cz = (mapPos.z >= 0) ? (mapPos.z >> 4) : ((mapPos.z + 1) / SIZE - 1);

            if (cx != currentCx || cz != currentCz) {
                currentCx = cx;
                currentCz = cz;
                auto it = chunks.find({ cx, cz });
                currentChunk = (it != chunks.end()) ? &it->second : nullptr;
            }

            if (currentChunk) {
                int lx = mapPos.x & SIZE_MASK;
                int lz = mapPos.z & SIZE_MASK;

                if (lx < 0) lx += SIZE;
                if (lz < 0) lz += SIZE;

                blockType = currentChunk->blocks[Chunk::index(lx, mapPos.y, lz)];
            }
        }

        if (blockType != 0) {
            result.hit = true;
            result.position = mapPos;
            result.blockType = blockType;

            if (lastPos.x != mapPos.x) result.normal = glm::ivec3(lastPos.x - mapPos.x, 0, 0);
            else if (lastPos.y != mapPos.y) result.normal = glm::ivec3(0, lastPos.y - mapPos.y, 0);
            else if (lastPos.z != mapPos.z) result.normal = glm::ivec3(0, 0, lastPos.z - mapPos.z);

            return result;
        }

        lastPos = mapPos;

        if (deltaDist.x < deltaDist.y && deltaDist.x < deltaDist.z) {
            maxDist = deltaDist.x;
            deltaDist.x += rayStep.x;
            mapPos.x += step.x;
        }
        else if (deltaDist.y < deltaDist.z) {
            maxDist = deltaDist.y;
            deltaDist.y += rayStep.y;
            mapPos.y += step.y;
        }
        else {
            maxDist = deltaDist.z;
            deltaDist.z += rayStep.z;
            mapPos.z += step.z;
        }
    }

    return result;
}

std::vector<Chunk*> World::GetActiveChunks() {
    std::vector<Chunk*> result;
    for (auto& pair : chunks) {
        result.push_back(&pair.second);
    }
    return result;
}

size_t World::CollectVisibleFaces(const std::array<Plane, 6>& frustumPlanes,
                                  std::vector<Chunk::FaceInstance>& outOpaque,
                                  std::vector<Chunk::FaceInstance>& outAlpha,
                                  std::vector<Chunk::FaceInstance>& outTransparent)
{
    // 与网格 worker 线程的写回互斥（chunks[task] = chunkCopy）
    std::shared_lock<std::shared_mutex> lock(chunksMutex);

    bool hasFrustum = false;
    for (const auto& p : frustumPlanes) {
        if (p.normal != glm::vec3(0.0f)) { hasFrustum = true; break; }
    }

    outOpaque.clear();
    outAlpha.clear();
    outTransparent.clear();
    size_t total = 0;
    for (auto& [coord, chunk] : chunks) {
        (void)coord;
        if (hasFrustum && !chunk.GetAABB().IsInsideFrustum(frustumPlanes)) continue;

        const auto& opaque = chunk.opaqueFaceInstances;
        const auto& alpha = chunk.alphaFaceInstances;
        const auto& transparent = chunk.transparentFaceInstances;

        outOpaque.insert(outOpaque.end(), opaque.begin(), opaque.end());
        outAlpha.insert(outAlpha.end(), alpha.begin(), alpha.end());
        outTransparent.insert(outTransparent.end(), transparent.begin(), transparent.end());
        total += opaque.size() + alpha.size() + transparent.size();
    }
    return total;
}

uint8_t World::GetBlockAt(int x, int y, int z) const
{
    std::shared_lock<std::shared_mutex> lock(chunksMutex);
    constexpr int SIZE = Chunk::SIZE;
    int cx = (int)std::floor(x / (float)SIZE);
    int cz = (int)std::floor(z / (float)SIZE);
    auto it = chunks.find({ cx, cz });
    if (it == chunks.end()) return 0;
    int lx = x - cx * SIZE;
    int lz = z - cz * SIZE;
    if (lx < 0) lx += SIZE;
    if (lz < 0) lz += SIZE;
    if (lx >= SIZE || lz >= SIZE) return 0;
    if (y < 0 || y >= Chunk::HEIGHT) return 0;
    return it->second.blocks[Chunk::index(lx, y, lz)];
}

void World::PlaceBlock(const glm::ivec3& position, int blockType) {
    std::lock_guard<std::mutex> lock(modificationMutex);
    modificationQueue.emplace(position.x, position.y, position.z, blockType);
}

void World::ProcessModifications() {
    std::lock_guard<std::mutex> lock(modificationMutex);
    while (!modificationQueue.empty()) {
        auto [x, y, z, type] = modificationQueue.front();
        modificationQueue.pop();
        constexpr int SIZE = Chunk::SIZE;
        constexpr int SIZE_MASK = SIZE - 1;
        int cx = (x >= 0) ? (x >> 4) : ((x + 1) / SIZE - 1);
        int cz = (z >= 0) ? (z >> 4) : ((z + 1) / SIZE - 1);

        auto it = chunks.find({ cx, cz });
        if (it != chunks.end()) {
            int lx = x & SIZE_MASK;
            int lz = z & SIZE_MASK;
            if (lx < 0) lx += SIZE;
            if (lz < 0) lz += SIZE;

            if (lx >= 0 && lx < Chunk::SIZE &&
                lz >= 0 && lz < Chunk::SIZE &&
                y >= 0 && y < Chunk::HEIGHT)
            {
                int idx = Chunk::index(lx, y, lz);
                it->second.blocks[idx] = type;
                it->second.needsMeshUpdate = true;

                if (IsOnChunkEdge(lx, lz)) {
                    // 优化：根据方块位置只标记受影响方向的邻居边
                    if (lx == 0) {
                        // 西边缘
                        MarkNeighborEdgeForUpdate(cx, cz, 1);
                    }
                    else if (lx == Chunk::SIZE - 1) {
                        // 东边缘
                        MarkNeighborEdgeForUpdate(cx, cz, 0);
                    }
                    
                    if (lz == 0) {
                        // 南边缘
                        MarkNeighborEdgeForUpdate(cx, cz, 3);
                    }
                    else if (lz == Chunk::SIZE - 1) {
                        // 北边缘
                        MarkNeighborEdgeForUpdate(cx, cz, 2);
                    }
                }

                EnqueueChunkUpdate({ cx, cz });
            }
        }
    }
}

// 优化版本：只标记实际存在的邻居的对应边
void World::MarkNeighborEdgeForUpdate(int cx, int cz, int direction) {
    // direction: 0=东, 1=西, 2=北, 3=南
    // 对应邻居块在 dirtyEdges 中的索引需要反向（因为是从邻居的角度看）
    const std::array<std::pair<int, int>, 4> directions = { {
        {1, 0},   // 0: 东 (cx+1, cz)
        {-1, 0},  // 1: 西 (cx-1, cz)
        {0, 1},   // 2: 北 (cx, cz+1)
        {0, -1}   // 3: 南 (cx, cz-1)
    } };
    
    // 反向方向索引：从当前区块看的方向 -> 邻居区块看到的反向
    // 东(0) -> 西(1), 西(1) -> 东(0), 北(2) -> 南(3), 南(3) -> 北(2)
    const std::array<int, 4> reverseDirection = { 1, 0, 3, 2 };

    if (direction < 0 || direction >= 4) return;
    
    const auto& [dx, dz] = directions[direction];
    auto neighborCoord = std::make_pair(cx + dx, cz + dz);
    
    auto it = chunks.find(neighborCoord);
    if (it != chunks.end()) {
        // 标记邻居区块的对应边为脏
        it->second.dirtyEdges[reverseDirection[direction]] = true;
        it->second.needsMeshUpdate = true;
        EnqueueChunkUpdate(neighborCoord);
    }
}

void World::MarkNeighborsForUpdate(int cx, int cz) {
    // 标记所有4个方向的邻居（如果存在）- 现在使用优化版本
    for (int dir = 0; dir < 4; ++dir) {
        MarkNeighborEdgeForUpdate(cx, cz, dir);
    }

    const std::array<std::pair<int, int>, 4> directions = { {
        {1, 0},  
        {-1, 0}, 
        {0, 1},  
        {0, -1} 
    } };

    for (const auto& [dx, dz] : directions) {
        auto neighborCoord = std::make_pair(cx + dx, cz + dz);
        if (chunks.count(neighborCoord)) {
            chunks[neighborCoord].needsMeshUpdate = true;
            EnqueueChunkUpdate(neighborCoord);
        }
    }
}

void World::EnqueueChunkUpdate(const std::pair<int, int>& coord) {
    if (inQueueChunks.insert(coord).second) {
        int chunkX = coord.first;
        int chunkZ = coord.second;
        glm::vec3 chunkCenter(
            chunkX * Chunk::SIZE + Chunk::SIZE / 2.0f,
            Chunk::HEIGHT / 2.0f,
            chunkZ * Chunk::SIZE + Chunk::SIZE / 2.0f
        );
        float distance = glm::distance(chunkCenter, currentCameraPos);
        chunkUpdateQueue.emplace(chunkX, chunkZ, distance);
    }
}

std::vector<Rect> UnifiedGreedyMesh(const std::vector<glm::ivec2>& positions, float mergeThreshold = 1.0f) {
    if (positions.empty()) return {};

    int minX = positions[0].x, maxX = positions[0].x;
    int minY = positions[0].y, maxY = positions[0].y;

    size_t i = 1;
    const size_t n = positions.size();
    for (; i + 3 < n; i += 4) {
        minX = std::min(minX, std::min(positions[i].x, std::min(positions[i + 1].x, positions[i + 2].x)));
        minX = std::min(minX, positions[i + 3].x);
        maxX = std::max(maxX, std::max(positions[i].x, std::max(positions[i + 1].x, positions[i + 2].x)));
        maxX = std::max(maxX, positions[i + 3].x);

        minY = std::min(minY, std::min(positions[i].y, std::min(positions[i + 1].y, positions[i + 2].y)));
        minY = std::min(minY, positions[i + 3].y);
        maxY = std::max(maxY, std::max(positions[i].y, std::max(positions[i + 1].y, positions[i + 2].y)));
        maxY = std::max(maxY, positions[i + 3].y);
    }

    for (; i < n; ++i) {
        minX = std::min(minX, positions[i].x);
        maxX = std::max(maxX, positions[i].x);
        minY = std::min(minY, positions[i].y);
        maxY = std::max(maxY, positions[i].y);
    }

    const int width = maxX - minX + 1;
    const int height = maxY - minY + 1;
    const int gridSize = width * height;

    // 防御（崩溃修复，不影响正常生成）：positions 坐标范围异常（width/height 溢出或过大）时
    // 放弃 greedy，避免 posToIndex 的 int 运算溢出导致越界访问损坏堆
    if (width <= 0 || height <= 0 || gridSize <= 0 || gridSize > 4096) {
        return {};
    }

    std::vector<uint8_t> grid(gridSize, 0);
    std::vector<uint8_t> visited(gridSize, 0);

    auto posToIndex = [width](int x, int y) {
        return y * width + x;
        };

    for (const auto& pos : positions) {
        const int x = pos.x - minX;
        const int y = pos.y - minY;
        if (x >= 0 && x < width && y >= 0 && y < height) {
            grid[posToIndex(x, y)] = 1;
        }
    }

    std::vector<Rect> rects;
    rects.reserve(std::min(gridSize / 16, 16)); 

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width;) {
            const int idx = posToIndex(x, y);

            if (grid[idx] == 0 || visited[idx] != 0) {
                ++x;
                continue;
            }

            int startX = x;
            int endX = x;
            int currentEndX = x;
            int maxHeight = 1;

            while (endX < width && grid[posToIndex(endX, y)] && !visited[posToIndex(endX, y)]) {
                ++endX;
            }
            endX--; 
            currentEndX = endX;


            for (int y2 = y + 1; y2 < height; ++y2) {

                int validCount = 0;
                int totalCells = currentEndX - startX + 1;

                for (int x2 = startX; x2 <= currentEndX; ++x2) {
                    const int testIdx = posToIndex(x2, y2);
                    if (grid[testIdx] && !visited[testIdx]) {
                        validCount++;
                    }
                }

                bool shouldExtend = false;
                if (mergeThreshold >= 1.0f) {
                    shouldExtend = (validCount == totalCells);
                }
                else {
                    shouldExtend = (static_cast<float>(validCount) >= mergeThreshold * totalCells);
                }

                if (!shouldExtend) break;

                maxHeight++;

                int extendX = currentEndX + 1;
                while (extendX < width) {
                    const int testIdx = posToIndex(extendX, y2);
                    if (!grid[testIdx] || visited[testIdx]) break;
                    bool columnValid = true;
                    for (int h = 0; h < maxHeight; ++h) {
                        const int colIdx = posToIndex(extendX, y + h);
                        if (!grid[colIdx] || visited[colIdx]) {
                            columnValid = false;
                            break;
                        }
                    }

                    if (columnValid) {
                        extendX++;
                    }
                    else {
                        break;
                    }
                }
                extendX--; 
                if (extendX > currentEndX) {
                    currentEndX = extendX;
                }
            }

            for (int h = 0; h < maxHeight; ++h) {
                const int rowIdx = posToIndex(startX, y + h);
                int fillCount = currentEndX - startX + 1;
                // 防御（崩溃修复，不影响正常生成）：clamp 计数，确保 fill_n 不越界写 visited
                if (fillCount < 0) fillCount = 0;
                if (rowIdx < 0) fillCount = 0;
                if (rowIdx + fillCount > gridSize) fillCount = gridSize - rowIdx;
                if (fillCount > 0) {
                    std::fill_n(&visited[rowIdx], fillCount, 1);
                }
            }

            const int MAX_RECT_HEIGHT = 16;
            if (maxHeight > MAX_RECT_HEIGHT) {
                for (int h = 0; h < maxHeight; h += MAX_RECT_HEIGHT) {
                    int segH = std::min(MAX_RECT_HEIGHT, maxHeight - h);
                    rects.push_back({
                        startX + minX,
                        (y + h) + minY,
                        currentEndX - startX + 1,
                        segH
                        });
                }
            }
            else {
                rects.push_back({
                    startX + minX,
                    y + minY,
                    currentEndX - startX + 1,
                    maxHeight
                    });
            }

            x = currentEndX + 1;
        }
    }

    if (rects.empty()) return rects;

    std::vector<Rect> finalRects;
    finalRects.reserve(rects.size());
    finalRects.push_back(rects[0]);

    for (size_t i = 1; i < rects.size(); ++i) {
        Rect& last = finalRects.back();
        const Rect& current = rects[i];

        if (last.y == current.y &&
            last.height == current.height &&
            last.x + last.width == current.x) {
            last.width += current.width;
        }
        else {
            finalRects.push_back(current);
        }
    }

    return finalRects;
}

MegaBlockInfo IsMegaBlockRendered(Chunk* neighborChunk, int bx, int by, int bz,
    const int LOD_SIZE, const int THRESHOLD,
    int chunkOffsetX = 0, int chunkOffsetZ = 0) {
    int startX = bx * LOD_SIZE + chunkOffsetX;
    int startY = by * LOD_SIZE;
    int startZ = bz * LOD_SIZE + chunkOffsetZ;

    int nonAirCount = 0;
    uint8_t dominantBlock = 0;
    std::unordered_map<uint8_t, int> blockCounts;
    int maxCount = 0;
    bool foundGrass = false;
    bool foundStone = false;

    for (int dx = 0; dx < LOD_SIZE; ++dx) {
        for (int dy = 0; dy < LOD_SIZE; ++dy) {
            for (int dz = 0; dz < LOD_SIZE; ++dz) {
                int x = startX + dx;
                int y = startY + dy;
                int z = startZ + dz;

                int localX = (x < 0) ? (Chunk::SIZE + x) : (x % Chunk::SIZE);
                int localZ = (z < 0) ? (Chunk::SIZE + z) : (z % Chunk::SIZE);

                if (localX < 0 || localX >= Chunk::SIZE ||
                    y < 0 || y >= Chunk::HEIGHT ||
                    localZ < 0 || localZ >= Chunk::SIZE) {
                    continue;
                }

                uint8_t blockType = neighborChunk->blocks[Chunk::index(localX, y, localZ)];

                if (blockType == 0 || blockType == 13 || blockType == 14)
                    continue;

                if (blockType == 3) { 
                    if (!foundGrass) {
                        dominantBlock = 3;
                        foundGrass = true;
                        nonAirCount += 2; 
                    }
                }
                else if (blockType == 1) { 
                    if (!foundGrass && !foundStone) {
                        dominantBlock = 1;
                        foundStone = true;
                        nonAirCount += 2; 
                    }
                }
                else if (!foundGrass && !foundStone) {
                    blockCounts[blockType]++;
                    if (blockCounts[blockType] > maxCount) {
                        maxCount = blockCounts[blockType];
                        dominantBlock = blockType;
                    }
                }

                nonAirCount++;
            }
        }
    }

    if (!foundGrass && !foundStone && maxCount > 0) {
        dominantBlock = dominantBlock;
    }

    bool shouldRender = false;
    if (foundGrass || foundStone) {
        shouldRender = (nonAirCount >= 1);
    }
    else {
        shouldRender = (nonAirCount >= THRESHOLD);
    }

    return { shouldRender, dominantBlock };
}


constexpr std::array<glm::vec3, 6> FACE_OFFSETS = {
    glm::vec3(0.0f, 0.0f, 1.0f),   // Z+ �棨����
    glm::vec3(0.0f, 0.0f, 0.0f),   // Z- �棨�ϣ�
    glm::vec3(0.0f, 0.0f, 0.0f),   // X- �棨����
    glm::vec3(1.0f, 0.0f, 0.0f),   // X+ �棨����
    glm::vec3(0.0f, 1.0f, 0.0f),   // Y+ �棨�ϣ�
    glm::vec3(0.0f, 0.0f, 0.0f)    // Y- �棨�£�
};

constexpr std::array<glm::vec3, 6> FACE_CENTERS = {
    glm::vec3(0.5f, 0.5f, 0.0f),   // Z+ ��
    glm::vec3(0.5f, 0.5f, 0.0f),   // Z- ��
    glm::vec3(0.0f, 0.5f, 0.5f),   // X- ��
    glm::vec3(0.0f, 0.5f, 0.5f),   // X+ ��
    glm::vec3(0.5f, 0.0f, 0.5f),   // Y+ ��
    glm::vec3(0.5f, 0.0f, 0.5f)    // Y- ��
};

uint8_t GetFaceTextureIndex(uint8_t blockType, int face) {
    BlockManager& blockManager = BlockManager::GetInstance();
    static const BlockType* blockTypes = blockManager.GetBlockTypesArray();
    const BlockType& type = blockTypes[blockType];

    switch (face) {
    case 0: return type.frontTexIndex;
    case 1: return type.backTexIndex;
    case 2: return type.leftTexIndex;
    case 3: return type.rightTexIndex;
    case 4: return type.topTexIndex;
    case 5: return type.bottomTexIndex;
    default: return 0;
    }
}

void World::GenerateLODMesh(Chunk* chunk, const int LOD_SIZE) {
    chunk->opaqueFaceInstances.clear();
    chunk->transparentFaceInstances.clear();

    auto start = std::chrono::high_resolution_clock::now();

    const int cx = chunk->chunkX;  
    const int cz = chunk->chunkZ; 
    const int chunkGridX = cx / Chunk::SIZE;  
    const int chunkGridZ = cz / Chunk::SIZE; 

    Chunk* eastChunk = nullptr;
    Chunk* westChunk = nullptr;
    Chunk* southChunk = nullptr;
    Chunk* northChunk = nullptr;

    auto eastIt = chunks.find({ chunkGridX + 1, chunkGridZ });
    if (eastIt != chunks.end()) eastChunk = &eastIt->second;

    auto westIt = chunks.find({ chunkGridX - 1, chunkGridZ });
    if (westIt != chunks.end()) westChunk = &westIt->second;

    auto southIt = chunks.find({ chunkGridX, chunkGridZ - 1 });
    if (southIt != chunks.end()) southChunk = &southIt->second;

    auto northIt = chunks.find({ chunkGridX, chunkGridZ + 1 });
    if (northIt != chunks.end()) northChunk = &northIt->second;

    BlockManager& blockManager = BlockManager::GetInstance();
    static const BlockType* blockTypes = blockManager.GetBlockTypesArray();

    const int THRESHOLD = 2; 

    int blocksX = (Chunk::SIZE + LOD_SIZE - 1) / LOD_SIZE;
    int blocksY = (Chunk::HEIGHT + LOD_SIZE - 1) / LOD_SIZE;
    int blocksZ = (Chunk::SIZE + LOD_SIZE - 1) / LOD_SIZE;

    std::vector<MegaBlockInfo> megaBlocksInfo(blocksX * blocksY * blocksZ);

    for (int bx = 0; bx < blocksX; ++bx) {
        for (int by = 0; by < blocksY; ++by) {
            for (int bz = 0; bz < blocksZ; ++bz) {
                int nonAirCount = 0;
                std::unordered_map<uint8_t, int> blockCounts;
                uint8_t dominantBlock = 0;
                int maxCount = 0;
                bool foundGrass = false;
                bool foundStone = false;
                int idx = bx * (blocksY * blocksZ) + by * blocksZ + bz;
                for (int dx = 0; dx < LOD_SIZE; ++dx) {
                    for (int dy = 0; dy < LOD_SIZE; ++dy) {
                        for (int dz = 0; dz < LOD_SIZE; ++dz) {
                            int x = bx * LOD_SIZE + dx;
                            int y = by * LOD_SIZE + dy;
                            int z = bz * LOD_SIZE + dz;

                            if (x >= Chunk::SIZE || y >= Chunk::HEIGHT || z >= Chunk::SIZE)
                                continue;

                            uint8_t blockType = chunk->blocks[Chunk::index(x, y, z)];

                            if (blockType == 0 || blockType == 13 || blockType == 14)
                                continue;

                            if (blockType == 3) { 
                                if (!foundGrass) {
                                    dominantBlock = 3;
                                    foundGrass = true;
                                    nonAirCount += 2; 
                                }
                            }
                            else if (blockType == 8) { 
                                if (!foundGrass) {
                                    dominantBlock = 8;
                                    foundStone = true;
                                    nonAirCount += 2; 
                                }
                            }
                            else if (!foundGrass && !foundStone) {

                                blockCounts[blockType]++;
                                if (blockCounts[blockType] > maxCount) {
                                    maxCount = blockCounts[blockType];
                                    dominantBlock = blockType;
                                }
                            }

                            nonAirCount++;
                        }
                    }
                }

                if (!foundGrass && !foundStone && maxCount > 0) {
                    dominantBlock = dominantBlock;
                }


                bool shouldRender = false;
                if (foundGrass) {
                    shouldRender = (nonAirCount >= 1);
                }
                else {
                    shouldRender = (nonAirCount >= THRESHOLD);
                }

                megaBlocksInfo[idx] = { shouldRender, dominantBlock };
            }
        }
    }

    thread_local std::unordered_map<FaceGroupKey, std::vector<glm::ivec2>> faceGroups;
    faceGroups.clear();
    faceGroups.reserve(256);

    for (int bx = 0; bx < blocksX; ++bx) {
        for (int by = 0; by < blocksY; ++by) {
            for (int bz = 0; bz < blocksZ; ++bz) {
                int idx = bx * (blocksY * blocksZ) + by * blocksZ + bz;
                const MegaBlockInfo& info = megaBlocksInfo[idx];
                if (!info.shouldRender || info.dominantBlock == 0) continue;

                const uint8_t blockType = info.dominantBlock;


                for (int face = 0; face < 6; ++face) {
                    bool neighborExists = false;
                    bool neighborLoaded = true;
                    uint8_t neighborType = 0; 
                    Chunk* neighborChunkPtr = nullptr; 


                    switch (face) {
                    case 0: // Z+ �棨����
                        if (bz < blocksZ - 1) {
                            int neighborIdx = bx * (blocksY * blocksZ) + by * blocksZ + (bz + 1);
                            neighborExists = megaBlocksInfo[neighborIdx].shouldRender;
                            neighborType = megaBlocksInfo[neighborIdx].dominantBlock;
                        }
                        else {
                            neighborLoaded = (northChunk != nullptr);
                            if (neighborLoaded) {
                                auto neighborInfo = IsMegaBlockRendered(northChunk, bx, by, 0, LOD_SIZE, THRESHOLD);
                                neighborChunkPtr = northChunk; // ����ָ��
                                neighborExists = neighborInfo.shouldRender;
                                neighborType = neighborInfo.dominantBlock;
                                //adjChunkOffsetZ = Chunk::SIZE; // ���������Zƫ��
                            }
                        }
                        break;
                    case 1: // Z- �棨�ϣ�
                        if (bz > 0) {
                            int neighborIdx = bx * (blocksY * blocksZ) + by * blocksZ + (bz - 1);
                            neighborExists = megaBlocksInfo[neighborIdx].shouldRender;
                            neighborType = megaBlocksInfo[neighborIdx].dominantBlock;
                        }
                        else {
                            neighborLoaded = (southChunk != nullptr);
                            if (neighborLoaded) {
                                auto neighborInfo = IsMegaBlockRendered(southChunk, bx, by, blocksZ - 1, LOD_SIZE, THRESHOLD);
                                neighborChunkPtr = southChunk; // ����ָ��
                                neighborExists = neighborInfo.shouldRender;
                                neighborType = neighborInfo.dominantBlock;
                                //adjChunkOffsetZ = -Chunk::SIZE; // ���������Zƫ��
                            }
                        }
                        break;
                    case 2: // X- �棨����
                        if (bx > 0) {
                            int neighborIdx = (bx - 1) * (blocksY * blocksZ) + by * blocksZ + bz;
                            neighborExists = megaBlocksInfo[neighborIdx].shouldRender;
                            neighborType = megaBlocksInfo[neighborIdx].dominantBlock;
                        }
                        else {
                            neighborLoaded = (westChunk != nullptr);
                            if (neighborLoaded) {
                                auto neighborInfo = IsMegaBlockRendered(westChunk, blocksX - 1, by, bz, LOD_SIZE, THRESHOLD);
                                neighborChunkPtr = westChunk; // ����ָ��
                                neighborExists = neighborInfo.shouldRender;
                                neighborType = neighborInfo.dominantBlock;
                                //adjChunkOffsetX = -Chunk::SIZE; // ���������Xƫ��
                            }
                        }
                        break;
                    case 3: // X+ �棨����
                        if (bx < blocksX - 1) {
                            int neighborIdx = (bx + 1) * (blocksY * blocksZ) + by * blocksZ + bz;
                            neighborExists = megaBlocksInfo[neighborIdx].shouldRender;
                            neighborType = megaBlocksInfo[neighborIdx].dominantBlock;
                        }
                        else {
                            neighborLoaded = (eastChunk != nullptr);
                            if (neighborLoaded) {
                                auto neighborInfo = IsMegaBlockRendered(eastChunk, 0, by, bz, LOD_SIZE, THRESHOLD);
                                neighborChunkPtr = eastChunk; // ����ָ��
                                neighborExists = neighborInfo.shouldRender;
                                neighborType = neighborInfo.dominantBlock;
                                //adjChunkOffsetX = Chunk::SIZE; // ���������Xƫ��
                            }
                        }
                        break;
                    case 4: // Y+ �棨�ϣ�
                        if (by < blocksY - 1) {
                            int neighborIdx = bx * (blocksY * blocksZ) + (by + 1) * blocksZ + bz;
                            neighborExists = megaBlocksInfo[neighborIdx].shouldRender;
                            neighborType = megaBlocksInfo[neighborIdx].dominantBlock;
                        }
                        break;
                    case 5: // Y- �棨�£�
                        if (by > 0) {
                            int neighborIdx = bx * (blocksY * blocksZ) + (by - 1) * blocksZ + bz;
                            neighborExists = megaBlocksInfo[neighborIdx].shouldRender;
                            neighborType = megaBlocksInfo[neighborIdx].dominantBlock;
                        }
                        break;
                    }

                    // �޸ģ������������δ���أ������������Ⱦ
                    if (!neighborLoaded) {
                        continue;
                    }
                    /*
                    bool lodMismatch = false;
                    // ֻ������߽紦���LOD��ƥ��
                    bool isChunkBorder = false;
                    if (face == 0 && bz == blocksZ - 1) isChunkBorder = true;     // Z+ �߽�
                    else if (face == 1 && bz == 0) isChunkBorder = true;          // Z- �߽�
                    else if (face == 2 && bx == 0) isChunkBorder = true;          // X- �߽�
                    else if (face == 3 && bx == blocksX - 1) isChunkBorder = true;// X+ �߽�

                    // ֻ��������߽紦�ż��LOD��ƥ��
                    if (isChunkBorder && neighborLoaded && neighborChunkPtr) {
                        int neighborLodLevel = neighborChunkPtr->lodLevel;

                        // ���LOD�㼶��ͬ
                        if (neighborLodLevel != chunk->lodLevel) {
                            lodMismatch = true;

                            // ǿ����Ⱦ����棬�������ڷ����Ƿ����
                            neighborExists = false;
                        }
                    }
                    */
                    // ˮ�������⴦����ֻ�е����ڷ��鲻��ˮʱ����Ⱦ
                    bool isWater = (blockType == 12);

                    if (isWater) {
                        // ����ھ�Ҳ��ˮ��������Ⱦ
                        if (neighborExists && neighborType == 12) {
                            continue;
                        }
                    }
                    // ��ˮ�������⴦����ֻ�е����ڷ�����ˮʱ����Ⱦ
                    else {
                        // ����ھ���ˮ������Ҫ��Ⱦ������������
                        if (neighborExists && neighborType != 12) {
                            continue;
                        }
                    }


                    // ��ȡ��������
                    uint8_t texIndex = GetFaceTextureIndex(blockType, face);

                    // ����������ȷ���̶������ƽ������ - �����߽�λ��
                    int fixedCoord = 0;
                    glm::ivec2 planePos(0, 0);

                    switch (face) {
                    case 0: // Z+ (��)
                        fixedCoord = cz + (bz + 1) * LOD_SIZE;
                        planePos = glm::ivec2(bx, by);
                        break;
                    case 1: // Z- (��)
                        fixedCoord = cz + bz * LOD_SIZE;
                        planePos = glm::ivec2(bx, by);
                        break;
                    case 2: // X- (��)
                        fixedCoord = cx + bx * LOD_SIZE;
                        planePos = glm::ivec2(bz, by);
                        break;
                    case 3: // X+ (��)
                        fixedCoord = cx + (bx + 1) * LOD_SIZE;
                        planePos = glm::ivec2(bz, by);
                        break;
                    case 4: // Y+ (��)
                        fixedCoord = by * LOD_SIZE + LOD_SIZE; // ����Y�������
                        planePos = glm::ivec2(bx, bz);
                        break;
                    case 5: // Y- (��)
                        fixedCoord = by * LOD_SIZE;
                        planePos = glm::ivec2(bx, bz);
                        break;
                    }

                    // ���������
                    FaceGroupKey key(face, blockType, texIndex, fixedCoord);
                    faceGroups[key].push_back(planePos);
                }
            }
        }
    }

    // ����ÿ������
    static std::vector<Rect> rects;
    rects.clear();

    for (auto& [key, positions] : faceGroups) {
        // ʹ��̰�������㷨�ϲ���
        rects = UnifiedGreedyMesh(positions, 1.0f);

        // �Ӽ�ֵ�������������
        uint8_t face = static_cast<uint8_t>((key.key >> 48) & 0xFF);
        uint8_t blockType = static_cast<uint8_t>((key.key >> 40) & 0xFF);
        uint8_t uvIndex = static_cast<uint8_t>((key.key >> 32) & 0xFF);
        int fixedCoord = static_cast<int>(key.key & 0xFFFFFFFF);

        for (const auto& rect : rects) {
            glm::vec3 worldPos;
            glm::vec2 size(rect.width * LOD_SIZE, rect.height * LOD_SIZE);

            // �����淽������������� - �����߽�λ��
            switch (face) {
            case 0: // Z+ (��)
                worldPos = glm::vec3(
                    cx + rect.x * LOD_SIZE + size.x * 0.5f,
                    rect.y * LOD_SIZE + size.y * 0.5f,
                    fixedCoord
                );
                break;
            case 1: // Z- (��)
                worldPos = glm::vec3(
                    cx + rect.x * LOD_SIZE + size.x * 0.5f,
                    rect.y * LOD_SIZE + size.y * 0.5f,
                    fixedCoord
                );
                break;
            case 2: // X- (��)
                worldPos = glm::vec3(
                    fixedCoord,
                    rect.y * LOD_SIZE + size.y * 0.5f,
                    cz + rect.x * LOD_SIZE + size.x * 0.5f
                );
                break;
            case 3: // X+ (��)
                worldPos = glm::vec3(
                    fixedCoord,
                    rect.y * LOD_SIZE + size.y * 0.5f,
                    cz + rect.x * LOD_SIZE + size.x * 0.5f
                );
                break;
            case 4: // Y+ (��)
                worldPos = glm::vec3(
                    cx + rect.x * LOD_SIZE + size.x * 0.5f,
                    fixedCoord,
                    cz + rect.y * LOD_SIZE + size.y * 0.5f
                );
                break;
            case 5: // Y- (��)
                worldPos = glm::vec3(
                    cx + rect.x * LOD_SIZE + size.x * 0.5f,
                    fixedCoord,
                    cz + rect.y * LOD_SIZE + size.y * 0.5f
                );
                break;
            }
            worldPos.y -= 1.0f;

            if (blockType == 12) {//water
                chunk->transparentFaceInstances.emplace_back(worldPos, static_cast<uint8_t>(face), blockType, uvIndex, size);
            }
            else {
                chunk->opaqueFaceInstances.emplace_back(worldPos, static_cast<uint8_t>(face), blockType, uvIndex, size);
            }
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    std::chrono::duration<double, std::milli> ms_double = end - start;
    //std::cout <<"lod��"<< std::fixed << std::setprecision(3) << ms_double.count() << " ms" << std::endl;
    totalMeshTime += ms_double.count();
}


void World::GenerateMesh(Chunk* chunk) {
    std::lock_guard<std::mutex> lock(modificationMutex);
    chunk->opaqueFaceInstances.clear();
    chunk->transparentFaceInstances.clear();
    chunk->alphaFaceInstances.clear();
    auto start = std::chrono::high_resolution_clock::now();
    static std::vector<Rect> rects;
    rects.clear();

    thread_local std::unordered_map<FaceGroupKey, std::vector<glm::ivec2>> faceGroups;
    faceGroups.clear();
    faceGroups.reserve(256);

    BlockManager& blockManager = BlockManager::GetInstance();
    static const BlockType* blockTypes = blockManager.GetBlockTypesArray();

    std::vector<glm::ivec3> plantPositions;

    const int cx = chunk->chunkX / Chunk::SIZE;
    const int cz = chunk->chunkZ / Chunk::SIZE;

    Chunk* eastChunk = nullptr;
    Chunk* westChunk = nullptr;
    Chunk* southChunk = nullptr;
    Chunk* northChunk = nullptr;
    
    // 获取邻居区块的LOD级别（用于边缘检测）
    int eastLod = -1, westLod = -1, southLod = -1, northLod = -1;

    auto eastIt = chunks.find({ cx + 1, cz });
    if (eastIt != chunks.end()) {
        eastChunk = &eastIt->second;
        eastLod = eastIt->second.lodLevel;
    }

    auto westIt = chunks.find({ cx - 1, cz });
    if (westIt != chunks.end()) {
        westChunk = &westIt->second;
        westLod = westIt->second.lodLevel;
    }

    auto southIt = chunks.find({ cx, cz - 1 });
    if (southIt != chunks.end()) {
        southChunk = &southIt->second;
        southLod = southIt->second.lodLevel;
    }

    auto northIt = chunks.find({ cx, cz + 1 });
    if (northIt != chunks.end()) {
        northChunk = &northIt->second;
        northLod = northIt->second.lodLevel;
    }

    const std::array<glm::ivec3, 6> dirs = {
    glm::ivec3(1,0,0), glm::ivec3(-1,0,0),
    glm::ivec3(0,1,0), glm::ivec3(0,-1,0),
    glm::ivec3(0,0,1), glm::ivec3(0,0,-1)
    };

    glm::vec3 chunkCenter(
        chunk->chunkX + Chunk::SIZE / 2.0f,
        Chunk::HEIGHT / 2.0f,
        chunk->chunkZ + Chunk::SIZE / 2.0f
    );
    float distance = glm::distance(chunkCenter, currentCameraPos);
    int lodLevel = 5;  // 默认最高LOD级别（最粗糙）
    constexpr int distances[] = {0, 16, 32, 48, 72,96};
    
    // 计算LOD级别：0=最详细（不合并面），1-5=逐渐粗糙（使用greedy mesh）
    for (int i = 0; i <= 5; i++) {
        if (distance < distances[i] * Chunk::SIZE) {
            lodLevel = i;  // lodLevel = 0表示最详细，不使用greedy mesh
            break;
        }
    }
    // 注意：必须在调用GenerateLODMesh之前更新chunk->lodLevel，因为GenerateLODMesh内部会检查邻居的LOD级别
    chunk->lodLevel = lodLevel;
    //GenerateLODMesh(chunk, 1);
    //return;
    // LOD从1开始：1=最详细，2=中等，3+=使用LOD网格
    if (lodLevel >= 3) {
         GenerateLODMesh(chunk, 2);
         if (lodLevel >= 4) {
             GenerateLODMesh(chunk, 4);
         }
         return;
     }
   

    for (int x = 0; x < Chunk::SIZE; x++) {
        for (int z = 0; z < Chunk::SIZE; z++) {
           // uint8_t currentSkyLight = 15; // �Ӷ�����ʼ
            for (int y = Chunk::HEIGHT - 1; y >= 0; y--) {
                uint8_t type = chunk->blocks[Chunk::index(x, y, z)];
                if (type == 0 || y < 1 || y > 255) continue;//�������������±߽�
                if (type == 13 || type == 14) {
                    if (lodLevel <= 1) {  // lodLevel从1开始，所以<=2表示最详细和中等级别
                        plantPositions.emplace_back(x, y, z);
                    }
                    continue;
                }
                /*
                uint8_t AO = 1;//��ʱ����Ϊ1
                chunk->lightData[Chunk::index(x, y, z)] = AO;


                if (type != 0) {
                    currentSkyLight = (currentSkyLight >= 1) ? (currentSkyLight - 1) : 0;
                }

                // ���¹������ݣ�����λ��չ⣬����λ����Ᵽ�ֲ��䣩
                chunk->lightData[Chunk::index(x, y, z)] = (currentSkyLight << 4) | (chunk->lightData[Chunk::index(x, y, z)] & 0x0F);
            */
                bool surrounded = true;
                uint8_t neighborType = 0;
                for (int i = 0; i < 6; ++i) {
                    const auto& dir = dirs[i];
                    int nx = x + dir.x;
                    int ny = y + dir.y;
                    int nz = z + dir.z;
                    if (nx >= 0 && nx < Chunk::SIZE && nz >= 0 && nz < Chunk::SIZE) {
                        neighborType = chunk->blocks[Chunk::index(nx, ny, nz)];
                    }
                    else {
                        Chunk* neighborChunk = nullptr;
                        if (dir.x > 0) neighborChunk = eastChunk;
                        else if (dir.x < 0) neighborChunk = westChunk;
                        else if (dir.z > 0) neighborChunk = northChunk;
                        else if (dir.z < 0) neighborChunk = southChunk;

                        if (neighborChunk) {
                            constexpr int SIZE_MASK = Chunk::SIZE - 1;
                            int lx = nx & SIZE_MASK;
                            int lz = nz & SIZE_MASK;
                            neighborType = neighborChunk->blocks[Chunk::index(lx, ny, lz)];
                        }

                    }

                    if (neighborType == type) continue;
                    bool isTransparent = (neighborType == 5 || neighborType == 6 || neighborType == 13 || neighborType == 14 || neighborType == 12);
                    if (neighborType == 0 || isTransparent) {
                        surrounded = false;
                        break;
                    }
                }


                if (surrounded) continue;

                for (const auto& dir : dirs) {
                    int nx = x + dir.x;
                    int ny = y + dir.y;
                    int nz = z + dir.z;

                    if (nx >= 0 && nx < Chunk::SIZE &&
                        nz >= 0 && nz < Chunk::SIZE) {
                        neighborType = chunk->blocks[Chunk::index(nx, ny, nz)];
                    }
                    else {
                        Chunk* neighborChunk = nullptr;
                        if (dir.x > 0 && eastChunk) neighborChunk = eastChunk;
                        else if (dir.x < 0 && westChunk) neighborChunk = westChunk;
                        else if (dir.z > 0 && northChunk) neighborChunk = northChunk;
                        else if (dir.z < 0 && southChunk) neighborChunk = southChunk;

                        if (neighborChunk) {
                            constexpr int SIZE_MASK = Chunk::SIZE - 1;
                            int lx = nx & SIZE_MASK;
                            int lz = nz & SIZE_MASK;
                            neighborType = neighborChunk->blocks[Chunk::index(lx, ny, lz)];
                        }
                        else {
                            neighborType = 255; // ����δ���أ���Ϊ�߽緽��
                        }
                    }
                    // LOD边缘检测：如果邻居区块使用GenerateLODMesh（lodLevel >= 4），即使neighborType == type也要保留边缘面
                    bool forceRenderEdge = false;
                    if (neighborType == type) {
                        // 检查是否在区块边缘且邻居使用GenerateLODMesh
                        if ((nx < 0 || nx >= Chunk::SIZE || nz < 0 || nz >= Chunk::SIZE)) {
                            int neighborLod = -1;
                            if (dir.x > 0) neighborLod = eastLod;
                            else if (dir.x < 0) neighborLod = westLod;
                            else if (dir.z > 0) neighborLod = northLod;
                            else if (dir.z < 0) neighborLod = southLod;
                            
                            // 如果邻居使用GenerateLODMesh（lodLevel >= 4）且当前区块使用GenerateMesh（lodLevel <= 3），需要保留边缘面
                            if (neighborLod >= 4 && lodLevel <= 3) {
                                forceRenderEdge = true;
                            }
                        }
                        if (!forceRenderEdge) continue; // 正常剔除
                    }
                    
                    bool isTransparent = (neighborType == 5 || neighborType == 6 || neighborType == 13 || neighborType == 14 || neighborType == 12);
                    //bool isTransparent = blockManager.IsTransparent(neighborType);
                    // 强制渲染边缘面或在正常条件下需要渲染的面
                    if (forceRenderEdge || ((neighborType == 0 || isTransparent) && neighborType != 255)) {
                        uint8_t faceIndex;
                        uint8_t texIndex;
                        int fixedCoord;
                        glm::ivec2 pos;
                        const BlockType& blockType = blockTypes[type]; // ���ñ��⿽��
                        // ȷ��������Ҫ�������
                        if (dir.x != 0) { // X�᷽��
                            bool positive = (dir.x > 0);
                            faceIndex = positive ? static_cast<uint8_t>(3) : static_cast<uint8_t>(2);
                            texIndex = GetFaceTextureIndex(type, faceIndex);
                            fixedCoord = positive ? (x + 1) : x;
                            pos = glm::ivec2(z, y); // X�᷽��ʹ��ZYƽ������
                        }
                        else if (dir.y != 0) { // Y�᷽��
                            bool positive = (dir.y > 0);
                            faceIndex = positive ? static_cast<uint8_t>(4) : static_cast<uint8_t>(5);
                            texIndex = GetFaceTextureIndex(type, faceIndex);
                            fixedCoord = positive ? (y + 1) : y;
                            pos = glm::ivec2(x, z); // Y�᷽��ʹ��XZƽ������
                        }
                        else { // Z�᷽��
                            bool positive = (dir.z > 0);
                            faceIndex = positive ? static_cast<uint8_t>(0) : static_cast<uint8_t>(1);
                            texIndex = GetFaceTextureIndex(type, faceIndex);
                            fixedCoord = positive ? (z + 1) : z;
                            pos = glm::ivec2(x, y); // Z�᷽��ʹ��XYƽ������
                        }
                        FaceGroupKey key(faceIndex, type, texIndex, fixedCoord);
                        faceGroups[key].push_back(pos);
                    }
                };
            }
        }

    }

    // ����ÿ�����飬���ɺϲ���ľ���
    for (auto& [key, positions] : faceGroups) {
        rects.clear();
        if (lodLevel > 0) {
            rects = UnifiedGreedyMesh(positions, 1.0f);
            if (lodLevel > 1) rects = UnifiedGreedyMesh(positions, 0.5f);
            if (lodLevel > 2) rects = UnifiedGreedyMesh(positions, 0.1f);
        }
        else {
            rects.reserve(positions.size());
            for (const auto& pos : positions) {
                rects.push_back({ pos.x, pos.y, 1, 1 });
            }
        }
        // lodLevel >= 3 时使用GenerateLODMesh，不会到达这里

        // ��64λ��ֵ�������������
        uint8_t faceData = static_cast<uint8_t>((key.key >> 48) & 0xFF);
        uint8_t blockType = static_cast<uint8_t>((key.key >> 40) & 0xFF);
        uint8_t uvIndex = static_cast<uint8_t>((key.key >> 32) & 0xFF);
        int fixedCoord = static_cast<int>(key.key & 0xFFFFFFFF);

        for (const auto& rect : rects) {
            glm::vec3 worldPos;
            glm::vec2 size(rect.width, rect.height);

            // ʹ�ý���������
            switch (faceData) {
            case 2: // X-
            case 3: { // X+
                worldPos.x = chunk->chunkX + fixedCoord;
                worldPos.y = rect.y + rect.height * 0.5f;
                worldPos.z = chunk->chunkZ + rect.x + rect.width * 0.5f;
                break;
            }
            case 4: // Y+
            case 5: { // Y-
                worldPos.x = chunk->chunkX + rect.x + rect.width * 0.5f;
                worldPos.y = fixedCoord;
                worldPos.z = chunk->chunkZ + rect.y + rect.height * 0.5f;
                break;
            }
            case 0: // Z+
            case 1: { // Z-
                worldPos.x = chunk->chunkX + rect.x + rect.width * 0.5f;
                worldPos.y = rect.y + rect.height * 0.5f;
                worldPos.z = chunk->chunkZ + fixedCoord;
                break;
            }
            }

            glm::vec2 clampedSize = glm::clamp(size, 1.0f, 16.0f);
            bool isAlphatest = ((blockType == 5 && lodLevel <= 1) || blockType == 6 || blockType == 13 || blockType == 14);
            bool isTransparent = (blockType == 12);
            if (isAlphatest) {
                chunk->alphaFaceInstances.emplace_back(worldPos, faceData, blockType, uvIndex, clampedSize);
            }
            else if (isTransparent) {
                chunk->transparentFaceInstances.emplace_back(worldPos, faceData, blockType, uvIndex, clampedSize);
            }
            else {
                chunk->opaqueFaceInstances.emplace_back(worldPos, faceData, blockType, uvIndex, clampedSize);
                Chunk::FaceInstance faceInstance(worldPos, faceData, blockType, uvIndex, size);
                //CreateRigidBodyForFace(faceInstance, chunk->chunkX, chunk->chunkZ);
            }
        }
    }

    for (const auto& pos : plantPositions) {
        uint8_t plantType = chunk->blocks[Chunk::index(pos.x, pos.y, pos.z)];
        const BlockType& blockType = blockTypes[plantType];
        uint8_t uvIndex = blockType.rightTexIndex;
        //for (int i = 0;i < 4;i++) {
        chunk->alphaFaceInstances.emplace_back(
            glm::vec3(chunk->chunkX + pos.x + 0.5f, pos.y + 0.5f, chunk->chunkZ + pos.z + 0.5f),

            0,
            plantType,
            blockType.rightTexIndex,
            glm::vec2(1.0f)
        );
        //if (lodLevel == 1) {
        chunk->alphaFaceInstances.emplace_back(
            glm::vec3(chunk->chunkX + pos.x + 0.5f, pos.y + 0.5f, chunk->chunkZ + pos.z + 0.5f),

            2,
            plantType,
            blockType.rightTexIndex,
            glm::vec2(1.0f)
        );
        //}
    }

    chunk->lastMeshDistance = distance;
    chunk->needsMeshUpdate = false;
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    std::chrono::duration<double, std::milli> ms_double = end - start;
    //std::cout << std::fixed << std::setprecision(3) << ms_double.count() << " ms" << std::endl;
    totalMeshTime += ms_double.count();
}

void World::Update(const glm::vec3& cameraPos, const std::array<Plane, 6>& frustumPlanes) {
    currentCameraPos = cameraPos;
    currentFrustumPlanes = frustumPlanes;
    auto start = std::chrono::high_resolution_clock::now();
    auto t0 = start;
    ProcessModifications();

    int camChunkX = static_cast<int>(std::floor(cameraPos.x / Chunk::SIZE));
    int camChunkZ = static_cast<int>(std::floor(cameraPos.z / Chunk::SIZE));
    bool cameraChunkChanged = (camChunkX != cachedCamChunkX) || (camChunkZ != cachedCamChunkZ);
    bool frustumChanged = (frustumPlanes != lastFrustumPlanes);
    {
        std::unique_lock lock(chunksMutex);
        if (!initialGenerationDone || cameraChunkChanged || frustumChanged) {
            std::set<std::pair<int, int>> spiralCoords;
            // ������Ⱦ�뾶�ڵ����к�ѡ��������
            for (int dx = -renderRadius; dx <= renderRadius; ++dx) {
                for (int dz = -renderRadius; dz <= renderRadius; ++dz) {
                    if (dx * dx + dz * dz <= renderRadius * renderRadius) {
                        spiralCoords.insert({ camChunkX + dx, camChunkZ + dz });
                    }
                }
            }

            //std::set<std::pair<int, int>> neededChunks;
            /*for (const auto& coord : spiralCoords) {
                int chunkX = coord.first;
                int chunkZ = coord.second;
                glm::vec3 min(chunkX * Chunk::SIZE, 0.0f, chunkZ * Chunk::SIZE);
                glm::vec3 max((chunkX + 1) * Chunk::SIZE, static_cast<float>(Chunk::HEIGHT), (chunkZ + 1) * Chunk::SIZE);
                AABB chunkAABB(min, max);
                if (chunkAABB.IsInsideFrustum(currentFrustumPlanes)) {
                    neededChunks.insert(coord);
                }
            }*/

            // ж�ز���neededChunks�е����飬�����뻺��
            auto it = chunks.begin();
            while (it != chunks.end()) {
                if (!spiralCoords.count(it->first)) {
                    if (chunkCache.size() >= MAX_CHUNK_CACHE) {
                        chunkCache.erase(chunkCache.begin());
                    }
                    chunkCache[it->first] = std::move(it->second);
                    MarkNeighborsForUpdate(it->first.first, it->first.second);
                    it = chunks.erase(it);
                }
                else {
                    ++it;
                }
            }

            // ������Ҫ�����飬����ʹ�û���
            for (const auto& coord : spiralCoords) {
                if (!chunks.count(coord)) {
                    auto cacheIt = chunkCache.find(coord);
                    if (cacheIt != chunkCache.end()) {
                        chunks[coord] = std::move(cacheIt->second);
                        chunkCache.erase(cacheIt);
                        EnqueueChunkUpdate(coord);
                        MarkNeighborsForUpdate(coord.first, coord.second);
                    }
                    else {
                        Chunk newChunk;
                        newChunk.Generate(coord.first * Chunk::SIZE, coord.second * Chunk::SIZE);
                        chunks[coord] = newChunk;
                        EnqueueChunkUpdate(coord);
                        MarkNeighborsForUpdate(coord.first, coord.second);
                    }
                }
            }

            cachedCamChunkX = camChunkX;
            cachedCamChunkZ = camChunkZ;
            lastFrustumPlanes = frustumPlanes;
            initialGenerationDone = true;
            //lastNeededChunks = neededChunks;
        }
    }
    auto tSetEnd = std::chrono::high_resolution_clock::now();

    // 检查所有活跃区块的LOD级别变化，如果改变则标记需要更新
    {
        std::shared_lock<std::shared_mutex> lock(chunksMutex);
        constexpr int distances[] = {0, 16, 32, 48, 72,96};

        for (auto& [coord, chunk] : chunks) {
            glm::vec3 chunkCenter(
                chunk.chunkX + Chunk::SIZE / 2.0f,
                Chunk::HEIGHT / 2.0f,
                chunk.chunkZ + Chunk::SIZE / 2.0f
            );
            float distance = glm::distance(chunkCenter, currentCameraPos);
            int newLodLevel = 5;  // 默认最高LOD级别（最粗糙）
            
            for (int i = 0; i <= 5; i++) {
                if (distance < distances[i] * Chunk::SIZE) {
                    newLodLevel = i;  // lodLevel = 0表示最详细，不使用greedy mesh
                    break;
                }
            }
            
            // 如果LOD级别改变了，标记需要更新
            if (chunk.lodLevel != newLodLevel) {
                chunk.needsMeshUpdate = true;
                EnqueueChunkUpdate(coord);
            }
        }
    }
    auto tLodEnd = std::chrono::high_resolution_clock::now();

    // ����������¶��� - ʹ��maxUpdatesPerFrame����
    int updatesThisFrame = 0;
    while (!chunkUpdateQueue.empty() && updatesThisFrame < maxUpdatesPerFrame) {
        auto task = chunkUpdateQueue.top();
        chunkUpdateQueue.pop();
        inQueueChunks.erase(task.coord);

        // ��������Ƿ����ڼ���
        auto it = chunks.find(task.coord);
        if (it != chunks.end()) {
            Chunk& chunk = it->second;

            // ����Ƿ��������
            if (chunk.needsMeshUpdate) {
                // ����������̳߳ض���
                {
                    std::unique_lock<std::mutex> lock(taskMutex);
                    meshTaskQueue.push(task.coord);
                }
                taskCondition.notify_one();

                updatesThisFrame++;
            }
        }
    }
    auto tQueueEnd = std::chrono::high_resolution_clock::now();

    double frameMeshTime = 0.0;
    {
        std::lock_guard<std::mutex> lock(timeMutex);
        frameMeshTime = totalMeshTime;
        //totalMeshTime = 0;
    }

    if (frameMeshTime > 0.0 && frameMeshTime != lastPrintedMeshTime) {
        //std::cout << "Total mesh generation time: " << frameMeshTime << " ms" << std::endl;
        lastPrintedMeshTime = frameMeshTime; // ��������ӡ��ʱ��
    }

    // ===== World::Update 分阶段耗时（每 240 帧输出）=====
    {
        static double s_accProc = 0, s_accSet = 0, s_accLod = 0, s_accQueue = 0, s_accTotal = 0;
        static int s_fc = 0;
        auto tEnd = std::chrono::high_resolution_clock::now();
        const double procMs = std::chrono::duration<double, std::milli>(tSetEnd - t0).count();
        const double setMs = std::chrono::duration<double, std::milli>(tLodEnd - tSetEnd).count();
        const double lodMs = std::chrono::duration<double, std::milli>(tQueueEnd - tLodEnd).count();
        const double queueMs = std::chrono::duration<double, std::milli>(tEnd - tQueueEnd).count();
        const double totalMs = std::chrono::duration<double, std::milli>(tEnd - t0).count();
        s_accProc += procMs; s_accSet += setMs; s_accLod += lodMs; s_accQueue += queueMs; s_accTotal += totalMs;
        if (++s_fc >= 240) {
            // 暂时注释：World::Update 耗时打印
            // printf("[World::Update] avg240: proc=%.4fms set=%.4fms lod=%.4fms queue=%.4fms total=%.4fms | last=%.4fms chunks=%zu\n",
            //     s_accProc / 240.0, s_accSet / 240.0, s_accLod / 240.0, s_accQueue / 240.0, s_accTotal / 240.0, totalMs,
            //     chunks.size());
            s_accProc = s_accSet = s_accLod = s_accQueue = s_accTotal = 0.0;
            s_fc = 0;
        }
    }
}

// World.cpp
void World::StartWorkerThreads(int numThreads) {
    stopWorkers = false;
    for (int i = 0; i < numThreads; ++i) {
        workerThreads.emplace_back(&World::WorkerThread, this);
    }
}

void World::StopWorkerThreads() {
    {
        std::lock_guard<std::mutex> lock(taskMutex);
        stopWorkers = true;
    }
    taskCondition.notify_all();

    for (auto& thread : workerThreads) {
        if (thread.joinable()) thread.join();
    }
    workerThreads.clear();
}

void World::WorkerThread() {
    while (true) {
        std::pair<int, int> task;

        // ������Χ��С
        {
            std::unique_lock<std::mutex> lock(taskMutex);
            taskCondition.wait(lock, [this] {
                return stopWorkers || !meshTaskQueue.empty();
                });

            if (stopWorkers) return;

            task = meshTaskQueue.front();
            meshTaskQueue.pop();
        }

        // 加共享锁读 chunks:主线程(World::Update)会增删 chunk 触发 rehash,
        // 无锁读会导致 worker 持有的迭代器/引用悬垂,访问已释放节点 → 堆损坏 → 任意时刻崩溃。
        Chunk chunkCopy;
        bool needRebuild = false;
        {
            std::shared_lock<std::shared_mutex> lock(chunksMutex);
            auto it = chunks.find(task);
            if (it != chunks.end() && it->second.needsMeshUpdate) {
                // 使用局部副本避免与主线程写回竞争
                chunkCopy = it->second;
                needRebuild = true;
            }
        }

        if (needRebuild) {
            GenerateMesh(&chunkCopy);

            // 加锁写回，避免与渲染线程的 CollectVisibleFaces/GetBlockAt 竞争（不改生成规则）
            {
                std::unique_lock<std::shared_mutex> lock(chunksMutex);
                if (chunks.find(task) != chunks.end()) {
                    chunks[task] = chunkCopy;
                }
            }
        }
    }

}