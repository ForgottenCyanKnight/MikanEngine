#include "PhysicsManager.h"
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/CompoundShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/Shape/MutableCompoundShape.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/MotionQuality.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/Factory.h>
#include <thread>
#include <cstdarg>
#include <cstdint>
#include <utility>
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <unordered_set>
#include <glm/gtc/quaternion.hpp>
#include "Core/ProjectManager.h"
#include "ModelLoader.h"

namespace Physics {

namespace {

// 碰撞凸包缓存：沿用 BVH 的磁头/版本/计数布局思路，但缓存的是每个凸包的最终顶点。
// 版本号变化或源模型时间戳/大小变化时自动回退到重新生成。
constexpr uint32_t kCollisionCacheMagic = 0x4D4B4343u; // "MKCC"
constexpr uint32_t kCollisionCacheVersion = 1u;
constexpr uint32_t kMaxCollisionCacheHulls = 1000000u;

struct CollisionCacheStamp {
    uint64_t fileSize = 0;
    uint64_t writeTime = 0;
    bool valid = false;
};

struct CollisionHullCacheEntry {
    uint32_t subMeshIndex = 0;
    std::vector<glm::vec3> points;
};

std::filesystem::path ResolveCollisionSourcePath(const std::string& modelPath) {
    const std::string resolved = ProjectManager::GetInstance().ResolveAssetPath(modelPath);
    std::filesystem::path resolvedPath(resolved);
    if (std::filesystem::exists(resolvedPath)) return resolvedPath;

    std::filesystem::path rawPath(modelPath);
    if (std::filesystem::exists(rawPath)) return rawPath;
    return resolvedPath;
}

CollisionCacheStamp GetCollisionCacheStamp(const std::string& modelPath) {
    CollisionCacheStamp stamp;
    const std::filesystem::path sourcePath = ResolveCollisionSourcePath(modelPath);
    std::error_code error;
    const uintmax_t size = std::filesystem::file_size(sourcePath, error);
    if (error) return stamp;

    const auto writeTime = std::filesystem::last_write_time(sourcePath, error);
    if (error) return stamp;

    stamp.fileSize = static_cast<uint64_t>(size);
    stamp.writeTime = static_cast<uint64_t>(writeTime.time_since_epoch().count());
    stamp.valid = true;
    return stamp;
}

std::string GetCollisionCachePath(const std::string& modelPath) {
    const std::filesystem::path sourcePath(modelPath);
    const std::filesystem::path cacheDir = sourcePath.parent_path() / "bvh";
#ifndef __ANDROID__
    std::error_code error;
    std::filesystem::create_directories(cacheDir, error);
#endif
    return (cacheDir / (sourcePath.stem().string() + "_collision.bin")).string();
}

template <typename T>
bool WriteCollisionValue(std::ofstream& file, const T& value) {
    file.write(reinterpret_cast<const char*>(&value), sizeof(T));
    return static_cast<bool>(file);
}

template <typename T>
bool ReadCollisionValue(std::ifstream& file, T& value) {
    file.read(reinterpret_cast<char*>(&value), sizeof(T));
    return static_cast<bool>(file);
}

void AppendUniqueCollisionPoint(std::vector<glm::vec3>& points, const glm::vec3& point) {
    for (const glm::vec3& existing : points) {
        const glm::vec3 delta = existing - point;
        if (glm::dot(delta, delta) <= 1.0e-12f) return;
    }
    points.push_back(point);
}

// 将 submesh 顶点压缩到 Jolt 凸包允许的输入规模。
// 不是按文件顺序截断，而是优先保留外轮廓极值，再用空间网格补足采样点。
std::vector<glm::vec3> ReduceCollisionHullPoints(const std::vector<glm::vec3>& source,
                                                 int maxPoints) {
    std::vector<glm::vec3> finitePoints;
    finitePoints.reserve(source.size());
    for (const glm::vec3& point : source) {
        if (std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z)) {
            finitePoints.push_back(point);
        }
    }

    if (finitePoints.size() <= static_cast<size_t>(maxPoints)) return finitePoints;

    glm::vec3 boundsMin(std::numeric_limits<float>::max());
    glm::vec3 boundsMax(std::numeric_limits<float>::lowest());
    for (const glm::vec3& point : finitePoints) {
        boundsMin = glm::min(boundsMin, point);
        boundsMax = glm::max(boundsMax, point);
    }

    std::vector<glm::vec3> reduced;
    reduced.reserve(static_cast<size_t>(maxPoints));

    const std::array<glm::vec3, 14> directions = {
        glm::vec3(1, 0, 0), glm::vec3(-1, 0, 0),
        glm::vec3(0, 1, 0), glm::vec3(0, -1, 0),
        glm::vec3(0, 0, 1), glm::vec3(0, 0, -1),
        glm::vec3(1, 1, 1), glm::vec3(1, 1, -1),
        glm::vec3(1, -1, 1), glm::vec3(1, -1, -1),
        glm::vec3(-1, 1, 1), glm::vec3(-1, 1, -1),
        glm::vec3(-1, -1, 1), glm::vec3(-1, -1, -1)
    };
    for (const glm::vec3& direction : directions) {
        size_t bestIndex = 0;
        float bestProjection = -std::numeric_limits<float>::max();
        for (size_t i = 0; i < finitePoints.size(); ++i) {
            const float projection = glm::dot(finitePoints[i], direction);
            if (projection > bestProjection) {
                bestProjection = projection;
                bestIndex = i;
            }
        }
        AppendUniqueCollisionPoint(reduced, finitePoints[bestIndex]);
    }

    const glm::vec3 extent = boundsMax - boundsMin;
    const int gridResolution = std::max(
        2, static_cast<int>(std::ceil(std::cbrt(static_cast<double>(maxPoints) * 2.0))));
    std::unordered_set<uint64_t> occupiedCells;
    occupiedCells.reserve(static_cast<size_t>(gridResolution * gridResolution * gridResolution));

    for (const glm::vec3& point : finitePoints) {
        if (reduced.size() >= static_cast<size_t>(maxPoints)) break;

        glm::vec3 normalized(0.0f);
        normalized.x = extent.x > 1.0e-6f ? (point.x - boundsMin.x) / extent.x : 0.0f;
        normalized.y = extent.y > 1.0e-6f ? (point.y - boundsMin.y) / extent.y : 0.0f;
        normalized.z = extent.z > 1.0e-6f ? (point.z - boundsMin.z) / extent.z : 0.0f;
        normalized = glm::clamp(normalized, glm::vec3(0.0f), glm::vec3(1.0f));

        const int x = std::min(gridResolution - 1,
                               static_cast<int>(normalized.x * gridResolution));
        const int y = std::min(gridResolution - 1,
                               static_cast<int>(normalized.y * gridResolution));
        const int z = std::min(gridResolution - 1,
                               static_cast<int>(normalized.z * gridResolution));
        const uint64_t key = static_cast<uint64_t>(x) |
            (static_cast<uint64_t>(y) << 21) |
            (static_cast<uint64_t>(z) << 42);
        if (occupiedCells.insert(key).second) {
            AppendUniqueCollisionPoint(reduced, point);
        }
    }

    if (reduced.size() < static_cast<size_t>(maxPoints)) {
        const size_t stride = std::max<size_t>(1, finitePoints.size() /
            static_cast<size_t>(maxPoints));
        for (size_t i = 0; i < finitePoints.size() && reduced.size() < static_cast<size_t>(maxPoints); i += stride) {
            AppendUniqueCollisionPoint(reduced, finitePoints[i]);
        }
    }

