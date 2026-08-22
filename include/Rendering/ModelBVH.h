#pragma once
#include "Platform/Export.h"

#include "AABB.h"
#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <memory>
#include <limits>
#include <algorithm>
#include <unordered_map>
#include <fstream>
#include <filesystem>
#include <iostream>

#ifdef __ANDROID__
#include <SDL3/SDL.h>
#endif

struct MIKAN_API BVHVertex {
    alignas(16) glm::vec3 position;
    alignas(16) glm::vec3 normal;
    alignas(8) glm::vec2 texCoords;
    alignas(4) char padding[8];

    BVHVertex() : position(0.0f), normal(0.0f, 1.0f, 0.0f), texCoords(0.0f) {
        memset(padding, 0, sizeof(padding));
    }

    BVHVertex(const glm::vec3& pos, const glm::vec3& norm, const glm::vec2& uv)
        : position(pos), normal(norm), texCoords(uv) {
        memset(padding, 0, sizeof(padding));
    }

    bool operator==(const BVHVertex& other) const {
        const float EPSILON = 0.001f;
        bool posEqual = glm::distance(position, other.position) < EPSILON;
        bool texEqual = glm::all(glm::lessThan(glm::abs(texCoords - other.texCoords), glm::vec2(EPSILON)));
        return posEqual && texEqual;
    }
};

struct MIKAN_API BVHVertexHash {
    size_t operator()(const BVHVertex& v) const {
        size_t h1 = std::hash<float>{}(v.position.x);
        size_t h2 = std::hash<float>{}(v.position.y);
        size_t h3 = std::hash<float>{}(v.position.z);
        return h1 ^ (h2 << 1) ^ (h3 << 2);
    }
};

struct MIKAN_API Triangle {
    alignas(4) uint32_t v0_index;
    alignas(4) uint32_t v1_index;
    alignas(4) uint32_t v2_index;
    alignas(4) uint32_t material;

    Triangle() : v0_index(0), v1_index(0), v2_index(0), material(1) {}

    Triangle(uint32_t idx0, uint32_t idx1, uint32_t idx2, uint32_t mat = 1)
        : v0_index(idx0), v1_index(idx1), v2_index(idx2), material(mat) {}
};

// 2026-08-09 BVH disk cache versioning: stale caches (pre-vertex-format / pre-merge changes) have stale AABBs -> frustum over-cull
static constexpr uint32_t kBVHCacheMagic = 0x4D4B4256u;   // "MKBV"
static constexpr uint32_t kBVHCacheVersion = 2u;          // v2 = full-leaf TLAS (MergeNearbyBLAS disabled)

struct MIKAN_API ModelBVHNode {
    alignas(16) glm::vec3 boundsMin;
    alignas(16) glm::vec3 boundsMax;
    alignas(4) int isLeaf;
    alignas(4) int left;
    alignas(4) int right;
    alignas(4) int startIndex;
    alignas(4) int count;
    alignas(4) int depth;
    alignas(4) char padding[8];

    ModelBVHNode() : boundsMin(0.0f), boundsMax(0.0f),
        isLeaf(false), left(-1), right(-1), startIndex(-1), count(0), depth(0) {
        memset(padding, 0, sizeof(padding));
    }

    ModelBVHNode(const AABB& aabb, bool leaf, int l, int r, int startIdx, int cnt, int d)
        : boundsMin(aabb.min), boundsMax(aabb.max),
        isLeaf(leaf ? 1 : 0), left(l), right(r), startIndex(startIdx), count(cnt), depth(d) {
        memset(padding, 0, sizeof(padding));
    }
};

struct MIKAN_API SAHBucket {
    int count = 0;
    AABB bounds;
    SAHBucket() : bounds(AABB(glm::vec3(FLT_MAX), glm::vec3(-FLT_MAX))) {}
};

class MIKAN_API ModelBVH {
private:
    std::vector<ModelBVHNode> nodes;
    std::vector<Triangle> triangles;
    std::vector<BVHVertex> vertices;
    int rootIndex;

    const float TRAVERSAL_COST = 1.0f;
    const float INTERSECTION_COST = 1.5f;
    const int SAH_BUCKETS = 16;
    const int MAX_TRIANGLES_PER_LEAF = 8;
    const int MAX_DEPTH = 32;

    AABB calculateTriangleAABB(const Triangle& tri) {
        if (tri.v0_index >= vertices.size() || tri.v1_index >= vertices.size() || tri.v2_index >= vertices.size()) {
            return AABB(glm::vec3(0.0f), glm::vec3(0.0f));
        }

        const BVHVertex& v0 = vertices[tri.v0_index];
        const BVHVertex& v1 = vertices[tri.v1_index];
        const BVHVertex& v2 = vertices[tri.v2_index];

        glm::vec3 minVert = glm::min(glm::min(v0.position, v1.position), v2.position);
        glm::vec3 maxVert = glm::max(glm::max(v0.position, v1.position), v2.position);
        return AABB(minVert, maxVert);
    }

    glm::vec3 calculateTriangleCentroid(const Triangle& tri) {
        if (tri.v0_index >= vertices.size() || tri.v1_index >= vertices.size() || tri.v2_index >= vertices.size()) {
            return glm::vec3(0.0f);
        }

        const BVHVertex& v0 = vertices[tri.v0_index];
        const BVHVertex& v1 = vertices[tri.v1_index];
        const BVHVertex& v2 = vertices[tri.v2_index];

        return (v0.position + v1.position + v2.position) / 3.0f;
    }

    AABB unionAABB(const AABB& a, const AABB& b) {
        return AABB(glm::min(a.min, b.min), glm::max(a.max, b.max));
    }

    float calculateSAH(const AABB& bounds, int count) {
        if (count == 0) return FLT_MAX;

        glm::vec3 extent = bounds.max - bounds.min;
        float area = 2.0f * (extent.x * extent.y + extent.x * extent.z + extent.y * extent.z);
        return INTERSECTION_COST * count * area;
    }

