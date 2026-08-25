#include "ECS/PhysicsSystem.h"
#include "ECS/Components.h"
#include "ECS/Coordinator.h"
#include "ECS/SceneECS.h"
#include "PhysicsManager.h"
#include "Rendering/HeightmapLoader.h"
#include "Rendering/ModelLoader.h"
#include "EngineConfig.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <unordered_set>
#include <vector>

namespace ECS {

namespace {

glm::vec3 AbsoluteScale(const glm::vec3& scale) {
    return glm::vec3(std::abs(scale.x), std::abs(scale.y), std::abs(scale.z));
}

glm::vec3 PhysicsScale(const glm::vec3& scale) {
    // Jolt ScaledShape 不接受零尺寸。模型可以在编辑器里被缩到 0，
    // 但碰撞形状仍保留一个极小的有效尺寸，恢复缩放时再自动跟随。
    return glm::max(AbsoluteScale(scale), glm::vec3(0.001f));
}

bool ScaleChanged(const glm::vec3& lhs, const glm::vec3& rhs) {
    return glm::length(lhs - rhs) > 0.00001f;
}

bool MatrixChanged(const glm::mat4& lhs, const glm::mat4& rhs) {
    constexpr float kMatrixEpsilon = 0.0001f;
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            if (std::abs(lhs[column][row] - rhs[column][row]) > kMatrixEpsilon) {
                return true;
            }
        }
    }
    return false;
}

struct WaterVolume {
    float minX = 0.0f;
    float maxX = 0.0f;
    float minZ = 0.0f;
    float maxZ = 0.0f;
    float surfaceY = 0.0f;
    float bottomY = 0.0f;
};

bool BuildWaterVolume(const WaterComponent& settings, const glm::mat4& worldMatrix,
                      WaterVolume& outVolume) {
    const glm::vec2 size = glm::max(glm::abs(settings.size), glm::vec2(0.001f));
    const float halfX = size.x * 0.5f;
    const float halfZ = size.y * 0.5f;
    const float localY = settings.surfaceOffset;

    const std::array<glm::vec3, 4> localCorners = {
        glm::vec3(-halfX, localY, -halfZ),
        glm::vec3( halfX, localY, -halfZ),
        glm::vec3(-halfX, localY,  halfZ),
        glm::vec3( halfX, localY,  halfZ),
    };
    outVolume.minX = std::numeric_limits<float>::max();
    outVolume.maxX = std::numeric_limits<float>::lowest();
    outVolume.minZ = std::numeric_limits<float>::max();
    outVolume.maxZ = std::numeric_limits<float>::lowest();
    for (const glm::vec3& localCorner : localCorners) {
        const glm::vec4 worldCorner = worldMatrix * glm::vec4(localCorner, 1.0f);
        if (!std::isfinite(worldCorner.x) || !std::isfinite(worldCorner.y) ||
            !std::isfinite(worldCorner.z) || std::abs(worldCorner.w) < 0.000001f) {
            return false;
        }
        const glm::vec3 corner = glm::vec3(worldCorner) / worldCorner.w;
        outVolume.minX = std::min(outVolume.minX, corner.x);
        outVolume.maxX = std::max(outVolume.maxX, corner.x);
        outVolume.minZ = std::min(outVolume.minZ, corner.z);
        outVolume.maxZ = std::max(outVolume.maxZ, corner.z);
    }

    const glm::vec4 worldSurface = worldMatrix * glm::vec4(0.0f, localY, 0.0f, 1.0f);
    if (!std::isfinite(worldSurface.y) || std::abs(worldSurface.w) < 0.000001f) {
        return false;
    }
    outVolume.surfaceY = worldSurface.y / worldSurface.w;
    const float verticalScale = std::max(0.001f, glm::length(glm::vec3(worldMatrix[1])));
    outVolume.bottomY = outVolume.surfaceY - std::max(0.0f, settings.depth) * verticalScale;
    return true;
}

bool IsPlayerBody(Entity entity, Coordinator& coordinator) {
    if (coordinator.HasComponent<PlayerControllerComponent>(entity)) {
        return true;
    }
    if (coordinator.HasComponent<ScriptComponent>(entity)) {
        return coordinator.GetComponent<ScriptComponent>(entity).scriptName ==
               "PlayerWalkScript";
    }
    return false;
}

bool TerrainCollisionSettingsEqual(const TerrainComponent& lhs,
                                   const TerrainComponent& rhs) {
    return lhs.enabled == rhs.enabled &&
           lhs.collisionEnabled == rhs.collisionEnabled &&
           lhs.heightmapPath == rhs.heightmapPath &&
           lhs.worldSize.x == rhs.worldSize.x && lhs.worldSize.y == rhs.worldSize.y &&
           lhs.heightScale == rhs.heightScale && lhs.heightOffset == rhs.heightOffset &&
           lhs.collisionResolution == rhs.collisionResolution;
}

float SampleHeightNormalized(const HeightmapPixels16& heightmap, float u, float v) {
    if (!heightmap.IsValid()) return 0.0f;

    // HeightmapLoader 保留 PNG 的 top-left 行序，而渲染上传路径将图像
    // bottom-up 上传到 Vulkan。地形本地 Z=0 对应渲染 UV.v=0，因此这里
    // 需要用 1-v 访问 CPU 图像，保证碰撞与画面方向一致。
    u = std::clamp(u, 0.0f, 1.0f);
    v = 1.0f - std::clamp(v, 0.0f, 1.0f);

    const float x = u * static_cast<float>(heightmap.width - 1u);
    const float y = v * static_cast<float>(heightmap.height - 1u);
    const uint32_t x0 = static_cast<uint32_t>(std::floor(x));
    const uint32_t y0 = static_cast<uint32_t>(std::floor(y));
    const uint32_t x1 = std::min(x0 + 1u, heightmap.width - 1u);
    const uint32_t y1 = std::min(y0 + 1u, heightmap.height - 1u);
    const float tx = x - static_cast<float>(x0);
    const float ty = y - static_cast<float>(y0);

    const auto sample = [&](uint32_t sx, uint32_t sy) {
        return static_cast<float>(heightmap.samples[
            static_cast<size_t>(sy) * heightmap.width + sx]) / 65535.0f;
    };

    const float h00 = sample(x0, y0);
    const float h10 = sample(x1, y0);
    const float h01 = sample(x0, y1);
    const float h11 = sample(x1, y1);
    const float h0 = h00 + (h10 - h00) * tx;
    const float h1 = h01 + (h11 - h01) * tx;
    return h0 + (h1 - h0) * ty;
}