    if (reduced.size() > static_cast<size_t>(maxPoints)) {
        reduced.resize(static_cast<size_t>(maxPoints));
    }
    return reduced;
}

bool CreateCollisionHull(const std::vector<glm::vec3>& points, float precision,
                         JPH::ShapeRefC& outShape,
                         std::vector<glm::vec3>* outFinalHullPoints = nullptr) {
    if (points.size() < 4 || points.size() > static_cast<size_t>(JPH::ConvexHullShape::cMaxPointsInHull)) {
        return false;
    }

    std::vector<JPH::Vec3> joltPoints;
    joltPoints.reserve(points.size());
    for (const glm::vec3& point : points) {
        joltPoints.emplace_back(point.x, point.y, point.z);
    }

    JPH::ConvexHullShapeSettings settings(joltPoints.data(), static_cast<int>(joltPoints.size()));
    settings.mHullTolerance = std::max(0.0f, precision);
    const JPH::ShapeSettings::ShapeResult result = settings.Create();
    if (!result.IsValid()) return false;

    outShape = result.Get();
    if (outFinalHullPoints == nullptr) return true;
    if (outShape->GetSubType() != JPH::EShapeSubType::ConvexHull) return false;

    const auto* hull = static_cast<const JPH::ConvexHullShape*>(outShape.GetPtr());
    const JPH::Vec3 centerOfMass = hull->GetCenterOfMass();
    outFinalHullPoints->clear();
    outFinalHullPoints->reserve(hull->GetNumPoints());
    for (uint32_t i = 0; i < hull->GetNumPoints(); ++i) {
        JPH::Float3 point;
        (hull->GetPoint(i) + centerOfMass).StoreFloat3(&point);
        outFinalHullPoints->emplace_back(point.x, point.y, point.z);
    }
    return !outFinalHullPoints->empty();
}

bool LoadCollisionHullCache(const std::string& modelPath,
                            const CollisionCacheStamp& sourceStamp,
                            float precision, int maxHullVertices,
                            bool generatePerSubmesh,
                            std::vector<CollisionHullCacheEntry>& outEntries) {
    const std::string cachePath = GetCollisionCachePath(modelPath);
    std::ifstream file(cachePath, std::ios::binary);
    if (!file.is_open()) return false;

    uint32_t magic = 0, version = 0, subMeshCount = 0, hullCount = 0;
    uint32_t cachedMaxHullVertices = 0, cachedPerSubmesh = 0;
    uint64_t sourceSize = 0, sourceWriteTime = 0;
    float cachedPrecision = 0.0f;
    if (!ReadCollisionValue(file, magic) || !ReadCollisionValue(file, version) ||
        !ReadCollisionValue(file, sourceSize) || !ReadCollisionValue(file, sourceWriteTime) ||
        !ReadCollisionValue(file, subMeshCount) || !ReadCollisionValue(file, hullCount) ||
        !ReadCollisionValue(file, cachedMaxHullVertices) || !ReadCollisionValue(file, cachedPrecision) ||
        !ReadCollisionValue(file, cachedPerSubmesh)) {
        return false;
    }

    if (magic != kCollisionCacheMagic || version != kCollisionCacheVersion ||
        subMeshCount == 0 || hullCount == 0 || hullCount > kMaxCollisionCacheHulls ||
        cachedMaxHullVertices != static_cast<uint32_t>(maxHullVertices) ||
        cachedPerSubmesh != (generatePerSubmesh ? 1u : 0u) ||
        cachedPrecision != precision ||
        (sourceStamp.valid && (sourceSize != sourceStamp.fileSize ||
                               sourceWriteTime != sourceStamp.writeTime))) {
        std::cout << "[PhysicsManager] Collision cache mismatch, rebuilding: " << cachePath << std::endl;
        return false;
    }

    file.seekg(0, std::ios::end);
    const std::streamoff fileSize = file.tellg();
    file.seekg(44, std::ios::beg); // header: 2*u32 + 2*u64 + 4*u32 + 1*f32
    if (!file || fileSize < 44) return false;

    std::vector<CollisionHullCacheEntry> entries;
    entries.reserve(hullCount);
    for (uint32_t i = 0; i < hullCount; ++i) {
        CollisionHullCacheEntry entry;
        uint32_t pointCount = 0;
        if (!ReadCollisionValue(file, entry.subMeshIndex) || !ReadCollisionValue(file, pointCount) ||
            pointCount < 4 || pointCount > static_cast<uint32_t>(JPH::ConvexHullShape::cMaxPointsInHull)) {
            return false;
        }
        entry.points.resize(pointCount);
        for (glm::vec3& point : entry.points) {
            if (!ReadCollisionValue(file, point.x) || !ReadCollisionValue(file, point.y) ||
                !ReadCollisionValue(file, point.z) ||
                !std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z)) {
                return false;
            }
        }
        entries.push_back(std::move(entry));
    }

    if (file.tellg() != fileSize) return false;
    outEntries = std::move(entries);
    std::cout << "[PhysicsManager] Collision cache loaded: " << cachePath
              << " hulls=" << outEntries.size() << std::endl;
    return true;
}

bool SaveCollisionHullCache(const std::string& modelPath,
                            const CollisionCacheStamp& sourceStamp,
                            uint32_t subMeshCount, float precision,
                            int maxHullVertices, bool generatePerSubmesh,
                            const std::vector<CollisionHullCacheEntry>& entries) {
#ifdef __ANDROID__
    (void)modelPath;
    (void)sourceStamp;
    (void)subMeshCount;
    (void)precision;
    (void)maxHullVertices;
    (void)generatePerSubmesh;
    (void)entries;
    return false;
#else
    const std::string cachePath = GetCollisionCachePath(modelPath);
    std::ofstream file(cachePath, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        std::cerr << "[PhysicsManager] Failed to save collision cache: " << cachePath << std::endl;
        return false;
    }

    const uint32_t hullCount = static_cast<uint32_t>(entries.size());
    const uint32_t cachedMaxHullVertices = static_cast<uint32_t>(maxHullVertices);
    const uint32_t cachedPerSubmesh = generatePerSubmesh ? 1u : 0u;
    if (!WriteCollisionValue(file, kCollisionCacheMagic) ||
        !WriteCollisionValue(file, kCollisionCacheVersion) ||
        !WriteCollisionValue(file, sourceStamp.fileSize) ||
        !WriteCollisionValue(file, sourceStamp.writeTime) ||
        !WriteCollisionValue(file, subMeshCount) ||
        !WriteCollisionValue(file, hullCount) ||
        !WriteCollisionValue(file, cachedMaxHullVertices) ||
        !WriteCollisionValue(file, precision) ||
        !WriteCollisionValue(file, cachedPerSubmesh)) {
        return false;
    }

    for (const CollisionHullCacheEntry& entry : entries) {
        const uint32_t pointCount = static_cast<uint32_t>(entry.points.size());
        if (!WriteCollisionValue(file, entry.subMeshIndex) ||
            !WriteCollisionValue(file, pointCount)) {
            return false;
        }
        for (const glm::vec3& point : entry.points) {
            if (!WriteCollisionValue(file, point.x) || !WriteCollisionValue(file, point.y) ||
                !WriteCollisionValue(file, point.z)) {
                return false;
            }
        }
    }
    file.close();

    std::cout << "[PhysicsManager] Collision cache saved: " << cachePath
              << " hulls=" << entries.size() << std::endl;
    return true;
#endif
}

