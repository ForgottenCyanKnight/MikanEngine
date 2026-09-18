#include "Rendering/SceneShadowPass.h"

#include "AABB.h"
#include "Core/Log.h"
#include "Rendering/ModelRendererInternals.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

void SceneShadowPass::RenderPointShadowMaps(
    SceneRenderer& sceneRenderer,
    VkCommandBuffer commandBuffer,
    const SceneRenderer::ShadowLight* lights,
    int lightCount,
    int shadowMapSize)
{
    if (lightCount <= 0) return;
    PointShadowRenderer* ps = sceneRenderer.EnsurePointShadows();
    if (!ps || !ps->IsInitialized()) return;
    const int n = std::min(lightCount, PointShadowRenderer::MAX_SHADOW_LIGHTS);

    std::vector<glm::vec3> positions(n);
    std::vector<float> ranges(n);
    for (int i = 0; i < n; i++) {
        positions[i] = lights[i].position;
        ranges[i] = lights[i].range;
    }
    ps->UpdateMatrices(positions.data(), ranges.data(), n);

    // 几何收集（同 RenderDepthPrepass，无剔除）——预构建每模型实例一次，6 面循环复用
    if (!sceneRenderer.m_RenderWorldValid) sceneRenderer.RefreshRenderWorld();
    const RenderWorld& world = sceneRenderer.m_RenderWorld;
    std::vector<std::pair<ModelRenderer*, std::vector<ModelInstanceData>>> renderers;
    const auto modelBatches = sceneRenderer.BuildModelBatches();
    for (const auto& group : modelBatches) {
        if (group.entities.empty()) continue;
        ModelRenderer* renderer = group.renderer;
        if (renderer == nullptr || !renderer->HasModelLoaded()) continue;
        std::vector<ModelInstanceData> instances;
        instances.reserve(group.entities.size());
        for (size_t entityIdx = 0; entityIdx < group.entities.size(); ++entityIdx) {
            const auto& entity = group.entities[entityIdx];
            const RenderWorldEntity* entityData = world.Find(entity);
            if (entityData == nullptr || !entityData->hasTransform) continue;
            ModelInstanceData id{};
            id.model = entityData->transform.worldMatrix;
            id.prevModel = id.model;
            if (entityIdx < group.animationRenderers.size()) {
                ModelRendererDetail::ApplySharedBonePalette(
                    id, group.animationRenderers[entityIdx]);
            }
            instances.push_back(id);
        }
        if (instances.empty()) continue;
        renderer->EnsureShadowPipelines(ps->GetRenderPass());   // 阴影管线惰性创建（depth-only pass）
        renderers.emplace_back(renderer, std::move(instances));
    }
    if (renderers.empty()) return;

    const int w = shadowMapSize, h = shadowMapSize;
    for (int l = 0; l < n; l++) {
        for (int face = 0; face < 6; face++) {
            ps->BeginFace(commandBuffer, l, face, w, h);
            const glm::mat4& faceProjView = ps->GetFaceProjView(l, face);
            auto facePlanes = AABBUtils::ExtractFrustumPlanes(faceProjView);
            for (auto& [modelRenderer, instances] : renderers) {
                // 通过后：实例数 ≤4 → per-instance subMesh 级 BVH 剔除（大模型多 subMesh 场景——
                // 用户拍板；TLAS 树遍历剪枝，同主渲染）；实例数多 → 整批绘制（防 draw call 爆炸）
                bool anyVisible = false;
                const AABB modelAABB = modelRenderer->GetAABB();
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

                // A skinned model's submesh bounds are not animation-aware;
                // render all its submeshes after the model-level test.
                if (visibleInstances.size() <= 4 && !modelRenderer->HasSkinning()) {
                    // per-instance subMesh 级 BVH 剔除（GetVisibleSubMeshIndices：TLAS 剪枝 + 线性回退）
                    for (const auto& inst : visibleInstances) {
                        std::vector<size_t> vis = sceneRenderer.m_OcclusionCulling.GetVisibleSubMeshIndices(
                            modelRenderer, inst.model, facePlanes, {}, positions[l], true, ECS::Entity());
                        if (vis.empty()) continue;
                        modelRenderer->RenderShadowDepth(commandBuffer, w, h, faceProjView,
                                                         positions[l], ranges[l], {inst}, vis);
                    }
                } else {
                    modelRenderer->RenderShadowDepth(commandBuffer, w, h, faceProjView,
                                                     positions[l], ranges[l], visibleInstances);
                }
            }
            ps->EndFace(commandBuffer);
        }
    }
    ps->Finalize(commandBuffer);   // cube array → SHADER_READ_ONLY（合成 pass 采样）
}

