#pragma once

#include <glm/glm.hpp>

// Shared dimensions for the GPU SPH observation vessel. The simulation and
// the transparent renderer both use the same local-space profile so particles
// can fall from the wide chamber through stepped openings into the observation
// channel.
namespace GpuSphContainerGeometry {

constexpr float kChamberHalfExtent = 1.05f;
constexpr float kStep1HalfExtent = 0.82f;
constexpr float kStep2HalfExtent = 0.64f;
constexpr float kOutletHalfExtent = 0.45f;
constexpr float kTopY = 2.25f;
constexpr float kChamberBottomY = 0.45f;
constexpr float kStep1Y = 0.15f;
constexpr float kStep2Y = -0.15f;
constexpr float kStep3Y = -0.45f;
constexpr float kBottomY = -1.15f;
constexpr float kCenterY = (kTopY + kBottomY) * 0.5f;
constexpr float kHalfHeight = (kTopY - kBottomY) * 0.5f;

inline glm::vec3 GetCenter() {
    return glm::vec3(0.0f, kCenterY, 0.0f);
}

inline glm::vec3 GetHalfExtents() {
    return glm::vec3(kChamberHalfExtent, kHalfHeight,
                     kChamberHalfExtent);
}

inline float GetHalfExtentAtHeight(float height) {
    if (height >= kStep1Y) return kChamberHalfExtent;
    if (height >= kStep2Y) return kStep1HalfExtent;
    if (height >= kStep3Y) return kStep2HalfExtent;
    return kOutletHalfExtent;
}

inline glm::vec3 NormalizeLocalPoint(const glm::vec3& point) {
    const glm::vec3 center = GetCenter();
    const glm::vec3 halfExtents = GetHalfExtents();
    return (point - center) / halfExtents;
}

} // namespace GpuSphContainerGeometry
