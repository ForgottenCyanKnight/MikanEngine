#include "Rendering/CpuClothSimulation.h"

#include <algorithm>
#include <cmath>

#include <glm/gtc/quaternion.hpp>

namespace {

constexpr float kParticleMass = 0.08f;
constexpr float kAirDamping = 3.2f;
// Keep the default prototype deterministic and quiet. Wind can be added back
// after the constraint solver is tuned without masking numerical problems.
constexpr float kWindForce = 0.0f;
constexpr float kMaximumAcceleration = 250.0f;
constexpr float kMaximumSpeed = 18.0f;
constexpr float kProjectionStiffness = 0.88f;
constexpr float kProjectionVelocityDamping = 0.985f;
constexpr uint32_t kProjectionIterations = 5;
constexpr float kFloorY = -1.05f;
constexpr float kCollisionRestitution = 0.05f;
constexpr float kCollisionMargin = 0.006f;
// Strong tangential damping keeps the falling demo visibly draped around the
// sphere instead of immediately sliding off to the floor.
constexpr float kSphereFriction = 0.30f;
// The right patch starts as a horizontal XZ sheet above the sphere.
constexpr float kWrapTopY = 2.10f;
// Make the falling patch a square in world space. Its particle lattice still
// uses the compact 28x20 prototype topology, so the row spacing is expanded
// only for this patch to keep the physical bounds square.
constexpr float kWrapSheetSide = 4.00f;
constexpr uint32_t kSphereSegments = 24;
constexpr uint32_t kSphereRings = 12;
constexpr float kPi = 3.14159265358979323846f;
constexpr float kEpsilon = 0.000001f;

glm::vec4 SurfaceColor() {
    return glm::vec4(0.06f, 0.36f, 0.90f, 0.68f);
}

glm::vec4 SphereColor() {
    return glm::vec4(0.72f, 0.24f, 0.06f, 0.90f);
}

} // namespace

CpuClothSimulation& CpuClothSimulation::GetInstance() {
    static CpuClothSimulation instance;
    return instance;
}

uint32_t CpuClothSimulation::GetParticleCount() const {
    uint32_t count = 0;
    for (const ClothPatch& patch : m_patches) {
        count += static_cast<uint32_t>(patch.particles.size());
    }
    return count;
}

uint32_t CpuClothSimulation::GetSpringCount() const {
    uint32_t count = 0;
    for (const ClothPatch& patch : m_patches) {
        count += static_cast<uint32_t>(patch.springs.size());
    }
    return count;
}

uint32_t CpuClothSimulation::GetPinnedParticleCount() const {
    uint32_t count = 0;
    for (const ClothPatch& patch : m_patches) {
        for (const Particle& particle : patch.particles) {
            if (particle.pinned) ++count;
        }
    }
    return count;
}

void CpuClothSimulation::Reset() {
    // Keep both demonstrations under the scene-controlled root transform.
    // The left sheet is flat with one edge fixed; the right sheet is the
    // centered square drop that wraps the sphere.
    m_wrapSphereCenter = glm::vec3(1.85f, -0.45f, 0.12f);
    ResetPatch(m_patches[0], ClothPatchKind::PinnedFlat,
               glm::vec3(-1.85f, 0.0f, 0.0f), kTopY);
    ResetPatch(m_patches[1], ClothPatchKind::FallingWrapSphere,
               glm::vec3(1.85f, 0.0f, 0.12f), kWrapTopY);

    m_simulationTime = 0.0f;
    m_initialized = true;
}

