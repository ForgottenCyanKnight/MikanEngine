#ifndef VK_ENABLE_BETA_EXTENSIONS
#define VK_ENABLE_BETA_EXTENSIONS
#endif

#include "Core/VulkanPostProcessChains.h"

#include "EngineGlobal.h"
#include "Core/Log.h"
#include "Core/ProjectManager.h"
#include "Core/Utf8Path.h"
#include "Rendering/PostProcessChain.h"
#include "Rendering/RenderWorld.h"
#include "SceneRenderer.h"

#include <filesystem>

bool g_PostProcessRebuildRequested = false;

// 后处理链按相机/视图选择：SceneView 使用项目清单中的编辑器自由相机链；
// GameView 与游戏模式共享当前场景主相机的链配置。
std::string s_ActiveScenePostProcessChainPath;
std::string s_ActiveGamePostProcessChainPath;
std::string s_ActiveSwapPostProcessChainPath;
std::string s_RequestedScenePostProcessChainPath;
std::string s_RequestedGamePostProcessChainPath;
std::string s_RequestedSwapPostProcessChainPath;
std::string s_LastMissingCameraPostProcessChain;
ECS::Entity s_CachedCameraPostProcessEntity = ECS::INVALID_ENTITY;
std::string s_CachedCameraPostProcessInput;
std::string s_CachedCameraPostProcessFallback;
std::string s_CachedCameraPostProcessAssetsRoot;
std::string s_CachedCameraPostProcessPath;
std::string s_CachedEditorPostProcessChainInput;
std::string s_CachedEditorPostProcessChainFallback;
std::string s_CachedEditorPostProcessChainAssetsRoot;
std::string s_CachedEditorPostProcessChainPath;
std::string s_LastMissingEditorPostProcessChain;

std::string DefaultPostProcessChainPath()
{
#ifdef __ANDROID__
    return ProjectManager::GetInstance().GetEngineAssetPath("postprocess_chain_mobile.json");
#else
    return ProjectManager::GetInstance().GetEngineAssetPath("postprocess_chain.json");
#endif
}

std::string ResolveEditorPostProcessChainPath()
{
    const std::string fallback = DefaultPostProcessChainPath();
    const auto& manifest = ProjectManager::GetInstance().GetManifest();
    const std::string input = manifest.editorPostProcessChain;
    const std::string assetsRoot = ProjectManager::GetInstance().GetAssetsDir();
    if (s_CachedEditorPostProcessChainInput == input &&
        s_CachedEditorPostProcessChainFallback == fallback &&
        s_CachedEditorPostProcessChainAssetsRoot == assetsRoot) {
        return s_CachedEditorPostProcessChainPath;
    }

    s_CachedEditorPostProcessChainInput = input;
    s_CachedEditorPostProcessChainFallback = fallback;
    s_CachedEditorPostProcessChainAssetsRoot = assetsRoot;
    s_CachedEditorPostProcessChainPath = fallback;
    if (input.empty()) return fallback;

    const std::string resolved = ProjectManager::GetInstance().ResolveAssetPath(input);
    if (resolved.empty()) return fallback;

#ifndef __ANDROID__
    std::error_code ec;
    if (!std::filesystem::is_regular_file(Utf8Path(resolved), ec)) {
        if (s_LastMissingEditorPostProcessChain != input) {
            LOGW("[PostProcess] editor chain missing: %s; using default chain: %s",
                 input.c_str(), fallback.c_str());
            s_LastMissingEditorPostProcessChain = input;
        }
        return s_CachedEditorPostProcessChainPath;
    }
    s_LastMissingEditorPostProcessChain.clear();
#endif

    s_CachedEditorPostProcessChainPath = resolved;
    return s_CachedEditorPostProcessChainPath;
}

