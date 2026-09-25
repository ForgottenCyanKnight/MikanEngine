#define GLM_ENABLE_EXPERIMENTAL

// Android 平台禁用光线追踪

#include <vulkan/vulkan.h>

#include "SceneRenderer.h"
#include "Rendering/SceneFramePreparation.h"
#include "Rendering/SceneGeometryPass.h"
#include "Rendering/SceneDebugPass.h"
#include "Rendering/SceneShadowPass.h"
#include "Rendering/SceneRenderCpuProfile.h"
#include "AABB.h"
#include "EngineGlobal.h"
#include "EngineConfig.h"
#include "Core/Log.h"
#include "Core/JobSystem.h"
#include "Core/ProjectManager.h"
#include "Core/AssetHotReload.h"
#include "Core/RenderGlobals.h"
#include "Rendering/RenderTarget.h"


#include "ModelRenderer.h"
#include "ModelRendererInternals.h"
#include "ModelLoader.h"
#include "VoxRenderer.h"
#include "World/WorldGlobals.h"
#include "Core/LogStream.h"
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3_image/SDL_image.h>
#include <vector>
#include <unordered_set>
#include <set>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <memory>
#include <iostream>
#include <filesystem>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <glm/gtc/matrix_transform.hpp>
#include <functional>
#include <stdexcept>
#include <utility>
#include <glm/gtx/quaternion.hpp>

// 光线追踪扩展函数指针（直接使用Vulkan头文件中定义的函数，不再手动定义）

namespace {

using CpuProfileClock = std::chrono::steady_clock;

using CpuProfileFrameTiming = SceneRenderCpuProfile::FrameTiming;

struct CpuProfileAccumulator {
    uint64_t calls = 0;
    double totalMs = 0.0;
    double prepareMs = 0.0;
    double geometryMs = 0.0;
    double rootsMs = 0.0;
    double modelCollectMs = 0.0;
    double voxCollectMs = 0.0;
    double cameraCollectMs = 0.0;
    double lightCollectMs = 0.0;
    uint64_t roots = 0;
    uint64_t modelGroups = 0;
    uint64_t modelEntities = 0;

    void Record(const CpuProfileFrameTiming& timing, const RenderFrameContext& ctx,
                bool isSceneView) {
        ++calls;
        totalMs += timing.totalMs;
        prepareMs += timing.prepareMs;
        geometryMs += timing.geometryMs;
        rootsMs += timing.rootsMs;
        modelCollectMs += timing.modelCollectMs;
        voxCollectMs += timing.voxCollectMs;
        cameraCollectMs += timing.cameraCollectMs;
        lightCollectMs += timing.lightCollectMs;
        if (ctx.renderWorld != nullptr) {
            roots += static_cast<uint64_t>(ctx.renderWorld->rootEntities.size());
            modelGroups += static_cast<uint64_t>(ctx.renderWorld->modelGroups.size());
            modelEntities += static_cast<uint64_t>(ctx.renderWorld->modelEntities.size());
        }

        // 每 60 次 RenderECS 输出一次累计平均值，方便固定帧数测试脚本解析。
        if ((calls % 60u) != 0u) return;
        const double invCalls = 1.0 / static_cast<double>(calls);
        LOGI("[SceneRenderer][CPU] view=%s calls=%llu avg_total_ms=%.3f "
               "avg_prepare_ms=%.3f avg_geometry_ms=%.3f avg_roots_ms=%.3f "
               "avg_model_collect_ms=%.3f avg_vox_collect_ms=%.3f "
               "avg_camera_collect_ms=%.3f avg_light_collect_ms=%.3f "
               "avg_roots=%.1f avg_model_groups=%.1f avg_model_entities=%.1f",
               isSceneView ? "Scene" : "Game",
               static_cast<unsigned long long>(calls),
               totalMs * invCalls,
               prepareMs * invCalls,
               geometryMs * invCalls,
               rootsMs * invCalls,
               modelCollectMs * invCalls,
               voxCollectMs * invCalls,
               cameraCollectMs * invCalls,
               lightCollectMs * invCalls,
               static_cast<double>(roots) * invCalls,
               static_cast<double>(modelGroups) * invCalls,
               static_cast<double>(modelEntities) * invCalls);
    }
};

bool IsCpuProfileEnabled() {
    return SceneRenderCpuProfile::IsEnabled();
}

bool IsRenderWorldProfileEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("MIKAN_RENDERWORLD_PROFILE");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

constexpr size_t kRenderWorldAsyncFinalizeMinEntities = 512;

// 临时总开关：Hi-Z 生成、交换和消费全部关闭，便于隔离当前剔除异常。
// 恢复测试时只需改为 true；地形自身仍保持独立的 Hi-Z 关闭状态。
constexpr bool kEnableEngineHiZForTesting = false;

bool ShouldFinalizeRenderWorldAsync(size_t entityCount)
{
    const char* overrideValue = std::getenv("MIKAN_RENDERWORLD_ASYNC_FINALIZE");
    if (overrideValue != nullptr && overrideValue[0] != '\0') {
        if (overrideValue[0] == '1' || overrideValue[0] == 'y' || overrideValue[0] == 'Y') {
            return true;
        }
        if (overrideValue[0] == '0' || overrideValue[0] == 'n' || overrideValue[0] == 'N') {
            return false;
        }
    }
    return entityCount >= kRenderWorldAsyncFinalizeMinEntities;
}

double CpuProfileMilliseconds(CpuProfileClock::time_point start,
                              CpuProfileClock::time_point end) {
    return SceneRenderCpuProfile::Milliseconds(start, end);
}

thread_local CpuProfileFrameTiming g_cpuProfileFrameTiming;
CpuProfileAccumulator g_sceneCpuProfile;
CpuProfileAccumulator g_gameCpuProfile;
uint64_t g_animationCpuProfileCalls = 0;
double g_animationCpuProfileTotalMs = 0.0;
uint64_t g_animationCpuProfileUniquePoses = 0;
uint64_t g_animationCpuProfileRenderers = 0;

} // namespace

SceneRenderer::SceneRenderer()
{
    m_ShowQuadTree = false;
    //std::cout << "[SceneRenderer] Constructor called, m_ShowQuadTree = " << m_ShowQuadTree << std::endl;
}

SceneRenderer::~SceneRenderer()
{
    LOGSTREAM(Info) << "[SceneRenderer] Destructor started" << std::endl;
    LOGSTREAM(Info) << "[SceneRenderer] Calling Cleanup..." << std::endl;
    Cleanup();
    LOGSTREAM(Info) << "[SceneRenderer] Destructor completed" << std::endl;
}

SceneRenderer* SceneRenderer::GetInstance() {
    // 引擎只持有 g_SceneRenderer 这一个 SceneRenderer（定义在 EngineGlobals.cpp，
    // 由 MikanEngine_OpenProject 负责 Init/Cleanup）。这里曾经 new 出一个独立的
    // 堆实例：调用方拿到的是从未 Init 过的第二个渲染器，其 TerrainRenderer 的
    // m_DescriptorLayout / m_DescriptorPool 恒为 VK_NULL_HANDLE，地形 GPU 资源
    // 每帧建了即销毁，于是反射全景 RT 里只有模型、没有任何地形（2026-09-22 用户
    // 报告的"只有树干轮廓、无地面无纹理色"）。VmdSystem.cpp 早已写下禁止调用本
    // 函数的注释，但禁令只能防住新增调用点、防不住已有调用点。
    // 现直接返回全局唯一实例，彻底消除"第二实例"这一类 bug。
    return &g_SceneRenderer;
}

HiZComputeShader* SceneRenderer::GetGrassHiZShader(int viewSlot) {
    if (viewSlot < 0 || viewSlot > 1) {
        return nullptr;
    }

    if (g_RunMode == RunMode::Editor && viewSlot == 0) {
        return m_EnableSceneGrassHiZCulling ? &m_SceneHiZShader : nullptr;
    }
    if ((g_RunMode == RunMode::Editor && viewSlot == 1) ||
        (g_RunMode == RunMode::Game && viewSlot == 0)) {
        return m_EnableGameGrassHiZCulling ? &m_HiZShader : nullptr;
    }
    return nullptr;
}

bool SceneRenderer::IsGrassHiZCullingEnabled(int viewSlot) const {
    if (viewSlot < 0 || viewSlot > 1) {
        return false;
    }
    if (g_RunMode == RunMode::Editor && viewSlot == 0) {
        return m_EnableSceneGrassHiZCulling;
    }
    return (g_RunMode == RunMode::Editor && viewSlot == 1) ||
           (g_RunMode == RunMode::Game && viewSlot == 0)
        ? m_EnableGameGrassHiZCulling
        : false;
}

void SceneRenderer::RefreshRenderWorld()
{
    if (m_RenderWorldFrameActive) {
        LOGSTREAM(Warn) << "[SceneRenderer] RenderWorld refresh ignored during active render frame" << std::endl;
        return;
    }

    // Preserve the synchronous contract for editor/tool callers.  The main
    // loop uses BeginRenderWorldBuild below so Finalize can overlap with
    // post-capture CPU work before BeginRenderFrame publishes the snapshot.
    BeginRenderWorldBuild();
    CompleteRenderWorldBuild();
}