void CpuClothSimulation::ResetPatch(ClothPatch& patch,
                                    ClothPatchKind kind,
                                    const glm::vec3& offset,
                                    float topY) {
    patch.kind = kind;
    patch.offset = offset;
    patch.particles.clear();
    patch.particles.resize(static_cast<size_t>(kColumns) * kRows);

    for (uint32_t y = 0; y < kRows; ++y) {
        for (uint32_t x = 0; x < kColumns; ++x) {
            Particle& particle = patch.particles[Index(x, y)];
            const bool isWrapPatch = kind == ClothPatchKind::FallingWrapSphere;
            const float patchWidth = isWrapPatch
                ? kWrapSheetSide
                : static_cast<float>(kColumns - 1) * kSpacing;
            const float patchDepth = isWrapPatch
                ? kWrapSheetSide
                : static_cast<float>(kRows - 1) * kSpacing;
            const float spacingX = patchWidth /
                static_cast<float>(kColumns - 1);
            const float spacingZ = patchDepth /
                static_cast<float>(kRows - 1);
            const float centeredX =
                (static_cast<float>(x) - static_cast<float>(kColumns - 1) * 0.5f) *
                spacingX;
            const float centeredZ =
                (static_cast<float>(y) - static_cast<float>(kRows - 1) * 0.5f) *
                spacingZ;
            // Both patches begin as horizontal XZ sheets. The left sheet
            // falls toward the floor while the right sheet is centered over
            // the sphere by its scene offset.
            particle.position = offset +
                glm::vec3(centeredX, topY, centeredZ);
            particle.previousPosition = particle.position;
            particle.velocity = glm::vec3(0.0f);
            particle.force = glm::vec3(0.0f);
            particle.pinned = kind == ClothPatchKind::PinnedFlat && x == 0;
        }
    }

    patch.springs.clear();
    patch.springs.reserve(3200);
    for (uint32_t y = 0; y < kRows; ++y) {
        for (uint32_t x = 0; x < kColumns; ++x) {
            const uint32_t current = Index(x, y);
            if (x + 1 < kColumns) {
                AddSpring(patch, current, Index(x + 1, y),
                          SpringKind::Structural);
            }
            if (y + 1 < kRows) {
                AddSpring(patch, current, Index(x, y + 1),
                          SpringKind::Structural);
            }
            if (x + 1 < kColumns && y + 1 < kRows) {
                AddSpring(patch, current, Index(x + 1, y + 1),
                          SpringKind::Shear);
            }
            if (x + 1 < kColumns && y > 0) {
                AddSpring(patch, current, Index(x + 1, y - 1),
                          SpringKind::Shear);
            }
            if (x + 2 < kColumns) {
                AddSpring(patch, current, Index(x + 2, y), SpringKind::Bend);
            }
            if (y + 2 < kRows) {
                AddSpring(patch, current, Index(x, y + 2), SpringKind::Bend);
            }
        }
    }
}

void CpuClothSimulation::AddSpring(ClothPatch& patch, uint32_t a,
                                   uint32_t b, SpringKind kind) {
    const glm::vec3 delta = patch.particles[b].position -
                            patch.particles[a].position;
    const float restLength = glm::length(delta);
    if (restLength <= kEpsilon) return;

    Spring spring;
    spring.a = a;
    spring.b = b;
    spring.restLength = restLength;
    spring.kind = kind;
    switch (kind) {
        case SpringKind::Structural:
            spring.stiffness = 360.0f;
            spring.damping = 12.0f;
            break;
        case SpringKind::Shear:
            spring.stiffness = 260.0f;
            spring.damping = 9.0f;
            break;
        case SpringKind::Bend:
            spring.stiffness = 90.0f;
            spring.damping = 5.0f;
            break;
    }
    patch.springs.push_back(spring);
}

