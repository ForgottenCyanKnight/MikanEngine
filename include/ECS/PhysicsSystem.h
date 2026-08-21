#pragma once
#include "Platform/Export.h"

#include "ECS/SystemManager.h"
#include "ECS/Types.h"
#include "PhysicsManager.h"
#include "ECS/Components.h"
#include <unordered_map>

namespace ECS {

class MIKAN_API PhysicsSystem : public System {
public:
    PhysicsSystem();
    ~PhysicsSystem();
    
    void Update(float deltaTime) override;
    
    // 带相机位置的更新方法，用于清理远距离刚体
    void Update(float deltaTime, const glm::vec3& cameraPos);
    
    // 实体添加/移除回调
    void OnEntityAdded(Entity entity) override;
    void OnEntityRemoved(Entity entity) override;
    
    void SetPhysicsManager(Physics::PhysicsManager* physicsManager);
    
    void Initialize();
    void Shutdown();
    
    JPH::BodyID CreateRigidBodyForEntity(Entity entity, const Physics::PhysicsManager::RigidBodyInfo& info);
    void RemoveRigidBodyForEntity(Entity entity);
    JPH::BodyID GetRigidBodyId(Entity entity) const;

    // Gameplay/plugin-facing body operations.  Keep PhysicsManager behind the
    // exported ECS facade so Game.dll plugins do not link against its global
    // instance directly.
    glm::vec3 GetLinearVelocity(Entity entity) const;
    void SetLinearVelocity(Entity entity, const glm::vec3& velocity);
    void SetRigidBodyOrientation(Entity entity, const glm::quat& orientation);
    
    // 设置刚体弹性
    void SetRestitution(Entity entity, float restitution);
    
    // 设置实体刚体重力开关(即时生效,无需重建刚体)
    void SetGravity(Entity entity, bool useGravity);
    
    // 同步模型变换到碰撞体
    void SyncModelTransforms();
    
    // 设置正在被 ImGuizmo 操作的实体（用于跳过物理→Transform 同步）
    void SetGizmoManipulatedEntity(Entity entity) { m_gizmoManipulatedEntity = entity; }
    Entity GetGizmoManipulatedEntity() const { return m_gizmoManipulatedEntity; }
    
private:
    Physics::PhysicsManager* physicsManager;
    std::unordered_map<Entity, JPH::BodyID> entityToRigidBodyMap;
    std::unordered_map<Entity, glm::vec3> entityToLastScaleMap;  // 缓存上次的缩放值
    // 上次"物理→Transform 同步完成"时的 transform.localVersion:
    // 若检测到 localVersion 变化且非物理写回造成,视为外部(脚本/输入)增量修改,
    // 把 Transform 同步给刚体,允许外部驱动刚体(否则每帧物理结果会覆盖外部修改)。
    std::unordered_map<Entity, uint32_t> m_lastSyncedTransformVersion;
    Entity m_gizmoManipulatedEntity = INVALID_ENTITY;  // 当前被 ImGuizmo 操作的实体
};

}