    bool findBestSplitSAH(int start, int end, int& bestAxis, float& bestSplit, float& bestCost) {
        AABB centroidBounds(glm::vec3(FLT_MAX), glm::vec3(-FLT_MAX));

        for (int i = start; i <= end; i++) {
            glm::vec3 centroid = calculateTriangleCentroid(triangles[i]);
            centroidBounds.min = glm::min(centroidBounds.min, centroid);
            centroidBounds.max = glm::max(centroidBounds.max, centroid);
        }

        glm::vec3 extent = centroidBounds.max - centroidBounds.min;
        if (glm::length(extent) < 0.001f) return false;

        bestCost = FLT_MAX;
        bestAxis = -1;

        for (int axis = 0; axis < 3; axis++) {
            std::vector<SAHBucket> buckets(SAH_BUCKETS);

            float minVal = centroidBounds.min[axis];
            float maxVal = centroidBounds.max[axis];
            float bucketWidth = (maxVal - minVal) / SAH_BUCKETS;

            if (bucketWidth < 0.001f) continue;

            for (int i = start; i <= end; i++) {
                glm::vec3 centroid = calculateTriangleCentroid(triangles[i]);
                int bucketIdx = glm::clamp(int((centroid[axis] - minVal) / bucketWidth), 0, SAH_BUCKETS - 1);

                buckets[bucketIdx].count++;
                AABB triAABB = calculateTriangleAABB(triangles[i]);
                buckets[bucketIdx].bounds = unionAABB(buckets[bucketIdx].bounds, triAABB);
            }

            std::vector<SAHBucket> rightAccum(SAH_BUCKETS);
            SAHBucket tempRight;
            for (int i = SAH_BUCKETS - 1; i >= 0; i--) {
                tempRight.count += buckets[i].count;
                tempRight.bounds = unionAABB(tempRight.bounds, buckets[i].bounds);
                rightAccum[i] = tempRight;
            }

            SAHBucket leftAccum;
            for (int i = 0; i < SAH_BUCKETS - 1; i++) {
                leftAccum.count += buckets[i].count;
                leftAccum.bounds = unionAABB(leftAccum.bounds, buckets[i].bounds);

                const SAHBucket& rightBucket = rightAccum[i + 1];

                if (leftAccum.count == 0 || rightBucket.count == 0) continue;

                float cost = TRAVERSAL_COST + calculateSAH(leftAccum.bounds, leftAccum.count)
                    + calculateSAH(rightBucket.bounds, rightBucket.count);

                if (cost < bestCost) {
                    bestCost = cost;
                    bestAxis = axis;
                    bestSplit = minVal + (i + 1) * bucketWidth;
                }
            }
        }

        return bestAxis != -1;
    }

    int buildRecursive(int start, int end, int depth) {
        int triangleCount = end - start + 1;

        AABB fullBounds(glm::vec3(FLT_MAX), glm::vec3(-FLT_MAX));
        for (int i = start; i <= end; i++) {
            AABB triAABB = calculateTriangleAABB(triangles[i]);
            fullBounds.min = glm::min(fullBounds.min, triAABB.min);
            fullBounds.max = glm::max(fullBounds.max, triAABB.max);
        }

        if (depth >= MAX_DEPTH || triangleCount <= MAX_TRIANGLES_PER_LEAF) {
            ModelBVHNode node(fullBounds, true, -1, -1, start, triangleCount, depth);
            nodes.push_back(node);
            return static_cast<int>(nodes.size()) - 1;
        }

        int bestAxis;
        float bestSplit, bestCost;

        float noSplitCost = calculateSAH(fullBounds, triangleCount);

        if (!findBestSplitSAH(start, end, bestAxis, bestSplit, bestCost)) {
            glm::vec3 centroidBoundsMin(FLT_MAX), centroidBoundsMax(-FLT_MAX);
            for (int i = start; i <= end; i++) {
                glm::vec3 centroid = calculateTriangleCentroid(triangles[i]);
                centroidBoundsMin = glm::min(centroidBoundsMin, centroid);
                centroidBoundsMax = glm::max(centroidBoundsMax, centroid);
            }

            glm::vec3 extent = centroidBoundsMax - centroidBoundsMin;
            bestAxis = 0;
            if (extent.y > extent.x) bestAxis = 1;
            if (extent.z > extent[bestAxis]) bestAxis = 2;
            bestSplit = (centroidBoundsMin[bestAxis] + centroidBoundsMax[bestAxis]) * 0.5f;
            bestCost = noSplitCost * 0.8f;
        }

        if (bestCost >= noSplitCost) {
            ModelBVHNode node(fullBounds, true, -1, -1, start, triangleCount, depth);
            nodes.push_back(node);
            return static_cast<int>(nodes.size()) - 1;
        }

        int mid = start;
        for (int i = start; i <= end; i++) {
            glm::vec3 centroid = calculateTriangleCentroid(triangles[i]);
            if (centroid[bestAxis] < bestSplit) {
                std::swap(triangles[i], triangles[mid]);
                mid++;
            }
        }

        if (mid <= start) mid = start + 1;
        if (mid > end) mid = end;

        int leftChild = buildRecursive(start, mid - 1, depth + 1);
        int rightChild = buildRecursive(mid, end, depth + 1);

        AABB leftAABB(nodes[leftChild].boundsMin, nodes[leftChild].boundsMax);
        AABB rightAABB(nodes[rightChild].boundsMin, nodes[rightChild].boundsMax);
        AABB mergedAABB = unionAABB(leftAABB, rightAABB);

        ModelBVHNode node(mergedAABB, false, leftChild, rightChild, -1, 0, depth);
        nodes.push_back(node);
        return static_cast<int>(nodes.size()) - 1;
    }

public:
    ModelBVH() : rootIndex(-1) {}

    ModelBVH(ModelBVH&& other) noexcept
        : nodes(std::move(other.nodes))
        , triangles(std::move(other.triangles))
        , vertices(std::move(other.vertices))
        , rootIndex(other.rootIndex) {
        other.rootIndex = -1;
    }

    ModelBVH& operator=(ModelBVH&& other) noexcept {
        if (this != &other) {
            nodes = std::move(other.nodes);
            triangles = std::move(other.triangles);
            vertices = std::move(other.vertices);
            rootIndex = other.rootIndex;
            other.rootIndex = -1;
        }
        return *this;
    }

    ModelBVH(const ModelBVH&) = delete;
    ModelBVH& operator=(const ModelBVH&) = delete;

    void build(const std::vector<BVHVertex>& vertexArray, const std::vector<uint32_t>& indexArray, const std::vector<uint32_t>& materialIndices = {}) {
        nodes.clear();
        vertices = vertexArray;
        triangles.clear();

        if (vertices.empty() || indexArray.size() < 3) {
            rootIndex = -1;
            return;
        }

        size_t triangleCount = indexArray.size() / 3;
        triangles.reserve(triangleCount);
        bool hasMaterialIndices = (materialIndices.size() == triangleCount);

        size_t triangleIndex = 0;
        for (size_t i = 0; i < indexArray.size(); i += 3) {
            if (i + 2 < indexArray.size()) {
                uint32_t idx0 = indexArray[i];
                uint32_t idx1 = indexArray[i + 1];
                uint32_t idx2 = indexArray[i + 2];

                if (idx0 < vertices.size() && idx1 < vertices.size() && idx2 < vertices.size()) {
                    uint32_t materialIndex = (hasMaterialIndices && triangleIndex < materialIndices.size())
                        ? materialIndices[triangleIndex]
                        : 1;
                    triangles.emplace_back(idx0, idx1, idx2, materialIndex);
                    triangleIndex++;
                }
            }
        }

        if (triangles.empty()) {
            rootIndex = -1;
            return;
        }

        rootIndex = buildRecursive(0, static_cast<int>(triangles.size()) - 1, 0);

        std::cout << "=== Model BVH Build Complete ===" << std::endl;
        std::cout << "Total vertices: " << vertices.size() << std::endl;
        std::cout << "Total triangles: " << triangles.size() << std::endl;
        std::cout << "Total nodes: " << nodes.size() << std::endl;

        int leafNodes = 0, totalTrianglesInLeaves = 0;
        for (const auto& node : nodes) {
            if (node.isLeaf) {
                leafNodes++;
                totalTrianglesInLeaves += node.count;
            }
        }
        std::cout << "Leaf nodes: " << leafNodes << std::endl;
        std::cout << "Average triangles per leaf: " << (float)totalTrianglesInLeaves / leafNodes << std::endl;
    }

