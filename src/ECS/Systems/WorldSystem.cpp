// WorldSystem.cpp - 体素世界 ECS 系统实现
#include "ECS/Systems/WorldSystem.h"
#include "ECS/Components.h"
#include "ECS/Coordinator.h"
#include "Core/ProjectManager.h"
#include "World/World.h"
#include "World/WorldGlobals.h"
#include "World/WorldRenderer.h"
#include <iostream>

namespace ECS {

WorldSystem::WorldSystem() = default;

WorldSystem::~WorldSystem() {
    Shutdown();
}

void WorldSystem::Update(float deltaTime) {
    Update(deltaTime, glm::vec3(0.0f), {});
}

void WorldSystem::Update(float deltaTime, const glm::vec3& cameraPos, const std::array<Plane, 6>& frustumPlanes)
{
    // 找到场景中的 WorldComponent（取第一个）
    const WorldComponent* wc = nullptr;
    for (Entity entity : m_Entities) {
        if (Coordinator::GetInstance().HasComponent<WorldComponent>(entity)) {
            wc = &Coordinator::GetInstance().GetComponent<WorldComponent>(entity);
            break;
        }
    }

    if (wc == nullptr || !wc->enabled) {
        if (m_WorldActive) {
            DestroyWorld();
            m_WorldActive = false;
        }
        return;
    }

    if (!m_WorldInitialized) {
        EnsureWorld(*wc);
    }

    if (!m_World) return;

    // 每帧驱动世界更新（chunk 生成/卸载/网格重建队列在内部按帧预算执行）
    m_World->Update(cameraPos, frustumPlanes);
    m_WorldActive = true;
}

void WorldSystem::OnEntityAdded(Entity entity)
{
    // 惰性初始化在 Update 中完成
    (void)entity;
}

void WorldSystem::OnEntityRemoved(Entity entity)
{
    (void)entity;
}

void WorldSystem::EnsureWorld(const WorldComponent& wc)
{
    m_World = std::make_unique<World>(wc.renderRadius);
    // 同步地形类型
    GetWorldConfig().terrainType = wc.terrainType;
    // 资源根（BlockManager 加载 blocks.csv 用）
    {
        std::string assetsDir = ProjectManager::GetInstance().GetAssetsDir();
        if (!assetsDir.empty() && assetsDir.back() == '/') assetsDir.pop_back();
        GetWorldConfig().AssetPath = assetsDir;
    }
    // 暴露全局指针（WorldRenderer / Camera 碰撞使用）
    g_World = m_World.get();
    if (g_WorldRenderer) {
        g_WorldRenderer->SetWorld(g_World);
    }
    m_WorldInitialized = true;
    std::cout << "[WorldSystem] World created (radius=" << wc.renderRadius
              << ", terrain=" << wc.terrainType << ")" << std::endl;
}

void WorldSystem::DestroyWorld()
{
    if (g_WorldRenderer) {
        g_WorldRenderer->SetWorld(nullptr);
    }
    g_World = nullptr;
    m_World.reset();
    m_WorldInitialized = false;
    std::cout << "[WorldSystem] World destroyed" << std::endl;
}

void WorldSystem::Shutdown()
{
    DestroyWorld();
}

} // namespace ECS
