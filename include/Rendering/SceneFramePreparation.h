#pragma once

#include "Platform/Export.h"
#include "Rendering/SceneRenderCpuProfile.h"

struct RenderFrameContext;
class SceneRenderer;

// CPU-only preparation for one render view.  The coordinator owns the
// renderer resources and frame lifetime; this module only fills the per-view
// context from the published RenderWorld and updates preparation-side caches.
class MIKAN_API SceneFramePreparation final {
public:
    static void Prepare(
        SceneRenderer& renderer,
        RenderFrameContext& context,
        bool cpuProfileEnabled,
        SceneRenderCpuProfile::FrameTiming& profileTiming,
        SceneRenderCpuProfile::Clock::time_point prepareStart);
};
