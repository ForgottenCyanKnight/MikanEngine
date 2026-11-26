#include "World/World.h"
#include "Core/CpuStageTrace.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string_view>

namespace {

enum class MeshMode { Lod, ExactGreedy, TieredGreedy };

MeshMode GetMeshMode() {
    static const auto mode = [] {
        const char* setting = std::getenv("MIKAN_VOXEL_GREEDY_ONLY");
        const std::string_view value = setting ? setting : "";
        if (value == "1") return MeshMode::ExactGreedy;
        if (value == "tiered") return MeshMode::TieredGreedy;
        return MeshMode::Lod;
    }();
    return mode;
}

int LodForChunk(const Chunk& chunk, const glm::vec3& cameraPos) {
    const glm::vec3 center(chunk.chunkX + Chunk::SIZE / 2.0f, Chunk::HEIGHT / 2.0f,
                           chunk.chunkZ + Chunk::SIZE / 2.0f);
    const float distance = glm::distance(center, cameraPos);
    constexpr int distanceInChunks[] = {0, 16, 32, 48, 72};
    if (GetMeshMode() == MeshMode::ExactGreedy) {
        // Keep the near/far material and plant rules at full mesh resolution.
        return distance < distanceInChunks[1] * Chunk::SIZE ? 1 : 2;
    }
    for (int lod = 1; lod < std::size(distanceInChunks); ++lod) {
        if (distance < distanceInChunks[lod] * Chunk::SIZE) return lod;
    }
    return 5;
}

int RequestedLod(const Chunk& chunk, const glm::vec3& cameraPos) {
    return chunk.requestedLodLevel >= 0 ? chunk.requestedLodLevel : LodForChunk(chunk, cameraPos);
}

int StepForLod(int lod) {
    if (GetMeshMode() == MeshMode::TieredGreedy) return 1;
    if (lod >= 4) return 4;
    return lod == 3 ? 2 : 1;
}

void ExtendRasterMeshBounds(AABB& bounds, const Chunk::FaceInstance& face) {
    const glm::vec3 center(face.posX, face.posY, face.posZ);
    const float halfWidth = 0.5f * (1 + ((face.packedData >> 24) & 0xf));
    const float halfHeight = 0.5f * (1 + ((face.packedData >> 28) & 0xf));
    const uint32_t blockType = (face.packedData >> 8) & 0xff;
    const uint32_t direction = face.packedData & 0xff;

    // Match the centered unit quad and world.vert's face rotations. For crossed
    // vegetation both horizontal extents are bounded by half the face width.
    glm::vec3 extent(halfWidth, halfHeight, 0.0f);
    if (blockType == 13 || blockType == 14) {
        extent.z = halfWidth;
    } else if (direction == 2 || direction == 3) {
        extent = glm::vec3(0.0f, halfHeight, halfWidth);
    } else if (direction == 4 || direction == 5) {
        extent = glm::vec3(halfWidth, 0.0f, halfHeight);
    }
    bounds.Expand(center - extent);
    bounds.Expand(center + extent);
}
}

// 体素世界全局配置（WorldSystem 在创建世界时设置）
WorldConfig& GetWorldConfig() {
    static WorldConfig config;
    return config;
}

World::World(int radius)
    : id([] {
        static std::atomic<uint64_t> next{1};
        return next.fetch_add(1, std::memory_order_relaxed);
    }()), renderRadius(radius) {
    CalculateMaxUpdatesPerFrame();
}

std::optional<World::MeshSnapshot> World::CaptureChunkMeshes(uint64_t since) const {
    Core::CpuStageTrace timing("world.rt_mesh_snapshot");
    std::shared_lock lock(chunksMutex);
    if (since == meshRevision) return std::nullopt;
    MeshSnapshot snapshot{meshRevision, {}};
    snapshot.chunks.reserve(chunks.size());
    for (const auto& [coordinate, chunk] : chunks) {
        if (!chunk.publishedMeshRevision) continue;
        ChunkMesh mesh{coordinate, chunk.publishedMeshRevision, !chunk.alphaFaceInstances.empty(), {}};
        if (mesh.revision > since) {
            mesh.faces = chunk.opaqueFaceInstances;
            mesh.faces.insert(mesh.faces.end(), chunk.alphaFaceInstances.begin(), chunk.alphaFaceInstances.end());
            mesh.faces.insert(mesh.faces.end(), chunk.transparentFaceInstances.begin(), chunk.transparentFaceInstances.end());
        }
        snapshot.chunks.push_back(std::move(mesh));
    }
    return snapshot;
}

