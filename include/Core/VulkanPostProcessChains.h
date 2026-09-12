#pragma once

#include <cstdint>
#include <string>
#include <vulkan/vulkan.h>

#include "ECS/Types.h"

class PostProcessChain;
struct RenderWorld;

extern bool g_PostProcessRebuildRequested;

extern std::string s_ActiveScenePostProcessChainPath;
extern std::string s_ActiveGamePostProcessChainPath;
extern std::string s_ActiveSwapPostProcessChainPath;
extern std::string s_RequestedScenePostProcessChainPath;
extern std::string s_RequestedGamePostProcessChainPath;
extern std::string s_RequestedSwapPostProcessChainPath;
extern std::string s_LastMissingCameraPostProcessChain;
extern ECS::Entity s_CachedCameraPostProcessEntity;
extern std::string s_CachedCameraPostProcessInput;
extern std::string s_CachedCameraPostProcessFallback;
extern std::string s_CachedCameraPostProcessAssetsRoot;
extern std::string s_CachedCameraPostProcessPath;
extern std::string s_CachedEditorPostProcessChainInput;
extern std::string s_CachedEditorPostProcessChainFallback;
extern std::string s_CachedEditorPostProcessChainAssetsRoot;
extern std::string s_CachedEditorPostProcessChainPath;
extern std::string s_LastMissingEditorPostProcessChain;

std::string DefaultPostProcessChainPath();
std::string ResolveEditorPostProcessChainPath();
std::string ResolveCameraPostProcessChainPath(const RenderWorld& world,
                                              ECS::Entity cameraEntity);
void RefreshPostProcessChainSelection();

bool LoadAndBuildPostProcessChain(PostProcessChain& chain,
                                  const std::string& path,
                                  uint32_t width,
                                  uint32_t height,
                                  VkRenderPass finalRenderPass,
                                  bool preserveRuntimeStates);

bool BuildPostProcessChainWithFallback(PostProcessChain& chain,
                                       std::string& activePath,
                                       const std::string& requestedPath,
                                       const std::string& fallbackPath,
                                       uint32_t width,
                                       uint32_t height,
                                       VkRenderPass finalRenderPass,
                                       const char* label,
                                       bool preserveRuntimeStates);

void RebuildSelectedPostProcessChain(PostProcessChain& chain,
                                     std::string& activePath,
                                     const std::string& requestedPath,
                                     const std::string& fallbackPath,
                                     uint32_t width,
                                     uint32_t height,
                                     VkRenderPass finalRenderPass,
                                     const char* label);

