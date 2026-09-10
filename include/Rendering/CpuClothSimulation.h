#pragma once

#include "Platform/Export.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cstdint>
#include <vector>

// CPU cloth render stream. The simulation keeps its particles and spring
// topology private; this compact vertex format is the hand-off to the Vulkan
// cloth renderer and is also useful for future debug views.
struct MIKAN_API ClothRenderVertex {
    glm::vec4 position = glm::vec4(0.0f);
    glm::vec4 color = glm::vec4(1.0f);
};

static_assert(sizeof(ClothRenderVertex) == sizeof(float) * 8,
              "ClothRenderVertex must remain two tightly packed vec4 values");

// A deliberately small CPU mass-spring cloth prototype. It uses two regular
// particle lattices: a left flat sheet with one pinned edge and a right square
// sheet that falls onto a sphere. Structural, shear and bend springs are
// integrated with semi-implicit Euler and then stabilized with positional
// constraint projection. The solver lives in local cloth coordinates; the scene
// Transform supplies the shared render frame and rotates world gravity into
// that local frame.
class MIKAN_API CpuClothSimulation {
public:
    static CpuClothSimulation& GetInstance();

    CpuClothSimulation(const CpuClothSimulation&) = delete;
    CpuClothSimulation& operator=(const CpuClothSimulation&) = delete;

    void Reset();
    void Step(float deltaSeconds);

    void SetTransform(const glm::vec3& center, const glm::quat& rotation);
    void ResetTransform();

    bool IsInitialized() const { return m_initialized; }
    uint32_t GetParticleCount() const;
    uint32_t GetSpringCount() const;
    uint32_t GetPinnedParticleCount() const;
    float GetSimulationTime() const { return m_simulationTime; }
    glm::vec3 GetCenter() const { return m_center; }
    glm::quat GetRotation() const { return m_rotation; }
    glm::vec3 GetWrapSphereCenter() const { return m_wrapSphereCenter; }
    float GetWrapSphereRadius() const { return m_wrapSphereRadius; }

    // Positions remain local to the cloth Transform. The renderer calls this
    // once per frame and uploads the resulting surface/edge streams to Vulkan.
    void BuildRenderVertices(std::vector<ClothRenderVertex>& surface,
                             std::vector<ClothRenderVertex>& springs) const;

private:
    CpuClothSimulation() = default;
    ~CpuClothSimulation() = default;

    enum class SpringKind : uint8_t {
        Structural,
        Shear,
        Bend,
    };

    enum class ClothPatchKind : uint8_t {
        PinnedFlat,
        FallingWrapSphere,
    };

    struct Particle {
        glm::vec3 position = glm::vec3(0.0f);
        glm::vec3 previousPosition = glm::vec3(0.0f);
        glm::vec3 velocity = glm::vec3(0.0f);
        glm::vec3 force = glm::vec3(0.0f);
        bool pinned = false;
    };

    struct Spring {
        uint32_t a = 0;
        uint32_t b = 0;
        float restLength = 0.0f;
        float stiffness = 0.0f;
        float damping = 0.0f;
        SpringKind kind = SpringKind::Structural;
    };

    struct ClothPatch {
        ClothPatchKind kind = ClothPatchKind::PinnedFlat;
        glm::vec3 offset = glm::vec3(0.0f);
        std::vector<Particle> particles;
        std::vector<Spring> springs;
    };

    static constexpr uint32_t kColumns = 28;
    static constexpr uint32_t kRows = 20;
    static constexpr float kSpacing = 0.12f;
    static constexpr float kTopY = 1.30f;

    uint32_t Index(uint32_t x, uint32_t y) const {
        return y * kColumns + x;
    }
    void ResetPatch(ClothPatch& patch, ClothPatchKind kind,
                    const glm::vec3& offset, float topY);
    void AddSpring(ClothPatch& patch, uint32_t a, uint32_t b,
                   SpringKind kind);
    glm::vec3 EstimateNormal(const ClothPatch& patch, uint32_t x,
                             uint32_t y) const;
    void ProjectConstraints(ClothPatch& patch) const;
    void SolveCollisions(const ClothPatch& patch, Particle& particle) const;

    std::array<ClothPatch, 2> m_patches;

    glm::vec3 m_center = glm::vec3(0.0f, 0.45f, 0.0f);
    glm::quat m_rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    glm::vec3 m_localGravity = glm::vec3(0.0f, -9.8f, 0.0f);
    glm::vec3 m_worldGravity = glm::vec3(0.0f, -9.8f, 0.0f);
    glm::vec3 m_wrapSphereCenter = glm::vec3(1.85f, -0.45f, 0.12f);
    float m_wrapSphereRadius = 0.60f;
    float m_simulationTime = 0.0f;
    bool m_initialized = false;
};