std::optional<World::RasterMeshUpdates> World::CaptureRasterMeshUpdates(uint64_t since) const {
    Core::CpuStageTrace timing("world.raster_mesh_updates");
    RasterMeshUpdates updates{};
    {
        Core::CpuStageTrace lockTiming("world.raster_snapshot_lock");
        std::optional<Core::CpuStageTrace> copyTiming;
        std::shared_lock lock(chunksMutex);
        lockTiming.Stop();
        if (since == meshRevision) return std::nullopt;
        copyTiming.emplace("world.raster_snapshot");

        updates.revision = meshRevision;
        updates.full = since == 0 || since < discardedRasterRevision || since > meshRevision;
        const auto appendMesh = [&updates](const std::pair<int, int>& coordinate, const Chunk& chunk) {
            updates.chunks.push_back({coordinate, chunk.publishedMeshRevision, {},
                {chunk.opaqueFaceInstances, chunk.alphaFaceInstances, chunk.transparentFaceInstances}});
        };

        if (updates.full) {
            updates.chunks.reserve(chunks.size());
            for (const auto& [coordinate, chunk] : chunks) {
                if (chunk.publishedMeshRevision != 0) appendMesh(coordinate, chunk);
            }
        } else {
            const auto first = std::upper_bound(rasterMeshChanges.begin(), rasterMeshChanges.end(), since,
                [](uint64_t revision, const RasterMeshChange& change) { return revision < change.revision; });
            std::unordered_set<std::pair<int, int>, ChunkCoordinateHash> seen;
            seen.reserve(static_cast<size_t>(rasterMeshChanges.end() - first));
            for (auto change = first; change != rasterMeshChanges.end(); ++change) {
                if (!seen.insert(change->coordinate).second) continue;
                const auto entry = chunks.find(change->coordinate);
                if (entry != chunks.end() && entry->second.publishedMeshRevision != 0) {
                    appendMesh(entry->first, entry->second);
                } else {
                    updates.chunks.push_back({change->coordinate, 0, {}, {}});
                }
            }
        }
        copyTiming->Stop();
    }

    {
        Core::CpuStageTrace boundsTiming("world.raster_bounds");
        for (auto& chunk : updates.chunks) {
            for (const auto& faces : chunk.faces) {
                for (const auto& face : faces) ExtendRasterMeshBounds(chunk.bounds, face);
            }
        }
    }
    return updates;
}

void World::RecordRasterMeshChange(const std::pair<int, int>& coordinate) {
    constexpr size_t maxChanges = 8192;
    if (rasterMeshChanges.size() == maxChanges) {
        discardedRasterRevision = rasterMeshChanges.front().revision;
        rasterMeshChanges.pop_front();
    }
    rasterMeshChanges.push_back({meshRevision, coordinate});
}

void World::CalculateMaxUpdatesPerFrame() {
    JobSystem& scheduler = JobSystem::GetInstance();
    scheduler.Start();
    const unsigned int coreCount = static_cast<unsigned int>(
        std::max<std::size_t>(scheduler.WorkerCount(), 1));
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
    WaitForMeshTasks();
}

void World::WaitForAsyncWork() {
    WaitForMeshTasks();
}

World::HitResult World::RayCast(const glm::vec3& start, const glm::vec3& direction, float maxDistance) {
    HitResult result;
    if (glm::dot(direction, direction) == 0.0f) return result;
    std::shared_lock lock(chunksMutex);
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
    if (modificationQueue.empty()) return;
    std::unique_lock chunkLock(chunksMutex);
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
                if (type < 0 || type > 255 || it->second.blocks[idx] == type) continue;
                it->second.blocks[idx] = static_cast<uint8_t>(type);

                // An edit anywhere in an edge LOD cell can change its coverage.
                const int step = StepForLod(RequestedLod(it->second, currentCameraPos));
                if (lx < step) MarkNeighborEdgeForUpdate(cx, cz, 1);
                if (lx >= Chunk::SIZE - step) MarkNeighborEdgeForUpdate(cx, cz, 0);
                if (lz < step) MarkNeighborEdgeForUpdate(cx, cz, 3);
                if (lz >= Chunk::SIZE - step) MarkNeighborEdgeForUpdate(cx, cz, 2);

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


}

