#ifndef WORLD_H
#define WORLD_H
#pragma once
#include <glm/glm.hpp>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <array>
#include <set>
#include <mutex>
#include "World/Chunk.h"
#include "AABB.h"
#include <chrono>
#include <thread>
#include <condition_variable>
#include <future>
#include <shared_mutex>
#include <atomic>
#include <tuple>
#include <limits>
#include <cassert>

struct FaceGroupKey {
    // ?????????��?????64��????
    uint64_t key;

    FaceGroupKey(uint8_t faceData, uint8_t blockType, uint8_t uvIndex, int fixedCoord)
        : key((static_cast<uint64_t>(faceData) << 48) |
            (static_cast<uint64_t>(blockType) << 40) |
            (static_cast<uint64_t>(uvIndex) << 32) |
            (static_cast<uint32_t>(fixedCoord)))
    {
        // ???fixedCoord????��??��??
        assert(fixedCoord >= 0 && fixedCoord < (1 << 16));
    }

    bool operator==(const FaceGroupKey& other) const {
        return key == other.key;
    }
};

namespace std {
    template<>
    struct hash<FaceGroupKey> {
        size_t operator()(const FaceGroupKey& k) const {
            // ??????64��????????
            return std::hash<uint64_t>{}(k.key);
        }
    };
}

struct Rect {
    int x, y;      // ?????????????
    int width, height; // ???��??
};


// ??????????????
struct MegaBlockInfo {
    bool shouldRender;
    uint8_t dominantBlock;
};


class World {
public:
    struct HitResult {
        bool hit = false;
        glm::ivec3 position;
        glm::ivec3 normal;
        uint8_t blockType = 0; // ????????????��????????
    };
    AABB aabb;
    bool meshGenerated = false; // ??????????
    World(int radius);
    ~World();
    HitResult RayCast(const glm::vec3& start, const glm::vec3& direction, float maxDistance);
    std::vector<Chunk*> GetActiveChunks();
    void PlaceBlock(const glm::ivec3& position, int blockType);
    void Update(const glm::vec3& cameraPos, const std::array<Plane, 6>& frustumPlanes);
    void CollectPointLights();
    struct PointLight {
        glm::vec3 position;
        glm::vec3 color;
        float intensity;
    };
    std::vector<PointLight> pointLights;
    static const int MAX_LIGHTS = 32;
    const std::vector<PointLight>& GetPointLights() const { return pointLights; }
    bool IsTransparent(int neighborType) {
        return (neighborType == 5 || neighborType == 6 || neighborType == 13 || neighborType == 14 || neighborType == 12);
    }

    // 在内部锁保护下收集可见 chunk 的面数据（渲染线程调用，避免与网格 worker 线程写回竞争）
    // frustumPlanes 为空（全零）时不做视锥剔除。返回收集的总面数。
    // outOpaque: 不透明方块面；outAlpha: 植物/树叶面；outTransparent: 水面
    size_t CollectVisibleFaces(const std::array<Plane, 6>& frustumPlanes,
                               std::vector<Chunk::FaceInstance>& outOpaque,
                               std::vector<Chunk::FaceInstance>& outAlpha,
                               std::vector<Chunk::FaceInstance>& outTransparent);

    // 线程安全查询世界方块类型（锁内 find + 读；供碰撞检测等外部线程调用）
    uint8_t GetBlockAt(int x, int y, int z) const;
private:
    struct ChunkUpdateTask {
        std::pair<int, int> coord;
        float distance;

        ChunkUpdateTask(int x, int z, float d) : coord(x, z), distance(d) {}

        bool operator<(const ChunkUpdateTask& other) const {
            return distance > other.distance;
        }
    };

    void CalculateMaxUpdatesPerFrame();
    // ?????????????
    std::vector<std::thread> workerThreads;
    std::queue<std::pair<int, int>> meshTaskQueue;
    std::mutex taskMutex;
    std::mutex instanceMutex;
    std::condition_variable taskCondition;
    bool stopWorkers = false;
    mutable std::shared_mutex chunksMutex; // 互斥锁（mutable：const 查询方法 GetBlockAt 也要加锁）
    void StartWorkerThreads(int numThreads);
    void StopWorkerThreads();
    void WorkerThread();
    int maxUpdatesPerFrame = 1;
    // 网格生成耗时统计（仅用于日志；C++17 下不用 atomic<double>）
    double totalMeshTime = 0.0;
    std::mutex timeMutex;
    double lastPrintedMeshTime = 0.0;

