#include "SnakeGame.h"

#include "Core/InputSystem.h"
#include "ECS/Components.h"
#include "ECS/Coordinator.h"
#include "ECS/SceneECS.h"
#include "Rendering/Renderer2D.h"
#include "Rendering/TextRenderer.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace Game {

SnakeGame& SnakeGame::GetInstance() {
    static SnakeGame instance;
    return instance;
}

void SnakeGame::OnSceneLoaded() {
    m_sceneInitialized = false;
    m_segmentEntities.clear();
    InitializeSceneBindings();
    ResetGame();
    m_state = State::Menu;
    m_sceneInitialized = true;
    SyncEntities();
    UpdateVisibility();
}

void SnakeGame::InitializeSceneBindings() {
    ECS::SceneECS& scene = ECS::SceneECS::GetInstance();
    ECS::Coordinator& coordinator = ECS::Coordinator::GetInstance();

    m_foodEntity = scene.FindByName("Food");
    for (int index = 0; ; ++index) {
        const ECS::Entity entity = scene.FindByName("Snake" + std::to_string(index));
        if (entity == ECS::INVALID_ENTITY) break;
        m_segmentEntities.push_back(entity);
    }
    m_menuTitleEntity = scene.FindByName("MenuTitle");
    m_startButtonEntity = scene.FindByName("StartButton");
    m_retryButtonEntity = scene.FindByName("RetryButton");
    m_overlayEntity = scene.FindByName("GameOverOverlay");

    if (m_startButtonEntity != ECS::INVALID_ENTITY &&
        coordinator.HasComponent<ECS::ButtonComponent>(m_startButtonEntity)) {
        coordinator.GetComponent<ECS::ButtonComponent>(m_startButtonEntity).onClick =
            [this]() { StartNewGame(); };
    }
    if (m_retryButtonEntity != ECS::INVALID_ENTITY &&
        coordinator.HasComponent<ECS::ButtonComponent>(m_retryButtonEntity)) {
        coordinator.GetComponent<ECS::ButtonComponent>(m_retryButtonEntity).onClick =
            [this]() { StartNewGame(); };
    }
}

void SnakeGame::EnsureSegment(std::size_t index) {
    if (index < m_segmentEntities.size()) return;

    ECS::SceneECS& scene = ECS::SceneECS::GetInstance();
    ECS::Coordinator& coordinator = ECS::Coordinator::GetInstance();
    ECS::Sprite2DComponent sprite;
    sprite.type = ECS::Sprite2DComponent::Type::Rect;
    sprite.width = kCellSize;
    sprite.height = kCellSize;
    sprite.color = glm::vec4(0.20f, 0.78f, 0.34f, 1.0f);
    if (!m_segmentEntities.empty() &&
        coordinator.HasComponent<ECS::Sprite2DComponent>(m_segmentEntities.front())) {
        sprite = coordinator.GetComponent<ECS::Sprite2DComponent>(m_segmentEntities.front());
    }

    const ECS::Entity entity = scene.CreateEmpty("Snake" + std::to_string(index));
    coordinator.AddComponent<ECS::Sprite2DComponent>(entity, sprite);
    m_segmentEntities.push_back(entity);
}

void SnakeGame::SyncEntities() {
    if (!m_sceneInitialized) return;
    ECS::SceneECS& scene = ECS::SceneECS::GetInstance();

    for (std::size_t i = 0; i < m_snake.size(); ++i) {
        EnsureSegment(i);
        const glm::vec2 position = GridToWorld(m_snake[i]);
        scene.SetPosition(m_segmentEntities[i], glm::vec3(position, 0.0f));
    }
    if (m_foodEntity != ECS::INVALID_ENTITY) {
        scene.SetPosition(m_foodEntity,
                          glm::vec3(GridToWorld(m_food), 0.0f));
    }
}

void SnakeGame::UpdateVisibility() {
    if (!m_sceneInitialized) return;
    ECS::SceneECS& scene = ECS::SceneECS::GetInstance();
    const bool menu = m_state == State::Menu;
    const bool gameOver = m_state == State::GameOver;
    if (m_menuTitleEntity != ECS::INVALID_ENTITY) scene.SetVisible(m_menuTitleEntity, menu);
    if (m_startButtonEntity != ECS::INVALID_ENTITY) scene.SetVisible(m_startButtonEntity, menu);
    if (m_retryButtonEntity != ECS::INVALID_ENTITY) scene.SetVisible(m_retryButtonEntity, gameOver);
    if (m_overlayEntity != ECS::INVALID_ENTITY) scene.SetVisible(m_overlayEntity, gameOver);
}

