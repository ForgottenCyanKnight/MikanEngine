#include "SceneRenderer.h"
#include "EngineGlobal.h"
#include "SkyboxRenderer.h"
#include "RenderTarget.h"
#include "FullscreenQuad.h"
#include "AtmosphereRenderer.h"
#include "EngineConfig.h"
#include "PhysicsManager.h"
#include "Core/SceneManager.h"
#include "ECS/PhysicsSystem.h"
#include "World/WorldGlobals.h"
#include "World/WorldRenderer.h"
#include "ECS/Systems/WorldSystem.h"
#include "ECS/SceneECS.h"
#include "Core/ProjectManager.h"
#include "Core/VulkanManager.h"
#include "Game/GameManager.h"
#include "SceneSerializer.h"
#include "Core/Physics2DSystem.h"
#include "Core/Physics2DManager.h"
#include "Core/Camera2DSystem.h"
#include "Core/ThirdPersonCameraSystem.h"
#include "Core/TilemapSystem.h"
#include "Core/AudioManager.h"
#include "ECS/ScriptSystem.h"
#include "Rendering/DescriptorSetCache.h"
#include "Rendering/ParticleSystem.h"
#include "UI/Canvas2D.h"
#include <chrono>
#include <algorithm>
#include <filesystem>
#include <system_error>
#include <SDL3/SDL.h>
#include <iostream>


SceneRenderer g_SceneRenderer;
SkyboxRenderer g_SkyboxRenderer;
RenderTarget g_SceneRenderTarget;
RenderTarget g_GameRenderTarget;
FullscreenQuad g_FullscreenQuad;
FullscreenQuad g_SceneCompositeQuad;      // 编辑器 SceneView 合成（SceneRT render pass subpass1 → 显示附件）
FullscreenQuad g_GameCompositeQuad;       // 编辑器 GameView 合成（GameRT render pass subpass1 → 显示附件）
FullscreenQuad g_SceneFilterQuad;         // 编辑器 SceneView final 后处理（黑白滤镜测试）
FullscreenQuad g_GameFilterQuad;          // 编辑器 GameView final 后处理

#include "Rendering/PostProcessChain.h"
PostProcessChain g_SceneChain;             // 编辑器 SceneView 后处理链（配置驱动）
PostProcessChain g_GameChain;              // 编辑器 GameView 后处理链
PostProcessChain g_SwapChain;              // 游戏模式 swapchain 后处理链
AtmosphereRenderer g_AtmosphereRenderer;   // physical sky (low-res sky RT)

// physical sky toggle (default on: 3D scenes with SkyboxComponent use atmosphere instead of cubemap)
bool g_AtmosphereEnabled = true;

World* g_World = nullptr;
WorldRenderer* g_WorldRenderer = nullptr;

bool g_EnableVoxelWorld = true;
int g_HandBlockId = 3;  // 手持方块类型（物品栏选中格写入；EngineMain 放置用）

std::shared_ptr<ECS::WorldSystem> g_WorldSystemPtr = nullptr;

struct GlobalDestructorTracker {
    ~GlobalDestructorTracker() {
        std::cout << "[GlobalDestructorTracker] All global objects destroyed" << std::endl;
    }
};
static GlobalDestructorTracker g_GlobalDestructorTracker;

SDL_Window* window = nullptr;

auto g_LastTime = std::chrono::high_resolution_clock::now();

extern bool g_IsPaused;

// 为 ECS 命名空间提供 g_SceneRenderer
namespace ECS {
    // 定义 g_SceneRenderer 为全局变量的引用
    SceneRenderer& g_SceneRenderer = ::g_SceneRenderer;
}

Physics::PhysicsManager g_PhysicsManager;
std::shared_ptr<ECS::PhysicsSystem> g_PhysicsSystemPtr = nullptr;

void CleanupPhysicsSystem() {
    std::cout << "[EngineGlobals] Cleaning up physics system..." << std::endl;
    g_PhysicsSystemPtr.reset();
    std::cout << "[EngineGlobals] Physics system cleanup completed" << std::endl;
}

// DLL module globals (declared in Core/EngineGlobal.h)
TexturePool* g_TexturePool = nullptr;
bool g_ShowSceneView = false;
bool g_ShowGameView = false;
bool g_ProjectSelectionPending = false;
bool g_SceneIs2D = false;
bool g_EnableZPrepass = false;
bool g_UseSeparateMrtRenderPass = false;
bool g_ShowFPS = true;
bool g_ShowPhysics2DDebug = false;
float g_FPS = 0.0f;
float g_UIOpacity = 0.6f;                // 运行时全局 UI 不透明度（0.2 到 1.0），默认 60%

