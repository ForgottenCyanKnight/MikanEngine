#define GLM_ENABLE_EXPERIMENTAL

// Android 平台禁用光线追踪

#include <vulkan/vulkan.h>

#include "SceneRenderer.h"
#include "AABB.h"
#include "EngineGlobal.h"
#include "EngineConfig.h"
#include "Core/Log.h"
#include "ECS/ECS.h"
#include "ECS/SceneECS.h"
#include "ECS/Components.h"
#include <functional>


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
#include <algorithm>   // 2026-08-09 z-prepass 可见集并集 sort/unique
#include <cstring>
#include <cmath>
#include <memory>
#include <iostream>
#include <filesystem>
#include <glm/gtx/quaternion.hpp>

// 光线追踪扩展函数指针（直接使用Vulkan头文件中定义的函数，不再手动定义）

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
    
    // Hi-Z 生成与消费当前均被 VulkanManager 关闭（见 RenderGameToTarget/RenderGameComposite）。
    // 不创建未使用的计算管线：部分驱动在初始化该管线时会访问无效的扩展路径，
    // 导致无体素的 headless/原型运行在首帧前崩溃。恢复 Hi-Z 消费者时再显式开启初始化。
    m_EnableHiZCulling = false;
    std::cout << "[SceneRenderer] Hi-Z disabled (no active consumer)" << std::endl;
    
    // 初始化全屏四边形（从未被 Render 调用，保留以兼容；render pass 为三 subpass，shader 用 subpassInput → subpass 2 合成）
    // Android：非 MRT 单 subpass 下 subpass index 2 越界 → Adreno vkCreateGraphicsPipelines 崩；Android 走几何直通，不创建
#ifndef __ANDROID__
    m_FullscreenQuad.Init(renderPass, 2);
#endif

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



bool SceneRenderer::GetMainCameraMatrices(float aspectRatio, glm::mat4& outView, glm::mat4& outProj, glm::vec3& outCameraPos)
{
    auto& coordinator = ECS::Coordinator::GetInstance();
    const auto& cameraEntities = EnsureCameraEntitiesCached(); // 帧缓存,避免每帧多次全树收集
    
    for (const auto& entity : cameraEntities) {
        // 防御:缓存可能含已销毁或组件被移除的实体(重载场景等),跳过避免野指针访问
        if (!coordinator.HasComponent<ECS::CameraComponent>(entity) ||
            !coordinator.HasComponent<ECS::TransformComponent>(entity)) {
            continue;
        }
        auto& camera = coordinator.GetComponent<ECS::CameraComponent>(entity);
        if (camera.isMainCamera) {
            auto& transform = coordinator.GetComponent<ECS::TransformComponent>(entity);
            outCameraPos = transform.position;
            outView = camera.GetViewMatrix(transform.position, transform.rotation);
            outProj = camera.GetProjectionMatrix(aspectRatio);
            outProj[1][1] *= -1;
            return true;
        }
    }
    
    return false;
}

// 相机实体列表帧缓存:本帧未收集过(或实体集合已变化)则全树收集一次并标记,否则直接返回缓存
const std::vector<ECS::Entity>& SceneRenderer::EnsureCameraEntitiesCached()
{
    const uint32_t sceneVersion = ECS::SceneECS::GetInstance().GetEntitySetVersion();
    if (m_CameraCacheFrameId == m_FrameId && m_CameraCacheSceneVersion == sceneVersion) {
        return m_CameraEntitiesCache; // 本帧已收集且实体集合未变,命中缓存
    }

    m_CameraEntitiesCache.clear();
    auto& sceneECS = ECS::SceneECS::GetInstance();
    auto rootEntities = sceneECS.GetRootEntities();
    for (const auto& entity : rootEntities) {
        SceneCollector::CollectCameraEntities(entity, m_CameraEntitiesCache);
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
    auto& coordinator = ECS::Coordinator::GetInstance();
    auto& sceneECS = ECS::SceneECS::GetInstance();

    // ---- 模型（2026-08-09：per-entity subMesh 视锥剔除（BVH）后提交；仅视锥不做遮挡——深度保守，几何 pass 自写深度）----
    std::unordered_map<std::string, ModelInstanceGroup> modelGroups;
    for (const auto& entity : sceneECS.GetRootEntities()) {
        SceneCollector::CollectModelEntitiesByPath(entity, modelGroups);
    }
    // 2026-08-09：SceneView（useMainCameraFrustum）用主相机视锥剔除——与几何 pass 一致（否则编辑器俯瞰视锥全量绘制 → 几何剔除后灰色清屏）
    // 与 useSubMeshCulling 开关绑定：开关关时回编辑器视锥（全景）
    const std::array<Plane, 6> zpreFrustumPlanes =
        (useMainCameraFrustum && m_HasMainCameraFrustum && m_MainCamUseSubMeshCulling) ? m_MainCameraFrustumPlanes
                                                                                       : AABBUtils::ExtractFrustumPlanes(projView);
    const bool zpreCullReady = m_OcclusionCulling.HasCullingContext();
    for (auto& [modelPath, group] : modelGroups) {
        if (group.entities.empty()) continue;
        auto it = m_ModelRenderers.find(modelPath);
        if (it == m_ModelRenderers.end() || !it->second || !it->second->HasModelLoaded()) continue;
        std::vector<ModelInstanceData> instances;
        std::vector<size_t> zpreVisible;
        instances.reserve(group.entities.size());
        for (const auto& entity : group.entities) {
            if (!coordinator.HasComponent<ECS::TransformComponent>(entity)) continue;
            const glm::mat4 modelMatrix = sceneECS.GetWorldMatrix(entity);
            ModelInstanceData id{};
            id.model = modelMatrix;
            id.prevModel = modelMatrix;   // z-prepass 不需要运动矢量
            instances.push_back(id);
            // subMesh 视锥剔除（与几何 pass 同逻辑；多实体并集，保守）
            if (zpreCullReady) {
                std::vector<size_t> vis = m_OcclusionCulling.GetVisibleSubMeshIndices(
                    it->second.get(), modelMatrix, zpreFrustumPlanes, {}, glm::vec3(0.0f), true, entity);
                zpreVisible.insert(zpreVisible.end(), vis.begin(), vis.end());
            }
        }
        if (instances.empty()) continue;
        if (zpreCullReady) {
            std::sort(zpreVisible.begin(), zpreVisible.end());
            zpreVisible.erase(std::unique(zpreVisible.begin(), zpreVisible.end()), zpreVisible.end());
            if (zpreVisible.empty()) continue;   // 剔除后无可见 subMesh
        }
        it->second->RenderDepthOnly(commandBuffer, width, height, projView, instances, zpreVisible);
    }

    // ---- 体素（动态 + 无 MDI 的静态 fallback）----
    std::unordered_map<std::string, VoxInstanceGroup> voxGroups;
    for (const auto& entity : sceneECS.GetRootEntities()) {
        SceneCollector::CollectVoxModelEntitiesByPath(entity, voxGroups);
    }
    const glm::mat4 flipZ = glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, 1.0f, -1.0f));
    for (auto& [voxPath, voxGroup] : voxGroups) {
        if (voxGroup.entities.empty()) continue;
        auto it = m_VoxRenderers.find(voxPath);
        if (it == m_VoxRenderers.end() || !it->second || !it->second->HasLoaded()) continue;
        auto& voxRenderer = it->second;
        std::vector<VoxelInstanceData> staticInstances;
        std::vector<VoxelInstanceData> dynamicInstances;
        for (const auto& entity : voxGroup.entities) {
            if (!coordinator.HasComponent<ECS::TransformComponent>(entity)) continue;
            const glm::mat4 modelMatrix = sceneECS.GetWorldMatrix(entity);
            VoxelInstanceData vd{};
            vd.model = modelMatrix * flipZ;
            vd.prevModel = vd.model;
            vd.worldMinBounds = voxRenderer->GetMinBounds();
            vd.voxelSize = voxRenderer->GetVoxelSize();
            vd.albedoColor = glm::vec4(1.0f);
            vd.materialData = glm::vec4(0.0f, 0.75f, 1.0f, 0.0f);   // 2026-08-11 默认粗糙石头/木头（无金属/无自发光/无AO）
            bool isStatic = true;
            if (coordinator.HasComponent<ECS::VoxModelComponent>(entity)) {
                isStatic = coordinator.GetComponent<ECS::VoxModelComponent>(entity).isStatic;
            }
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
}

// ===== 点光源阴影（2026-08-13）：cubemap 数组逐光源×6 面渲染线性深度（dist/range）=====
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
    if (lightCount <= 0) return;
    PointShadowRenderer* ps = EnsurePointShadows();
    if (!ps || !ps->IsInitialized()) return;
    const int n = std::min(lightCount, PointShadowRenderer::MAX_SHADOW_LIGHTS);

    std::vector<glm::vec3> positions(n);
    std::vector<float> ranges(n);
    for (int i = 0; i < n; i++) { positions[i] = lights[i].position; ranges[i] = lights[i].range; }
    ps->UpdateMatrices(positions.data(), ranges.data(), n);

    // 几何收集（同 RenderDepthPrepass，无剔除）——预构建每模型实例一次，6 面循环复用
    auto& coordinator = ECS::Coordinator::GetInstance();
    auto& sceneECS = ECS::SceneECS::GetInstance();
    std::unordered_map<std::string, ModelInstanceGroup> modelGroups;
    for (const auto& entity : sceneECS.GetRootEntities()) {
        SceneCollector::CollectModelEntitiesByPath(entity, modelGroups);
    }
    std::vector<std::pair<ModelRenderer*, std::vector<ModelInstanceData>>> renderers;
    for (auto& [modelPath, group] : modelGroups) {
        if (group.entities.empty()) continue;
        auto it = m_ModelRenderers.find(modelPath);
        if (it == m_ModelRenderers.end() || !it->second || !it->second->HasModelLoaded()) continue;
        std::vector<ModelInstanceData> instances;
        instances.reserve(group.entities.size());
        for (const auto& entity : group.entities) {
            if (!coordinator.HasComponent<ECS::TransformComponent>(entity)) continue;
            ModelInstanceData id{};
            id.model = sceneECS.GetWorldMatrix(entity);
            id.prevModel = id.model;
            instances.push_back(id);
        }
        if (instances.empty()) continue;
        it->second->EnsureShadowPipelines(ps->GetRenderPass());   // 阴影管线惰性创建（depth-only pass）
        renderers.emplace_back(it->second.get(), std::move(instances));
    }
    if (renderers.empty()) return;

    const int w = shadowMapSize, h = shadowMapSize;
    for (int l = 0; l < n; l++) {
        for (int face = 0; face < 6; face++) {
            ps->BeginFace(commandBuffer, l, face, w, h);
            const glm::mat4& faceProjView = ps->GetFaceProjView(l, face);
            auto facePlanes = AABBUtils::ExtractFrustumPlanes(faceProjView);
            for (auto& [renderer, instances] : renderers) {
                // 2026-08-13 剔除：模型级球体测试（距离 + 面视锥）先过滤无关模型；
                // 通过后：实例数 ≤4 → per-instance subMesh 级 BVH 剔除（大模型多 subMesh 场景——
                // 用户拍板；TLAS 树遍历剪枝，同主渲染）；实例数多 → 整批绘制（防 draw call 爆炸）
                bool anyVisible = false;
                const AABB modelAABB = renderer->GetAABB();
                std::vector<ModelInstanceData> visibleInstances;
                for (const auto& inst : instances) {
                    AABB worldAABB = modelAABB.Transform(inst.model);
                    // 距离剔除冗余：视锥 far 平面 = 光源 range（PointShadowRenderer::UpdateMatrices
                    // farP=lightRanges[l]）——range 外模型天然被 IsAABBInFrustum 剔除（AABB 相交测试
                    // 比球心距离更精确，且无需半径膨胀）
                    if (!AABBUtils::IsAABBInFrustum(worldAABB, facePlanes)) continue;
                    anyVisible = true;
                    visibleInstances.push_back(inst);
                }
                if (!anyVisible) continue;

                if (visibleInstances.size() <= 4) {
                    // per-instance subMesh 级 BVH 剔除（GetVisibleSubMeshIndices：TLAS 剪枝 + 线性回退）
                    for (const auto& inst : visibleInstances) {
                        std::vector<size_t> vis = m_OcclusionCulling.GetVisibleSubMeshIndices(
                            renderer, inst.model, facePlanes, {}, positions[l], true, ECS::Entity());
                        if (vis.empty()) continue;
                        renderer->RenderShadowDepth(commandBuffer, w, h, faceProjView,
                                                    positions[l], ranges[l], {inst}, vis);
                    }
                } else {
                    renderer->RenderShadowDepth(commandBuffer, w, h, faceProjView,
                                                positions[l], ranges[l], visibleInstances);
                }
            }
            ps->EndFace(commandBuffer);
        }
    }
    ps->Finalize(commandBuffer);   // cube array → SHADER_READ_ONLY（合成 pass 采样）
}