bool CreateCompoundFromChildShapes(const std::vector<CollisionHullCacheEntry>& entries,
                                   const std::vector<JPH::ShapeRefC>& childShapes,
                                   PhysicsManager::RigidBodyInfo::Type bodyType,
                                   JPH::ShapeRefC& outShape) {
    if (entries.empty() || entries.size() != childShapes.size()) return false;
    if (childShapes.size() == 1) {
        outShape = childShapes.front();
        return true;
    }

    if (bodyType == PhysicsManager::RigidBodyInfo::Type::Static) {
        JPH::StaticCompoundShapeSettings settings;
        for (size_t i = 0; i < childShapes.size(); ++i) {
            settings.AddShape(JPH::Vec3::sZero(), JPH::Quat::sIdentity(),
                              childShapes[i].GetPtr(), entries[i].subMeshIndex);
        }
        const JPH::ShapeSettings::ShapeResult result = settings.Create();
        if (!result.IsValid()) return false;
        outShape = result.Get();
        return true;
    }

    JPH::MutableCompoundShapeSettings settings;
    for (size_t i = 0; i < childShapes.size(); ++i) {
        settings.AddShape(JPH::Vec3::sZero(), JPH::Quat::sIdentity(),
                          childShapes[i].GetPtr(), entries[i].subMeshIndex);
    }
    const JPH::ShapeSettings::ShapeResult result = settings.Create();
    if (!result.IsValid()) return false;
    outShape = result.Get();
    return true;
}

bool CreateCompoundCollisionShape(const std::vector<CollisionHullCacheEntry>& entries,
                                  PhysicsManager::RigidBodyInfo::Type bodyType,
                                  float precision,
                                  JPH::ShapeRefC& outShape) {
    if (entries.empty()) return false;

    std::vector<JPH::ShapeRefC> childShapes;
    childShapes.reserve(entries.size());
    for (const CollisionHullCacheEntry& entry : entries) {
        JPH::ShapeRefC childShape;
        if (!CreateCollisionHull(entry.points, precision, childShape)) return false;
        childShapes.push_back(childShape);
    }
    return CreateCompoundFromChildShapes(entries, childShapes, bodyType, outShape);
}

bool BuildCollisionShapeFromModel(const MeshData& meshData,
                                  const PhysicsManager::RigidBodyInfo& info,
                                  int maxHullVertices,
                                  std::vector<CollisionHullCacheEntry>& outEntries,
                                  JPH::ShapeRefC& outShape) {
    outEntries.clear();
    std::vector<JPH::ShapeRefC> childShapes;
    const float precision = std::max(0.0f, info.collisionPrecision);

    if (info.generatePerSubmesh) {
        outEntries.reserve(meshData.subMeshes.size());
        childShapes.reserve(meshData.subMeshes.size());
        for (size_t subMeshIndex = 0; subMeshIndex < meshData.subMeshes.size(); ++subMeshIndex) {
            std::vector<glm::vec3> sourcePoints;
            sourcePoints.reserve(meshData.subMeshes[subMeshIndex].vertices.size());
            for (const Vertex& vertex : meshData.subMeshes[subMeshIndex].vertices) {
                sourcePoints.push_back(vertex.Position);
            }

            const std::vector<glm::vec3> reducedPoints =
                ReduceCollisionHullPoints(sourcePoints, maxHullVertices);
            CollisionHullCacheEntry entry;
            entry.subMeshIndex = static_cast<uint32_t>(subMeshIndex);
            JPH::ShapeRefC childShape;
            if (CreateCollisionHull(reducedPoints, precision, childShape, &entry.points)) {
                outEntries.push_back(std::move(entry));
                childShapes.push_back(childShape);
            }
        }
    } else {
        std::vector<glm::vec3> sourcePoints;
        size_t totalVertexCount = 0;
        for (const auto& subMesh : meshData.subMeshes) totalVertexCount += subMesh.vertices.size();
        sourcePoints.reserve(totalVertexCount);
        for (const auto& subMesh : meshData.subMeshes) {
            for (const Vertex& vertex : subMesh.vertices) sourcePoints.push_back(vertex.Position);
        }

        CollisionHullCacheEntry entry;
        entry.subMeshIndex = 0;
        const std::vector<glm::vec3> reducedPoints =
            ReduceCollisionHullPoints(sourcePoints, maxHullVertices);
        JPH::ShapeRefC childShape;
        if (CreateCollisionHull(reducedPoints, precision, childShape, &entry.points)) {
            outEntries.push_back(std::move(entry));
            childShapes.push_back(childShape);
        }
    }

    if (outEntries.empty()) return false;
    return CreateCompoundFromChildShapes(outEntries, childShapes, info.type, outShape);
}

bool LoadOrBuildCollisionShape(const PhysicsManager::RigidBodyInfo& info,
                               JPH::ShapeRefC& outShape) {
    const int maxHullVertices = std::clamp(
        info.maxConvexHullVertices, 4, JPH::ConvexHullShape::cMaxPointsInHull);
    const CollisionCacheStamp sourceStamp = GetCollisionCacheStamp(info.modelPath);
    std::vector<CollisionHullCacheEntry> entries;

    if (LoadCollisionHullCache(info.modelPath, sourceStamp,
                               info.collisionPrecision, maxHullVertices,
                               info.generatePerSubmesh, entries) &&
        CreateCompoundCollisionShape(entries, info.type,
                                     std::max(0.0f, info.collisionPrecision), outShape)) {
        return true;
    }

    std::cout << "[PhysicsManager] Collision cache miss, generating from model: "
              << info.modelPath << std::endl;
    const ModelLoadResult result = ModelLoader::LoadModelWithTextures(info.modelPath);
    if (!BuildCollisionShapeFromModel(result.meshData, info, maxHullVertices, entries, outShape)) {
        std::cerr << "[PhysicsManager] Failed to generate convex collision: "
                  << info.modelPath << std::endl;
        return false;
    }

    SaveCollisionHullCache(info.modelPath, sourceStamp,
                           static_cast<uint32_t>(result.meshData.subMeshes.size()),
                           info.collisionPrecision, maxHullVertices,
                           info.generatePerSubmesh, entries);
    std::cout << "[PhysicsManager] Convex collision generated: " << info.modelPath
              << " hulls=" << entries.size() << std::endl;
    return true;
}

} // namespace

// JoltPhysics trace callback function
void JoltTrace(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

// 自定义 ObjectLayerPairFilter - 优化：减少不必要的碰撞检测
class ObjectLayerPairFilterImpl : public JPH::ObjectLayerPairFilter {
public:
    virtual bool ShouldCollide(JPH::ObjectLayer inObject1, JPH::ObjectLayer inObject2) const override {
        // 使用 PhysicsManager 中定义的碰撞层常量
        // 触发器不与触发器碰撞
        if (inObject1 == Physics::PhysicsManager::TRIGGER && inObject2 == Physics::PhysicsManager::TRIGGER) return false;
        
        // 静态物体之间不碰撞
        if (inObject1 == Physics::PhysicsManager::NON_MOVING && inObject2 == Physics::PhysicsManager::NON_MOVING) return false;
        
        // 其他情况都可以碰撞
        return true;
    }
};

// 自定义 BroadPhaseLayerInterface - 优化：支持 3 个碰撞层
class BroadPhaseLayerInterfaceImpl : public JPH::BroadPhaseLayerInterface {
public:
    virtual JPH::uint GetNumBroadPhaseLayers() const override {
        return 3; // 支持 NON_MOVING, MOVING, TRIGGER
    }
    
    virtual JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const override {
        // 将 ObjectLayer 映射到 BroadPhaseLayer
        return JPH::BroadPhaseLayer(inLayer);
    }
    
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    virtual const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer inLayer) const override {
        // 使用 if-else 替代 switch，因为 BroadPhaseLayer 不能用于 switch
        if (inLayer == JPH::BroadPhaseLayer(0)) return "NON_MOVING";
        else if (inLayer == JPH::BroadPhaseLayer(1)) return "MOVING";
        else if (inLayer == JPH::BroadPhaseLayer(2)) return "TRIGGER";
        else return "UNKNOWN";
    }
#endif
};