void SnakeGame::ResetGame() {
    m_snake.clear();
    const glm::ivec2 head(kGridWidth / 2, kGridHeight / 2);
    m_snake.push_back(head);
    m_snake.push_back(head - glm::ivec2(1, 0));
    m_snake.push_back(head - glm::ivec2(2, 0));
    m_direction = glm::ivec2(1, 0);
    m_nextDirection = m_direction;
    m_score = 0;
    m_tickAccumulator = 0.0f;

    if (m_sceneInitialized) {
        ECS::SceneECS& scene = ECS::SceneECS::GetInstance();
        while (m_segmentEntities.size() > m_snake.size()) {
            scene.DestroyEntity(m_segmentEntities.back());
            m_segmentEntities.pop_back();
        }
    }
    SpawnFood();
}

void SnakeGame::StartNewGame() {
    if (!m_sceneInitialized) return;
    ResetGame();
    m_state = State::Playing;
    SyncEntities();
    UpdateVisibility();
    std::cout << "[SnakeGame] started" << std::endl;
}

void SnakeGame::SpawnFood() {
    for (int attempt = 0; attempt < 256; ++attempt) {
        const glm::ivec2 candidate(std::rand() % kGridWidth,
                                   std::rand() % kGridHeight);
        if (!HitsSelf(candidate)) {
            m_food = candidate;
            return;
        }
    }
    for (int y = 0; y < kGridHeight; ++y) {
        for (int x = 0; x < kGridWidth; ++x) {
            const glm::ivec2 candidate(x, y);
            if (!HitsSelf(candidate)) {
                m_food = candidate;
                return;
            }
        }
    }
}

bool SnakeGame::HitsSelf(const glm::ivec2& cell) const {
    for (const glm::ivec2& segment : m_snake) {
        if (segment == cell) return true;
    }
    return false;
}

void SnakeGame::Step() {
    if (m_nextDirection != -m_direction) m_direction = m_nextDirection;
    const glm::ivec2 newHead = m_snake.front() + m_direction;
    if (newHead.x < 0 || newHead.x >= kGridWidth ||
        newHead.y < 0 || newHead.y >= kGridHeight || HitsSelf(newHead)) {
        m_state = State::GameOver;
        UpdateVisibility();
        return;
    }

    const bool ateFood = newHead == m_food;
    m_snake.insert(m_snake.begin(), newHead);
    if (ateFood) {
        m_score += 10;
        SpawnFood();
    } else {
        m_snake.pop_back();
    }
}

void SnakeGame::OnUpdate(float deltaTime) {
    if (!m_sceneInitialized || m_state != State::Playing) return;

    Input::InputSystem& input = Input::InputSystem::GetInstance();
    if (input.IsDown("MoveUp")) m_nextDirection = {0, -1};
    else if (input.IsDown("MoveDown")) m_nextDirection = {0, 1};
    else if (input.IsDown("MoveLeft")) m_nextDirection = {-1, 0};
    else if (input.IsDown("MoveRight")) m_nextDirection = {1, 0};

    m_tickAccumulator += deltaTime;
    while (m_tickAccumulator >= kTickInterval && m_state == State::Playing) {
        m_tickAccumulator -= kTickInterval;
        Step();
    }
    SyncEntities();
}

void SnakeGame::OnKey(SDL_Keycode key) {
    if (key == SDLK_SPACE && m_state == State::Menu) StartNewGame();
    if (key == SDLK_R && m_state == State::GameOver) StartNewGame();
}

void SnakeGame::OnRenderUI(Renderer2D& renderer, int viewWidth, int viewHeight) {
    (void)renderer;
    if (viewWidth <= 0 || viewHeight <= 0) return;

    TextRenderer& text = TextRenderer::GetInstance();
    if (m_state == State::Playing) {
        const std::string score = "分数: " + std::to_string(m_score);
        text.DrawStringMsdf(score, 24.0f, viewHeight - 42.0f, 24.0f,
                            glm::vec4(1.0f, 0.9f, 0.45f, 1.0f), 3);
    } else if (m_state == State::GameOver) {
        const float center = viewWidth * 0.5f;
        const float titleWidth = text.MeasureString("游戏结束", 52.0f);
        text.DrawStringMsdf("游戏结束", center - titleWidth * 0.5f,
                            viewHeight * 0.5f - 110.0f, 52.0f,
                            glm::vec4(0.95f, 0.35f, 0.3f, 1.0f), 3);
        const std::string score = "分数: " + std::to_string(m_score);
        const float scoreWidth = text.MeasureString(score, 28.0f);
        text.DrawStringMsdf(score, center - scoreWidth * 0.5f,
                            viewHeight * 0.5f - 40.0f, 28.0f,
                            glm::vec4(1.0f, 0.9f, 0.45f, 1.0f), 3);
    }
}

void SnakeGame::OnGameStart() {
    StartNewGame();
}

void SnakeGame::OnGameStop() {
    if (!m_sceneInitialized) return;
    m_state = State::Menu;
    ResetGame();
    SyncEntities();
    UpdateVisibility();
}

} // namespace Game