void SceneRenderer::BeginRenderWorldBuild()
{
    if (m_RenderWorldFrameActive) {
        LOGSTREAM(Warn) << "[SceneRenderer] RenderWorld build ignored during active render frame" << std::endl;
        return;
    }
    if (m_RenderWorldFinalizePending) {
        CompleteRenderWorldBuild();
    }

    m_RenderWorldBuildStart = std::chrono::steady_clock::now();
    RenderWorldBuilder::Capture(m_RenderWorldBuildBuffer);
    const auto captureEnd = std::chrono::steady_clock::now();
    m_RenderWorldCaptureMilliseconds =
        std::chrono::duration<double, std::milli>(captureEnd - m_RenderWorldBuildStart).count();
    m_RenderWorldFinalizeMilliseconds = 0.0;
    m_RenderWorldPublishWaitMilliseconds = 0.0;
    m_RenderWorldFinalizedAsynchronously =
        ShouldFinalizeRenderWorldAsync(m_RenderWorldBuildBuffer.entities.size());
    if (!m_RenderWorldFinalizedAsynchronously) {
        // Thread handoff overhead dominates tiny scenes.  Keep the same
        // staging/publish contract, but finalize directly until the snapshot
        // is large enough for worker overlap to be worthwhile.
        const auto finalizeStart = std::chrono::steady_clock::now();
        RenderWorldBuilder::Finalize(m_RenderWorldBuildBuffer);
        const auto finalizeEnd = std::chrono::steady_clock::now();
        m_RenderWorldFinalizeMilliseconds =
            std::chrono::duration<double, std::milli>(finalizeEnd - finalizeStart).count();
        m_RenderWorldFinalizePending = true;
        return;
    }
    if (!m_RenderWorldFinalizeWorker.Submit(m_RenderWorldBuildBuffer)) {
        throw std::runtime_error("RenderWorld finalize worker rejected a build");
    }
    m_RenderWorldFinalizePending = true;
}

void SceneRenderer::CompleteRenderWorldBuild()
{
    if (!m_RenderWorldFinalizePending) return;

    const auto waitStart = std::chrono::steady_clock::now();
    double workerFinalizeMilliseconds = 0.0;
    try {
        m_RenderWorldFinalizeWorker.Wait(&workerFinalizeMilliseconds);
    } catch (...) {
        m_RenderWorldFinalizePending = false;
        throw;
    }
    const auto waitEnd = std::chrono::steady_clock::now();

    m_RenderWorldFinalizePending = false;
    m_RenderWorldPublishWaitMilliseconds =
        std::chrono::duration<double, std::milli>(waitEnd - waitStart).count();
    if (m_RenderWorldFinalizedAsynchronously) {
        m_RenderWorldFinalizeMilliseconds = workerFinalizeMilliseconds;
    }
    RenderWorldBuildStats buildStats;
    RenderWorldBuilder::CollectStats(
        m_RenderWorldBuildBuffer,
        buildStats,
        m_RenderWorldCaptureMilliseconds + m_RenderWorldFinalizeMilliseconds);
    buildStats.captureMilliseconds = m_RenderWorldCaptureMilliseconds;
    buildStats.finalizeMilliseconds = m_RenderWorldFinalizeMilliseconds;
    buildStats.publishWaitMilliseconds = m_RenderWorldPublishWaitMilliseconds;
    buildStats.finalizedAsynchronously = m_RenderWorldFinalizedAsynchronously;
    std::swap(m_RenderWorld, m_RenderWorldBuildBuffer);
    m_RenderWorldBuildStats = buildStats;
    ++m_RenderWorldBuildCount;
    m_RenderWorldValid = true;

    if (IsRenderWorldProfileEnabled() && (m_RenderWorldBuildCount % 60u) == 0u) {
        LOGE("[RenderWorld][CPU] builds=%llu frame=%llu build_ms=%.3f "
               "capture_ms=%.3f finalize_ms=%.3f publish_wait_ms=%.3f mode=%s "
               "incremental=%s transform_reused=%u transform_recomputed=%u "
               "component_reused=%u component_recomputed=%u "
               "hierarchy=%u entities=%u visible=%u model_groups=%u "
               "model_entities=%u vox_groups=%u vox_entities=%u cameras=%u "
               "lights=%u terrains=%u waters=%u skyboxes=%u clouds=%u "
               "particles=%u estimated_container_bytes=%llu validation=%s",
               static_cast<unsigned long long>(m_RenderWorldBuildCount),
               static_cast<unsigned long long>(buildStats.frameNumber),
               buildStats.buildMilliseconds,
               buildStats.captureMilliseconds,
               buildStats.finalizeMilliseconds,
               buildStats.publishWaitMilliseconds,
               buildStats.finalizedAsynchronously ? "async" : "sync",
               buildStats.incrementalCaptureUsed ? "yes" : "no",
               buildStats.reusedTransformCount,
               buildStats.recomputedTransformCount,
               buildStats.reusedComponentCount,
               buildStats.recomputedComponentCount,
               buildStats.hierarchyEntityCount,
               buildStats.entityCount,
               buildStats.visibleEntityCount,
               buildStats.modelGroupCount,
               buildStats.modelEntityCount,
               buildStats.voxGroupCount,
               buildStats.voxEntityCount,
               buildStats.cameraCount,
               buildStats.lightCount,
               buildStats.terrainCount,
               buildStats.waterCount,
               buildStats.skyboxCount,
               buildStats.cloudCount,
               buildStats.particleCount,
               static_cast<unsigned long long>(buildStats.estimatedContainerBytes),
               buildStats.validationChecked
                   ? (buildStats.invariantsValid ? "pass" : "fail")
                   : "off");
    }
}

void SceneRenderer::EnsureRenderWorldPublished()
{
    // A caller may query renderer data during the first frame while the
    // post-gameplay capture is still being finalized.  Complete that pending
    // snapshot instead of starting a second capture or reading an empty
    // published buffer.
    if (m_RenderWorldFinalizePending) {
        CompleteRenderWorldBuild();
    }
    if (!m_RenderWorldValid) {
        RefreshRenderWorld();
    }
}

void SceneRenderer::BeginRenderFrame()
{
    EnsureRenderWorldPublished();
    m_RenderWorldFrameActive = true;
    // A single render frame may record SceneView and GameView.  Advance the
    // shared-preparation epoch once here so both views can reuse frame-local
    // caches while standalone PrepareFrame callers still get a fresh epoch.
    ++m_FrameId;
    ModelRendererDetail::BeginSharedBonePaletteFrame();
    UpdateSceneMode();
}

void SceneRenderer::EndRenderFrame()
{
    m_RenderWorldFrameActive = false;
    // The resize nudge is render-only. Do not leak it into gameplay/editor
    // queries after this frame, and do not persistently move the ECS camera.
    m_ResizeCameraNudgePending = false;
    // Keep the last published snapshot available between frames.  Gameplay
    // and world systems can query camera/render data before the next explicit
    // publish without rebuilding the ECS snapshot a second time.  The next
    // RefreshRenderWorld call remains the sole point that replaces it.
}

void SceneRenderer::ForceCameraRefreshAfterResize()
{
    // A millimetre is enough to invalidate exact camera-keyed CPU visibility
    // caches while remaining visually negligible. The nudge is applied only
    // during the next BeginRenderFrame/EndRenderFrame interval.
    m_ResizeCameraNudgePending = true;
}

