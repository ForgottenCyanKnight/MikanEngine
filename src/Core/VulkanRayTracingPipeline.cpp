#include "Core/DlssFrameGeneration.h"
#include "Core/VulkanRayTracingPipeline.h"
#include "Core/VulkanRenderHelpers.h"
#include "Core/EngineGlobal.h"
#include "Core/Log.h"
#include "Core/VulkanGpuProfiler.h"
#include "Rendering/SceneRenderer.h"
#include "Rendering/PostProcessChain.h"
#include "Rendering/AtmosphereRenderer.h"
#include "Rendering/SkyboxRenderer.h"
#include "Core/VulkanManager.h"
#include "Core/VulkanContext.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
extern AtmosphereRenderer g_AtmosphereRenderer;
extern bool g_AtmosphereEnabled;

const RayTracingEnvironment& PrepareHardwareRayTracingEnvironment(VkCommandBuffer cmd,const glm::vec3& fallbackCamera){
    static uint64_t preparedSerial=std::numeric_limits<uint64_t>::max();
    static RayTracingEnvironment environment;
    const auto serial=GetCurrentFrameSerial();
    if(preparedSerial==serial)return environment;
    preparedSerial=serial;environment={};
    const auto& world=g_SceneRenderer.GetRenderWorld();
    g_SkyboxRenderer.SyncFromRenderWorld(world);
    if(!g_SkyboxRenderer.IsEnabled() || !g_AtmosphereEnabled || !g_AtmosphereRenderer.IsInitialized())return environment;
    glm::vec3 camera=fallbackCamera;glm::mat4 mainView,mainProj;
    g_SceneRenderer.GetMainCameraMatrices(1.0f,mainView,mainProj,camera);
    glm::vec3 sun=g_AtmosphereRenderer.GetSunDirection(),color(1.0f,.96f,.89f);float intensity=1;
    GetSceneDirectionalLight(world,sun,color,intensity);
    const glm::vec4 light(color*intensity,1);
    Core::VulkanGpuScope environmentTiming(cmd,"rt.environment");
    g_AtmosphereRenderer.RenderSkyRT(cmd,sun,camera,light);
    // Keep the existing cube/SH producer's cloud input initialized, including the no-cloud case.
    g_AtmosphereRenderer.RenderCloudRT(cmd,sun,camera,world);
    g_AtmosphereRenderer.RenderSkyCubeRT(cmd,sun,camera,light);
    environment.transmittance=g_AtmosphereRenderer.GetTransmittanceView();
    environment.panorama=g_AtmosphereRenderer.GetSkyImageView();
    environment.sampler=g_AtmosphereRenderer.GetSkySampler();
    environment.irradiance=g_AtmosphereRenderer.GetSkyCubeSHBuffer();
    environment.altitudeMeters=glm::max(camera.y+200.0f,0.0f);
    const auto& sky=world.skyboxes.front();environment.tintIntensity=glm::vec4(sky.tint,glm::max(sky.intensity,0.0f));
    return environment;
}
bool RenderPureRayTracingView(VkCommandBuffer cmd,PostProcessChain& chain,
    uint32_t width,uint32_t height,uint32_t outputWidth,uint32_t outputHeight,
    VkRenderPass finalPass,VkFramebuffer framebuffer,VkRenderPass uiPass,
    const glm::mat4& view,const glm::mat4& proj,int viewSlot,bool swapchain){
    if(!chain.UsesHardwareRayTracing())return false;
    g_CurrentTAAJitter=glm::vec2(0);
    const auto& environment=PrepareHardwareRayTracingEnvironment(cmd,glm::vec3(glm::inverse(view)[3]));
    glm::vec3 sun=g_AtmosphereRenderer.GetSunDirection(),color(1.0f,.96f,.89f);float intensity=1;
    GetSceneDirectionalLight(g_SceneRenderer.GetRenderWorld(),sun,color,intensity);
    // Classify the GPU actually selected for rendering, not a second adapter
    // merely present in the machine. AMD APUs must not be mistaken for discrete Radeon GPUs.
    static VkPhysicalDevice cachedDevice=VK_NULL_HANDLE;
    static VkPhysicalDeviceProperties gpu{};
    static uint32_t loggedWidth[2]{},loggedHeight[2]{};
    if(cachedDevice!=g_PhysicalDevice){
        vkGetPhysicalDeviceProperties(g_PhysicalDevice,&gpu);cachedDevice=g_PhysicalDevice;
        loggedWidth[0]=loggedWidth[1]=loggedHeight[0]=loggedHeight[1]=0;
    }
    const bool amdIntegrated=gpu.vendorID==0x1002u&&gpu.deviceType==VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    // Optional matched-resolution comparison cap; preserve selected-GPU defaults.
    static const VkExtent2D comparisonCap=[](){
        const char* w=std::getenv("MIKAN_HWRT_MAX_WIDTH"),*h=std::getenv("MIKAN_HWRT_MAX_HEIGHT");
        if(!w&&!h)return VkExtent2D{};
        char* we=nullptr,*he=nullptr;
        const long width=w?std::strtol(w,&we,10):0,height=h?std::strtol(h,&he,10):0;
        if(!w||!h||we==w||he==h||*we!='\0'||*he!='\0'||width<1||height<1||width>16384||height>16384){
            LOGW("[HardwareRT] ignoring comparison cap: MAX_WIDTH/MAX_HEIGHT must both be integers 1..16384");return VkExtent2D{};
        }
        LOGI("[HardwareRT] comparison cap=%ldx%ld",width,height);
        return VkExtent2D{uint32_t(width),uint32_t(height)};
    }();
    const uint32_t maxWidth=std::min(amdIntegrated?960u:1920u,comparisonCap.width?comparisonCap.width:UINT32_MAX);
    const uint32_t maxHeight=std::min(amdIntegrated?540u:1080u,comparisonCap.height?comparisonCap.height:UINT32_MAX);
    const double scale=std::min({1.0,double(maxWidth)/std::max(width,1u),double(maxHeight)/std::max(height,1u)});
    uint32_t rtWidth=std::max(1u,uint32_t(std::floor(width*scale)));
    uint32_t rtHeight=std::max(1u,uint32_t(std::floor(height*scale)));
    const auto srRender=mikan::denoising::ConfigureDlssSuperResolution(uint32_t(viewSlot),{rtWidth,rtHeight});rtWidth=srRender.width;rtHeight=srRender.height;
    const uint32_t logSlot=viewSlot==0?0u:1u;
    if(loggedWidth[logSlot]!=rtWidth||loggedHeight[logSlot]!=rtHeight){
        LOGI("[HardwareRT] GPU=%s type=%u vendor=0x%x; view=%d native geometry/DI/GI=%ux%u, cap=%ux%u, output=%ux%u",
            gpu.deviceName,uint32_t(gpu.deviceType),gpu.vendorID,viewSlot,rtWidth,rtHeight,maxWidth,maxHeight,outputWidth,outputHeight);
        loggedWidth[logSlot]=rtWidth;loggedHeight[logSlot]=rtHeight;
    }
    const auto image=g_SceneRenderer.RenderHardwareRayTracing(cmd,rtWidth,rtHeight,view,proj,viewSlot,sun,color*intensity,environment);
    if(image){PostProcessChain::ExternalInputs ext;ext.hardwareRayTracingView=image;
        ext.cameraUBO.cameraPos=glm::vec4(glm::vec3(glm::inverse(view)[3]),1);ext.cameraUBO.view=view;ext.cameraUBO.proj=proj;ext.cameraUBO.invView=glm::inverse(view);ext.cameraUBO.invProj=glm::inverse(proj);
        chain.Execute(cmd,int(outputWidth),int(outputHeight),ext,framebuffer);
    }else{
        static bool reported=false;if(!reported){LOGE("[HardwareRT] pure RT viewport unavailable; no raster fallback");reported=true;}
        VkClearValue clear{};clear.color={{.15f,0,.15f,1}};
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};begin.renderPass=finalPass;begin.framebuffer=framebuffer;begin.renderArea.extent={outputWidth,outputHeight};begin.clearValueCount=1;begin.pClearValues=&clear;
        vkCmdBeginRenderPass(cmd,&begin,VK_SUBPASS_CONTENTS_INLINE);vkCmdEndRenderPass(cmd);
    }
    if(swapchain && image)Core::DlssFG::CaptureHudless(cmd,g_MainWindowData.Frames[g_MainWindowData.FrameIndex].Backbuffer,g_MainWindowData.SurfaceFormat.format,outputWidth,outputHeight);
    RenderUIOverlay(cmd,outputWidth,outputHeight,uiPass,framebuffer,swapchain,viewSlot==0?&view:nullptr,viewSlot==0?&proj:nullptr,&view,&proj);
    return true;
}