bool BuildTerrainCollisionMesh(const TerrainComponent& terrain,
                               const glm::mat4& worldMatrix,
                               std::vector<glm::vec3>& vertices,
                               std::vector<uint32_t>& indices,
                               uint32_t& outResolutionX,
                               uint32_t& outResolutionZ) {
    vertices.clear();
    indices.clear();
    outResolutionX = 0;
    outResolutionZ = 0;

    HeightmapPixels16 heightmap;
    std::string errorMessage;
    const std::string heightmapPath = EngineConfig::GetFullPath(terrain.heightmapPath.c_str());
    if (!HeightmapLoader::LoadPng16(heightmapPath, heightmap, &errorMessage)) {
        printf("[PhysicsSystem] Terrain heightmap collision load failed '%s': %s\n",
               heightmapPath.c_str(), errorMessage.c_str());
        return false;
    }

    // 257x257 ~= 131k triangles: enough for gameplay support while keeping
    // Jolt's static BVH and memory cost bounded. The field is user-configurable,
    // but hard-clamped here so an accidental value cannot allocate an enormous
    // collision mesh during scene loading.
    constexpr uint32_t kMinResolution = 2u;
    constexpr uint32_t kMaxResolution = 1025u;
    const uint32_t requestedResolution = static_cast<uint32_t>(std::clamp(
        terrain.collisionResolution,
        static_cast<int>(kMinResolution),
        static_cast<int>(kMaxResolution)));
    const uint32_t resolutionX = std::max(
        kMinResolution, std::min(requestedResolution, heightmap.width));
    const uint32_t resolutionZ = std::max(
        kMinResolution, std::min(requestedResolution, heightmap.height));
    outResolutionX = resolutionX;
    outResolutionZ = resolutionZ;

    const size_t vertexCount = static_cast<size_t>(resolutionX) * resolutionZ;
    const size_t quadCount = static_cast<size_t>(resolutionX - 1u) * (resolutionZ - 1u);
    vertices.reserve(vertexCount);
    indices.reserve(quadCount * 6u);

    const glm::vec2 worldSize = glm::max(glm::abs(terrain.worldSize), glm::vec2(0.001f));
    const glm::vec2 localOrigin = -worldSize * 0.5f;
    for (uint32_t z = 0; z < resolutionZ; ++z) {
        const float v = static_cast<float>(z) / static_cast<float>(resolutionZ - 1u);
        for (uint32_t x = 0; x < resolutionX; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(resolutionX - 1u);
            const float localHeight = SampleHeightNormalized(heightmap, u, v) *
                                          terrain.heightScale + terrain.heightOffset;
            const glm::vec3 localPosition(
                localOrigin.x + u * worldSize.x,
                localHeight,
                localOrigin.y + v * worldSize.y);
            const glm::vec4 worldPosition = worldMatrix * glm::vec4(localPosition, 1.0f);
            if (!std::isfinite(worldPosition.x) || !std::isfinite(worldPosition.y) ||
                !std::isfinite(worldPosition.z) || !std::isfinite(worldPosition.w) ||
                std::abs(worldPosition.w) < 0.000001f) {
                vertices.clear();
                indices.clear();
                printf("[PhysicsSystem] Terrain collision mesh contains non-finite world vertex\n");
                return false;
            }
            vertices.emplace_back(worldPosition.x / worldPosition.w,
                                  worldPosition.y / worldPosition.w,
                                  worldPosition.z / worldPosition.w);
        }
    }

    for (uint32_t z = 0; z + 1u < resolutionZ; ++z) {
        for (uint32_t x = 0; x + 1u < resolutionX; ++x) {
            const uint32_t a = z * resolutionX + x;
            const uint32_t b = a + 1u;
            const uint32_t d = a + resolutionX;
            const uint32_t c = d + 1u;
            // 与 TerrainRenderer 的 patch 绕序保持一致，法线朝局部 +Y。
            indices.push_back(a);
            indices.push_back(c);
            indices.push_back(b);
            indices.push_back(a);
            indices.push_back(d);
            indices.push_back(c);
        }
    }

    return !vertices.empty() && !indices.empty();
}

glm::vec3 ScaledColliderOffset(const TransformComponent& transform,
                               const RigidBodyComponent& rigidBody) {
    return rigidBody.offset * transform.scale;
}

glm::vec3 PhysicsBodyPosition(const TransformComponent& transform,
                              const RigidBodyComponent& rigidBody) {
    return transform.position + transform.rotation * ScaledColliderOffset(transform, rigidBody);
}

struct ModelBounds {
    glm::vec3 min = glm::vec3(std::numeric_limits<float>::max());
    glm::vec3 max = glm::vec3(std::numeric_limits<float>::lowest());
    bool valid = false;
};

bool GetModelLocalBounds(Entity entity, Coordinator& coordinator, ModelBounds& bounds) {
    std::string modelPath;
    if (coordinator.HasComponent<MeshComponent>(entity)) {
        const auto& mesh = coordinator.GetComponent<MeshComponent>(entity);
        modelPath = mesh.modelPath;
    }
    if (modelPath.empty() && coordinator.HasComponent<ColliderComponent>(entity)) {
        modelPath = coordinator.GetComponent<ColliderComponent>(entity).modelPath;
    }
    if (modelPath.empty() && coordinator.HasComponent<RigidBodyComponent>(entity)) {
        modelPath = coordinator.GetComponent<RigidBodyComponent>(entity).collisionModelPath;
    }
    if (modelPath.empty()) return false;

    const ModelLoadResult result = ModelLoader::LoadModelWithTextures(modelPath);
    for (const auto& subMesh : result.meshData.subMeshes) {
        for (const auto& vertex : subMesh.vertices) {
            bounds.min = glm::min(bounds.min, vertex.Position);
            bounds.max = glm::max(bounds.max, vertex.Position);
            bounds.valid = true;
        }
    }
    return bounds.valid;
}

