// Vulkan Manager
// 负责 Vulkan 相关的初始化、配置和管理

// 启用Vulkan光线追踪扩展（必须在包含vulkan.h之前定义）
#define VK_ENABLE_BETA_EXTENSIONS

#include "EngineGlobal.h"
#include "RenderGlobals.h"
#include "Core/Log.h"

#include "AtmosphereRenderer.h"
#include "Rendering/CMAA2.h"

#include "imgui_impl_vulkan.h"
#include "UI/Canvas2D.h"
#include <filesystem>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <vector>
#include <array>
#include <fstream>
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <iostream>
#include <chrono>
#include <cstdlib>
#include <glm/glm.hpp>
#include <glm/ext/matrix_transform.hpp>

// Android 平台使用 logcat 输出日志
#ifdef __ANDROID__
#include <android/log.h>
#endif

// 全局变量定义
// Vulkan核心对象
VkAllocationCallbacks*   g_Allocator = nullptr;  // 内存分配回调
VkInstance               g_Instance = VK_NULL_HANDLE;  // Vulkan实例
VkPhysicalDevice         g_PhysicalDevice = VK_NULL_HANDLE;  // 物理设备
VkDevice                 g_Device = VK_NULL_HANDLE;  // 逻辑设备
uint32_t                 g_QueueFamily = (uint32_t)-1;  // 队列族索引
VkQueue                  g_Queue = VK_NULL_HANDLE;  // 队列
VkDescriptorPool         g_DescriptorPool = VK_NULL_HANDLE;  // 描述符池
VkCommandPool            g_CommandPool = VK_NULL_HANDLE;  // 命令池

// 窗口和渲染相关
ImGui_ImplVulkanH_Window g_MainWindowData;  // 窗口数据
uint32_t                 g_MinImageCount = 2;  // 最小图像数量

bool                     g_SwapChainRebuild = false;  // 交换链重建标志
#ifdef __ANDROID__
bool                     g_VSyncEnabled = true;   // Android 默认开启 FIFO 垂直同步，避免移动设备撕裂和无意义的超高帧率
#else
bool                     g_VSyncEnabled = false;  // 桌面默认保持非 VSync + 三重缓冲配置
#endif
bool                     g_TripleBufferingEnabled = true;  // 桌面默认三重缓冲；Android 也保留至少 3 张交换链图像
bool                     g_IsPaused = false;  // 应用暂停标志

// 相机和输入控制器
Camera                   g_Camera(glm::vec3(0.0f, 8.0f, 12.0f)); // 编辑器相机:世界原点附近,默认朝向 -Z 看向原点
InputController          g_InputController;
RunMode                  g_RunMode = RunMode::Editor;  // 默认编辑器模式
bool                     g_EditorActive = false;       // 编辑器 DLL 附着状态（EngineMain attach/detach 置位）
GizmoMode                g_GizmoMode = GizmoMode::Translate;
bool                     g_ShowAxis = true;

// 场景渲染器
extern SceneRenderer g_SceneRenderer;
extern SkyboxRenderer g_SkyboxRenderer;
extern RenderTarget g_SceneRenderTarget;
extern RenderTarget g_GameRenderTarget;
extern FullscreenQuad g_FullscreenQuad;
extern FullscreenQuad g_SceneCompositeQuad;
extern FullscreenQuad g_GameCompositeQuad;
extern AtmosphereRenderer g_AtmosphereRenderer;
extern bool g_AtmosphereEnabled;

CMAA2 g_SceneCMAA2;
CMAA2 g_GameCMAA2;
CMAA2 g_SwapCMAA2;

// 游戏模式的 swapchain composite render pass：独立读取 GameRT 的 G-Buffer
// 纹理并写入 swapchain；离屏 geometry/composite pass 由 RenderTarget 管理。
VkRenderPass g_CompositeRenderPass = VK_NULL_HANDLE;
VkRenderPass g_CompositeUIPass = VK_NULL_HANDLE;   // swapchain UI 叠加 pass（loadOp=LOAD，链后画 UI）
std::vector<VkFramebuffer> g_CompositeFramebuffers;  // per swapchain image（离屏附件 + swapchain 附件）

// 独占全屏绕开 DWM 合成（窗口模式 present 平台税 0.6-0.9ms → ~0.05ms）
// SDL3：SetWindowFullscreenMode(mode) —— NULL=桌面无边框；非 NULL=独占；SetWindowFullscreen(bool) 应用
extern SDL_Window* window;   // EngineGlobals.cpp 全局窗口
int g_FullscreenMode = 0;    // 当前模式（控制面板读取/设置）

// 渲染场景到离屏目标（统一管线：物理天空 + 合成 subpass → 显示附件，SceneView 面板采样）