// 自定义ObjectVsBroadPhaseLayerFilter
class ObjectVsBroadPhaseLayerFilterImpl : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    virtual bool ShouldCollide(JPH::ObjectLayer inLayer1, JPH::BroadPhaseLayer inLayer2) const override {
        // 简单的逻辑：所有对象层都可以与所有BroadPhase层碰撞
        return true;
    }
};

PhysicsManager::PhysicsManager() : 
    physicsSystem(nullptr),
    jobSystem(nullptr),
    tempAllocator(nullptr),
    broadPhaseLayerInterface(nullptr),
    objectVsBroadPhaseLayerFilter(nullptr),
    objectLayerPairFilter(nullptr)
{
}

PhysicsManager::~PhysicsManager() {
    Shutdown();
}

// 碰撞回调实现
class JoltCollisionListener : public JPH::ContactListener {
private:
    Physics::PhysicsManager* physicsManager;
public:
    JoltCollisionListener(Physics::PhysicsManager* manager) : physicsManager(manager) {}
    
    virtual JPH::ValidateResult OnContactValidate(const JPH::Body& body1, const JPH::Body& body2, JPH::RVec3Arg baseOffset, const JPH::CollideShapeResult& collisionResult) override {
        return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
    }
    
    virtual void OnContactAdded(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold, JPH::ContactSettings& settings) override {
        // 简化处理，暂时不实现具体的碰撞回调
    }
    
    virtual void OnContactPersisted(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold, JPH::ContactSettings& settings) override {
        // 简化处理，暂时不实现具体的碰撞回调
    }
    
    virtual void OnContactRemoved(const JPH::SubShapeIDPair& subShapePair) override {
        // 简化处理，暂时不实现具体的碰撞回调
    }
};

void PhysicsManager::Initialize() {
    // 初始化JoltPhysics
    JPH::RegisterDefaultAllocator();
    
    // 设置Trace回调函数
    JPH::Trace = Physics::JoltTrace;
    
    // 创建Factory
    if (JPH::Factory::sInstance == nullptr) {
        JPH::Factory::sInstance = new JPH::Factory();
    }
    
    // 注册所有类型
    JPH::RegisterTypes();
    
    // 创建临时分配器
    tempAllocator = new JPH::TempAllocatorImpl(10 * 1024 * 1024);
    
    // 创建作业系统线程池 - 优化：使用所有可用的 CPU 核心
    uint32_t num_threads = std::max(1u, std::thread::hardware_concurrency());
    jobSystem = new JPH::JobSystemThreadPool(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, num_threads);
    
    // 创建碰撞层接口（作为成员变量，确保在 PhysicsSystem 生命周期内保持有效）
    broadPhaseLayerInterface = new BroadPhaseLayerInterfaceImpl();
    objectVsBroadPhaseLayerFilter = new ObjectVsBroadPhaseLayerFilterImpl();
    objectLayerPairFilter = new ObjectLayerPairFilterImpl();
    
    // 创建物理系统
    physicsSystem = new JPH::PhysicsSystem();
    
    // 初始化物理系统 - 优化：减少 maxBodyPairs 和 maxContactConstraints 以提升性能
    // 参数：maxBodies, numBodyMutexes, maxBodyPairs, maxContactConstraints, broadPhaseLayerInterface, objectVsBroadPhaseLayerFilter, objectLayerPairFilter
    physicsSystem->Init(1024, 0, 512, 512, 
                        *broadPhaseLayerInterface,
                        *objectVsBroadPhaseLayerFilter,
                        *objectLayerPairFilter);
    
    // 设置碰撞监听器
    physicsSystem->SetBodyActivationListener(nullptr);
    physicsSystem->SetContactListener(nullptr);
    
    // 设置重力（向下为负 Y 轴）
    physicsSystem->SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));
    
    printf("[PhysicsManager] Initialized successfully (%d threads)\n", num_threads);
}

void PhysicsManager::Shutdown() {
    if (physicsSystem) {
        delete physicsSystem;
        physicsSystem = nullptr;
    }

    persistentStaticBodies.clear();
    
    // 删除碰撞层接口
    delete broadPhaseLayerInterface;
    broadPhaseLayerInterface = nullptr;
    
    delete objectVsBroadPhaseLayerFilter;
    objectVsBroadPhaseLayerFilter = nullptr;
    
    delete objectLayerPairFilter;
    objectLayerPairFilter = nullptr;
    
    delete jobSystem;
    jobSystem = nullptr;
    
    delete tempAllocator;
    tempAllocator = nullptr;
    
    if (JPH::Factory::sInstance) {
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }
}

void PhysicsManager::Update(float deltaTime) {
    if (!physicsSystem) return;
    
    // 优化：使用固定时间步长进行物理更新，提升稳定性和性能
    const float fixedDeltaTime = 1.0f / 60.0f; // 60Hz 固定步长
    const int collisionSteps = 1;
    
    // 累积时间
    static float timeAccumulator = 0.0f;
    timeAccumulator += deltaTime;
    
    // 限制最大累积时间，避免螺旋死亡（spiral of death）
    const float maxAccumulatorTime = 0.25f; // 最多累积 0.25 秒
    if (timeAccumulator > maxAccumulatorTime) {
        timeAccumulator = maxAccumulatorTime;
    }
    
    // 多次执行固定步长的物理更新
    while (timeAccumulator >= fixedDeltaTime) {
        physicsSystem->Update(fixedDeltaTime, collisionSteps, tempAllocator, jobSystem);
        timeAccumulator -= fixedDeltaTime;
    }
}

void PhysicsManager::Update(float deltaTime, const glm::vec3& cameraPos) {
    // 先执行物理更新
    Update(deltaTime);
    
    // 清理远距离刚体
    CleanupDistantBodies(cameraPos);
}