void SceneRenderer::Cleanup()
{
    if (m_RenderWorldFinalizePending) {
        try {
            CompleteRenderWorldBuild();
        } catch (const std::exception& error) {
            LOGSTREAM(Error) << "[SceneRenderer] RenderWorld finalize failed during cleanup: "
                      << error.what() << std::endl;
            m_RenderWorldFinalizePending = false;
        } catch (...) {
            LOGSTREAM(Error) << "[SceneRenderer] RenderWorld finalize failed during cleanup" << std::endl;
            m_RenderWorldFinalizePending = false;
        }
    }

    extern VkDevice g_Device;
    extern VkAllocationCallbacks* g_Allocator;
    bool deviceValid = (g_Device != VK_NULL_HANDLE);

    // The probe owns its cubemap, face framebuffers, resolve pipeline and
    // composite quad.  It must be torn down with the rest of the scene
    // renderer so a subsequent project can recreate descriptors against the
    // new scene/device lifetime instead of reusing stale Vulkan handles.
    m_SceneReflectionProbe.Cleanup();
    
    m_DebugRenderer.Cleanup();
    m_FullscreenQuad.Cleanup();
    
    for (auto& [path, renderer] : m_ModelRenderers) {
        renderer->Cleanup();
    }
    m_ModelRenderers.clear();
    m_ModelLoadFailureNextRetryFrame.clear();
    ModelRendererDetail::ReleaseSharedBonePalette();
    
    // 清理体素渲染器
    for (auto& [path, renderer] : m_VoxRenderers) {
        renderer->Cleanup();
    }
    m_VoxRenderers.clear();
    
    // 清理 VoxelMeshMultiDrawIndirect（必须在 Vulkan 设备销毁前）
    if (m_VoxelMeshMultiDrawIndirect) {
        m_VoxelMeshMultiDrawIndirect.reset();
    }
    
    // 清理体素世界渲染器
    if (m_WorldRenderer) {
        m_WorldRenderer->Cleanup();
        if (g_WorldRenderer == m_WorldRenderer.get()) {
            g_WorldRenderer = nullptr;
        }
        m_WorldRenderer.reset();
    }

    // SceneRenderer 可能跨场景/设备生命周期复用；Entity 是可复用索引，
    // 不能让旧场景的模型历史矩阵泄漏到下一次渲染。
    m_PrevModelMatrices.clear();
    m_PrevModelMatricesSceneVersion = std::numeric_limits<uint32_t>::max();
    m_PrevProjViewMatrix = glm::mat4(1.0f);
    m_HasPrevFrameMatrices = false;

    // Point/CSM 阴影属于场景渲染资源；项目切换时一并释放，下一项目
    // 首次需要阴影时由 Ensure*Shadows 重新创建。
    if (m_PointShadows) {
        m_PointShadows->Cleanup();
    }
    if (m_CascadeShadows) {
        m_CascadeShadows->Cleanup();
    }

    // 清理高度图地形（必须先于纹理池和 Vulkan 设备销毁）
    m_TerrainRenderer.Cleanup();
    // Hi-Z image views are referenced by grass descriptors; release the
    // consumer before destroying the per-view Hi-Z resources.
    m_HiZShader.Cleanup();
    m_SceneHiZShader.Cleanup();
    m_EnableTerrainHiZCulling = false;
    m_EnableGameGrassHiZCulling = false;
    m_EnableSceneGrassHiZCulling = false;
    // 清理水体（必须先于 Vulkan 设备销毁）
    m_WaterRenderer.Cleanup();
    // 水面目标 RT（在设备仍有效时释放）
    m_WaterTarget.Cleanup();

    m_RenderWorld.Clear();
    m_RenderWorldBuildBuffer.Clear();
    m_RenderWorldBuildStats = {};
    m_RenderWorldBuildCount = 0;
    m_RenderWorldValid = false;
    m_RenderWorldFrameActive = false;
    m_RenderWorldFinalizePending = false;
    m_RenderWorldCaptureMilliseconds = 0.0;
    m_RenderWorldFinalizeMilliseconds = 0.0;
    m_RenderWorldPublishWaitMilliseconds = 0.0;
    m_RenderWorldFinalizedAsynchronously = false;
    
    // 清理相机 Uniform Buffer
    m_CameraUniformBuffer.Cleanup();
    
    BaseRenderer::Cleanup();
}

void SceneRenderer::Init(VkRenderPass renderPass)
{
    m_RenderPass = renderPass;
    
    // 清空模型渲染器，确保在应用恢复时重新加载模型
    for (auto& [path, renderer] : m_ModelRenderers) {
        renderer->Cleanup();
    }
    m_ModelRenderers.clear();
    m_ModelLoadFailureNextRetryFrame.clear();
    
    // 初始化线框渲染器
    m_DebugRenderer.Init(renderPass);

    // 初始化高度图地形管线；具体地形资源在 PrepareFrame 中按 ECS 实体惰性创建
    m_TerrainRenderer.Init(renderPass);
    // 场景反射探针（cubemap）：水面合成 pass 的 scene_probe 输入要求「每一帧
    // 都存在有效视图」——描述符一旦绑到 NULL 视图，采样端行为未定义。因此
    // 在这里就把 6 面 128² RGBA16F + 深度建好，而不是等第一次捕获再懒建。
    if (!m_SceneReflectionProbe.Init(SceneReflectionProbe::kDefaultFaceSize)) {
        LOGSTREAM(Error) << "[SceneRenderer] reflection probe cubemap init FAILED"
                         << std::endl;
    }
    // 水面目标 RT（deferred water compositing）：render pass 一次创建，
    // 地形水与实体水管线均指向它；水面不再写 G-buffer/主深度。
    if (!m_WaterTarget.EnsureRenderPass()) {
        LOGSTREAM(Error) << "[SceneRenderer] water target render pass creation FAILED" << std::endl;
    }
    m_TerrainRenderer.EnsureWaterTargetPipeline(m_WaterTarget.GetRenderPass());
    m_WaterRenderer.Init(m_WaterTarget.GetRenderPass());
    
    // Hi-Z 临时全局关闭：不初始化/生成/交换任何 Hi-Z，所有剔除回到
    // CPU 粗筛 + GPU 视锥/距离细筛或原有 fallback。
    m_HiZShader.Cleanup();
    m_SceneHiZShader.Cleanup();
    m_EnableHiZCulling = false;
    m_EnableTerrainHiZCulling = false;
    m_EnableGameGrassHiZCulling = false;
    m_EnableSceneGrassHiZCulling = false;
    if constexpr (kEnableEngineHiZForTesting) {
        if (g_GameRenderTarget.GetHiZOccluderImage() != VK_NULL_HANDLE &&
            g_GameRenderTarget.GetHiZOccluderImageView() != VK_NULL_HANDLE &&
            g_GameRenderTarget.GetWidth() > 1 && g_GameRenderTarget.GetHeight() > 1) {
            m_EnableGameGrassHiZCulling = m_HiZShader.Init(
                g_Device, g_PhysicalDevice,
                g_GameRenderTarget.GetWidth(), g_GameRenderTarget.GetHeight());
        }
        if (g_SceneRenderTarget.GetHiZOccluderImage() != VK_NULL_HANDLE &&
            g_SceneRenderTarget.GetHiZOccluderImageView() != VK_NULL_HANDLE &&
            g_SceneRenderTarget.GetWidth() > 1 && g_SceneRenderTarget.GetHeight() > 1) {
            m_EnableSceneGrassHiZCulling = m_SceneHiZShader.Init(
                g_Device, g_PhysicalDevice,
                g_SceneRenderTarget.GetWidth(), g_SceneRenderTarget.GetHeight());
        }
    }
    if (!kEnableEngineHiZForTesting) {
        LOGSTREAM(Info) << "[SceneRenderer] Hi-Z globally disabled; using frustum/distance fallback"
                        << std::endl;
    }
    
    // Composite quad 由 VulkanManager 绑定到独立 composite render pass；
    // 这里不再创建旧的 input-attachment subpass 2 兼容管线。

    // 初始化 GPU 驱动的 Multi Draw Indirect 渲染器（体素静态渲染）
    // Android：体素世界已关闭，MDI 的 3M/6M 顶点缓冲区也一并跳过（省内存）
#ifndef __ANDROID__
    if (!m_VoxelMeshMultiDrawIndirect) {
        m_VoxelMeshMultiDrawIndirect = std::make_unique<VoxelMeshMultiDrawIndirect>();
        if (m_VoxelMeshMultiDrawIndirect->Initialize(64, 3000000, 6000000)) {
            LOGSTREAM(Info) << "[SceneRenderer] GPU-based Multi Draw Indirect initialized" << std::endl;
        } else {
            LOGSTREAM(Error) << "[SceneRenderer] MDI initialization FAILED, falling back to direct rendering" << std::endl;
            m_VoxelMeshMultiDrawIndirect.reset();
        }
    }
#endif

    // 初始化体素世界渲染器（无限世界，从 OpenGL 版迁移；g_EnableVoxelWorld=false 时不创建）
    if (g_EnableVoxelWorld) {
        if (!m_WorldRenderer) {
            m_WorldRenderer = std::make_unique<WorldRenderer>();
            m_WorldRenderer->Init(renderPass);
            // 世界数据由 WorldSystem 创建后通过 SetWorld 关联（g_World）
            m_WorldRenderer->SetWorld(g_World);
            g_WorldRenderer = m_WorldRenderer.get();
            LOGSTREAM(Info) << "[SceneRenderer] WorldRenderer initialized" << std::endl;
        } else {
            // RecreateSwapChain 后重建：重新关联世界并同步全局指针
            m_WorldRenderer->Init(renderPass);
            m_WorldRenderer->SetWorld(g_World);
            g_WorldRenderer = m_WorldRenderer.get();
        }
    }

    // ===== 资产热重载：注册 handler（Init 可能因 swapchain 重建多次调用，注册幂等）=====
    AssetHotReload::GetInstance().SetTextureReloadHandler(
        [this](const std::string& resolvedPath) { ReloadTextureAsset(resolvedPath); });
    AssetHotReload::GetInstance().SetModelReloadHandler(
        [this](const std::string& resolvedPath) { ReloadModelAsset(resolvedPath); });

    LOGSTREAM(Info) << "[SceneRenderer] Init completed" << std::endl;
}

// 纹理热重载：TexturePool 命中已加载条目才重载；随后所有 ModelRenderer 重写引用该
// 纹理的材质描述符绑定（句柄不变）。必须在 GPU 空闲安全点调用（Poll 契约）。
void SceneRenderer::ReloadTextureAsset(const std::string& resolvedPath)
{
    if (!g_TexturePool) return;
    if (!g_TexturePool->ReloadTextureByPath(resolvedPath)) {
        return;   // 未加载过（首次使用自然取新文件）或重载失败（旧纹理保留）
    }
    for (auto& [path, renderer] : m_ModelRenderers) {
        if (renderer) renderer->RefreshTextureDescriptors(resolvedPath);
    }
}