glm::vec3 ShapeCenterFallback(const RigidBodyComponent& rigidBody) {
    const glm::vec3 size = glm::max(glm::abs(rigidBody.size), glm::vec3(0.001f));
    switch (rigidBody.shapeType) {
    case RigidBodyComponent::ShapeType::Capsule:
        // Jolt capsule height = cylinder height + diameter.
        return glm::vec3(0.0f, (size.y + size.x) * 0.5f, 0.0f);
    case RigidBodyComponent::ShapeType::Sphere:
        return glm::vec3(0.0f, size.x * 0.5f, 0.0f);
    case RigidBodyComponent::ShapeType::Box:
    case RigidBodyComponent::ShapeType::OBB:
    default:
        return glm::vec3(0.0f, size.y * 0.5f, 0.0f);
    }
}

bool AutoFitRigidBodyToModel(Entity entity,
                             RigidBodyComponent& rigidBody,
                             Coordinator& coordinator) {
    if (!rigidBody.autoFitToModel) return false;
#ifdef __ANDROID__
    // 移动端避免为动画/复杂 glTF 刚体再次加载模型计算 bounds；场景提供的
    // capsule/box 尺寸已经足够用于角色和原型碰撞，也避免 APK 资产线程中的
    // 复杂模型解析路径。
    (void)entity;
    // 胶囊参数以“Transform 原点为脚底”为约定，Jolt 的刚体原点则是
    // 胶囊中心。桌面端的模型 AABB 自动适配会得到同样的中心偏移；安卓
    // 不读 GLB bounds 时必须保留这个轻量回退，否则角色会被地面顶起。
    if (rigidBody.shapeType == RigidBodyComponent::ShapeType::Capsule) {
        rigidBody.offset = ShapeCenterFallback(rigidBody);
        if (coordinator.HasComponent<ColliderComponent>(entity)) {
            auto& collider = coordinator.GetComponent<ColliderComponent>(entity);
            if (collider.autoFitToModel || rigidBody.autoFitToModel) {
                collider.size = rigidBody.size;
                collider.offset = rigidBody.offset;
            }
        }
    }
    return false;
#endif

    ModelBounds bounds;
    if (!GetModelLocalBounds(entity, coordinator, bounds)) {
        // 模型加载失败时仍保持“Transform 原点为脚底”的安全约定，
        // 但不伪造模型尺寸；下一次重建刚体时会再次尝试读取模型。
        rigidBody.offset = ShapeCenterFallback(rigidBody);
        return false;
    }

    const glm::vec3 extent = glm::max(
        glm::abs(bounds.max - bounds.min), glm::vec3(0.001f));
    const glm::vec3 center = (bounds.min + bounds.max) * 0.5f;

    switch (rigidBody.shapeType) {
    case RigidBodyComponent::ShapeType::Box:
    case RigidBodyComponent::ShapeType::OBB:
        rigidBody.size = extent;
        rigidBody.offset = center;
        break;
    case RigidBodyComponent::ShapeType::Sphere: {
        const float diameter = std::max(extent.x, std::max(extent.y, extent.z));
        rigidBody.size = glm::vec3(std::max(0.001f, diameter));
        rigidBody.offset = center;
        break;
    }
    case RigidBodyComponent::ShapeType::Capsule: {
        // 胶囊横向半径仍是玩法参数：静态/T-Pose AABB 可能包含伸展的手臂，
        // 直接用整模型宽度会让角色碰撞体异常变粗。高度和局部中心则由模型 AABB 适配。
        const float diameter = std::max(0.001f,
            std::max(std::abs(rigidBody.size.x), std::abs(rigidBody.size.z)));
        rigidBody.size.x = diameter;
        rigidBody.size.z = diameter;
        rigidBody.size.y = std::max(0.001f, extent.y - diameter);
        rigidBody.offset = center;
        break;
    }
    case RigidBodyComponent::ShapeType::Mesh:
        // Mesh 碰撞体由 PhysicsManager 的模型路径/凸包流程负责，不覆盖其尺寸参数。
        // 顶点已经在模型局部空间中，不能再把 AABB 中心作为刚体偏移，否则会重复平移。
        rigidBody.offset = glm::vec3(0.0f);
        break;
    }

    if (coordinator.HasComponent<ColliderComponent>(entity)) {
        auto& collider = coordinator.GetComponent<ColliderComponent>(entity);
        if (collider.autoFitToModel || rigidBody.autoFitToModel) {
            collider.size = rigidBody.size;
            collider.offset = rigidBody.offset;
        }
    }

    printf("[PhysicsSystem] Auto-fit entity %u: bounds min(%.3f,%.3f,%.3f) max(%.3f,%.3f,%.3f), size(%.3f,%.3f,%.3f), offset(%.3f,%.3f,%.3f)\n",
           static_cast<unsigned>(entity),
           bounds.min.x, bounds.min.y, bounds.min.z,
           bounds.max.x, bounds.max.y, bounds.max.z,
           rigidBody.size.x, rigidBody.size.y, rigidBody.size.z,
           rigidBody.offset.x, rigidBody.offset.y, rigidBody.offset.z);
    return true;
}

bool IsDefaultRigidBody(const RigidBodyComponent& rigidBody) {
    return rigidBody.type == RigidBodyComponent::Type::Dynamic &&
           rigidBody.mass == 1.0f && rigidBody.useGravity && !rigidBody.isTrigger &&
           rigidBody.restitution == 0.5f &&
           rigidBody.shapeType == RigidBodyComponent::ShapeType::Box &&
           rigidBody.size == glm::vec3(1.0f) && rigidBody.offset == glm::vec3(0.0f) &&
           !rigidBody.useOBB && !rigidBody.syncWithModel && !rigidBody.autoFitToModel &&
           rigidBody.collisionModelPath.empty() &&
           rigidBody.collisionPrecision == 0.01f && rigidBody.useConvexHull &&
           rigidBody.maxConvexHullVertices == 256 && !rigidBody.generatePerSubmesh;
}

