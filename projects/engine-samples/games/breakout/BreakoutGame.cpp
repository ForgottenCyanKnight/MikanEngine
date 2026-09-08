// BreakoutGame.cpp - 打砖块色块原型(IGameModule 实现,独立插件 DLL,见本项目 games/breakout)
// 挡板/球为场景树实体(scenes/breakout.json),砖块网格启动时动态创建;
// 每帧同步实体位置,命中砖块销毁实体。
// 注: 本文件由 src/Game/BreakoutGame.cpp 迁移为插件(静态注册改为导出函数);
//     1-270 行为原文恢复,OnRenderUI 主体因源文件意外删除,按 UI 文本证据重建(功能等价)。
#include "BreakoutGame.h"
#include "Game/GameManager.h"
#include "ECS/SceneECS.h"
#include "ECS/Coordinator.h"
#include "ECS/Components.h"
#include "Rendering/Renderer2D.h"
#include "Rendering/TextRenderer.h"
#include "Core/RenderGlobals.h"
#include "Core/InputSystem.h"

#include <cstdlib>
#include <cmath>
#include <iostream>

namespace Game {

BreakoutGame& BreakoutGame::GetInstance() {
    static BreakoutGame instance;
    return instance;
}

glm::vec4 BreakoutGame::BrickColor(int row) const {
    // 按行渐变色块(顶部亮 → 底部暗)
    const glm::vec4 palette[] = {
        { 0.95f, 0.35f, 0.35f, 1.0f }, // 红
        { 0.95f, 0.65f, 0.30f, 1.0f }, // 橙
        { 0.95f, 0.85f, 0.30f, 1.0f }, // 黄
        { 0.45f, 0.85f, 0.40f, 1.0f }, // 绿
        { 0.40f, 0.65f, 0.95f, 1.0f }, // 蓝
        { 0.75f, 0.50f, 0.95f, 1.0f }, // 紫
    };
    int idx = row % (int)(sizeof(palette) / sizeof(palette[0]));
    return palette[idx];
}

void BreakoutGame::InitFromScene() {
    auto& sceneECS = ECS::SceneECS::GetInstance();
    auto& coordinator = ECS::Coordinator::GetInstance();

    m_paddleEntity = sceneECS.FindByName("Paddle");
    m_ballEntity = sceneECS.FindByName("Ball");
    m_titleEntity = sceneECS.FindByName("MenuTitle");
    m_hintEntity = sceneECS.FindByName("StartHint");

    if (m_titleEntity != ECS::INVALID_ENTITY) m_titleHomePos = sceneECS.GetPosition(m_titleEntity);
    if (m_hintEntity != ECS::INVALID_ENTITY) m_hintHomePos = sceneECS.GetPosition(m_hintEntity);

    m_sceneInitialized = true;
    std::cout << "[BreakoutGame] Scene entities: paddle=" << (m_paddleEntity != ECS::INVALID_ENTITY)
              << " ball=" << (m_ballEntity != ECS::INVALID_ENTITY)
              << " title=" << (m_titleEntity != ECS::INVALID_ENTITY)
              << " hint=" << (m_hintEntity != ECS::INVALID_ENTITY) << std::endl;
}

void BreakoutGame::UpdateMenuVisibility() {
    if (!m_sceneInitialized) return;
    auto& sceneECS = ECS::SceneECS::GetInstance();
    const bool show = (m_state == State::Menu);
    if (m_titleEntity != ECS::INVALID_ENTITY) sceneECS.SetVisible(m_titleEntity, show);
    if (m_hintEntity != ECS::INVALID_ENTITY) sceneECS.SetVisible(m_hintEntity, show);
}

void BreakoutGame::SyncEntity(ECS::Entity e, glm::vec2 pos, glm::vec2 size) {
    if (e == ECS::INVALID_ENTITY) return;
    auto& sceneECS = ECS::SceneECS::GetInstance();
    auto& coordinator = ECS::Coordinator::GetInstance();
    sceneECS.SetPosition(e, glm::vec3(pos, 0.0f));
    if (coordinator.HasComponent<ECS::Sprite2DComponent>(e)) {
        auto& s = coordinator.GetComponent<ECS::Sprite2DComponent>(e);
        s.width = size.x;
        s.height = size.y;
    }
}

void BreakoutGame::ClearBricks() {
    auto& sceneECS = ECS::SceneECS::GetInstance();
    for (ECS::Entity e : m_brickEntities) {
        if (e != ECS::INVALID_ENTITY) sceneECS.DestroyEntity(e);
    }
    m_brickEntities.clear();
}

void BreakoutGame::CreateBricks() {
    auto& sceneECS = ECS::SceneECS::GetInstance();
    auto& coordinator = ECS::Coordinator::GetInstance();

    const float gridW = kBOKBrickCols * (kBOKBrickW + kBOKBrickGap) - kBOKBrickGap;
    const float startX = -gridW * 0.5f + kBOKBrickW * 0.5f;
    const float startY = -kBOHalfH + 60.0f; // 顶部向下第一行(画布 y 向下)

    for (int r = 0; r < kBOKBrickRows; ++r) {
        for (int c = 0; c < kBOKBrickCols; ++c) {
            glm::vec2 center(startX + c * (kBOKBrickW + kBOKBrickGap),
                             startY + r * (kBOKBrickH + kBOKBrickGap));
            ECS::Entity e = sceneECS.CreateEmpty("Brick" + std::to_string(m_brickEntities.size()));
            ECS::Sprite2DComponent sp;
            sp.type = ECS::Sprite2DComponent::Type::Rect;
            sp.isUI = false;
            sp.width = kBOKBrickW;
            sp.height = kBOKBrickH;
            sp.color = BrickColor(r);
            sp.layer = 0;
            coordinator.AddComponent<ECS::Sprite2DComponent>(e, sp);
            sceneECS.SetPosition(e, glm::vec3(center, 0.0f));
            m_brickEntities.push_back(e);
        }
    }
    std::cout << "[BreakoutGame] Bricks created: " << m_brickEntities.size() << std::endl;
}

void BreakoutGame::StartNewGame() {
    if (!m_sceneInitialized) InitFromScene();
    // 重置挡板/球
    m_paddleX = 0.0f;
    m_ballPos = glm::vec2(0.0f, 470.0f);
    m_ballVel = glm::vec2(0.0f, 0.0f);
    m_ballAttached = true;
    m_score = 0;
    // 重建砖块
    ClearBricks();
    CreateBricks();
    m_state = State::Playing;
    UpdateMenuVisibility();
    std::cout << "[BreakoutGame] Game started" << std::endl;
}

void BreakoutGame::StepBall(float deltaTime) {
    if (m_ballAttached) return;
    m_ballPos += m_ballVel * deltaTime;

    // 左右墙反弹
    if (m_ballPos.x < -kBOHalfW + kBOKBallSize * 0.5f) { m_ballPos.x = -kBOHalfW + kBOKBallSize * 0.5f; m_ballVel.x = std::abs(m_ballVel.x); }
    if (m_ballPos.x > kBOHalfW - kBOKBallSize * 0.5f) { m_ballPos.x = kBOHalfW - kBOKBallSize * 0.5f; m_ballVel.x = -std::abs(m_ballVel.x); }
    // 顶墙反弹
    if (m_ballPos.y < -kBOHalfH + kBOKBallSize * 0.5f) { m_ballPos.y = -kBOHalfH + kBOKBallSize * 0.5f; m_ballVel.y = std::abs(m_ballVel.y); }
    // 底部:漏球 → 失败
    if (m_ballPos.y > kBOHalfH - kBOKBallSize * 0.5f) {
        m_state = State::Lose;
        UpdateMenuVisibility();
        std::cout << "[BreakoutGame] Lose, score=" << m_score << std::endl;
        return;
    }

    // 挡板碰撞(球向下运动时检测)
    if (m_ballVel.y > 0.0f) {
        float paddleTop = 500.0f - kBOPaddleH * 0.5f;
        if (m_ballPos.y + kBOKBallSize * 0.5f >= paddleTop &&
            m_ballPos.y - kBOKBallSize * 0.5f <= paddleTop + kBOPaddleH &&
            std::abs(m_ballPos.x - m_paddleX) <= kBOPaddleW * 0.5f + kBOKBallSize * 0.5f) {
            m_ballPos.y = paddleTop - kBOKBallSize * 0.5f - 0.1f;
            // 按击中位置偏挡板中心的程度控制反弹角度
            float hit = (m_ballPos.x - m_paddleX) / (kBOPaddleW * 0.5f);
            hit = std::max(-1.0f, std::min(1.0f, hit));
            m_ballVel.y = -std::abs(m_ballVel.y);
            m_ballVel.x = hit * kBOKBallSpeed * 0.75f;
        }
    }

    // 砖块碰撞
    for (size_t i = 0; i < m_brickEntities.size(); ++i) {
        if (BallHitsBrick(i)) break;
    }

    // 全部清空 → 胜利
    if (m_brickEntities.empty()) {
        m_state = State::Win;
        UpdateMenuVisibility();
        std::cout << "[BreakoutGame] Win! score=" << m_score << std::endl;
    }
}

bool BreakoutGame::BallHitsBrick(size_t index) {
    if (index >= m_brickEntities.size()) return false;
    auto& sceneECS = ECS::SceneECS::GetInstance();
    glm::vec3 b3 = sceneECS.GetPosition(m_brickEntities[index]);
    glm::vec2 bc(b3.x, b3.y);
    glm::vec2 bh(kBOKBrickW * 0.5f, kBOKBrickH * 0.5f);
    float r = kBOKBallSize * 0.5f;

    glm::vec2 closest(std::max(bc.x - bh.x, std::min(m_ballPos.x, bc.x + bh.x)),
                      std::max(bc.y - bh.y, std::min(m_ballPos.y, bc.y + bh.y)));
    glm::vec2 d = m_ballPos - closest;
    if (d.x * d.x + d.y * d.y > r * r) return false;

    // 命中:按重叠深度确定反弹轴
    float overlapX = bh.x + r - std::abs(m_ballPos.x - bc.x);
    float overlapY = bh.y + r - std::abs(m_ballPos.y - bc.y);
    if (overlapX < overlapY) m_ballVel.x = (m_ballPos.x < bc.x) ? -std::abs(m_ballVel.x) : std::abs(m_ballVel.x);
    else m_ballVel.y = (m_ballPos.y < bc.y) ? -std::abs(m_ballVel.y) : std::abs(m_ballVel.y);

    // 销毁砖块
    sceneECS.DestroyEntity(m_brickEntities[index]);
    m_brickEntities.erase(m_brickEntities.begin() + index);
    m_score += 10;
    return true;
}

void BreakoutGame::SyncEntities() {
    if (!m_sceneInitialized) return;
    // 挡板
    SyncEntity(m_paddleEntity, glm::vec2(m_paddleX, 500.0f), glm::vec2(kBOPaddleW, kBOPaddleH));
    // 球(未发射时贴挡板)
    if (m_ballAttached) m_ballPos.x = m_paddleX;
    SyncEntity(m_ballEntity, m_ballPos, glm::vec2(kBOKBallSize, kBOKBallSize));
    // 砖块位置在创建时已固定,无需每帧同步
}

void BreakoutGame::OnSceneLoaded() {
    InitFromScene();
    UpdateMenuVisibility();
}

void BreakoutGame::OnGameStop() {
    // 引擎停止: 重置回菜单,清理砖块实体
    if (!m_sceneInitialized) return;
    m_state = State::Menu;
    ClearBricks();
    m_paddleX = 0.0f;
    m_ballPos = glm::vec2(0.0f, 470.0f);
    m_ballVel = glm::vec2(0.0f, 0.0f);
    m_ballAttached = true;
    m_score = 0;
    SyncEntities();
    UpdateMenuVisibility();
    std::cout << "[BreakoutGame] Stopped -> menu" << std::endl;
}

void BreakoutGame::OnUpdate(float deltaTime) {
    // 引擎标准输入(动作映射): 每帧轮询
    auto& inp = Input::InputSystem::GetInstance();

    if (!m_sceneInitialized) {
        if (ECS::SceneECS::GetInstance().FindByName("Paddle") != ECS::INVALID_ENTITY) {
            InitFromScene();
            UpdateMenuVisibility();
        }
    }

    switch (m_state) {
    case State::Menu:
        if (inp.AnyKeyPressed()) StartNewGame(); // 按任意键启动
        return;
    case State::Win:
    case State::Lose:
        if (inp.IsPressed("Restart")) StartNewGame(); // R 重开
        return;
    case State::Playing:
        break;
    }

    // 挡板移动(动作轴: MoveLeft/MoveRight)
    float move = inp.GetAxis("MoveLeft", "MoveRight");
    m_paddleX += move * kBOKPaddleSpeed * deltaTime;
    float halfW = kBOPaddleW * 0.5f;
    m_paddleX = std::max(-kBOHalfW + halfW, std::min(kBOHalfW - halfW, m_paddleX));

    // 发射(Confirm: 空格/回车)
    if (m_ballAttached && inp.IsPressed("Confirm")) {
        float ang = (rand() % 41 - 20) * 3.14159f / 180.0f;
        m_ballVel = glm::vec2(std::sin(ang) * kBOKBallSpeed, -std::cos(ang) * kBOKBallSpeed);
        m_ballAttached = false;
    }

    // 球贴挡板时跟随(仅 x)
    if (m_ballAttached) {
        m_ballPos.x = m_paddleX;
        m_ballPos.y = 470.0f;
    }

    // 推进物理
    StepBall(deltaTime);

    // 同步实体位置
    SyncEntities();
}

void BreakoutGame::OnKey(SDL_Keycode key) {
    // 键鼠操作经 InputSystem 动作映射处理;此处无额外按键逻辑
    (void)key;
}

// ===== OnRenderUI(主体于源文件意外删除后重建;UI 文本与字号按原 obj 字面量恢复) =====
void BreakoutGame::OnRenderUI(Renderer2D& r2d, int viewWidth, int viewHeight) {
    auto& text = TextRenderer::GetInstance();
    const float cx = (float)viewWidth * 0.5f;

    // 分数与剩余砖块(顶部居中)
    if (m_state == State::Playing || m_state == State::Win || m_state == State::Lose) {
        std::string scoreStr = "分数: " + std::to_string(m_score);
        float w = text.MeasureString(scoreStr, 24.0f);
        text.DrawStringMsdf(scoreStr, cx - w * 0.5f, 20.0f, 24.0f,
                            glm::vec4(1.0f, 0.9f, 0.5f, 1.0f), 2);
        std::string brickStr = "剩余砖块: " + std::to_string(m_brickEntities.size());
        float w2 = text.MeasureString(brickStr, 20.0f);
        text.DrawStringMsdf(brickStr, cx - w2 * 0.5f, 50.0f, 20.0f,
                            glm::vec4(0.8f, 0.8f, 0.8f, 1.0f), 2);
    }

    // 结束覆盖文本(居中)
    if (m_state == State::Win) {
        float w = text.MeasureString("胜利!", 56.0f);
        text.DrawStringMsdf("胜利!", cx - w * 0.5f, (float)viewHeight * 0.5f - 100.0f, 56.0f,
                            glm::vec4(0.4f, 0.9f, 0.4f, 1.0f), 2);
    } else if (m_state == State::Lose) {
        float w = text.MeasureString("游戏结束", 56.0f);
        text.DrawStringMsdf("游戏结束", cx - w * 0.5f, (float)viewHeight * 0.5f - 100.0f, 56.0f,
                            glm::vec4(0.9f, 0.35f, 0.3f, 1.0f), 2);
    }
    if (m_state == State::Win || m_state == State::Lose) {
        float w = text.MeasureString("按 R 重新开始", 28.0f);
        text.DrawStringMsdf("按 R 重新开始", cx - w * 0.5f, (float)viewHeight * 0.5f - 20.0f, 28.0f,
                            glm::vec4(1.0f, 0.9f, 0.5f, 1.0f), 2);
    }

    // FPS 显示(右上角,F 键开关——由游戏自身绘制,引擎不硬编码
    if (g_ShowFPS) {
        char buf[32];
        snprintf(buf, sizeof(buf), "FPS: %.0f", g_FPS);
        float w = text.MeasureString(buf, 20.0f);
        text.DrawStringMsdf(buf, (float)viewWidth - w - 20.0f, (float)viewHeight - 40.0f,
                            20.0f, glm::vec4(0.9f, 0.9f, 0.5f, 1.0f), 3);
    }
}

} // namespace Game

// ===== 插件导出(引擎 GameManager::LoadPlugin 约定;同项目其他 games 插件) =====
extern "C" __declspec(dllexport) const char* GetGameModuleName() {
    return "breakout"; // 与项目场景顶层 "game" 键一致
}

extern "C" __declspec(dllexport) Game::IGameModule* CreateGameModule() {
    return &Game::BreakoutGame::GetInstance();
}
