#include "ParticleSystem.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr float kMinimumLifetime = 0.001f;
constexpr float kMaximumSimulationStep = 0.1f;

float ClampNonNegative(float value) {
    return std::max(0.0f, value);
}

} // namespace

ParticleSystem& ParticleSystem::GetInstance() {
    static ParticleSystem instance;
    return instance;
}

ParticleEmitterConfig ParticleSystem::NormalizeConfig(const ParticleEmitterConfig& config) {
    ParticleEmitterConfig normalized = config;
    normalized.maxParticles = std::max(1u, config.maxParticles);
    normalized.emissionRate = ClampNonNegative(config.emissionRate);
    normalized.particleLifetime = std::max(kMinimumLifetime, config.particleLifetime);
    normalized.spawnOffsetMin = glm::min(config.spawnOffsetMin, config.spawnOffsetMax);
    normalized.spawnOffsetMax = glm::max(config.spawnOffsetMin, config.spawnOffsetMax);
    normalized.initialVelocityMin = glm::min(config.initialVelocityMin, config.initialVelocityMax);
    normalized.initialVelocityMax = glm::max(config.initialVelocityMin, config.initialVelocityMax);
    normalized.startSize = ClampNonNegative(config.startSize);
    normalized.endSize = ClampNonNegative(config.endSize);
    if (normalized.rotationSpeedMin > normalized.rotationSpeedMax) {
        std::swap(normalized.rotationSpeedMin, normalized.rotationSpeedMax);
    }
    return normalized;
}

ParticleEmitterHandle ParticleSystem::CreateEmitter(const ParticleEmitterConfig& config) {
    EmitterState emitter;
    emitter.config = NormalizeConfig(config);
    emitter.particles.reserve(emitter.config.maxParticles);

    // 给每个发射器一个不同但确定的随机序列，便于复现玩法测试。
    emitter.randomState = 0x9E3779B9u ^
                          static_cast<uint32_t>((m_Emitters.size() + 1u) * 0x85EBCA6Bu);
    m_Emitters.push_back(std::move(emitter));
    return static_cast<ParticleEmitterHandle>(m_Emitters.size());
}

ParticleSystem::EmitterState* ParticleSystem::GetEmitter(ParticleEmitterHandle handle) {
    if (handle == kInvalidParticleEmitter) return nullptr;
    const size_t index = static_cast<size_t>(handle - 1u);
    if (index >= m_Emitters.size() || !m_Emitters[index].alive) return nullptr;
    return &m_Emitters[index];
}

const ParticleSystem::EmitterState* ParticleSystem::GetEmitter(ParticleEmitterHandle handle) const {
    if (handle == kInvalidParticleEmitter) return nullptr;
    const size_t index = static_cast<size_t>(handle - 1u);
    if (index >= m_Emitters.size() || !m_Emitters[index].alive) return nullptr;
    return &m_Emitters[index];
}

bool ParticleSystem::IsEmitterValid(ParticleEmitterHandle handle) const {
    return GetEmitter(handle) != nullptr;
}

bool ParticleSystem::SetEmitterConfig(ParticleEmitterHandle handle,
                                       const ParticleEmitterConfig& config) {
    EmitterState* emitter = GetEmitter(handle);
    if (emitter == nullptr) return false;

    emitter->config = NormalizeConfig(config);
    if (emitter->particles.size() > emitter->config.maxParticles) {
        emitter->particles.resize(emitter->config.maxParticles);
    }
    emitter->particles.reserve(emitter->config.maxParticles);
    RebuildRenderInstances();
    return true;
}

bool ParticleSystem::SetEmitterPosition(ParticleEmitterHandle handle,
                                        const glm::vec3& position) {
    EmitterState* emitter = GetEmitter(handle);
    if (emitter == nullptr) return false;
    emitter->config.position = position;
    return true;
}

bool ParticleSystem::SetEmitterEnabled(ParticleEmitterHandle handle, bool enabled) {
    EmitterState* emitter = GetEmitter(handle);
    if (emitter == nullptr) return false;
    emitter->config.enabled = enabled;
    RebuildRenderInstances();
    return true;
}

void ParticleSystem::DestroyEmitter(ParticleEmitterHandle handle) {
    EmitterState* emitter = GetEmitter(handle);
    if (emitter == nullptr) return;
    emitter->particles.clear();
    emitter->particles.shrink_to_fit();
    emitter->alive = false;
    RebuildRenderInstances();
}

size_t ParticleSystem::GetEmitterCount() const {
    size_t count = 0;
    for (const EmitterState& emitter : m_Emitters) {
        if (emitter.alive) ++count;
    }
    return count;
}

uint32_t ParticleSystem::NextRandom(EmitterState& emitter) const {
    // Small deterministic LCG: no dependency on platform RNG and no heap work
    // in the hot path. Visual randomness is sufficient for this first stage.
    emitter.randomState = emitter.randomState * 1664525u + 1013904223u;
    return emitter.randomState;
}

float ParticleSystem::Random01(EmitterState& emitter) const {
    constexpr float kInvUintMax = 1.0f / 4294967295.0f;
    return static_cast<float>(NextRandom(emitter)) * kInvUintMax;
}

