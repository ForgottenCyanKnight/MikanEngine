#include "Rendering/Denoising/DlssRayReconstruction.h"
#include "Rendering/RendererBase.h"
#include "Core/EngineConfig.h"
#include "Core/Log.h"
#include "Core/PipelineCapture.h"
#include "Core/VulkanContext.h"
#include "Core/VulkanGpuProfiler.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <glm/gtc/type_ptr.hpp>

#if defined(_WIN32) && ((defined(MIKAN_ENABLE_DLSS_RR) && MIKAN_ENABLE_DLSS_RR) || (defined(MIKAN_ENABLE_DLSS_SR) && MIKAN_ENABLE_DLSS_SR))
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_dlssd_vk.h>
#include <nvsdk_ngx_params.h>

namespace mikan::denoising {
namespace {
std::recursive_mutex ngxMutex; // All NGX calls, across every viewport, are serialized.
VkPhysicalDevice supportedPhysical = VK_NULL_HANDLE, supportedSRPhysical = VK_NULL_HANDLE;
bool srFailed=false;std::unordered_map<uint32_t,VkExtent2D> outputSizes;
// Engine-generated custom Project ID, not an NVIDIA-issued application ID.
constexpr char projectId[] = "8b0362d7-316e-4609-b91f-658fd865d909";
// Supported NVIDIA devices use RR + Quality by default; other GPUs retain NRD + TAA.
bool SRRequested(){const char* v=std::getenv("MIKAN_HWRT_DLSS_SR");return MIKAN_ENABLE_DLSS_SR && (!v || v[0]!='0');}
NVSDK_NGX_PerfQuality_Value Quality(){
    const char* v=std::getenv("MIKAN_HWRT_DLSS_SR");
    if(v && std::strcmp(v,"performance")==0)return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    if(v && std::strcmp(v,"balanced")==0)return NVSDK_NGX_PerfQuality_Value_Balanced;
    if(v && std::strcmp(v,"ultraperformance")==0)return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
    if(v && std::strcmp(v,"dlaa")==0)return NVSDK_NGX_PerfQuality_Value_DLAA;
    return NVSDK_NGX_PerfQuality_Value_MaxQuality;
}
bool Requested() { const char* value=std::getenv("MIKAN_HWRT_DLSS_RR"); return MIKAN_ENABLE_DLSS_RR && (!value || value[0]!='0'); }
bool Failed(NVSDK_NGX_Result result,const char* operation) {
    if(NVSDK_NGX_SUCCEED(result))return false;
    LOGW("[DLSS RR] %s failed (NGX=0x%08x); using NRD/TAA",operation,unsigned(result));return true;
}
void NVSDK_CONV NgxLog(const char* message,NVSDK_NGX_Logging_Level,NVSDK_NGX_Feature) {
    if(const char* debug=std::getenv("MIKAN_HWRT_DLSS_RR_DEBUG");debug&&debug[0]=='1')LOGI("[NGX RR] %s",message);
}
struct Paths {
    std::wstring runtime, cache;
    const wchar_t* search[1]{};
    NVSDK_NGX_FeatureCommonInfo common{};
    bool Initialize() {
        std::array<wchar_t,32768> buffer{};
        const DWORD count=GetModuleFileNameW(nullptr,buffer.data(),DWORD(buffer.size()));
        if(!count || count>=buffer.size())return false;
        runtime=std::filesystem::path(std::wstring(buffer.data(),count)).parent_path().wstring();
        std::error_code error;
        if(!std::filesystem::is_regular_file(std::filesystem::path(runtime)/L"nvngx_dlssd.dll",error) && !std::filesystem::is_regular_file(std::filesystem::path(runtime)/L"nvngx_dlss.dll",error)){
            LOGW("[DLSS RR] nvngx_dlssd.dll missing beside executable; using NRD/TAA");return false;
        }
        auto cachePath=std::filesystem::temp_directory_path(error);
        if(error)return false;
        cachePath/=L"MikanEngine";cachePath/=L"NGX";
        std::filesystem::create_directories(cachePath,error);if(error)return false;
        cache=cachePath.wstring();search[0]=runtime.c_str();
        common.PathListInfo.Path=search;common.PathListInfo.Length=1;
        common.LoggingInfo.LoggingCallback=NgxLog;
        common.LoggingInfo.MinimumLoggingLevel=NVSDK_NGX_LOGGING_LEVEL_ON;
        return true;
    }
    NVSDK_NGX_FeatureDiscoveryInfo Discovery(NVSDK_NGX_Feature feature=NVSDK_NGX_Feature_RayReconstruction) const {
        NVSDK_NGX_FeatureDiscoveryInfo info{};info.SDKVersion=NVSDK_NGX_Version_API;
        info.FeatureID=feature;
        info.Identifier.IdentifierType=NVSDK_NGX_Application_Identifier_Type_Project_Id;
        info.Identifier.v.ProjectDesc={projectId,NVSDK_NGX_ENGINE_TYPE_CUSTOM,"MikanEngine-2026.10"};
        info.ApplicationDataPath=cache.c_str();info.FeatureInfo=&common;return info;
    }
};
struct Session {
    VkDevice device=VK_NULL_HANDLE;
    Paths paths;
    bool initialized=false,rrAvailable=false,srAvailable=false;
    ~Session() { std::lock_guard lock(ngxMutex);if(initialized){LOGI("[DLSS RR] NGX shutdown begin");NVSDK_NGX_VULKAN_Shutdown1(device);LOGI("[DLSS RR] NGX shutdown complete");} }
};
std::unordered_map<VkDevice,std::shared_ptr<Session>> sessions; // Retain until device shutdown, including swapchain rebuild.
std::shared_ptr<Session> AcquireSession(VkInstance instance,const Device& device) {
    if(auto found=sessions.find(device.logical);found!=sessions.end())return found->second;
    auto shared=std::make_shared<Session>();shared->device=device.logical;
    if(!shared->paths.Initialize())return {};
    if(Failed(NVSDK_NGX_VULKAN_Init_with_ProjectID(projectId,NVSDK_NGX_ENGINE_TYPE_CUSTOM,"MikanEngine-2026.10",
        shared->paths.cache.c_str(),instance,device.physical,device.logical,vkGetInstanceProcAddr,vkGetDeviceProcAddr,
        &shared->paths.common),"Vulkan Init"))return {};
    shared->initialized=true;
    NVSDK_NGX_Parameter* capabilities=nullptr;
    if(Failed(NVSDK_NGX_VULKAN_GetCapabilityParameters(&capabilities),"capability query"))return {};
    int rr=0,sr=0,needsRR=0,needsSR=0;
    capabilities->Get(NVSDK_NGX_Parameter_SuperSamplingDenoising_Available,&rr);
    capabilities->Get(NVSDK_NGX_Parameter_SuperSampling_Available,&sr);
    capabilities->Get(NVSDK_NGX_Parameter_SuperSamplingDenoising_NeedsUpdatedDriver,&needsRR);
    capabilities->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver,&needsSR);
    shared->rrAvailable=rr && !needsRR;shared->srAvailable=sr && !needsSR;
    NVSDK_NGX_VULKAN_DestroyParameters(capabilities);
    if(!shared->rrAvailable && !shared->srAvailable)return {};
    sessions[device.logical]=shared;return shared;
}
constexpr std::array<VkFormat,9> formats={VK_FORMAT_R16G16B16A16_SFLOAT,VK_FORMAT_R16G16B16A16_SFLOAT,
    VK_FORMAT_R16G16B16A16_SFLOAT,VK_FORMAT_R16G16B16A16_SFLOAT,VK_FORMAT_R32_SFLOAT,
    VK_FORMAT_R16G16_SFLOAT,VK_FORMAT_R16_SFLOAT,VK_FORMAT_R16G16B16A16_SFLOAT,VK_FORMAT_R16G16_SFLOAT};
class DlssRayReconstruction final:public IRayReconstruction {
    Device device{};VkExtent2D extent{},outputExtent{};bool reconstruction=true;
    std::shared_ptr<Session> session;
    NVSDK_NGX_Parameter* parameters=nullptr;
    NVSDK_NGX_Handle* feature=nullptr;
    std::array<VulkanImage,9> images;
    VkDescriptorSetLayout setLayout=VK_NULL_HANDLE;
    VkDescriptorPool pool=VK_NULL_HANDLE;
    VkDescriptorSet set=VK_NULL_HANDLE;
    VkPipelineLayout layout=VK_NULL_HANDLE;
    VkPipeline pipeline=VK_NULL_HANDLE;
    VkSampler sampler=VK_NULL_HANDLE;
    bool initializedImages=false, reset=true, failed=false;
public:
    explicit DlssRayReconstruction(bool rr=true):reconstruction(rr){}
    ~DlssRayReconstruction() override {
        // Owner clears histories only after previous offscreen work has completed.
        std::lock_guard lock(ngxMutex);
        LOGI("[DLSS RR] cleanup: releasing feature");if(feature)NVSDK_NGX_VULKAN_ReleaseFeature(feature);LOGI("[DLSS RR] cleanup: feature released");
        if(parameters)NVSDK_NGX_VULKAN_DestroyParameters(parameters);
        if(device.logical){
            if(pipeline)vkDestroyPipeline(device.logical,pipeline,device.allocator);
            if(layout)vkDestroyPipelineLayout(device.logical,layout,device.allocator);
            if(pool)vkDestroyDescriptorPool(device.logical,pool,device.allocator);
            if(setLayout)vkDestroyDescriptorSetLayout(device.logical,setLayout,device.allocator);
            if(sampler)vkDestroySampler(device.logical,sampler,device.allocator);
        }
        // Release resources while the NGX session and Vulkan device still exist.
        for(auto& image:images)image.Cleanup();LOGI("[DLSS RR] cleanup: releasing session");session.reset();LOGI("[DLSS RR] cleanup: complete");
    }
    bool Initialize(VkInstance instance,const Device& inDevice,VkExtent2D size,VkDescriptorSetLayout root,VkExtent2D outputSize) override {
        std::lock_guard lock(ngxMutex);device=inDevice;extent=size;outputExtent=outputSize.width&&outputSize.height?outputSize:size;
        if((reconstruction?(device.physical!=supportedPhysical||!Requested()):(device.physical!=supportedSRPhysical||!SRRequested()))||!root||!size.width||!size.height)return false;
        session=AcquireSession(instance,device);if(!session || (reconstruction?!session->rrAvailable:!session->srAvailable))return false;
        if(Failed(NVSDK_NGX_VULKAN_AllocateParameters(&parameters),"parameter allocation"))return false;
        const auto usage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
        for(size_t i=0;i<images.size();++i){
            VkFormatProperties properties{};vkGetPhysicalDeviceFormatProperties(device.physical,formats[i],&properties);
            if((properties.optimalTilingFeatures&(VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))!=
                (VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))return false;
            const bool unusedSRGuide=!reconstruction&&(i==1||i==2||i==3||i==6||i==8);const auto imageSize=unusedSRGuide?VkExtent2D{1,1}:(i==7?outputExtent:size);
            if(!images[i].Create(imageSize.width,imageSize.height,formats[i],VK_IMAGE_TILING_OPTIMAL,usage|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)||
               !images[i].CreateView(formats[i],VK_IMAGE_ASPECT_COLOR_BIT))return false;
        }
        VkDescriptorSetLayoutBinding bindings[17]{};
        for(uint32_t i=0;i<17;++i)bindings[i]={i,(i<7||i==13)?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};setInfo.bindingCount=17;setInfo.pBindings=bindings;
        if(vkCreateDescriptorSetLayout(device.logical,&setInfo,device.allocator,&setLayout)!=VK_SUCCESS)return false;
        VkDescriptorPoolSize counts[]={{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,8},{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,9}};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};poolInfo.maxSets=1;poolInfo.poolSizeCount=2;poolInfo.pPoolSizes=counts;
        if(vkCreateDescriptorPool(device.logical,&poolInfo,device.allocator,&pool)!=VK_SUCCESS)return false;
        VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};allocation.descriptorPool=pool;allocation.descriptorSetCount=1;allocation.pSetLayouts=&setLayout;
        if(vkAllocateDescriptorSets(device.logical,&allocation,&set)!=VK_SUCCESS)return false;
        VkDescriptorSetLayout layouts[]={root,setLayout};VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,128};
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};layoutInfo.setLayoutCount=2;layoutInfo.pSetLayouts=layouts;layoutInfo.pushConstantRangeCount=1;layoutInfo.pPushConstantRanges=&push;
        if(vkCreatePipelineLayout(device.logical,&layoutInfo,device.allocator,&layout)!=VK_SUCCESS)return false;
        auto code=RendererUtils::ReadFile(EngineConfig::GetShaderPath("dlss_rr_prepare.comp.spv"));
        auto shader=RendererUtils::CreateShaderModule(code,"dlss_rr_prepare.comp.spv");if(!shader)return false;
        VkComputePipelineCreateInfo compute{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};compute.layout=layout;
        compute.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};compute.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;compute.stage.module=shader;compute.stage.pName="main";
        auto result=vkCreateComputePipelines(device.logical,VK_NULL_HANDLE,1,&compute,device.allocator,&pipeline);
        vkDestroyShaderModule(device.logical,shader,device.allocator);if(result!=VK_SUCCESS)return false;
        VkSamplerCreateInfo sampling{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};sampling.magFilter=sampling.minFilter=VK_FILTER_NEAREST;
        sampling.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;sampling.addressModeU=sampling.addressModeV=sampling.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if(vkCreateSampler(device.logical,&sampling,device.allocator,&sampler)!=VK_SUCCESS)return false;
        LOGI("[DLSS %s] Vulkan adapter initialized: %ux%u -> %ux%u; feature creation pending",reconstruction?"RR":"SR",size.width,size.height,outputExtent.width,outputExtent.height);
        return true;
    }
    bool Record(VkCommandBuffer cmd,const RayReconstructionFrame& frame,const RayReconstructionInputs& input) override {
        std::lock_guard lock(ngxMutex);
        if(failed||!cmd||!frame.rayTracingSet||!session)return false;
        const char* label=reconstruction?(extent.width==outputExtent.width&&extent.height==outputExtent.height?"RR":"RR+SR"):"SR";
        if(!feature && reconstruction){
            // SDK 310.9.1 validates every quality preset: default F requires driver 580+, even for DLAA. // Use transformer D across all modes for compatibility with driver 576.80.
            parameters->Set(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_DLAA,
            unsigned(NVSDK_NGX_RayReconstruction_Hint_Render_Preset_D));
        parameters->Set(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality,
            unsigned(NVSDK_NGX_RayReconstruction_Hint_Render_Preset_D));
        parameters->Set(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Balanced,
            unsigned(NVSDK_NGX_RayReconstruction_Hint_Render_Preset_D));
        parameters->Set(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance,
            unsigned(NVSDK_NGX_RayReconstruction_Hint_Render_Preset_D));
        parameters->Set(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraPerformance,
            unsigned(NVSDK_NGX_RayReconstruction_Hint_Render_Preset_D));
        parameters->Set(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraQuality,
            unsigned(NVSDK_NGX_RayReconstruction_Hint_Render_Preset_D));
        NVSDK_NGX_DLSSD_Create_Params create{};
            create.InDenoiseMode=NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;
            create.InRoughnessMode=NVSDK_NGX_DLSS_Roughness_Mode_Packed;
            create.InUseHWDepth=NVSDK_NGX_DLSS_Depth_Type_Linear;
            create.InWidth=extent.width;create.InHeight=extent.height;create.InTargetWidth=outputExtent.width;create.InTargetHeight=outputExtent.height;
            create.InPerfQualityValue=extent.width==outputExtent.width&&extent.height==outputExtent.height?NVSDK_NGX_PerfQuality_Value_DLAA:Quality();
            create.InFeatureCreateFlags=NVSDK_NGX_DLSS_Feature_Flags_IsHDR|NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
            if(Failed(NGX_VULKAN_CREATE_DLSSD_EXT1(device.logical,cmd,1,1,&feature,parameters,&create),"feature creation")){failed=true;return false;}
            LOGI("[DLSS %s] enabled: %ux%u -> %ux%u, replacing NRD + TAA",label,extent.width,extent.height,outputExtent.width,outputExtent.height);
        }
        if(!feature && !reconstruction){
            for(const char* hint:{NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance})parameters->Set(hint,unsigned(NVSDK_NGX_DLSS_Hint_Render_Preset_K));
            NVSDK_NGX_DLSS_Create_Params create{};create.Feature.InWidth=extent.width;create.Feature.InHeight=extent.height;
            create.Feature.InTargetWidth=outputExtent.width;create.Feature.InTargetHeight=outputExtent.height;
            create.Feature.InPerfQualityValue=extent.width==outputExtent.width&&extent.height==outputExtent.height?NVSDK_NGX_PerfQuality_Value_DLAA:Quality();
            create.InFeatureCreateFlags=NVSDK_NGX_DLSS_Feature_Flags_IsHDR|NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
            if(Failed(NGX_VULKAN_CREATE_DLSS_EXT1(device.logical,cmd,1,1,&feature,parameters,&create),"SR creation")){failed=srFailed=true;return false;}
            LOGI("[DLSS SR] enabled after NRD: %ux%u -> %ux%u, replacing TAA",extent.width,extent.height,outputExtent.width,outputExtent.height);
        }
        const auto prepareTiming=Core::g_VulkanGpuProfiler.BeginScope(cmd,reconstruction?"dlss_rr.prepare":"dlss_sr.prepare",VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        VkImageMemoryBarrier barriers[9]{};
        for(uint32_t i=0;i<9;++i){
            auto& b=barriers[i];b={VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=images[i].GetImage();
            b.oldLayout=initializedImages?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout=VK_IMAGE_LAYOUT_GENERAL;b.srcAccessMask=initializedImages?VK_ACCESS_SHADER_READ_BIT:0;b.dstAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
            b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        }
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,9,barriers);
        const Texture sources[]={input.baseColor,input.diffuse,input.diffuseMaterial,input.specular,input.specularMaterial,input.primaryMotionDepth,input.virtualMotion.view?input.virtualMotion:input.primaryMotionDepth,input.virtualDepth.view?input.virtualDepth:input.primaryMotionDepth,input.virtualNormal.view?input.virtualNormal:input.primaryMotionDepth};
        VkDescriptorImageInfo infos[17]{};VkWriteDescriptorSet writes[17]{};
        for(uint32_t i=0;i<17;++i){
            infos[i]=(i<7||i==13)?VkDescriptorImageInfo{VK_NULL_HANDLE,images[i==13?8:i].GetView(),VK_IMAGE_LAYOUT_GENERAL}:
                VkDescriptorImageInfo{sampler,sources[i>=14?i-8:i-7].view,sources[i>=14?i-8:i-7].layout};
            writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[i].dstSet=set;writes[i].dstBinding=i;writes[i].descriptorCount=1;
            writes[i].descriptorType=(i<7||i==13)?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;writes[i].pImageInfo=&infos[i];
        }
        vkUpdateDescriptorSets(device.logical,17,writes,0,nullptr);
        glm::mat4 projection=frame.projection;
        for(int column=0;column<4;++column){projection[column][0]-=2*frame.jitterUV.x*frame.projection[column][3];projection[column][1]-=2*frame.jitterUV.y*frame.projection[column][3];}
        struct Push {glm::mat4 inverseViewProj;glm::vec4 eye,sun,light,extent;};static_assert(sizeof(Push)==128);
        Push push{glm::inverse(projection*frame.view),glm::vec4(frame.projection[2][2],frame.projection[3][2],frame.projection[2][3],frame.projection[3][3]),glm::vec4(0),glm::vec4(0),glm::vec4(extent.width,extent.height,reset||frame.resetHistory?1:0,reconstruction?0:1)};
        VkDescriptorSet sets[]={frame.rayTracingSet,set};
        vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,2,sets,0,nullptr);
        const char* producerGuides=std::getenv("MIKAN_HWRT_RR_PRODUCER_GUIDES");
        push.sun.x=reconstruction&&producerGuides&&producerGuides[0]=='1'?1.0f:0.0f;
        vkCmdPushConstants(cmd,layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),&push);vkCmdDispatch(cmd,(extent.width+7)/8,(extent.height+7)/8,1);
        for(uint32_t j=0;j<8;++j){const uint32_t i=j==7?8:j;auto& b=barriers[i];b.oldLayout=VK_IMAGE_LAYOUT_GENERAL;b.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;b.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;}
        VkImageMemoryBarrier inputBarriers[8]{};
        for(uint32_t j=0;j<8;++j)inputBarriers[j]=barriers[j==7?8:j];
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,8,inputBarriers);
        auto& capture=Core::PipelineCapture::GetInstance();
        if(reconstruction && frame.captureSerial!=UINT64_MAX && capture.Wants(frame.captureSerial,frame.captureViewSlot)){
            const char* names[]={"rr-input-noisy-color","rr-input-normal-roughness","rr-input-diffuse-albedo","rr-input-specular-albedo","rr-input-linear-depth","rr-input-motion-pixels"};
            for(uint32_t i=0;i<6;++i)capture.Record(cmd,images[i].GetImage(),formats[i],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                extent.width,extent.height,frame.captureSerial,frame.captureViewSlot,names[i],"dlss_rr_prepare.comp",i==1||i==5?"signed":(i==4?"depth":(i==0?"hdr":"unit")));
            capture.Record(cmd,images[8].GetImage(),formats[8],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,extent.width,extent.height,
                frame.captureSerial,frame.captureViewSlot,"rr-input-reflection-motion-pixels","dlss_rr_prepare.comp","signed");
        }
        NVSDK_NGX_Resource_VK resources[9]{};
        for(uint32_t i=0;i<9;++i){
            auto& r=resources[i];r.Type=NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW;r.ReadWrite=i==7;
            r.Resource.ImageViewInfo={images[i].GetView(),images[i].GetImage(),{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1},formats[i],i==7?outputExtent.width:extent.width,i==7?outputExtent.height:extent.height};
        }
        // RR uses row-major matrices with row vectors. GLM column-major/column-vector bytes already match; do not transpose.
        glm::mat4 worldToView=frame.view,viewToClip=frame.projection;
        NVSDK_NGX_VK_DLSSD_Eval_Params evaluate{};
        evaluate.pInColor=&resources[0];evaluate.pInNormals=&resources[1];evaluate.pInDiffuseAlbedo=&resources[2];evaluate.pInSpecularAlbedo=&resources[3];
        evaluate.pInDepth=&resources[4];evaluate.pInMotionVectors=&resources[5];evaluate.pInSpecularHitDistance=nullptr;evaluate.pInOutput=&resources[7];
        evaluate.pInMotionVectorsReflections=&resources[8];
        evaluate.InRenderSubrectDimensions={extent.width,extent.height};evaluate.InReset=reset||frame.resetHistory?1:0;
        // Rays sample at +jitterUV, so projected geometry shifts by -jitterUV.
        evaluate.InJitterOffsetX=-frame.jitterUV.x*extent.width;evaluate.InJitterOffsetY=-frame.jitterUV.y*extent.height;
        evaluate.InMVScaleX=evaluate.InMVScaleY=1.0f;evaluate.InPreExposure=evaluate.InExposureScale=1.0f;
        evaluate.pInWorldToViewMatrix=glm::value_ptr(worldToView);evaluate.pInViewToClipMatrix=glm::value_ptr(viewToClip);
        Core::g_VulkanGpuProfiler.EndScope(cmd,prepareTiming);
        Core::VulkanGpuScope evaluateTiming(cmd,reconstruction?"dlss_rr.evaluate":"dlss_sr.evaluate");
        NVSDK_NGX_Result result;
        if(reconstruction)result=NGX_VULKAN_EVALUATE_DLSSD_EXT(cmd,feature,parameters,&evaluate);
        else {
            NVSDK_NGX_VK_DLSS_Eval_Params sr{};sr.Feature.pInColor=&resources[0];sr.Feature.pInOutput=&resources[7];sr.pInDepth=&resources[4];sr.pInMotionVectors=&resources[5];
            sr.InRenderSubrectDimensions={extent.width,extent.height};sr.InReset=evaluate.InReset;
            sr.InJitterOffsetX=evaluate.InJitterOffsetX;sr.InJitterOffsetY=evaluate.InJitterOffsetY;
            sr.InMVScaleX=sr.InMVScaleY=sr.InPreExposure=sr.InExposureScale=1.0f;
            result=NGX_VULKAN_EVALUATE_DLSS_EXT(cmd,feature,parameters,&sr);
        }
        auto& output=barriers[7];output.oldLayout=VK_IMAGE_LAYOUT_GENERAL;output.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        output.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;output.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&output);
        initializedImages=true;
        if(Failed(result,"feature evaluation")){failed=true;if(outputExtent.width!=extent.width||!reconstruction)srFailed=true;return false;}
        if(reset)LOGI("[DLSS %s] first evaluation succeeded (Preset %s)",label,reconstruction?"D":"K");reset=false;return true;
    }
    Texture Output() const override {return {images[7].GetImage(),images[7].GetView(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};}
    void ResetHistory() override {reset=true;}
};
} // namespace