// 模型热重载：先刷新 ModelLoader CPU 缓存（含动画缓存），成功才销毁对应渲染器，
// 下一帧 SceneFramePreparation 按 assetPath 懒重建——组件的 modelPath 字符串不动。
// 动画-only 姿态渲染器（key = modelPath + "#entity:N"）一并销毁：其持有的
// shared_ptr<const AnimationAsset> 指向已被逐出的旧资产，必须随之重建。
void SceneRenderer::ReloadModelAsset(const std::string& resolvedPath)
{
    if (resolvedPath.empty()) return;

    // m_ModelRenderers 的 key 是组件里的 modelPath 原值（通常项目相对路径），
    // 与扫描到的绝对路径经 ResolveAssetPath 归一后比较。
    auto resolveKeyBase = [](const std::string& key) -> std::string {
        const size_t tagPos = key.find("#entity:");
        const std::string basePath = tagPos != std::string::npos ? key.substr(0, tagPos) : key;
        return ProjectManager::GetInstance().ResolveAssetPath(basePath);
    };

    std::string canonicalKey;                 // 几何渲染器 key（== modelPath 原值）
    std::vector<std::string> poseKeys;        // 动画-only 姿态渲染器 key
    for (const auto& [key, renderer] : m_ModelRenderers) {
        const std::string resolvedKey = resolveKeyBase(key);
        if (resolvedKey.empty() || resolvedKey != resolvedPath) continue;
        if (key.find("#entity:") != std::string::npos) {
            poseKeys.push_back(key);
        } else {
            canonicalKey = key;
        }
    }

    if (canonicalKey.empty() && poseKeys.empty()) {
        LOGD("[AssetHotReload] changed model not in use, skipped: %s", resolvedPath.c_str());
        return;
    }

    if (!canonicalKey.empty()) {
        // 先重载 CPU 缓存；新文件读取失败（DCC 保存到一半）则保留旧渲染器，回滚语义。
        const ModelLoadResult reloaded = ModelLoader::ReloadModelWithTextures(canonicalKey);
        if (reloaded.meshData.subMeshes.empty()) {
            LOGW("[AssetHotReload] model reload failed (kept old GPU renderer): %s", canonicalKey.c_str());
            return;
        }

        auto it = m_ModelRenderers.find(canonicalKey);
        if (it != m_ModelRenderers.end() && it->second) {
            it->second->Cleanup();
        }
        m_ModelRenderers.erase(canonicalKey);
        m_ModelLoadFailureNextRetryFrame.erase(canonicalKey);
    }

    for (const std::string& poseKey : poseKeys) {
        auto it = m_ModelRenderers.find(poseKey);
        if (it != m_ModelRenderers.end() && it->second) {
            it->second->Cleanup();
        }
        m_ModelRenderers.erase(poseKey);
    }

    LOGI("[AssetHotReload] model reloaded, rebuilding renderer next frame: %s (pose renderers: %d)",
         canonicalKey.c_str(), static_cast<int>(poseKeys.size()));
}

void SceneRenderer::Render(VkCommandBuffer commandBuffer, const glm::mat4& view, const glm::mat4& proj)
{
    (void)commandBuffer;
    (void)view;
    (void)proj;
}



ECS::Entity SceneRenderer::GetMainCameraEntity()
{
    EnsureRenderWorldPublished();
    for (const auto& camera : m_RenderWorld.cameras) {
        if (camera.isMainCamera) return camera.entity;
    }
    return ECS::INVALID_ENTITY;
}

bool SceneRenderer::GetMainCameraMatrices(float aspectRatio, glm::mat4& outView, glm::mat4& outProj, glm::vec3& outCameraPos)
{
    EnsureRenderWorldPublished();
    const ECS::Entity entity = GetMainCameraEntity();
    if (entity != ECS::INVALID_ENTITY) {
        const RenderWorldEntity* entityData = m_RenderWorld.Find(entity);
        if (entityData == nullptr || !entityData->hasCamera) return false;
        const RenderCameraData& camera = entityData->camera;
        const glm::vec3 resizeNudge =
            (m_RenderWorldFrameActive && m_ResizeCameraNudgePending)
                ? glm::vec3(0.001f, 0.0f, 0.0f)
                : glm::vec3(0.0f);
        outCameraPos = camera.position + resizeNudge;
        outView = camera.GetViewMatrix();
        if (resizeNudge.x != 0.0f || resizeNudge.y != 0.0f || resizeNudge.z != 0.0f) {
            // V' = V * T(-delta): move the render camera without changing
            // the ECS transform that gameplay/editor logic owns.
            outView = glm::translate(outView, -resizeNudge);
        }
        outProj = camera.GetProjectionMatrix(aspectRatio);
        outProj[1][1] *= -1;
        return true;
    }

    return false;
}

// 相机实体列表帧缓存:本帧未收集过(或实体集合已变化)则全树收集一次并标记,否则直接返回缓存
const std::vector<ECS::Entity>& SceneRenderer::EnsureCameraEntitiesCached()
{
    EnsureRenderWorldPublished();
    const uint32_t sceneVersion = m_RenderWorld.entitySetVersion;
    if (m_CameraCacheFrameId == m_FrameId && m_CameraCacheSceneVersion == sceneVersion) {
        return m_CameraEntitiesCache; // 本帧已收集且实体集合未变,命中缓存
    }

    m_CameraEntitiesCache.clear();
    m_CameraEntitiesCache.reserve(m_RenderWorld.cameras.size());
    for (const auto& camera : m_RenderWorld.cameras) {
        m_CameraEntitiesCache.push_back(camera.entity);
    }
    m_CameraCacheFrameId = m_FrameId;
    m_CameraCacheSceneVersion = sceneVersion;
    return m_CameraEntitiesCache;
}

// ===== z-prepass（render pass subpass 0，depth-only）：只写 3D 深度 =====
// MRT 几何阶段（subpass 1）被遮挡片元在 fragment shader 执行前被深度测试剔除，
// 减少 G-Buffer 4 附件写入的 overdraw（几何 shader 含 discard 关键字禁用 early-z，z-prepass 无 discard 可早期剔除）。
// 由 VulkanManager 在 BeginRender 后、NextSubpass 前调用；2D 场景无 3D 几何跳过。
// 说明：MDI 静态体素不参与本 pass（其实例数据由 MRT 阶段每帧 UpdateVoxelModel 更新，此处提交会一帧滞后错位；
//        MRT 阶段 MDI 片元仍受益于 z-prepass 对其他模型的深度剔除）；动态/无 MDI 静态体素参与。
void SceneRenderer::RenderDepthPrepass(VkCommandBuffer commandBuffer, int width, int height,
                                       const glm::mat4& view, const glm::mat4& proj,
                                       bool useMainCameraFrustum, int viewSlot,
                                       int probeFace)
{
    // 2D/3D 统一走 3D 管线：z-prepass 无条件提交（2D 场景无模型实体 → 空提交，无害）
    const glm::mat4 projView = proj * view;
    EnsureRenderWorldPublished();
    const RenderWorld& world = m_RenderWorld;
    // 与 useSubMeshCulling 开关绑定：开关关时回编辑器视锥（全景）
    const std::array<Plane, 6> zpreFrustumPlanes =
        (useMainCameraFrustum && m_HasMainCameraFrustum && m_MainCamUseSubMeshCulling) ? m_MainCameraFrustumPlanes
                                                                                       : AABBUtils::ExtractFrustumPlanes(projView);
    const bool zpreCullReady = m_OcclusionCulling.HasCullingContext();
    const auto modelBatches = BuildModelBatches();
    for (const auto& group : modelBatches) {
        if (group.entities.empty()) continue;
        ModelRenderer* renderer = group.renderer;
        if (renderer == nullptr || !renderer->HasModelLoaded()) continue;
        std::vector<ModelInstanceData> instances;
        std::vector<size_t> zpreVisible;
        // Skinning changes the rendered bounds. The submesh BVH stores bind/raw
        // mesh bounds, so using it for a skinned model can drop a whole submesh.
        const bool zpreUseSubMeshCulling = zpreCullReady && !renderer->HasSkinning();
        instances.reserve(group.entities.size());
        for (size_t entityIdx = 0; entityIdx < group.entities.size(); ++entityIdx) {
            const auto& entity = group.entities[entityIdx];
            const RenderWorldEntity* entityData = world.Find(entity);
            if (entityData == nullptr || !entityData->hasTransform) continue;
            const glm::mat4 modelMatrix = entityData->transform.worldMatrix;
            ModelInstanceData id{};
            id.model = modelMatrix;
            id.prevModel = modelMatrix;   // z-prepass 不需要运动矢量
            if (entityIdx < group.animationRenderers.size()) {
                ModelRendererDetail::ApplySharedBonePalette(
                    id, group.animationRenderers[entityIdx]);
            }
            instances.push_back(id);
            // subMesh 视锥剔除（与几何 pass 同逻辑；多实体并集，保守）
            if (zpreUseSubMeshCulling) {
                std::vector<size_t> vis = m_OcclusionCulling.GetVisibleSubMeshIndices(
                    renderer, modelMatrix, zpreFrustumPlanes, {}, glm::vec3(0.0f), true, entity);
                zpreVisible.insert(zpreVisible.end(), vis.begin(), vis.end());
            }
        }
        if (instances.empty()) continue;
        if (zpreUseSubMeshCulling) {
            std::sort(zpreVisible.begin(), zpreVisible.end());
            zpreVisible.erase(std::unique(zpreVisible.begin(), zpreVisible.end()), zpreVisible.end());
            if (zpreVisible.empty()) continue;   // 剔除后无可见 subMesh
        }
        renderer->RenderDepthOnly(commandBuffer, width, height, projView, instances, zpreVisible);
    }

    // ---- 体素（动态 + 无 MDI 的静态 fallback）----
    const glm::mat4 flipZ = glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, 1.0f, -1.0f));
    for (const auto& voxGroup : world.voxGroups) {
        if (voxGroup.entities.empty()) continue;
        auto it = m_VoxRenderers.find(voxGroup.voxPath);
        if (it == m_VoxRenderers.end() || !it->second || !it->second->HasLoaded()) continue;
        auto& voxRenderer = it->second;
        std::vector<VoxelInstanceData> staticInstances;
        std::vector<VoxelInstanceData> dynamicInstances;
        for (const auto& entity : voxGroup.entities) {
            const RenderWorldEntity* entityData = world.Find(entity);
            if (entityData == nullptr || !entityData->hasTransform) continue;
            const glm::mat4 modelMatrix = entityData->transform.worldMatrix;
            VoxelInstanceData vd{};
            vd.model = modelMatrix * flipZ;
            vd.prevModel = vd.model;
            vd.worldMinBounds = voxRenderer->GetMinBounds();
            vd.voxelSize = voxRenderer->GetVoxelSize();
            vd.albedoColor = glm::vec4(1.0f);
            vd.materialData = glm::vec4(0.0f, 0.75f, 1.0f, 0.0f);
            const bool isStatic = !entityData->hasVoxel || entityData->voxel.isStatic;
            if (isStatic) staticInstances.push_back(vd);
            else dynamicInstances.push_back(vd);
        }
        if (!dynamicInstances.empty()) {
            voxRenderer->RenderInstanced(commandBuffer, width, height, projView, projView, glm::vec3(0.0f), dynamicInstances, true);
        }
        if (!staticInstances.empty() && !m_VoxelMeshMultiDrawIndirect) {
            voxRenderer->RenderMeshWithBackfaceCulling(commandBuffer, width, height, projView, projView, glm::vec3(0.0f), staticInstances, true, true);
        }
    }

    // ---- 高度图地形：独立收集（z-prepass 早于 PrepareFrame）并提交三档 patch 实例 ----
    glm::vec3 terrainCameraPosition = glm::vec3(glm::inverse(view)[3]);
    if (useMainCameraFrustum && m_HasMainCameraFrustum) {
        terrainCameraPosition = GetCameraPosition();
    }
    m_TerrainRenderer.Prepare(m_RenderWorld, terrainCameraPosition, zpreFrustumPlanes, true,
                              viewSlot);
    m_TerrainRenderer.RenderDepthPrepass(commandBuffer, width, height, projView,
                                         terrainCameraPosition, viewSlot, probeFace);
    // 水面已移出 z-prepass：若水写主深度，不透明几何会被 early-z 剔掉，
    // 水底在 G-buffer/场景色里就成空洞——水底信息必须完整保留给后处理合成。
    // 水面数据由 SceneFramePreparation 统一 Prepare，几何 pass 后写独立目标 RT。
}

