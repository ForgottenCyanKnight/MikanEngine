#include "Game/IGameModule.h"
#include "Game/GameManager.h"
#include "Rendering/ParticleSystem.h"
#include "Rendering/Renderer2D.h"

#include <SDL3/SDL.h>
#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <unordered_map>
#include <vector>

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kFixedStep = 1.0f / 120.0f;
constexpr int kMaxSubstepsPerFrame = 8;

struct GridKey {
    int x = 0;
    int y = 0;
    int z = 0;

    bool operator==(const GridKey& other) const {
        return x == other.x && y == other.y && z == other.z;
    }
};

struct GridKeyHash {
    size_t operator()(const GridKey& key) const noexcept {
        const uint32_t x = static_cast<uint32_t>(key.x) * 73856093u;
        const uint32_t y = static_cast<uint32_t>(key.y) * 19349663u;
        const uint32_t z = static_cast<uint32_t>(key.z) * 83492791u;
        return static_cast<size_t>(x ^ y ^ z);
    }
};

struct SphParticle {
    glm::vec3 position = glm::vec3(0.0f);
    glm::vec3 velocity = glm::vec3(0.0f);
    glm::vec3 acceleration = glm::vec3(0.0f);
    float density = 1000.0f;
    float pressure = 0.0f;
};

class SphSimulation {
public:
    void Reset() {
        m_particles.clear();
        m_grid.clear();
        m_grid.reserve(512);

        constexpr int countX = 14;
        constexpr int countY = 12;
        constexpr int countZ = 14;
        m_particles.reserve(static_cast<size_t>(countX * countY * countZ));

        const glm::vec3 start(-0.72f, 0.32f, -0.72f);
        for (int z = 0; z < countZ; ++z) {
            for (int y = 0; y < countY; ++y) {
                for (int x = 0; x < countX; ++x) {
                    SphParticle particle;
                    particle.position = start + glm::vec3(
                        static_cast<float>(x) * m_particleSpacing,
                        static_cast<float>(y) * m_particleSpacing,
                        static_cast<float>(z) * m_particleSpacing);
                    particle.position += glm::vec3(
                        0.004f * std::sin(static_cast<float>(x * 17 + z * 3)),
                        0.004f * std::cos(static_cast<float>(y * 11 + x)),
                        0.004f * std::sin(static_cast<float>(z * 13 + y)));
                    m_particles.push_back(particle);
                }
            }
        }

        BuildGrid();
        ComputeDensityAndPressure();
        ComputeAccelerations();
    }

    void Step(float deltaSeconds) {
        if (m_particles.empty()) return;

        BuildGrid();
        ComputeDensityAndPressure();
        ComputeAccelerations();

        for (SphParticle& particle : m_particles) {
            particle.velocity += particle.acceleration * deltaSeconds;
            particle.velocity *= std::pow(0.999f, deltaSeconds * 120.0f);
            const float velocityLength = glm::length(particle.velocity);
            if (velocityLength > m_maxVelocity) {
                particle.velocity *= m_maxVelocity / velocityLength;
            }
            particle.position += particle.velocity * deltaSeconds;
            SolveBoundary(particle);
        }
    }

    size_t GetParticleCount() const { return m_particles.size(); }

    void BuildRenderInstances(std::vector<ParticleInstance>& instances) const {
        instances.clear();
        instances.reserve(m_particles.size());

        for (const SphParticle& particle : m_particles) {
            const float densityRatio = std::clamp(
                particle.density / m_restDensity, 0.75f, 1.35f);
            const float blue = std::clamp(0.82f + (densityRatio - 1.0f) * 0.55f,
                                          0.65f, 1.0f);

            ParticleInstance instance;
            instance.positionSize = glm::vec4(particle.position, 0.095f);
            instance.color = glm::vec4(0.10f, 0.48f, blue, 0.88f);
            instance.rotationBlend = glm::vec4(0.0f);
            instances.push_back(instance);
        }
    }

private:
    GridKey CellFor(const glm::vec3& position) const {
        return GridKey{
            static_cast<int>(std::floor(position.x / m_smoothingRadius)),
            static_cast<int>(std::floor(position.y / m_smoothingRadius)),
            static_cast<int>(std::floor(position.z / m_smoothingRadius))};
    }