JPH::BodyID PhysicsManager::CreateRigidBody(const RigidBodyInfo& info) {
    if (!physicsSystem) return JPH::BodyID();
    
    // 创建碰撞形状
    JPH::ShapeRefC shape;
    switch (info.shapeType) {
    case RigidBodyInfo::ShapeType::Box:
        shape = new JPH::BoxShape(JPH::Vec3(info.size.x * 0.5f, info.size.y * 0.5f, info.size.z * 0.5f));
        break;
    case RigidBodyInfo::ShapeType::Sphere:
        shape = new JPH::SphereShape(info.size.x * 0.5f);
        break;
    case RigidBodyInfo::ShapeType::Capsule:
        shape = new JPH::CapsuleShape(info.size.y * 0.5f, info.size.x * 0.5f);
        break;
    case RigidBodyInfo::ShapeType::OBB: {
        // 创建 OBB（有向包围盒）- 直接使用 BoxShape，旋转由刚体控制
        // 注意：不使用 RotatedTranslatedShape，避免双重旋转
        JPH::BoxShape* boxShape = new JPH::BoxShape(JPH::Vec3(info.size.x * 0.5f, info.size.y * 0.5f, info.size.z * 0.5f));
        shape = boxShape;
        break;
    }
    case RigidBodyInfo::ShapeType::Mesh: {
        if (info.modelPath.empty()) {
            printf("[PhysicsManager] Mesh collision skipped: model path is empty\n");
            return JPH::BodyID();
        }

        // Jolt 的 MeshShape 只能作为静态几何使用。动态/运动学 Mesh
        // 自动退化为凸包；静态 Mesh 只有显式启用 useConvexHull 时才走
        // 凸包/缓存路径，这样关闭凸包就能保留桥洞、门洞等凹形空间。
        // generatePerSubmesh 只在凸包路径中生效，不能覆盖 useConvexHull=false。
        const bool buildConvexHull =
            info.type != RigidBodyInfo::Type::Static || info.useConvexHull;
        if (buildConvexHull) {
            if (info.type != RigidBodyInfo::Type::Static && !info.useConvexHull) {
                printf("[PhysicsManager] Mesh collision for dynamic/kinematic body uses convex hull: %s\n",
                       info.modelPath.c_str());
            }
            LoadOrBuildCollisionShape(info, shape);
        } else {
            printf("[PhysicsManager] Static mesh collision uses original triangles (holes preserved): %s\n",
                   info.modelPath.c_str());
            const ModelLoadResult result = ModelLoader::LoadModelWithTextures(info.modelPath);
            // 保留模型的真实三角面，不再用 AABB/盒子近似。
            JPH::VertexList meshVertices;
            JPH::IndexedTriangleList meshTriangles;
            size_t vertexCount = 0;
            size_t triangleCount = 0;
            for (const auto& subMesh : result.meshData.subMeshes) {
                vertexCount += subMesh.vertices.size();
                triangleCount += subMesh.indices.size() / 3;
            }
            meshVertices.reserve(vertexCount);
            meshTriangles.reserve(triangleCount);

            for (const auto& subMesh : result.meshData.subMeshes) {
                const uint32_t baseVertex = static_cast<uint32_t>(meshVertices.size());
                for (const auto& vertex : subMesh.vertices) {
                    meshVertices.push_back(JPH::Float3{
                        vertex.Position.x, vertex.Position.y, vertex.Position.z});
                }
                for (size_t i = 0; i + 2 < subMesh.indices.size(); i += 3) {
                    const uint32_t i0 = subMesh.indices[i];
                    const uint32_t i1 = subMesh.indices[i + 1];
                    const uint32_t i2 = subMesh.indices[i + 2];
                    if (i0 >= subMesh.vertices.size() ||
                        i1 >= subMesh.vertices.size() ||
                        i2 >= subMesh.vertices.size()) {
                        continue;
                    }
                    meshTriangles.emplace_back(
                        baseVertex + i0, baseVertex + i1, baseVertex + i2, 0);
                }
            }

            if (!meshVertices.empty() && !meshTriangles.empty()) {
                JPH::MeshShapeSettings settings(
                    std::move(meshVertices), std::move(meshTriangles));
                settings.mMaxTrianglesPerLeaf = 8;
                JPH::ShapeSettings::ShapeResult shapeResult = settings.Create();
                if (shapeResult.IsValid()) {
                    shape = shapeResult.Get();
                    printf("[PhysicsManager] Mesh collision created: %s vertices=%zu triangles=%zu\n",
                           info.modelPath.c_str(), vertexCount, triangleCount);
                } else {
                    printf("[PhysicsManager] Mesh collision build failed: %s (%s)\n",
                           info.modelPath.c_str(), shapeResult.GetError().c_str());
                }
            } else {
                printf("[PhysicsManager] Mesh collision has no valid triangles: %s\n",
                       info.modelPath.c_str());
            }
        }

        if (!shape) {
            printf("[PhysicsManager] Mesh rigid body not created: %s\n",
                   info.modelPath.c_str());
            return JPH::BodyID();
        }
        break;
    }
    default:
        shape = new JPH::BoxShape(JPH::Vec3(0.5f, 0.5f, 0.5f));
        break;
    }
    
    // 设置刚体属性
    JPH::EMotionType motionType = info.type == RigidBodyInfo::Type::Static ? JPH::EMotionType::Static :
                                  info.type == RigidBodyInfo::Type::Kinematic ? JPH::EMotionType::Kinematic :
                                  JPH::EMotionType::Dynamic;
    
    // 优化：使用正确的碰撞层
    JPH::ObjectLayer objectLayer = info.type == RigidBodyInfo::Type::Static ? NON_MOVING : 
                                   info.isTrigger ? TRIGGER : MOVING;
    
    // 计算旋转四元数
    glm::quat rotation;
    if (info.shapeType == RigidBodyInfo::ShapeType::OBB) {
        // OBB 模式：直接使用 orientation 作为旋转
        rotation = info.orientation;
    } else {
        // 其他模式：使用欧拉角
        rotation = glm::quat(glm::radians(info.rotation));
    }
    
    // 创建 BodyCreationSettings
    JPH::BodyCreationSettings settings(shape, 
                                       JPH::RVec3(info.position.x, info.position.y, info.position.z), 
                                       JPH::Quat(rotation.x, rotation.y, rotation.z, rotation.w), 
                                       motionType, 
                                       objectLayer);
    
    settings.mIsSensor = info.isTrigger;

    // 胶囊体用于角色/敌人时只允许平移，不允许碰撞冲量把角色翻滚。
    // 角色仍可通过 SetRigidBodyOrientation 显式改变朝向；这里仅限制物理
    // 求解器产生的旋转，避免死亡动画结束后胶囊体旋入地面/墙角造成穿模。
    if (motionType == JPH::EMotionType::Dynamic &&
        info.shapeType == RigidBodyInfo::ShapeType::Capsule) {
        settings.mAllowedDOFs = JPH::EAllowedDOFs::TranslationX |
                                JPH::EAllowedDOFs::TranslationY |
                                JPH::EAllowedDOFs::TranslationZ;
    }
    
    // 动态刚体使用连续线性碰撞，避免小/高速物体在离散步进中跨过薄碰撞体。
    // 静态和运动学体保持离散模式；它们的位置由场景/脚本控制，不应走动态 CCD 路径。
    settings.mMotionQuality = motionType == JPH::EMotionType::Dynamic
        ? JPH::EMotionQuality::LinearCast
        : JPH::EMotionQuality::Discrete;
    
    // 优化：启用刚体休眠以减少不必要的计算（Kinematic 不允许休眠）
    if (motionType != JPH::EMotionType::Kinematic) {
        settings.mAllowSleeping = true;
    } else {
        settings.mAllowSleeping = false;  // Kinematic 刚体不能休眠
    }
    
    // 设置摩擦力和弹性
    settings.mFriction = 0.5f; // 摩擦系数
    settings.mRestitution = info.restitution; // 弹性系数
    
    // 设置重力因子（0=不受重力影响，1=正常重力）
    settings.mGravityFactor = info.useGravity ? 1.0f : 0.0f;
    
    // 设置质量属性
    if (motionType == JPH::EMotionType::Dynamic) {
        // 动态刚体：设置质量
        settings.mMassPropertiesOverride.mMass = info.mass;
        settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
    } else if (motionType == JPH::EMotionType::Kinematic) {
        // 运动学刚体仍由 Transform 驱动，但碰撞响应使用场景配置的质量。
        // 之前这里固定为 1000，导致编辑器/场景中的 mass 字段对运动学方块无效。
        const float kinematicMass = info.mass > 0.0f ? info.mass : 1.0f;
        settings.mMassPropertiesOverride.mMass = kinematicMass;
        settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
    }
    
    // 创建并添加刚体
    JPH::BodyInterface& bodyInterface = physicsSystem->GetBodyInterface();
    JPH::BodyID bodyID = bodyInterface.CreateAndAddBody(settings, JPH::EActivation::Activate);

    // 静态体创建后局部唤醒附近动态体:新出现的静态体若支撑着睡眠动态体,接触检测需要重新建立
    if (motionType == JPH::EMotionType::Static) {
        WakeBodiesNear(bodyID);
    }

    return bodyID;
}

