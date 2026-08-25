#pragma once

#include "Platform/Export.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

// 第一阶段粒子混合模式。渲染器会把两种模式拆成两个实例化 draw，
// 因此同一个发射器内的粒子仍然可以保持统一的 GPU 管线状态。
enum class ParticleBlendMode : uint32_t {
    Alpha = 0,
    Additive = 1,
};

using ParticleEmitterHandle = uint32_t;
constexpr ParticleEmitterHandle kInvalidParticleEmitter = 0;

// CPU 发射器配置。第一阶段只描述常用的点发射器，后续可在不改变
// ParticleInstance GPU 布局的前提下扩展球形/盒形发射区域和纹理图集。
struct MIKAN_API ParticleEmitterConfig {
    uint32_t maxParticles = 256;
    float emissionRate = 0.0f;          // 每秒发射数；0 表示只接受 EmitBurst
    float particleLifetime = 1.0f;

    glm::vec3 position = glm::vec3(0.0f);
    glm::vec3 spawnOffsetMin = glm::vec3(0.0f);
    glm::vec3 spawnOffsetMax = glm::vec3(0.0f);
    glm::vec3 initialVelocityMin = glm::vec3(-0.25f, 0.5f, -0.25f);
    glm::vec3 initialVelocityMax = glm::vec3(0.25f, 1.0f, 0.25f);
    glm::vec3 gravity = glm::vec3(0.0f, -1.0f, 0.0f);

    float startSize = 0.20f;
    float endSize = 0.05f;
    glm::vec4 startColor = glm::vec4(1.0f);
    glm::vec4 endColor = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);

    float startRotation = 0.0f;
    float rotationSpeedMin = -1.0f;
    float rotationSpeedMax = 1.0f;
    ParticleBlendMode blendMode = ParticleBlendMode::Alpha;
    bool enabled = true;
};

// GPU 实例流：每个粒子一个 48-byte 实例。rotationBlend.y 保留混合模式，
// z/w 为后续纹理图集 UV 矩形预留，避免第一阶段之后重新设计实例 ABI。
struct MIKAN_API ParticleInstance {
    glm::vec4 positionSize = glm::vec4(0.0f); // xyz=world position, w=size
    glm::vec4 color = glm::vec4(1.0f);
    glm::vec4 rotationBlend = glm::vec4(0.0f); // x=rotation, y=ParticleBlendMode
};

static_assert(sizeof(ParticleInstance) == sizeof(float) * 12,
              "ParticleInstance must stay 48 bytes for the instanced vertex stream");

class MIKAN_API ParticleSystem {
public:
    static ParticleSystem& GetInstance();

    ParticleSystem(const ParticleSystem&) = delete;
    ParticleSystem& operator=(const ParticleSystem&) = delete;

    ParticleEmitterHandle CreateEmitter(const ParticleEmitterConfig& config = {});
    void DestroyEmitter(ParticleEmitterHandle handle);
    bool IsEmitterValid(ParticleEmitterHandle handle) const;

    bool SetEmitterConfig(ParticleEmitterHandle handle, const ParticleEmitterConfig& config);
    bool SetEmitterPosition(ParticleEmitterHandle handle, const glm::vec3& position);
    bool SetEmitterEnabled(ParticleEmitterHandle handle, bool enabled);
    void EmitBurst(ParticleEmitterHandle handle, uint32_t count);

    // 由游戏逻辑阶段每帧调用一次。渲染器只读 GetRenderInstances()，
    // 不在多视口渲染期间重复模拟粒子。
    void Update(float deltaSeconds);

    const std::vector<ParticleInstance>& GetRenderInstances() const { return m_RenderInstances; }
    size_t GetActiveParticleCount() const { return m_RenderInstances.size(); }
    size_t GetEmitterCount() const;

    // 场景/游戏重载时清理运行时粒子，不触碰 Vulkan 资源。
    void Clear();

private:
    struct ParticleState {
        glm::vec3 position = glm::vec3(0.0f);
        glm::vec3 velocity = glm::vec3(0.0f);
        float age = 0.0f;
        float lifetime = 1.0f;
        float startSize = 0.0f;
        float endSize = 0.0f;
        glm::vec4 startColor = glm::vec4(1.0f);
        glm::vec4 endColor = glm::vec4(1.0f);
        float rotation = 0.0f;
        float rotationSpeed = 0.0f;
        ParticleBlendMode blendMode = ParticleBlendMode::Alpha;
    };

    struct EmitterState {
        ParticleEmitterConfig config{};
        std::vector<ParticleState> particles;
        float emissionAccumulator = 0.0f;
        uint32_t randomState = 0x9E3779B9u;
        bool alive = true;
    };

    ParticleSystem() = default;

    static ParticleEmitterConfig NormalizeConfig(const ParticleEmitterConfig& config);
    EmitterState* GetEmitter(ParticleEmitterHandle handle);
    const EmitterState* GetEmitter(ParticleEmitterHandle handle) const;
    uint32_t NextRandom(EmitterState& emitter) const;
    float Random01(EmitterState& emitter) const;
    float RandomRange(EmitterState& emitter, float minValue, float maxValue) const;
    glm::vec3 RandomRange(EmitterState& emitter, const glm::vec3& minValue,
                          const glm::vec3& maxValue) const;
    void SpawnParticle(EmitterState& emitter);
    void RebuildRenderInstances();

    std::vector<EmitterState> m_Emitters;
    std::vector<ParticleInstance> m_RenderInstances;
};
