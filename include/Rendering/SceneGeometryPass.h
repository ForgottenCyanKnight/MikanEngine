#pragma once

#include "Platform/Export.h"
#include "Rendering/SceneRenderer.h"

class MIKAN_API SceneGeometryPass final {
public:
    static void Render(SceneRenderer& sceneRenderer, RenderFrameContext& context);
};

