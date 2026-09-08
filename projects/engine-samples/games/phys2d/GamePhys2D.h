#pragma once
// GamePhys2D.h - 2D 物理极简演示(IGameModule 插件)
// 场景 scenes/phys2d.json: 一个箱子 + 一个地面。
// 刚体通过引擎 Physics2DManager 创建, 启动即下落, 无菜单/无输入/无交互。
#include "Platform/Export.h"
#include "Game/IGameModule.h"
#include "Core/Physics2DManager.h"
#include "ECS/Types.h"
#include <SDL3/SDL.h>
#include <glm/glm.hpp>

class Renderer2D;

namespace Game {

class GamePhys2D : public IGameModule {
public:
    static GamePhys2D& GetInstance();

    const char* GetName() const override { return "phys2d"; }
    void OnSceneLoaded() override;
    void OnAlwaysUpdate(float deltaTime) override; // 无(物理在 OnUpdate 播放态驱动)
    void OnUpdate(float deltaTime) override;
    void OnGameStop() override;                    // 停止: 重置物理(箱子回初始位置)
    void OnKey(SDL_Keycode) override {}
    void OnRenderUI(Renderer2D& r2d, int viewWidth, int viewHeight) override;

private:
    GamePhys2D() = default;
    ~GamePhys2D() = default;
    GamePhys2D(const GamePhys2D&) = delete;
    GamePhys2D& operator=(const GamePhys2D&) = delete;

    void InitFromScene();
    void ControlPlayer();                // 左右施力 + 跳跃(播放态 OnUpdate 内)
    void ResetBody(ECS::Entity e, Physics2DManager::BodyHandle body, const glm::vec3& home); // 停止时重置单个刚体

    bool m_sceneInitialized = false;
    ECS::Entity m_box = ECS::INVALID_ENTITY;     // "Box"(动态)
    ECS::Entity m_ground = ECS::INVALID_ENTITY;  // "Ground"(静态)
    Physics2DManager::BodyHandle m_boxBody = 0;
    Physics2DManager::BodyHandle m_groundBody = 0;
    ECS::Entity m_boxL = ECS::INVALID_ENTITY;    // "BoxL"(黄色, 碰撞测试)
    ECS::Entity m_boxR = ECS::INVALID_ENTITY;    // "BoxR"(黄色, 碰撞测试)
    Physics2DManager::BodyHandle m_boxLBody = 0;
    Physics2DManager::BodyHandle m_boxRBody = 0;
    ECS::Entity m_camera = ECS::INVALID_ENTITY;  // "Camera2D"(跟随玩家)
    glm::vec3 m_boxHomePos{ 0.0f };              // 三个箱子初始位置(停止时全部重置)
    glm::vec3 m_boxLHome{ 0.0f };
    glm::vec3 m_boxRHome{ 0.0f };
};

} // namespace Game