void World::EnqueueChunkUpdate(const std::pair<int, int>& coord) {
    auto it = chunks.find(coord);
    if (it == chunks.end()) return;
    it->second.meshRequest = ++meshRequest;
    it->second.needsMeshUpdate = true;
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

static void SplitPackedFaceRects(const std::vector<Rect>& rects, int blockScale, std::vector<Rect>& output) {
    // FaceInstance stores extents of up to 16 world cells per axis.
    const int maxExtent = 16 / blockScale;
    output.clear();
    for (const auto& rect : rects) {
        for (int v = 0; v < rect.height; v += maxExtent) {
            for (int u = 0; u < rect.width; u += maxExtent) {
                output.push_back({rect.x + u, rect.y + v,
                    std::min(maxExtent, rect.width - u), std::min(maxExtent, rect.height - v)});
            }
        }
    }
}

namespace {
constexpr uint8_t kWaterBlock = BlockManager::WATER;

int LodYOffset(int step) {
    if (step == 1) return 0;
    if (GetWorldConfig().terrainType != FLAT) return -1;

    // Align the coarse cell containing the flat terrain's grass layer with
    // the top of that layer in the detailed mesh.
    const int remainder = Chunk::FLAT_SURFACE_Y % step;
    return remainder == 0 ? 0 : remainder - step;
}

bool IsPlantBlock(uint8_t type) {
    return type == 13 || type == 14;
}

bool ExposesWaterFace(uint8_t adjacent) {
    return adjacent == BlockManager::AIR || IsPlantBlock(adjacent);
}

uint8_t SampleLodCell(const Chunk& chunk, const glm::ivec3& origin, int step) {
    // Preserve the terrain mesher's material priority and minimum occupancy.
    std::array<uint8_t, 256> counts{};
    uint8_t dominant = 0;
    uint8_t mostCommon = 0;
    int occupied = 0;
    bool hasSand = false;
    int topWaterCells = 0;
    const int topY = origin.y + step - 1;
    for (int x = origin.x; x < origin.x + step; ++x) {
        for (int y = origin.y; y < origin.y + step; ++y) {
            for (int z = origin.z; z < origin.z + step; ++z) {
                const uint8_t type = chunk.blocks[Chunk::index(x, y, z)];
                if (type == 0 || IsPlantBlock(type)) continue;
                if (type == 3) return 3;
                ++occupied;
                topWaterCells += type == kWaterBlock && y == topY;
                hasSand |= type == 8;
                if (const uint8_t count = ++counts[type]; count > mostCommon) {
                    dominant = type;
                    mostCommon = count;
                }
            }
        }
    }
    // A fully flooded top layer still represents water over a shallow bed.
    if (topWaterCells == step * step) return kWaterBlock;
    if (hasSand) return 8;
    return occupied >= 2 ? dominant : 0;
}

size_t LodCellIndex(const glm::ivec3& cell, int step) {
    const int width = Chunk::SIZE / step;
    return ((cell.x / step) * (Chunk::HEIGHT / step) + cell.y / step) * width + cell.z / step;
}

constexpr std::array<glm::ivec3, 6> kFaceDirections{{
    {0, 0, 1}, {0, 0, -1}, {-1, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, -1, 0}
}};

glm::ivec3 SideCell(int face, int u, int y, int step) {
    switch (face) {
    case 0: return {u, y, Chunk::SIZE - step};
    case 1: return {u, y, 0};
    case 2: return {0, y, u};
    default: return {Chunk::SIZE - step, y, u};
    }
}

glm::vec3 FaceCenter(const Chunk& chunk, int face, int fixed, const Rect& rect, int step) {
    const float u = (rect.x + rect.width * 0.5f) * step;
    const float v = (rect.y + rect.height * 0.5f) * step;
    if (face < 2) return {chunk.chunkX + u, v, chunk.chunkZ + fixed};
    if (face < 4) return {chunk.chunkX + fixed, v, chunk.chunkZ + u};
    return {chunk.chunkX + u, fixed, chunk.chunkZ + v};
}

int BoundaryCoverage(uint8_t type) {
    if (type == 0 || IsPlantBlock(type)) return 0;
    if (type == kWaterBlock) return 1;
    if (type == 5 || type == 6) return 2;
    return 3;
}

bool KeepSideFace(uint8_t own, uint8_t adjacent, int step, int otherStep) {
    if (own == kWaterBlock) return ExposesWaterFace(adjacent);
    const int coverage = BoundaryCoverage(own);
    if (coverage == 0) return false;
    if (step != otherStep) return coverage > BoundaryCoverage(adjacent);
    if (step == 1) return own != adjacent && BoundaryCoverage(adjacent) < 3;
    return adjacent == BlockManager::AIR || adjacent == kWaterBlock;
}

void AppendMergedFaces(Chunk& chunk, uint8_t face, uint8_t type, uint8_t texture,
                       int fixed, int step, int yOffset, const std::vector<glm::ivec2>& cells,
                       std::vector<Rect>& split) {
    SplitPackedFaceRects(UnifiedGreedyMesh(cells, 1.0f), step, split);
    auto& instances = type == kWaterBlock ? chunk.transparentFaceInstances
        : (StepForLod(chunk.lodLevel) == 1 && (type == 6 || (type == 5 && chunk.lodLevel <= 1)))
            ? chunk.alphaFaceInstances : chunk.opaqueFaceInstances;
    for (const Rect& rect : split) {
        glm::vec3 center = FaceCenter(chunk, face, fixed, rect, step);
        center.y += yOffset;
        instances.emplace_back(center, face, type, texture,
                               glm::vec2(rect.width * step, rect.height * step));
    }
}

void AppendChunkSides(Chunk& chunk, int step, std::span<const uint8_t> lodCells,
                      const ChunkMap& chunks, const glm::vec3& cameraPos) {
    const int chunkX = chunk.chunkX / Chunk::SIZE;
    const int chunkZ = chunk.chunkZ / Chunk::SIZE;
    const int yOffset = LodYOffset(step);
    const bool alignWater = GetWorldConfig().terrainType == ADVANCED;
    const int waterYOffset = alignWater ? 0 : yOffset;
    std::array<uint8_t, Chunk::SIZE * Chunk::HEIGHT> neighborTypes;
    std::unordered_map<uint8_t, std::vector<glm::ivec2>> facesByType;
    std::vector<Rect> split;
    for (int face = 0; face < 4; ++face) {
        const auto direction = kFaceDirections[face];
        const auto neighbor = chunks.find({chunkX + direction.x, chunkZ + direction.z});
        if (neighbor == chunks.end()) continue;
        const Chunk& other = neighbor->second;
        const int otherStep = StepForLod(RequestedLod(other, cameraPos));
        const int edgeWidth = Chunk::SIZE / otherStep;
        const int edgeStep = std::min(step, otherStep);
        const int otherYOffset = LodYOffset(otherStep);
        const bool otherWaterUnshifted = alignWater && otherYOffset != 0;

        // Sample each neighbor cell once at its own LOD, then compare coverage on
        // the finer grid along the shared plane.
        for (int y = 0; y < Chunk::HEIGHT; y += otherStep) {
            for (int u = 0; u < Chunk::SIZE; u += otherStep) {
                const glm::ivec3 cell = SideCell(face ^ 1, u, y, otherStep);
                neighborTypes[(y / otherStep) * edgeWidth + u / otherStep] = otherStep == 1
                    ? other.blocks[Chunk::index(cell.x, cell.y, cell.z)] : SampleLodCell(other, cell, otherStep);
            }
        }

        const auto neighborAt = [&](int u, int y) -> uint8_t {
            if (y < 0 || y >= Chunk::HEIGHT) return BlockManager::AIR;
            return neighborTypes[(y / otherStep) * edgeWidth + u / otherStep];
        };
        facesByType.clear();
        for (int y = step == 1 ? 1 : 0; y < Chunk::HEIGHT; y += edgeStep) {
            for (int u = 0; u < Chunk::SIZE; u += edgeStep) {
                const glm::ivec3 cell = SideCell(face, u, y, step);
                const uint8_t own = step == 1 ? chunk.blocks[Chunk::index(cell.x, cell.y, cell.z)]
                                              : lodCells[LodCellIndex(cell, step)];
                if (own == BlockManager::AIR || IsPlantBlock(own)) continue;

                // Water keeps its world height across LODs. Terrain still uses
                // the existing downshift, so check both neighbor grids at seams.
                const int worldY = y + (own == kWaterBlock ? waterYOffset : yOffset);
                uint8_t adjacent = neighborAt(u, worldY - otherYOffset);
                if (otherWaterUnshifted && (adjacent == kWaterBlock || ExposesWaterFace(adjacent))) {
                    adjacent = neighborAt(u, worldY) == kWaterBlock ? kWaterBlock : BlockManager::AIR;
                }
                if (KeepSideFace(own, adjacent, step, otherStep)) {
                    facesByType[own].emplace_back(u / edgeStep, y / edgeStep);
                }
            }
        }
        const int fixed = (face == 0 || face == 3) ? Chunk::SIZE : 0;
        for (const auto& [type, positions] : facesByType) {
            AppendMergedFaces(chunk, static_cast<uint8_t>(face), type, GetFaceTextureIndex(type, face),
                              fixed, edgeStep, type == kWaterBlock ? waterYOffset : yOffset, positions, split);
        }
    }
}
}

void World::GenerateLODMesh(Chunk* chunk, int step) {
    chunk->opaqueFaceInstances.clear();
    chunk->alphaFaceInstances.clear();
    chunk->transparentFaceInstances.clear();
    const auto start = std::chrono::high_resolution_clock::now();
    const int width = Chunk::SIZE / step;
    const int height = Chunk::HEIGHT / step;
    std::vector<uint8_t> cells(width * width * height);
    for (int x = 0; x < Chunk::SIZE; x += step) {
        for (int y = 0; y < Chunk::HEIGHT; y += step) {
            for (int z = 0; z < Chunk::SIZE; z += step) {
                const glm::ivec3 cell(x, y, z);
                cells[LodCellIndex(cell, step)] = SampleLodCell(*chunk, cell, step);
            }
        }
    }

    thread_local std::unordered_map<FaceGroupKey, std::vector<glm::ivec2>> faceGroups;
    faceGroups.clear();
    for (int x = 0; x < Chunk::SIZE; x += step) {
        for (int y = 0; y < Chunk::HEIGHT; y += step) {
            for (int z = 0; z < Chunk::SIZE; z += step) {
                const glm::ivec3 cell(x, y, z);
                const uint8_t type = cells[LodCellIndex(cell, step)];
                if (type == 0) continue;

                for (int face = 0; face < 6; ++face) {
                    const glm::ivec3 adjacent = cell + kFaceDirections[face] * step;
                    if (adjacent.x < 0 || adjacent.x >= Chunk::SIZE ||
                        adjacent.z < 0 || adjacent.z >= Chunk::SIZE) continue;
                    const uint8_t neighbor = (adjacent.y < 0 || adjacent.y >= Chunk::HEIGHT)
                        ? 0 : cells[LodCellIndex(adjacent, step)];
                    const bool visible = type == kWaterBlock ? ExposesWaterFace(neighbor)
                        : neighbor == BlockManager::AIR || neighbor == kWaterBlock;
                    if (!visible) continue;

                    const int fixed = face < 2 ? z + (face == 0 ? step : 0)
                                    : face < 4 ? x + (face == 3 ? step : 0)
                                               : y + (face == 4 ? step : 0);
                    const glm::ivec2 plane = face < 2 ? glm::ivec2(x, y)
                                           : face < 4 ? glm::ivec2(z, y) : glm::ivec2(x, z);
                    const FaceGroupKey key(face, type, GetFaceTextureIndex(type, face), fixed);
                    faceGroups[key].push_back(plane / step);
                }
            }
        }
    }

    std::vector<Rect> split;
    const int yOffset = LodYOffset(step);
    const int waterYOffset = GetWorldConfig().terrainType == ADVANCED ? 0 : yOffset;
    for (const auto& [key, positions] : faceGroups) {
        const auto face = static_cast<uint8_t>((key.key >> 48) & 0xff);
        const auto type = static_cast<uint8_t>((key.key >> 40) & 0xff);
        const auto texture = static_cast<uint8_t>((key.key >> 32) & 0xff);
        const int fixed = static_cast<int>(key.key & 0xffffffff);
        AppendMergedFaces(*chunk, face, type, texture, fixed, step,
                          type == kWaterBlock ? waterYOffset : yOffset, positions, split);
    }
    AppendChunkSides(*chunk, step, cells, chunks, currentCameraPos);

    const auto end = std::chrono::high_resolution_clock::now();
    std::lock_guard lock(timeMutex);
    totalMeshTime += std::chrono::duration<double, std::milli>(end - start).count();
}

void World::GenerateMesh(Chunk* chunk) {
    chunk->opaqueFaceInstances.clear();
    chunk->transparentFaceInstances.clear();
    chunk->alphaFaceInstances.clear();
    auto start = std::chrono::high_resolution_clock::now();
    thread_local std::vector<Rect> rects;
    rects.clear();

    thread_local std::unordered_map<FaceGroupKey, std::vector<glm::ivec2>> faceGroups;
    faceGroups.clear();
    faceGroups.reserve(256);

    BlockManager& blockManager = BlockManager::GetInstance();
    static const BlockType* blockTypes = blockManager.GetBlockTypesArray();

    std::vector<glm::ivec3> plantPositions;

    const auto& dirs = kFaceDirections;
    const auto faceVisible = [this](uint8_t type, uint8_t adjacent) {
        if (type == kWaterBlock) return ExposesWaterFace(adjacent);
        return adjacent != type && (adjacent == BlockManager::AIR || IsTransparent(adjacent));
    };
    const int lodLevel = RequestedLod(*chunk, currentCameraPos);
    const MeshMode meshMode = GetMeshMode();
    chunk->lodLevel = lodLevel;
    const glm::vec3 chunkCenter(chunk->chunkX + Chunk::SIZE / 2.0f, Chunk::HEIGHT / 2.0f,
                                chunk->chunkZ + Chunk::SIZE / 2.0f);
    const float distance = glm::distance(chunkCenter, currentCameraPos);
    const int step = StepForLod(lodLevel);
    if (step > 1) {
        GenerateLODMesh(chunk, step);
        return;
    }

    for (int x = 0; x < Chunk::SIZE; x++) {
        for (int z = 0; z < Chunk::SIZE; z++) {
            for (int y = Chunk::HEIGHT - 1; y >= 0; y--) {
                uint8_t type = chunk->blocks[Chunk::index(x, y, z)];
                if (type == 0 || y == 0) continue;
                if (type == 13 || type == 14) {
                    if (lodLevel <= 1) {  // lodLevel从1开始，所以<=2表示最详细和中等级别
                        plantPositions.emplace_back(x, y, z);
                    }
                    continue;
                }
                bool surrounded = true;
                uint8_t neighborType = 0;
                for (int i = 0; i < 6; ++i) {
                    const auto& dir = dirs[i];
                    int nx = x + dir.x;
                    int ny = y + dir.y;
                    int nz = z + dir.z;
                    if (nx < 0 || nx >= Chunk::SIZE || nz < 0 || nz >= Chunk::SIZE) continue;
                    neighborType = (ny < 0 || ny >= Chunk::HEIGHT)
                        ? 0 : chunk->blocks[Chunk::index(nx, ny, nz)];

                    if (faceVisible(type, neighborType)) {
                        surrounded = false;
                        break;
                    }
                }


                if (surrounded) continue;

                for (const auto& dir : dirs) {
                    int nx = x + dir.x;
                    int ny = y + dir.y;
                    int nz = z + dir.z;

                    if (nx < 0 || nx >= Chunk::SIZE || nz < 0 || nz >= Chunk::SIZE) continue;
                    neighborType = (ny < 0 || ny >= Chunk::HEIGHT)
                        ? 0 : chunk->blocks[Chunk::index(nx, ny, nz)];
                    if (faceVisible(type, neighborType)) {
                        uint8_t faceIndex;
                        uint8_t texIndex;
                        int fixedCoord;
                        glm::ivec2 pos;
                        const BlockType& blockType = blockTypes[type];
                        if (dir.x != 0) {
                            bool positive = (dir.x > 0);
                            faceIndex = positive ? static_cast<uint8_t>(3) : static_cast<uint8_t>(2);
                            texIndex = GetFaceTextureIndex(type, faceIndex);
                            fixedCoord = positive ? (x + 1) : x;
                            pos = glm::ivec2(z, y);
                        }
                        else if (dir.y != 0) {
                            bool positive = (dir.y > 0);
                            faceIndex = positive ? static_cast<uint8_t>(4) : static_cast<uint8_t>(5);
                            texIndex = GetFaceTextureIndex(type, faceIndex);
                            fixedCoord = positive ? (y + 1) : y;
                            pos = glm::ivec2(x, z);
                        }
                        else {
                            bool positive = (dir.z > 0);
                            faceIndex = positive ? static_cast<uint8_t>(0) : static_cast<uint8_t>(1);
                            texIndex = GetFaceTextureIndex(type, faceIndex);
                            fixedCoord = positive ? (z + 1) : z;
                            pos = glm::ivec2(x, y);
                        }
                        FaceGroupKey key(faceIndex, type, texIndex, fixedCoord);
                        faceGroups[key].push_back(pos);
                    }
                };
            }
        }

    }

    std::vector<Rect> splitRects;
    for (auto& [key, positions] : faceGroups) {
        rects.clear();
        const uint8_t blockType = static_cast<uint8_t>((key.key >> 40) & 0xFF);
        if (blockType == kWaterBlock) {
            rects = UnifiedGreedyMesh(positions, 1.0f);
        } else if (meshMode == MeshMode::TieredGreedy) {
            const float threshold = lodLevel <= 1 ? 1.0f : (lodLevel == 2 ? 0.5f : 0.1f);
            rects = UnifiedGreedyMesh(positions, threshold);
        } else if (lodLevel > 0) {
            rects = UnifiedGreedyMesh(positions, 1.0f);
            if (meshMode == MeshMode::Lod && lodLevel > 1) rects = UnifiedGreedyMesh(positions, 0.5f);
        }
        else {
            rects.reserve(positions.size());
            for (const auto& pos : positions) {
                rects.push_back({ pos.x, pos.y, 1, 1 });
            }
        }
        uint8_t faceData = static_cast<uint8_t>((key.key >> 48) & 0xFF);
        uint8_t uvIndex = static_cast<uint8_t>((key.key >> 32) & 0xFF);
        int fixedCoord = static_cast<int>(key.key & 0xFFFFFFFF);

        SplitPackedFaceRects(rects, 1, splitRects);
        for (const auto& rect : splitRects) {
            glm::vec3 worldPos;
            glm::vec2 size(rect.width, rect.height);

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
            bool isTransparent = (blockType == kWaterBlock);
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

    AppendChunkSides(*chunk, 1, {}, chunks, currentCameraPos);
    chunk->lastMeshDistance = distance;
    chunk->needsMeshUpdate = false;
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    std::chrono::duration<double, std::milli> ms_double = end - start;
    //std::cout << std::fixed << std::setprecision(3) << ms_double.count() << " ms" << std::endl;
    {
        std::lock_guard<std::mutex> lock(timeMutex);
        totalMeshTime += ms_double.count();
    }
}

void World::Update(const glm::vec3& cameraPos, const std::array<Plane, 6>& frustumPlanes) {
    Core::CpuStageTrace timing("world.update");
    auto start = std::chrono::high_resolution_clock::now();
    auto t0 = start;
    ProcessModifications();

    int camChunkX = static_cast<int>(std::floor(cameraPos.x / Chunk::SIZE));
    int camChunkZ = static_cast<int>(std::floor(cameraPos.z / Chunk::SIZE));
    bool cameraChunkChanged = (camChunkX != cachedCamChunkX) || (camChunkZ != cachedCamChunkZ);
    {
        Core::CpuStageTrace lockTiming("world.stream_lock");
        std::unique_lock lock(chunksMutex);
        lockTiming.Stop();
        currentCameraPos = cameraPos;
        currentFrustumPlanes = frustumPlanes;
        lastFrustumPlanes = frustumPlanes;
        if (!initialGenerationDone || cameraChunkChanged || streamedRadius != renderRadius) {
            Core::CpuStageTrace streamTiming("world.stream_window");
            const int64_t radius = renderRadius;
            const int64_t radiusSquared = radius * radius;
            if (!initialGenerationDone || streamedRadius != renderRadius) {
                Core::CpuStageTrace windowTiming("world.stream_build_window");
                streamOffsets.clear();
                for (int64_t dx = -radius; dx <= radius; ++dx) {
                    for (int64_t dz = -radius; dz <= radius; ++dz) {
                        if (dx * dx + dz * dz <= radiusSquared) {
                            streamOffsets.emplace_back(static_cast<int>(dx), static_cast<int>(dz));
                        }
                    }
                }
                chunks.reserve(streamOffsets.size());
                chunkCache.reserve(MAX_CHUNK_CACHE);
            }

            {
                Core::CpuStageTrace unloadTiming("world.stream_unload");
                auto it = chunks.begin();
                while (it != chunks.end()) {
                    const int64_t dx = static_cast<int64_t>(it->first.first) - camChunkX;
                    const int64_t dz = static_cast<int64_t>(it->first.second) - camChunkZ;
                    if (std::abs(dx) > radius || std::abs(dz) > radius || dx * dx + dz * dz > radiusSquared) {
                        Core::CpuStageTrace chunkTiming("world.stream_unload_chunk");
                        Core::CpuStageTrace cacheTiming("world.stream_unload_cache");
                        const auto outgoing = it++;
                        const auto coord = outgoing->first;
                        if (chunkCache.size() >= MAX_CHUNK_CACHE) {
                            chunkCache.erase(chunkCache.begin());
                        }
                        ++meshRevision;
                        if (outgoing->second.publishedMeshRevision != 0) RecordRasterMeshChange(coord);
                        // The active and cached maps have disjoint keys and identical node types.
                        chunkCache.insert(chunks.extract(outgoing));
                        cacheTiming.Stop();
                        {
                            Core::CpuStageTrace neighborTiming("world.stream_unload_neighbors");
                            MarkNeighborsForUpdate(coord.first, coord.second);
                        }
                    }
                    else {
                        ++it;
                    }
                }
            }

            {
                Core::CpuStageTrace loadTiming("world.stream_load");
                for (const auto& [dx, dz] : streamOffsets) {
                    const auto coord = std::pair{camChunkX + dx, camChunkZ + dz};
                    if (!chunks.count(coord)) {
                        Core::CpuStageTrace chunkTiming("world.stream_load_missing");
                        const auto cacheIt = chunkCache.find(coord);
                        if (cacheIt != chunkCache.end()) {
                            Core::CpuStageTrace restoreTiming("world.restore_cached_chunk");
                            Core::CpuStageTrace insertionTiming("world.stream_activate_chunk");
                            Chunk& chunk = chunks.insert(chunkCache.extract(cacheIt)).position->second;
                            insertionTiming.Stop();
                            if (chunk.publishedMeshRevision != 0) {
                                chunk.publishedMeshRevision = ++meshRevision;
                                RecordRasterMeshChange(coord);
                            }
                        }
                        else {
                            Core::CpuStageTrace insertionTiming("world.stream_activate_chunk");
                            const auto entry = chunks.try_emplace(coord).first;
                            insertionTiming.Stop();
                            Core::CpuStageTrace generationTiming("world.generate_chunk");
                            try {
                                entry->second.Generate(coord.first * Chunk::SIZE, coord.second * Chunk::SIZE);
                            } catch (...) {
                                chunks.erase(entry);
                                throw;
                            }
                        }
                        Core::CpuStageTrace neighborTiming("world.stream_load_neighbors");
                        EnqueueChunkUpdate(coord);
                        MarkNeighborsForUpdate(coord.first, coord.second);
                    }
                }
            }

            cachedCamChunkX = camChunkX;
            cachedCamChunkZ = camChunkZ;
            streamedRadius = renderRadius;
            initialGenerationDone = true;
        }
    }
    auto tSetEnd = std::chrono::high_resolution_clock::now();

    // 检查所有活跃区块的LOD级别变化，如果改变则标记需要更新
    {
        Core::CpuStageTrace lockTiming("world.lod_lock");
        std::optional<Core::CpuStageTrace> scanTiming;
        std::unique_lock lock(chunksMutex);
        lockTiming.Stop();
        scanTiming.emplace("world.lod_scan");
        for (auto& [coord, chunk] : chunks) {
            const int lod = LodForChunk(chunk, currentCameraPos);
            const int previous = chunk.requestedLodLevel;
            if (previous == lod) continue;
            chunk.requestedLodLevel = lod;

            // Changing the sampling stride changes coverage on both sides of a seam.
            if (previous >= 0 && StepForLod(previous) != StepForLod(lod)) {
                MarkNeighborsForUpdate(coord.first, coord.second);
            }
            if (previous >= 0 || !chunk.needsMeshUpdate) EnqueueChunkUpdate(coord);
        }
        scanTiming->Stop();
    }
    auto tLodEnd = std::chrono::high_resolution_clock::now();

    {
        Core::CpuStageTrace queueTiming("world.mesh_queue");
        int updatesThisFrame = 0;
        while (!chunkUpdateQueue.empty() && updatesThisFrame < maxUpdatesPerFrame) {
            auto task = chunkUpdateQueue.top();
            chunkUpdateQueue.pop();
            inQueueChunks.erase(task.coord);

            bool needsMesh = false;
            {
                std::shared_lock lock(chunksMutex);
                const auto it = chunks.find(task.coord);
                needsMesh = it != chunks.end() && it->second.needsMeshUpdate;
            }
            if (needsMesh) {
                SubmitMeshTask(task.coord);
                ++updatesThisFrame;
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
        lastPrintedMeshTime = frameMeshTime;
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

void World::SubmitMeshTask(const std::pair<int, int>& coord) {
    Core::CpuStageTrace timing("world.mesh_submit");
    JobSystem& scheduler = JobSystem::GetInstance();
    scheduler.Start();

    std::lock_guard<std::mutex> lock(meshJobsMutex);
    meshJobs.erase(std::remove_if(meshJobs.begin(), meshJobs.end(), [](const JobSystem::JobHandle& job) {
        return job.valid() && job.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    }), meshJobs.end());
    meshJobs.emplace_back(scheduler.Submit([this, coord] {
        ProcessMeshTask(coord);
    }));
}

void World::WaitForMeshTasks() {
    std::vector<JobSystem::JobHandle> jobs;
    {
        std::lock_guard<std::mutex> lock(meshJobsMutex);
        jobs.swap(meshJobs);
    }
    for (const JobSystem::JobHandle& job : jobs) {
        if (job.valid()) job.wait();
    }
}

void World::ProcessMeshTask(const std::pair<int, int>& task) {
    Chunk mesh;
    {
        // The mesher reads current neighbor blocks under the shared lock.
        Core::CpuStageTrace lockTiming("world.mesh_worker_lock");
        std::optional<Core::CpuStageTrace> workerTiming;
        std::optional<Core::CpuStageTrace> geometryTiming;
        std::shared_lock lock(chunksMutex);
        lockTiming.Stop();
        const auto entry = chunks.find(task);
        if (entry == chunks.end() || !entry->second.needsMeshUpdate) return;
        workerTiming.emplace("world.mesh_worker");
        mesh.chunkX = entry->second.chunkX;
        mesh.chunkZ = entry->second.chunkZ;
        mesh.meshRequest = entry->second.meshRequest;
        mesh.requestedLodLevel = entry->second.requestedLodLevel;
        std::copy_n(entry->second.blocks, std::size(mesh.blocks), mesh.blocks);
        geometryTiming.emplace("world.mesh_geometry");
        GenerateMesh(&mesh);
        geometryTiming->Stop();
        workerTiming->Stop();
    }

    Core::CpuStageTrace lockTiming("world.mesh_publish_lock");
    std::optional<Core::CpuStageTrace> publishTiming;
    std::unique_lock lock(chunksMutex);
    lockTiming.Stop();
    const auto entry = chunks.find(task);
    if (entry == chunks.end() || !entry->second.needsMeshUpdate ||
        entry->second.meshRequest != mesh.meshRequest) return;

    publishTiming.emplace("world.mesh_publish");
    Chunk& chunk = entry->second;
    chunk.opaqueFaceInstances = std::move(mesh.opaqueFaceInstances);
    chunk.alphaFaceInstances = std::move(mesh.alphaFaceInstances);
    chunk.transparentFaceInstances = std::move(mesh.transparentFaceInstances);
    chunk.lastMeshDistance = mesh.lastMeshDistance;
    chunk.lodLevel = mesh.lodLevel;
    chunk.needsMeshUpdate = false;
    chunk.publishedMeshRevision = ++meshRevision;
    RecordRasterMeshChange(task);
    publishTiming->Stop();
}
