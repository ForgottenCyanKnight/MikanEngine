#pragma once
// SceneCollector.h - ECS extraction helpers for renderer-owned data
#include "Platform/Export.h"
#include <string>
#include "ECS/Types.h"
#include "RenderWorld.h"

class RenderWorldBuilder;

class MIKAN_API SceneCollector {
public:
    // Renderer key: static meshes share the model path; animated entities get an
    // entity-local key so each AnimatorComponent owns an independent pose.
    static std::string GetModelRendererKey(ECS::Entity entity);

private:
    friend class RenderWorldBuilder;

    // Main-thread half of the RenderWorld boundary.  It reads ECS exactly
    // once and leaves only renderer-owned data in the staging buffer.
    static void CaptureRenderWorldFromECS(RenderWorld& out);

    // Pure-data half of the boundary.  It performs grouping, derived list
    // construction, and index rebuild without reading live ECS state.
    static void FinalizeRenderWorld(RenderWorld& out);
};