    void BuildFromSubMesh(const std::vector<struct Vertex>& vertexArray, const std::vector<uint32_t>& indexArray);

    const std::vector<ModelBVHNode>& getNodes() const { return nodes; }
    int getRootIndex() const { return rootIndex; }
    const std::vector<Triangle>& getTriangles() const { return triangles; }
    const std::vector<BVHVertex>& getVertices() const { return vertices; }

    bool IsValid() const { return rootIndex >= 0 && !nodes.empty(); }
    int GetNodeCount() const { return static_cast<int>(nodes.size()); }

    std::string getCachePath(const std::string& modelPath) const {
        std::filesystem::path path(modelPath);
        std::filesystem::path parentPath = path.parent_path();
        std::filesystem::path bvhDir = parentPath / "bvh";
        
#ifndef __ANDROID__
        // 桌面平台：创建bvh目录（如果不存在）
        std::filesystem::create_directories(bvhDir);
#endif
        
        std::string stem = path.stem().string();
        
        // 直接使用完整的stem名称，确保包含submesh索引
        return (bvhDir / (stem + "_bvh.txt")).string();
    }

    bool saveToFile(const std::string& modelPath) const {
#ifdef __ANDROID__
        // 安卓平台：跳过写入，因为assets是只读的
        return false;
#endif
        
        std::string cachePath = getCachePath(modelPath);
        std::ofstream file(cachePath, std::ios::binary);

        std::cout << "\n===== BVH Cache Saving =====" << std::endl;
        std::cout << "Model path: " << modelPath << std::endl;
        std::cout << "Cache path: " << cachePath << std::endl;

        if (!file.is_open()) {
            std::cerr << "Failed to open BVH cache file for writing: " << cachePath << std::endl;
            return false;
        }

        uint32_t nodeCount = static_cast<uint32_t>(nodes.size());
        uint32_t triangleCount = static_cast<uint32_t>(triangles.size());
        uint32_t vertexCount = static_cast<uint32_t>(vertices.size());
        uint32_t rootIdx = static_cast<uint32_t>(rootIndex);

        std::cout << "Data to save:" << std::endl;
        std::cout << "  - Nodes: " << nodeCount << std::endl;
        std::cout << "  - Triangles: " << triangleCount << std::endl;
        std::cout << "  - Vertices: " << vertexCount << std::endl;
        std::cout << "  - Root index: " << rootIdx << std::endl;

        file.write(reinterpret_cast<const char*>(&kBVHCacheMagic), sizeof(uint32_t));
        file.write(reinterpret_cast<const char*>(&kBVHCacheVersion), sizeof(uint32_t));
        file.write(reinterpret_cast<const char*>(&nodeCount), sizeof(uint32_t));
        file.write(reinterpret_cast<const char*>(&triangleCount), sizeof(uint32_t));
        file.write(reinterpret_cast<const char*>(&vertexCount), sizeof(uint32_t));
        file.write(reinterpret_cast<const char*>(&rootIdx), sizeof(uint32_t));

        if (!nodes.empty()) {
            file.write(reinterpret_cast<const char*>(nodes.data()), nodes.size() * sizeof(ModelBVHNode));
        }

        if (!triangles.empty()) {
            file.write(reinterpret_cast<const char*>(triangles.data()), triangles.size() * sizeof(Triangle));
        }

        if (!vertices.empty()) {
            file.write(reinterpret_cast<const char*>(vertices.data()), vertices.size() * sizeof(BVHVertex));
        }

        file.close();

        size_t fileSize = std::filesystem::file_size(cachePath);
        std::cout << "BVH cache saved successfully!" << std::endl;
        std::cout << "File size: " << fileSize / 1024 << " KB" << std::endl;
        std::cout << "=============================\n" << std::endl;

        return true;
    }

