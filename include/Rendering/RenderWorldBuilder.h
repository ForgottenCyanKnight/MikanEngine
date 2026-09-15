#pragma once

#include "Platform/Export.h"
#include "Rendering/RenderWorld.h"

#include <cstddef>
#include <cstdint>
#include <vector>

// Metrics produced alongside one ECS -> RenderWorld extraction.  Counts are
// intentionally renderer-facing so the values can be displayed or emitted in
// headless profiling without exposing ECS internals.
struct MIKAN_API RenderWorldBuildStats {
    uint64_t frameNumber = 0;
    uint32_t entitySetVersion = 0;
    uint32_t hierarchyEntityCount = 0;
    uint32_t entityCount = 0;
    uint32_t visibleEntityCount = 0;
    uint32_t modelGroupCount = 0;
    uint32_t modelEntityCount = 0;
    uint32_t voxGroupCount = 0;
    uint32_t voxEntityCount = 0;
    uint32_t cameraCount = 0;
    uint32_t lightCount = 0;
    uint32_t terrainCount = 0;
    uint32_t waterCount = 0;
    uint32_t skyboxCount = 0;
    uint32_t cloudCount = 0;
    uint32_t particleCount = 0;
    uint64_t estimatedContainerBytes = 0;
    // CPU time spent inside the RenderWorld stages.  These fields exclude
    // caller work that may overlap an asynchronous Finalize task.
    double captureMilliseconds = 0.0;
    double finalizeMilliseconds = 0.0;
    double buildMilliseconds = 0.0;
    // Time spent waiting at the publish boundary for the staging snapshot.
    double publishWaitMilliseconds = 0.0;
    bool finalizedAsynchronously = false;
    bool incrementalCaptureUsed = false;
    uint32_t reusedTransformCount = 0;
    uint32_t recomputedTransformCount = 0;
    uint32_t reusedComponentCount = 0;
    uint32_t recomputedComponentCount = 0;
    bool validationChecked = false;
    bool invariantsValid = true;
    uint32_t invariantErrorCount = 0;
};

// Per-range output produced by the pure-data finalization stage.  A partition
// never writes to the RenderWorld itself, so multiple ranges can be finalized
// concurrently and merged deterministically at the publish boundary.
struct MIKAN_API RenderWorldFinalizePartition {
    std::vector<RenderModelGroup> modelGroups;
    std::vector<RenderVoxGroup> voxGroups;

    void Reset() {
        modelGroups.clear();
        voxGroups.clear();
    }
};

// Owns the ECS -> RenderWorld extraction boundary.  Legacy SceneCollector
// collection helpers remain available for source compatibility, but they are
// no longer involved in building the renderer snapshot.
class MIKAN_API RenderWorldBuilder final {
public:
    // Capture the live ECS state into an exclusive staging RenderWorld.
    // This function must run on the main thread because ECS and SceneECS are
    // not thread-safe for concurrent reads.
    static void Capture(RenderWorld& out);

    // Complete a captured staging snapshot without touching ECS, SceneECS,
    // or other gameplay systems.  The staging buffer must remain exclusive
    // while this function runs, so it can be moved to a worker in a later
    // scheduling step without changing the extraction contract.
    static void Finalize(RenderWorld& out);

    // Finalize one entity range into isolated derived-data output.  The range
    // is read-only and excludes ECS access, making it safe for JobSystem tasks.
    static void FinalizeRange(const RenderWorld& source,
                              std::size_t begin,
                              std::size_t end,
                              RenderWorldFinalizePartition& out);

    // Merge range outputs in source order and rebuild flattened lists/indexes.
    // This is the only function in the partitioned path that mutates derived
    // fields on the staging RenderWorld.
    static void MergeFinalizedPartitions(
        RenderWorld& out,
        const std::vector<RenderWorldFinalizePartition>& partitions);

    // Compute metrics and optional invariant checks from a finalized snapshot.
    // This is also free of ECS access and is kept separate so completion code
    // can publish a worker-built snapshot without rebuilding it.
    static void CollectStats(const RenderWorld& world,
                             RenderWorldBuildStats& stats,
                             double buildMilliseconds = 0.0);

    static void Build(RenderWorld& out, RenderWorldBuildStats* stats = nullptr);
};