std::string ResolveCameraPostProcessChainPath(const RenderWorld& world,
                                                     ECS::Entity cameraEntity)
{
    const std::string fallback = DefaultPostProcessChainPath();
    if (cameraEntity == ECS::INVALID_ENTITY) return fallback;

    const RenderWorldEntity* cameraEntityData = world.Find(cameraEntity);
    if (cameraEntityData == nullptr || !cameraEntityData->hasCamera) return fallback;

    const RenderCameraData& camera = cameraEntityData->camera;
    const std::string assetsRoot = ProjectManager::GetInstance().GetAssetsDir();
    if (s_CachedCameraPostProcessEntity == cameraEntity &&
        s_CachedCameraPostProcessInput == camera.postProcessChain &&
        s_CachedCameraPostProcessFallback == fallback &&
        s_CachedCameraPostProcessAssetsRoot == assetsRoot) {
        return s_CachedCameraPostProcessPath;
    }

    s_CachedCameraPostProcessEntity = cameraEntity;
    s_CachedCameraPostProcessInput = camera.postProcessChain;
    s_CachedCameraPostProcessFallback = fallback;
    s_CachedCameraPostProcessAssetsRoot = assetsRoot;
    s_CachedCameraPostProcessPath = fallback;
    if (camera.postProcessChain.empty()) return fallback;

    const std::string resolved = ProjectManager::GetInstance().ResolveAssetPath(camera.postProcessChain);
    if (resolved.empty()) return fallback;

#ifndef __ANDROID__
    std::error_code ec;
    if (!std::filesystem::is_regular_file(Utf8Path(resolved), ec)) {
        // 属性被逐帧检查，缺失路径只报告一次，避免把日志刷满。
        if (s_LastMissingCameraPostProcessChain != camera.postProcessChain) {
            LOGW("[PostProcess] camera chain missing: %s; using default chain: %s",
                 camera.postProcessChain.c_str(), fallback.c_str());
            s_LastMissingCameraPostProcessChain = camera.postProcessChain;
        }
        return s_CachedCameraPostProcessPath;
    }
    s_LastMissingCameraPostProcessChain.clear();
#endif
    s_CachedCameraPostProcessPath = resolved;
    return s_CachedCameraPostProcessPath;
}

void RefreshPostProcessChainSelection()
{
    const std::string fallback = DefaultPostProcessChainPath();
    const ECS::Entity mainCamera = g_SceneRenderer.GetMainCameraEntity();
    const std::string requestedScene = ResolveEditorPostProcessChainPath();
    const std::string requestedGame = ResolveCameraPostProcessChainPath(
        g_SceneRenderer.GetRenderWorld(), mainCamera);
    const std::string requestedSwap = requestedGame;

    if (s_RequestedScenePostProcessChainPath != requestedScene ||
        s_RequestedGamePostProcessChainPath != requestedGame ||
        s_RequestedSwapPostProcessChainPath != requestedSwap) {
        s_RequestedScenePostProcessChainPath = requestedScene;
        s_RequestedGamePostProcessChainPath = requestedGame;
        s_RequestedSwapPostProcessChainPath = requestedSwap;
        g_PostProcessRebuildRequested = true;
        LOGI("[Graphics] post-process selection pending: editor=%s game=%s",
             requestedScene.c_str(), requestedGame.c_str());
    }
}

bool LoadAndBuildPostProcessChain(PostProcessChain& chain,
                                         const std::string& path,
                                         uint32_t width,
                                         uint32_t height,
                                         VkRenderPass finalRenderPass,
                                         bool preserveRuntimeStates)
{
    // LoadFromJson 只在同一配置的重建场景保留运行时开关；切换相机配置时
    // 使用新 JSON 的 enable 值，避免旧 profile 的 pass 状态泄漏过来。
    chain.Cleanup();
    if (!chain.LoadFromJson(path, preserveRuntimeStates)) return false;
    return chain.Build(width, height, finalRenderPass);
}

bool BuildPostProcessChainWithFallback(PostProcessChain& chain,
                                              std::string& activePath,
                                              const std::string& requestedPath,
                                              const std::string& fallbackPath,
                                              uint32_t width,
                                              uint32_t height,
                                              VkRenderPass finalRenderPass,
                                              const char* label,
                                              bool preserveRuntimeStates)
{
    const std::string target = requestedPath.empty() ? fallbackPath : requestedPath;
    const bool samePath = activePath == target;
    if (LoadAndBuildPostProcessChain(chain, target, width, height, finalRenderPass,
                                     preserveRuntimeStates && samePath)) {
        activePath = target;
        return true;
    }

    if (target != fallbackPath &&
        LoadAndBuildPostProcessChain(chain, fallbackPath, width, height, finalRenderPass, false)) {
        activePath = fallbackPath;
        LOGW("[PostProcess] %s chain failed, using default chain: %s", label, fallbackPath.c_str());
        return true;
    }

    activePath = fallbackPath;
    LOGE("[PostProcess] %s chain could not be built: %s", label, target.c_str());
    return false;
}

void RebuildSelectedPostProcessChain(PostProcessChain& chain,
                                            std::string& activePath,
                                            const std::string& requestedPath,
                                            const std::string& fallbackPath,
                                            uint32_t width,
                                            uint32_t height,
                                            VkRenderPass finalRenderPass,
                                            const char* label)
{
    const std::string target = requestedPath.empty() ? fallbackPath : requestedPath;
    if (activePath != target || !chain.IsBuilt()) {
        BuildPostProcessChainWithFallback(chain, activePath, target, fallbackPath,
                                          width, height, finalRenderPass, label, false);
        return;
    }

    if (!chain.Build(width, height, finalRenderPass)) {
        BuildPostProcessChainWithFallback(chain, activePath, target, fallbackPath,
                                          width, height, finalRenderPass, label, false);
    }
}

void RequestPostProcessRebuild()
{
    g_PostProcessRebuildRequested = true;
}