void InitializeRigidBodyFromColliderIfDefault(Entity entity,
                                              RigidBodyComponent& rigidBody,
                                              Coordinator& coordinator) {
    if (!IsDefaultRigidBody(rigidBody) ||
        !coordinator.HasComponent<ColliderComponent>(entity)) {
        return;
    }

    const auto& collider = coordinator.GetComponent<ColliderComponent>(entity);
    switch (collider.type) {
    case ColliderComponent::Type::Box:
        rigidBody.shapeType = RigidBodyComponent::ShapeType::Box;
        break;
    case ColliderComponent::Type::Sphere:
        rigidBody.shapeType = RigidBodyComponent::ShapeType::Sphere;
        break;
    case ColliderComponent::Type::Capsule:
        rigidBody.shapeType = RigidBodyComponent::ShapeType::Capsule;
        break;
    }
    rigidBody.size = glm::max(AbsoluteScale(collider.size), glm::vec3(0.001f));
    rigidBody.offset = collider.offset;
    rigidBody.isTrigger = collider.isTrigger;
    rigidBody.useOBB = collider.useOBB;
    rigidBody.syncWithModel = collider.syncWithModel;
    rigidBody.autoFitToModel = collider.autoFitToModel;
    rigidBody.collisionModelPath = collider.modelPath;
}

} // namespace

PhysicsSystem::PhysicsSystem() : physicsManager(nullptr) {
}

PhysicsSystem::~PhysicsSystem() {
    Shutdown();
}

void PhysicsSystem::CollectTerrainEntities(Entity entity,
                                            std::vector<Entity>& entities) const {
    auto& coordinator = Coordinator::GetInstance();
    if (coordinator.HasComponent<TerrainComponent>(entity)) {
        entities.push_back(entity);
    }

    for (Entity child : SceneECS::GetInstance().GetChildren(entity)) {
        CollectTerrainEntities(child, entities);
    }
}

void PhysicsSystem::RemoveTerrainCollider(Entity entity) {
    auto it = m_terrainColliders.find(entity);
    if (it == m_terrainColliders.end()) return;

    if (physicsManager) {
        physicsManager->RemoveRigidBody(it->second.bodyID);
    }
    m_terrainColliders.erase(it);
}

void PhysicsSystem::UpdateTerrainColliders() {
    if (!physicsManager) return;

    SceneECS& scene = SceneECS::GetInstance();
    auto& coordinator = Coordinator::GetInstance();
    std::vector<Entity> terrainEntities;
    for (Entity root : scene.GetRootEntities()) {
        CollectTerrainEntities(root, terrainEntities);
    }

    std::unordered_set<Entity> seenEntities;
    seenEntities.reserve(terrainEntities.size());

    for (Entity entity : terrainEntities) {
        seenEntities.insert(entity);
        if (!coordinator.HasComponent<TransformComponent>(entity)) {
            RemoveTerrainCollider(entity);
            continue;
        }

        const TerrainComponent& terrain = coordinator.GetComponent<TerrainComponent>(entity);
        if (!terrain.enabled || !terrain.collisionEnabled || terrain.heightmapPath.empty()) {
            RemoveTerrainCollider(entity);
            continue;
        }

        const glm::mat4 worldMatrix = scene.GetWorldMatrix(entity);
        auto existing = m_terrainColliders.find(entity);
        const bool currentBodyValid = existing != m_terrainColliders.end() &&
                                       physicsManager->IsRigidBodyValid(existing->second.bodyID);
        if (currentBodyValid &&
            TerrainCollisionSettingsEqual(existing->second.settings, terrain) &&
            !MatrixChanged(existing->second.worldMatrix, worldMatrix)) {
            continue;
        }

        // 先在 CPU 上构建新网格，再替换旧体。高度图路径输入错误时保留
        // 旧碰撞体，避免编辑器修改属性的一帧让角色掉穿地形。
        std::vector<glm::vec3> vertices;
        std::vector<uint32_t> indices;
        uint32_t resolutionX = 0;
        uint32_t resolutionZ = 0;
        if (!BuildTerrainCollisionMesh(terrain, worldMatrix, vertices, indices,
                                       resolutionX, resolutionZ)) {
            continue;
        }

        const JPH::BodyID newBodyID = physicsManager->CreateStaticMeshBody(vertices, indices);
        if (newBodyID.IsInvalid()) {
            continue;
        }

        if (existing != m_terrainColliders.end()) {
            physicsManager->RemoveRigidBody(existing->second.bodyID);
        }

        TerrainColliderState state;
        state.bodyID = newBodyID;
        state.settings = terrain;
        state.worldMatrix = worldMatrix;
        m_terrainColliders[entity] = std::move(state);
        printf("[PhysicsSystem] Terrain collider ready: entity=%u resolution=%ux%u triangles=%zu\n",
               static_cast<unsigned>(entity),
               resolutionX, resolutionZ, indices.size() / 3u);
    }

    // 场景重载或实体销毁后，根节点遍历不再能看到旧地形，及时释放其
    // 持久静态体；这与普通刚体映射的生命周期保持一致。
    for (auto it = m_terrainColliders.begin(); it != m_terrainColliders.end(); ) {
        if (seenEntities.find(it->first) == seenEntities.end()) {
            if (physicsManager) {
                physicsManager->RemoveRigidBody(it->second.bodyID);
            }
            it = m_terrainColliders.erase(it);
        } else {
            ++it;
        }
    }
}