std::vector<std::string> ProbeDlssRayReconstruction(VkInstance instance,VkPhysicalDevice physical,
    std::span<const char* const> enabledInstance,std::span<const VkExtensionProperties> availableDevice) {
    std::lock_guard lock(ngxMutex);supportedPhysical=supportedSRPhysical=VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(physical,&properties);
    if(properties.vendorID!=0x10de||(!Requested()&&!SRRequested()))return {};
    VkPhysicalDeviceFeatures core{};vkGetPhysicalDeviceFeatures(physical,&core);if(!core.shaderStorageImageExtendedFormats)return {};
    Paths paths;if(!paths.Initialize())return {};std::vector<std::string> requested;
    for(const auto feature:{NVSDK_NGX_Feature_RayReconstruction,NVSDK_NGX_Feature_SuperSampling}){
        const bool rr=feature==NVSDK_NGX_Feature_RayReconstruction;if(rr?!Requested():!SRRequested())continue;
        const auto discovery=paths.Discovery(feature);NVSDK_NGX_FeatureRequirement requirement{};
        if(Failed(NVSDK_NGX_VULKAN_GetFeatureRequirements(instance,physical,&discovery,&requirement),"requirements")||requirement.FeatureSupported!=NVSDK_NGX_FeatureSupportResult_Supported)continue;
        uint32_t count=0;VkExtensionProperties* extensions=nullptr;bool usable=true;
        if(Failed(NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&discovery,&count,&extensions),"instance extensions"))continue;
        for(uint32_t i=0;i<count;++i)usable&=std::any_of(enabledInstance.begin(),enabledInstance.end(),[&](const char* e){return std::strcmp(e,extensions[i].extensionName)==0;});if(!usable)continue;
        if(Failed(NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(instance,physical,&discovery,&count,&extensions),"device extensions"))continue;
        for(uint32_t i=0;i<count;++i)usable&=std::any_of(availableDevice.begin(),availableDevice.end(),[&](const auto& e){return std::strcmp(e.extensionName,extensions[i].extensionName)==0;});if(!usable)continue;
        for(uint32_t i=0;i<count;++i){std::string name=extensions[i].extensionName;if(std::find(requested.begin(),requested.end(),name)==requested.end())requested.push_back(name);}
        (rr?supportedPhysical:supportedSRPhysical)=physical;LOGI("[DLSS %s] selected NVIDIA device requirements supported",rr?"RR":"SR");
    }
    return requested;
}
VkExtent2D ConfigureDlssSuperResolution(uint32_t viewSlot,VkExtent2D nativeExtent){
    std::lock_guard lock(ngxMutex);static std::unordered_map<uint32_t,VkExtent2D> renderSizes;
    if(!SRRequested()||srFailed||g_PhysicalDevice!=supportedSRPhysical){outputSizes[viewSlot]=nativeExtent;return nativeExtent;}
    auto found=outputSizes.find(viewSlot);
    if(found!=outputSizes.end()&&found->second.width==nativeExtent.width&&found->second.height==nativeExtent.height&&renderSizes.contains(viewSlot))return renderSizes[viewSlot];
    Device device{g_PhysicalDevice,g_Device,g_Allocator,g_QueueFamily,std::max(1u,g_MainWindowData.ImageCount)};
    auto session=AcquireSession(g_Instance,device);if(!session||!session->srAvailable)return nativeExtent;
    NVSDK_NGX_Parameter* capabilities{};if(Failed(NVSDK_NGX_VULKAN_GetCapabilityParameters(&capabilities),"SR settings capabilities"))return nativeExtent;
    VkExtent2D input{};unsigned maxW{},maxH{},minW{},minH{};float sharpness{};
    const auto result=NGX_DLSS_GET_OPTIMAL_SETTINGS(capabilities,nativeExtent.width,nativeExtent.height,Quality(),&input.width,&input.height,&maxW,&maxH,&minW,&minH,&sharpness);
    NVSDK_NGX_VULKAN_DestroyParameters(capabilities);
    if(Failed(result,"SR optimal settings")||!input.width||!input.height||input.width>nativeExtent.width||input.height>nativeExtent.height)return nativeExtent;
    outputSizes[viewSlot]=nativeExtent;renderSizes[viewSlot]=input;LOGI("[DLSS SR] view=%u SDK optimal render=%ux%u output=%ux%u mode=%u",viewSlot,input.width,input.height,nativeExtent.width,nativeExtent.height,unsigned(Quality()));return input;
}
VkExtent2D GetDlssOutputResolution(uint32_t viewSlot,VkExtent2D input){std::lock_guard lock(ngxMutex);auto found=outputSizes.find(viewSlot);return found==outputSizes.end()?input:found->second;}
void ShutdownDlssDevice(VkDevice device){std::lock_guard lock(ngxMutex);sessions.erase(device);outputSizes.clear();}