// ===== CSM 方向光阴影（2026-08-14）：级联 depth-only（默认 NDC 深度），参考 LimitlessSquare 组织方式 =====
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
    CascadeShadowRenderer* csm = EnsureCascadeShadows();
    if (!csm || !csm->IsInitialized()) return;

    // ⚠️ 2026-08-15 阴影缓存（脏检测跳过重渲）：大场景（Sponza/Bistro）CSM 4 级联几何 ×4 是帧率大头——
    // 相机/光源/场景几何不变时 shadowmap 内容不变 → 跳过渲染（GPU 用上帧内容，layout 保持 SHADER_READ_ONLY）。
    // 判据：① 级联矩阵全同（含相机/光源/snap 变化）② 场景哈希同（worldMatrix 位级 FNV）③ 无蒙皮/动画模型（骨骼姿势不在哈希内）。
    struct CsmShadowCache {
        glm::mat4 mats[CascadeShadowRenderer::MAX_CASCADES];
        uint64_t sceneHash = 0;
        bool valid = false;
    };
    static CsmShadowCache s_csmCache[CascadeShadowRenderer::MAX_SLOTS];
    static uint64_t s_frameCounter = 0;

    // 几何收集（同 RenderPointShadowMaps，模型分组）——无 caster 时零渲染；同时累计场景哈希（零额外遍历）
    auto& coordinator = ECS::Coordinator::GetInstance();
    auto& sceneECS = ECS::SceneECS::GetInstance();
    std::unordered_map<std::string, ModelInstanceGroup> modelGroups;
    uint64_t sceneHash = 1469598103934665603ull;   // FNV-1a offset basis
    bool hasSkinnedOrAnimated = false;
    for (const auto& entity : sceneECS.GetRootEntities()) {
        SceneCollector::CollectModelEntitiesByPath(entity, modelGroups);
    }
    std::vector<std::pair<ModelRenderer*, std::vector<ModelInstanceData>>> renderers;
    for (auto& [modelPath, group] : modelGroups) {
        if (group.entities.empty()) continue;
        auto it = m_ModelRenderers.find(modelPath);
        if (it == m_ModelRenderers.end() || !it->second || !it->second->HasModelLoaded()) continue;
        if (it->second->HasSkinning() || it->second->HasAnimation()) hasSkinnedOrAnimated = true;
        std::vector<ModelInstanceData> instances;
        instances.reserve(group.entities.size());
        for (const auto& entity : group.entities) {
            if (!coordinator.HasComponent<ECS::TransformComponent>(entity)) continue;
            ModelInstanceData id{};
            id.model = sceneECS.GetWorldMatrix(entity);
            id.prevModel = id.model;
            // 场景哈希（worldMatrix 位级 FNV-1a）
            const float* f = &id.model[0][0];
            for (int k = 0; k < 16; k++) {
                uint32_t u; std::memcpy(&u, &f[k], sizeof(u));
                sceneHash = (sceneHash ^ u) * 1099511628211ull;
            }
            instances.push_back(id);
        }
        if (instances.empty()) continue;
        it->second->EnsureCsmPipelines(csm->GetRenderPass());   // CSM 深度管线惰性创建
        renderers.emplace_back(it->second.get(), std::move(instances));
    }
    // 蒙皮/动画场景：骨骼姿势不在哈希内 → 强制每帧重渲（帧计数搅入哈希）
    if (hasSkinnedOrAnimated) sceneHash ^= ++s_frameCounter * 0x9E3779B97F4A7C15ull;

    csm->UpdateCascades(slot, view, proj, lightDir);

    // 缓存命中 → 跳过渲染（shadowmap 内容 + SHADER_READ_ONLY layout 保持上帧状态）
    bool cacheHit = s_csmCache[slot].valid && s_csmCache[slot].sceneHash == sceneHash;
    if (cacheHit) {
        for (int c = 0; c < CascadeShadowRenderer::MAX_CASCADES; c++) {
            if (s_csmCache[slot].mats[c] != csm->GetShadowMatrix(slot, c)) { cacheHit = false; break; }
        }
    }
    if (cacheHit) {
        LOGD("[CSM] slot %d cache hit（阴影未变，跳过重渲）", slot);
        return;
    }
    s_csmCache[slot].valid = true;
    s_csmCache[slot].sceneHash = sceneHash;
    for (int c = 0; c < CascadeShadowRenderer::MAX_CASCADES; c++)
        s_csmCache[slot].mats[c] = csm->GetShadowMatrix(slot, c);

    csm->PrepareRender(commandBuffer, slot);   // 2026-08-14：SHADER_READ（上帧 Finalize 残留）→ DEPTH_ATTACHMENT + 同步先前采样读
    if (renderers.empty()) {
        csm->Finalize(commandBuffer, slot);   // 无几何也转布局（合成 descriptor 已绑 view，避免 layout 残留）
        return;
    }

    const int w = CascadeShadowRenderer::CASCADE_SIZE, h = CascadeShadowRenderer::CASCADE_SIZE;
    for (int c = 0; c < CascadeShadowRenderer::MAX_CASCADES; c++) {
        if (!csm->IsCascadeValid(slot, c)) continue;
        const glm::mat4& shadowMatrix = csm->GetShadowMatrix(slot, c);
        auto cascadePlanes = AABBUtils::ExtractFrustumPlanes(shadowMatrix);
        // 收集本级联可见实例（模型级 AABB 剔除；实例少时 subMesh 级 BVH 剔除）——一次 render pass 画全部
        struct VisBatch { ModelRenderer* renderer; std::vector<ModelInstanceData> instances; std::vector<size_t> vis; };
        std::vector<VisBatch> visBatches;
        for (auto& [renderer, instances] : renderers) {
            const AABB modelAABB = renderer->GetAABB();
            std::vector<ModelInstanceData> visibleInstances;
            for (const auto& inst : instances) {
                AABB worldAABB = modelAABB.Transform(inst.model);
                if (!AABBUtils::IsAABBInFrustum(worldAABB, cascadePlanes)) continue;
                visibleInstances.push_back(inst);
            }
            if (visibleInstances.empty()) continue;
            if (visibleInstances.size() <= 4) {
                // per-instance subMesh 级 BVH 剔除（TLAS 剪枝 + 线性回退）
                for (const auto& inst : visibleInstances) {
                    std::vector<size_t> vis = m_OcclusionCulling.GetVisibleSubMeshIndices(
                        renderer, inst.model, cascadePlanes, {}, glm::vec3(0.0f), true, ECS::Entity());
                    if (vis.empty()) continue;
                    visBatches.push_back({ renderer, {inst}, std::move(vis) });
                }
            } else {
                visBatches.push_back({ renderer, std::move(visibleInstances), {} });
            }
        }
        if (visBatches.empty()) continue;

        static bool s_csmLogged = false;   // 一次性诊断（用户偏好：进度类日志用 LOGD）
        if (!s_csmLogged) {
            int totalInst = 0;
            for (auto& b : visBatches) totalInst += (int)b.instances.size();
            LOGD("[CSM] cascade %d: %d batches / %d instances (slot %d)", c, (int)visBatches.size(), totalInst, slot);
            s_csmLogged = true;
        }

        csm->BeginCascade(commandBuffer, slot, c, w, h);
        for (auto& batch : visBatches) {
            batch.renderer->RenderCsmDepth(commandBuffer, w, h, shadowMatrix, batch.instances, batch.vis);
        }
        csm->EndCascade(commandBuffer);
    }
    csm->Finalize(commandBuffer, slot);   // 2D array → SHADER_READ_ONLY（合成 pass 采样）
}