    bool loadFromFile(const std::string& modelPath, bool loadVertices = true) {
        std::string cachePath = getCachePath(modelPath);
        
        std::cout << "\n===== BVH Cache Loading =====" << std::endl;
        std::cout << "Model path: " << modelPath << std::endl;
        std::cout << "Cache path: " << cachePath << std::endl;

#ifdef __ANDROID__
        // 安卓平台：使用SDL从assets读取
        SDL_IOStream* io = SDL_IOFromFile(cachePath.c_str(), "rb");
        if (!io) {
            std::cout << "Cache file not found in assets, will build BVH from scratch" << std::endl;
            std::cout << "=============================\n" << std::endl;
            return false;
        }
        
        Sint64 fileSize = SDL_GetIOSize(io);
        if (fileSize < static_cast<Sint64>(sizeof(uint32_t) * 6)) {
            std::cerr << "BVH cache file too small: " << cachePath << std::endl;
            SDL_CloseIO(io);
            std::cout << "=============================\n" << std::endl;
            return false;
        }
        
        uint32_t magic, version, nodeCount, triangleCount, vertexCount, rootIdx;

        if (SDL_ReadIO(io, &magic, sizeof(uint32_t)) != sizeof(uint32_t) ||
            SDL_ReadIO(io, &version, sizeof(uint32_t)) != sizeof(uint32_t)) {
            std::cerr << "Failed to read BVH cache version header: " << cachePath << std::endl;
            SDL_CloseIO(io);
            std::cout << "=============================\n" << std::endl;
            return false;
        }
        if (magic != kBVHCacheMagic || version != kBVHCacheVersion) {
            std::cout << "BVH cache version mismatch, will rebuild: " << cachePath << std::endl;
            SDL_CloseIO(io);
            std::cout << "=============================\n" << std::endl;
            return false;
        }

        if (SDL_ReadIO(io, &nodeCount, sizeof(uint32_t)) != sizeof(uint32_t) ||
            SDL_ReadIO(io, &triangleCount, sizeof(uint32_t)) != sizeof(uint32_t) ||
            SDL_ReadIO(io, &vertexCount, sizeof(uint32_t)) != sizeof(uint32_t) ||
            SDL_ReadIO(io, &rootIdx, sizeof(uint32_t)) != sizeof(uint32_t)) {
            std::cerr << "Failed to read BVH cache header: " << cachePath << std::endl;
            SDL_CloseIO(io);
            std::cout << "=============================\n" << std::endl;
            return false;
        }
        
        uint64_t expectedFileSize = sizeof(uint32_t) * 6 +
            nodeCount * sizeof(ModelBVHNode) +
            triangleCount * sizeof(Triangle) +
            vertexCount * sizeof(BVHVertex);
        
        if (static_cast<uint64_t>(fileSize) != expectedFileSize) {
            std::cout << "Cache file structure mismatch, will build BVH from scratch" << std::endl;
            SDL_CloseIO(io);
            std::cout << "=============================\n" << std::endl;
            return false;
        }
        
        std::cout << "Data loaded from cache:" << std::endl;
        std::cout << "  - Nodes: " << nodeCount << std::endl;
        std::cout << "  - Triangles: " << triangleCount << std::endl;
        std::cout << "  - Vertices in cache: " << vertexCount << std::endl;
        std::cout << "  - Root index: " << rootIdx << std::endl;
        
        nodes.resize(nodeCount);
        triangles.resize(triangleCount);
        if (loadVertices) {
            vertices.resize(vertexCount);
        }
        rootIndex = static_cast<int>(rootIdx);
        
        if (nodeCount > 0) {
            if (SDL_ReadIO(io, nodes.data(), nodeCount * sizeof(ModelBVHNode)) != static_cast<size_t>(nodeCount * sizeof(ModelBVHNode))) {
                std::cerr << "Failed to read BVH nodes: " << cachePath << std::endl;
                SDL_CloseIO(io);
                std::cout << "=============================\n" << std::endl;
                return false;
            }
        }
        
        if (triangleCount > 0) {
            if (SDL_ReadIO(io, triangles.data(), triangleCount * sizeof(Triangle)) != static_cast<size_t>(triangleCount * sizeof(Triangle))) {
                std::cerr << "Failed to read BVH triangles: " << cachePath << std::endl;
                SDL_CloseIO(io);
                std::cout << "=============================\n" << std::endl;
                return false;
            }
        }
        
        if (loadVertices && vertexCount > 0) {
            if (SDL_ReadIO(io, vertices.data(), vertexCount * sizeof(BVHVertex)) != static_cast<size_t>(vertexCount * sizeof(BVHVertex))) {
                std::cerr << "Failed to read BVH vertices: " << cachePath << std::endl;
                SDL_CloseIO(io);
                std::cout << "=============================\n" << std::endl;
                return false;
            }
        }
        else if (!loadVertices && vertexCount > 0) {
            SDL_SeekIO(io, vertexCount * sizeof(BVHVertex), SDL_IO_SEEK_CUR);
        }
        
        SDL_CloseIO(io);
        
        std::cout << "BVH cache loaded successfully from assets!" << std::endl;
        std::cout << "=============================\n" << std::endl;
        return true;
#else
        // 桌面平台：使用标准文件流
        std::ifstream file(cachePath, std::ios::binary);

        if (!file.is_open()) {
            std::cout << "Cache file not found, will build BVH from scratch" << std::endl;
            std::cout << "=============================\n" << std::endl;
            return false;
        }

        uint32_t magic, version, nodeCount, triangleCount, vertexCount, rootIdx;

        file.read(reinterpret_cast<char*>(&magic), sizeof(uint32_t));
        file.read(reinterpret_cast<char*>(&version), sizeof(uint32_t));
        if (!file || magic != kBVHCacheMagic || version != kBVHCacheVersion) {
            std::cout << "BVH cache version mismatch, will rebuild: " << cachePath << std::endl;
            return false;
        }
        file.read(reinterpret_cast<char*>(&nodeCount), sizeof(uint32_t));
        file.read(reinterpret_cast<char*>(&triangleCount), sizeof(uint32_t));
        file.read(reinterpret_cast<char*>(&vertexCount), sizeof(uint32_t));
        file.read(reinterpret_cast<char*>(&rootIdx), sizeof(uint32_t));

        if (file.fail()) {
            std::cerr << "Failed to read BVH cache header: " << cachePath << std::endl;
            std::cout << "=============================\n" << std::endl;
            return false;
        }

        uint64_t expectedFileSize = sizeof(uint32_t) * 6 +
            nodeCount * sizeof(ModelBVHNode) +
            triangleCount * sizeof(Triangle) +
            vertexCount * sizeof(BVHVertex);

        file.seekg(0, std::ios::end);
        uint64_t actualFileSize = file.tellg();
        file.seekg(sizeof(uint32_t) * 6, std::ios::beg);

        if (actualFileSize != expectedFileSize) {
            std::cout << "Cache file structure mismatch, will build BVH from scratch" << std::endl;
            std::cout << "=============================\n" << std::endl;
            return false;
        }

        std::cout << "Data loaded from cache:" << std::endl;
        std::cout << "  - Nodes: " << nodeCount << std::endl;
        std::cout << "  - Triangles: " << triangleCount << std::endl;
        std::cout << "  - Vertices in cache: " << vertexCount << std::endl;
        std::cout << "  - Root index: " << rootIdx << std::endl;

        nodes.resize(nodeCount);
        triangles.resize(triangleCount);
        if (loadVertices) {
            vertices.resize(vertexCount);
        }
        rootIndex = static_cast<int>(rootIdx);

        if (nodeCount > 0) {
            file.read(reinterpret_cast<char*>(nodes.data()), nodeCount * sizeof(ModelBVHNode));
        }

        if (triangleCount > 0) {
            file.read(reinterpret_cast<char*>(triangles.data()), triangleCount * sizeof(Triangle));
        }

        if (loadVertices && vertexCount > 0) {
            file.read(reinterpret_cast<char*>(vertices.data()), vertexCount * sizeof(BVHVertex));
        }
        else if (!loadVertices && vertexCount > 0) {
            file.seekg(vertexCount * sizeof(BVHVertex), std::ios::cur);
        }

        if (file.fail()) {
            std::cerr << "Failed to read BVH cache data: " << cachePath << std::endl;
            std::cout << "=============================\n" << std::endl;
            return false;
        }

        file.close();

        std::cout << "BVH cache loaded successfully!" << std::endl;
        std::cout << "=============================\n" << std::endl;
        return true;
#endif
    }

    std::vector<AABB> GetAllNodeBounds() const {
        std::vector<AABB> bounds;
        bounds.reserve(nodes.size());

        for (const auto& node : nodes) {
            bounds.push_back(AABB(node.boundsMin, node.boundsMax));
        }

        return bounds;
    }

    // BVH遍历方法，用于可见性测试
    bool IsVisibleFromPoint(const glm::vec3& point, const glm::mat4& modelMatrix) const;
    bool IsVisibleFromPointRecursive(int nodeIndex, const glm::vec3& point, const glm::mat4& modelMatrix, const glm::mat4& invModelMatrix, int currentDepth) const;
    bool IsPointVisibleThroughNode(int nodeIndex, const glm::vec3& point, const glm::mat4& modelMatrix, const glm::mat4& invModelMatrix) const;
    