bool IsDlssRayReconstructionSupported(VkPhysicalDevice physical){std::lock_guard lock(ngxMutex);return physical==supportedPhysical&&supportedPhysical!=VK_NULL_HANDLE;}
std::unique_ptr<IRayReconstruction> CreateDlssRayReconstruction(){
    std::lock_guard lock(ngxMutex);if(!supportedPhysical||!Requested())return {};
    return std::make_unique<DlssRayReconstruction>();
}
std::unique_ptr<IRayReconstruction> CreateDlssSuperResolution(){std::lock_guard lock(ngxMutex);if(!supportedSRPhysical||!SRRequested()||srFailed)return {};return std::make_unique<DlssRayReconstruction>(false);}
} // namespace mikan::denoising
#else
namespace mikan::denoising {
std::vector<std::string> ProbeDlssRayReconstruction(VkInstance,VkPhysicalDevice,
    std::span<const char* const>,std::span<const VkExtensionProperties>){return {};}
bool IsDlssRayReconstructionSupported(VkPhysicalDevice){return false;}
std::unique_ptr<IRayReconstruction> CreateDlssRayReconstruction(){return {};}
std::unique_ptr<IRayReconstruction> CreateDlssSuperResolution(){return {};}
VkExtent2D ConfigureDlssSuperResolution(uint32_t,VkExtent2D extent){return extent;}
VkExtent2D GetDlssOutputResolution(uint32_t,VkExtent2D extent){return extent;}
void ShutdownDlssDevice(VkDevice){}
} // namespace mikan::denoising
#endif
