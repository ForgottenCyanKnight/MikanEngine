#define GLM_ENABLE_EXPERIMENTAL

#include "Rendering/SceneFramePreparation.h"

#include "Rendering/SceneRenderer.h"
#include "AABB.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cfloat>
#include <memory>
#include <utility>

#include <glm/gtx/quaternion.hpp>

namespace {

using CpuProfileClock = SceneRenderCpuProfile::Clock;

double CpuProfileMilliseconds(CpuProfileClock::time_point start,
                              CpuProfileClock::time_point end)
{
    return SceneRenderCpuProfile::Milliseconds(start, end);
}

} // namespace

void SceneFramePreparation::Prepare(
    SceneRenderer& renderer,
    RenderFrameContext& ctx,
    bool cpuProfileEnabled,
    SceneRenderCpuProfile::FrameTiming& profileTiming,
    SceneRenderCpuProfile::Clock::time_point prepareStart)
{
    auto& m_RenderWorldValid = renderer.m_RenderWorldValid;
    auto& m_RenderWorld = renderer.m_RenderWorld;
    auto& m_RenderWorldFrameActive = renderer.m_RenderWorldFrameActive;
    auto& m_PrevModelMatricesSceneVersion = renderer.m_PrevModelMatricesSceneVersion;
    auto& m_PrevModelMatrices = renderer.m_PrevModelMatrices;
    auto& m_PrevProjViewMatrix = renderer.m_PrevProjViewMatrix;
    auto& m_HasPrevFrameMatrices = renderer.m_HasPrevFrameMatrices;
    auto& m_FrameId = renderer.m_FrameId;
    auto& m_CameraEntitiesCache = renderer.m_CameraEntitiesCache;
    auto& m_CameraCacheFrameId = renderer.m_CameraCacheFrameId;
    auto& m_CameraCacheSceneVersion = renderer.m_CameraCacheSceneVersion;
    auto& m_ModelPreloadFrameId = renderer.m_ModelPreloadFrameId;
    auto& m_ModelLoadFailureNextRetryFrame = renderer.m_ModelLoadFailureNextRetryFrame;
    constexpr uint64_t kFailedModelRetryIntervalFrames =
        SceneRenderer::kFailedModelRetryIntervalFrames;
    auto& m_DebugRenderer = renderer.m_DebugRenderer;
    auto& m_ModelRenderers = renderer.m_ModelRenderers;
    auto& m_MainCameraFrustumPlanes = renderer.m_MainCameraFrustumPlanes;
    auto& m_HasMainCameraFrustum = renderer.m_HasMainCameraFrustum;
    auto& m_LightCount = renderer.m_LightCount;
    auto& m_TerrainRenderer = renderer.m_TerrainRenderer;
    auto& m_WaterRenderer = renderer.m_WaterRenderer;
    auto& m_LastCameraPos = renderer.m_LastCameraPos;
    auto& m_CameraMoveThreshold = renderer.m_CameraMoveThreshold;
    auto& m_LastModelCount = renderer.m_LastModelCount;
    auto& m_ShowQuadTree = renderer.m_ShowQuadTree;
    auto& m_QuadTreeDirty = renderer.m_QuadTreeDirty;
    auto& m_CullingContext = renderer.m_CullingContext;
    auto& m_OcclusionCulling = renderer.m_OcclusionCulling;
    auto& m_MainCamUseSubMeshCulling = renderer.m_MainCamUseSubMeshCulling;
    auto& m_RenderPass = renderer.m_RenderPass;
    auto& g_cpuProfileFrameTiming = profileTiming;

    const auto RefreshRenderWorld = [&renderer]() { renderer.RefreshRenderWorld(); };
    const auto UpdateSceneMode = [&renderer]() { renderer.UpdateSceneMode(); };
    const auto GetCameraPosition = [&renderer]() { return renderer.GetCameraPosition(); };

    // 纯 CPU 阶段：收集场景实体/相机/光源，计算剔除状态，构建四叉树，收集调试线。
    // 不向 commandBuffer 发出任何 GPU 命令。

    if (!m_RenderWorldValid) RefreshRenderWorld();
    ctx.renderWorld = &m_RenderWorld;
    const RenderWorld& world = m_RenderWorld;
    const uint32_t sceneVersion = world.entitySetVersion;
    if (m_PrevModelMatricesSceneVersion != sceneVersion) {
        // Entity ID 在实体销毁后会复用。场景结构变化时旧历史不能继续作为
        // 运动矢量输入，否则会出现跨场景拖影，甚至把新实体匹配到旧矩阵。
        m_PrevModelMatrices.clear();
        m_PrevProjViewMatrix = glm::mat4(1.0f);
        m_HasPrevFrameMatrices = false;
        m_PrevModelMatricesSceneVersion = sceneVersion;
    }
    // BeginRenderFrame advances the epoch once for all views recorded in the
    // frame. Tool/test callers that prepare a view without that boundary get
    // an independent epoch and therefore cannot accidentally reuse a prior
    // standalone view's cache.
    if (!m_RenderWorldFrameActive) ++m_FrameId;

    // Model/voxel groups and entity lists are immutable snapshot data.  Both
    // views consume the same storage; only camera-dependent culling remains
    // in this per-view preparation pass.

    // Debug geometry is a SceneView-only product. GameView must not clear the
    // SceneView collection before its chain-end overlay has consumed it.
    if (ctx.isSceneView) {
        m_DebugRenderer.ClearInstances();
        m_DebugRenderer.ClearFrustums();
    }

    // 收集摄像机实体(帧缓存,与 GetMainCameraMatrices/GetCameraPosition 共用,避免同一帧多视图重复收集)
    const bool cameraCacheHit =
        m_CameraCacheFrameId == m_FrameId &&
        m_CameraCacheSceneVersion == world.entitySetVersion;
    const auto cameraCollectStart = cpuProfileEnabled
        ? CpuProfileClock::now()
        : CpuProfileClock::time_point{};
    const auto& cameraEntities = renderer.EnsureCameraEntitiesCached();
    if (cpuProfileEnabled && !cameraCacheHit) {
        g_cpuProfileFrameTiming.cameraCollectMs =
            CpuProfileMilliseconds(cameraCollectStart, CpuProfileClock::now());
    }

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
        const RenderWorldEntity* cameraEntityData = world.Find(cameraEntity);
        if (cameraEntityData == nullptr || !cameraEntityData->hasCamera) continue;
        const RenderCameraData& camera = cameraEntityData->camera;
        if (!camera.isMainCamera) continue;

        // 计算主相机的视锥平面
        glm::mat4 camView = camera.GetViewMatrix();
        float aspectRatio = (float)ctx.width / (float)ctx.height;
        glm::mat4 camProj = camera.GetProjectionMatrix(aspectRatio);
        camProj[1][1] *= -1; // Vulkan的Y轴翻转

        if (camera.enableFrustumCulling) {
            glm::mat4 mainViewProj = camProj * camView;
            mainCameraFrustumPlanes = AABBUtils::ExtractFrustumPlanes(mainViewProj);
            useMainCameraCulling = true;
            m_MainCameraFrustumPlanes = mainCameraFrustumPlanes;
            m_HasMainCameraFrustum = true;
        }
        break;
    }

    // 处理视锥体线框和剔除矩阵
    for (const auto& cameraEntity : cameraEntities) {
        const RenderWorldEntity* cameraEntityData = world.Find(cameraEntity);
        if (cameraEntityData == nullptr || !cameraEntityData->hasCamera) continue;
        const RenderCameraData& camera = cameraEntityData->camera;

        // 计算摄像机的视图投影矩阵
        glm::mat4 camView = camera.GetViewMatrix();
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

    // 相机属性面板开启后，在场景视图叠加全局三维碰撞体线框。
    if (ctx.isSceneView) {
        m_DebugRenderer.CollectCollisionWireframes(world, m_ModelRenderers);
    }

    // ===== 场景模式判定:仅启用 2D 相机且无主 3D 相机 → 2D 游戏 =====
    // 驱动渲染侧(游戏/场景视图跳过 3D)与编辑器形态(隐藏 3D 网格/gizmo)。
    // 每帧无条件调用（见主循环 FrameRender 开头），不得只放在 3D 渲染路径内。
    if (!m_RenderWorldFrameActive) {
        UpdateSceneMode();
    }

    // 更新场景光源数量
    m_LightCount = static_cast<int>(world.lights.size());

    glm::vec3& lightDir = ctx.lightDir;
    float& lightIntensity = ctx.lightIntensity;
    glm::vec3& lightColor = ctx.lightColor;

    if (!world.lights.empty()) {
        const RenderLightData& light = world.lights.front();
        lightColor = light.color;
        lightIntensity = light.intensity;

        if (light.type == RenderLightType::Directional) {
            glm::mat4 rotMat = glm::mat4_cast(light.rotation);
            lightDir = glm::normalize(glm::vec3(rotMat * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
        } else if (light.type == RenderLightType::Point) {
            lightDir = glm::normalize(glm::vec3(0.0f) - light.position);
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

    // These lists were derived once by RenderWorldBuilder and are shared by
    // SceneView/GameView without per-view allocation or copying.
    const auto& allModelEntities = world.modelEntities;

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
        const RenderWorldEntity* cameraEntityData = world.Find(cameraEntity);
        if (cameraEntityData != nullptr && cameraEntityData->hasCamera &&
            cameraEntityData->camera.isMainCamera) {
            useSubMeshCulling = cameraEntityData->camera.useSubMeshCulling;
            break;
        }
    }

    if (ctx.isSceneView && useMainCameraCulling && useSubMeshCulling) {
        ctx.frustumPlanes = mainCameraFrustumPlanes;
    }
    m_MainCamUseSubMeshCulling = useSubMeshCulling;

    // SceneView 的实际光栅相机是编辑器相机，但地形/草地的可见集必须与
    // 主相机一致：否则编辑器窗口会把主相机视锥外的地形也绘制出来，且
    // 草和地形会因为各自采用不同参考系而出现剔除不一致。没有可用主相机
    // 视锥时才回退到 SceneView 编辑器相机。
    const bool useMainCameraTerrainCulling =
        ctx.isSceneView && ctx.useMainCameraCulling;
    const std::array<Plane, 6>& terrainFrustum =
        useMainCameraTerrainCulling
            ? ctx.mainCameraFrustumPlanes
            : (ctx.useSceneCameraCulling ? ctx.frustumPlanes : ctx.mainCameraFrustumPlanes);
    const bool terrainUseFrustum = ctx.useSceneCameraCulling || ctx.useMainCameraCulling;
    const glm::vec3 terrainCameraPosition = useMainCameraTerrainCulling
        ? GetCameraPosition()
        : (ctx.isSceneView ? glm::vec3(glm::inverse(ctx.view)[3]) : cameraPos);
    m_TerrainRenderer.Prepare(world, terrainCameraPosition,
                              terrainFrustum, terrainUseFrustum);
    m_WaterRenderer.Prepare(world, terrainCameraPosition,
                            terrainFrustum, terrainUseFrustum);

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

    // 预加载所有需要的模型（在渲染前完成）。SceneView/GameView 共享同一
    // RenderWorld，因此同一渲染帧只需做一次 map 查找/资源发现；剔除仍在
    // SceneGeometryPass 中按视图独立执行。
    if (m_ModelPreloadFrameId != m_FrameId) {
        for (const auto& group : world.modelGroups) {
            if (group.entities.empty()) continue;
            const std::string assetPath = group.modelPath.empty()
                ? group.rendererKey : group.modelPath;

            // One canonical renderer owns geometry/material/pipeline resources
            // for an asset. Entity-qualified keys retain only animation state.
            auto assetRendererIt = m_ModelRenderers.find(assetPath);
            if (assetRendererIt == m_ModelRenderers.end()) {
                const auto retryIt = m_ModelLoadFailureNextRetryFrame.find(assetPath);
                const bool retryDeferred =
                    retryIt != m_ModelLoadFailureNextRetryFrame.end() &&
                    m_FrameId < retryIt->second;
                if (!retryDeferred) {
                    auto rendererInstance = std::make_unique<ModelRenderer>();
                    rendererInstance->Init(m_RenderPass);
                    rendererInstance->LoadModel(assetPath);
                    // Only register the renderer if the model actually loaded;
                    // failed assets must not re-enter Assimp/Vulkan setup every
                    // frame, but remain retryable after a cooldown.
                    if (rendererInstance->HasModelLoaded()) {
                        m_ModelLoadFailureNextRetryFrame.erase(assetPath);
                        m_ModelRenderers[assetPath] = std::move(rendererInstance);
                    } else {
                        m_ModelLoadFailureNextRetryFrame[assetPath] =
                            m_FrameId + kFailedModelRetryIntervalFrames;
                    }
                }
            }

            if (group.rendererKey != assetPath &&
                m_ModelRenderers.find(group.rendererKey) == m_ModelRenderers.end()) {
                const auto geometryRendererIt = m_ModelRenderers.find(assetPath);
                const bool geometryReady =
                    geometryRendererIt != m_ModelRenderers.end() &&
                    geometryRendererIt->second &&
                    geometryRendererIt->second->HasModelLoaded();
                // An animation-only renderer is meaningful only when its
                // canonical geometry asset exists. This also prevents a
                // missing .bin/.gltf dependency from being decoded twice.
                if (geometryReady) {
                    auto poseRenderer = std::make_unique<ModelRenderer>();
                    if (poseRenderer->LoadAnimationOnly(assetPath)) {
                        m_ModelRenderers[group.rendererKey] = std::move(poseRenderer);
                    }
                }
            }
        }
        m_ModelPreloadFrameId = m_FrameId;
    }

    if (m_ShowQuadTree) {
        m_CullingContext.BuildQuadTreeAroundCamera(world, cameraPos, &renderer);
    }
    m_OcclusionCulling.SetCullingContext(&m_CullingContext);   // 始终接线（z-prepass/GetVisibleSubMeshIndices 只视锥不碰四叉树）

    m_LastCameraPos = cameraPos;
    m_QuadTreeDirty = false;

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

    if (cpuProfileEnabled) {
        g_cpuProfileFrameTiming.prepareMs =
            CpuProfileMilliseconds(prepareStart, CpuProfileClock::now());
    }
}