void SceneRenderer::RenderGeometryOpaque(RenderFrameContext& ctx)
{    VkCommandBuffer commandBuffer = ctx.commandBuffer;
    int width = ctx.width, height = ctx.height;
    const glm::mat4& view = ctx.view;
    const glm::mat4& proj = ctx.proj;
    const glm::mat4& cullView = ctx.cullView;
    const glm::mat4& cullProj = ctx.cullProj;
    bool isSceneView = ctx.isSceneView;
    auto& modelGroups = ctx.modelGroups;
    auto& voxGroups = ctx.voxGroups;
    auto& allModelEntities = ctx.allModelEntities;
    auto& allVoxEntities = ctx.allVoxEntities;
    glm::vec3& cameraPos = ctx.cameraPos;
    glm::vec3& cullingCameraPos = ctx.cullingCameraPos;
    glm::mat4& viewProj = ctx.viewProj;
    std::array<Plane, 6>& frustumPlanes = ctx.frustumPlanes;
    bool& useFrustumCulling = ctx.useFrustumCulling;
    bool& useSceneCameraCulling = ctx.useSceneCameraCulling;
    bool& useSubMeshCulling = ctx.useSubMeshCulling;
    bool& cameraMoved = ctx.cameraMoved;
    bool& modelCountChanged = ctx.modelCountChanged;
    struct VulkanBuffer* uniformBuffer = ctx.uniformBuffer;
    VkDescriptorSet descriptorSet = ctx.descriptorSet;
    glm::mat4& projView = ctx.projView;
    glm::mat4& prevProjView = ctx.prevProjView;
    glm::mat4& effectiveCullView = ctx.effectiveCullView;
    glm::mat4& effectiveCullProj = ctx.effectiveCullProj;
    bool& useMainCameraCulling = ctx.useMainCameraCulling;
    std::array<Plane, 6>& mainCameraFrustumPlanes = ctx.mainCameraFrustumPlanes;
    auto& rootEntities = ctx.rootEntities;
    auto& coordinator = ECS::Coordinator::GetInstance();
    auto& sceneECS = ECS::SceneECS::GetInstance();
    // [diag] 模型绘制入口（运行中交换链重建后蒙皮模型消失排查；前几次打印对比启动 vs 重建）
    {
        static int s_drawDiag = 0;
        if (s_drawDiag < 4) {
            s_drawDiag++;
            printf("[SceneRenderer][diag] model draw: groups=%zu renderers=%zu",
                   modelGroups.size(), m_ModelRenderers.size());
            for (const auto& [p, g] : modelGroups) printf(" '%s'[%zu]", p.c_str(), g.entities.size());
            printf("\n");
        }
    }
    // 渲染模型
    for (auto& [modelPath, group] : modelGroups) {
        if (group.entities.empty()) continue;
        
        auto& coordinator = ECS::Coordinator::GetInstance();
        
        auto rendererIt = m_ModelRenderers.find(modelPath);
        if (rendererIt == m_ModelRenderers.end() || !rendererIt->second) {
            static bool s_warnedMissing = false;
            if (!s_warnedMissing) {
                s_warnedMissing = true;
                printf("[SceneRenderer][diag] model '%s' MISSING from m_ModelRenderers (%zu entries):",
                       modelPath.c_str(), m_ModelRenderers.size());
                for (const auto& [p, r] : m_ModelRenderers) printf(" '%s'", p.c_str());
                printf("\n");
            }
            continue; // 跳过未加载的模型
        }
        
        auto& renderer = rendererIt->second;
        
        // 检查模型是否有效
        if (!renderer->HasModelLoaded()) {
            static bool s_warnedNotLoaded = false;
            if (!s_warnedNotLoaded) {
                s_warnedNotLoaded = true;
                printf("[SceneRenderer][diag] model '%s' exists but HasModelLoaded()=false\n",
                       modelPath.c_str());
            }
            continue;
        }
        
        // 注: BLAS 级别视锥剔除暂未启用（实现保留在 git 历史中；viewProj 已在主流程统一计算）
        
        bool modelHasAlbedoTexture = renderer->HasAlbedoTexture();
        bool modelHasNormalTexture = renderer->HasNormalTexture();
        bool modelHasEmissiveTexture = renderer->HasEmissiveTexture();   // 2026-08-09
        bool modelHasRoughnessTexture = renderer->HasRoughnessTexture();   // 2026-08-09
        bool modelHasMetallicTexture = renderer->HasMetallicTexture();   // 2026-08-09
        // 2026-08-11 诊断完成（useMR 注入验证通过）——注释每帧打印
        // if (modelHasRoughnessTexture || modelHasMetallicTexture) {
        //     printf("[SceneRenderer][diag] useMR=1 model='%s' rough=%d metal=%d\n", modelPath.c_str(), modelHasRoughnessTexture ? 1 : 0, modelHasMetallicTexture ? 1 : 0);
        // }
        
        // 根据剔除粒度选择渲染方式
        // 2026-08-09：与 useSubMeshCulling 开关绑定（默认开；不再 SceneView 强制——用户拍板）
        const bool effectiveSubMeshCulling = useSubMeshCulling;
        if (effectiveSubMeshCulling) {
            // 逐submesh剔除：先用模型级AABB快速筛选，再对子网格精确剔除
            struct VisibleEntityData {
                ECS::Entity entity;
                glm::mat4 modelMatrix;
                std::vector<size_t> visibleSubMeshIndices;
                ModelInstanceData instanceData;
            };
            std::vector<VisibleEntityData> visibleEntities;
            
            for (size_t entityIdx = 0; entityIdx < group.entities.size(); ++entityIdx) {
                const auto& entity = group.entities[entityIdx];
                
                auto& transform = coordinator.GetComponent<ECS::TransformComponent>(entity);
                glm::mat4 modelMatrix = ECS::SceneECS::GetInstance().GetWorldMatrix(entity);
                
                // 第一步：主相机视锥剔除（快速筛选）
                if (useMainCameraCulling) {
                    AABB localAABB = renderer->GetAABB();
                    AABB worldAABB = localAABB.Transform(modelMatrix);
                    if (!AABBUtils::IsAABBInFrustum(worldAABB, mainCameraFrustumPlanes)) {
                        continue; // 模型不在主相机视锥体内，跳过所有子网格
                    }
                }
                
                // 第二步：场景相机视锥剔除（仅场景视图，编辑器相机视锥）
                if (useSceneCameraCulling) {
                    AABB localAABB = renderer->GetAABB();
                    AABB worldAABB = localAABB.Transform(modelMatrix);
                    if (!AABBUtils::IsAABBInFrustum(worldAABB, frustumPlanes)) {
                        continue; // 模型不在场景相机视锥体内，跳过所有子网格
                    }
                }
                
                // 第二步：对模型内的子网格进行精确剔除
                std::vector<size_t> visibleSubMeshIndices = m_OcclusionCulling.GetVisibleSubMeshIndices(
                    renderer.get(), modelMatrix, frustumPlanes, allModelEntities, cameraPos,
                    useFrustumCulling, entity);

                if (visibleSubMeshIndices.empty()) {
                    continue; // 没有可见的submesh
                }
                
                VisibleEntityData ved;
                ved.entity = entity;
                ved.modelMatrix = modelMatrix;
                ved.visibleSubMeshIndices = visibleSubMeshIndices;
                
                // 准备实例数据
                ved.instanceData.model = modelMatrix;
                // 获取上一帧的模型矩阵，如果没有则使用当前帧矩阵
                auto prevModelIt = m_PrevModelMatrices.find(entity);
                ved.instanceData.prevModel = (prevModelIt != m_PrevModelMatrices.end()) ? prevModelIt->second : modelMatrix;
                
                if (coordinator.HasComponent<ECS::MaterialComponent>(entity)) {
                    auto& material = coordinator.GetComponent<ECS::MaterialComponent>(entity);
                    ved.instanceData.albedoColor = glm::vec4(material.albedoColor, 1.0f);
                    // 2026-08：materialData.w = 自发光强度（原 useAlbedoTexture 移至 textureFlags.y）
                    ved.instanceData.materialData = glm::vec4(
                        material.metallic,
                        material.roughness,
                        material.ao,
                        material.emissiveIntensity
                    );
                    ved.instanceData.textureFlags = glm::vec4(
                        (material.useNormalTexture || modelHasNormalTexture) ? 1.0f : 0.0f,
                        (material.useAlbedoTexture && modelHasAlbedoTexture) ? 1.0f : 0.0f,
                        (material.useEmissiveTexture || modelHasEmissiveTexture) ? 1.0f : 0.0f,   // 2026-08-09
(modelHasRoughnessTexture || modelHasMetallicTexture) ? 1.0f : 0.0f   // 2026-08-09 w=useMR
                    );
                } else {
                    ved.instanceData.albedoColor = glm::vec4(1.0f);
                    ved.instanceData.materialData = glm::vec4(-1.0f, -1.0f, -1.0f, 0.0f); // 没有MaterialComponent：-1 标记 → model.frag 用 per-subMesh glTF factor（2026-08-11）
                    ved.instanceData.textureFlags = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f); // 没有MaterialComponent时不使用纹理
                }
                
                visibleEntities.push_back(std::move(ved));
            }
            
            // 收集所有出现过的可见 submesh 索引
            std::set<size_t> allVisibleSubMeshIndices;
            for (const auto& ved : visibleEntities) {
                for (size_t idx : ved.visibleSubMeshIndices) {
                    allVisibleSubMeshIndices.insert(idx);
                }
            }
            
            // 按可见 submesh 组织批次：每个可见 submesh 一批（含包含它的实体的实例）
            std::vector<SubMeshInstanceData> batches;
            batches.reserve(allVisibleSubMeshIndices.size());
            for (size_t subMeshIdx : allVisibleSubMeshIndices) {
                SubMeshInstanceData batch;
                batch.subMeshIndex = subMeshIdx;
                for (const auto& ved : visibleEntities) {
                    for (size_t idx : ved.visibleSubMeshIndices) {
                        if (idx == subMeshIdx) {
                            batch.instances.push_back(ved.instanceData);
                            break;
                        }
                    }
                }
                if (!batch.instances.empty()) {
                    batches.push_back(std::move(batch));
                }
            }
            
            if (!batches.empty()) {
                const ECS::MaterialComponent* materialPtr = nullptr;
                if (!visibleEntities.empty() && coordinator.HasComponent<ECS::MaterialComponent>(visibleEntities[0].entity)) {
                    materialPtr = &coordinator.GetComponent<ECS::MaterialComponent>(visibleEntities[0].entity);
                }
                
                // 管线选择（保持原有语义：任一可见实体为 wireframe/双面则整体用对应管线）
                bool hasDoubleSided = false;
                bool hasWireframe = false;
                for (const auto& ved : visibleEntities) {
                    if (coordinator.HasComponent<ECS::RenderComponent>(ved.entity)) {
                        auto& render = coordinator.GetComponent<ECS::RenderComponent>(ved.entity);
                        if (render.doubleSided) hasDoubleSided = true;
                        if (render.wireframe) hasWireframe = true;
                    }
                }
                
                // 一次调用渲染全部可见 submesh：内部按材质(描述符集)分组，共享管线/常量/实例缓冲
                renderer->RenderInstancedBatches(commandBuffer, width, height, projView, prevProjView,
                                                 cameraPos, batches, materialPtr, hasDoubleSided, hasWireframe);
            }
            
            // 保存当前帧的模型矩阵作为下一帧的上一帧矩阵
            for (const auto& ved : visibleEntities) {
                m_PrevModelMatrices[ved.entity] = ved.modelMatrix;
            }

        } else {
            // 逐模型剔除：常规 AABB 视锥剔除（2026-08-09：移除四叉树预筛——subMesh BVH 已足够快，四叉树仅留遮挡用途）
            std::vector<ModelInstanceData> instanceData;
            instanceData.reserve(group.entities.size());

            for (const auto& entity : group.entities) {
                auto& transform = coordinator.GetComponent<ECS::TransformComponent>(entity);
                
                // 计算模型的世界空间AABB
                AABB localAABB = renderer->GetAABB();
                glm::mat4 modelMatrix = ECS::SceneECS::GetInstance().GetWorldMatrix(entity);
                AABB worldAABB = localAABB.Transform(modelMatrix);
                
                // 第一步：主相机视锥剔除
                if (useMainCameraCulling) {
                    if (!AABBUtils::IsAABBInFrustum(worldAABB, mainCameraFrustumPlanes)) {
                        continue; // 模型不在主相机视锥体内，跳过
                    }
                }
                
                // 第二步：场景相机视锥剔除（常规 AABB，仅场景视图；2026-08-09 替换四叉树结果查找）
                if (useSceneCameraCulling) {
                    if (!AABBUtils::IsAABBInFrustum(worldAABB, frustumPlanes)) {
                        continue;
                    }
                }
                
                ModelInstanceData data;
                data.model = modelMatrix;
                // 获取上一帧的模型矩阵，如果没有则使用当前帧矩阵
                auto prevModelIt = m_PrevModelMatrices.find(entity);
                data.prevModel = (prevModelIt != m_PrevModelMatrices.end()) ? prevModelIt->second : modelMatrix;
                
                if (coordinator.HasComponent<ECS::MaterialComponent>(entity)) {
                    auto& material = coordinator.GetComponent<ECS::MaterialComponent>(entity);
                    data.albedoColor = glm::vec4(material.albedoColor, 1.0f);
                    // 2026-08：materialData.w = 自发光强度（原 useAlbedoTexture 移至 textureFlags.y）
                    data.materialData = glm::vec4(
                        material.metallic,
                        material.roughness,
                        material.ao,
                        material.emissiveIntensity
                    );
                    data.textureFlags = glm::vec4(
                        (material.useNormalTexture || modelHasNormalTexture) ? 1.0f : 0.0f,
                        (material.useAlbedoTexture && modelHasAlbedoTexture) ? 1.0f : 0.0f,
                        (material.useEmissiveTexture || modelHasEmissiveTexture) ? 1.0f : 0.0f,   // 2026-08-09
(modelHasRoughnessTexture || modelHasMetallicTexture) ? 1.0f : 0.0f   // 2026-08-09 w=useMR
                    );
                } else {
                    data.albedoColor = glm::vec4(1.0f);
                    data.materialData = glm::vec4(-1.0f, -1.0f, -1.0f, 0.0f); // 没有MaterialComponent：-1 标记 → model.frag 用 per-subMesh glTF factor（2026-08-11）
                    data.textureFlags = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f); // 没有MaterialComponent时不使用纹理
                }
                
                instanceData.push_back(data);
            }
            
            const ECS::MaterialComponent* materialPtr = nullptr;
            if (!group.entities.empty() && coordinator.HasComponent<ECS::MaterialComponent>(group.entities[0])) {
                materialPtr = &coordinator.GetComponent<ECS::MaterialComponent>(group.entities[0]);
            }
            
            // 检查是否需要双面渲染和线框渲染
            bool hasDoubleSided = false;
            bool hasWireframe = false;
            for (const auto& entity : group.entities) {
                if (coordinator.HasComponent<ECS::RenderComponent>(entity)) {
                    auto& render = coordinator.GetComponent<ECS::RenderComponent>(entity);
                    if (render.doubleSided) {
                        hasDoubleSided = true;
                    }
                    if (render.wireframe) {
                        hasWireframe = true;
                    }
                }
            }
            
            // 根据渲染模式选择不同的管线
            if (hasWireframe) {
                // 线框渲染
                renderer->RenderInstancedWireframe(commandBuffer, width, height, projView, prevProjView, cameraPos, instanceData, materialPtr, {});
            } else if (hasDoubleSided || renderer->HasDoubleSided()) {
                // 双面渲染（2026-08-17：ECS 开关 或 任一 subMesh 材质 doubleSided——glTF doubleSided 材质自动双面）
                renderer->RenderInstancedDoubleSided(commandBuffer, width, height, projView, prevProjView, cameraPos, instanceData, materialPtr, {});
            } else {
                // 单面渲染
                renderer->RenderInstanced(commandBuffer, width, height, projView, prevProjView, cameraPos, instanceData, materialPtr, {});
            }
            
            // 保存当前帧的模型矩阵作为下一帧的上一帧矩阵
            for (size_t i = 0; i < group.entities.size(); ++i) {
                const auto& entity = group.entities[i];
                if (i < instanceData.size()) {
                    m_PrevModelMatrices[entity] = instanceData[i].model;
                } else {
                    // 对于不可见的实体，也保存其当前矩阵
                    auto& transform = coordinator.GetComponent<ECS::TransformComponent>(entity);
                    m_PrevModelMatrices[entity] = ECS::SceneECS::GetInstance().GetWorldMatrix(entity);
                }
            }

        }
    }
    
    // 保存当前帧的 ProjView 矩阵作为下一帧的上一帧矩阵
    m_PrevProjViewMatrix = projView;
    m_HasPrevFrameMatrices = true;
    
    // 收集并渲染 AABB 线框（仅在场景视图中）
    if (isSceneView) {
        for (const auto& entity : rootEntities) {
            m_DebugRenderer.CollectAABBs(entity, modelGroups, m_ModelRenderers, m_VoxRenderers);
        }
        
        // 体素 AABB 可视化
        for (const auto& entity : rootEntities) {
            m_DebugRenderer.CollectVoxAABBs(entity, m_VoxRenderers);
        }
        
        // BVH 可视化
        for (const auto& entity : rootEntities) {
            m_DebugRenderer.CollectBVH(entity, cameraPos, effectiveCullView, effectiveCullProj, m_ModelRenderers, m_VoxRenderers);
        }
        
        // 渲染已移到链末 RenderOverlayLinework（UI overlay pass，不再写 G-Buffer）——2026-08-11 重做移植
    }
    
    // 渲染体素模型（在模型渲染之后）
    // 注意：不再每帧调用 Clear()，只在需要真正清空时才调用（如场景切换）
    // 现在使用 UpdateVoxelModel 每帧更新模型数据（通过脏标记优化）
    
    // 统计当前帧可见的静态体素数量
    size_t totalVisibleStaticVoxels = 0;
    for (const auto& [voxPath, voxGroup] : voxGroups) {
        for (const auto& entity : voxGroup.entities) {
            bool isWireframe = false;
            bool isStatic = true;
            if (coordinator.HasComponent<ECS::RenderComponent>(entity)) {
                auto& render = coordinator.GetComponent<ECS::RenderComponent>(entity);
                isWireframe = render.wireframe;
            }
            if (coordinator.HasComponent<ECS::VoxModelComponent>(entity)) {
                auto& vox = coordinator.GetComponent<ECS::VoxModelComponent>(entity);
                isStatic = vox.isStatic;
            }
            if (!isWireframe && isStatic) {
                totalVisibleStaticVoxels++;
            }
        }
    }
    
    // 如果 MDI 中的模型数量与当前帧不匹配，需要清空重建
    if (m_VoxelMeshMultiDrawIndirect && m_VoxelMeshMultiDrawIndirect->GetTotalInstances() != totalVisibleStaticVoxels) {
        m_VoxelMeshMultiDrawIndirect->Clear();
    }
    
    for (auto& [voxPath, voxGroup] : voxGroups) {
        auto voxRendererIt = m_VoxRenderers.find(voxPath);
        if (voxRendererIt == m_VoxRenderers.end()) {
            // 创建新的 VoxRenderer
            auto renderer = std::make_unique<VoxRenderer>();
            renderer->Init(m_RenderPass);
            
            printf("SceneRenderer: Attempting to load Vox file: %s", voxPath.c_str());
            if (renderer->LoadVoxFile(voxPath)) {
                m_VoxRenderers[voxPath] = std::move(renderer);
                printf("SceneRenderer: Successfully loaded Vox file: %s", voxPath.c_str());
                // 标记该组所有实体为已加载
                for (const auto& entity : voxGroup.entities) {
                    if (coordinator.HasComponent<ECS::VoxModelComponent>(entity)) {
                        auto& voxComp = coordinator.GetComponent<ECS::VoxModelComponent>(entity);
                        voxComp.loaded = true;
                    }
                }
            } else {
                printf("SceneRenderer: Failed to load Vox file: %s", voxPath.c_str());
                continue; // 加载失败，跳过
            }
        }
        
        voxRendererIt = m_VoxRenderers.find(voxPath);
        if (voxRendererIt != m_VoxRenderers.end() && voxRendererIt->second && voxRendererIt->second->HasLoaded()) {
            auto& voxRenderer = voxRendererIt->second;
            
            std::vector<VoxelInstanceData> staticInstances;
            std::vector<VoxelInstanceData> dynamicInstances;
            std::vector<VoxelInstanceData> wireframeInstances;
            
            // 获取体素的局部 AABB
            glm::vec3 minBounds = voxRenderer->GetMinBounds();
            glm::vec3 maxBounds = voxRenderer->GetMaxBounds();
            AABB localAABB;
            localAABB.min = minBounds;
            localAABB.max = maxBounds;
            
            // 收集所有通过主相机（游戏相机）视锥剔除的体素
            for (const auto& entity : voxGroup.entities) {
                auto& transform = coordinator.GetComponent<ECS::TransformComponent>(entity);
                glm::mat4 modelMatrix = ECS::SceneECS::GetInstance().GetWorldMatrix(entity);
                
                // 主相机视锥剔除（只使用游戏相机）
                if (useMainCameraCulling) {
                    AABB worldAABB = localAABB.Transform(modelMatrix);
                    if (!AABBUtils::IsAABBInFrustum(worldAABB, mainCameraFrustumPlanes)) {
                        continue; // 体素不在主相机视锥体内，跳过
                    }
                }
                
                VoxelInstanceData instanceData;
                
                // 获取原始模型矩阵并翻转 Z 轴（左右手坐标系转换）
                glm::mat4 flipZ = glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, 1.0f, -1.0f));
                instanceData.model = modelMatrix * flipZ;
                
                auto prevModelIt = m_PrevModelMatrices.find(entity);
                instanceData.prevModel = (prevModelIt != m_PrevModelMatrices.end()) ? prevModelIt->second : instanceData.model;
                
                // 设置体素数据
                instanceData.worldMinBounds = voxRenderer->GetMinBounds();
                instanceData.voxelSize = voxRenderer->GetVoxelSize();
                
                if (coordinator.HasComponent<ECS::MaterialComponent>(entity)) {
                    auto& material = coordinator.GetComponent<ECS::MaterialComponent>(entity);
                    instanceData.albedoColor = glm::vec4(material.albedoColor, 1.0f);
                    // 2026-08：w = 自发光强度
                    instanceData.materialData = glm::vec4(material.metallic, material.roughness, material.ao, material.emissiveIntensity);
                } else {
                    instanceData.albedoColor = glm::vec4(1.0f);
                    instanceData.materialData = glm::vec4(0.0f, 0.75f, 1.0f, 0.0f);   // 2026-08-11 默认粗糙石头/木头
                }
                bool isWireframe = false;
                bool isStatic = true;
                if (coordinator.HasComponent<ECS::RenderComponent>(entity)) {
                    auto& render = coordinator.GetComponent<ECS::RenderComponent>(entity);
                    isWireframe = render.wireframe;
                }
                if (coordinator.HasComponent<ECS::VoxModelComponent>(entity)) {
                    auto& vox = coordinator.GetComponent<ECS::VoxModelComponent>(entity);
                    isStatic = vox.isStatic;
                }
                
                if (isWireframe) {
                    wireframeInstances.push_back(instanceData);
                } else if (isStatic) {
                    staticInstances.push_back(instanceData);
                } else {
                    dynamicInstances.push_back(instanceData);
                }
                
                m_PrevModelMatrices[entity] = instanceData.model;
            }
            
            // 使用 MDI 渲染静态体素
            if (!staticInstances.empty() && m_VoxelMeshMultiDrawIndirect) {
                // 将每个静态体素模型添加到 MDI 渲染器（使用 UpdateVoxelModel 每帧更新）
                // 注意：staticInstances 中的模型都是可见的（通过视锥剔除）
                // 需要为每个 staticInstance 找到对应的 entity
                size_t staticInstIdx = 0;
                for (const auto& entity : voxGroup.entities) {
                    if (staticInstIdx >= staticInstances.size()) break;
                    
                    // 检查该实体是否是静态的且在视锥体内
                    auto& transform = coordinator.GetComponent<ECS::TransformComponent>(entity);
                    glm::mat4 modelMatrix = ECS::SceneECS::GetInstance().GetWorldMatrix(entity);
                    
                    // 主相机视锥剔除（只使用游戏相机）
                    if (useMainCameraCulling) {
                        AABB worldAABB = localAABB.Transform(modelMatrix);
                        if (!AABBUtils::IsAABBInFrustum(worldAABB, mainCameraFrustumPlanes)) {
                            continue; // 体素不在主相机视锥体内，跳过
                        }
                    }
                    
                    bool isWireframe = false;
                    bool isStatic = true;
                    bool isVisible = true;
                    if (coordinator.HasComponent<ECS::RenderComponent>(entity)) {
                        auto& render = coordinator.GetComponent<ECS::RenderComponent>(entity);
                        isWireframe = render.wireframe;
                        isVisible = render.visible;
                    }
                    if (coordinator.HasComponent<ECS::VoxModelComponent>(entity)) {
                        auto& vox = coordinator.GetComponent<ECS::VoxModelComponent>(entity);
                        isStatic = vox.isStatic;
                    }
                    
                    if (!isWireframe && isStatic) {
                        const auto& instanceData = staticInstances[staticInstIdx];
                        m_VoxelMeshMultiDrawIndirect->UpdateVoxelModel((void*)entity, voxPath, voxRenderer.get(), instanceData.model, instanceData.albedoColor, true);
                        staticInstIdx++;
                    }
                }
            } else if (!staticInstances.empty()) {
                // 使用带背面剔除的渲染方式（只绘制可见面）
                voxRenderer->RenderMeshWithBackfaceCulling(commandBuffer, width, height, projView, prevProjView, cameraPos, staticInstances, true);
            }
            
            // 动态体素使用实例化面渲染
            if (!dynamicInstances.empty()) {
                voxRenderer->RenderInstanced(commandBuffer, width, height, projView, prevProjView, cameraPos, dynamicInstances);
            }
            
            // 线框渲染
            if (!wireframeInstances.empty()) {
                voxRenderer->RenderMeshWireframe(commandBuffer, width, height, projView, prevProjView, cameraPos, wireframeInstances);
            }
        }
    }
    
    // 执行 MDI 渲染
    if (m_VoxelMeshMultiDrawIndirect) {
        // 计算游戏相机的 ProjView 矩阵用于双重剔除
        glm::mat4 cullProjView = cullProj * cullView;
        
        // 根据是否启用双重剔除来决定传递的参数
        bool useDualCulling = true;  // 启用双重剔除
        
        // 使用主相机位置进行背面剔除
        m_VoxelMeshMultiDrawIndirect->Render(commandBuffer, width, height, projView, prevProjView, 
                                            cullProjView, cullingCameraPos, useDualCulling, true);
    }
    
    // ---- 体素世界渲染（从 OpenGL 版迁移的无限世界）----
    if (m_WorldRenderEnabled && m_WorldRenderer && m_WorldRenderer->IsInitialized() && g_World != nullptr) {
        // 与 model 渲染一致：世界剔除基于"游戏相机（Main Camera）"的视锥，
        // 这样 SceneView 预览的是游戏相机视野内的世界；无主相机时用渲染相机视锥兜底
        std::array<Plane, 6> worldFrustum = mainCameraFrustumPlanes;
        if (!useMainCameraCulling) {
            worldFrustum = AABBUtils::ExtractFrustumPlanes(proj * view, -10.0f);
        }
        m_WorldRenderer->RenderWorld(commandBuffer, width, height, view, proj, cameraPos, worldFrustum);
    }
}

