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
    // 水面不再写入 G-buffer/主深度（deferred water compositing）：不透明场景
    // 保持"无水"状态，水底几何与颜色完整保留；水面在几何 pass 结束后由
    // WaterRenderer::RenderTargets 写独立目标 RT，供后处理 water_composite 合成。
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