    // 射线-AABB相交测试（用于BLAS级别的遮挡剔除）
    bool RayIntersects(const glm::vec3& rayOrigin, const glm::vec3& rayDir, float maxDistance) const {
        if (!IsValid()) return false;
        
        // 使用根节点AABB进行快速测试
        const ModelBVHNode& root = nodes[rootIndex];
        AABB rootAABB(root.boundsMin, root.boundsMax);
        
        float t;
        return AABBUtils::RayIntersectsAABB(rayOrigin, rayDir, rootAABB, t) && t < maxDistance;
    }

    struct MIKAN_API Frustum {
        glm::vec4 planes[6];
        
        Frustum() {}
        
        Frustum(const glm::mat4& viewProj) {
            glm::mat4 m = glm::transpose(viewProj);
            planes[0] = m[3] + m[0];
            planes[1] = m[3] - m[0];
            planes[2] = m[3] + m[1];
            planes[3] = m[3] - m[1];
            planes[4] = m[3] + m[2];
            planes[5] = m[3] - m[2];
            
            for (int i = 0; i < 6; i++) {
                float len = glm::length(glm::vec3(planes[i]));
                planes[i] /= len;
            }
        }
        
        bool IntersectsAABB(const glm::vec3& min, const glm::vec3& max) const {
            for (int i = 0; i < 6; i++) {
                glm::vec3 normal(planes[i]);
                float distance = planes[i].w;
                
                glm::vec3 p;
                p.x = (normal.x >= 0) ? max.x : min.x;
                p.y = (normal.y >= 0) ? max.y : min.y;
                p.z = (normal.z >= 0) ? max.z : min.z;
                
                if (glm::dot(normal, p) + distance < 0) {
                    return false;
                }
            }
            return true;
        }
    };

    void CullWithFrustum(const Frustum& frustum, std::vector<int>& visibleLeafIndices) const {
        visibleLeafIndices.clear();
        if (rootIndex < 0 || nodes.empty()) return;
        
        std::vector<int> stack;
        stack.reserve(64);
        stack.push_back(rootIndex);
        
        while (!stack.empty()) {
            int nodeIdx = stack.back();
            stack.pop_back();
            
            if (nodeIdx < 0 || nodeIdx >= static_cast<int>(nodes.size())) continue;
            
            const ModelBVHNode& node = nodes[nodeIdx];
            
            if (!frustum.IntersectsAABB(node.boundsMin, node.boundsMax)) {
                continue;
            }
            
            if (node.isLeaf) {
                visibleLeafIndices.push_back(nodeIdx);
            } else {
                if (node.left >= 0) stack.push_back(node.left);
                if (node.right >= 0) stack.push_back(node.right);
            }
        }
    }

    void GetVisibleTriangles(const std::vector<int>& visibleLeafIndices, 
                             std::vector<uint32_t>& triangleIndices) const {
        triangleIndices.clear();
        
        for (int leafIdx : visibleLeafIndices) {
            if (leafIdx < 0 || leafIdx >= static_cast<int>(nodes.size())) continue;
            
            const ModelBVHNode& node = nodes[leafIdx];
            
            for (int i = 0; i < node.count; i++) {
                int triIdx = node.startIndex + i;
                if (triIdx >= 0 && triIdx < static_cast<int>(triangles.size())) {
                    triangleIndices.push_back(static_cast<uint32_t>(triIdx));
                }
            }
        }
    }

    int GetVisibleTriangleCount(const std::vector<int>& visibleLeafIndices) const {
        int count = 0;
        for (int leafIdx : visibleLeafIndices) {
            if (leafIdx >= 0 && leafIdx < static_cast<int>(nodes.size())) {
                count += nodes[leafIdx].count;
            }
        }
        return count;
    }
};

struct MIKAN_API SubMeshCullResult {
    int subMeshIndex;
    std::vector<int> visibleLeafIndices;
    int visibleTriangleCount;
};

struct MIKAN_API TopLevelBVHNode {
    alignas(16) glm::vec3 boundsMin;
    alignas(16) glm::vec3 boundsMax;
    alignas(4) int isLeaf;
    alignas(4) int left;
    alignas(4) int right;
    alignas(4) int subMeshIndex;
    alignas(4) int depth;
    alignas(4) int visible;  // 可见性掩码
    alignas(4) char padding[4];
    
    TopLevelBVHNode() : boundsMin(0.0f), boundsMax(0.0f),
        isLeaf(false), left(-1), right(-1), subMeshIndex(-1), depth(0), visible(1) {
        memset(padding, 0, sizeof(padding));
    }
};

struct MIKAN_API ModelBVHData {
    // 2026-08-09：内存级共享——同几何 subMesh 共享同一 BLAS（shared_ptr，构建一次；禁拷贝见 ModelBVH L301-302）
    // BLAS 顶点已规范化（去 subMesh AABB 中心）——同形状不同放置位置可共享；原始位置由 subMeshAABBs + 实体变换承担
    std::vector<std::shared_ptr<ModelBVH>> subMeshBVHs;
    std::vector<AABB> subMeshAABBs;   // 每 subMesh 原始局部 AABB（含放置偏移；TLAS 构建用）
    std::vector<TopLevelBVHNode> topLevelNodes;
    int topLevelRoot;
    
    ModelBVHData() : topLevelRoot(-1) {}

    bool IsValid() const { return !subMeshBVHs.empty(); }
    bool HasTopLevelBVH() const { return topLevelRoot >= 0 && !topLevelNodes.empty(); }
    size_t GetSubMeshCount() const { return subMeshBVHs.size(); }
    // 2026-08-09：BLAS 可懒构建（非光追 nullptr）——返回指针避免解引用崩溃
    const ModelBVH* GetSubMeshBVH(size_t index) const {
        if (index >= subMeshBVHs.size()) return nullptr;
        return subMeshBVHs[index].get();
    }

    std::vector<AABB> GetAllNodeBounds(size_t subMeshIndex) const {
        if (subMeshIndex < subMeshBVHs.size() && subMeshBVHs[subMeshIndex]) {
            return subMeshBVHs[subMeshIndex]->GetAllNodeBounds();
        }
        return {};
    }

    std::vector<AABB> GetAllNodeBounds() const {
        std::vector<AABB> allBounds;

        for (const auto& bvh : subMeshBVHs) {
            if (!bvh) continue;
            auto bounds = bvh->GetAllNodeBounds();
            allBounds.insert(allBounds.end(), bounds.begin(), bounds.end());
        }

        return allBounds;
    }
    
    std::vector<AABB> GetTopLevelNodeBounds() const {
        std::vector<AABB> bounds;
        bounds.reserve(topLevelNodes.size());
        
        for (const auto& node : topLevelNodes) {
            bounds.push_back(AABB(node.boundsMin, node.boundsMax));
        }
        
        return bounds;
    }
    