void PhysicsSystem::ApplyWaterBuoyancy(float deltaTime) {
    (void)deltaTime;
    if (!physicsManager) return;

    struct WaterField {
        WaterVolume volume;
        WaterComponent settings;
    };

    auto& coordinator = Coordinator::GetInstance();
    SceneECS& scene = SceneECS::GetInstance();
    std::vector<WaterField> waterFields;
    std::function<void(Entity)> collectWater = [&](Entity entity) {
        if (coordinator.HasComponent<WaterComponent>(entity) &&
            coordinator.HasComponent<TransformComponent>(entity)) {
            const WaterComponent& settings = coordinator.GetComponent<WaterComponent>(entity);
            if (settings.enabled) {
                WaterVolume volume;
                if (BuildWaterVolume(settings, scene.GetWorldMatrix(entity), volume)) {
                    waterFields.push_back({volume, settings});
                }
            }
        }

        for (Entity child : scene.GetChildren(entity)) {
            collectWater(child);
        }
    };

    for (Entity root : scene.GetRootEntities()) {
        collectWater(root);
    }

    constexpr float kGravity = 9.81f;
    for (Entity entity : m_Entities) {
        bool inWater = false;
        auto bodyIt = entityToRigidBodyMap.find(entity);
        auto& stateIt = m_entityWaterState[entity];

        if (bodyIt == entityToRigidBodyMap.end() ||
            bodyIt->second.IsInvalid() ||
            !physicsManager->IsRigidBodyValid(bodyIt->second) ||
            !coordinator.HasComponent<RigidBodyComponent>(entity) ||
            !coordinator.HasComponent<TransformComponent>(entity)) {
            if (stateIt) {
                std::fprintf(stderr, "[PhysicsSystem] water exit entity=%u\n",
                             static_cast<unsigned>(entity));
            }
            stateIt = false;
            continue;
        }

        const RigidBodyComponent& rigidBody = coordinator.GetComponent<RigidBodyComponent>(entity);
        if (rigidBody.type != RigidBodyComponent::Type::Dynamic) {
            stateIt = false;
            continue;
        }

        const TransformComponent& transform = coordinator.GetComponent<TransformComponent>(entity);
        const glm::vec3 halfExtents = glm::max(
            glm::abs(rigidBody.size) * glm::max(glm::abs(transform.scale), glm::vec3(0.001f)) * 0.5f,
            glm::vec3(0.001f));
        const glm::vec3 bodyPosition = physicsManager->GetRigidBodyPosition(bodyIt->second);
        const float bodyMinX = bodyPosition.x - halfExtents.x;
        const float bodyMaxX = bodyPosition.x + halfExtents.x;
        const float bodyMinZ = bodyPosition.z - halfExtents.z;
        const float bodyMaxZ = bodyPosition.z + halfExtents.z;
        const float bodyBottom = bodyPosition.y - halfExtents.y;
        const float bodyTop = bodyPosition.y + halfExtents.y;
        const float bodyHeight = std::max(0.001f, halfExtents.y * 2.0f);

        glm::vec3 accumulatedForce(0.0f);
        const float mass = std::max(0.001f, rigidBody.mass);
        const glm::vec3 velocity = physicsManager->GetLinearVelocity(bodyIt->second);

        for (const WaterField& water : waterFields) {
            if (water.settings.affectPlayersOnly && !IsPlayerBody(entity, coordinator)) {
                continue;
            }

            const WaterVolume& volume = water.volume;
            const bool horizontalOverlap = bodyMaxX >= volume.minX && bodyMinX <= volume.maxX &&
                                           bodyMaxZ >= volume.minZ && bodyMinZ <= volume.maxZ;
            if (!horizontalOverlap) continue;

            const float submergedBottom = std::max(bodyBottom, volume.bottomY);
            const float submergedTop = std::min(bodyTop, volume.surfaceY);
            const float submergedHeight = std::max(0.0f, submergedTop - submergedBottom);
            const float submergence = std::clamp(submergedHeight / bodyHeight, 0.0f, 1.0f);
            if (submergence <= 0.0f) continue;

            inWater = true;
            accumulatedForce.y += mass * kGravity * submergence *
                                  std::max(0.0f, water.settings.buoyancy);

            const float drag = std::max(0.0f, water.settings.drag) * submergence;
            accumulatedForce += -glm::vec3(velocity.x, velocity.y * 0.35f, velocity.z) *
                                (mass * drag);
        }

        if (inWater && glm::dot(accumulatedForce, accumulatedForce) > 0.000001f) {
            physicsManager->ApplyForce(bodyIt->second, accumulatedForce);
        }

        if (stateIt != inWater) {
            std::fprintf(stderr, "[PhysicsSystem] water %s entity=%u\n",
                         inWater ? "enter" : "exit", static_cast<unsigned>(entity));
        }
        stateIt = inWater;
    }
}

