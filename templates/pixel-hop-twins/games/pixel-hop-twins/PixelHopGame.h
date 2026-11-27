#pragma once

#include "Game/IGameModule.h"
#include "ECS/Types.h"
#include "NativeModel.h"
#include "NativeRender.h"
#include "NativeAudio.h"

namespace Game {

class PixelHopGame final : public IGameModule {
public:
    static PixelHopGame& GetInstance();
    const char* GetName() const override { return "pixel-hop-twins"; }
    void OnSceneLoaded() override;
    void OnGameStart() override;
    void OnGameStop() override;
    void OnGameplayTestStart() override { OnGameStart(); }
    void OnUpdate(float deltaTime) override;
    void OnRenderUI(Renderer2D& renderer, int width, int height) override;
    void OnKey(SDL_Keycode) override {}

private:
    PixelHop::InputFrame ReadInput();
    void SyncScene();
    void RenderTouchControls(Renderer2D& renderer);

    PixelHop::Model m_model;
    PixelHop::NativeRender m_renderer;
    PixelHop::NativeAudio m_audio;
    ECS::EntityHandle m_player;
    ECS::EntityHandle m_hud;
    PixelHop::InputFrame m_pending;
    bool m_loaded = false;
    bool m_started = false;
    bool m_touchJumpHeld = false;
    double m_remainder = 0;
};

} // namespace Game