    void BuildTopLevelBVH() {
        if (subMeshBVHs.size() <= 1) {
            topLevelRoot = -1;
            topLevelNodes.clear();
            return;
        }
        
        topLevelNodes.clear();
        
        std::vector<int> subMeshIndices;
        // 2026-08-09：TLAS 只依赖 subMeshAABBs（BLAS 可懒构建/跳过——非光追时全 nullptr）
        for (size_t i = 0; i < subMeshAABBs.size(); i++) {
            subMeshIndices.push_back(static_cast<int>(i));
        }
        
        if (subMeshIndices.empty()) {
            topLevelRoot = -1;
            return;
        }
        
        topLevelRoot = BuildTopLevelRecursive(subMeshIndices, 0);
    }
    
    // TLAS磁盘缓存路径
    std::string getTLASCachePath(const std::string& modelPath) const {
        std::filesystem::path path(modelPath);
        std::filesystem::path parentPath = path.parent_path();
        std::filesystem::path bvhDir = parentPath / "bvh";
#ifdef __ANDROID__
        // APK assets 只读，不能在模型目录下创建 bvh；缓存若随 APK 提供则由
        // SDL_IOFromFile 读取，不存在时由调用方走内存构建路径。
        std::string stem = path.stem().string();
        return (bvhDir / (stem + "_tlas.txt")).string();
#else
        std::filesystem::create_directories(bvhDir);
        
        std::string stem = path.stem().string();
        return (bvhDir / (stem + "_tlas.txt")).string();
#endif
    }
    
    // 保存TLAS到磁盘
    bool saveTLASToFile(const std::string& modelPath) const {
#ifdef __ANDROID__
        // 安卓平台：跳过写入，因为assets是只读的
        return false;
#endif
        
        if (!HasTopLevelBVH()) return false;
        
        std::string cachePath = getTLASCachePath(modelPath);
        std::ofstream file(cachePath, std::ios::binary);
        
        if (!file.is_open()) {
            std::cerr << "Failed to open TLAS cache file for writing: " << cachePath << std::endl;
            return false;
        }
        
        uint32_t nodeCount = static_cast<uint32_t>(topLevelNodes.size());
        uint32_t rootIdx = static_cast<uint32_t>(topLevelRoot);
        
        file.write(reinterpret_cast<const char*>(&kBVHCacheMagic), sizeof(uint32_t));
        file.write(reinterpret_cast<const char*>(&kBVHCacheVersion), sizeof(uint32_t));
        file.write(reinterpret_cast<const char*>(&nodeCount), sizeof(uint32_t));
        file.write(reinterpret_cast<const char*>(&rootIdx), sizeof(uint32_t));
        
        for (const auto& node : topLevelNodes) {
            file.write(reinterpret_cast<const char*>(&node.boundsMin), sizeof(glm::vec3));
            file.write(reinterpret_cast<const char*>(&node.boundsMax), sizeof(glm::vec3));
            file.write(reinterpret_cast<const char*>(&node.isLeaf), sizeof(int));
            file.write(reinterpret_cast<const char*>(&node.left), sizeof(int));
            file.write(reinterpret_cast<const char*>(&node.right), sizeof(int));
            file.write(reinterpret_cast<const char*>(&node.subMeshIndex), sizeof(int));
            file.write(reinterpret_cast<const char*>(&node.depth), sizeof(int));
            file.write(reinterpret_cast<const char*>(&node.visible), sizeof(int));
        }
        
        std::cout << "TLAS saved to: " << cachePath << " (" << nodeCount << " nodes)" << std::endl;
        return true;
    }
    
    // 从磁盘加载TLAS
    bool loadTLASFromFile(const std::string& modelPath) {
        std::string cachePath = getTLASCachePath(modelPath);
        
#ifdef __ANDROID__
        // 安卓平台：使用SDL从assets读取
        SDL_IOStream* io = SDL_IOFromFile(cachePath.c_str(), "rb");
        if (!io) {
            return false;
        }
        
        uint32_t magic, version, nodeCount, rootIdx;
        if (SDL_ReadIO(io, &magic, sizeof(uint32_t)) != sizeof(uint32_t) ||
            SDL_ReadIO(io, &version, sizeof(uint32_t)) != sizeof(uint32_t)) {
            SDL_CloseIO(io);
            return false;
        }
        if (magic != kBVHCacheMagic || version != kBVHCacheVersion) {
            SDL_CloseIO(io);
            return false;
        }
        if (SDL_ReadIO(io, &nodeCount, sizeof(uint32_t)) != sizeof(uint32_t) ||
            SDL_ReadIO(io, &rootIdx, sizeof(uint32_t)) != sizeof(uint32_t)) {
            SDL_CloseIO(io);
            return false;
        }
        
        topLevelNodes.clear();
        topLevelNodes.reserve(nodeCount);
        
        for (uint32_t i = 0; i < nodeCount; i++) {
            TopLevelBVHNode node;
            if (SDL_ReadIO(io, &node.boundsMin, sizeof(glm::vec3)) != sizeof(glm::vec3) ||
                SDL_ReadIO(io, &node.boundsMax, sizeof(glm::vec3)) != sizeof(glm::vec3) ||
                SDL_ReadIO(io, &node.isLeaf, sizeof(int)) != sizeof(int) ||
                SDL_ReadIO(io, &node.left, sizeof(int)) != sizeof(int) ||
                SDL_ReadIO(io, &node.right, sizeof(int)) != sizeof(int) ||
                SDL_ReadIO(io, &node.subMeshIndex, sizeof(int)) != sizeof(int) ||
                SDL_ReadIO(io, &node.depth, sizeof(int)) != sizeof(int) ||
                SDL_ReadIO(io, &node.visible, sizeof(int)) != sizeof(int)) {
                SDL_CloseIO(io);
                return false;
            }
            topLevelNodes.push_back(node);
        }
        
        topLevelRoot = static_cast<int>(rootIdx);
        
        SDL_CloseIO(io);
        
        std::cout << "TLAS loaded from assets: " << cachePath << " (" << nodeCount << " nodes)" << std::endl;
        return true;
#else
        // 桌面平台：使用标准文件流
        std::ifstream file(cachePath, std::ios::binary);
        
        if (!file.is_open()) {
            return false;
        }
        
        uint32_t magic, version, nodeCount, rootIdx;
        file.read(reinterpret_cast<char*>(&magic), sizeof(uint32_t));
        file.read(reinterpret_cast<char*>(&version), sizeof(uint32_t));
        if (!file || magic != kBVHCacheMagic || version != kBVHCacheVersion) {
            std::cout << "TLAS cache version mismatch, will rebuild: " << cachePath << std::endl;
            return false;
        }
        file.read(reinterpret_cast<char*>(&nodeCount), sizeof(uint32_t));
        file.read(reinterpret_cast<char*>(&rootIdx), sizeof(uint32_t));
        
        topLevelNodes.clear();
        topLevelNodes.reserve(nodeCount);
        
        for (uint32_t i = 0; i < nodeCount; i++) {
            TopLevelBVHNode node;
            file.read(reinterpret_cast<char*>(&node.boundsMin), sizeof(glm::vec3));
            file.read(reinterpret_cast<char*>(&node.boundsMax), sizeof(glm::vec3));
            file.read(reinterpret_cast<char*>(&node.isLeaf), sizeof(int));
            file.read(reinterpret_cast<char*>(&node.left), sizeof(int));
            file.read(reinterpret_cast<char*>(&node.right), sizeof(int));
            file.read(reinterpret_cast<char*>(&node.subMeshIndex), sizeof(int));
            file.read(reinterpret_cast<char*>(&node.depth), sizeof(int));
            file.read(reinterpret_cast<char*>(&node.visible), sizeof(int));
            topLevelNodes.push_back(node);
        }
        
        topLevelRoot = static_cast<int>(rootIdx);
        
        std::cout << "TLAS loaded from: " << cachePath << " (" << nodeCount << " nodes)" << std::endl;
        return true;
#endif
    }
    