void CpuClothSimulation::Step(float deltaSeconds) {
    if (!m_initialized) Reset();

    const float dt = std::clamp(deltaSeconds, 0.0f, 0.02f);
    if (dt <= 0.0f) return;

    for (ClothPatch& patch : m_patches) {
        for (Particle& particle : patch.particles) {
            particle.force = particle.pinned
                ? glm::vec3(0.0f)
                : m_localGravity * kParticleMass;
        }

        if (patch.kind == ClothPatchKind::PinnedFlat && kWindForce > 0.0f) {
            for (uint32_t y = 0; y < kRows; ++y) {
                for (uint32_t x = 0; x < kColumns; ++x) {
                    Particle& particle = patch.particles[Index(x, y)];
                    if (particle.pinned) continue;

                    const glm::vec3 normal = EstimateNormal(patch, x, y);
                    const float phase = m_simulationTime * 1.7f +
                                        particle.position.x * 1.9f +
                                        particle.position.y * 1.3f;
                    const glm::vec3 windVelocity(
                        0.35f * std::sin(phase),
                        0.12f * std::cos(phase * 0.7f),
                        1.25f * std::sin(phase * 0.83f));
                    const float normalSpeed = glm::dot(
                        windVelocity - particle.velocity, normal);
                    particle.force += normal *
                        (normalSpeed * std::abs(normalSpeed) * kWindForce);
                }
            }
        }

        for (const Spring& spring : patch.springs) {
            Particle& a = patch.particles[spring.a];
            Particle& b = patch.particles[spring.b];
            const glm::vec3 delta = b.position - a.position;
            const float length = glm::length(delta);
            if (length <= kEpsilon) continue;

            const glm::vec3 direction = delta / length;
            const float relativeSpeed = glm::dot(b.velocity - a.velocity,
                                                 direction);
            const float scalar = spring.stiffness * (length - spring.restLength) +
                                 spring.damping * relativeSpeed;
            const glm::vec3 springForce = direction * scalar;
            if (!a.pinned) a.force += springForce;
            if (!b.pinned) b.force -= springForce;
        }

        for (Particle& particle : patch.particles) {
            particle.previousPosition = particle.position;
            if (particle.pinned) {
                particle.velocity = glm::vec3(0.0f);
                continue;
            }

            glm::vec3 acceleration = particle.force / kParticleMass;
            const float accelerationLength = glm::length(acceleration);
            if (accelerationLength > kMaximumAcceleration) {
                acceleration *= kMaximumAcceleration / accelerationLength;
            }
            particle.velocity += acceleration * dt;
            particle.velocity *= std::exp(-kAirDamping * dt);
            particle.position += particle.velocity * dt;
        }

        // The force solve provides the dynamics; projection removes the
        // high-frequency stretch error that otherwise makes a stiff Euler
        // cloth visibly buzz even after external forces have settled.
        for (uint32_t iteration = 0; iteration < kProjectionIterations;
             ++iteration) {
            ProjectConstraints(patch);
            for (Particle& particle : patch.particles) {
                if (!particle.pinned) SolveCollisions(patch, particle);
            }
        }

        for (Particle& particle : patch.particles) {
            if (particle.pinned) {
                particle.velocity = glm::vec3(0.0f);
                continue;
            }

            particle.velocity = (particle.position - particle.previousPosition) /
                                dt;
            particle.velocity *= kProjectionVelocityDamping;
            const float speed = glm::length(particle.velocity);
            if (speed > kMaximumSpeed) {
                particle.velocity *= kMaximumSpeed / speed;
            }
            SolveCollisions(patch, particle);
        }
    }

    m_simulationTime += dt;
}

void CpuClothSimulation::ProjectConstraints(ClothPatch& patch) const {
    for (const Spring& spring : patch.springs) {
        Particle& a = patch.particles[spring.a];
        Particle& b = patch.particles[spring.b];
        const glm::vec3 delta = b.position - a.position;
        const float length = glm::length(delta);
        if (length <= kEpsilon) continue;

        const float inverseMassA = a.pinned ? 0.0f : 1.0f;
        const float inverseMassB = b.pinned ? 0.0f : 1.0f;
        const float inverseMassSum = inverseMassA + inverseMassB;
        if (inverseMassSum <= kEpsilon) continue;

        const float normalizedError = (length - spring.restLength) / length;
        const glm::vec3 correction = delta *
            (kProjectionStiffness * normalizedError / inverseMassSum);
        if (!a.pinned) a.position += correction * inverseMassA;
        if (!b.pinned) b.position -= correction * inverseMassB;
    }
}

