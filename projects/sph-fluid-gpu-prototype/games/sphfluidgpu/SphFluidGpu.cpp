#include "Game/IGameModule.h"
#include "Game/GameManager.h"
#include "ECS/SceneECS.h"
#include "Rendering/GpuSphSimulation.h"
#include "Rendering/Renderer2D.h"

#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>

namespace {

glm::vec4 PressureColor(float value) {
    const glm::vec4 low(0.04f, 0.22f, 1.00f, 0.92f);
    const glm::vec4 mid(0.02f, 0.95f, 0.82f, 0.92f);
    const glm::vec4 high(1.00f, 0.10f, 0.02f, 0.92f);
    value = std::clamp(value, 0.0f, 1.0f);
    return value < 0.5f
        ? glm::mix(low, mid, value * 2.0f)
        : glm::mix(mid, high, (value - 0.5f) * 2.0f);
}

class SphFluidGpuPrototype final : public Game::IGameModule {
public:
    static SphFluidGpuPrototype& GetInstance() {
        static SphFluidGpuPrototype instance;
        return instance;
    }

    const char* GetName() const override { return "sphfluidgpu"; }

    void OnSceneLoaded() override {
        m_containerEntity = ECS::INVALID_ENTITY;
        m_lastEntitySetVersion = UINT32_MAX;
        m_hasLastContainerRotation = false;
        m_missingContainerReported = false;
        GpuSphSimulation& simulation = GpuSphSimulation::GetInstance();
        simulation.SetPlaybackActive(false);
        simulation.SetPaused(true);
        simulation.ResetContainerRotation();
        simulation.Reset();
        SyncContainerTransform();
        std::printf("[GpuSph] scene loaded: compute SPH will initialize on the first Vulkan frame\n");
    }

    // The engine records the compute passes from FrameRender so that the
    // simulation is submitted exactly once even when SceneView and GameView
    // are both visible. The game module only owns input/lifecycle concerns.
    void OnUpdate(float) override {}

    // Camera and global input keep ownership of keyboard events. Container
    // rotation is authored by the scene-tree Transform below.
    void OnKey(SDL_Keycode) override {}

    void OnAlwaysUpdate(float) override {
        SyncContainerTransform();
    }

    void OnRenderUI(Renderer2D& renderer, int viewWidth, int viewHeight) override {
        if (viewWidth <= 0 || viewHeight <= 0) return;

        constexpr float kMaxParticles = 8192.0f;
        const float occupancy = std::clamp(
            static_cast<float>(GpuSphSimulation::GetInstance().GetParticleCount()) /
                kMaxParticles,
            0.0f, 1.0f);
        const bool paused = GpuSphSimulation::GetInstance().IsPaused();
        renderer.DrawRect(glm::vec2(24.0f, 24.0f), glm::vec2(260.0f, 12.0f),
                          glm::vec4(0.02f, 0.04f, 0.08f, 0.75f), 20);
        renderer.DrawRect(glm::vec2(26.0f, 26.0f),
                          glm::vec2(256.0f * occupancy, 8.0f),
                          paused ? glm::vec4(0.95f, 0.62f, 0.16f, 0.92f)
                                 : glm::vec4(0.10f, 0.58f, 0.95f, 0.92f),
                          21);

        // Pressure legend: blue = low pressure, red = high pressure.
        renderer.DrawRect(glm::vec2(24.0f, 46.0f), glm::vec2(260.0f, 14.0f),
                          glm::vec4(0.02f, 0.04f, 0.08f, 0.75f), 20);
        constexpr int kLegendSteps = 10;
        constexpr float kLegendWidth = 256.0f / kLegendSteps;
        for (int i = 0; i < kLegendSteps; ++i) {
            const float t = (static_cast<float>(i) + 0.5f) /
                            static_cast<float>(kLegendSteps);
            renderer.DrawRect(glm::vec2(26.0f + i * kLegendWidth, 49.0f),
                              glm::vec2(kLegendWidth, 8.0f), PressureColor(t), 21);
        }
    }