PointShadowRenderer* SceneRenderer::EnsurePointShadows()
{
    if (!m_PointShadows) {
        m_PointShadows = std::make_unique<PointShadowRenderer>();
    }
    if (!m_PointShadows->IsInitialized()) {
        m_PointShadows->Init();
    }
    return m_PointShadows.get();
}

void SceneRenderer::RenderPointShadowMaps(VkCommandBuffer commandBuffer, const ShadowLight* lights, int lightCount, int shadowMapSize)
{
    SceneShadowPass::RenderPointShadowMaps(*this, commandBuffer, lights, lightCount, shadowMapSize);
}

CascadeShadowRenderer* SceneRenderer::EnsureCascadeShadows()
{
    if (!m_CascadeShadows) {
        m_CascadeShadows = std::make_unique<CascadeShadowRenderer>();
    }
    if (!m_CascadeShadows->IsInitialized()) {
        m_CascadeShadows->Init();
    }
    return m_CascadeShadows.get();
}

void SceneRenderer::RenderCascadeShadowMaps(VkCommandBuffer commandBuffer, int slot,
                                            const glm::mat4& view, const glm::mat4& proj, const glm::vec3& lightDir)
{
    SceneShadowPass::RenderCascadeShadowMaps(*this, commandBuffer, slot, view, proj, lightDir);
}

void SceneRenderer::RenderGeometryOpaque(RenderFrameContext& ctx)
{
    SceneGeometryPass::Render(*this, ctx);
}

void SceneRenderer::RenderECS(VkCommandBuffer commandBuffer, int width, int height, const glm::mat4& view, const glm::mat4& proj, const glm::mat4& cullView, const glm::mat4& cullProj, VulkanBuffer& uniformBuffer, VkDescriptorSet descriptorSet, SceneRenderer::ViewRenderMode mode, int viewSlotOverride, int probeFaceOverride)
{
    // 统一视图抽象：编辑器场景视图 = 编辑器相机 + 调试渲染；游戏视图 = 主相机
    const bool isSceneView = (mode == SceneRenderer::ViewRenderMode::EditorScene);
    const bool cpuProfileEnabled = IsCpuProfileEnabled();
    CpuProfileClock::time_point cpuStart;
    if (cpuProfileEnabled) {
        cpuStart = CpuProfileClock::now();
        g_cpuProfileFrameTiming = {};
    }

    // ===== Pass 0: 帧上下文准备 =====
    RenderFrameContext ctx;
    ctx.commandBuffer = commandBuffer;
    ctx.width = width; ctx.height = height;
    ctx.view = view; ctx.proj = proj;
    ctx.cullView = cullView; ctx.cullProj = cullProj;
    ctx.isSceneView = isSceneView;
    // 视图槽位：显式覆盖优先（反射探针 = 2），否则沿用「场景视图 0 / 游戏视图 1」。
    ctx.viewSlot = viewSlotOverride >= 0 ? viewSlotOverride : (isSceneView ? 0 : 1);
    // 探针面序号：只有探针视图有意义，决定相机 UBO / 描述符集取 [face] 哪一份。
    ctx.probeFace = viewSlotOverride >= 0 ? probeFaceOverride : 0;
    ctx.uniformBuffer = &uniformBuffer;
    ctx.descriptorSet = descriptorSet;
    ctx.projView = proj * view;                                             // 当前帧 ProjView
    ctx.prevProjView = m_HasPrevFrameMatrices ? m_PrevProjViewMatrix : ctx.projView; // 上一帧 ProjView（运动矢量）

    // ===== Pass 1: 场景收集 / 剔除准备（纯 CPU，不发 GPU 命令）=====
    PrepareFrame(ctx);
    const CpuProfileClock::time_point afterPrepare =
        cpuProfileEnabled ? CpuProfileClock::now() : CpuProfileClock::time_point{};

    // ===== Pass 2: 几何渲染（模型 + 静态/动态体素 + 无限体素世界）=====
    RenderGeometryOpaque(ctx);
    if (cpuProfileEnabled) {
        const auto afterGeometry = CpuProfileClock::now();
        g_cpuProfileFrameTiming.prepareMs =
            CpuProfileMilliseconds(cpuStart, afterPrepare);
        g_cpuProfileFrameTiming.geometryMs =
            CpuProfileMilliseconds(afterPrepare, afterGeometry);
        g_cpuProfileFrameTiming.totalMs =
            CpuProfileMilliseconds(cpuStart, afterGeometry);

        CpuProfileAccumulator& accumulator =
            isSceneView ? g_sceneCpuProfile : g_gameCpuProfile;
        accumulator.Record(g_cpuProfileFrameTiming, ctx, isSceneView);
    }

    // ===== Pass 3: 调试叠加已移到链末 RenderOverlayLinework（UI overlay pass，不再写 G-Buffer）=====
}

