#pragma once

#include "Platform/Export.h"
#include "Rendering/RenderWorld.h"

#include <cstdint>

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
    double buildMilliseconds = 0.0;
    bool validationChecked = false;
    bool invariantsValid = true;
    uint32_t invariantErrorCount = 0;
};

// Owns the ECS -> RenderWorld extraction boundary.  Legacy SceneCollector
// collection helpers remain available for source compatibility, but they are
// no longer involved in building the renderer snapshot.
class MIKAN_API RenderWorldBuilder final {
public:
    static void Build(RenderWorld& out, RenderWorldBuildStats* stats = nullptr);
};
