#pragma once
// WorldSystem.h - 体素世界 ECS 系统
// 从 OpenGL 版迁移：负责创建/更新世界（World::Update 驱动 chunk 生成与网格重建）。
// 渲染由 WorldRenderer 在 SceneRenderer 中完成；本系统不依赖图形 API。
#include "Platform/Export.h"
#include "ECS/System.h"
#include "AABB.h"
#include <glm/glm.hpp>
#include <array>
#include <memory>

class World;

namespace ECS {

class MIKAN_API WorldSystem : public System {
public:
    WorldSystem();
    ~WorldSystem() override;

    void Update(float deltaTime) override;
    // 带相机位置与视锥平面的更新（主循环调用）
    void Update(float deltaTime, const glm::vec3& cameraPos, const std::array<Plane, 6>& frustumPlanes);

    void OnEntityAdded(Entity entity) override;
    void OnEntityRemoved(Entity entity) override;

    World* GetWorld() const { return m_World.get(); }
    bool IsWorldActive() const { return m_WorldActive; }

    void Shutdown();

private:
    void EnsureWorld(const class WorldComponent& wc);
    void DestroyWorld();

    std::unique_ptr<World> m_World;
    bool m_WorldActive = false;      // 场景中有启用的 WorldComponent
    bool m_WorldInitialized = false; // 世界对象已创建
    float m_UpdateTimer = 0.0f;
};

} // namespace ECS
