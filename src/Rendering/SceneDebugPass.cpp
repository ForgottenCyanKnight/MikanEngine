#include "Rendering/SceneDebugPass.h"

void SceneDebugPass::Collect(SceneRenderer& sceneRenderer, RenderFrameContext& context)
{
    if (!context.isSceneView) return;

    const RenderWorld& world = context.renderWorld != nullptr
        ? *context.renderWorld
        : sceneRenderer.m_RenderWorld;
    sceneRenderer.m_DebugRenderer.CollectAABBs(world, sceneRenderer.m_ModelRenderers);
    sceneRenderer.m_DebugRenderer.CollectVoxAABBs(world, sceneRenderer.m_VoxRenderers);

    // CollectBVH used to rediscover all camera entities inside every root
    // subtree call. With a flat scene this turned the disabled debug path
    // into an O(N^2) scan. Check the camera flag once and avoid entering
    // that traversal unless the user actually requests BVH wireframes.
    bool showBVHWireframe = false;
    for (const ECS::Entity cameraEntity : sceneRenderer.m_CameraEntitiesCache) {
        const RenderWorldEntity* cameraData = world.Find(cameraEntity);
        if (cameraData != nullptr && cameraData->hasCamera &&
            cameraData->camera.showBVHWireframe) {
            showBVHWireframe = true;
            break;
        }
    }
    if (showBVHWireframe) {
        sceneRenderer.m_DebugRenderer.CollectBVH(
            world,
            context.cameraPos,
            context.effectiveCullView,
            context.effectiveCullProj,
            sceneRenderer.m_ModelRenderers,
            sceneRenderer.m_VoxRenderers);
    }
}

void SceneDebugPass::RenderOverlay(SceneRenderer& sceneRenderer,
                                   VkCommandBuffer commandBuffer,
                                   int width,
                                   int height,
                                   VkRenderPass uiPass,
                                   const glm::mat4& view,
                                   const glm::mat4& proj)
{
    (void)width;
    (void)height;
    auto& wireframe = sceneRenderer.m_DebugRenderer.GetWireframeRenderer();
    wireframe.EnsureInit(uiPass);
    if (sceneRenderer.m_DebugRenderer.HasInstances()) {
        sceneRenderer.m_DebugRenderer.Render(commandBuffer, view, proj);
    }
    if (wireframe.HasFrustums()) {
        wireframe.RenderFrustums(commandBuffer, view, proj);
    }
}