JPH::BodyID PhysicsManager::CreateStaticMeshBody(
    const std::vector<glm::vec3>& vertices,
    const std::vector<uint32_t>& indices) {
    if (!physicsSystem) return JPH::BodyID();
    if (vertices.empty() || indices.size() < 3) {
        printf("[PhysicsManager] Static mesh collision skipped: empty mesh\n");
        return JPH::BodyID();
    }

    JPH::VertexList meshVertices;
    meshVertices.reserve(vertices.size());
    for (const glm::vec3& vertex : vertices) {
        if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y) || !std::isfinite(vertex.z)) {
            printf("[PhysicsManager] Static mesh collision skipped: non-finite vertex\n");
            return JPH::BodyID();
        }
        meshVertices.push_back(JPH::Float3{vertex.x, vertex.y, vertex.z});
    }

    JPH::IndexedTriangleList meshTriangles;
    meshTriangles.reserve(indices.size() / 3);
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const uint32_t i0 = indices[i];
        const uint32_t i1 = indices[i + 1];
        const uint32_t i2 = indices[i + 2];
        if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) {
            continue;
        }
        meshTriangles.emplace_back(i0, i1, i2, 0);
    }

    if (meshTriangles.empty()) {
        printf("[PhysicsManager] Static mesh collision skipped: no valid triangles\n");
        return JPH::BodyID();
    }

    JPH::MeshShapeSettings shapeSettings(std::move(meshVertices), std::move(meshTriangles));
    shapeSettings.mMaxTrianglesPerLeaf = 8;
    JPH::ShapeSettings::ShapeResult shapeResult = shapeSettings.Create();
    if (!shapeResult.IsValid()) {
        printf("[PhysicsManager] Static mesh collision build failed: %s\n",
               shapeResult.GetError().c_str());
        return JPH::BodyID();
    }

    JPH::BodyCreationSettings bodySettings(
        shapeResult.Get(),
        JPH::RVec3(0.0f, 0.0f, 0.0f),
        JPH::Quat(0.0f, 0.0f, 0.0f, 1.0f),
        JPH::EMotionType::Static,
        NON_MOVING);
    bodySettings.mMotionQuality = JPH::EMotionQuality::Discrete;
    bodySettings.mAllowSleeping = true;
    bodySettings.mIsSensor = false;
    bodySettings.mFriction = 0.8f;
    bodySettings.mRestitution = 0.0f;
    bodySettings.mGravityFactor = 0.0f;

    JPH::BodyInterface& bodyInterface = physicsSystem->GetBodyInterface();
    const JPH::BodyID bodyID = bodyInterface.CreateAndAddBody(
        bodySettings, JPH::EActivation::Activate);
    if (bodyID.IsInvalid()) {
        printf("[PhysicsManager] Static mesh collision body creation failed\n");
        return bodyID;
    }

    persistentStaticBodies.push_back(bodyID);
    WakeBodiesNear(bodyID);
    printf("[PhysicsManager] Persistent static mesh collision created: vertices=%zu triangles=%zu\n",
           vertices.size(), indices.size() / 3);
    return bodyID;
}

void PhysicsManager::RemoveRigidBody(JPH::BodyID bodyID) {
    if (bodyID.IsInvalid()) return;

    persistentStaticBodies.erase(
        std::remove(persistentStaticBodies.begin(), persistentStaticBodies.end(), bodyID),
        persistentStaticBodies.end());

    if (!physicsSystem) return;
    JPH::BodyInterface& bodyInterface = physicsSystem->GetBodyInterface();
    if (!bodyInterface.IsAdded(bodyID)) return;
    bodyInterface.RemoveBody(bodyID);
    bodyInterface.DestroyBody(bodyID);
}

JPH::Body* PhysicsManager::GetRigidBody(JPH::BodyID bodyID) {
    if (!physicsSystem) return nullptr;
    // 注意：JoltPhysics不直接通过BodyInterface获取Body指针
    // 需要使用BodyLock来安全访问
    return nullptr;
}

void PhysicsManager::SetRigidBodyPosition(JPH::BodyID bodyID, const glm::vec3& position) {
    if (physicsSystem && !bodyID.IsInvalid()) {
        JPH::BodyInterface& bodyInterface = physicsSystem->GetBodyInterface();
        
        // 激活刚体（特别是 Kinematic 刚体）
        bodyInterface.ActivateBody(bodyID);
        
        // 设置位置
        bodyInterface.SetPosition(bodyID, JPH::RVec3(position.x, position.y, position.z), JPH::EActivation::Activate);

        // 移动静态体会使原本被它支撑的睡眠动态体失去接触而不自知(悬浮),局部唤醒附近动态体
        if (bodyInterface.GetMotionType(bodyID) == JPH::EMotionType::Static) {
            WakeBodiesNear(bodyID);
        }
    }
}

void PhysicsManager::SetRigidBodyRotation(JPH::BodyID bodyID, const glm::vec3& eulerAngles) {
    if (physicsSystem && !bodyID.IsInvalid()) {
        glm::quat rotation = glm::quat(glm::radians(eulerAngles));
        JPH::BodyInterface& bodyInterface = physicsSystem->GetBodyInterface();
        
        // 激活刚体（特别是 Kinematic 刚体）
        bodyInterface.ActivateBody(bodyID);
        
        // 设置旋转
        bodyInterface.SetRotation(bodyID, JPH::Quat(rotation.x, rotation.y, rotation.z, rotation.w), JPH::EActivation::Activate);

        // 旋转静态体同样可能改变支撑关系,局部唤醒附近动态体
        if (bodyInterface.GetMotionType(bodyID) == JPH::EMotionType::Static) {
            WakeBodiesNear(bodyID);
        }
    }
}

void PhysicsManager::SetRigidBodyOrientation(JPH::BodyID bodyID, const glm::quat& orientation) {
    if (physicsSystem && !bodyID.IsInvalid()) {
        JPH::BodyInterface& bodyInterface = physicsSystem->GetBodyInterface();
        
        // 激活刚体（特别是 Kinematic 刚体）
        bodyInterface.ActivateBody(bodyID);
        
        // Jolt 的 Quat 构造函数参数顺序是 (x, y, z, w)
        bodyInterface.SetRotation(bodyID, JPH::Quat(orientation.x, orientation.y, orientation.z, orientation.w), JPH::EActivation::Activate);

        // 旋转静态体可能改变支撑关系,局部唤醒附近动态体
        if (bodyInterface.GetMotionType(bodyID) == JPH::EMotionType::Static) {
            WakeBodiesNear(bodyID);
        }
    }
}