float GetUIOpacity() {
    return g_UIOpacity;
}

void SetUIOpacity(float opacity) {
    g_UIOpacity = std::clamp(opacity, 0.2f, 1.0f);
}

extern "C" MIKAN_API void MikanEngine_CloseProject()
{
    // 项目切换发生在编辑器帧的 CPU 阶段；先等待上一帧，确保模型、阴影
    // 和地形等 Vulkan 资源不再被 GPU 使用，然后再销毁场景资源。
    if (g_Device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(g_Device);
    }

    if (auto* game = Game::GameManager::GetInstance().GetCurrent()) {
        game->OnGameStop();
    }
    AudioManager::GetInstance().StopAll();

    // 先销毁脚本实例和外部运行时对象，再销毁 ECS 实体；插件脚本的
    // OnDestroy 仍能安全访问当前场景和自己的 DLL 代码。
    ECS::ScriptSystem::GetInstance().DestroyAll();
    Physics2DSystem::GetInstance().ClearBodies();
    TilemapSystem::GetInstance().ClearAll();
    if (g_WorldSystemPtr) {
        g_WorldSystemPtr->Shutdown();
    }

    // SceneSerializer::ClearScene 是序列化器内部实现，不把它暴露为项目
    // 生命周期 API；这里通过 SceneECS 的公共层级接口销毁当前场景实体。
    auto clearEntity = [&](auto&& self, ECS::Entity entity) -> void {
        const auto children = ECS::SceneECS::GetInstance().GetChildren(entity);
        for (const ECS::Entity child : children) {
            self(self, child);
        }
        ECS::SceneECS::GetInstance().DestroyEntity(entity);
    };
    const auto roots = ECS::SceneECS::GetInstance().GetRootEntities();
    for (const ECS::Entity root : roots) {
        clearEntity(clearEntity, root);
    }
    ECS::SceneECS::GetInstance().SetSceneGameModule({});
    Camera2DSystem::GetInstance().Reset();
    ThirdPersonCameraSystem::GetInstance().Reset();
    ParticleSystem::GetInstance().Clear();
    UI::Canvas2D::GetInstance().Clear();
    ECS::SceneECS::GetInstance().ClearSelection();

    // 清掉插件工厂后再卸载 DLL，避免 ScriptSystem/GameManager 留下悬空
    // std::function。下一项目会在 Activate 时重新加载自己的插件。
    Game::GameManager::GetInstance().Deactivate();
    ECS::ScriptSystem::GetInstance().ClearRegisteredScripts();
    Game::GameManager::GetInstance().UnloadPlugins();

    if (g_Device != VK_NULL_HANDLE) {
        g_SceneRenderer.Cleanup();
        DescriptorSetCache::GetInstance().Cleanup();
    }

    ProjectManager::GetInstance().ClearProjectRoot();
    g_ProjectSelectionPending = true;
    g_RunMode = RunMode::Editor;
    g_ShowSceneView = false;
    g_ShowGameView = false;
    g_ShowPhysics2DDebug = false;
    g_SceneIs2D = false;
    g_IsPaused = false;
    std::printf("[MikanEngine] Project unloaded; waiting for project manager selection\n");
}

