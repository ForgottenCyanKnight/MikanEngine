#pragma once
#include <cstdint>

// Counts refer to exterior unit faces, not the six faces of every solid voxel.
// Keep the ordinary greedy result separate from optional contact backfill.
struct VoxSurfaceReductionStats {
    uint64_t exposedFaceCount = 0;
    uint64_t greedyQuadCount = 0;
    uint64_t finalQuadCount = 0;

    double GreedyReductionFraction() const {
        return exposedFaceCount ? 1.0 - double(greedyQuadCount) / double(exposedFaceCount) : 0.0;
    }
    double ReductionFraction() const {
        return exposedFaceCount ? 1.0 - double(finalQuadCount) / double(exposedFaceCount) : 0.0;
    }
    double MergeFactor() const {
        return finalQuadCount ? double(exposedFaceCount) / double(finalQuadCount) : 0.0;
    }
    uint64_t TriangleCount() const { return finalQuadCount * 2; }

    // Composite totals are instance-weighted; ratios must be derived from sums,
    // rather than averaging ratios of differently sized source meshes.
    VoxSurfaceReductionStats& operator+=(const VoxSurfaceReductionStats& other) {
        exposedFaceCount += other.exposedFaceCount;
        greedyQuadCount += other.greedyQuadCount;
        finalQuadCount += other.finalQuadCount;
        return *this;
    }
};