    bool IsOnChunkEdge(int lx, int lz) const {
        return lx == 0 || lx == Chunk::SIZE - 1 ||
            lz == 0 || lz == Chunk::SIZE - 1;
    }

    // ????Pair???????
    struct PairHash {
        template <typename T, typename U>
        size_t operator()(const std::pair<T, U>& p) const {
            size_t seed = 0;
            seed ^= std::hash<T>{}(p.first) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
            seed ^= std::hash<U>{}(p.second) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
            return seed;
        }
    };

    struct NeighborCache {
        Chunk* east = nullptr;
        Chunk* west = nullptr;
        Chunk* south = nullptr;
        Chunk* north = nullptr;
    };

    std::unordered_map<std::pair<int, int>, NeighborCache> neighborCacheMap;
    void UpdateNeighborCache(int cx, int cz);
    // ????????????
    struct InstanceBuffers {
        // ????????
        uint32_t opaqueSSBO = 0;
        size_t opaqueCapacity = 0;
        size_t opaqueCount = 0;          // ????��?????????????
        Chunk::FaceInstance* mappedOpaque = nullptr;

        // ??????
        uint32_t transparentSSBO = 0;
        size_t transparentCapacity = 0;
        size_t transparentCount = 0;      // ????��????????????
        Chunk::FaceInstance* mappedTransparent = nullptr;
    };

    InstanceBuffers instanceBuffers;

    std::vector<std::pair<int, int>> dirtyChunks; // ???????��?

    // ?????????��???
    struct InstanceRange {
        size_t opaqueStart;
        size_t opaqueCount;
        size_t transparentStart;
        size_t transparentCount;
    };

    std::unordered_map<std::pair<int, int>, InstanceRange, PairHash> chunkInstanceRanges;

    // ?????/?????????????
    void ResizeInstanceBuffer(size_t requiredOpaque, size_t requiredTransparent);
    void UpdateChunkInstances(const std::pair<int, int>& coord, Chunk& chunk);
public:

    void AddPointLight(const PointLight& light) {
        if (pointLights.size() >= MAX_LIGHTS) {
            pointLights.erase(pointLights.begin());
        }
        pointLights.push_back(light);
    }
    void UpdatePhysics(float deltaTime);
    void CleanupPhysics(); // ???????????
    std::vector<PointLight> manualPointLights; // ?��??????????
    void GenerateLODMesh(Chunk* chunk, const int LOD_SIZE);
    void EnqueueChunkUpdate(const std::pair<int, int>& coord);
    void MarkNeighborsForUpdate(int cx, int cz);
    // �Ż���ֻ����ض�������ھӱ���Ҫ����
    // direction: 0=��, 1=��, 2=��, 3=��
    void MarkNeighborEdgeForUpdate(int cx, int cz, int direction);
    void GenerateMesh(Chunk* chunk);
    void ProcessModifications();
    std::unordered_map<std::pair<int, int>, Chunk> chunks;
    std::unordered_set<std::pair<int, int>> inQueueChunks;
    std::priority_queue<ChunkUpdateTask> chunkUpdateQueue;
    std::mutex modificationMutex;
    std::queue<std::tuple<int, int, int, int>> modificationQueue;
    std::array<Plane, 6> currentFrustumPlanes; // ????
    int renderRadius;
    bool isUpdating = false;
    glm::vec3 currentCameraPos;
    std::set<std::pair<int, int>> lastNeededChunks;
    int cachedCamChunkX = 0;
    int cachedCamChunkZ = 0;
    std::array<Plane, 6> lastFrustumPlanes;
    bool initialGenerationDone = false; // ?????????????????????
    std::unordered_map<std::pair<int, int>, Chunk> chunkCache; // ??????��??????�v??
    const size_t MAX_CHUNK_CACHE = 1024; // ????????
};

#endif // WORLD_H