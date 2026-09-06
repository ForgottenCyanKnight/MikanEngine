#include "Core/GameplayRuntime.h"

#include "Core/Camera2DSystem.h"
#include "Core/InputSystem.h"
#include "Core/Physics2DManager.h"
#include "Core/Physics2DSystem.h"
#include "Core/PhysicsGlobals.h"
#include "Core/RuntimeCapabilities.h"
#include "Core/SceneSerializer.h"
#include "Core/TilemapSystem.h"
#include "ECS/Components.h"
#include "ECS/Coordinator.h"
#include "ECS/PhysicsSystem.h"
#include "ECS/SceneECS.h"
#include "ECS/ScriptSystem.h"
#include "Core/PlayerControllerSystem.h"
#include "ECS/Systems/SpriteAnimatorSystem.h"
#include "Rendering/ParticleSystem.h"
#include "Game/GameManager.h"
#include "UI/TweenSystem.h"

#include <cstdio>
#include <filesystem>
#include <functional>
#include <vector>

namespace Core {

GameplayRuntime::~GameplayRuntime() {
    Shutdown();
}

bool GameplayRuntime::Initialize() {
    if (m_initialized) return true;

    UseGameplayTestCapabilities();
    ECS::SceneECS::GetInstance().Init();

    auto& coordinator = ECS::Coordinator::GetInstance();
    g_PhysicsSystemPtr = coordinator.RegisterSystem<ECS::PhysicsSystem>();
    ECS::Signature signature;
    signature.set(coordinator.GetComponentType<ECS::TransformComponent>());
    signature.set(coordinator.GetComponentType<ECS::RigidBodyComponent>());
    coordinator.SetSystemSignature<ECS::PhysicsSystem>(signature);

    g_PhysicsSystemPtr->SetPhysicsManager(&g_PhysicsManager);
    g_PhysicsSystemPtr->Initialize();
    Physics2DSystem::GetInstance().Initialize();

    m_initialized = true;
    std::fprintf(stderr, "[GameplayRuntime] initialized (rendering=0 audioDevice=0 inputDevices=0)\n");
    return true;
}

bool GameplayRuntime::LoadScene(const std::string& scenePath, const std::string& requestedGame,
                                bool autoStartGame) {
    if (!m_initialized || scenePath.empty()) return false;

    ECS::SceneSerializer serializer;
    if (!serializer.LoadScene(scenePath)) {
        std::fprintf(stderr, "[GameplayRuntime] scene load failed: %s\n", scenePath.c_str());
        return false;
    }
    Camera2DSystem::GetInstance().SetSceneContext(scenePath);
    Camera2DSystem::GetInstance().Reset();
    Physics2DSystem::GetInstance().ClearBodies();
    TilemapSystem::GetInstance().LoadAllFromScene();

    m_activeGame = requestedGame.empty()
        ? ECS::SceneECS::GetInstance().GetSceneGameModule()
        : requestedGame;
    if (!m_activeGame.empty()) {
        auto* game = Game::GameManager::GetInstance().Activate(m_activeGame);
        if (!game) {
            std::fprintf(stderr, "[GameplayRuntime] game module load failed: %s\n", m_activeGame.c_str());
            return false;
        }
        game->OnSceneLoaded();
        if (autoStartGame) game->OnGameplayTestStart();
        ECS::ScriptSystem::GetInstance().InstantiateAll(false);
        game->OnGameStart();
        m_gameStarted = true;
    } else {
        ECS::ScriptSystem::GetInstance().InstantiateAll(false);
    }
    Camera2DSystem::GetInstance().Rebind();

    std::fprintf(stderr, "[GameplayRuntime] scene ready: %s game=%s\n",
        scenePath.c_str(), m_activeGame.empty() ? "<none>" : m_activeGame.c_str());
    return true;
}

void GameplayRuntime::Tick(float deltaTime) {
    if (!m_initialized) return;

    if (g_PhysicsSystemPtr) {
        ECS::Coordinator::GetInstance().GetSystem<ECS::PlayerControllerSystem>()->Update(deltaTime);
        g_PhysicsSystemPtr->Update(deltaTime);
    }
    Physics2DSystem::GetInstance().Update(deltaTime);
    Camera2DSystem::GetInstance().Update(deltaTime);
    UI::TweenSystem::GetInstance().Update(deltaTime);
    SpriteAnimatorSystem::GetInstance().Update(deltaTime);

    if (auto* game = Game::GameManager::GetInstance().GetCurrent()) game->OnAlwaysUpdate(deltaTime);
    ECS::ScriptSystem::GetInstance().Update(deltaTime);
    if (auto* game = Game::GameManager::GetInstance().GetCurrent()) game->OnUpdate(deltaTime);
    // GameplayRuntime 是 CPU-only 测试宿主，也推进同一套粒子生命周期；
    // 渲染宿主会在自己的主循环中调用一次，二者不会同时运行。
    ParticleSystem::GetInstance().Update(deltaTime);
}

void GameplayRuntime::SetSyntheticPlayerInput(const glm::vec2& move, bool jump) {
    if (!m_initialized) return;
    Input::InputSystem::GetInstance().SetSyntheticState(move, jump);
    auto controller = ECS::Coordinator::GetInstance().GetSystem<ECS::PlayerControllerSystem>();
    if (controller) controller->SetSyntheticInput(move, jump);
}

void GameplayRuntime::ClearSyntheticPlayerInput() {
    if (!m_initialized) return;
    Input::InputSystem::GetInstance().ClearSyntheticState();
    auto controller = ECS::Coordinator::GetInstance().GetSystem<ECS::PlayerControllerSystem>();
    if (controller) controller->ClearSyntheticInput();
}

void GameplayRuntime::Shutdown() {
    if (!m_initialized) return;

    ClearSyntheticPlayerInput();
    Camera2DSystem::GetInstance().Reset();
    if (m_gameStarted) {
        if (auto* game = Game::GameManager::GetInstance().GetCurrent()) game->OnGameStop();
    }
    ECS::ScriptSystem::GetInstance().DestroyAll();
    ParticleSystem::GetInstance().Clear();
    TilemapSystem::GetInstance().ClearAll();
    Physics2DSystem::GetInstance().ClearBodies();
    Physics2DManager::GetInstance().Shutdown();
    // PhysicsSystem's destructor owns the final Shutdown call.  Resetting here
    // avoids running the backend shutdown sequence twice.
    g_PhysicsSystemPtr.reset();
    Game::GameManager::GetInstance().Deactivate();
    ECS::SceneECS::GetInstance().Shutdown();

    m_activeGame.clear();
    m_gameStarted = false;
    m_initialized = false;
    UseFullEngineCapabilities();
    std::fprintf(stderr, "[GameplayRuntime] shutdown complete\n");
}

bool GameplayRuntime::DumpState(const std::string& path, int frames, const char* runtimeLayer, float fps) {
    FILE* file = nullptr;
#ifdef _WIN32
    const std::filesystem::path dumpPath = std::filesystem::u8path(path);
    if (_wfopen_s(&file, dumpPath.c_str(), L"w") != 0 || !file) {
#else
    file = std::fopen(path.c_str(), "w");
    if (!file) {
#endif
        std::fprintf(stderr, "[GameplayRuntime] ERROR: cannot open dump file: %s\n", path.c_str());
        return false;
    }

    auto& scene = ECS::SceneECS::GetInstance();
    std::vector<ECS::Entity> entities;
    std::vector<ECS::Entity> stack = scene.GetRootEntities();
    while (!stack.empty()) {
        const ECS::Entity entity = stack.back();
        stack.pop_back();
        entities.push_back(entity);
        for (ECS::Entity child : scene.GetChildren(entity)) stack.push_back(child);
    }

    auto escapeJson = [](const std::string& value) {
        std::string escaped;
        const char* hex = "0123456789ABCDEF";
        for (unsigned char ch : value) {
            switch (ch) {
                case '"': escaped += "\\\""; break;
                case '\\': escaped += "\\\\"; break;
                case '\b': escaped += "\\b"; break;
                case '\f': escaped += "\\f"; break;
                case '\n': escaped += "\\n"; break;
                case '\r': escaped += "\\r"; break;
                case '\t': escaped += "\\t"; break;
                default:
                    if (ch < 0x20) {
                        escaped += "\\u00";
                        escaped += hex[(ch >> 4) & 0x0F];
                        escaped += hex[ch & 0x0F];
                    } else {
                        escaped += static_cast<char>(ch);
                    }
                    break;
            }
        }
        return escaped;
    };

    std::fprintf(file, "{\n");
    std::fprintf(file, "  \"runtime_layer\": \"%s\",\n", escapeJson(runtimeLayer ? runtimeLayer : "unknown").c_str());
    std::fprintf(file, "  \"frames\": %d,\n", frames);
    std::fprintf(file, "  \"fps\": %.3f,\n", fps);
    std::fprintf(file, "  \"game\": \"%s\",\n", escapeJson(scene.GetSceneGameModule()).c_str());

    const ECS::Entity activeCamera = Camera2DSystem::GetInstance().GetActiveCamera();
    std::fprintf(file, "  \"active_camera\": ");
    if (activeCamera != ECS::INVALID_ENTITY &&
        ECS::Coordinator::GetInstance().HasComponent<ECS::Camera2DComponent>(activeCamera)) {
        const auto& camera = ECS::Coordinator::GetInstance().GetComponent<ECS::Camera2DComponent>(activeCamera);
        const std::string followTarget = camera.followTarget != ECS::INVALID_ENTITY
            ? scene.GetName(camera.followTarget)
            : camera.followTargetName;
        std::fprintf(file,
            "{\"id\": %u, \"name\": \"%s\", \"center\": [%.3f, %.3f], "
            "\"zoom\": %.3f, \"followTarget\": \"%s\"},\n",
            activeCamera, escapeJson(scene.GetName(activeCamera)).c_str(),
            camera.center.x, camera.center.y, camera.zoom, escapeJson(followTarget).c_str());
    } else {
        std::fprintf(file, "null,\n");
    }
    std::fprintf(file, "  \"entity_count\": %zu,\n", entities.size());
    std::fprintf(file, "  \"entities\": [\n");
    for (size_t index = 0; index < entities.size(); ++index) {
        const ECS::Entity entity = entities[index];
        const glm::vec3 position = scene.GetPosition(entity);
        const glm::vec3 worldPosition = scene.GetWorldPosition(entity);
        const glm::vec3 rotation = scene.GetRotationEuler(entity);
        const glm::vec3 scale = scene.GetScale(entity);
        std::fprintf(file,
            "    {\"id\": %u, \"name\": \"%s\", \"visible\": %s, "
            "\"pos\": [%.3f, %.3f, %.3f], \"wpos\": [%.3f, %.3f, %.3f], "
            "\"rot_deg\": [%.3f, %.3f, %.3f], \"scale\": [%.3f, %.3f, %.3f]}%s\n",
            entity, escapeJson(scene.GetName(entity)).c_str(), scene.IsVisible(entity) ? "true" : "false",
            position.x, position.y, position.z, worldPosition.x, worldPosition.y, worldPosition.z,
            rotation.x, rotation.y, rotation.z, scale.x, scale.y, scale.z,
            (index + 1 < entities.size()) ? "," : "");
    }
    std::fprintf(file, "  ]\n}\n");
    const bool streamError = std::ferror(file) != 0;
    const bool closeError = std::fclose(file) != 0;
    if (streamError || closeError) {
        std::fprintf(stderr, "[GameplayRuntime] ERROR: dump write failed: %s\n", path.c_str());
        return false;
    }
    std::fprintf(stderr, "[GameplayRuntime] state dumped: %s (%zu entities, %d frames)\n",
        path.c_str(), entities.size(), frames);
    return true;
}

} // namespace Core
