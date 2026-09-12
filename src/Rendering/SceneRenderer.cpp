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


#include "ModelRenderer.h"
#include "ModelLoader.h"
#include "VoxRenderer.h"
#include "World/WorldGlobals.h"
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3_image/SDL_image.h>
#include <vector>
#include <unordered_set>
#include <set>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <memory>
#include <iostream>
#include <filesystem>
#include <chrono>
#include <cstdlib>
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
        roots += static_cast<uint64_t>(ctx.rootEntities.size());
        modelGroups += static_cast<uint64_t>(ctx.modelGroups.size());
        modelEntities += static_cast<uint64_t>(ctx.allModelEntities.size());

        // 每 60 次 RenderECS 输出一次累计平均值，方便固定帧数测试脚本解析。
        if ((calls % 60u) != 0u) return;
        const double invCalls = 1.0 / static_cast<double>(calls);
        printf("[SceneRenderer][CPU] view=%s calls=%llu avg_total_ms=%.3f "
               "avg_prepare_ms=%.3f avg_geometry_ms=%.3f avg_roots_ms=%.3f "
               "avg_model_collect_ms=%.3f avg_vox_collect_ms=%.3f "
               "avg_camera_collect_ms=%.3f avg_light_collect_ms=%.3f "
               "avg_roots=%.1f avg_model_groups=%.1f avg_model_entities=%.1f\n",
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

double CpuProfileMilliseconds(CpuProfileClock::time_point start,
                              CpuProfileClock::time_point end) {
    return SceneRenderCpuProfile::Milliseconds(start, end);
}

thread_local CpuProfileFrameTiming g_cpuProfileFrameTiming;
CpuProfileAccumulator g_sceneCpuProfile;
CpuProfileAccumulator g_gameCpuProfile;

} // namespace

SceneRenderer::SceneRenderer()
{
    m_ShowQuadTree = false;
    //std::cout << "[SceneRenderer] Constructor called, m_ShowQuadTree = " << m_ShowQuadTree << std::endl;
}

SceneRenderer::~SceneRenderer()
{
    std::cout << "[SceneRenderer] Destructor started" << std::endl;
    std::cout << "[SceneRenderer] Calling Cleanup..." << std::endl;
    Cleanup();
    std::cout << "[SceneRenderer] Destructor completed" << std::endl;
}

SceneRenderer* SceneRenderer::GetInstance() {
    static SceneRenderer* instance = nullptr;
    if (!instance) {
        instance = new SceneRenderer();
    }
    return instance;
}