    template <typename Callback>
    void ForEachNeighbor(size_t particleIndex, Callback&& callback) const {
        const GridKey center = CellFor(m_particles[particleIndex].position);
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const GridKey key{center.x + dx, center.y + dy, center.z + dz};
                    const auto it = m_grid.find(key);
                    if (it == m_grid.end()) continue;
                    for (const uint32_t neighborIndex : it->second) {
                        callback(static_cast<size_t>(neighborIndex));
                    }
                }
            }
        }
    }

    void BuildGrid() {
        m_grid.clear();
        m_grid.reserve(m_particles.size() * 2);
        for (size_t i = 0; i < m_particles.size(); ++i) {
            m_grid[CellFor(m_particles[i].position)].push_back(static_cast<uint32_t>(i));
        }
    }

    float Poly6(float distanceSquared) const {
        const float h2MinusR2 = m_smoothingRadius * m_smoothingRadius - distanceSquared;
        if (h2MinusR2 <= 0.0f) return 0.0f;
        return m_poly6Coefficient * h2MinusR2 * h2MinusR2 * h2MinusR2;
    }

    glm::vec3 SpikyGradient(const glm::vec3& distance, float length) const {
        if (length <= 0.00001f || length >= m_smoothingRadius) {
            return glm::vec3(0.0f);
        }
        const float scale = m_spikyGradientCoefficient *
                            (m_smoothingRadius - length) *
                            (m_smoothingRadius - length) / length;
        return distance * scale;
    }

    float ViscosityLaplacian(float length) const {
        if (length < 0.0f || length >= m_smoothingRadius) return 0.0f;
        return m_viscosityLaplacianCoefficient * (m_smoothingRadius - length);
    }

    void ComputeDensityAndPressure() {
        for (size_t i = 0; i < m_particles.size(); ++i) {
            float density = 0.0f;
            ForEachNeighbor(i, [&](size_t neighborIndex) {
                const glm::vec3 distance = m_particles[i].position -
                                            m_particles[neighborIndex].position;
                density += m_particleMass * Poly6(glm::dot(distance, distance));
            });

            SphParticle& particle = m_particles[i];
            particle.density = std::max(density, m_restDensity * 0.25f);
            // Keep the prototype away from tensile instability when the
            // initial lattice samples below the target density.
            particle.pressure = std::max(
                0.0f, m_pressureStiffness * (particle.density - m_restDensity));
        }
    }

    void ComputeAccelerations() {
        for (size_t i = 0; i < m_particles.size(); ++i) {
            const SphParticle& particle = m_particles[i];
            glm::vec3 pressureForce(0.0f);
            glm::vec3 viscosityForce(0.0f);
            glm::vec3 separationAcceleration(0.0f);

            ForEachNeighbor(i, [&](size_t neighborIndex) {
                if (neighborIndex == i) return;
                const SphParticle& neighbor = m_particles[neighborIndex];
                const glm::vec3 distance = particle.position - neighbor.position;
                const float distanceLength = glm::length(distance);
                if (distanceLength <= 0.00001f || distanceLength >= m_smoothingRadius) return;

                const glm::vec3 gradient = SpikyGradient(distance, distanceLength);
                const float neighborDensity = std::max(neighbor.density, 1.0f);
                const float pressureTerm = particle.pressure /
                        std::max(particle.density * particle.density, 1.0f) +
                    neighbor.pressure / std::max(neighborDensity * neighborDensity, 1.0f);
                pressureForce -= m_particleMass * pressureTerm * gradient;

                viscosityForce += m_viscosity * m_particleMass *
                    (neighbor.velocity - particle.velocity) / neighborDensity *
                    ViscosityLaplacian(distanceLength);

                // The positive-only EOS deliberately avoids tensile
                // instability, but that also leaves sparse boundary layers
                // without a restoring force. Add a small rest-distance
                // correction so the prototype preserves volume while the
                // pressure solver is still intentionally lightweight.
                if (distanceLength < m_particleSpacing) {
                    const float penetration =
                        (m_particleSpacing - distanceLength) / m_particleSpacing;
                    separationAcceleration += (distance / distanceLength) *
                        (m_shortRangeRepulsion * penetration * penetration);
                }
            });

            glm::vec3 acceleration =
                (pressureForce + viscosityForce) / std::max(particle.density, 1.0f) +
                separationAcceleration + BoundaryAcceleration(particle) + m_gravity;
            const float accelerationLength = glm::length(acceleration);
            if (accelerationLength > m_maxAcceleration) {
                acceleration *= m_maxAcceleration / accelerationLength;
            }
            m_particles[i].acceleration = acceleration;
        }
    }

    glm::vec3 BoundaryAcceleration(const SphParticle& particle) const {
        glm::vec3 acceleration(0.0f);
        for (int axis = 0; axis < 3; ++axis) {
            const float lowerDistance = particle.position[axis] - m_lowerBound[axis];
            if (lowerDistance < m_smoothingRadius) {
                const float penetration =
                    (m_smoothingRadius - lowerDistance) / m_smoothingRadius;
                acceleration[axis] += m_boundaryRepulsion * penetration * penetration;
            }

            const float upperDistance = m_upperBound[axis] - particle.position[axis];
            if (upperDistance < m_smoothingRadius) {
                const float penetration =
                    (m_smoothingRadius - upperDistance) / m_smoothingRadius;
                acceleration[axis] -= m_boundaryRepulsion * penetration * penetration;
            }
        }
        return acceleration;
    }

    void SolveBoundary(SphParticle& particle) const {
        constexpr float restitution = 0.35f;
        const float margin = 0.055f;
        for (int axis = 0; axis < 3; ++axis) {
            if (particle.position[axis] < m_lowerBound[axis] + margin) {
                particle.position[axis] = m_lowerBound[axis] + margin;
                if (particle.velocity[axis] < 0.0f) particle.velocity[axis] *= -restitution;
            } else if (particle.position[axis] > m_upperBound[axis] - margin) {
                particle.position[axis] = m_upperBound[axis] - margin;
                if (particle.velocity[axis] > 0.0f) particle.velocity[axis] *= -restitution;
            }
        }
    }

    std::vector<SphParticle> m_particles;
    std::unordered_map<GridKey, std::vector<uint32_t>, GridKeyHash> m_grid;

    const glm::vec3 m_lowerBound = glm::vec3(-1.05f, 0.0f, -1.05f);
    const glm::vec3 m_upperBound = glm::vec3(1.05f, 2.25f, 1.05f);
    const glm::vec3 m_gravity = glm::vec3(0.0f, -9.8f, 0.0f);
    const float m_particleSpacing = 0.11f;
    const float m_smoothingRadius = 0.20f;
    const float m_restDensity = 700.0f;
    const float m_particleMass = 0.95f;
    const float m_pressureStiffness = 350.0f;
    const float m_viscosity = 0.12f;
    const float m_shortRangeRepulsion = 8.0f;
    const float m_boundaryRepulsion = 12.0f;
    const float m_maxAcceleration = 60.0f;
    const float m_maxVelocity = 6.0f;
    const float m_poly6Coefficient =
        315.0f / (64.0f * kPi * std::pow(m_smoothingRadius, 9.0f));
    const float m_spikyGradientCoefficient =
        -45.0f / (kPi * std::pow(m_smoothingRadius, 6.0f));
    const float m_viscosityLaplacianCoefficient =
        45.0f / (kPi * std::pow(m_smoothingRadius, 6.0f));
};