void PhysicsSystem::Update(float deltaTime) {
    if (!physicsManager) return;

    // 地形不属于 Transform+RigidBody 的常规 ECS 查询集合，因此在物理
    // 更新前按场景树惰性创建/重建一次静态碰撞网格。
    UpdateTerrainColliders();
    
    // 清理失效刚体映射:CleanupDistantBodies 等直接销毁 body 时不更新本映射,
    // 残留条目指向已销毁的 bodyID,后续访问会导致崩溃;此处按 IsAdded 状态剔除。
    for (auto it = entityToRigidBodyMap.begin(); it != entityToRigidBodyMap.end(); ) {
        if (!physicsManager->IsRigidBodyValid(it->second)) {
            m_lastSyncedTransformVersion.erase(it->first);
            it = entityToRigidBodyMap.erase(it);
        } else {
            ++it;
        }
    }

    ApplyWaterBuoyancy(deltaTime);

    // 纯 2D 场景（无 3D 刚体实体）：跳过 Jolt Step 与状态同步，避免空世界每帧空转。
    // 已创建的地形静态体不需要单独 Step，仍会保留在 Jolt broad phase 中。
    if (m_Entities.empty()) return;

    // 更新物理系统
    physicsManager->Update(deltaTime);
    
    // 同步物理状态到实体（仅对未启用 syncWithModel 的刚体）
    for (Entity entity : m_Entities) {
        auto& coordinator = Coordinator::GetInstance();
        if (!coordinator.HasComponent<RigidBodyComponent>(entity)) {
            continue;
        }
        auto& rigidBody = coordinator.GetComponent<RigidBodyComponent>(entity);

        // 如果实体正在被 ImGuizmo 操作，跳过物理→Transform 同步
        // 改为在后面的 SyncModelTransforms 中处理 Transform→物理同步
        if (entity == m_gizmoManipulatedEntity) {
            continue;
        }
        
        // 如果启用了 syncWithModel，跳过物理→模型的同步
        if (rigidBody.syncWithModel) {
            continue;  // 跳过，避免覆盖模型的旋转
        }
        
        auto& transform = coordinator.GetComponent<TransformComponent>(entity);
        
        // 获取刚体 ID
        auto it = entityToRigidBodyMap.find(entity);
        if (it == entityToRigidBodyMap.end() || it->second.IsInvalid()) {
            continue;
        }
        JPH::BodyID bodyID = it->second;

        // 外部增量检测：利用 TransformComponent::localVersion（外部写点统一调用 MarkDirty）。
        // 物理写回自身也会 MarkDirty，但写回后立即记录 lastSynced 版本，
        // 因此 localVersion 与 lastSynced 不一致只能来自"外部修改"→ 允许外部驱动刚体；
        // 一致则正常物理 → Transform（物理结果写回模型）。
        auto verIt = m_lastSyncedTransformVersion.find(entity);
        uint32_t lastSynced = (verIt != m_lastSyncedTransformVersion.end()) ? verIt->second : transform.localVersion;

        if (transform.localVersion != lastSynced) {
            // 外部增量：Transform 权威 → 同步给刚体（外部移动/旋转即时生效）
            physicsManager->SetRigidBodyRotation(bodyID, transform.GetEulerAngles());
            physicsManager->SetRigidBodyPosition(bodyID, PhysicsBodyPosition(transform, rigidBody));
            m_lastSyncedTransformVersion[entity] = transform.localVersion;
        } else {
            // 正常物理 → Transform：同步位置和旋转
            glm::vec3 eulerAngles = physicsManager->GetRigidBodyRotation(bodyID);
            transform.rotation = glm::quat(glm::radians(eulerAngles));
            transform.position = physicsManager->GetRigidBodyPosition(bodyID) -
                transform.rotation * ScaledColliderOffset(transform, rigidBody);
            transform.MarkDirty(); // 物理结果写回 Transform,世界矩阵缓存需失效
            m_lastSyncedTransformVersion[entity] = transform.localVersion;
        }
    }
    
    // 同步模型变换到碰撞体（如果启用了 syncWithModel 或正在被 ImGuizmo 操作）
    SyncModelTransforms();
    
    // 清除 ImGuizmo 操作标记（每帧重置，需要持续检测）
    // 注意：这个标记会在 EditorManager 中每帧更新，所以这里不需要清除
}

void PhysicsSystem::Update(float deltaTime, const glm::vec3& cameraPos) {
    if (!physicsManager) return;

    // 先执行物理更新
    Update(deltaTime);
    
    // 清理远距离刚体
    physicsManager->CleanupDistantBodies(cameraPos);
}

void PhysicsSystem::SetPhysicsManager(Physics::PhysicsManager* physicsManager) {
    this->physicsManager = physicsManager;
}

void PhysicsSystem::Initialize() {
    // 初始化物理系统
    if (physicsManager) {
        physicsManager->Initialize();
    }
    
    printf("[PhysicsSystem] Initialized with %d entities\n", static_cast<int>(m_Entities.size()));
}

void PhysicsSystem::Shutdown() {
    // 移除所有刚体
    for (auto& pair : entityToRigidBodyMap) {
        if (physicsManager) {
            physicsManager->RemoveRigidBody(pair.second);
        }
    }
    entityToRigidBodyMap.clear();
    entityToLastScaleMap.clear();  // 清理缩放缓存
    m_lastSyncedTransformVersion.clear(); // 清理外部增量检测基线
    m_entityWaterState.clear();

    for (auto& pair : m_terrainColliders) {
        if (physicsManager) {
            physicsManager->RemoveRigidBody(pair.second.bodyID);
        }
    }
    m_terrainColliders.clear();
    
    // 关闭物理管理器
    if (physicsManager) {
        physicsManager->Shutdown();
    }
}

JPH::BodyID PhysicsSystem::CreateRigidBodyForEntity(Entity entity, const Physics::PhysicsManager::RigidBodyInfo& info) {
    if (!physicsManager) {
        printf("[PhysicsSystem] Cannot create rigid body: physics manager is null\n");
        return JPH::BodyID();
    }

    Physics::PhysicsManager::RigidBodyInfo resolvedInfo = info;
    auto& coordinator = Coordinator::GetInstance();
    if (coordinator.HasComponent<RigidBodyComponent>(entity)) {
        auto& rigidBody = coordinator.GetComponent<RigidBodyComponent>(entity);
        const bool colliderRequestsAutoFit =
            coordinator.HasComponent<ColliderComponent>(entity) &&
            coordinator.GetComponent<ColliderComponent>(entity).autoFitToModel;
        if (colliderRequestsAutoFit) rigidBody.autoFitToModel = true;

        if (rigidBody.autoFitToModel) {
            AutoFitRigidBodyToModel(entity, rigidBody, coordinator);
            resolvedInfo.size = rigidBody.size;
            if (coordinator.HasComponent<TransformComponent>(entity)) {
                auto& transform = coordinator.GetComponent<TransformComponent>(entity);
                resolvedInfo.position = PhysicsBodyPosition(transform, rigidBody);
                resolvedInfo.rotation = transform.GetEulerAngles();
            }
        }
    }
    
    // 移除已存在的刚体
    RemoveRigidBodyForEntity(entity);
    
    // 创建新刚体
    JPH::BodyID bodyID = physicsManager->CreateRigidBody(resolvedInfo);
    if (!bodyID.IsInvalid()) {
        entityToRigidBodyMap[entity] = bodyID;
        if (coordinator.HasComponent<TransformComponent>(entity)) {
            auto& transform = coordinator.GetComponent<TransformComponent>(entity);
            // info.size 始终是模型空间基准尺寸。物理 Shape 创建完成后再套用
            // Transform.scale，避免后续缩放同步时对已缩放 Shape 重复套缩放。
            const glm::vec3 modelScale = PhysicsScale(transform.scale);
            if (ScaleChanged(modelScale, glm::vec3(1.0f))) {
                physicsManager->SetRigidBodyScale(
                    bodyID, modelScale,
                    resolvedInfo.shapeType == Physics::PhysicsManager::RigidBodyInfo::ShapeType::OBB);
            }
            entityToLastScaleMap[entity] = modelScale;
            // 外部增量检测基线：以创建时的 Transform 版本为同步起点
            m_lastSyncedTransformVersion[entity] = transform.localVersion;
        }
    }
    
    return bodyID;
}