JPH::BodyID PhysicsManager::SetRigidBodyScale(JPH::BodyID bodyID, const glm::vec3& scale, bool isOBB) {
    if (!physicsSystem || bodyID.IsInvalid()) {
        return bodyID;
    }
    
    JPH::BodyInterface& bodyInterface = physicsSystem->GetBodyInterface();
    
    // 检查刚体是否存在
    if (!bodyInterface.IsAdded(bodyID)) {
        return bodyID;
    }
    
    // 获取当前的 Shape
    const JPH::Shape* currentShape = bodyInterface.GetShape(bodyID);
    if (!currentShape) {
        return bodyID;
    }
    
    // 获取当前的位置和旋转
    JPH::RVec3 position = bodyInterface.GetPosition(bodyID);
    JPH::Quat rotation = bodyInterface.GetRotation(bodyID);
    
    // 获取运动类型和对象层
    JPH::EMotionType motionType = bodyInterface.GetMotionType(bodyID);
    JPH::ObjectLayer objectLayer = bodyInterface.GetObjectLayer(bodyID);
    
    // 获取当前是否为传感器
    bool isSensor = bodyInterface.IsSensor(bodyID);
    
    // 检查当前 Shape 是否已经是 ScaledShape
    const JPH::ScaledShape* scaledShape = dynamic_cast<const JPH::ScaledShape*>(currentShape);
    const JPH::Shape* innerShape = scaledShape ? scaledShape->GetInnerShape() : currentShape;
    
    // 创建新的 ScaledShape（如果缩放不是单位缩放）
    JPH::ShapeRefC newShape;
    const glm::vec3 absoluteScale = glm::abs(scale);
    if (absoluteScale != glm::vec3(1.0f)) {
        JPH::Vec3 joltScale(absoluteScale.x, absoluteScale.y, absoluteScale.z);
        newShape = new JPH::ScaledShape(innerShape, joltScale);
    } else {
        // 如果缩放是单位缩放，直接使用内部 Shape
        newShape = innerShape;
    }
    
    // 更新刚体的 Shape（不重建刚体）
    bodyInterface.SetShape(bodyID, newShape, true, JPH::EActivation::Activate);

    // 静态体形状/缩放变化可能改变支撑关系,局部唤醒附近动态体
    if (motionType == JPH::EMotionType::Static) {
        WakeBodiesNear(bodyID);
    }
    
    // 返回原来的 BodyID（不需要改变）
    return bodyID;
}

glm::vec3 PhysicsManager::GetRigidBodyPosition(JPH::BodyID bodyID) const {
    if (physicsSystem && !bodyID.IsInvalid()) {
        JPH::BodyInterface& bodyInterface = const_cast<JPH::PhysicsSystem*>(physicsSystem)->GetBodyInterface();
        JPH::RVec3 pos = bodyInterface.GetPosition(bodyID);
        return glm::vec3(pos.GetX(), pos.GetY(), pos.GetZ());
    }
    return glm::vec3(0.0f);
}

glm::vec3 PhysicsManager::GetRigidBodyRotation(JPH::BodyID bodyID) const {
    if (physicsSystem && !bodyID.IsInvalid()) {
        JPH::BodyInterface& bodyInterface = const_cast<JPH::PhysicsSystem*>(physicsSystem)->GetBodyInterface();
        JPH::Quat rot = bodyInterface.GetRotation(bodyID);
        glm::quat glmRot(rot.GetW(), rot.GetX(), rot.GetY(), rot.GetZ());
        return glm::degrees(glm::eulerAngles(glmRot));
    }
    return glm::vec3(0.0f);
}

void PhysicsManager::ApplyForce(JPH::BodyID bodyID, const glm::vec3& force) {
    if (physicsSystem && !bodyID.IsInvalid()) {
        JPH::BodyInterface& bodyInterface = physicsSystem->GetBodyInterface();
        bodyInterface.AddForce(bodyID, JPH::Vec3(force.x, force.y, force.z));
    }
}

void PhysicsManager::ApplyImpulse(JPH::BodyID bodyID, const glm::vec3& impulse) {
    if (physicsSystem && !bodyID.IsInvalid()) {
        JPH::BodyInterface& bodyInterface = physicsSystem->GetBodyInterface();
        bodyInterface.AddImpulse(bodyID, JPH::Vec3(impulse.x, impulse.y, impulse.z));
    }
}

void PhysicsManager::SetLinearVelocity(JPH::BodyID bodyID, const glm::vec3& velocity) {
    if (physicsSystem && !bodyID.IsInvalid()) {
        JPH::BodyInterface& bodyInterface = physicsSystem->GetBodyInterface();
        bodyInterface.SetLinearVelocity(bodyID, JPH::Vec3(velocity.x, velocity.y, velocity.z));
    }
}

glm::vec3 PhysicsManager::GetLinearVelocity(JPH::BodyID bodyID) const {
    if (physicsSystem && !bodyID.IsInvalid()) {
        JPH::BodyInterface& bodyInterface = const_cast<JPH::PhysicsSystem*>(physicsSystem)->GetBodyInterface();
        JPH::Vec3 vel = bodyInterface.GetLinearVelocity(bodyID);
        return glm::vec3(vel.GetX(), vel.GetY(), vel.GetZ());
    }
    return glm::vec3(0.0f);
}

void PhysicsManager::SetRestitution(JPH::BodyID bodyID, float restitution) {
    if (physicsSystem && !bodyID.IsInvalid()) {
        JPH::BodyInterface& bodyInterface = physicsSystem->GetBodyInterface();
        bodyInterface.SetRestitution(bodyID, restitution);
    }
}

float PhysicsManager::GetRestitution(JPH::BodyID bodyID) const {
    if (physicsSystem && !bodyID.IsInvalid()) {
        JPH::BodyInterface& bodyInterface = const_cast<JPH::PhysicsSystem*>(physicsSystem)->GetBodyInterface();
        return bodyInterface.GetRestitution(bodyID);
    }
    return 0.0f;
}

void PhysicsManager::SetRigidBodyGravityFactor(JPH::BodyID bodyID, float factor) {
    if (physicsSystem && !bodyID.IsInvalid()) {
        JPH::BodyInterface& bodyInterface = physicsSystem->GetBodyInterface();
        bodyInterface.SetGravityFactor(bodyID, factor);
    }
}

bool PhysicsManager::IsRigidBodyValid(JPH::BodyID bodyID) const {
    if (!physicsSystem || bodyID.IsInvalid()) return false;
    JPH::BodyInterface& bodyInterface = const_cast<JPH::PhysicsSystem*>(physicsSystem)->GetBodyInterface();
    return bodyInterface.IsAdded(bodyID);
}