    void OnGameStart() override {
        GpuSphSimulation& simulation = GpuSphSimulation::GetInstance();
        simulation.SetPlaybackActive(true);
        simulation.SetPaused(false);
    }

    void OnGamePause() override {
        GpuSphSimulation& simulation = GpuSphSimulation::GetInstance();
        simulation.SetPlaybackActive(false);
        simulation.SetPaused(true);
    }

    void OnGameResume() override {
        GpuSphSimulation& simulation = GpuSphSimulation::GetInstance();
        simulation.SetPlaybackActive(true);
        simulation.SetPaused(false);
    }

    void OnGameStop() override {
        GpuSphSimulation& simulation = GpuSphSimulation::GetInstance();
        simulation.SetPlaybackActive(false);
        simulation.SetPaused(true);
        simulation.Reset();
    }

private:
    void SyncContainerTransform() {
        constexpr const char* kContainerName = "GPU SPH Container";
        ECS::SceneECS& scene = ECS::SceneECS::GetInstance();
        const uint32_t entitySetVersion = scene.GetEntitySetVersion();
        if (entitySetVersion != m_lastEntitySetVersion) {
            m_lastEntitySetVersion = entitySetVersion;
            m_containerEntity = scene.FindByName(kContainerName);
            m_hasLastContainerRotation = false;
            m_missingContainerReported = false;
        }

        if (m_containerEntity == ECS::INVALID_ENTITY) {
            if (!m_missingContainerReported) {
                std::printf("[GpuSph] scene entity '%s' not found; using identity container rotation\n",
                            kContainerName);
                m_missingContainerReported = true;
            }
            return;
        }

        const glm::mat4 worldMatrix = scene.GetWorldMatrix(m_containerEntity);
        glm::mat3 rotationBasis(worldMatrix);
        for (int axis = 0; axis < 3; ++axis) {
            const float axisLength = glm::length(rotationBasis[axis]);
            if (axisLength <= 0.0001f) return;
            rotationBasis[axis] /= axisLength;
        }

        const glm::quat sceneRotation =
            glm::normalize(glm::quat_cast(rotationBasis));
        if (!m_hasLastContainerRotation ||
            std::abs(glm::dot(m_lastContainerRotation, sceneRotation)) < 0.99999f) {
            GpuSphSimulation::GetInstance().SetContainerRotation(sceneRotation);
            m_lastContainerRotation = sceneRotation;
            m_hasLastContainerRotation = true;
            std::printf("[GpuSph] container rotation synced from scene entity '%s'\n",
                        kContainerName);
        }
    }

    SphFluidGpuPrototype() = default;
    ~SphFluidGpuPrototype() override = default;
    SphFluidGpuPrototype(const SphFluidGpuPrototype&) = delete;
    SphFluidGpuPrototype& operator=(const SphFluidGpuPrototype&) = delete;

    ECS::Entity m_containerEntity = ECS::INVALID_ENTITY;
    uint32_t m_lastEntitySetVersion = UINT32_MAX;
    glm::quat m_lastContainerRotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    bool m_hasLastContainerRotation = false;
    bool m_missingContainerReported = false;
};

} // namespace

#ifndef __ANDROID__
extern "C" __declspec(dllexport) const char* GetGameModuleName() {
    return "sphfluidgpu";
}

extern "C" __declspec(dllexport) Game::IGameModule* CreateGameModule() {
    return &SphFluidGpuPrototype::GetInstance();
}
#else
namespace {
struct SphFluidGpuAndroidRegistration {
    SphFluidGpuAndroidRegistration() {
        Game::GameManager::GetInstance().Register(
            "sphfluidgpu", []() -> Game::IGameModule* {
                return &SphFluidGpuPrototype::GetInstance();
            });
    }
};
static SphFluidGpuAndroidRegistration g_sphFluidGpuAndroidRegistration;
}
#endif
