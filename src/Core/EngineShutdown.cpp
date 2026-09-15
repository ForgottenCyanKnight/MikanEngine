#include "Core/EngineShutdown.h"

#include "Core/AudioManager.h"
#include "Core/EngineGlobal.h"
#include "Core/Log.h"
#include "Core/LogStream.h"
#include "Core/RenderDocCapture.h"
#include "Core/ScreenshotCapture.h"
#include "Core/VulkanManager.h"
#include "ECS/Systems/WorldSystem.h"
#include "Rendering/AtmosphereRenderer.h"
#include "Rendering/DescriptorSetCache.h"
#include "Rendering/FullscreenQuad.h"
#include "Rendering/InfiniteGridRenderer.h"
#include "Rendering/PostProcessChain.h"
#include "Rendering/Renderer2D.h"
#include "Rendering/SceneRenderer.h"
#include "Rendering/RenderTarget.h"
#include "Rendering/SkyboxRenderer.h"
#include "Rendering/TextRenderer.h"
#include "UI/Canvas2D.h"
#include "World/WorldGlobals.h"

#include <memory>
#include <SDL3/SDL.h>

extern void CleanupPhysicsSystem();
extern std::shared_ptr<ECS::WorldSystem> g_WorldSystemPtr;

// These composite objects are defined with the renderer globals but are not
// part of the legacy aggregate header yet.
extern FullscreenQuad g_SceneCompositeQuad;
extern FullscreenQuad g_GameCompositeQuad;
extern FullscreenQuad g_SceneFilterQuad;
extern FullscreenQuad g_GameFilterQuad;
extern AtmosphereRenderer g_AtmosphereRenderer;
extern InfiniteGridRenderer g_InfiniteGridRenderer;

namespace Core {

int ShutdownEngine(SDL_Window* window, int screenshotFrame, int finalExitCode)
{
    // GPU work must be complete before destroying targets, pipelines, or the
    // editor's descriptors that can still reference them.
    if (g_Device != VK_NULL_HANDLE) {
        check_vk_result(vkDeviceWaitIdle(g_Device));
    }

    const bool screenshotFinalized = ScreenshotCapture::GetInstance().Finalize();
    if (screenshotFrame > 0 &&
        (!screenshotFinalized || !ScreenshotCapture::GetInstance().WasCaptured())) {
        LOGE("[Screenshot] ERROR: requested frame %d was not captured",
                     screenshotFrame);
        if (finalExitCode == 0) finalExitCode = 4;
    }
    RenderDocCapture::GetInstance().Finalize();

    // Canvas and physics can still own references into the scene while the
    // renderer is alive, so release them before Vulkan objects.
    UI::Canvas2D::GetInstance().Clear();
    if (g_PhysicsSystemPtr) {
        g_PhysicsSystemPtr->Shutdown();
    }
    if (g_EnableVoxelWorld && g_WorldSystemPtr) {
        g_WorldSystemPtr->Shutdown();
    }

    LOGSTREAM(Info) << "[EngineMain] Cleaning renderers...";
    g_SceneRenderer.Cleanup();
    LOGSTREAM(Info) << "[EngineMain] SceneRenderer cleanup done";

    g_SkyboxRenderer.Cleanup();
    LOGSTREAM(Info) << "[EngineMain] SkyboxRenderer cleanup done";

    g_InfiniteGridRenderer.Cleanup();
    LOGSTREAM(Info) << "[EngineMain] InfiniteGridRenderer cleanup done";

    g_SceneRenderTarget.Cleanup();
    LOGSTREAM(Info) << "[EngineMain] SceneRenderTarget cleanup done";

    g_GameRenderTarget.Cleanup();
    LOGSTREAM(Info) << "[EngineMain] GameRenderTarget cleanup done";

    g_FullscreenQuad.Cleanup();
    LOGSTREAM(Info) << "[EngineMain] FullscreenQuad cleanup done";

    g_SceneCompositeQuad.Cleanup();
    g_GameCompositeQuad.Cleanup();
    g_SceneFilterQuad.Cleanup();
    g_GameFilterQuad.Cleanup();
    g_SceneChain.Cleanup();
    g_GameChain.Cleanup();
    g_SwapChain.Cleanup();
    LOGSTREAM(Info) << "[EngineMain] CompositeQuad cleanup done";

    g_AtmosphereRenderer.Cleanup();
    LOGSTREAM(Info) << "[EngineMain] AtmosphereRenderer cleanup done";

    TextRenderer::GetInstance().Cleanup();
    Renderer2D::GetInstance().Cleanup();
    LOGSTREAM(Info) << "[EngineMain] TextRenderer/Renderer2D cleanup done";

    ::CleanupVulkanWindow();
    ::CleanupVulkan();
    LOGSTREAM(Info) << "[EngineMain] Vulkan cleanup done";

    LOGSTREAM(Info) << "[EngineMain] Destroying audio manager...";
    AudioManager::DestroyInstance();
    LOGSTREAM(Info) << "[EngineMain] Audio manager destroyed";

    // PhysicsManager owns Jolt resources and must be released after the ECS
    // system has stopped using it, but before SDL/window teardown completes.
    CleanupPhysicsSystem();

    LOGSTREAM(Info) << "[EngineMain] Destroying window...";
    if (window != nullptr) SDL_DestroyWindow(window);
    LOGSTREAM(Info) << "[EngineMain] Window destroyed";

    LOGSTREAM(Info) << "[EngineMain] Quitting SDL...";
    SDL_Quit();
    LOGSTREAM(Info) << "[EngineMain] SDL quit";

    LOGSTREAM(Info) << "[EngineMain] Exit successfully";
    LOGSTREAM(Info) << "[EngineMain] Returning from main()";
    LOGI("==== MikanEngine shutting down cleanly ====");
    ShutdownLog();
    return finalExitCode;
}

} // namespace Core
