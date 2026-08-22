#include "ECS/PhysicsSystem.h"
#include "ECS/Components.h"
#include "ECS/Coordinator.h"
#include "PhysicsManager.h"
#include "Rendering/ModelLoader.h"
#include <cmath>
#include <limits>

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

void PhysicsSystem::Update(float deltaTime) {
    if (!physicsManager) return;
    
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

    // 纯 2D 场景（无 3D 刚体实体）：跳过 Jolt Step 与状态同步，避免空世界每帧空转
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
        physicsManager->RemoveRigidBody(pair.second);
    }
    entityToRigidBodyMap.clear();
    entityToLastScaleMap.clear();  // 清理缩放缓存
    m_lastSyncedTransformVersion.clear(); // 清理外部增量检测基线
    
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
}

JPH::BodyID PhysicsSystem::GetRigidBodyId(Entity entity) const {
    auto it = entityToRigidBodyMap.find(entity);
    if (it != entityToRigidBodyMap.end()) {
        return it->second;
    }
    return JPH::BodyID();
}

glm::vec3 PhysicsSystem::GetLinearVelocity(Entity entity) const {
    if (!physicsManager) return glm::vec3(0.0f);
    auto it = entityToRigidBodyMap.find(entity);
    if (it == entityToRigidBodyMap.end()) return glm::vec3(0.0f);
    return physicsManager->GetLinearVelocity(it->second);
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
        info.useGravity = rigidBody.useGravity;
        
        // 创建物理体
        CreateRigidBodyForEntity(entity, info);
    }
}

void PhysicsSystem::OnEntityRemoved(Entity entity) {
    // 移除实体的物理体
    RemoveRigidBodyForEntity(entity);
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
