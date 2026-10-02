#pragma once
#include <cstdint>
#include <cmath>
#include <glm/glm.hpp>

namespace HiZHistory {
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
