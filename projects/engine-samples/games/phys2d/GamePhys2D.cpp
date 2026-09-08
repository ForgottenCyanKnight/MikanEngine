// GamePhys2D.cpp - 2D 物理极简演示(完全重写)
// 场景 scenes/phys2d.json: 一个箱子 + 一个地面(纯视觉 Sprite2D 实体)。
// 刚体通过引擎 Physics2DManager 创建, 不依赖组件序列化。
// 无菜单/无输入/无交互: 启动(场景 game 键激活)即看到箱子受重力下落、落到地面。
#include "GamePhys2D.h"
#include "ECS/SceneECS.h"
#include "ECS/Coordinator.h"
#include "ECS/Components.h"
#include "Core/Physics2DManager.h"
#include "Core/InputSystem.h"

#include <iostream>

namespace {
constexpr float kScale = 100.0f; // 像素/米
}

namespace Game {

GamePhys2D& GamePhys2D::GetInstance() {
    static GamePhys2D instance;
    return instance;
}

static Physics2DManager::BodyHandle CreateBoxBody(
    ECS::Entity e,
    glm::vec2 halfSizeMeters,
    bool dynamic,
    float density,
    float friction) {
    auto& coordinator = ECS::Coordinator::GetInstance();
    auto& t = coordinator.GetComponent<ECS::TransformComponent>(e);
    const glm::vec2 centerMeters{
        (t.position.x + halfSizeMeters.x * kScale) / kScale,
        (t.position.y + halfSizeMeters.y * kScale) / kScale
    };
    return Physics2DManager::GetInstance().CreateBoxBody(
        centerMeters,
        halfSizeMeters,
        dynamic,
        density,
        friction,
        static_cast<std::uintptr_t>(e));
}

void GamePhys2D::InitFromScene() {
    auto& sceneECS = ECS::SceneECS::GetInstance();
    m_box = sceneECS.FindByName("Box");
    m_ground = sceneECS.FindByName("Ground");
    if (m_box == ECS::INVALID_ENTITY || m_ground == ECS::INVALID_ENTITY) {
        std::cerr << "[GamePhys2D] Box/Ground not found" << std::endl;
        return;
    }

    // 代码创建刚体(玩家/黄色箱子动态近零摩擦, 地面静态)
    auto& coord = ECS::Coordinator::GetInstance();
    m_boxHomePos = coord.GetComponent<ECS::TransformComponent>(m_box).position;
    m_boxBody = CreateBoxBody(m_box, { 0.4f, 0.4f }, true, 1.0f, 0.01f);   // 80x80 px, 摩擦≈0
    m_groundBody = CreateBoxBody(m_ground, { 7.5f, 0.2f }, false, 1.0f, 0.05f); // 地面

    // 左右黄色箱子(碰撞测试, 质量同玩家 density=1; 记录初始位置供停止重置)
    m_boxL = sceneECS.FindByName("BoxL");
    m_boxR = sceneECS.FindByName("BoxR");
    m_camera = sceneECS.FindByName("Camera2D");
    if (m_boxL != ECS::INVALID_ENTITY) {
        m_boxLHome = coord.GetComponent<ECS::TransformComponent>(m_boxL).position;
        m_boxLBody = CreateBoxBody(m_boxL, { 0.4f, 0.4f }, true, 1.0f, 0.01f);
    }
    if (m_boxR != ECS::INVALID_ENTITY) {
        m_boxRHome = coord.GetComponent<ECS::TransformComponent>(m_boxR).position;
        m_boxRBody = CreateBoxBody(m_boxR, { 0.4f, 0.4f }, true, 1.0f, 0.01f);
    }
    m_sceneInitialized = true;
    std::cout << "[GamePhys2D] bodies created (box dynamic, ground static)" << std::endl;
}

void GamePhys2D::OnSceneLoaded() {
    if (!m_sceneInitialized) InitFromScene();
}

// 物理步进 + 玩家控制 + 全部刚体写回: 仅在播放态(工具栏运行按钮)调用, 暂停/停止冻结
void GamePhys2D::OnUpdate(float dt) {
    if (!m_sceneInitialized) return;
    Physics2DManager::GetInstance().Update(dt);
    ControlPlayer();

    // 写回所有动态箱子 Transform(左下角 = 中心 - 半尺寸 40px)
    auto& physics = Physics2DManager::GetInstance();
    auto writeBack = [&](ECS::Entity e, Physics2DManager::BodyHandle body) {
        if (e == ECS::INVALID_ENTITY) return;
        if (!physics.IsBodyValid(body)) return;
        auto& coordinator = ECS::Coordinator::GetInstance();
        auto& t = coordinator.GetComponent<ECS::TransformComponent>(e);
        const glm::vec2 p = physics.GetBodyPosition(body);
        t.position = glm::vec3(p.x * kScale - 40.0f, p.y * kScale - 40.0f, 0.0f);
    };
    writeBack(m_box, m_boxBody);
    writeBack(m_boxL, m_boxLBody);
    writeBack(m_boxR, m_boxRBody);

    // 2D 相机跟随玩家(玩家中心, 略偏下留视野)
    auto& coordinator = ECS::Coordinator::GetInstance();
    if (m_camera != ECS::INVALID_ENTITY &&
        coordinator.HasComponent<ECS::Camera2DComponent>(m_camera)) {
        auto& cam = coordinator.GetComponent<ECS::Camera2DComponent>(m_camera);
        auto& pt = coordinator.GetComponent<ECS::TransformComponent>(m_box);
        cam.center = glm::vec2(pt.position.x + 40.0f, pt.position.y + 40.0f - 120.0f);
    }
}

// 玩家控制: 施力(保持惯性, 不被锁死) + 跳跃(空格, 冲量)
// 参考 3D 相机直接改速度会"吸附感", 这里回归施力: 松手后靠摩擦滑行(低摩擦), 碰撞自然旋转
void GamePhys2D::ControlPlayer() {
    auto& inp = Input::InputSystem::GetInstance();
    auto& physics = Physics2DManager::GetInstance();
    float axis = inp.GetAxis("MoveLeft", "MoveRight");
    const auto box = m_boxBody;
    if (!physics.IsBodyValid(box)) return;
    float mass = physics.GetBodyMass(box);
    if (mass <= 0.0f) return;

    // 水平施力: ~20 m/s² 加速度(响应快, 地面近零摩擦→顺畅移动)
    physics.ApplyForceToCenter(box, { axis * mass * 20.0f, 0.0f }, true);

    // 单段跳(空格): 只有"接地"时才可跳; 空中按住/再按无效, 落地后才能再跳
    bool grounded = physics.GetBodyContactCapacity(box) > 0; // 场景仅地面 → 有接触即接地
    static bool s_wasJumpDown = false;
    bool jumpDown = inp.IsDown("Jump");
    bool jumpEdge = jumpDown && !s_wasJumpDown; // 按下边沿(自维护, 不依赖 InputSystem IsPressed)
    s_wasJumpDown = jumpDown;
    if (grounded && jumpEdge) {
        physics.ApplyLinearImpulseToCenter(box, { 0.0f, -mass * 6.0f }, true); // 6 m/s(力加大; 轻重力下跳得高落得慢)
    }
}

void GamePhys2D::OnAlwaysUpdate(float) {}

// 停止: 重置单个刚体(回初始位置, 速度/角速度清零, 写回 Transform)
void GamePhys2D::ResetBody(ECS::Entity e, Physics2DManager::BodyHandle body, const glm::vec3& home) {
    if (e == ECS::INVALID_ENTITY || body == 0) return;
    auto& physics = Physics2DManager::GetInstance();
    if (!physics.IsBodyValid(body)) return;
    const glm::vec2 center{ (home.x + 40.0f) / kScale, (home.y + 40.0f) / kScale }; // 左下角 → 中心(米)
    physics.SetBodyTransform(body, center);
    physics.SetBodyLinearVelocity(body, { 0.0f, 0.0f });
    physics.SetBodyAngularVelocity(body, 0.0f);
    auto& coordinator = ECS::Coordinator::GetInstance();
    auto& t = coordinator.GetComponent<ECS::TransformComponent>(e);
    t.position = home;
}

// 停止: 重置物理世界(玩家 + 左右黄色箱子全部回初始位置)
void GamePhys2D::OnGameStop() {
    if (!m_sceneInitialized) return;
    ResetBody(m_box, m_boxBody, m_boxHomePos);
    ResetBody(m_boxL, m_boxLBody, m_boxLHome);
    ResetBody(m_boxR, m_boxRBody, m_boxRHome);
    std::cout << "[GamePhys2D] Physics reset (all boxes back to start)" << std::endl;
}

void GamePhys2D::OnRenderUI(Renderer2D&, int, int) {}

} // namespace Game

// ===== 插件导出(引擎 GameManager::LoadPlugin 约定)=====
extern "C" __declspec(dllexport) const char* GetGameModuleName() {
    return "phys2d";
}

extern "C" __declspec(dllexport) Game::IGameModule* CreateGameModule() {
    return &Game::GamePhys2D::GetInstance();
}
