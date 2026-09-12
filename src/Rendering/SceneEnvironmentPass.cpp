#include "Rendering/SceneEnvironmentPass.h"

#include "AABB.h"
#include "World/WorldGlobals.h"

#include <array>

void SceneEnvironmentPass::RenderTerrainWater(SceneRenderer& sceneRenderer, RenderFrameContext& context)
{
    // 地形已经在 PrepareFrame 中完成资源/可见 chunk 准备；这里写入 G-buffer，
    // 与模型、体素共享同一个不透明几何 subpass。
    sceneRenderer.m_TerrainRenderer.Render(
        context.commandBuffer,
        context.width,
        context.height,
        context.projView,
        context.prevProjView,
        context.cameraPos);
    sceneRenderer.m_WaterRenderer.Render(
        context.commandBuffer,
        context.width,
        context.height,
        context.projView,
        context.prevProjView,
        context.cameraPos);
}

void SceneEnvironmentPass::RenderVoxelWorld(SceneRenderer& sceneRenderer, RenderFrameContext& context)
{
    // ---- 体素世界渲染（从 OpenGL 版迁移的无限世界）----
    if (!sceneRenderer.m_WorldRenderEnabled ||
        !sceneRenderer.m_WorldRenderer ||
        !sceneRenderer.m_WorldRenderer->IsInitialized() ||
        g_World == nullptr) {
        return;
    }

    // 与 model 渲染一致：世界剔除基于"游戏相机（Main Camera）"的视锥，
    // 这样 SceneView 预览的是游戏相机视野内的世界；无主相机时用渲染相机视锥兜底。
    std::array<Plane, 6> worldFrustum = context.mainCameraFrustumPlanes;
    if (!context.useMainCameraCulling) {
        worldFrustum = AABBUtils::ExtractFrustumPlanes(context.proj * context.view, -10.0f);
    }
    sceneRenderer.m_WorldRenderer->RenderWorld(
        context.commandBuffer,
        context.width,
        context.height,
        context.view,
        context.proj,
        context.cameraPos,
        worldFrustum);
}