extern "C" MIKAN_API bool MikanEngine_OpenProject(const char* dir)
{
    if (!dir || !dir[0]) return false;
    if (!g_ProjectSelectionPending && ProjectManager::GetInstance().HasActiveProject()) {
        MikanEngine_CloseProject();
    }
    if (!ProjectManager::GetInstance().SetProjectRoot(dir)) {
        printf("[MikanEngine] OpenProject failed: %s\n", dir);
        return false;
    }
    printf("[MikanEngine] Opening project, loading scene...\n");
    const auto& projectManager = ProjectManager::GetInstance();
    const ProjectManifest& mf = projectManager.GetManifest();

    // 项目场景必须由 project.json.scene 指定；不再回退到根目录或旧式 sence.json。
    std::string scenePath;
    if (mf.valid && !mf.scene.empty()) {
        scenePath = projectManager.ResolveAssetPath(mf.scene);
    }

    if (scenePath.empty()) {
        printf("[MikanEngine] Project has no scene: %s\n", dir);
        return false;
    }

    if (mf.valid) {
        for (const auto& asset : mf.assets) {
            std::error_code ec;
            const std::string assetPath = projectManager.ResolveAssetPath(asset);
            if (assetPath.empty() || !std::filesystem::exists(assetPath, ec)) {
                printf("[MikanEngine] Project asset MISSING (add to project.json assets[]?): %s\n", asset.c_str());
            }
        }
    }

    // 场景反序列化会预加载模型并申请材质 descriptor。首次启动时这些资源
    // 已由引擎初始化；从项目管理器切换回来后则需要在加载场景前重建它们。
    const bool renderResourcesNeedInit =
        DescriptorSetCache::GetInstance().GetLayout() == VK_NULL_HANDLE;
    if (renderResourcesNeedInit) {
        DescriptorSetCache::GetInstance().Init();
        DescriptorSetCache::GetInstance().CreateDescriptorSetLayout();
        DescriptorSetCache::GetInstance().CreateDescriptorPool(1000);
        g_SceneRenderer.Init(g_SceneRenderTarget.GetRenderPass());
        UpdateFullscreenQuadDescriptors();
    }

    if (!SceneManager::GetInstance().ChangeScene(scenePath)) {
        printf("[MikanEngine] Project scene FAILED: %s\n", scenePath.c_str());
        if (renderResourcesNeedInit) {
            g_SceneRenderer.Cleanup();
            DescriptorSetCache::GetInstance().Cleanup();
        }
        ProjectManager::GetInstance().ClearProjectRoot();
        return false;
    }

    // 场景没有覆盖 game 时，允许 project.json 提供项目默认玩法模块。
    const std::string& sceneGame = ECS::SceneECS::GetInstance().GetSceneGameModule();
    if (sceneGame.empty() && mf.valid && !mf.game.empty()) {
        if (auto* gm = Game::GameManager::GetInstance().Activate(mf.game)) {
            gm->OnSceneLoaded();
        }
    }

    Physics2DSystem::GetInstance().ClearBodies();
    g_ProjectSelectionPending = false;
    printf("[MikanEngine] Project opened: %s\n", dir);
    return true;
}

extern "C" MIKAN_API void MikanEngine_LoadSceneFile(const char* path)
{
    if (!path || !path[0]) return;
    if (!SceneManager::GetInstance().ChangeScene(path)) {
        printf("[MikanEngine] LoadSceneFile failed: %s\n", path);
    } else {
        printf("[MikanEngine] Scene imported: %s\n", path);
    }
    g_ProjectSelectionPending = false;
}

extern "C" MIKAN_API bool MikanEngine_ReloadCurrentGame()
{
    auto* gm = Game::GameManager::GetInstance().GetCurrent();
    if (!gm) {
        printf("[MikanEngine] Reload: no active game\n");
        return false;
    }
    std::string name = gm->GetName();
    if (!Game::GameManager::GetInstance().ReloadPlugin(name)) {
        printf("[MikanEngine] Reload failed (not a plugin or missing): %s\n", name.c_str());
        return false;
    }
    if (auto* fresh = Game::GameManager::GetInstance().Activate(name)) {
        fresh->OnSceneLoaded();
        printf("[MikanEngine] Game plugin hot-reloaded: %s\n", name.c_str());
        return true;
    }
    return false;
}

// GPU skinning master switch (--cpu-skinning forces CPU fallback)
// NOTE: GPU skinning breaks after swapchain rebuild (VSync toggle) - skinned models
// become invisible. Temporarily defaulted to CPU skinning until the GPU path is fixed.
bool g_UseGpuSkinning = true;

// 模型纹理 mipmap 开关：true=三线性 mip（默认）；false=强制 level0（禁用 mip，模型纹理用 LinearNoMip 采样器）。
// 关闭后远处会锯齿闪烁（无 mip 滤波），但近处保持 level0 原分辨率清晰。
bool g_ModelMipmap = true;

// 模型 mip LOD 偏置：负值让采样偏向高分辨率 mip（过渡距离拉远——更远处才切到低 mip）。
// 近/中距离更清晰；代价是远处低 mip 覆盖不足可能轻微闪缩。
float g_ModelMipLodBias = -1.0f;

// 模型纹理环绕方式：默认 REPEAT（obj/gltf 未指定 sampler 时的标准，平铺纹理安全）。
// 纹理自身的环绕由模型/gltf 的 sampler 定义决定（per-texture，见 ModelLoader 的 gltf sampler 解析），
// 不要全局改 CLAMP（会破坏 UV>1 的平铺纹理）。
bool g_ModelAddressRepeat = true;