void CpuClothSimulation::SetTransform(const glm::vec3& center,
                                       const glm::quat& rotation) {
    m_center = center;
    const float rotationLength = glm::length(rotation);
    m_rotation = rotationLength > kEpsilon
        ? glm::normalize(rotation)
        : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    m_localGravity = glm::inverse(m_rotation) * m_worldGravity;
}

void CpuClothSimulation::ResetTransform() {
    SetTransform(glm::vec3(0.0f, 0.45f, 0.0f),
                 glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
}

glm::vec3 CpuClothSimulation::EstimateNormal(const ClothPatch& patch,
                                             uint32_t x,
                                             uint32_t y) const {
    const auto positionAt = [this, &patch](uint32_t px, uint32_t py) {
        return patch.particles[Index(px, py)].position;
    };

    glm::vec3 tangentX;
    if (x + 1 < kColumns) {
        tangentX = positionAt(x + 1, y) - positionAt(x, y);
    } else {
        tangentX = positionAt(x, y) - positionAt(x - 1, y);
    }

    glm::vec3 tangentY;
    if (y + 1 < kRows) {
        tangentY = positionAt(x, y + 1) - positionAt(x, y);
    } else {
        tangentY = positionAt(x, y) - positionAt(x, y - 1);
    }

    const glm::vec3 normal = patch.kind == ClothPatchKind::FallingWrapSphere
        ? glm::cross(tangentY, tangentX)
        : glm::cross(tangentX, tangentY);
    const float normalLength = glm::length(normal);
    return normalLength > kEpsilon
        ? normal / normalLength
        : glm::vec3(0.0f, 0.0f, 1.0f);
}

void CpuClothSimulation::SolveCollisions(const ClothPatch& patch,
                                          Particle& particle) const {
    const auto solveFloor = [&particle]() {
        if (particle.position.y < kFloorY) {
            particle.position.y = kFloorY;
            if (particle.velocity.y < 0.0f) {
                particle.velocity.y *= -kCollisionRestitution;
            }
            particle.velocity.x *= 0.92f;
            particle.velocity.z *= 0.92f;
        }
    };

    solveFloor();
    if (patch.kind != ClothPatchKind::FallingWrapSphere) return;

    const glm::vec3 offset = particle.position - m_wrapSphereCenter;
    const float distance = glm::length(offset);
    const float collisionRadius = m_wrapSphereRadius + kCollisionMargin;
    if (distance < collisionRadius) {
        const glm::vec3 normal = distance > kEpsilon
            ? offset / distance
            : glm::vec3(0.0f, 1.0f, 0.0f);
        particle.position = m_wrapSphereCenter + normal * collisionRadius;

        const float normalVelocity = glm::dot(particle.velocity, normal);
        if (normalVelocity < 0.0f) {
            particle.velocity -= normal *
                (normalVelocity * (1.0f + kCollisionRestitution));
        }
        const float correctedNormalVelocity =
            glm::dot(particle.velocity, normal);
        const glm::vec3 tangent = particle.velocity -
                                  normal * correctedNormalVelocity;
        particle.velocity = normal * correctedNormalVelocity +
                            tangent * kSphereFriction;
    }

    // The sphere is seated on the same floor plane. Reapply the floor after
    // sphere projection so the bottom contact cannot drift below the floor.
    solveFloor();
}

void CpuClothSimulation::BuildRenderVertices(
    std::vector<ClothRenderVertex>& surface,
    std::vector<ClothRenderVertex>& springs) const {
    surface.clear();
    springs.clear();
    if (!m_initialized) return;

    const size_t patchSurfaceCount =
        static_cast<size_t>(kColumns - 1) * (kRows - 1) * 6;
    const size_t sphereSurfaceCount =
        static_cast<size_t>(kSphereSegments) * kSphereRings * 6;
    surface.reserve(sphereSurfaceCount + patchSurfaceCount * m_patches.size());

    size_t springCount = 0;
    for (const ClothPatch& patch : m_patches) {
        springCount += patch.springs.size() * 2;
    }
    springs.reserve(springCount);

    const auto appendSurfaceVertex = [&surface](const glm::vec3& position,
                                                 const glm::vec4& color) {
        ClothRenderVertex vertex;
        vertex.position = glm::vec4(position, 1.0f);
        vertex.color = color;
        surface.push_back(vertex);
    };
    const auto appendTriangle = [&appendSurfaceVertex](
        const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
        const glm::vec4& color) {
        appendSurfaceVertex(a, color);
        appendSurfaceVertex(b, color);
        appendSurfaceVertex(c, color);
    };

    // Draw the sphere first in the transparent surface stream so the cloth
    // remains legible over the obstacle while depth testing still removes
    // geometry hidden behind the rest of the scene.
    const glm::vec4 sphereColor = SphereColor();
    for (uint32_t ring = 0; ring < kSphereRings; ++ring) {
        const float latitude0 = -0.5f * kPi +
                                kPi * static_cast<float>(ring) /
                                static_cast<float>(kSphereRings);
        const float latitude1 = -0.5f * kPi +
                                kPi * static_cast<float>(ring + 1) /
                                static_cast<float>(kSphereRings);
        const float y0 = std::sin(latitude0);
        const float y1 = std::sin(latitude1);
        const float radius0 = std::cos(latitude0);
        const float radius1 = std::cos(latitude1);
        for (uint32_t segment = 0; segment < kSphereSegments; ++segment) {
            const float longitude0 = 2.0f * kPi *
                                     static_cast<float>(segment) /
                                     static_cast<float>(kSphereSegments);
            const float longitude1 = 2.0f * kPi *
                                     static_cast<float>(segment + 1) /
                                     static_cast<float>(kSphereSegments);
            const auto spherePoint = [this](float y, float ringRadius,
                                             float longitude) {
                return m_wrapSphereCenter + m_wrapSphereRadius *
                    glm::vec3(ringRadius * std::cos(longitude), y,
                              ringRadius * std::sin(longitude));
            };
            const glm::vec3 p00 = spherePoint(y0, radius0, longitude0);
            const glm::vec3 p10 = spherePoint(y0, radius0, longitude1);
            const glm::vec3 p11 = spherePoint(y1, radius1, longitude1);
            const glm::vec3 p01 = spherePoint(y1, radius1, longitude0);
            appendTriangle(p00, p10, p11, sphereColor);
            appendTriangle(p11, p01, p00, sphereColor);
        }
    }

    const glm::vec4 surfaceColor = SurfaceColor();
    for (const ClothPatch& patch : m_patches) {
        if (patch.particles.empty()) continue;

        for (uint32_t y = 0; y + 1 < kRows; ++y) {
            for (uint32_t x = 0; x + 1 < kColumns; ++x) {
                const glm::vec3& a = patch.particles[Index(x, y)].position;
                const glm::vec3& b = patch.particles[Index(x + 1, y)].position;
                const glm::vec3& c = patch.particles[Index(x + 1, y + 1)].position;
                const glm::vec3& d = patch.particles[Index(x, y + 1)].position;
                appendTriangle(a, b, c, surfaceColor);
                appendTriangle(c, d, a, surfaceColor);
            }
        }

        for (const Spring& spring : patch.springs) {
            glm::vec4 color(1.0f);
            switch (spring.kind) {
                case SpringKind::Structural:
                    color = glm::vec4(0.10f, 0.88f, 1.00f, 0.96f);
                    break;
                case SpringKind::Shear:
                    color = glm::vec4(0.82f, 0.26f, 1.00f, 0.90f);
                    break;
                case SpringKind::Bend:
                    color = glm::vec4(1.00f, 0.58f, 0.12f, 0.88f);
                    break;
            }
            ClothRenderVertex a;
            a.position = glm::vec4(patch.particles[spring.a].position, 1.0f);
            a.color = color;
            ClothRenderVertex b;
            b.position = glm::vec4(patch.particles[spring.b].position, 1.0f);
            b.color = color;
            springs.push_back(a);
            springs.push_back(b);
        }
    }
}