void PhysicsSystem::RemoveRigidBodyForEntity(Entity entity) {
    if (!physicsManager) return;
    
    auto it = entityToRigidBodyMap.find(entity);
    if (it != entityToRigidBodyMap.end()) {
        physicsManager->RemoveRigidBody(it->second);
        entityToRigidBodyMap.erase(it);
        // 清理缩放缓存
        entityToLastScaleMap.erase(entity);
        // 清理外部增量检测基线
        m_lastSyncedTransformVersion.erase(entity);
    }
    m_entityWaterState.erase(entity);
}

JPH::BodyID PhysicsSystem::GetRigidBodyId(Entity entity) const {
    auto it = entityToRigidBodyMap.find(entity);
    if (it != entityToRigidBodyMap.end()) {
        return it->second;
    }
    return JPH::BodyID();
}

bool PhysicsSystem::QueryOrientedBox(const glm::vec3& center,
                                     const glm::quat& rotation,
                                     const glm::vec3& halfExtents,
                                     Entity ignoreEntity,
                                     std::vector<Entity>& outEntities) const {
    outEntities.clear();
    if (!physicsManager) return false;

    std::vector<JPH::BodyID> bodyIDs;
    if (!physicsManager->QueryOrientedBox(center, rotation, halfExtents, bodyIDs,
                                          GetRigidBodyId(ignoreEntity))) {
        return false;
    }

    outEntities.reserve(bodyIDs.size());
    for (const auto& mapping : entityToRigidBodyMap) {
        if (mapping.first == ignoreEntity || mapping.second.IsInvalid()) continue;
        if (std::find(bodyIDs.begin(), bodyIDs.end(), mapping.second) == bodyIDs.end()) {
            continue;
        }
        outEntities.push_back(mapping.first);
    }
    return !outEntities.empty();
}

glm::vec3 PhysicsSystem::GetLinearVelocity(Entity entity) const {
    if (!physicsManager) return glm::vec3(0.0f);
    auto it = entityToRigidBodyMap.find(entity);
    if (it == entityToRigidBodyMap.end()) return glm::vec3(0.0f);
    return physicsManager->GetLinearVelocity(it->second);
}

bool PhysicsSystem::IsEntityInWater(Entity entity) const {
    const auto it = m_entityWaterState.find(entity);
    return it != m_entityWaterState.end() && it->second;
}

void PhysicsSystem::SetLinearVelocity(Entity entity, const glm::vec3& velocity) {
    if (!physicsManager) return;
    auto it = entityToRigidBodyMap.find(entity);
    if (it != entityToRigidBodyMap.end()) {
        physicsManager->SetLinearVelocity(it->second, velocity);
    }
}

void PhysicsSystem::SetRigidBodyOrientation(Entity entity, const glm::quat& orientation) {
    if (!physicsManager) return;
    auto it = entityToRigidBodyMap.find(entity);
    if (it != entityToRigidBodyMap.end()) {
        physicsManager->SetRigidBodyOrientation(it->second, orientation);
    }
}

void PhysicsSystem::SetRestitution(Entity entity, float restitution) {
    auto it = entityToRigidBodyMap.find(entity);
    if (it != entityToRigidBodyMap.end() && physicsManager) {
        physicsManager->SetRestitution(it->second, restitution);
    }
}

void PhysicsSystem::SetGravity(Entity entity, bool useGravity) {
    auto it = entityToRigidBodyMap.find(entity);
    if (it != entityToRigidBodyMap.end() && physicsManager) {
        physicsManager->SetRigidBodyGravityFactor(it->second, useGravity ? 1.0f : 0.0f);
    }
}