void SceneRenderer::RenderECS(VkCommandBuffer commandBuffer, int width, int height, const glm::mat4& view, const glm::mat4& proj, const glm::mat4& cullView, const glm::mat4& cullProj, VulkanBuffer& uniformBuffer, VkDescriptorSet descriptorSet, SceneRenderer::ViewRenderMode mode)
{
    // 统一视图抽象：编辑器场景视图 = 编辑器相机 + 调试渲染；游戏视图 = 主相机
    const bool isSceneView = (mode == SceneRenderer::ViewRenderMode::EditorScene);

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

    // ===== Pass 2: 几何渲染（模型 + 静态/动态体素 + 无限体素世界）=====
    RenderGeometryOpaque(ctx);

    // ===== Pass 3: 调试叠加已移到链末 RenderOverlayLinework（UI overlay pass，不再写 G-Buffer）=====
}

// ===== 场景模式判定：仅启用 2D 相机且无主 3D 相机 → g_SceneIs2D=true =====
// 每帧无条件调用（VulkanManager::FrameRender 开头，任何渲染分支判断之前）。
// 历史 bug：此判定曾内联在 PrepareFrame（仅 3D 路径执行），2D 场景不经过它，
// 导致 2D→3D 场景切换后 g_SceneIs2D 卡在 true，3D 场景被错误地按 2D 分支渲染（无画面）。
void SceneRenderer::UpdateSceneMode() {
    auto& coordinator = ECS::Coordinator::GetInstance();
    auto& sceneECS = ECS::SceneECS::GetInstance();

    bool has3DCamera = false;
    const auto& cameraEntities = EnsureCameraEntitiesCached();
    for (const auto& ce : cameraEntities) {
        if (!coordinator.HasComponent<ECS::CameraComponent>(ce)) continue;
        auto& cam = coordinator.GetComponent<ECS::CameraComponent>(ce);
        if (cam.isMainCamera) { has3DCamera = true; break; }
    }

    bool has2DCamera = false;
    std::function<void(ECS::Entity)> visit2D = [&](ECS::Entity e) {
        if (has2DCamera) return;
        if (coordinator.HasComponent<ECS::Camera2DComponent>(e)) {
            if (coordinator.GetComponent<ECS::Camera2DComponent>(e).enabled) has2DCamera = true;
            return;
        }
        for (const auto& child : sceneECS.GetChildren(e)) visit2D(child);
    };
    for (const auto& root : sceneECS.GetRootEntities()) visit2D(root);
    g_SceneIs2D = has2DCamera && !has3DCamera;
}

