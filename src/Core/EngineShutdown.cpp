#include "Core/EngineShutdown.h"

#include "Core/AudioManager.h"
#include "Core/EngineGlobal.h"
#include "Core/Log.h"
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

#include <cstdio>
#include <iostream>
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
        std::fprintf(stderr, "[Screenshot] ERROR: requested frame %d was not captured\n",
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

    std::cout << "[EngineMain] Cleaning renderers..." << std::endl;
    g_SceneRenderer.Cleanup();
    std::cout << "[EngineMain] SceneRenderer cleanup done" << std::endl;

    g_SkyboxRenderer.Cleanup();
    std::cout << "[EngineMain] SkyboxRenderer cleanup done" << std::endl;

    g_InfiniteGridRenderer.Cleanup();
    std::cout << "[EngineMain] InfiniteGridRenderer cleanup done" << std::endl;

    g_SceneRenderTarget.Cleanup();
    std::cout << "[EngineMain] SceneRenderTarget cleanup done" << std::endl;

    g_GameRenderTarget.Cleanup();
    std::cout << "[EngineMain] GameRenderTarget cleanup done" << std::endl;

    g_FullscreenQuad.Cleanup();
    std::cout << "[EngineMain] FullscreenQuad cleanup done" << std::endl;

    g_SceneCompositeQuad.Cleanup();
    g_GameCompositeQuad.Cleanup();
    g_SceneFilterQuad.Cleanup();
    g_GameFilterQuad.Cleanup();
    g_SceneChain.Cleanup();
    g_GameChain.Cleanup();
    g_SwapChain.Cleanup();
    std::cout << "[EngineMain] CompositeQuad cleanup done" << std::endl;

    g_AtmosphereRenderer.Cleanup();
    std::cout << "[EngineMain] AtmosphereRenderer cleanup done" << std::endl;

    TextRenderer::GetInstance().Cleanup();
    Renderer2D::GetInstance().Cleanup();
    std::cout << "[EngineMain] TextRenderer/Renderer2D cleanup done" << std::endl;

    ::CleanupVulkanWindow();
    ::CleanupVulkan();
    std::cout << "[EngineMain] Vulkan cleanup done" << std::endl;

    std::cout << "[EngineMain] Destroying audio manager..." << std::endl;
    AudioManager::DestroyInstance();
    std::cout << "[EngineMain] Audio manager destroyed" << std::endl;

    // PhysicsManager owns Jolt resources and must be released after the ECS
    // system has stopped using it, but before SDL/window teardown completes.
    CleanupPhysicsSystem();

    std::cout << "[EngineMain] Destroying window..." << std::endl;
    if (window != nullptr) SDL_DestroyWindow(window);
    std::cout << "[EngineMain] Window destroyed" << std::endl;

    std::cout << "[EngineMain] Quitting SDL..." << std::endl;
    SDL_Quit();
    std::cout << "[EngineMain] SDL quit" << std::endl;

    std::cout << "[EngineMain] Exit successfully" << std::endl;
    std::cout << "[EngineMain] Returning from main()" << std::endl;
    LOGI("==== MikanEngine shutting down cleanly ====");
    ShutdownLog();
    return finalExitCode;
}

} // namespace Core
