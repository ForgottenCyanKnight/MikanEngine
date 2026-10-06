#include "Rendering/SceneRenderer.h"
#include "Rendering/RayTracing/RayTracingScene.h"
#include "Core/VulkanManager.h"
#include "Core/VulkanGpuProfiler.h"
#include "Rendering/VoxRenderer.h"
#include "Rendering/RenderFrameContext.h"
#include <cstdlib>
bool SceneRenderer::SetVoxHardwareRayTracingEnabled(bool enabled) {
    if(!m_RayTracingScene)m_RayTracingScene=std::make_unique<RayTracingScene>();
    return m_RayTracingScene->SetEnabled(enabled);
}
RayTracingScene* SceneRenderer::GetHardwareRayTracingScene(){return m_RayTracingScene.get();}
void SceneRenderer::PrepareHardwareRayTracing(VkCommandBuffer commandBuffer) {
    if(!m_RayTracingScene){const char* value=std::getenv("MIKAN_HWRT_BUILD");if(!value || value[0]!='1')return;SetVoxHardwareRayTracingEnabled(true);}
    if(!m_RayTracingScene->IsEnabled())return;
    PreloadModels(); // Pure RT game views also need the canonical CPU meshes and texture resources.
    const auto& world=GetRenderWorld();
    for(const auto& group:world.voxGroups)if(!m_VoxRenderers.contains(group.voxPath)){
        auto renderer=std::make_unique<VoxRenderer>();renderer->Init(m_RenderPass);
        if(renderer->LoadVoxFile(group.voxPath))m_VoxRenderers.emplace(group.voxPath,std::move(renderer));else renderer->Cleanup();
    }
    // The prototype ground is a standard cube. Represent its unit geometry with six quads too.
    constexpr const char* cubeKey="builtin:rt-unit-cube";
    if(!m_VoxRenderers.contains(cubeKey)){
        VoxFormat::VoxData data;VoxFormat::Model cube{};cube.sizeX=cube.sizeY=cube.sizeZ=1;cube.voxels.push_back({0,0,0,1});data.models.push_back(cube);data.palette[1]={255,255,255,255};
        auto renderer=std::make_unique<VoxRenderer>();if(renderer->LoadFromVoxData(data,1))m_VoxRenderers.emplace(cubeKey,std::move(renderer));else renderer->Cleanup();
    }
    Core::VulkanGpuScope buildTiming(commandBuffer,"rt.as_prepare");
    m_RayTracingScene->Prepare(commandBuffer,world,m_VoxRenderers,m_ModelRenderers,GetCurrentFrameIndex(),GetCurrentFrameSerial());
}

VkImageView SceneRenderer::RenderHardwareRayTracing(VkCommandBuffer commandBuffer,uint32_t width,uint32_t height,
    const glm::mat4& view,const glm::mat4& proj,int viewSlot,const glm::vec3& sun,const glm::vec3& radiance,
    const RayTracingEnvironment& environment){
    if(!SetVoxHardwareRayTracingEnabled(true))return VK_NULL_HANDLE;
    PrepareHardwareRayTracing(commandBuffer);
    // CPU preparation preserves debug linework and picking assets; no geometry draw follows it.
    if(viewSlot==0){RenderFrameContext context{};context.commandBuffer=commandBuffer;context.width=int(width);context.height=int(height);
        context.view=view;context.proj=proj;context.cullView=view;context.cullProj=proj;context.isSceneView=true;context.viewSlot=0;
        context.uniformBuffer=&m_SceneUniformBuffer;context.descriptorSet=m_SceneDescriptorSet;context.projView=proj*view;context.prevProjView=context.projView;PrepareFrame(context);}
    if(!m_RayTracingViewport)m_RayTracingViewport=std::make_unique<RayTracingViewport>();
    return m_RayTracingViewport->Record(commandBuffer,*m_RayTracingScene,GetCurrentFrameIndex(),GetCurrentFrameSerial(),uint32_t(viewSlot),width,height,view,proj,sun,radiance,environment);
}