    // 检查TLAS缓存是否存在且有效
    bool hasTLASCache(const std::string& modelPath) const {
        std::string cachePath = getTLASCachePath(modelPath);
        return std::filesystem::exists(cachePath);
    }
    
private:
    int BuildTopLevelRecursive(const std::vector<int>& subMeshIndices, int depth) {
        if (subMeshIndices.empty()) {
            return -1;
        }
        
        AABB fullBounds(glm::vec3(FLT_MAX), glm::vec3(-FLT_MAX));
        glm::vec3 centroidMin(FLT_MAX), centroidMax(-FLT_MAX);
        
        for (int idx : subMeshIndices) {
            if (idx < 0 || idx >= (int)subMeshAABBs.size()) continue;
            AABB subMeshAABB = subMeshAABBs[idx];
            fullBounds.min = glm::min(fullBounds.min, subMeshAABB.min);
            fullBounds.max = glm::max(fullBounds.max, subMeshAABB.max);
            glm::vec3 centroid = subMeshAABB.GetCenter();
            centroidMin = glm::min(centroidMin, centroid);
            centroidMax = glm::max(centroidMax, centroid);
        }
        
        if (subMeshIndices.size() == 1) {
            TopLevelBVHNode node;
            node.boundsMin = fullBounds.min;
            node.boundsMax = fullBounds.max;
            node.isLeaf = 1;
            node.subMeshIndex = subMeshIndices[0];
            node.depth = depth;
            topLevelNodes.push_back(node);
            return static_cast<int>(topLevelNodes.size()) - 1;
        }
        
        glm::vec3 extent = centroidMax - centroidMin;
        int bestAxis = 0;
        if (extent.y > extent.x) bestAxis = 1;
        if (extent.z > extent[bestAxis]) bestAxis = 2;
        
        float splitPos = (centroidMin[bestAxis] + centroidMax[bestAxis]) * 0.5f;
        
        std::vector<int> leftIndices, rightIndices;
        for (int idx : subMeshIndices) {
            if (idx < 0 || idx >= (int)subMeshAABBs.size()) continue;
            AABB subMeshAABB = subMeshAABBs[idx];
            glm::vec3 centroid = subMeshAABB.GetCenter();
            if (centroid[bestAxis] < splitPos) {
                leftIndices.push_back(idx);
            } else {
                rightIndices.push_back(idx);
            }
        }
        
        if (leftIndices.empty() || rightIndices.empty()) {
            size_t mid = subMeshIndices.size() / 2;
            leftIndices.clear();
            rightIndices.clear();
            for (size_t i = 0; i < mid; i++) {
                leftIndices.push_back(subMeshIndices[i]);
            }
            for (size_t i = mid; i < subMeshIndices.size(); i++) {
                rightIndices.push_back(subMeshIndices[i]);
            }
        }
        
        int leftChild = BuildTopLevelRecursive(leftIndices, depth + 1);
        int rightChild = BuildTopLevelRecursive(rightIndices, depth + 1);
        
        TopLevelBVHNode node;
        node.boundsMin = fullBounds.min;
        node.boundsMax = fullBounds.max;
        node.isLeaf = 0;
        node.left = leftChild;
        node.right = rightChild;
        node.depth = depth;
        topLevelNodes.push_back(node);
        
        return static_cast<int>(topLevelNodes.size()) - 1;
    }

public:
    // 更新TLAS节点可见性（基于视锥剔除和遮挡剔除）
    void UpdateTLASVisibility(const std::vector<bool>& subMeshVisible) {
        if (!HasTopLevelBVH()) return;
        
        // 更新叶子节点的可见性
        for (auto& node : topLevelNodes) {
            if (node.isLeaf && node.subMeshIndex >= 0 && node.subMeshIndex < static_cast<int>(subMeshVisible.size())) {
                node.visible = subMeshVisible[node.subMeshIndex] ? 1 : 0;
            }
        }
        
        // 更新内部节点的可见性（如果子节点都不可见，则节点不可见）
        UpdateNodeVisibilityRecursive(topLevelRoot);
    }
    
    // 获取可见的TLAS节点索引
    std::vector<int> GetVisibleTLASNodeIndices() const {
        std::vector<int> visibleIndices;
        if (!HasTopLevelBVH()) return visibleIndices;
        
        CollectVisibleNodesRecursive(topLevelRoot, visibleIndices);
        return visibleIndices;
    }
    
    // 获取TLAS节点数量
    size_t GetTLASNodeCount() const { return topLevelNodes.size(); }
    
    // 获取TLAS根节点索引
    int GetTLASRootIndex() const { return topLevelRoot; }
    
    // 获取TLAS节点（用于SSBO构建）
    const std::vector<TopLevelBVHNode>& GetTopLevelNodes() const { return topLevelNodes; }
    
