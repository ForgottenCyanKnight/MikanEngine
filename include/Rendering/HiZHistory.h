#pragma once
#include <cstdint>
#include <cmath>
#include <bit>
#include "Rendering/RenderWorld.h"
#include <glm/glm.hpp>

namespace HiZHistory {
// Any occluder edit invalidates temporal visibility. Animated meshes are unstable.
inline uint64_t OccluderRevision(const RenderWorld& world) {
    uint64_t hash = 14695981039346656037ull;
    const auto mix = [&hash](uint64_t v) { hash = (hash ^ v) * 1099511628211ull; };
    mix(world.entitySetVersion);
    for (const auto& entity : world.entities) {
        if (!entity.hasMesh && !entity.hasTerrain && !entity.hasVoxel) continue;
        mix(entity.entity); mix(entity.visible); mix(entity.hasTransform);
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                mix(std::bit_cast<uint32_t>(entity.transform.worldMatrix[c][r]));
        for (const auto component : {RenderWorldCaptureComponent::Mesh,
                RenderWorldCaptureComponent::Material, RenderWorldCaptureComponent::RenderFlags,
                RenderWorldCaptureComponent::Terrain, RenderWorldCaptureComponent::Voxel}) {
            const auto i = static_cast<size_t>(component);
            mix(entity.capturedComponentPresence[i]); mix(entity.capturedComponentRevisions[i]);
        }
        if (entity.hasAnimator || entity.hasVmdPlayer) mix(world.frameNumber);
    }
    return hash;
}
// Previous-frame depth cannot prove occlusion after camera disocclusion.
inline bool CanReuse(const glm::mat4& current, const glm::mat4& previous,
                     uint64_t revision, uint64_t previousRevision, bool valid) {
    if (!valid || revision != previousRevision) return false;
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            if (!std::isfinite(current[c][r]) || !std::isfinite(previous[c][r]) ||
                current[c][r] != previous[c][r]) return false;
        }
    }
    return true;
}
// Geometry adds jitter to clip.xy after multiplying the unjittered VP.
inline glm::mat4 WithJitter(glm::mat4 matrix, const glm::vec2& jitter) {
    for (int c = 0; c < 4; ++c) {
        matrix[c][0] += jitter.x * matrix[c][3];
        matrix[c][1] += jitter.y * matrix[c][3];
    }
    return matrix;
}
} // namespace HiZHistory