void PhysicsSystem::OnEntityAdded(Entity entity) {
    auto& coordinator = Coordinator::GetInstance();
    
    // 检查实体是否有 RigidBodyComponent
    if (coordinator.HasComponent<RigidBodyComponent>(entity) && coordinator.HasComponent<TransformComponent>(entity)) {
        auto& rigidBody = coordinator.GetComponent<RigidBodyComponent>(entity);
        auto& transform = coordinator.GetComponent<TransformComponent>(entity);

        // 编辑器“添加刚体”会先挂载默认 RigidBodyComponent；如果实体已有
        // ColliderComponent，则把碰撞类型、尺寸和同步开关作为刚体初始值，
        // 不再留下默认的 1,1,1 / Box 配置。
        InitializeRigidBodyFromColliderIfDefault(entity, rigidBody, coordinator);
        
        // 创建刚体信息
        Physics::PhysicsManager::RigidBodyInfo info;
        info.type = (rigidBody.type == RigidBodyComponent::Type::Static) ? 
            Physics::PhysicsManager::RigidBodyInfo::Type::Static : 
            (rigidBody.type == RigidBodyComponent::Type::Kinematic) ? 
            Physics::PhysicsManager::RigidBodyInfo::Type::Kinematic : 
            Physics::PhysicsManager::RigidBodyInfo::Type::Dynamic;
        
        switch (rigidBody.shapeType) {
        case RigidBodyComponent::ShapeType::Box:
            info.shapeType = Physics::PhysicsManager::RigidBodyInfo::ShapeType::Box;
            break;
        case RigidBodyComponent::ShapeType::Sphere:
            info.shapeType = Physics::PhysicsManager::RigidBodyInfo::ShapeType::Sphere;
            break;
        case RigidBodyComponent::ShapeType::Capsule:
            info.shapeType = Physics::PhysicsManager::RigidBodyInfo::ShapeType::Capsule;
            break;
        case RigidBodyComponent::ShapeType::OBB:
            info.shapeType = Physics::PhysicsManager::RigidBodyInfo::ShapeType::OBB;
            info.orientation = transform.rotation;
            info.size = rigidBody.size;
            break;
        case RigidBodyComponent::ShapeType::Mesh:
            info.shapeType = Physics::PhysicsManager::RigidBodyInfo::ShapeType::Mesh;
            info.fromModel = true;
            if (coordinator.HasComponent<MeshComponent>(entity)) {
                info.modelPath = coordinator.GetComponent<MeshComponent>(entity).modelPath;
            }
            if (!rigidBody.collisionModelPath.empty()) {
                info.modelPath = rigidBody.collisionModelPath;
            }
            info.collisionPrecision = rigidBody.collisionPrecision;
            info.useConvexHull = rigidBody.useConvexHull;
            info.maxConvexHullVertices = rigidBody.maxConvexHullVertices;
            info.generatePerSubmesh = rigidBody.generatePerSubmesh;
            break;
        }
        
        // info.size 保持模型空间基准尺寸；CreateRigidBodyForEntity 会统一
        // 根据 Transform.scale 套用实际世界缩放。
        info.size = rigidBody.size;
        info.position = PhysicsBodyPosition(transform, rigidBody);
        info.rotation = transform.GetEulerAngles();
        info.mass = rigidBody.mass;
        info.isTrigger = rigidBody.isTrigger;
        info.restitution = rigidBody.restitution;
        info.useGravity = rigidBody.useGravity;
        
        // 创建物理体
        CreateRigidBodyForEntity(entity, info);
    }
}

void PhysicsSystem::OnEntityRemoved(Entity entity) {
    // 移除实体的物理体
    RemoveRigidBodyForEntity(entity);
    RemoveTerrainCollider(entity);
    m_entityWaterState.erase(entity);
}

void PhysicsSystem::SyncModelTransforms() {
    if (!physicsManager) return;

    auto& coordinator = Coordinator::GetInstance();
    
    // 遍历所有有刚体组件的实体
    for (Entity entity : m_Entities) {
        if (!coordinator.HasComponent<RigidBodyComponent>(entity)) continue;
        
        auto& rigidBody = coordinator.GetComponent<RigidBodyComponent>(entity);
        
        // 位置/旋转是否由模型驱动，仍由 syncWithModel 控制；缩放是模型的
        // 几何尺寸属性，必须独立同步。这样动态刚体即使由物理驱动位置，
        // 编辑器/脚本主动修改 Transform.scale 后，碰撞形状也会自动适配。
        const bool shouldSyncTransform = rigidBody.syncWithModel ||
            (entity == m_gizmoManipulatedEntity);
        
        auto it = entityToRigidBodyMap.find(entity);
        if (it == entityToRigidBodyMap.end() || it->second.IsInvalid()) continue;
        
        JPH::BodyID bodyID = it->second;
        
        // 如果有 TransformComponent，同步变换到物理碰撞体
        if (coordinator.HasComponent<TransformComponent>(entity)) {
            auto& transform = coordinator.GetComponent<TransformComponent>(entity);

            auto versionIt = m_lastSyncedTransformVersion.find(entity);
            const bool transformChanged = entity == m_gizmoManipulatedEntity ||
                versionIt == m_lastSyncedTransformVersion.end() ||
                transform.localVersion != versionIt->second;

            if (shouldSyncTransform && transformChanged) {
                // 同步位置
                physicsManager->SetRigidBodyPosition(bodyID, PhysicsBodyPosition(transform, rigidBody));

                // 同步旋转
                if (rigidBody.useOBB || rigidBody.shapeType == RigidBodyComponent::ShapeType::OBB) {
                    // OBB 模式：同步四元数旋转
                    physicsManager->SetRigidBodyOrientation(bodyID, transform.rotation);
                } else {
                    // AABB 模式：同步欧拉角旋转
                    glm::vec3 eulerAngles = glm::degrees(glm::eulerAngles(transform.rotation));
                    physicsManager->SetRigidBodyRotation(bodyID, eulerAngles);
                }
                m_lastSyncedTransformVersion[entity] = transform.localVersion;
            }

            // 同步缩放（不受 syncWithModel 的位置/旋转开关影响）。
            // Shape 的基准尺寸仍来自 rigidBody.size，Transform.scale 作为
            // 世界缩放包在 Shape 外层，因此不会重复放大或污染序列化尺寸。
            auto scaleIt = entityToLastScaleMap.find(entity);
            glm::vec3 lastScale = (scaleIt != entityToLastScaleMap.end()) ? scaleIt->second : glm::vec3(1.0f);
            
            const glm::vec3 modelScale = PhysicsScale(transform.scale);
            if (ScaleChanged(modelScale, lastScale)) {
                JPH::BodyID newBodyID = physicsManager->SetRigidBodyScale(bodyID, modelScale,
                    rigidBody.useOBB || rigidBody.shapeType == RigidBodyComponent::ShapeType::OBB);
                // 如果 BodyID 改变了，更新映射
                if (newBodyID != bodyID) {
                    entityToRigidBodyMap[entity] = newBodyID;
                }
                bodyID = newBodyID;

                // offset 也属于模型局部空间，缩放后必须同步刚体原点。
                // 对 syncWithModel=false 的动态刚体，这一步只在缩放发生
                // 的那一帧执行，不会夺走物理对位置的持续控制权。
                physicsManager->SetRigidBodyPosition(
                    bodyID, PhysicsBodyPosition(transform, rigidBody));

                // 更新缓存的缩放值
                entityToLastScaleMap[entity] = modelScale;
            }
        }
    }
}

} // namespace ECS
