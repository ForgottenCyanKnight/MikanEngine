#pragma once
#include "Platform/Export.h"

#include "ECS/SystemManager.h"
#include "ECS/Types.h"
#include "PhysicsManager.h"
#include "ECS/Components.h"
#include <glm/glm.hpp>
#include <array>
#include <limits>
#include <unordered_map>
#include <vector>

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

    // 精确的有向盒体实体查询，供近战攻击盒、技能范围和后续射击复用。
    bool QueryOrientedBox(const glm::vec3& center,
                          const glm::quat& rotation,
                          const glm::vec3& halfExtents,
                          Entity ignoreEntity,
                          std::vector<Entity>& outEntities) const;

    // Gameplay/plugin-facing body operations.  Keep PhysicsManager behind the
    // exported ECS facade so Game.dll plugins do not link against its global
    // instance directly.
    glm::vec3 GetLinearVelocity(Entity entity) const;
    void SetLinearVelocity(Entity entity, const glm::vec3& velocity);
    void SetRigidBodyOrientation(Entity entity, const glm::quat& orientation);
    bool IsEntityInWater(Entity entity) const;
    
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
    struct TerrainColliderState {
        JPH::BodyID bodyID;
        TerrainComponent settings;
        glm::mat4 worldMatrix = glm::mat4(1.0f);
    };

    void RefreshEnvironmentEntityCache();
    void UpdateTerrainColliders();
    void ApplyWaterBuoyancy(float deltaTime);
    void RemoveTerrainCollider(Entity entity);

    Physics::PhysicsManager* physicsManager;
    std::unordered_map<Entity, JPH::BodyID> entityToRigidBodyMap;
    // Jolt 查询返回 BodyID；反向索引避免为每个命中体扫描全部实体映射。
    // key 使用 index+sequence，保留 Jolt BodyID 的代际语义，避免复用槽位误匹配。
    std::unordered_map<uint32_t, Entity> rigidBodyToEntityMap;
    std::unordered_map<Entity, glm::vec3> entityToLastScaleMap;  // 缓存上次的缩放值
    std::unordered_map<Entity, TerrainColliderState> m_terrainColliders;
    std::unordered_map<Entity, bool> m_entityWaterState;
    // 地形/水体都属于场景环境，不在 PhysicsSystem 的刚体签名里。缓存
    // SceneECS 已经整理好的平面实体列表，避免每个物理帧分别从所有根节点
    // 做递归 DFS；Terrain/Water 组件的增删仍会在平面列表上即时过滤。
    std::vector<Entity> m_environmentEntitiesCache;
    uint32_t m_environmentSceneVersion = std::numeric_limits<uint32_t>::max();
    // 上次"物理→Transform 同步完成"时的 transform.localVersion:
    // 若检测到 localVersion 变化且非物理写回造成,视为外部(脚本/输入)增量修改,
    // 把 Transform 同步给刚体,允许外部驱动刚体(否则每帧物理结果会覆盖外部修改)。
    std::unordered_map<Entity, uint32_t> m_lastSyncedTransformVersion;
    Entity m_gizmoManipulatedEntity = INVALID_ENTITY;  // 当前被 ImGuizmo 操作的实体
    // 远距离刚体清理不需要每个物理帧扫描整个 Body 列表；按帧节流，
    // 避免大量堆叠场景把维护扫描和日志输出变成主要开销。
    uint32_t m_cleanupFrameCounter = 0;
};

}