class SphFluidPrototype final : public Game::IGameModule {
public:
    static SphFluidPrototype& GetInstance() {
        static SphFluidPrototype instance;
        return instance;
    }

    const char* GetName() const override { return "sphfluid"; }

    void OnSceneLoaded() override {
        m_simulation.Reset();
        CreateEmitter();
        SyncRenderInstances();
        std::printf("[SphFluid] loaded: particles=%zu, fixedStep=%.5f\n",
                    m_simulation.GetParticleCount(), kFixedStep);
    }

    void OnUpdate(float deltaSeconds) override {
        if (m_paused) return;

        m_timeAccumulator = std::min(m_timeAccumulator +
                                         std::clamp(deltaSeconds, 0.0f, 0.1f),
                                     0.25f);
        int substeps = 0;
        while (m_timeAccumulator >= kFixedStep && substeps < kMaxSubstepsPerFrame) {
            m_simulation.Step(kFixedStep);
            m_timeAccumulator -= kFixedStep;
            ++substeps;
        }

        if (substeps > 0) SyncRenderInstances();
    }

    void OnKey(SDL_Keycode key) override {
        if (key == SDLK_SPACE) {
            m_paused = !m_paused;
            std::printf("[SphFluid] %s\n", m_paused ? "paused" : "running");
        } else if (key == SDLK_R) {
            m_simulation.Reset();
            SyncRenderInstances();
            std::printf("[SphFluid] reset\n");
        }
    }

