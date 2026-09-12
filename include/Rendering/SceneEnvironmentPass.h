#pragma once

#include "Platform/Export.h"
#include "Rendering/SceneRenderer.h"

class MIKAN_API SceneEnvironmentPass final {
public:
    static void RenderTerrainWater(SceneRenderer& sceneRenderer, RenderFrameContext& context);
    static void RenderVoxelWorld(SceneRenderer& sceneRenderer, RenderFrameContext& context);
};