    // 合并邻近的BLAS以简化TLAS结构
    void MergeNearbyBLAS(float thresholdPercentage = 0.01f) {
        if (subMeshBVHs.size() <= 1) return;
        
        // 计算整个模型的包围盒
            AABB modelAABB;
            bool firstAABB = true;
            for (size_t i = 0; i < subMeshBVHs.size(); i++) {
                if (!subMeshBVHs[i] || !subMeshBVHs[i]->IsValid()) continue;
                
                auto bounds = subMeshBVHs[i]->GetAllNodeBounds();
                if (bounds.empty()) continue;
                
                AABB blasAABB = bounds[subMeshBVHs[i]->getRootIndex()];
                if (firstAABB) {
                    modelAABB = blasAABB;
                    firstAABB = false;
                } else {
                    modelAABB = AABBUtils::Union(modelAABB, blasAABB);
                }
            }
            
            // 计算包围盒的对角线长度
            glm::vec3 size = modelAABB.max - modelAABB.min;
            float diagonalLength = glm::length(size);
            
            // 根据对角线长度的百分比计算合并阈值
            float mergeThreshold = diagonalLength * thresholdPercentage;
            std::cout << "Merge threshold calculated: " << mergeThreshold << " (" << (thresholdPercentage * 100) << "% of diagonal length " << diagonalLength << ")" << std::endl;
        
        std::vector<bool> merged(subMeshBVHs.size(), false);
        std::vector<std::shared_ptr<ModelBVH>> mergedBVHs;
        
        for (size_t i = 0; i < subMeshBVHs.size(); i++) {
            if (merged[i] || !subMeshBVHs[i] || !subMeshBVHs[i]->IsValid()) continue;
            
            // 查找邻近的BLAS
            std::vector<size_t> nearbyBLAS;
            nearbyBLAS.push_back(i);
            
            AABB mergedAABB = subMeshBVHs[i]->GetAllNodeBounds()[subMeshBVHs[i]->getRootIndex()];
            
            for (size_t j = i + 1; j < subMeshBVHs.size(); j++) {
                if (merged[j] || !subMeshBVHs[j] || !subMeshBVHs[j]->IsValid()) continue;
                
                AABB jAABB = subMeshBVHs[j]->GetAllNodeBounds()[subMeshBVHs[j]->getRootIndex()];
                
                // 计算两个BLAS之间的中心点距离
                glm::vec3 centerA = mergedAABB.GetCenter();
                glm::vec3 centerB = jAABB.GetCenter();
                float distance = glm::distance(centerA, centerB);
                
                // 如果距离小于阈值，合并它们
                if (distance < mergeThreshold) {
                    nearbyBLAS.push_back(j);
                    merged[j] = true;
                    mergedAABB = AABBUtils::Union(mergedAABB, jAABB);
                }
            }
            
            // 如果找到多个邻近的BLAS，创建一个合并后的BVH
            if (nearbyBLAS.size() > 1) {
                ModelBVH mergedBVH;
                std::vector<BVHVertex> mergedVertices;
                std::vector<uint32_t> mergedIndices;
                std::vector<uint32_t> mergedMaterials;
                
                // 合并所有邻近BLAS的三角形
                for (size_t blasIdx : nearbyBLAS) {
                    const ModelBVH& blas = *subMeshBVHs[blasIdx];
                    const auto& vertices = blas.getVertices();
                    const auto& triangles = blas.getTriangles();
                    
                    uint32_t vertexOffset = static_cast<uint32_t>(mergedVertices.size());
                    
                    // 添加顶点
                    mergedVertices.insert(mergedVertices.end(), vertices.begin(), vertices.end());
                    
                    // 添加索引
                    for (const auto& tri : triangles) {
                        mergedIndices.push_back(tri.v0_index + vertexOffset);
                        mergedIndices.push_back(tri.v1_index + vertexOffset);
                        mergedIndices.push_back(tri.v2_index + vertexOffset);
                        mergedMaterials.push_back(tri.material);
                    }
                }
                
                // 构建合并后的BVH
                mergedBVH.build(mergedVertices, mergedIndices, mergedMaterials);
                mergedBVHs.push_back(std::make_shared<ModelBVH>(std::move(mergedBVH)));
            } else {
                // 单个BLAS，使用移动语义添加
                mergedBVHs.push_back(std::move(subMeshBVHs[i]));
            }
        }
        
        // 替换原来的BLAS
        subMeshBVHs = std::move(mergedBVHs);
        
        // 重新构建TLAS
        BuildTopLevelBVH();
        
        std::cout << "BLAS merged successfully. New BLAS count: " << subMeshBVHs.size() << std::endl;
    }
    
    void CullAllWithFrustum(const ModelBVH::Frustum& frustum, 
                            std::vector<SubMeshCullResult>& results) const {
        results.clear();
        results.reserve(subMeshBVHs.size());
        
        for (size_t i = 0; i < subMeshBVHs.size(); i++) {
            SubMeshCullResult result;
            result.subMeshIndex = static_cast<int>(i);
            
            if (!subMeshBVHs[i]) continue;
            subMeshBVHs[i]->CullWithFrustum(frustum, result.visibleLeafIndices);
            result.visibleTriangleCount = subMeshBVHs[i]->GetVisibleTriangleCount(result.visibleLeafIndices);
            
            results.push_back(result);
        }
    }

    int GetTotalVisibleTriangles(const std::vector<SubMeshCullResult>& results) const {
        int total = 0;
        for (const auto& r : results) {
            total += r.visibleTriangleCount;
        }
        return total;
    }
    

    
    // 构建简化版TLAS（仅包含可见的BLAS）
    void BuildSimplifiedTLAS(const std::vector<bool>& visibleFlags) {
        topLevelNodes.clear();
        topLevelRoot = -1;
        
        std::vector<int> visibleSubMeshIndices;
        for (size_t i = 0; i < subMeshBVHs.size(); i++) {
            if (visibleFlags[i] && subMeshBVHs[i] && subMeshBVHs[i]->IsValid()) {
                visibleSubMeshIndices.push_back(static_cast<int>(i));
            }
        }
        
        if (visibleSubMeshIndices.empty()) {
            return;
        }
        
        // 为可见的BLAS构建简化版TLAS
        topLevelRoot = BuildTopLevelRecursive(visibleSubMeshIndices, 0);
        
        std::cout << "Simplified TLAS built. Visible BLAS count: " << visibleSubMeshIndices.size() << std::endl;
    }
    
    bool UpdateNodeVisibilityRecursive(int nodeIndex) {
        if (nodeIndex < 0 || nodeIndex >= static_cast<int>(topLevelNodes.size())) {
            return false;
        }
        
        TopLevelBVHNode& node = topLevelNodes[nodeIndex];
        
        if (node.isLeaf) {
            return node.visible != 0;
        }
        
        bool leftVisible = UpdateNodeVisibilityRecursive(node.left);
        bool rightVisible = UpdateNodeVisibilityRecursive(node.right);
        
        node.visible = (leftVisible || rightVisible) ? 1 : 0;
        return node.visible != 0;
    }
    
    void CollectVisibleNodesRecursive(int nodeIndex, std::vector<int>& visibleIndices) const {
        if (nodeIndex < 0 || nodeIndex >= static_cast<int>(topLevelNodes.size())) {
            return;
        }
        
        const TopLevelBVHNode& node = topLevelNodes[nodeIndex];
        
        if (node.visible == 0) {
            return;
        }
        
        visibleIndices.push_back(nodeIndex);
        
        if (!node.isLeaf) {
            CollectVisibleNodesRecursive(node.left, visibleIndices);
            CollectVisibleNodesRecursive(node.right, visibleIndices);
        }
    }
};