void SceneShadowPass::RenderCascadeShadowMaps(
    SceneRenderer& sceneRenderer,
    VkCommandBuffer commandBuffer,
    int slot,
    const glm::mat4& view,
    const glm::mat4& proj,
    const glm::vec3& lightDir)
{
    CascadeShadowRenderer* csm = sceneRenderer.EnsureCascadeShadows();
    if (!csm || !csm->IsInitialized()) return;

    // 相机/光源/场景几何不变时 shadowmap 内容不变 → 跳过渲染（GPU 用上帧内容，layout 保持 SHADER_READ_ONLY）。
    // 判据：① 级联矩阵全同（含相机/光源/snap 变化）② 场景哈希同（worldMatrix 位级 FNV）③ 无蒙皮/动画模型（骨骼姿势不在哈希内）。
    struct CsmShadowCache {
        glm::mat4 mats[CascadeShadowRenderer::MAX_CASCADES];
        uint64_t sceneHash = 0;
        bool valid = false;
    };
    static CsmShadowCache s_csmCache[CascadeShadowRenderer::MAX_SLOTS];
    static uint64_t s_frameCounter = 0;

    // 几何收集（同 RenderPointShadowMaps，模型分组）——同时累计场景哈希。
    if (!sceneRenderer.m_RenderWorldValid) sceneRenderer.RefreshRenderWorld();
    const RenderWorld& world = sceneRenderer.m_RenderWorld;
    uint64_t sceneHash = 1469598103934665603ull;   // FNV-1a offset basis
    bool hasSkinnedOrAnimated = false;
    std::vector<std::pair<ModelRenderer*, std::vector<ModelInstanceData>>> renderers;
    const auto modelBatches = sceneRenderer.BuildModelBatches();
    for (const auto& group : modelBatches) {
        if (group.entities.empty()) continue;
        ModelRenderer* renderer = group.renderer;
        if (renderer == nullptr || !renderer->HasModelLoaded()) continue;
        if (renderer->HasSkinning() || renderer->HasAnimation()) hasSkinnedOrAnimated = true;
        std::vector<ModelInstanceData> instances;
        instances.reserve(group.entities.size());
        for (size_t entityIdx = 0; entityIdx < group.entities.size(); ++entityIdx) {
            const auto& entity = group.entities[entityIdx];
            const RenderWorldEntity* entityData = world.Find(entity);
            if (entityData == nullptr || !entityData->hasTransform) continue;
            ModelInstanceData id{};
            id.model = entityData->transform.worldMatrix;
            id.prevModel = id.model;
            if (entityIdx < group.animationRenderers.size()) {
                ModelRendererDetail::ApplySharedBonePalette(
                    id, group.animationRenderers[entityIdx]);
            }
            // 场景哈希（worldMatrix 位级 FNV-1a）
            const float* f = &id.model[0][0];
            for (int k = 0; k < 16; k++) {
                uint32_t u;
                std::memcpy(&u, &f[k], sizeof(u));
                sceneHash = (sceneHash ^ u) * 1099511628211ull;
            }
            instances.push_back(id);
        }
        if (instances.empty()) continue;
        renderer->EnsureCsmPipelines(csm->GetRenderPass());   // CSM 深度管线惰性创建
        renderers.emplace_back(renderer, std::move(instances));
    }
    // 蒙皮/动画场景：骨骼姿势不在哈希内 → 强制每帧重渲（帧计数搅入哈希）
    if (hasSkinnedOrAnimated) sceneHash ^= ++s_frameCounter * 0x9E3779B97F4A7C15ull;

    csm->UpdateCascades(slot, view, proj, lightDir);

    // Terrain 是独立于 ModelRenderer 的 caster。CSM 在 RenderECS 之前执行，
    // 所以不能依赖主几何阶段稍后填充的 m_PreparedResources；这里先按相机距离
    // 准备一次无视锥裁剪的 terrain caster 集合，供四个级联共同使用。
    const glm::vec3 terrainCameraPosition = glm::vec3(glm::inverse(view)[3]);
    const std::array<Plane, 6> noTerrainFrustum{};
    sceneRenderer.m_TerrainRenderer.Prepare(sceneRenderer.m_RenderWorld, terrainCameraPosition,
                                             noTerrainFrustum, false);
    const bool hasTerrainCasters = sceneRenderer.m_TerrainRenderer.GetVisibleChunkCount() > 0;
    if (hasTerrainCasters) {
        sceneHash = (sceneHash ^ static_cast<uint64_t>(sceneRenderer.m_TerrainRenderer.GetTerrainCount())) * 1099511628211ull;
        sceneHash = (sceneHash ^ static_cast<uint64_t>(sceneRenderer.m_TerrainRenderer.GetVisibleChunkCount())) * 1099511628211ull;
        if (!sceneRenderer.m_TerrainRenderer.EnsureCsmDepthPipeline(csm->GetRenderPass())) {
            LOGW("[CSM] terrain caster pipeline unavailable; terrain will not cast CSM shadows");
        }
        // 草地 GPU 逐桶剔除（阶段一）：主 pass 草绘制改为 vkCmdDrawIndirect，
        // 剔除命令必须在本 pass（render pass 之外）录制完成。这里是每帧唯一
        // 同时持有当前视图矩阵又在所有 render pass 之外的钩子点，viewSlot 直接
        // 复用 CSM slot 语义（0=场景视图/移动端游戏、1=编辑器游戏视图）。
        // 必须放在缓存命中判断之前：阴影缓存命中时本函数会提前返回，
        // 但主 pass 草绘制每帧都需要本视图的剔除命令段。
        sceneRenderer.m_TerrainRenderer.RecordGrassGpuCull(commandBuffer, view, proj, slot);
    }

    // 缓存命中 → 跳过渲染（shadowmap 内容 + SHADER_READ_ONLY layout 保持上帧状态）
    bool cacheHit = s_csmCache[slot].valid && s_csmCache[slot].sceneHash == sceneHash;
    if (cacheHit) {
        for (int c = 0; c < CascadeShadowRenderer::MAX_CASCADES; c++) {
            if (s_csmCache[slot].mats[c] != csm->GetShadowMatrix(slot, c)) {
                cacheHit = false;
                break;
            }
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

    csm->PrepareRender(commandBuffer, slot);

    const int w = CascadeShadowRenderer::CASCADE_SIZE, h = CascadeShadowRenderer::CASCADE_SIZE;
    // 草影只画最近两个级联（用户拍板）：远处级联里草叶远小于阴影图纹素，
    // 画了只剩噪点还烧带宽。级联 0 = 最近。
    constexpr int kGrassCsmCascadeCount = 2;
    // 草影收集只取主相机视锥内的草：级联光视锥覆盖范围比主相机视锥宽。
    const std::array<Plane, 6> mainCameraFrustum = AABBUtils::ExtractFrustumPlanes(view * proj);
    for (int c = 0; c < CascadeShadowRenderer::MAX_CASCADES; c++) {
        if (!csm->IsCascadeValid(slot, c)) continue;
        const glm::mat4& shadowMatrix = csm->GetShadowMatrix(slot, c);
        auto cascadePlanes = AABBUtils::ExtractFrustumPlanes(shadowMatrix);
        // 收集本级联可见实例（模型级 AABB 剔除；实例少时 subMesh 级 BVH 剔除）——一次 render pass 画全部
        struct VisBatch {
            ModelRenderer* renderer;
            std::vector<ModelInstanceData> instances;
            std::vector<size_t> vis;
        };
        std::vector<VisBatch> visBatches;
        for (auto& [modelRenderer, instances] : renderers) {
            const AABB modelAABB = modelRenderer->GetAABB();
            std::vector<ModelInstanceData> visibleInstances;
            for (const auto& inst : instances) {
                AABB worldAABB = modelAABB.Transform(inst.model);
                if (!AABBUtils::IsAABBInFrustum(worldAABB, cascadePlanes)) continue;
                visibleInstances.push_back(inst);
            }
            if (visibleInstances.empty()) continue;
            // A skinned model's submesh bounds are not animation-aware;
            // render all its submeshes after the model-level test.
            if (visibleInstances.size() <= 4 && !modelRenderer->HasSkinning()) {
                // per-instance subMesh 级 BVH 剔除（TLAS 剪枝 + 线性回退）
                for (const auto& inst : visibleInstances) {
                    std::vector<size_t> vis = sceneRenderer.m_OcclusionCulling.GetVisibleSubMeshIndices(
                        modelRenderer, inst.model, cascadePlanes, {}, glm::vec3(0.0f), true, ECS::Entity());
                    if (vis.empty()) continue;
                    visBatches.push_back({ modelRenderer, {inst}, std::move(vis) });
                }
            } else {
                visBatches.push_back({ modelRenderer, std::move(visibleInstances), {} });
            }
        }
        csm->BeginCascade(commandBuffer, slot, c, w, h);
        if (!visBatches.empty()) {
            static bool s_csmLogged = false;   // 一次性诊断（用户偏好：进度类日志用 LOGD）
            if (!s_csmLogged) {
                int totalInst = 0;
                for (auto& batch : visBatches) totalInst += static_cast<int>(batch.instances.size());
                LOGD("[CSM] cascade %d: %d batches / %d instances (slot %d)", c, static_cast<int>(visBatches.size()), totalInst, slot);
                s_csmLogged = true;
            }
            for (auto& batch : visBatches) {
                batch.renderer->RenderCsmDepth(commandBuffer, w, h, shadowMatrix, batch.instances, batch.vis);
            }
        }
        if (hasTerrainCasters && sceneRenderer.m_TerrainRenderer.IsInitialized()) {
            sceneRenderer.m_TerrainRenderer.RenderCsmDepth(commandBuffer, w, h, shadowMatrix, terrainCameraPosition);
            // 草投影暂时关闭（用户拍板 2026-09-18）：草影位置一直有偏移
            // （此前草密未察觉），修复投影错位前不收集草影。级联/视锥门控
            // 代码保留，修复后把 kGrassCsmShadowsEnabled 翻回 true 即可回归。
            constexpr bool kGrassCsmShadowsEnabled = false;
            if (kGrassCsmShadowsEnabled && c < kGrassCsmCascadeCount &&
                sceneRenderer.m_TerrainRenderer.EnsureGrassDepthPipeline(csm->GetRenderPass())) {
                sceneRenderer.m_TerrainRenderer.RenderGrassCsmDepth(commandBuffer, w, h, shadowMatrix, terrainCameraPosition,
                                                                    mainCameraFrustum);
            }
        }
        csm->EndCascade(commandBuffer);
    }
    csm->Finalize(commandBuffer, slot);   // 2D array → SHADER_READ_ONLY（合成 pass 采样）
}