void SceneRenderer::PrepareFrame(RenderFrameContext& ctx)
{
    // 纯 CPU 阶段：收集场景实体/相机/光源，计算剔除状态，构建四叉树，收集调试线。
    // 不向 commandBuffer 发出任何 GPU 命令。

    auto& sceneECS = ECS::SceneECS::GetInstance();
    auto& coordinator = ECS::Coordinator::GetInstance();
    auto& rootEntities = ctx.rootEntities;
    rootEntities = sceneECS.GetRootEntities();

    ++m_FrameId; // 帧号递增,使相机实体缓存对本帧失效(下方重建)

    auto& modelGroups = ctx.modelGroups;
    for (const auto& entity : rootEntities) {
        SceneCollector::CollectModelEntitiesByPath(entity, modelGroups);
    }

    auto& voxGroups = ctx.voxGroups;
    for (const auto& entity : rootEntities) {
        SceneCollector::CollectVoxModelEntitiesByPath(entity, voxGroups);
    }

    // 清除之前的 AABB 实例和视锥体
    m_DebugRenderer.ClearInstances();
    m_DebugRenderer.ClearFrustums();

    // 收集摄像机实体(帧缓存,与 GetMainCameraMatrices/GetCameraPosition 共用,避免每帧多次全树收集)
    m_CameraEntitiesCache.clear();
    for (const auto& entity : rootEntities) {
        SceneCollector::CollectCameraEntities(entity, m_CameraEntitiesCache);
    }
    m_CameraCacheFrameId = m_FrameId;
    m_CameraCacheSceneVersion = ECS::SceneECS::GetInstance().GetEntitySetVersion();
    const auto& cameraEntities = m_CameraEntitiesCache;

    // 场景视图始终使用编辑器摄像机进行视锥剔除（传入的 cullView/cullProj）；
    // 游戏视图使用主摄像机的设置
    glm::mat4& effectiveCullView = ctx.effectiveCullView;
    effectiveCullView = ctx.cullView;
    glm::mat4& effectiveCullProj = ctx.effectiveCullProj;
    effectiveCullProj = ctx.cullProj;
    bool hasCullingCamera = ctx.isSceneView;  // 场景视图始终启用剔除

    // 主相机视锥平面
    std::array<Plane, 6>& mainCameraFrustumPlanes = ctx.mainCameraFrustumPlanes;
    bool& useMainCameraCulling = ctx.useMainCameraCulling;

    for (const auto& cameraEntity : cameraEntities) {
        auto& camera = coordinator.GetComponent<ECS::CameraComponent>(cameraEntity);
        if (!camera.isMainCamera) continue;

        // 计算主相机的视锥平面
        auto& transform = coordinator.GetComponent<ECS::TransformComponent>(cameraEntity);
        glm::mat4 camView = camera.GetViewMatrix(transform.position, transform.rotation);
        float aspectRatio = (float)ctx.width / (float)ctx.height;
        glm::mat4 camProj = camera.GetProjectionMatrix(aspectRatio);
        camProj[1][1] *= -1; // Vulkan的Y轴翻转

        if (camera.enableFrustumCulling) {
            glm::mat4 mainViewProj = camProj * camView;
            mainCameraFrustumPlanes = AABBUtils::ExtractFrustumPlanes(mainViewProj);
            useMainCameraCulling = true;
            // 2026-08-09：缓存供 z-prepass（SceneView 剔除用，与几何一致）
            m_MainCameraFrustumPlanes = mainCameraFrustumPlanes;
            m_HasMainCameraFrustum = true;
        }
        break;
    }

    // 处理视锥体线框和剔除矩阵
    for (const auto& cameraEntity : cameraEntities) {
        auto& camera = coordinator.GetComponent<ECS::CameraComponent>(cameraEntity);
        auto& transform = coordinator.GetComponent<ECS::TransformComponent>(cameraEntity);

        // 计算摄像机的视图投影矩阵
        glm::mat4 camView = camera.GetViewMatrix(transform.position, transform.rotation);
        float aspectRatio = (float)ctx.width / (float)ctx.height;
        glm::mat4 camProj = camera.GetProjectionMatrix(aspectRatio);
        camProj[1][1] *= -1; // Vulkan的Y轴翻转

        // 游戏视图：使用主摄像机的剔除设置
        if (!ctx.isSceneView && camera.isMainCamera && camera.enableFrustumCulling) {
            effectiveCullView = camView;
            effectiveCullProj = camProj;
            hasCullingCamera = true;
        }

        // 只在Scene View中渲染视锥体线框
        if (ctx.isSceneView && camera.showFrustumWireframe) {
            glm::mat4 camViewProj = camProj * camView;
            Frustum frustum = Frustum::FromViewProj(camViewProj);
            m_DebugRenderer.GetWireframeRenderer().AddFrustum(frustum, glm::vec3(1.0f, 1.0f, 0.0f));
        }
    }

    // ===== 场景模式判定:仅启用 2D 相机且无主 3D 相机 → 2D 游戏 =====
    // 驱动渲染侧(游戏/场景视图跳过 3D)与编辑器形态(隐藏 3D 网格/gizmo)。
    // 每帧无条件调用（见主循环 FrameRender 开头），不得只放在 3D 渲染路径内。
    UpdateSceneMode();

    auto& lightEntities = ctx.lightEntities;
    for (const auto& entity : rootEntities) {
        SceneCollector::CollectLightEntities(entity, lightEntities);
    }

    // 更新场景光源数量
    m_LightCount = static_cast<int>(lightEntities.size());

    glm::vec3& lightDir = ctx.lightDir;
    float& lightIntensity = ctx.lightIntensity;
    glm::vec3& lightColor = ctx.lightColor;

    if (!lightEntities.empty()) {
        auto& lightEntity = lightEntities[0];
        auto& light = coordinator.GetComponent<ECS::LightComponent>(lightEntity);
        auto& transform = coordinator.GetComponent<ECS::TransformComponent>(lightEntity);

        lightColor = light.color;
        lightIntensity = light.intensity;

        if (light.type == ECS::LightComponent::Type::Directional) {
            glm::mat4 rotMat = glm::mat4_cast(transform.rotation);
            lightDir = glm::normalize(glm::vec3(rotMat * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
        } else if (light.type == ECS::LightComponent::Type::Point) {
            lightDir = glm::normalize(glm::vec3(0.0f) - transform.position);
        }
    }

    // 计算视锥体平面（使用剔除用的视图投影矩阵）
    ctx.viewProj = effectiveCullProj * effectiveCullView;
    ctx.useFrustumCulling = hasCullingCamera;
    // 模型级场景相机二次剔除：仅场景视图需要（编辑器相机视锥，省编辑器性能）。
    // 游戏视图渲染相机=主相机，模型级剔除已由 mainCameraFrustumPlanes 覆盖，无需重复。
    ctx.useSceneCameraCulling = ctx.isSceneView && hasCullingCamera;
    if (hasCullingCamera) {
        ctx.frustumPlanes = AABBUtils::ExtractFrustumPlanes(ctx.viewProj);
    }

    // 收集所有模型实体用于遮挡检测
    auto& allModelEntities = ctx.allModelEntities;
    for (const auto& [modelPath, group] : modelGroups) {
        allModelEntities.insert(allModelEntities.end(), group.entities.begin(), group.entities.end());
    }

    // 收集所有vox实体用于四叉树管理
    auto& allVoxEntities = ctx.allVoxEntities;
    for (const auto& [voxPath, group] : voxGroups) {
        allVoxEntities.insert(allVoxEntities.end(), group.entities.begin(), group.entities.end());
    }

    // 合并模型和vox实体用于四叉树构建
    auto& allEntitiesForQuadTree = ctx.allEntitiesForQuadTree;
    allEntitiesForQuadTree = allModelEntities;
    allEntitiesForQuadTree.insert(allEntitiesForQuadTree.end(), allVoxEntities.begin(), allVoxEntities.end());

    // 获取相机位置
    auto& cameraPos = ctx.cameraPos;
    auto& cullingCameraPos = ctx.cullingCameraPos;
    if (ctx.isSceneView) {
        // 从视图矩阵中提取场景相机的位置（第 3 行存储的是负的相机位置，需要取反）
        cameraPos = -glm::vec3(ctx.view[0][3], ctx.view[1][3], ctx.view[2][3]);
        // 背面剔除使用主相机的位置
        cullingCameraPos = GetCameraPosition();
    } else {
        // 使用主相机的位置
        cameraPos = GetCameraPosition();
        cullingCameraPos = cameraPos;
    }

    // 检查是否启用剔除粒度（使用主摄像机的设置）
    bool& useSubMeshCulling = ctx.useSubMeshCulling;
    for (const auto& cameraEntity : cameraEntities) {
        auto& camera = coordinator.GetComponent<ECS::CameraComponent>(cameraEntity);
        if (camera.isMainCamera) {
            useSubMeshCulling = camera.useSubMeshCulling;
            break;
        }
    }

    // 2026-08-09：SceneView 主相机剔除效果预览——subMesh 剔除视锥覆盖为主相机视锥（与 useSubMeshCulling 开关绑定；关闭则回编辑器视锥=全景）
    if (ctx.isSceneView && useMainCameraCulling && useSubMeshCulling) {
        ctx.frustumPlanes = mainCameraFrustumPlanes;
    }
    m_MainCamUseSubMeshCulling = useSubMeshCulling;   // 2026-08-09：z-prepass 同步（开关关时回编辑器视锥）

    // 检测摄像机是否移动超过阈值
    float cameraMoveDistance = glm::distance(cameraPos, m_LastCameraPos);
    bool& cameraMoved = ctx.cameraMoved;
    cameraMoved = (m_LastCameraPos.x == FLT_MAX) || (cameraMoveDistance > m_CameraMoveThreshold);

    // 检测模型数量是否变化（只在Scene View中检测和更新）
    bool& modelCountChanged = ctx.modelCountChanged;
    if (ctx.isSceneView) {
        modelCountChanged = (allModelEntities.size() != m_LastModelCount);
        m_LastModelCount = allModelEntities.size();
    }

    // 预加载所有需要的模型（在渲染前完成）
    for (auto& [modelPath, group] : modelGroups) {
        if (group.entities.empty()) continue;

        auto rendererIt = m_ModelRenderers.find(modelPath);
        if (rendererIt == m_ModelRenderers.end()) {
            auto renderer = std::make_unique<ModelRenderer>();
            renderer->Init(m_RenderPass);
            renderer->LoadModel(modelPath);
            // Only register the renderer if the model actually loaded; otherwise
            // retry on the next frame instead of silently drawing nothing forever.
            if (renderer->HasModelLoaded()) {
                m_ModelRenderers[modelPath] = std::move(renderer);
            }
        }
    }

    // 构建四叉树加速结构（2026-08-09：仅调试显示时构建——视锥剔除已回归常规 AABB + subMesh BVH，CPU 遮挡已清理）
    if (m_ShowQuadTree) {
        m_CullingContext.BuildQuadTreeAroundCamera(allEntitiesForQuadTree, cameraPos, this);
    }
    m_OcclusionCulling.SetCullingContext(&m_CullingContext);   // 始终接线（z-prepass/GetVisibleSubMeshIndices 只视锥不碰四叉树）

    m_LastCameraPos = cameraPos;
    m_QuadTreeDirty = false;

    // 世界网格 + 原点坐标轴(场景视图,Godot 风格:三级固定 LOD 网格,近细中疏远更疏且逐级变暗,
    // 范围逐级放大到视野外(±1000 视距)≈ 无限延伸;无随距离缩放产生的跳变。2D 游戏不显示)
    if (ctx.isSceneView && g_ShowGrid && !g_SceneIs2D) {
        auto& wf = m_DebugRenderer.GetWireframeRenderer();
        const int halfLines = 20; // 每方向 20 条线(线数恒定,密度由步长决定)
        struct GridBand { float unit; float halfSize; glm::vec3 color; };
        const GridBand bands[] = {
            {   1.0f,    20.0f, glm::vec3(0.42f, 0.42f, 0.42f) }, // 近: 1 米格
            {  10.0f,   200.0f, glm::vec3(0.34f, 0.34f, 0.34f) }, // 中: 10 米格
            { 100.0f,  1000.0f, glm::vec3(0.26f, 0.26f, 0.26f) }, // 远: 100 米格(延伸到视野外)
        };
        for (const auto& band : bands) {
            for (int i = -halfLines; i <= halfLines; ++i) {
                if (i == 0) continue; // 中心十字单独画(更亮更粗)
                const float c = band.unit * (float)i;
                wf.AddLine(glm::vec3(c, 0.0f, -band.halfSize), glm::vec3(c, 0.0f, band.halfSize), band.color);   // 平行 Z 轴
                wf.AddLine(glm::vec3(-band.halfSize, 0.0f, c), glm::vec3(band.halfSize, 0.0f, c), band.color);   // 平行 X 轴
            }
        }
        // 中心十字粗线(贯穿远带边缘,Godot 亮色主轴;size 为 halfExtents,实际长 ±1000)
        wf.AddCrossGrid(glm::vec3(0.0f), 1000.0f, 0.08f, glm::vec3(0.85f, 0.85f, 0.85f));

        // 原点三色坐标轴(Godot 风格:红=X右 绿=Y上 蓝=Z前,末端 V 形箭头;长度固定 3 米)
        const float axisLen = 3.0f;
        const float arrowLen = axisLen * 0.22f; // 箭头长度
        const float wing = axisLen * 0.14f;     // 箭头翼展
        // X 轴(红)
        wf.AddLine(glm::vec3(0.0f), glm::vec3(axisLen, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f));
        wf.AddLine(glm::vec3(axisLen, 0.0f, 0.0f), glm::vec3(axisLen - arrowLen, wing, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f));
        wf.AddLine(glm::vec3(axisLen, 0.0f, 0.0f), glm::vec3(axisLen - arrowLen, -wing, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f));
        // Y 轴(绿)
        wf.AddLine(glm::vec3(0.0f), glm::vec3(0.0f, axisLen, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        wf.AddLine(glm::vec3(0.0f, axisLen, 0.0f), glm::vec3(wing, axisLen - arrowLen, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        wf.AddLine(glm::vec3(0.0f, axisLen, 0.0f), glm::vec3(-wing, axisLen - arrowLen, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        // Z 轴(蓝)
        wf.AddLine(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, axisLen), glm::vec3(0.0f, 0.0f, 1.0f));
        wf.AddLine(glm::vec3(0.0f, 0.0f, axisLen), glm::vec3(0.0f, wing, axisLen - arrowLen), glm::vec3(0.0f, 0.0f, 1.0f));
        wf.AddLine(glm::vec3(0.0f, 0.0f, axisLen), glm::vec3(0.0f, -wing, axisLen - arrowLen), glm::vec3(0.0f, 0.0f, 1.0f));
    }

    // 四叉树可视化 - 绘制节点边界框（仅在 Scene View 中显示）
    if (ctx.isSceneView && m_ShowQuadTree) {
        std::vector<AABB> nodeBounds;
        m_CullingContext.GetQuadTree().CollectNodeBounds(nodeBounds, 10);

        // 使用摄像机高度作为网格高度
        float gridHeight = cameraPos.y;

        for (const auto& bounds : nodeBounds) {
            // 绘制 AABB 的边界框（在摄像机高度处的 4 条边）
            float minX = bounds.min.x;
            float maxX = bounds.max.x;
            float minZ = bounds.min.z;
            float maxZ = bounds.max.z;

            // 在摄像机高度处的 4 条边
            m_DebugRenderer.GetWireframeRenderer().AddLine(
                glm::vec3(minX, gridHeight, minZ),
                glm::vec3(maxX, gridHeight, minZ),
                glm::vec3(1.0f, 1.0f, 1.0f)
            );
            m_DebugRenderer.GetWireframeRenderer().AddLine(
                glm::vec3(maxX, gridHeight, minZ),
                glm::vec3(maxX, gridHeight, maxZ),
                glm::vec3(1.0f, 1.0f, 1.0f)
            );
            m_DebugRenderer.GetWireframeRenderer().AddLine(
                glm::vec3(maxX, gridHeight, maxZ),
                glm::vec3(minX, gridHeight, maxZ),
                glm::vec3(1.0f, 1.0f, 1.0f)
            );
            m_DebugRenderer.GetWireframeRenderer().AddLine(
                glm::vec3(minX, gridHeight, maxZ),
                glm::vec3(minX, gridHeight, minZ),
                glm::vec3(1.0f, 1.0f, 1.0f)
            );
        }
    }
}

void SceneRenderer::RenderOverlayLinework(VkCommandBuffer commandBuffer, int width, int height,
                                          VkRenderPass uiPass, const glm::mat4& view, const glm::mat4& proj)
{
    auto& wf = m_DebugRenderer.GetWireframeRenderer();
    wf.EnsureInit(uiPass);
    if (m_DebugRenderer.HasInstances()) {
        m_DebugRenderer.Render(commandBuffer, view, proj);
    }
    if (wf.HasFrustums()) {
        wf.RenderFrustums(commandBuffer, view, proj);
    }
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
    // 1) AnimatorComponent -> 对应 ModelRenderer 同步（按实体 mesh.modelPath）；
    //    time 回写组件（属性面板可查看当前动画时间/帧）
    auto& scene = ECS::SceneECS::GetInstance();
    auto& coordinator = ECS::Coordinator::GetInstance();
    std::function<void(ECS::Entity)> visit = [&](ECS::Entity e) {
        if (coordinator.HasComponent<ECS::AnimatorComponent>(e) &&
            coordinator.HasComponent<ECS::MeshComponent>(e)) {
            auto& anim = coordinator.GetComponent<ECS::AnimatorComponent>(e);
            auto& mesh = coordinator.GetComponent<ECS::MeshComponent>(e);
            if (!mesh.modelPath.empty()) {
                ModelRenderer* r = GetModelRenderer(mesh.modelPath);
                if (r && r->HasAnimation()) {
                    if (anim.clipIndex != r->GetCurrentClip()) {
                        r->PlayAnimation(anim.clipIndex, anim.loop);
                    } else {
                        r->SetAnimationLoop(anim.loop);
                    }
                    r->SetAnimationSpeed(anim.speed);
                    r->SetAnimationPlaying(anim.playing);
                    anim.time = r->GetAnimationTime(); // 回写当前播放时间（查看动画帧）
                }
            }
        }
        for (ECS::Entity c : scene.GetChildren(e)) visit(c);
    };
    for (ECS::Entity root : scene.GetRootEntities()) visit(root);

    // 2) 推进所有模型渲染器动画（骨骼采样 + 蒙皮矩阵更新）
    for (auto& [path, renderer] : m_ModelRenderers) {
        (void)path;
        if (renderer) renderer->UpdateAnimation(deltaTime);
    }
}

void SceneRenderer::PreloadModels()
{
    auto& sceneECS = ECS::SceneECS::GetInstance();
    auto& coordinator = ECS::Coordinator::GetInstance();
    auto rootEntities = sceneECS.GetRootEntities();
    
    std::unordered_map<std::string, ModelInstanceGroup> modelGroups;
    for (const auto& entity : rootEntities) {
        SceneCollector::CollectModelEntitiesByPath(entity, modelGroups);
    }
    
    bool hasNewModels = false;
    
    for (auto& [modelPath, group] : modelGroups) {
        if (group.entities.empty()) continue;
        
        auto rendererIt = m_ModelRenderers.find(modelPath);
        if (rendererIt == m_ModelRenderers.end()) {
            auto renderer = std::make_unique<ModelRenderer>();
            renderer->Init(m_RenderPass);
            
            renderer->LoadModel(modelPath);
            bool loadSuccess = renderer->HasModelLoaded();
            
            if (loadSuccess) {
                hasNewModels = true;  // 标记有新模型加载
            }

            m_ModelRenderers[modelPath] = std::move(renderer);
        }
        
        auto& renderer = m_ModelRenderers[modelPath];
        
        for (const auto& entity : group.entities) {
            if (coordinator.HasComponent<ECS::MaterialComponent>(entity)) {
                auto& material = coordinator.GetComponent<ECS::MaterialComponent>(entity);
                
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
    

}

ModelRenderer* SceneRenderer::GetModelRenderer(const std::string& modelPath)
{
    auto it = m_ModelRenderers.find(modelPath);
    if (it != m_ModelRenderers.end()) {
        return it->second.get();
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
    auto& coordinator = ECS::Coordinator::GetInstance();
    const auto& cameraEntities = EnsureCameraEntitiesCached(); // 帧缓存
    
    // 查找主摄像机
    for (const auto& cameraEntity : cameraEntities) {
        // 防御:跳过已销毁/缺组件的实体,避免野指针
        if (!coordinator.HasComponent<ECS::CameraComponent>(cameraEntity) ||
            !coordinator.HasComponent<ECS::TransformComponent>(cameraEntity)) {
            continue;
        }
        auto& camera = coordinator.GetComponent<ECS::CameraComponent>(cameraEntity);
        if (camera.isMainCamera) {
            auto& transform = coordinator.GetComponent<ECS::TransformComponent>(cameraEntity);
            return transform.position;
        }
    }
    
    // 如果没有主摄像机，返回默认位置
    return glm::vec3(0.0f, 0.0f, 3.0f);
}