    void OnRenderUI(Renderer2D& renderer, int viewWidth, int viewHeight) override {
        if (viewWidth <= 0 || viewHeight <= 0) return;

        const float occupancy = std::clamp(
            static_cast<float>(m_simulation.GetParticleCount()) / 2400.0f, 0.0f, 1.0f);
        renderer.DrawRect(glm::vec2(24.0f, 24.0f), glm::vec2(220.0f, 12.0f),
                          glm::vec4(0.02f, 0.04f, 0.08f, 0.75f), 20);
        renderer.DrawRect(glm::vec2(26.0f, 26.0f), glm::vec2(216.0f * occupancy, 8.0f),
                          m_paused ? glm::vec4(0.95f, 0.62f, 0.16f, 0.92f)
                                   : glm::vec4(0.10f, 0.58f, 0.95f, 0.92f),
                          21);
    }

    void OnGameStart() override { m_paused = false; }
    void OnGamePause() override { m_paused = true; }
    void OnGameResume() override { m_paused = false; }

    void OnGameStop() override {
        if (m_emitter != kInvalidParticleEmitter) {
            ParticleSystem::GetInstance().DestroyEmitter(m_emitter);
            m_emitter = kInvalidParticleEmitter;
        }
        m_instances.clear();
        m_timeAccumulator = 0.0f;
    }

private:
    SphFluidPrototype() = default;
    ~SphFluidPrototype() override = default;
    SphFluidPrototype(const SphFluidPrototype&) = delete;
    SphFluidPrototype& operator=(const SphFluidPrototype&) = delete;

    void CreateEmitter() {
        if (m_emitter != kInvalidParticleEmitter) {
            ParticleSystem::GetInstance().DestroyEmitter(m_emitter);
        }

        ParticleEmitterConfig config;
        config.maxParticles = static_cast<uint32_t>(m_simulation.GetParticleCount());
        config.startSize = 0.095f;
        config.endSize = 0.095f;
        config.startColor = glm::vec4(0.10f, 0.48f, 0.95f, 0.88f);
        config.endColor = config.startColor;
        config.emissionRate = 0.0f;
        config.enabled = true;
        m_emitter = ParticleSystem::GetInstance().CreateEmitter(config);
    }

    void SyncRenderInstances() {
        m_simulation.BuildRenderInstances(m_instances);
        if (m_emitter != kInvalidParticleEmitter) {
            ParticleSystem::GetInstance().SetExternalInstances(m_emitter, m_instances);
        }
    }

    SphSimulation m_simulation;
    std::vector<ParticleInstance> m_instances;
    ParticleEmitterHandle m_emitter = kInvalidParticleEmitter;
    float m_timeAccumulator = 0.0f;
    bool m_paused = false;
};

} // namespace

#ifndef __ANDROID__
extern "C" __declspec(dllexport) const char* GetGameModuleName() {
    return "sphfluid";
}

extern "C" __declspec(dllexport) Game::IGameModule* CreateGameModule() {
    return &SphFluidPrototype::GetInstance();
}
#else
namespace {
struct SphFluidAndroidRegistration {
    SphFluidAndroidRegistration() {
        Game::GameManager::GetInstance().Register(
            "sphfluid", []() -> Game::IGameModule* {
                return &SphFluidPrototype::GetInstance();
            });
    }
};
static SphFluidAndroidRegistration g_sphFluidAndroidRegistration;
}
#endif