// ===== 水面目标 RT 统一编排（deferred water compositing）=====
// 几何 pass 结束后由帧管线调用（编辑器 SceneView/GameView、游戏模式、
// 安卓四条路径）。地形涂刷水与 WaterComponent 实体水画进同一张
// WaterTargetRT（R=mask G=NDC 深度 BA=法线），后处理 water_composite
// 用它和主深度做双深度比较判覆盖，再算吸收/反射/高光。
void SceneRenderer::RenderWaterTargets(VkCommandBuffer commandBuffer, uint32_t width, uint32_t height,
                                       const glm::mat4& projView,
                                       const glm::mat4& prevProjView,
                                       const glm::vec3& cameraPosition)
{
    if (commandBuffer == VK_NULL_HANDLE || width == 0 || height == 0) {
        return;
    }
    const bool hasTerrainWater = m_TerrainRenderer.HasPreparedTerrain();
    const bool hasEntityWater = m_WaterRenderer.IsInitialized() &&
                                m_WaterRenderer.GetVisibleWaterCount() > 0;
    if (!hasTerrainWater && !hasEntityWater) {
        return;
    }
    if (!m_WaterTarget.EnsureSize(width, height)) {
        return;
    }

    m_WaterTarget.BeginPass(commandBuffer);
    VkViewport viewport{};
    viewport.width = static_cast<float>(width);
    viewport.height = static_cast<float>(height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{};
    scissor.extent = { width, height };
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    m_TerrainRenderer.DrawWaterToTarget(commandBuffer,
                                        static_cast<int>(width), static_cast<int>(height),
                                        projView);
    m_WaterRenderer.DrawWater(commandBuffer, projView, prevProjView, cameraPosition);
    m_WaterTarget.EndPass(commandBuffer);

    // ===== TEMP-PROBE: 水面 RT GPU 回读诊断（MIKAN_WATER_PROBE=1 启用）=====
    // 第 30 帧把颜色附件拷到 host-visible buffer，隔 2 帧（跨 frames-in-flight，
    // fence 已保证拷贝完成）CPU 统计 mask 覆盖。定位"合成 ветbranch 没跑"是
    // mask 侧（守门/顶点/描述符）还是 gtao 侧（绑定/条件）。
    {
        static const bool s_probeEnabled = [] {
            const char* e = std::getenv("MIKAN_WATER_PROBE");
            return e != nullptr && e[0] != '\0';
        }();
        if (s_probeEnabled) {
            // RGBA16F 半精度 → float（IEEE 754 half）
            static auto halfToFloat = [](uint16_t h) -> float {
                uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
                uint32_t exp = (h & 0x7C00u) >> 10;
                uint32_t man = h & 0x03FFu;
                uint32_t bits;
                if (exp == 0) {
                    if (man == 0) {
                        bits = sign;
                    } else {
                        int e2 = -1;
                        uint32_t m = man;
                        do { ++e2; m <<= 1; } while ((m & 0x0400u) == 0);
                        m &= 0x03FFu;
                        bits = sign | (static_cast<uint32_t>(127 - 15 - e2) << 23) | (m << 13);
                    }
                } else if (exp == 31) {
                    bits = sign | 0x7F800000u | (man << 13);
                } else {
                    bits = sign | ((exp - 15u + 127u) << 23) | (man << 13);
                }
                float f;
                std::memcpy(&f, &bits, sizeof(f));
                return f;
            };

            if (m_WaterProbeCopyPending && m_WaterProbeBuffer.GetMappedPtr() != nullptr) {
                m_WaterProbeCopyPending = false;
                const uint16_t* t = static_cast<const uint16_t*>(m_WaterProbeBuffer.GetMappedPtr());
                const size_t total = static_cast<size_t>(m_WaterProbeWidth) * m_WaterProbeHeight;
                size_t masked = 0, minX = SIZE_MAX, minY = SIZE_MAX, maxX = 0, maxY = 0;
                double sumZ = 0.0, maxZ = 0.0;
                for (size_t y = 0; y < m_WaterProbeHeight; ++y) {
                    for (size_t x = 0; x < m_WaterProbeWidth; ++x) {
                        const uint16_t* px = t + (y * m_WaterProbeWidth + x) * 4;
                        if (halfToFloat(px[0]) > 0.5f) {
                            ++masked;
                            minX = std::min(minX, x); maxX = std::max(maxX, x);
                            minY = std::min(minY, y); maxY = std::max(maxY, y);
                            float z = halfToFloat(px[1]);
                            sumZ += z;
                            maxZ = std::max(maxZ, static_cast<double>(z));
                        }
                    }
                }
                if (masked > 0) {
                    LOGSTREAM(Info) << "[WaterProbeRT] " << masked << "/" << total
                                    << " px mask=1 (" << (100.0 * masked / total) << "%)"
                                    << " bbox=[" << minX << "," << minY << "]-[" << maxX << "," << maxY << "]"
                                    << " meanZ=" << (sumZ / masked) << " maxZ=" << maxZ << std::endl;
                } else {
                    LOGSTREAM(Info) << "[WaterProbeRT] mask ALL ZERO over " << total << " px"
                                    << " (RT " << m_WaterProbeWidth << "x" << m_WaterProbeHeight << ")"
                                    << std::endl;
                }
                // dump mask 为 8bit 灰度 PGM（PIL 可直接读），与盘上水位图对照
                {
                    std::vector<uint8_t> gray(total, 0);
                    for (size_t i = 0; i < total; ++i) {
                        gray[i] = static_cast<uint8_t>(std::min(
                            255.0f, halfToFloat(t[i * 4]) * 255.0f));
                    }
                    FILE* fp = nullptr;
                    if (fopen_s(&fp, "D:/Engine project/vulkan engine/tmp/water_mask_dump.pgm", "wb") == 0 && fp) {
                        fprintf(fp, "P5\n%u %u\n255\n", m_WaterProbeWidth, m_WaterProbeHeight);
                        fwrite(gray.data(), 1, gray.size(), fp);
                        fclose(fp);
                        LOGI("[WaterProbeRT] mask dumped to tmp/water_mask_dump.pgm");
                    }
                }
            }

            if (++m_WaterProbeFrameCounter == 30) {
                const VkDeviceSize bytes = VkDeviceSize(width) * height * 8; // RGBA16F
                if (m_WaterProbeBuffer.GetBuffer() == VK_NULL_HANDLE ||
                    m_WaterProbeWidth != width || m_WaterProbeHeight != height) {
                    m_WaterProbeBuffer.Cleanup();
                    if (m_WaterProbeBuffer.Create(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                        m_WaterProbeBuffer.Map();
                        m_WaterProbeWidth = width;
                        m_WaterProbeHeight = height;
                    } else {
                        m_WaterProbeWidth = m_WaterProbeHeight = 0;
                    }
                }
                if (m_WaterProbeBuffer.GetBuffer() != VK_NULL_HANDLE) {
                    VkImageMemoryBarrier toSrc{};
                    toSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                    toSrc.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                    toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    toSrc.image = m_WaterTarget.GetImage();
                    toSrc.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                    toSrc.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                    toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                    vkCmdPipelineBarrier(commandBuffer,
                                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                         0, nullptr, 0, nullptr, 1, &toSrc);
                    VkBufferImageCopy region{};
                    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                    region.imageExtent = { width, height, 1 };
                    vkCmdCopyImageToBuffer(commandBuffer, m_WaterTarget.GetImage(),
                                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                           m_WaterProbeBuffer.GetBuffer(), 1, &region);
                    VkImageMemoryBarrier toRead = toSrc;
                    toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                    toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    toRead.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                    toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                    vkCmdPipelineBarrier(commandBuffer,
                                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                                         0, nullptr, 0, nullptr, 1, &toRead);
                    m_WaterProbeCopyPending = true;
                    LOGI("[WaterProbeRT] copy armed (frame 30, %ux%u)", width, height);
                }
            }
        }
    }
}

// ===== 场景模式判定：仅启用 2D 相机且无主 3D 相机 → g_SceneIs2D=true =====
// 每帧无条件调用（VulkanManager::FrameRender 开头，任何渲染分支判断之前）。
// 历史 bug：此判定曾内联在 PrepareFrame（仅 3D 路径执行），2D 场景不经过它，
// 导致 2D→3D 场景切换后 g_SceneIs2D 卡在 true，3D 场景被错误地按 2D 分支渲染（无画面）。
void SceneRenderer::UpdateSceneMode() {
    EnsureRenderWorldPublished();
    bool has3DCamera = false;
    for (const auto& camera : m_RenderWorld.cameras) {
        if (camera.isMainCamera) {
            has3DCamera = true;
            break;
        }
    }

    bool has2DCamera = false;
    for (const auto& entity : m_RenderWorld.entities) {
        if (entity.hasCamera2D && entity.camera2DEnabled) {
            has2DCamera = true;
            break;
        }
    }
    g_SceneIs2D = has2DCamera && !has3DCamera;
}

void SceneRenderer::PrepareFrame(RenderFrameContext& ctx)
{
    const bool cpuProfileEnabled = IsCpuProfileEnabled();
    const auto prepareStart = cpuProfileEnabled
        ? CpuProfileClock::now()
        : CpuProfileClock::time_point{};
    SceneFramePreparation::Prepare(*this, ctx, cpuProfileEnabled,
                                   g_cpuProfileFrameTiming, prepareStart);
}

void SceneRenderer::RenderOverlayLinework(VkCommandBuffer commandBuffer, int width, int height,
                                          VkRenderPass uiPass, const glm::mat4& view, const glm::mat4& proj)
{
    SceneDebugPass::RenderOverlay(*this, commandBuffer, width, height, uiPass, view, proj);
}
void SceneRenderer::RenderSceneView(VkCommandBuffer commandBuffer, int width, int height, const glm::mat4& view, const glm::mat4& proj)
{
    // 渲染场景（编辑器场景视图：编辑器相机 + 调试渲染）
    RenderECS(commandBuffer, width, height, view, proj, view, proj, m_SceneUniformBuffer, m_SceneDescriptorSet, ViewRenderMode::EditorScene);
}

void SceneRenderer::RenderSceneView(VkCommandBuffer commandBuffer, int width, int height, const glm::mat4& view, const glm::mat4& proj, const glm::mat4& cullView, const glm::mat4& cullProj)
{
    RenderECS(commandBuffer, width, height, view, proj, cullView, cullProj, m_SceneUniformBuffer, m_SceneDescriptorSet, ViewRenderMode::EditorScene);
}

void SceneRenderer::RenderGameView(VkCommandBuffer commandBuffer, int width, int height, const glm::mat4& view, const glm::mat4& proj)
{
    RenderECS(commandBuffer, width, height, view, proj, view, proj, m_GameUniformBuffer, m_GameDescriptorSet, ViewRenderMode::Game);
}

void SceneRenderer::RenderProbeView(VkCommandBuffer commandBuffer, int width, int height,
                                     const glm::mat4& view, const glm::mat4& proj,
                                     int probeFace)
{
    // 反射探针 = 第三个视图：与 SceneView/GameView 共用同一条视图管线，
    // 只是相机换成探针 6 面中的某一个、资源槽位换成 2。
    //
    // isSceneView=false：不收集调试线（探针不需要线框/gizmo overlay），
    // 也不启用编辑器场景相机的二次剔除。视口裁剪仍然照常按本面的 view/proj
    // 走，因此每个面只画自己视锥内的几何 —— 这正是「真正独立相机」的意义。
    //
    // probeFace：6 个面在同一命令缓冲里顺序录制，而地形/草的相机 UBO 是 host
    // memcpy 写的（无命令流排序），必须按面各自分段，否则 6 面全用最后一面矩阵。
    //
    // 资源句柄沿用场景视图的空壳（m_SceneUniformBuffer / m_SceneDescriptorSet
    // 自早期起就不再被模型管线消费，模型数据走各自的 UBO/描述符集）。
    RenderECS(commandBuffer, width, height, view, proj, view, proj,
              m_SceneUniformBuffer, m_SceneDescriptorSet,
              ViewRenderMode::Game, 2, probeFace);
}

void SceneRenderer::UpdateModelAnimations(float deltaTime)
{
    const bool cpuProfileEnabled = IsCpuProfileEnabled();
    const CpuProfileClock::time_point animationStart = cpuProfileEnabled
        ? CpuProfileClock::now()
        : CpuProfileClock::time_point{};

    // Pose results are frame-local: entities with the same asset/clip/time
    // share one hierarchy traversal, while a new gameplay frame samples only
    // the poses that actually occur in that frame.
    ModelLoader::ClearAnimationPoseCache();

    // EngineMain publishes the post-gameplay snapshot before this call.  Keep
    // the fallback for editor/tool callers that invoke animation sync alone.
    if (!m_RenderWorldValid && !m_RenderWorldFinalizePending) RefreshRenderWorld();

    // Capture has completed before the worker is submitted.  Finalize only
    // writes groups/lists/index data, so reading the captured entity records
    // here is independent of the worker and keeps the current-frame pose
    // selection from waiting on the worker.
    const RenderWorld& animationWorld = m_RenderWorldFinalizePending
        ? m_RenderWorldBuildBuffer
        : m_RenderWorld;
    std::unordered_set<std::string> vmdModelRendererKeys;
    for (const auto& entity : animationWorld.entities) {
        const bool vmdTargetsModel =
            entity.hasVmdPlayer && entity.hasMesh && !entity.mesh.modelPath.empty() &&
            entity.vmd.enabled && entity.vmd.hasMotion &&
            (entity.vmd.target == RenderVmdTarget::Model ||
             (entity.vmd.target == RenderVmdTarget::Auto && !entity.hasCamera));
        if (vmdTargetsModel) {
            vmdModelRendererKeys.insert(
                entity.mesh.modelPath + "#entity:" + std::to_string(entity.entity));
        }

        if (entity.hasAnimator && entity.hasMesh && !entity.mesh.modelPath.empty()) {
            std::string rendererKey = entity.mesh.modelPath;
            if (entity.hasAnimator || entity.hasVmdPlayer) {
                rendererKey += "#entity:" + std::to_string(entity.entity);
            }
            ModelRenderer* r = GetModelRendererForKey(rendererKey);
            if (r && r->HasAnimation()) {
                if (entity.animator.clipIndex != r->GetCurrentClip()) {
                    r->PlayAnimation(entity.animator.clipIndex, entity.animator.loop);
                } else {
                    r->SetAnimationLoop(entity.animator.loop);
                }
                r->SetAnimationSpeed(entity.animator.speed);
                r->SetAnimationPlaying(entity.animator.playing);
            }
        }
    }

    // Advance animation state on the caller thread, then batch pure pose
    // sampling jobs.  Vulkan buffer writes remain on this thread below.
    struct AnimationPoseRequestKey {
        const AnimationAsset* asset = nullptr;
        int clipIndex = 0;
        float time = 0.0f;

        bool operator==(const AnimationPoseRequestKey& other) const {
            return asset == other.asset && clipIndex == other.clipIndex &&
                time == other.time;
        }
    };
    struct AnimationPoseRequestKeyHash {
        size_t operator()(const AnimationPoseRequestKey& key) const noexcept {
            size_t hash = std::hash<const void*>{}(key.asset);
            hash ^= std::hash<int>{}(key.clipIndex) + static_cast<size_t>(0x9e3779b9u) +
                (hash << 6) + (hash >> 2);
            hash ^= std::hash<float>{}(key.time) + static_cast<size_t>(0x9e3779b9u) +
                (hash << 6) + (hash >> 2);
            return hash;
        }
    };
    struct AnimationPoseTask {
        std::shared_ptr<const AnimationAsset> asset;
        int clipIndex = 0;
        float time = 0.0f;
        std::vector<ModelRenderer*> renderers;
        std::shared_ptr<const AnimationPose> pose;
        JobSystem::JobHandle handle;
    };

    std::unordered_map<AnimationPoseRequestKey, size_t,
                       AnimationPoseRequestKeyHash> taskIndices;
    std::vector<std::shared_ptr<AnimationPoseTask>> poseTasks;

    // 推进所有模型渲染器动画状态。动画资产存在时不在这里采样，避免
    // 每个实体把昂贵的层级遍历重新做一遍。
    for (auto& [path, renderer] : m_ModelRenderers) {
        (void)path;
        if (vmdModelRendererKeys.find(path) != vmdModelRendererKeys.end()) continue;
        if (!renderer) continue;

        if (!renderer->GetAnimationAsset()) {
            // Legacy/custom payloads have no immutable asset to sample on a
            // worker; preserve their original per-renderer update path.
            renderer->UpdateAnimation(deltaTime);
            continue;
        }

        const bool needsPose = renderer->AdvanceAnimationState(deltaTime);
        if (!needsPose) {
            renderer->RefreshCurrentAnimationPose();
            continue;
        }

        const auto asset = renderer->GetAnimationAsset();
        const AnimationPoseRequestKey key{
            asset.get(), renderer->GetCurrentClip(), renderer->GetAnimationTime()};
        const auto taskIt = taskIndices.find(key);
        if (taskIt != taskIndices.end()) {
            poseTasks[taskIt->second]->renderers.push_back(renderer.get());
            continue;
        }

        auto task = std::make_shared<AnimationPoseTask>();
        task->asset = asset;
        task->clipIndex = key.clipIndex;
        task->time = key.time;
        task->renderers.push_back(renderer.get());
        taskIndices.emplace(key, poseTasks.size());
        poseTasks.push_back(std::move(task));
    }

    const bool useParallelPoseJobs = poseTasks.size() > 1;
    for (const auto& task : poseTasks) {
        if (!useParallelPoseJobs) {
            task->pose = ModelLoader::SampleAnimationPose(
                task->asset, task->clipIndex, task->time);
            continue;
        }
        try {
            task->handle = JobSystem::GetInstance().Submit([task] {
                task->pose = ModelLoader::SampleAnimationPose(
                    task->asset, task->clipIndex, task->time);
            });
        } catch (const std::exception& error) {
            LOGE("[SceneRenderer] animation pose job submit failed: %s",
                error.what());
            task->pose = ModelLoader::SampleAnimationPose(
                task->asset, task->clipIndex, task->time);
        } catch (...) {
            LOGE("[SceneRenderer] animation pose job submit failed");
            task->pose = ModelLoader::SampleAnimationPose(
                task->asset, task->clipIndex, task->time);
        }
    }

    static uint32_t s_animationPoseJobDiag = 0;
    if (useParallelPoseJobs && s_animationPoseJobDiag < 3) {
        ++s_animationPoseJobDiag;
        LOGI("[SceneRenderer][AnimationJobs] unique_poses=%zu workers=%zu mode=parallel",
                    poseTasks.size(), JobSystem::GetInstance().WorkerCount());
    }

    for (const auto& task : poseTasks) {
        if (task->handle.valid()) {
            try {
                task->handle.get();
            } catch (const std::exception& error) {
                LOGE("[SceneRenderer] animation pose job failed: %s",
                    error.what());
                task->pose.reset();
            } catch (...) {
                LOGE("[SceneRenderer] animation pose job failed");
                task->pose.reset();
            }
        }

        for (ModelRenderer* renderer : task->renderers) {
            if (renderer == nullptr || renderer->ApplyAnimationPose(task->pose)) {
                continue;
            }
            // Sampling failure is rare (corrupt/legacy payload); retain the
            // already-advanced time and let the serial compatibility path
            // recover the current frame without advancing it again.
            renderer->UpdateAnimation(0.0f);
        }
    }

    if (cpuProfileEnabled) {
        ++g_animationCpuProfileCalls;
        g_animationCpuProfileTotalMs +=
            CpuProfileMilliseconds(animationStart, CpuProfileClock::now());
        g_animationCpuProfileUniquePoses +=
            static_cast<uint64_t>(poseTasks.size());
        g_animationCpuProfileRenderers +=
            static_cast<uint64_t>(m_ModelRenderers.size());
        if ((g_animationCpuProfileCalls % 60u) == 0u) {
            const double invCalls = 1.0 /
                static_cast<double>(g_animationCpuProfileCalls);
            LOGI("[SceneRenderer][CPU] animations=%llu "
                        "avg_animation_ms=%.3f avg_unique_poses=%.1f "
                        "avg_renderers=%.1f",
                        static_cast<unsigned long long>(g_animationCpuProfileCalls),
                        g_animationCpuProfileTotalMs * invCalls,
                        static_cast<double>(g_animationCpuProfileUniquePoses) * invCalls,
                        static_cast<double>(g_animationCpuProfileRenderers) * invCalls);
        }
    }
}

void SceneRenderer::PreloadModels()
{
    EnsureRenderWorldPublished();
    const RenderWorld& world = m_RenderWorld;

    bool hasNewModels = false;

    for (const auto& group : world.modelGroups) {
        if (group.entities.empty()) continue;

        const std::string assetPath = group.modelPath.empty()
            ? group.rendererKey : group.modelPath;
        auto assetRendererIt = m_ModelRenderers.find(assetPath);
        if (assetRendererIt == m_ModelRenderers.end()) {
            const auto retryIt = m_ModelLoadFailureNextRetryFrame.find(assetPath);
            const bool retryDeferred =
                retryIt != m_ModelLoadFailureNextRetryFrame.end() &&
                m_FrameId < retryIt->second;
            if (!retryDeferred) {
                auto renderer = std::make_unique<ModelRenderer>();
                renderer->Init(m_RenderPass);
#ifdef __ANDROID__
                LOGI("[Android] Preload model: %s", assetPath.c_str());
#endif

                renderer->LoadModel(assetPath);
                bool loadSuccess = renderer->HasModelLoaded();

                if (loadSuccess) {
                    hasNewModels = true;  // 标记有新模型加载
                    m_ModelLoadFailureNextRetryFrame.erase(assetPath);
                    m_ModelRenderers[assetPath] = std::move(renderer);
                } else {
                    m_ModelLoadFailureNextRetryFrame[assetPath] =
                        m_FrameId + kFailedModelRetryIntervalFrames;
                }
            }
        }

        // 动画/VMD 实体保留独立姿态，但不再重复创建几何和管线资源。
        if (group.rendererKey != assetPath &&
            m_ModelRenderers.find(group.rendererKey) == m_ModelRenderers.end()) {
            auto poseRenderer = std::make_unique<ModelRenderer>();
            if (poseRenderer->LoadAnimationOnly(assetPath)) {
                m_ModelRenderers[group.rendererKey] = std::move(poseRenderer);
            }
        }

        auto geometryRendererIt = m_ModelRenderers.find(assetPath);
        if (geometryRendererIt == m_ModelRenderers.end() || !geometryRendererIt->second ||
            !geometryRendererIt->second->HasModelLoaded()) {
            continue;
        }
        ModelRenderer* renderer = geometryRendererIt->second.get();

        for (const auto& entity : group.entities) {
            const RenderWorldEntity* entityData = world.Find(entity);
            if (entityData != nullptr && entityData->hasMaterial) {
                const auto& material = entityData->material;
                
                for (size_t i = 0; i < renderer->GetSubMeshCount(); i++) {
                    renderer->UpdateSubMeshSampler(i, 0, material.albedoSamplerType);
                    renderer->UpdateSubMeshSampler(i, 1, material.normalSamplerType);
                    renderer->UpdateSubMeshSampler(i, 2, material.roughnessSamplerType);
                    renderer->UpdateSubMeshSampler(i, 3, material.metallicSamplerType);
                    renderer->UpdateSubMeshSampler(i, 4, material.aoSamplerType);
                    renderer->UpdateSubMeshSampler(i, 5, material.emissiveSamplerType);
                }
            }
        }
    }
    (void)hasNewModels;
}

std::vector<SceneModelBatch> SceneRenderer::BuildModelBatches() const
{
    std::vector<SceneModelBatch> batches;
    std::unordered_map<std::string, size_t> batchIndices;

    for (const auto& sourceGroup : m_RenderWorld.modelGroups) {
        if (sourceGroup.entities.empty()) continue;

        const auto poseRendererIt = m_ModelRenderers.find(sourceGroup.rendererKey);
        if (poseRendererIt == m_ModelRenderers.end() || !poseRendererIt->second) {
            continue;
        }

        ModelRenderer* sourceRenderer = poseRendererIt->second.get();
        const std::string assetPath = sourceGroup.modelPath.empty()
            ? sourceRenderer->GetModelPath() : sourceGroup.modelPath;
        ModelRenderer* geometryRenderer = sourceRenderer;
        const auto geometryRendererIt = m_ModelRenderers.find(assetPath);
        if (geometryRendererIt != m_ModelRenderers.end() && geometryRendererIt->second &&
            geometryRendererIt->second->HasModelLoaded()) {
            geometryRenderer = geometryRendererIt->second.get();
        }
        if (!geometryRenderer->HasModelLoaded()) continue;

        for (const ECS::Entity entity : sourceGroup.entities) {
            const RenderWorldEntity* entityData = m_RenderWorld.Find(entity);
            bool doubleSided = geometryRenderer->HasDoubleSided();
            bool wireframe = false;
            if (entityData != nullptr && entityData->hasRenderFlags) {
                doubleSided = doubleSided || entityData->render.doubleSided;
                wireframe = entityData->render.wireframe;
            }

            // The separator is not a valid path character on the supported
            // platforms and keeps this small frame-local key allocation-free
            // beyond the normal std::string bucket lookup.
            std::string batchKey = assetPath;
            batchKey.push_back('\x1f');
            batchKey += doubleSided ? '1' : '0';
            batchKey += wireframe ? '1' : '0';

            auto [batchIt, inserted] = batchIndices.emplace(batchKey, batches.size());
            if (inserted) {
                SceneModelBatch batch;
                batch.modelPath = assetPath;
                batch.renderer = geometryRenderer;
                batch.doubleSided = doubleSided;
                batch.wireframe = wireframe;
                batches.push_back(std::move(batch));
            }

            SceneModelBatch& batch = batches[batchIt->second];
            batch.entities.push_back(entity);
            batch.animationRenderers.push_back(sourceRenderer);
        }
    }

    return batches;
}

ModelRenderer* SceneRenderer::GetModelRenderer(const std::string& modelPath)
{
    auto it = m_ModelRenderers.find(modelPath);
    if (it != m_ModelRenderers.end() && it->second && it->second->HasModelLoaded()) {
        return it->second.get();
    }

    // Animated entities use a path#entity key, but callers that only need the
    // model geometry (camera collision, AABB/quad-tree/editor inspection) still
    // ask by asset path. Fall back to the first renderer loaded from that path.
    for (const auto& [key, renderer] : m_ModelRenderers) {
        (void)key;
        if (renderer && renderer->HasModelLoaded() && renderer->GetModelPath() == modelPath) {
            return renderer.get();
        }
    }
    return nullptr;
}

ModelRenderer* SceneRenderer::GetModelRendererForKey(const std::string& rendererKey)
{
    const auto it = m_ModelRenderers.find(rendererKey);
    return it != m_ModelRenderers.end() ? it->second.get() : nullptr;
}

ModelRenderer* SceneRenderer::GetModelRendererByIndex(size_t index)
{
    if (index >= m_ModelRenderers.size()) {
        return nullptr;
    }
    
    auto it = m_ModelRenderers.begin();
    std::advance(it, index);
    return it->second.get();
}

VoxRenderer* SceneRenderer::GetVoxRenderer(const std::string& voxPath)
{
    auto it = m_VoxRenderers.find(voxPath);
    if (it != m_VoxRenderers.end()) {
        return it->second.get();
    }
    return nullptr;
}

bool SceneRenderer::HasVoxRenderer(const std::string& voxPath) const
{
    return m_VoxRenderers.find(voxPath) != m_VoxRenderers.end();
}

glm::vec3 SceneRenderer::GetCameraPosition() {
    EnsureRenderWorldPublished();
    for (const auto& camera : m_RenderWorld.cameras) {
        if (camera.isMainCamera) {
            if (m_RenderWorldFrameActive && m_ResizeCameraNudgePending) {
                return camera.position + glm::vec3(0.001f, 0.0f, 0.0f);
            }
            return camera.position;
        }
    }
    
    // 如果没有主摄像机，返回默认位置
    return glm::vec3(0.0f, 0.0f, 3.0f);
}