void SceneRenderer::RefreshRenderWorld()
{
    // The published buffer is read-only for all passes between Begin/End.
    // Accidentally rebuilding during that interval would invalidate pointers
    // held by RenderFrameContext, so keep the current frame stable.
    if (m_RenderWorldFrameActive) {
        std::cerr << "[SceneRenderer] RenderWorld refresh ignored during active render frame" << std::endl;
        return;
    }

    RenderWorldBuildStats buildStats;
    RenderWorldBuilder::Build(m_RenderWorldBuildBuffer, &buildStats);
    std::swap(m_RenderWorld, m_RenderWorldBuildBuffer);
    m_RenderWorldBuildStats = buildStats;
    ++m_RenderWorldBuildCount;
    m_RenderWorldValid = true;

    if (IsRenderWorldProfileEnabled() && (m_RenderWorldBuildCount % 60u) == 0u) {
        printf("[RenderWorld][CPU] builds=%llu frame=%llu build_ms=%.3f "
               "hierarchy=%u entities=%u visible=%u model_groups=%u "
               "model_entities=%u vox_groups=%u vox_entities=%u cameras=%u "
               "lights=%u terrains=%u waters=%u skyboxes=%u clouds=%u "
               "particles=%u estimated_container_bytes=%llu validation=%s\n",
               static_cast<unsigned long long>(m_RenderWorldBuildCount),
               static_cast<unsigned long long>(buildStats.frameNumber),
               buildStats.buildMilliseconds,
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

void SceneRenderer::BeginRenderFrame()
{
    if (!m_RenderWorldValid) {
        RefreshRenderWorld();
    }
    m_RenderWorldFrameActive = true;
    UpdateSceneMode();
}

void SceneRenderer::EndRenderFrame()
{
    m_RenderWorldFrameActive = false;
    m_RenderWorldValid = false;
}

void SceneRenderer::Cleanup()
{
    extern VkDevice g_Device;
    extern VkAllocationCallbacks* g_Allocator;
    bool deviceValid = (g_Device != VK_NULL_HANDLE);
    
    m_HiZShader.Cleanup();
    m_DebugRenderer.Cleanup();
    m_FullscreenQuad.Cleanup();
    
    for (auto& [path, renderer] : m_ModelRenderers) {
        renderer->Cleanup();
    }
    m_ModelRenderers.clear();
    
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
    // 清理水体（必须先于 Vulkan 设备销毁）
    m_WaterRenderer.Cleanup();

    m_RenderWorld.Clear();
    m_RenderWorldBuildBuffer.Clear();
    m_RenderWorldBuildStats = {};
    m_RenderWorldBuildCount = 0;
    m_RenderWorldValid = false;
    m_RenderWorldFrameActive = false;
    
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
    
    // 初始化线框渲染器
    m_DebugRenderer.Init(renderPass);

    // 初始化高度图地形管线；具体地形资源在 PrepareFrame 中按 ECS 实体惰性创建
    m_TerrainRenderer.Init(renderPass);
    // 初始化水体共享网格和不透明管线；实体数据在 PrepareFrame 中收集
    m_WaterRenderer.Init(renderPass);
    
    // Hi-Z 生成与消费当前均被 VulkanManager 关闭（见 RenderGameToTarget/RenderGameComposite）。
    // 不创建未使用的计算管线：部分驱动在初始化该管线时会访问无效的扩展路径，
    // 导致无体素的 headless/原型运行在首帧前崩溃。恢复 Hi-Z 消费者时再显式开启初始化。
    m_EnableHiZCulling = false;
    std::cout << "[SceneRenderer] Hi-Z disabled (no active consumer)" << std::endl;
    
    // Composite quad 由 VulkanManager 绑定到独立 composite render pass；
    // 这里不再创建旧的 input-attachment subpass 2 兼容管线。

    // 初始化 GPU 驱动的 Multi Draw Indirect 渲染器（体素静态渲染）
    // Android：体素世界已关闭，MDI 的 3M/6M 顶点缓冲区也一并跳过（省内存）
#ifndef __ANDROID__
    if (!m_VoxelMeshMultiDrawIndirect) {
        m_VoxelMeshMultiDrawIndirect = std::make_unique<VoxelMeshMultiDrawIndirect>();
        if (m_VoxelMeshMultiDrawIndirect->Initialize(64, 3000000, 6000000)) {
            std::cout << "[SceneRenderer] GPU-based Multi Draw Indirect initialized" << std::endl;
        } else {
            std::cout << "[SceneRenderer] MDI initialization FAILED, falling back to direct rendering" << std::endl;
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
            std::cout << "[SceneRenderer] WorldRenderer initialized" << std::endl;
        } else {
            // RecreateSwapChain 后重建：重新关联世界并同步全局指针
            m_WorldRenderer->Init(renderPass);
            m_WorldRenderer->SetWorld(g_World);
            g_WorldRenderer = m_WorldRenderer.get();
        }
    }

    std::cout << "[SceneRenderer] Init completed" << std::endl;
}

void SceneRenderer::Render(VkCommandBuffer commandBuffer, const glm::mat4& view, const glm::mat4& proj)
{
    (void)commandBuffer;
    (void)view;
    (void)proj;
}



ECS::Entity SceneRenderer::GetMainCameraEntity()
{
    if (!m_RenderWorldValid) RefreshRenderWorld();
    for (const auto& camera : m_RenderWorld.cameras) {
        if (camera.isMainCamera) return camera.entity;
    }
    return ECS::INVALID_ENTITY;
}

bool SceneRenderer::GetMainCameraMatrices(float aspectRatio, glm::mat4& outView, glm::mat4& outProj, glm::vec3& outCameraPos)
{
    if (!m_RenderWorldValid) RefreshRenderWorld();
    const ECS::Entity entity = GetMainCameraEntity();
    if (entity != ECS::INVALID_ENTITY) {
        const RenderWorldEntity* entityData = m_RenderWorld.Find(entity);
        if (entityData == nullptr || !entityData->hasCamera) return false;
        const RenderCameraData& camera = entityData->camera;
        outCameraPos = camera.position;
        outView = camera.GetViewMatrix();
        outProj = camera.GetProjectionMatrix(aspectRatio);
        outProj[1][1] *= -1;
        return true;
    }

    return false;
}

// 相机实体列表帧缓存:本帧未收集过(或实体集合已变化)则全树收集一次并标记,否则直接返回缓存
const std::vector<ECS::Entity>& SceneRenderer::EnsureCameraEntitiesCached()
{
    if (!m_RenderWorldValid) RefreshRenderWorld();
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
                                       bool useMainCameraFrustum)
{
    // 2D/3D 统一走 3D 管线：z-prepass 无条件提交（2D 场景无模型实体 → 空提交，无害）
    const glm::mat4 projView = proj * view;
    if (!m_RenderWorldValid) RefreshRenderWorld();
    const RenderWorld& world = m_RenderWorld;
    // 与 useSubMeshCulling 开关绑定：开关关时回编辑器视锥（全景）
    const std::array<Plane, 6> zpreFrustumPlanes =
        (useMainCameraFrustum && m_HasMainCameraFrustum && m_MainCamUseSubMeshCulling) ? m_MainCameraFrustumPlanes
                                                                                       : AABBUtils::ExtractFrustumPlanes(projView);
    const bool zpreCullReady = m_OcclusionCulling.HasCullingContext();
    for (const auto& group : world.modelGroups) {
        if (group.entities.empty()) continue;
        auto it = m_ModelRenderers.find(group.rendererKey);
        if (it == m_ModelRenderers.end() || !it->second || !it->second->HasModelLoaded()) continue;
        std::vector<ModelInstanceData> instances;
        std::vector<size_t> zpreVisible;
        // Skinning changes the rendered bounds. The submesh BVH stores bind/raw
        // mesh bounds, so using it for a skinned model can drop a whole submesh.
        const bool zpreUseSubMeshCulling = zpreCullReady && !it->second->HasSkinning();
        instances.reserve(group.entities.size());
        for (const auto& entity : group.entities) {
            const RenderWorldEntity* entityData = world.Find(entity);
            if (entityData == nullptr || !entityData->hasTransform) continue;
            const glm::mat4 modelMatrix = entityData->transform.worldMatrix;
            ModelInstanceData id{};
            id.model = modelMatrix;
            id.prevModel = modelMatrix;   // z-prepass 不需要运动矢量
            instances.push_back(id);
            // subMesh 视锥剔除（与几何 pass 同逻辑；多实体并集，保守）
            if (zpreUseSubMeshCulling) {
                std::vector<size_t> vis = m_OcclusionCulling.GetVisibleSubMeshIndices(
                    it->second.get(), modelMatrix, zpreFrustumPlanes, {}, glm::vec3(0.0f), true, entity);
                zpreVisible.insert(zpreVisible.end(), vis.begin(), vis.end());
            }
        }
        if (instances.empty()) continue;
        if (zpreUseSubMeshCulling) {
            std::sort(zpreVisible.begin(), zpreVisible.end());
            zpreVisible.erase(std::unique(zpreVisible.begin(), zpreVisible.end()), zpreVisible.end());
            if (zpreVisible.empty()) continue;   // 剔除后无可见 subMesh
        }
        it->second->RenderDepthOnly(commandBuffer, width, height, projView, instances, zpreVisible);
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
    m_TerrainRenderer.Prepare(m_RenderWorld, terrainCameraPosition, zpreFrustumPlanes, true);
    m_TerrainRenderer.RenderDepthPrepass(commandBuffer, width, height, projView,
                                         terrainCameraPosition);
    m_WaterRenderer.Prepare(m_RenderWorld, terrainCameraPosition, zpreFrustumPlanes, true);
    m_WaterRenderer.RenderDepthPrepass(commandBuffer, width, height, projView,
                                       terrainCameraPosition);
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

void SceneRenderer::RenderECS(VkCommandBuffer commandBuffer, int width, int height, const glm::mat4& view, const glm::mat4& proj, const glm::mat4& cullView, const glm::mat4& cullProj, VulkanBuffer& uniformBuffer, VkDescriptorSet descriptorSet, SceneRenderer::ViewRenderMode mode)
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

// ===== 场景模式判定：仅启用 2D 相机且无主 3D 相机 → g_SceneIs2D=true =====
// 每帧无条件调用（VulkanManager::FrameRender 开头，任何渲染分支判断之前）。
// 历史 bug：此判定曾内联在 PrepareFrame（仅 3D 路径执行），2D 场景不经过它，
// 导致 2D→3D 场景切换后 g_SceneIs2D 卡在 true，3D 场景被错误地按 2D 分支渲染（无画面）。
void SceneRenderer::UpdateSceneMode() {
    if (!m_RenderWorldValid) RefreshRenderWorld();
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

void SceneRenderer::UpdateModelAnimations(float deltaTime)
{
    // EngineMain publishes the post-gameplay snapshot before this call.  Keep
    // the fallback for editor/tool callers that invoke animation sync alone.
    if (!m_RenderWorldValid) RefreshRenderWorld();

    std::unordered_set<std::string> vmdModelRendererKeys;
    for (const auto& entity : m_RenderWorld.entities) {
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
            ModelRenderer* r = GetModelRenderer(rendererKey);
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

    // 推进所有模型渲染器动画（骨骼采样 + 蒙皮矩阵更新）。
    for (auto& [path, renderer] : m_ModelRenderers) {
        (void)path;
        if (vmdModelRendererKeys.find(path) != vmdModelRendererKeys.end()) continue;
        if (renderer) renderer->UpdateAnimation(deltaTime);
    }
}

void SceneRenderer::PreloadModels()
{
    if (!m_RenderWorldValid) RefreshRenderWorld();
    const RenderWorld& world = m_RenderWorld;

    bool hasNewModels = false;

    for (const auto& group : world.modelGroups) {
        if (group.entities.empty()) continue;

        auto rendererIt = m_ModelRenderers.find(group.rendererKey);
        if (rendererIt == m_ModelRenderers.end()) {
            auto renderer = std::make_unique<ModelRenderer>();
            renderer->Init(m_RenderPass);
#ifdef __ANDROID__
            LOGI("[Android] Preload model: %s", group.modelPath.c_str());
#endif
            
            renderer->LoadModel(group.modelPath);
            bool loadSuccess = renderer->HasModelLoaded();
            
            if (loadSuccess) {
                hasNewModels = true;  // 标记有新模型加载
            }

            m_ModelRenderers[group.rendererKey] = std::move(renderer);
        }
        
        auto& renderer = m_ModelRenderers[group.rendererKey];
        
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

ModelRenderer* SceneRenderer::GetModelRenderer(const std::string& modelPath)
{
    auto it = m_ModelRenderers.find(modelPath);
    if (it != m_ModelRenderers.end()) {
        return it->second.get();
    }

    // Animated entities use a path#entity key, but callers that only need the
    // model geometry (camera collision, AABB/quad-tree/editor inspection) still
    // ask by asset path. Fall back to the first renderer loaded from that path.
    for (const auto& [key, renderer] : m_ModelRenderers) {
        (void)key;
        if (renderer && renderer->GetModelPath() == modelPath) return renderer.get();
    }
    return nullptr;
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
    if (!m_RenderWorldValid) RefreshRenderWorld();
    for (const auto& camera : m_RenderWorld.cameras) {
        if (camera.isMainCamera) {
            return camera.position;
        }
    }
    
    // 如果没有主摄像机，返回默认位置
    return glm::vec3(0.0f, 0.0f, 3.0f);
}
