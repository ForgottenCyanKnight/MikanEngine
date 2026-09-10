#include "Game/IGameModule.h"
#include "Game/GameManager.h"
#include "ECS/SceneECS.h"
#include "Rendering/CpuClothSimulation.h"
#include "Rendering/Renderer2D.h"

#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace {

constexpr float kFixedStep = 1.0f / 120.0f;
constexpr int kMaxSubstepsPerFrame = 8;

class ClothSpringPrototype final : public Game::IGameModule {
public:
    static ClothSpringPrototype& GetInstance() {
        static ClothSpringPrototype instance;
        return instance;
    }

    const char* GetName() const override { return "clothspring"; }

    void OnSceneLoaded() override {
        m_clothEntity = ECS::INVALID_ENTITY;
        m_lastEntitySetVersion = UINT32_MAX;
        m_hasLastTransform = false;
        m_missingClothReported = false;
        m_timeAccumulator = 0.0f;
        m_paused = true;

        CpuClothSimulation& simulation = CpuClothSimulation::GetInstance();
        simulation.ResetTransform();
        simulation.Reset();
        SyncClothTransform();
        std::printf(
            "[ClothSpring] loaded: patches=2 leftPinnedFlat particles=%u springs=%u pinned=%u sphereRadius=%.2f fixedStep=%.5f\n",
            simulation.GetParticleCount(), simulation.GetSpringCount(),
            simulation.GetPinnedParticleCount(),
            simulation.GetWrapSphereRadius(), kFixedStep);
    }

    void OnUpdate(float deltaSeconds) override {
        if (m_paused) return;

        m_timeAccumulator = std::min(
            m_timeAccumulator + std::clamp(deltaSeconds, 0.0f, 0.1f),
            0.25f);
        int substeps = 0;
        while (m_timeAccumulator >= kFixedStep &&
               substeps < kMaxSubstepsPerFrame) {
            CpuClothSimulation::GetInstance().Step(kFixedStep);
            m_timeAccumulator -= kFixedStep;
            ++substeps;
        }
    }

    // Scene Transform owns the cloth's world frame; no keyboard binding is
    // added so camera movement and cloth authoring never compete for input.
    void OnAlwaysUpdate(float) override {
        SyncClothTransform();
    }

    void OnKey(SDL_Keycode) override {}

    void OnRenderUI(Renderer2D& renderer, int viewWidth,
                    int viewHeight) override {
        if (viewWidth <= 0 || viewHeight <= 0) return;

        const CpuClothSimulation& simulation =
            CpuClothSimulation::GetInstance();
        const float timeProgress = std::clamp(
            simulation.GetSimulationTime() / 4.0f, 0.0f, 1.0f);
        renderer.DrawRect(glm::vec2(24.0f, 24.0f), glm::vec2(280.0f, 12.0f),
                          glm::vec4(0.02f, 0.04f, 0.08f, 0.75f), 20);
        renderer.DrawRect(
            glm::vec2(26.0f, 26.0f), glm::vec2(276.0f * timeProgress, 8.0f),
            m_paused ? glm::vec4(0.95f, 0.62f, 0.16f, 0.92f)
                     : glm::vec4(0.10f, 0.78f, 0.95f, 0.92f),
            21);

        // Legend: structural/shear/bend springs use cyan/purple/orange.
        renderer.DrawRect(glm::vec2(24.0f, 46.0f), glm::vec2(280.0f, 10.0f),
                          glm::vec4(0.10f, 0.88f, 1.00f, 0.96f), 21);
        renderer.DrawRect(glm::vec2(24.0f, 58.0f), glm::vec2(280.0f, 10.0f),
                          glm::vec4(0.82f, 0.26f, 1.00f, 0.90f), 21);
        renderer.DrawRect(glm::vec2(24.0f, 70.0f), glm::vec2(280.0f, 10.0f),
                          glm::vec4(1.00f, 0.58f, 0.12f, 0.88f), 21);
    }

    void OnGameStart() override { m_paused = false; }
    void OnGamePause() override { m_paused = true; }
    void OnGameResume() override { m_paused = false; }

    void OnGameStop() override {
        m_paused = true;
        m_timeAccumulator = 0.0f;
        CpuClothSimulation::GetInstance().Reset();
    }

private:
    ClothSpringPrototype() = default;
    ~ClothSpringPrototype() override = default;
    ClothSpringPrototype(const ClothSpringPrototype&) = delete;
    ClothSpringPrototype& operator=(const ClothSpringPrototype&) = delete;

    void SyncClothTransform() {
        constexpr const char* kClothName = "CPU Cloth";
        ECS::SceneECS& scene = ECS::SceneECS::GetInstance();
        const uint32_t entitySetVersion = scene.GetEntitySetVersion();
        if (entitySetVersion != m_lastEntitySetVersion) {
            m_lastEntitySetVersion = entitySetVersion;
            m_clothEntity = scene.FindByName(kClothName);
            m_hasLastTransform = false;
            m_missingClothReported = false;
        }

        if (m_clothEntity == ECS::INVALID_ENTITY) {
            if (!m_missingClothReported) {
                std::printf(
                    "[ClothSpring] scene entity '%s' not found; using default transform\n",
                    kClothName);
                m_missingClothReported = true;
            }
            return;
        }

        const glm::mat4 worldMatrix = scene.GetWorldMatrix(m_clothEntity);
        glm::mat3 rotationBasis(worldMatrix);
        for (int axis = 0; axis < 3; ++axis) {
            const float axisLength = glm::length(rotationBasis[axis]);
            if (axisLength <= 0.0001f) return;
            rotationBasis[axis] /= axisLength;
        }

        const glm::vec3 center = glm::vec3(worldMatrix[3]);
        const glm::quat rotation =
            glm::normalize(glm::quat_cast(rotationBasis));
        if (!m_hasLastTransform ||
            glm::length(center - m_lastCenter) > 0.0001f ||
            std::abs(glm::dot(m_lastRotation, rotation)) < 0.99999f) {
            CpuClothSimulation::GetInstance().SetTransform(center, rotation);
            m_lastCenter = center;
            m_lastRotation = rotation;
            m_hasLastTransform = true;
            std::printf("[ClothSpring] transform synced from scene entity '%s'\n",
                        kClothName);
        }
    }

    ECS::Entity m_clothEntity = ECS::INVALID_ENTITY;
    uint32_t m_lastEntitySetVersion = UINT32_MAX;
    glm::vec3 m_lastCenter = glm::vec3(0.0f);
    glm::quat m_lastRotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    float m_timeAccumulator = 0.0f;
    bool m_hasLastTransform = false;
    bool m_missingClothReported = false;
    bool m_paused = true;
};

} // namespace

#ifndef __ANDROID__
extern "C" __declspec(dllexport) const char* GetGameModuleName() {
    return "clothspring";
}

extern "C" __declspec(dllexport) Game::IGameModule* CreateGameModule() {
    return &ClothSpringPrototype::GetInstance();
}
#else
namespace {
struct ClothSpringAndroidRegistration {
    ClothSpringAndroidRegistration() {
        Game::GameManager::GetInstance().Register(
            "clothspring", []() -> Game::IGameModule* {
                return &ClothSpringPrototype::GetInstance();
            });
    }
};
static ClothSpringAndroidRegistration g_clothSpringAndroidRegistration;
}
#endif