bool PhysicsManager::QueryOrientedBox(const glm::vec3& center,
                                      const glm::quat& rotation,
                                      const glm::vec3& halfExtents,
                                      std::vector<JPH::BodyID>& outBodyIDs,
                                      JPH::BodyID ignoreBodyID) const {
    outBodyIDs.clear();
    if (!physicsSystem) return false;

    const bool finiteCenter = std::isfinite(center.x) &&
                              std::isfinite(center.y) &&
                              std::isfinite(center.z);
    const bool finiteExtents = std::isfinite(halfExtents.x) &&
                               std::isfinite(halfExtents.y) &&
                               std::isfinite(halfExtents.z);
    const bool finiteRotation = std::isfinite(rotation.x) &&
                                std::isfinite(rotation.y) &&
                                std::isfinite(rotation.z) &&
                                std::isfinite(rotation.w);
    if (!finiteCenter || !finiteExtents || !finiteRotation ||
        halfExtents.x <= 0.0f || halfExtents.y <= 0.0f || halfExtents.z <= 0.0f) {
        return false;
    }

    const float rotationLengthSquared = rotation.x * rotation.x +
                                        rotation.y * rotation.y +
                                        rotation.z * rotation.z +
                                        rotation.w * rotation.w;
    if (!std::isfinite(rotationLengthSquared) || rotationLengthSquared < 1e-8f) {
        return false;
    }

    const glm::quat normalizedRotation = glm::normalize(rotation);
    const JPH::BoxShape queryShape(JPH::Vec3(halfExtents.x,
                                             halfExtents.y,
                                             halfExtents.z),
                                   0.0f);
    const JPH::RMat44 queryTransform = JPH::RMat44::sRotationTranslation(
        JPH::Quat(normalizedRotation.x,
                  normalizedRotation.y,
                  normalizedRotation.z,
                  normalizedRotation.w),
        JPH::RVec3(center.x, center.y, center.z));
    JPH::CollideShapeSettings settings;
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;

    // CollideShape performs the broad-phase candidate lookup and then tests the
    // actual query BoxShape against every candidate in the narrow phase. This
    // is intentionally different from an AABB-only query: a rotated hitbox
    // must not damage bodies that only overlap its enclosing AABB.
    const auto& narrowPhase = physicsSystem->GetNarrowPhaseQuery();
    if (ignoreBodyID.IsInvalid()) {
        narrowPhase.CollideShape(&queryShape,
                                 JPH::Vec3::sOne(),
                                 queryTransform,
                                 settings,
                                 JPH::RVec3::sZero(),
                                 collector);
    } else {
        const JPH::IgnoreSingleBodyFilter bodyFilter(ignoreBodyID);
        narrowPhase.CollideShape(&queryShape,
                                 JPH::Vec3::sOne(),
                                 queryTransform,
                                 settings,
                                 JPH::RVec3::sZero(),
                                 collector,
                                 {},
                                 {},
                                 bodyFilter);
    }

    outBodyIDs.reserve(collector.mHits.size());
    for (const JPH::CollideShapeResult& hit : collector.mHits) {
        const JPH::BodyID bodyID = hit.mBodyID2;
        if (bodyID.IsInvalid() ||
            std::find(outBodyIDs.begin(), outBodyIDs.end(), bodyID) != outBodyIDs.end()) {
            continue;
        }
        outBodyIDs.push_back(bodyID);
    }
    return !outBodyIDs.empty();
}

// 收集与查询 AABB 相交的 body(用于静态体移动后的局部唤醒)
class WakeBodyCollector : public JPH::CollideShapeBodyCollector {
public:
    void AddHit(const JPH::BodyID& inBodyID) override {
        if (mCount < 1024) {
            mBodies[mCount++] = inBodyID;
        }
    }
    JPH::BodyID mBodies[1024];
    int mCount = 0;
};

void PhysicsManager::WakeBodiesNear(JPH::BodyID movedBody) {
    if (!physicsSystem || movedBody.IsInvalid()) return;
    JPH::BodyInterface& bodyInterface = physicsSystem->GetBodyInterface();
    if (!bodyInterface.IsAdded(movedBody)) return;

    // 被移动静态体的世界 AABB,略微膨胀以覆盖紧贴的接触体
    const JPH::Shape* shape = bodyInterface.GetShape(movedBody);
    if (!shape) return;
    JPH::AABox worldBounds = shape->GetLocalBounds().Transformed(bodyInterface.GetCenterOfMassTransform(movedBody));
    worldBounds.ExpandBy(JPH::Vec3::sReplicate(0.1f));

    // 只查询与该 AABB 相交的 body(空间索引,不是全遍历)
    WakeBodyCollector collector;
    physicsSystem->GetBroadPhaseQuery().CollideAABox(worldBounds, collector);

    // 唤醒命中的睡眠动态体
    for (int i = 0; i < collector.mCount; ++i) {
        const JPH::BodyID id = collector.mBodies[i];
        if (id == movedBody) continue;
        if (bodyInterface.GetMotionType(id) != JPH::EMotionType::Dynamic) continue;
        if (!bodyInterface.IsActive(id)) {
            bodyInterface.ActivateBody(id);
        }
    }
}

void PhysicsManager::SetCollisionListener(CollisionListener* listener) {
    collisionListener = listener;
}

void PhysicsManager::CleanupDistantBodies(const glm::vec3& cameraPos) {
    if (!physicsSystem) return;
    
    JPH::BodyInterface& bodyInterface = physicsSystem->GetBodyInterface();
    
    // 遍历所有刚体
    JPH::BodyIDVector bodyIDs;
    physicsSystem->GetBodies(bodyIDs);
    
    std::vector<JPH::BodyID> bodiesToRemove;
    int bodiesPutToSleep = 0;
    
    for (JPH::BodyID bodyID : bodyIDs) {
        if (bodyID.IsInvalid()) continue;

        if (std::find(persistentStaticBodies.begin(), persistentStaticBodies.end(), bodyID) !=
            persistentStaticBodies.end()) {
            continue;
        }
        
        // 获取刚体位置
        JPH::RVec3 pos = bodyInterface.GetPosition(bodyID);
        glm::vec3 bodyPos(pos.GetX(), pos.GetY(), pos.GetZ());
        
        // 检查是否在y轴-1000以下
        if (bodyPos.y < -1000.0f) {
            bodiesToRemove.push_back(bodyID);
            continue;
        }
        
        // 计算到相机的距离
        float distance = glm::distance(bodyPos, cameraPos);
        
        // 如果距离超过阈值（例如500单位），启用休眠
        const float sleepDistanceThreshold = 500.0f;
        if (distance > sleepDistanceThreshold) {
            // 获取刚体的运动类型
            JPH::EMotionType motionType = bodyInterface.GetMotionType(bodyID);
            
            // 只有动态刚体可以休眠
            if (motionType == JPH::EMotionType::Dynamic) {
                // 检查刚体是否已经休眠
                if (!bodyInterface.IsActive(bodyID)) {
                    // 已经休眠，跳过
                    continue;
                }
                
                // 让刚体进入休眠状态
                bodyInterface.DeactivateBody(bodyID);
                bodiesPutToSleep++;
            }
        }
        
        // 额外的清除条件：距离相机过远的刚体也直接移除
        const float removeDistanceThreshold = 1000.0f;
        if (distance > removeDistanceThreshold) {
            bodiesToRemove.push_back(bodyID);
        }
    }
    
    // 移除y轴-1000以下和距离过远的刚体
    for (JPH::BodyID bodyID : bodiesToRemove) {
        RemoveRigidBody(bodyID);
    }
    
    if (!bodiesToRemove.empty()) {
        printf("[PhysicsManager] Removed %d bodies (below y=-1000 or too far)\n", (int)bodiesToRemove.size());
    }
    
    if (bodiesPutToSleep > 0) {
        printf("[PhysicsManager] Put %d bodies to sleep\n", bodiesPutToSleep);
    }
}

} // namespace Physics
