#include "ECS/PhysicsSystem.h"
#include "ECS/Components.h"
#include "ECS/Coordinator.h"
#include "PhysicsManager.h"

namespace ECS {

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
        // 如果实体正在被 ImGuizmo 操作，跳过物理→Transform 同步
        // 改为在后面的 SyncModelTransforms 中处理 Transform→物理同步
        if (entity == m_gizmoManipulatedEntity) {
            continue;
        }
        
        // 如果启用了 syncWithModel，跳过物理→模型的同步
        if (Coordinator::GetInstance().HasComponent<RigidBodyComponent>(entity)) {
            auto& rigidBody = Coordinator::GetInstance().GetComponent<RigidBodyComponent>(entity);
            if (rigidBody.syncWithModel) {
                continue;  // 跳过，避免覆盖模型的旋转
            }
        }
        
        auto& transform = Coordinator::GetInstance().GetComponent<TransformComponent>(entity);
        
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
            physicsManager->SetRigidBodyPosition(bodyID, transform.position);
            physicsManager->SetRigidBodyRotation(bodyID, transform.GetEulerAngles());
            m_lastSyncedTransformVersion[entity] = transform.localVersion;
        } else {
            // 正常物理 → Transform：同步位置和旋转
            transform.position = physicsManager->GetRigidBodyPosition(bodyID);
            glm::vec3 eulerAngles = physicsManager->GetRigidBodyRotation(bodyID);
            transform.rotation = glm::quat(glm::radians(eulerAngles));
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
    
    printf("[PhysicsSystem] Initialized with %d entities\n", m_Entities.size());
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
    
    // 移除已存在的刚体
    RemoveRigidBodyForEntity(entity);
    
    // 创建新刚体
    JPH::BodyID bodyID = physicsManager->CreateRigidBody(info);
    if (!bodyID.IsInvalid()) {
        entityToRigidBodyMap[entity] = bodyID;
        // 初始化缩放缓存（使用 transform.scale 而不是 info.size）
        auto& coordinator = Coordinator::GetInstance();
        if (coordinator.HasComponent<TransformComponent>(entity)) {
            auto& transform = coordinator.GetComponent<TransformComponent>(entity);
            entityToLastScaleMap[entity] = transform.scale;
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
        }
        
        // 非 OBB 模式使用碰撞体组件的大小，并考虑模型缩放
        if (rigidBody.shapeType != RigidBodyComponent::ShapeType::OBB) {
            // 应用模型缩放到碰撞体大小
            info.size = rigidBody.size * transform.scale;
        } else {
            // OBB 模式也应用模型缩放
            info.size = rigidBody.size * transform.scale;
        }
        info.position = transform.position;
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
    auto& coordinator = Coordinator::GetInstance();
    
    // 遍历所有有刚体组件的实体
    for (Entity entity : m_Entities) {
        if (!coordinator.HasComponent<RigidBodyComponent>(entity)) continue;
        
        auto& rigidBody = coordinator.GetComponent<RigidBodyComponent>(entity);
        
        // 检查是否启用了同步模型变换，或者实体正在被 ImGuizmo 操作
        bool shouldSync = rigidBody.syncWithModel || (entity == m_gizmoManipulatedEntity);
        if (!shouldSync) continue;
        
        // 静态刚体不能同步变换（物理引擎限制）
        if (rigidBody.type == RigidBodyComponent::Type::Static) {
            // 静态刚体无法同步，跳过
            continue;
        }
        
        auto it = entityToRigidBodyMap.find(entity);
        if (it == entityToRigidBodyMap.end() || it->second.IsInvalid()) continue;
        
        JPH::BodyID bodyID = it->second;
        
        // 如果有 TransformComponent，同步变换到物理碰撞体
        if (coordinator.HasComponent<TransformComponent>(entity)) {
            auto& transform = coordinator.GetComponent<TransformComponent>(entity);
            
            // 同步位置
            physicsManager->SetRigidBodyPosition(bodyID, transform.position);
            
            // 同步旋转
            if (rigidBody.useOBB || rigidBody.shapeType == RigidBodyComponent::ShapeType::OBB) {
                // OBB 模式：同步四元数旋转
                physicsManager->SetRigidBodyOrientation(bodyID, transform.rotation);
            } else {
                // AABB 模式：同步欧拉角旋转
                glm::vec3 eulerAngles = glm::degrees(glm::eulerAngles(transform.rotation));
                physicsManager->SetRigidBodyRotation(bodyID, eulerAngles);
            }
            
            // 同步缩放（只在缩放改变时才重建刚体）
            auto scaleIt = entityToLastScaleMap.find(entity);
            glm::vec3 lastScale = (scaleIt != entityToLastScaleMap.end()) ? scaleIt->second : glm::vec3(1.0f);
            
            if (transform.scale != lastScale) {
                JPH::BodyID newBodyID = physicsManager->SetRigidBodyScale(bodyID, transform.scale, 
                    rigidBody.useOBB || rigidBody.shapeType == RigidBodyComponent::ShapeType::OBB);
                // 如果 BodyID 改变了，更新映射
                if (newBodyID != bodyID) {
                    entityToRigidBodyMap[entity] = newBodyID;
                }
                // 更新缓存的缩放值
                entityToLastScaleMap[entity] = transform.scale;
                
                // 如果是 OBB 模式，更新 rigidBody.size 以同步 UI 显示
                if (rigidBody.useOBB || rigidBody.shapeType == RigidBodyComponent::ShapeType::OBB) {
                    rigidBody.size = transform.scale;
                }
            }
        }
    }
}

} // namespace ECS
