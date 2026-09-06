// SnakeGame.cpp - 贪吃蛇插件实现(独立 DLL,引擎零修改加载)
// 资源来自场景树(assets/snake.json);本类只管动态逻辑并每帧同步实体位置。
// 注册由引擎 GameManager::LoadPlugin 通过导出函数完成(本文件不静态注册)。
#include "SnakeGame.h"
#include "ECS/SceneECS.h"
#include "ECS/Coordinator.h"
#include "ECS/Components.h"
#include "Rendering/Renderer2D.h"
#include "Rendering/TextRenderer.h"
#include "Core/RenderGlobals.h"
#include "Core/InputSystem.h"

#include <cstdlib>
#include <iostream>

namespace Game {

SnakeGame& SnakeGame::GetInstance() {
    static SnakeGame instance;
    return instance;
}

void SnakeGame::OnSceneLoaded() {
    InitFromScene();
    UpdateMenuVisibility();
}

void SnakeGame::OnGameplayTestStart() {
    if (!m_sceneInitialized) InitFromScene();
    StartNewGame();
    // 自动化回放不依赖 rand() 的平台实现或随机食物位置；将食物放到
    // 固定角落，短回放即可稳定验证移动、场景实体同步和菜单状态切换。
    m_food = { 0, 0 };
    SyncEntities();
    std::cout << "[SnakeGame] Gameplay test started deterministically" << std::endl;
}

void SnakeGame::OnGameStop() {
    // 引擎停止: 重置回菜单,清理运行时实体(蛇回到初始 3 节并同步位置)
    if (!m_sceneInitialized) return;
    m_state = State::Menu;
    Reset();        // 逻辑蛇重置 + 销毁多余蛇身实体
    SyncEntities(); // 同步前 3 节到初始位置
    UpdateMenuVisibility();
    std::cout << "[SnakeGame] Stopped -> menu" << std::endl;
}

// ===== 场景树实体同步 =====

void SnakeGame::InitFromScene() {
    auto& sceneECS = ECS::SceneECS::GetInstance();
    auto& coordinator = ECS::Coordinator::GetInstance();

    m_foodEntity = sceneECS.FindByName("Food");
    m_snakeEntities.clear();
    for (int i = 0; ; ++i) {
        ECS::Entity e = sceneECS.FindByName("Snake" + std::to_string(i));
        if (e == ECS::INVALID_ENTITY) break;
        m_snakeEntities.push_back(e);
    }
    m_titleEntity = sceneECS.FindByName("MenuTitle");
    m_buttonEntity = sceneECS.FindByName("StartButton");

    if (m_titleEntity != ECS::INVALID_ENTITY) m_titleHomePos = sceneECS.GetPosition(m_titleEntity);
    if (m_buttonEntity != ECS::INVALID_ENTITY) m_buttonHomePos = sceneECS.GetPosition(m_buttonEntity);

    // 绑定"开始游戏"按钮 → 开始新游戏(序列化不存 onClick,须加载后重绑)
    if (m_buttonEntity != ECS::INVALID_ENTITY && coordinator.HasComponent<ECS::ButtonComponent>(m_buttonEntity)) {
        auto& btn = coordinator.GetComponent<ECS::ButtonComponent>(m_buttonEntity);
        btn.onClick = [this]() { StartNewGame(); };
        std::cout << "[SnakeGame] StartButton onClick bound" << std::endl;
    }

    // 方向键按钮(移动端虚拟按键): 点击转向
    const char* dirNames[4] = { "BtnUp", "BtnDown", "BtnLeft", "BtnRight" };
    const glm::ivec2 dirs[4] = { { 0, -1 }, { 0, 1 }, { -1, 0 }, { 1, 0 } };
    for (int i = 0; i < 4; ++i) {
        m_dirBtn[i] = sceneECS.FindByName(dirNames[i]);
        if (m_dirBtn[i] != ECS::INVALID_ENTITY) {
            m_dirBtnHome[i] = sceneECS.GetPosition(m_dirBtn[i]);
            if (coordinator.HasComponent<ECS::ButtonComponent>(m_dirBtn[i])) {
                coordinator.GetComponent<ECS::ButtonComponent>(m_dirBtn[i]).onClick =
                    [this, dirs, i]() { m_nextDir = dirs[i]; };
            }
        }
    }

    // 重试按钮(GameOver 显示): 点击重开
    m_retryEntity = sceneECS.FindByName("RetryButton");
    if (m_retryEntity != ECS::INVALID_ENTITY) {
        m_retryHomePos = sceneECS.GetPosition(m_retryEntity);
        if (coordinator.HasComponent<ECS::ButtonComponent>(m_retryEntity)) {
            coordinator.GetComponent<ECS::ButtonComponent>(m_retryEntity).onClick =
                [this]() { StartNewGame(); };
        }
    }

    // 暗影(GameOver 覆盖层): 场景树实体, 与按钮同周期渲染, 保证按钮在暗影之上
    m_dimEntity = sceneECS.FindByName("OverlayDim");
    if (m_dimEntity != ECS::INVALID_ENTITY) {
        m_dimHomePos = sceneECS.GetPosition(m_dimEntity);
    }

    m_sceneInitialized = true;
    std::cout << "[SnakeGame] Scene entities: food=" << (m_foodEntity != ECS::INVALID_ENTITY)
              << " snakeSegs=" << m_snakeEntities.size()
              << " title=" << (m_titleEntity != ECS::INVALID_ENTITY)
              << " button=" << (m_buttonEntity != ECS::INVALID_ENTITY) << std::endl;
}

void SnakeGame::EnsureSnakeEntity(size_t index) {
    if (index < m_snakeEntities.size()) return;
    auto& sceneECS = ECS::SceneECS::GetInstance();
    auto& coordinator = ECS::Coordinator::GetInstance();

    ECS::Sprite2DComponent tmpl;
    if (!m_snakeEntities.empty()) {
        ECS::Entity t = m_snakeEntities.front();
        if (coordinator.HasComponent<ECS::Sprite2DComponent>(t))
            tmpl = coordinator.GetComponent<ECS::Sprite2DComponent>(t);
    } else {
        tmpl.type = ECS::Sprite2DComponent::Type::Rect;
        tmpl.width = kCell;
        tmpl.height = kCell;
        tmpl.color = glm::vec4(0.16f, 0.55f, 0.27f, 1.0f);
    }

    ECS::Entity e = sceneECS.CreateEmpty("Snake" + std::to_string(index));
    coordinator.AddComponent<ECS::Sprite2DComponent>(e, tmpl);
    m_snakeEntities.push_back(e);
}

void SnakeGame::SyncEntities() {
    if (!m_sceneInitialized) return;
    auto& sceneECS = ECS::SceneECS::GetInstance();

    for (size_t i = 0; i < m_snake.size(); ++i) {
        EnsureSnakeEntity(i);
        glm::vec2 w = GridToWorld(m_snake[i]);
        sceneECS.SetPosition(m_snakeEntities[i], glm::vec3(w, 0.0f));
    }
    if (m_foodEntity != ECS::INVALID_ENTITY) {
        glm::vec2 w = GridToWorld(m_food);
        sceneECS.SetPosition(m_foodEntity, glm::vec3(w, 0.0f));
    }
}

void SnakeGame::UpdateMenuVisibility() {
    if (!m_sceneInitialized) return;
    auto& sceneECS = ECS::SceneECS::GetInstance();
    const bool menu = (m_state == State::Menu);
    const bool playing = (m_state == State::Playing);
    const bool over = (m_state == State::GameOver);
    // 菜单元素: 仅 Menu 显示
    if (m_titleEntity != ECS::INVALID_ENTITY) sceneECS.SetVisible(m_titleEntity, menu);
    if (m_buttonEntity != ECS::INVALID_ENTITY) sceneECS.SetVisible(m_buttonEntity, menu);
    // 方向键(移动端虚拟按键): 仅游戏进行时显示
    for (int i = 0; i < 4; ++i) {
        if (m_dirBtn[i] != ECS::INVALID_ENTITY) sceneECS.SetVisible(m_dirBtn[i], playing);
    }
    // 重试按钮: 仅 GameOver 显示
    if (m_retryEntity != ECS::INVALID_ENTITY) sceneECS.SetVisible(m_retryEntity, over);
    // 暗影(覆盖层): 仅 GameOver 显示
    if (m_dimEntity != ECS::INVALID_ENTITY) sceneECS.SetVisible(m_dimEntity, over);
}

// ===== 游戏逻辑 =====

void SnakeGame::Reset() {
    m_snake.clear();
    const glm::ivec2 startHead(kGridW / 2, kGridH / 2);
    m_snake.push_back(startHead);
    m_snake.push_back(startHead - glm::ivec2(1, 0));
    m_snake.push_back(startHead - glm::ivec2(2, 0));
    m_dir = glm::ivec2(1, 0);
    m_nextDir = glm::ivec2(1, 0);
    m_score = 0;
    m_tickAccum = 0.0f;

    // 重开时清理多余蛇身实体(历史增长创建的场景实体销毁,防止残留)
    if (m_sceneInitialized) {
        auto& sceneECS = ECS::SceneECS::GetInstance();
        while (m_snakeEntities.size() > m_snake.size()) {
            ECS::Entity e = m_snakeEntities.back();
            if (e != ECS::INVALID_ENTITY) sceneECS.DestroyEntity(e);
            m_snakeEntities.pop_back();
        }
    }

    SpawnFood();
}

void SnakeGame::StartNewGame() {
    if (!m_sceneInitialized) InitFromScene();
    Reset();
    m_state = State::Playing;
    UpdateMenuVisibility();
    std::cout << "[SnakeGame] Game started" << std::endl;
}

void SnakeGame::SpawnFood() {
    for (int attempt = 0; attempt < 256; ++attempt) {
        glm::ivec2 p(rand() % kGridW, rand() % kGridH);
        bool occupied = false;
        for (const auto& seg : m_snake) {
            if (seg == p) { occupied = true; break; }
        }
        if (!occupied) { m_food = p; return; }
    }
    for (int y = 0; y < kGridH; ++y) {
        for (int x = 0; x < kGridW; ++x) {
            glm::ivec2 p(x, y);
            bool occupied = false;
            for (const auto& seg : m_snake) { if (seg == p) { occupied = true; break; } }
            if (!occupied) { m_food = p; return; }
        }
    }
}

bool SnakeGame::HitSelf(const glm::ivec2& head) const {
    for (size_t i = 0; i < m_snake.size(); ++i) {
        if (m_snake[i] == head) return true;
    }
    return false;
}

void SnakeGame::Step() {
    if (!(m_nextDir == -m_dir)) m_dir = m_nextDir;

    glm::ivec2 newHead = m_snake.front() + m_dir;

    if (newHead.x < 0 || newHead.x >= kGridW || newHead.y < 0 || newHead.y >= kGridH) {
        m_state = State::GameOver;
        UpdateMenuVisibility();
        std::cout << "[SnakeGame] Game over (wall), score=" << m_score << std::endl;
        return;
    }

    bool ate = (newHead == m_food);
    if (HitSelf(newHead)) {
        m_state = State::GameOver;
        UpdateMenuVisibility();
        std::cout << "[SnakeGame] Game over (self), score=" << m_score << std::endl;
        return;
    }

    m_snake.insert(m_snake.begin(), newHead);
    if (ate) {
        m_score += 10;
        SpawnFood();
    } else {
        m_snake.pop_back();
    }
}

void SnakeGame::OnUpdate(float deltaTime) {
    // 引擎标准输入(动作映射): 每帧轮询, 不依赖事件级 OnKey
    auto& inp = Input::InputSystem::GetInstance();

    if (!m_sceneInitialized) {
        // 兜底:场景重载后未触发 OnSceneLoaded 时懒初始化
        if (ECS::SceneECS::GetInstance().FindByName("Food") != ECS::INVALID_ENTITY) {
            InitFromScene();
            UpdateMenuVisibility();
        }
    }

    switch (m_state) {
    case State::Menu:
        // 启动仅靠"开始游戏"按钮(StartButton onClick);不响应任意键
        return;
    case State::GameOver:
        // 重开仅靠"重新开始"按钮(RetryButton onClick)
        return;
    case State::Playing:
        break;
    }

    // 方向控制(动作映射;画布坐标 y 向下:"上"= y 减小)
    if (inp.IsDown("MoveUp")) m_nextDir = { 0, -1 };
    else if (inp.IsDown("MoveDown")) m_nextDir = { 0, 1 };
    else if (inp.IsDown("MoveLeft")) m_nextDir = { -1, 0 };
    else if (inp.IsDown("MoveRight")) m_nextDir = { 1, 0 };

    m_tickAccum += deltaTime;
    while (m_tickAccum >= kTickInterval) {
        m_tickAccum -= kTickInterval;
        Step();
        if (m_state != State::Playing) break;
    }
    SyncEntities();
}

void SnakeGame::OnKey(SDL_Keycode) {
    // 输入已迁移到 InputSystem 动作轮询(OnUpdate 内);事件级 OnKey 保留接口,暂不处理
}

void SnakeGame::OnRenderUI(Renderer2D& r2d, int viewWidth, int viewHeight) {
    // 菜单由场景树 UI 实体渲染(Canvas2D),本函数只画动态文本
    auto& text = TextRenderer::GetInstance();
    const float cx = viewWidth * 0.5f;
    const float cy = viewHeight * 0.5f;

    switch (m_state) {
    case State::Playing: {
        std::string scoreStr = "分数: " + std::to_string(m_score);
        float w = text.MeasureString(scoreStr, 24.0f);
        text.DrawStringMsdf(scoreStr, cx - w * 0.5f, viewHeight - 40.0f, 24.0f,
                            glm::vec4(1.0f, 0.9f, 0.5f, 1.0f), 2);
        break;
    }
    case State::GameOver: {
        // 暗影由场景树 OverlayDim 实体提供(layer 1, 与按钮同周期排序, 按钮在其上)
        // 布局(画布 y 向下): 标题在上, 分数居中, 重试按钮在标题下方
        float titleW = text.MeasureString("游戏结束", 56.0f);
        text.DrawStringMsdf("游戏结束", cx - titleW * 0.5f, cy - 100.0f, 56.0f,
                            glm::vec4(0.9f, 0.35f, 0.3f, 1.0f), 2);
        std::string scoreStr = "分数: " + std::to_string(m_score);
        float scoreW = text.MeasureString(scoreStr, 32.0f);
        text.DrawStringMsdf(scoreStr, cx - scoreW * 0.5f, cy - 20.0f, 32.0f,
                            glm::vec4(1.0f, 0.9f, 0.5f, 1.0f), 2);
        break;
    }
    default:
        break; // Menu: 场景树渲染
    }

    // FPS 显示(右上角;F 键开关)——由游戏自身绘制,引擎不硬编码
    if (g_ShowFPS) {
        char buf[32];
        snprintf(buf, sizeof(buf), "FPS: %.0f", g_FPS);
        float w = text.MeasureString(buf, 20.0f);
        text.DrawStringMsdf(buf, (float)viewWidth - w - 20.0f, (float)viewHeight - 40.0f,
                            20.0f, glm::vec4(0.9f, 0.9f, 0.5f, 1.0f), 3);
    }
}

} // namespace Game
