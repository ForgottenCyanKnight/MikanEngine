// Native integration of Pixel Hop Twins by stm1978 (MIT). See licenses/MIT_LICENSE.txt.
#include "PixelHopGame.h"

#include "Core/InputSystem.h"
#include "Core/Log.h"
#include "Core/ProjectManager.h"
#include "Core/RuntimeCapabilities.h"
#include "ECS/Components.h"
#include "ECS/Coordinator.h"
#include "ECS/SceneECS.h"
#include "Rendering/Renderer2D.h"
#include "UI/Canvas2D.h"
#include "UI/UiDocument.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <memory>
#include <stdexcept>

namespace Game {
namespace {

constexpr char kTouchDocument[] = "ui/mobile.ui.json";
#if defined(__ANDROID__) || defined(MIKAN_OPENHARMONY)
constexpr bool kMobile = true;
#else
constexpr bool kMobile = false;
#endif

nlohmann::json ReadJson(const char* relative) {
    const auto path = ProjectManager::GetInstance().ResolveAssetPath(relative);
    size_t bytes = 0;
    std::unique_ptr<void, decltype(&SDL_free)> file(SDL_LoadFile(path.c_str(), &bytes), SDL_free);
    if (!file) throw std::runtime_error(std::string("Could not read ") + relative + ": " + SDL_GetError());
    return nlohmann::json::parse(static_cast<const char*>(file.get()),
                                 static_cast<const char*>(file.get()) + bytes);
}

} // namespace

PixelHopGame& PixelHopGame::GetInstance() {
    static PixelHopGame instance;
    return instance;
}

void PixelHopGame::OnSceneLoaded() {
    OnGameStop();
    m_loaded = false;
    try {
        m_model = PixelHop::Model{};
        m_model.Load({ReadJson("levels/level1.json"), ReadJson("levels/level2.json")});
        if (Core::GetRuntimeCapabilities().rendering && !m_renderer.Initialize()) {
            throw std::runtime_error("Could not initialize original sprite and font data");
        }
        auto& scene = ECS::SceneECS::GetInstance();
        auto& coordinator = ECS::Coordinator::GetInstance();
        auto player = scene.FindByName("PixelHop Player");
        if (!coordinator.IsAlive(player)) player = scene.CreateEmpty("PixelHop Player");
        m_player = coordinator.GetEntityHandle(player);
        m_hud = coordinator.GetEntityHandle(UI::FindUiDocumentByPath(kTouchDocument));
        for (const auto* id : {"move-left", "move-right", "jump", "pause", "restart", "mute"}) {
            const auto entity = UI::FindUiDocumentNode(m_hud.entity, id);
            if (coordinator.IsAlive(entity)) scene.SetVisible(entity, kMobile);
        }
        auto& input = Input::InputSystem::GetInstance();
        for (const auto* action : {"PixelHopJump", "PixelHopEnter", "PixelHopMute"}) input.UnbindAction(action);
        for (const auto key : {SDLK_SPACE, SDLK_UP, SDLK_W}) input.BindKey("PixelHopJump", key);
        input.BindKey("PixelHopEnter", SDLK_RETURN);
        input.BindKey("PixelHopMute", SDLK_M);
        m_loaded = true;
        SyncScene();
        LOGI("[PixelHop] Loaded original two-level course, upstream 9835295888c7");
    } catch (const std::exception& error) {
        LOGE("[PixelHop] Project load failed: %s", error.what());
    }
}

void PixelHopGame::OnGameStart() {
    if (!m_loaded) return;
    m_started = true;
    m_remainder = 0;
    m_pending = {};
    m_touchJumpHeld = false;
    m_model.FullReset();
    if (Core::GetRuntimeCapabilities().audioDevice) m_audio.Initialize();
    SyncScene();
}

void PixelHopGame::OnGameStop() {
    m_started = false;
    m_audio.Shutdown();
    m_pending = {};
    m_remainder = 0;
    m_touchJumpHeld = false;
}

PixelHop::InputFrame PixelHopGame::ReadInput() {
    auto& keys = Input::InputSystem::GetInstance();
    auto& touch = UI::Canvas2D::GetInstance();
    PixelHop::InputFrame input;
    input.left = keys.IsDown("MoveLeft") || touch.IsActionHeld("move-left");
    input.right = keys.IsDown("MoveRight") || touch.IsActionHeld("move-right");
    const bool touchJump = touch.IsActionHeld("jump");
    input.jumpHeld = keys.IsDown("PixelHopJump") || keys.IsDown("Jump") || touchJump;
    input.jumpPressed = keys.IsPressed("PixelHopJump") || keys.IsPressed("Jump") ||
                        touch.WasActionPressed("jump");
    input.jumpReleased = keys.IsReleased("PixelHopJump") || keys.IsReleased("Jump") ||
                         ((m_touchJumpHeld || touch.WasActionPressed("jump")) && !touchJump);
    input.pause = keys.IsPressed("Pause") || touch.WasActionPressed("pause");
    input.restart = keys.IsPressed("PixelHopEnter") || touch.WasActionPressed("restart");
    input.mute = keys.IsPressed("PixelHopMute") || touch.WasActionPressed("mute");
    m_touchJumpHeld = touchJump;
    return input;
}

void PixelHopGame::OnUpdate(float deltaTime) {
    if (!m_loaded || !m_started) return;
    const auto previousState = m_model._state;
    const auto previousLevel = m_model._levelIndex;
    const auto input = ReadInput();
    m_pending.left = input.left;
    m_pending.right = input.right;
    m_pending.jumpHeld = input.jumpHeld;
    m_pending.jumpPressed |= input.jumpPressed;
    m_pending.jumpReleased |= input.jumpReleased;
    m_pending.pause |= input.pause;
    m_pending.restart |= input.restart;
    m_pending.mute |= input.mute;
    m_remainder += std::clamp(static_cast<double>(deltaTime), 0.0, 0.1);
    while (m_remainder >= PixelHop::kFixedDt) {
        m_model.Update(PixelHop::kFixedDt, m_pending);
        m_audio.SetMuted(m_model.muted);
        for (const auto& event : m_model.soundEvents) m_audio.Play(event);
        m_remainder -= PixelHop::kFixedDt;
        m_pending.jumpPressed = false;
        m_pending.jumpReleased = false;
        m_pending.pause = false;
        m_pending.restart = false;
        m_pending.mute = false;
    }
    if (m_model._state != previousState || m_model._levelIndex != previousLevel) {
        LOGI("[PixelHop] Level %d, state=%s, score=%d, lives=%d, victory=%d",
             m_model._levelIndex + 1, m_model._state.c_str(), m_model._score,
             m_model._lives, m_model._victory ? 1 : 0);
    }
    SyncScene();
}

void PixelHopGame::SyncScene() {
    auto& coordinator = ECS::Coordinator::GetInstance();
    if (coordinator.IsAlive(m_player)) {
        ECS::SceneECS::GetInstance().SetPosition(m_player.entity,
            {static_cast<float>(m_model._player.x), static_cast<float>(m_model._player.y), 0.0f});
    }
    if (!coordinator.IsAlive(m_hud)) return;
    const bool playing = m_model._state == "playing";
    const bool paused = m_model._state == "paused";
    const auto visible = [&](const char* id, bool show) {
        const auto entity = UI::FindUiDocumentNode(m_hud.entity, id);
        if (coordinator.IsAlive(entity)) ECS::SceneECS::GetInstance().SetVisible(entity, kMobile && show);
    };
    for (const auto* id : {"move-left", "move-right", "jump"}) visible(id, playing);
    visible("pause", playing || paused);
    visible("restart", !playing && m_model._state != "loading");
    visible("mute", true);
}

void PixelHopGame::OnRenderUI(Renderer2D& renderer, int width, int height) {
    if (!m_loaded) return;
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    m_renderer.Render(renderer, m_model, width, height, static_cast<double>(milliseconds));
    if (kMobile) RenderTouchControls(renderer);
}

void PixelHopGame::RenderTouchControls(Renderer2D& renderer) {
    auto& coordinator = ECS::Coordinator::GetInstance();
    if (!coordinator.IsAlive(m_hud)) return;
    constexpr int layer = 1000000;
    for (const auto* id : {"move-left", "move-right", "jump", "pause", "restart", "mute"}) {
        const auto entity = UI::FindUiDocumentNode(m_hud.entity, id);
        glm::vec2 position, size;
        if (!coordinator.IsAlive(entity) || !UI::Canvas2D::GetEntityLayout(entity, position, size)) continue;
        const auto& button = coordinator.GetComponent<ECS::ButtonComponent>(entity);
        if (!button.visible) continue;
        const bool held = UI::Canvas2D::GetInstance().IsActionHeld(id);
        renderer.DrawRect(position, size, held ? glm::vec4(0.22f, 0.28f, 0.55f, 0.93f) :
                          glm::vec4(0.06f, 0.08f, 0.16f, 0.82f), layer);
        constexpr std::array<int, 6> sizes{14, 16, 20, 24, 28, 32};
        const float desired = std::clamp(size.y * 0.28f, 14.0f, 32.0f);
        const auto selected = std::min_element(sizes.begin(), sizes.end(),
            [desired](int first, int second) {
                return std::abs(first - desired) < std::abs(second - desired);
            });
        const glm::vec2 baseline = position + size * 0.5f + glm::vec2(0.0f, *selected * 0.35f);
        m_renderer.DrawLabel(renderer, button.text, baseline, *selected,
                             glm::vec4(0.75f, 0.82f, 1.0f, 1.0f), layer + 1);

    }
}

} // namespace Game