float ParticleSystem::RandomRange(EmitterState& emitter, float minValue, float maxValue) const {
    return minValue + (maxValue - minValue) * Random01(emitter);
}

glm::vec3 ParticleSystem::RandomRange(EmitterState& emitter, const glm::vec3& minValue,
                                      const glm::vec3& maxValue) const {
    return glm::vec3(
        RandomRange(emitter, minValue.x, maxValue.x),
        RandomRange(emitter, minValue.y, maxValue.y),
        RandomRange(emitter, minValue.z, maxValue.z));
}

void ParticleSystem::SpawnParticle(EmitterState& emitter) {
    if (!emitter.alive || emitter.particles.size() >= emitter.config.maxParticles) return;

    ParticleState particle;
    particle.position = emitter.config.position +
                        RandomRange(emitter, emitter.config.spawnOffsetMin,
                                    emitter.config.spawnOffsetMax);
    particle.velocity = RandomRange(emitter, emitter.config.initialVelocityMin,
                                    emitter.config.initialVelocityMax);
    particle.lifetime = emitter.config.particleLifetime;
    particle.startSize = emitter.config.startSize;
    particle.endSize = emitter.config.endSize;
    particle.startColor = emitter.config.startColor;
    particle.endColor = emitter.config.endColor;
    particle.rotation = emitter.config.startRotation;
    particle.rotationSpeed = RandomRange(emitter, emitter.config.rotationSpeedMin,
                                          emitter.config.rotationSpeedMax);
    particle.blendMode = emitter.config.blendMode;
    emitter.particles.push_back(particle);
}

void ParticleSystem::EmitBurst(ParticleEmitterHandle handle, uint32_t count) {
    EmitterState* emitter = GetEmitter(handle);
    if (emitter == nullptr) return;

    const size_t freeSlots = emitter->config.maxParticles - emitter->particles.size();
    const size_t spawnCount = std::min<size_t>(freeSlots, count);
    for (size_t i = 0; i < spawnCount; ++i) {
        SpawnParticle(*emitter);
    }
    RebuildRenderInstances();
}

void ParticleSystem::Update(float deltaSeconds) {
    const float dt = std::clamp(deltaSeconds, 0.0f, kMaximumSimulationStep);

    for (EmitterState& emitter : m_Emitters) {
        if (!emitter.alive) continue;

        if (emitter.config.enabled && emitter.config.emissionRate > 0.0f && dt > 0.0f) {
            emitter.emissionAccumulator += emitter.config.emissionRate * dt;
            const uint32_t requested = static_cast<uint32_t>(
                std::floor(emitter.emissionAccumulator));
            if (requested > 0u) {
                emitter.emissionAccumulator -= static_cast<float>(requested);
                const size_t freeSlots = emitter.config.maxParticles - emitter.particles.size();
                const size_t spawnCount = std::min<size_t>(freeSlots, requested);
                for (size_t i = 0; i < spawnCount; ++i) {
                    SpawnParticle(emitter);
                }
            }
            // 防止一个长时间满载的发射器积累一个巨大的待发射数。
            emitter.emissionAccumulator = std::min(emitter.emissionAccumulator, 1.0f);
        }

        for (size_t i = 0; i < emitter.particles.size();) {
            ParticleState& particle = emitter.particles[i];
            particle.age += dt;
            if (particle.age >= particle.lifetime) {
                particle = std::move(emitter.particles.back());
                emitter.particles.pop_back();
                continue;
            }

            particle.velocity += emitter.config.gravity * dt;
            particle.position += particle.velocity * dt;
            particle.rotation += particle.rotationSpeed * dt;
            ++i;
        }
    }

    RebuildRenderInstances();
}

void ParticleSystem::RebuildRenderInstances() {
    size_t activeCount = 0;
    for (const EmitterState& emitter : m_Emitters) {
        if (emitter.alive && emitter.config.enabled) activeCount += emitter.particles.size();
    }
    m_RenderInstances.clear();
    m_RenderInstances.reserve(activeCount);

    for (const EmitterState& emitter : m_Emitters) {
        if (!emitter.alive || !emitter.config.enabled) continue;
        for (const ParticleState& particle : emitter.particles) {
            const float t = std::clamp(particle.age / particle.lifetime, 0.0f, 1.0f);
            const float size = particle.startSize +
                               (particle.endSize - particle.startSize) * t;
            const glm::vec4 color = particle.startColor +
                                     (particle.endColor - particle.startColor) * t;

            ParticleInstance instance;
            instance.positionSize = glm::vec4(particle.position, std::max(0.0f, size));
            instance.color = glm::clamp(color, glm::vec4(0.0f), glm::vec4(1.0f));
            instance.rotationBlend = glm::vec4(
                particle.rotation,
                static_cast<float>(static_cast<uint32_t>(particle.blendMode)),
                0.0f, 0.0f);
            m_RenderInstances.push_back(instance);
        }
    }
}

void ParticleSystem::Clear() {
    m_Emitters.clear();
    m_RenderInstances.clear();
}
