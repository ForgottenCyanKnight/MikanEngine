#include "Rendering/RayTracing/RayTracingViewport.h"
#include "Rendering/RayTracing/RayTracingQualityOptions.h"
#include "Core/PipelineCapture.h"
#include <Rtxdi/DI/ReSTIRDI.h>
#include <Rtxdi/RtxdiUtils.h>
#include "Core/VulkanContext.h"
#include "Core/EngineConfig.h"
#include "Core/Log.h"
#include "Core/VulkanGpuProfiler.h"
#include <algorithm>
#include <cstring>
#include <cmath>
#include <cstdlib>

namespace {
uint32_t& NeeDiffuseSamples(){
    static uint32_t samples=[](){
        const char* value=std::getenv("MIKAN_HWRT_FRESH_DIFFUSE_SPP");
        if(value && (!std::strcmp(value,"1")||!std::strcmp(value,"2")||!std::strcmp(value,"4")))return uint32_t(value[0]-'0');
        return 1u;
    }();
    return samples;
}
}
namespace mikan::rt {
uint32_t GetNeeDiffuseSamples(){return NeeDiffuseSamples();}
uint32_t GetNeeDirectSamples(){
    static const uint32_t overrideSamples=[](){
        const char* v=std::getenv("MIKAN_HWRT_NEE_DI_SPP");if(!v)return 0u;
        char* end=nullptr;const long n=std::strtol(v,&end,10);
        return uint32_t(end==v||*end!='\0'||(n!=1&&n!=2&&n!=4&&n!=8)?2:n);
    }();
    return overrideSamples?overrideSamples:(GetNeeDiffuseSamples()==2u?4u:(GetNeeDiffuseSamples()==1u?1u:2u));
}
void SetNeeDiffuseSamples(uint32_t samples){
    if(samples==1u||samples==2u||samples==4u)NeeDiffuseSamples()=samples;
}
}

bool RayTracingViewport::InitializeComposite(){
    VkDescriptorSetLayoutBinding bindings[]={
        {0,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
        {1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
        {2,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
        {3,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
        {4,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
    VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set.bindingCount=5;set.pBindings=bindings;
    if(vkCreateDescriptorSetLayout(g_Device,&set,g_Allocator,&compositeSetLayout)!=VK_SUCCESS)return false;
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount=1;li.pSetLayouts=&compositeSetLayout;
    if(vkCreatePipelineLayout(g_Device,&li,g_Allocator,&compositeLayout)!=VK_SUCCESS)return false;
    auto code=RendererUtils::ReadFile(EngineConfig::GetShaderPath("vox_rt_composite.comp.spv"));
    auto shader=RendererUtils::CreateShaderModule(code,"vox_rt_composite.comp.spv");if(!shader)return false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};ci.layout=compositeLayout;
    ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;ci.stage.module=shader;ci.stage.pName="main";
    const auto result=vkCreateComputePipelines(g_Device,VK_NULL_HANDLE,1,&ci,g_Allocator,&compositePipeline);
    vkDestroyShaderModule(g_Device,shader,g_Allocator);if(result!=VK_SUCCESS)return false;
    VkDescriptorSetLayoutBinding taaBindings[]={
        {0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
        {1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
        {2,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
        {3,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
    set.bindingCount=4;set.pBindings=taaBindings;
    if(vkCreateDescriptorSetLayout(g_Device,&set,g_Allocator,&taaSetLayout)!=VK_SUCCESS)return false;
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,32};
    li.pSetLayouts=&taaSetLayout;li.pushConstantRangeCount=1;li.pPushConstantRanges=&push;
    if(vkCreatePipelineLayout(g_Device,&li,g_Allocator,&taaLayout)!=VK_SUCCESS)return false;
    code=RendererUtils::ReadFile(EngineConfig::GetShaderPath("vox_rt_taa.comp.spv"));
    shader=RendererUtils::CreateShaderModule(code,"vox_rt_taa.comp.spv");if(!shader)return false;
    ci.layout=taaLayout;ci.stage.module=shader;
    const auto taaResult=vkCreateComputePipelines(g_Device,VK_NULL_HANDLE,1,&ci,g_Allocator,&taaPipeline);
    vkDestroyShaderModule(g_Device,shader,g_Allocator);return taaResult==VK_SUCCESS;
}
bool RayTracingViewport::EnsureDenoisingFrame(Frame& frame,uint32_t width,uint32_t height){
    if(!frame.compositeDescriptor){
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool=pool;ai.descriptorSetCount=1;ai.pSetLayouts=&compositeSetLayout;
        if(vkAllocateDescriptorSets(g_Device,&ai,&frame.compositeDescriptor)!=VK_SUCCESS)return false;
    }
    if(!frame.taaDescriptor){
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool=pool;ai.descriptorSetCount=1;ai.pSetLayouts=&taaSetLayout;
        if(vkAllocateDescriptorSets(g_Device,&ai,&frame.taaDescriptor)!=VK_SUCCESS)return false;
    }
    frame.taaGuide.Cleanup();
    if(!frame.taaGuide.Create(width,height,VK_FORMAT_R32G32B32A32_SFLOAT,VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)||
        !frame.taaGuide.CreateView(VK_FORMAT_R32G32B32A32_SFLOAT,VK_IMAGE_ASPECT_COLOR_BIT))return false;
    const auto usage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    for(auto* image:{&frame.diffuse,&frame.viewZ,&frame.normal,&frame.motion,&frame.material,&frame.specular,&frame.specularMaterial}){
        image->Cleanup();const auto format=image==&frame.viewZ?VK_FORMAT_R32_SFLOAT:(image==&frame.motion?VK_FORMAT_R16G16_SFLOAT:VK_FORMAT_R16G16B16A16_SFLOAT);
        if(!image->Create(width,height,format,VK_IMAGE_TILING_OPTIMAL,usage,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)||
           !image->CreateView(format,VK_IMAGE_ASPECT_COLOR_BIT))return false;
    }
    frame.activeTiles.Cleanup();
    const VkDeviceSize tileBytes=16+VkDeviceSize((width+7)/8)*((height+7)/8)*4;
    if(!frame.activeTiles.Create(tileBytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))return false;
    frame.primaryHits.Cleanup();
    VkPhysicalDeviceProperties primaryLimits{};vkGetPhysicalDeviceProperties(g_PhysicalDevice,&primaryLimits);
    const VkDeviceSize primaryBytes=VkDeviceSize(width)*height*24u;
    if(primaryBytes>primaryLimits.limits.maxStorageBufferRange || !frame.primaryHits.Create(primaryBytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))return false;
    frame.gi.Cleanup();
    // Retain only a descriptor-compatible dummy for the inactive half-GI prototype.
    // No separate GI tracing, denoising or reconstruction pass is recorded.
    if(!frame.gi.Create(1,1,VK_FORMAT_R16G16B16A16_SFLOAT,VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)||
       !frame.gi.CreateView(VK_FORMAT_R16G16B16A16_SFLOAT,VK_IMAGE_ASPECT_COLOR_BIT))return false;
    frame.giWidth=frame.giHeight=1;
    LOGI("[HardwareRT GI] full-resolution combined DI/GI NRD: %ux%u",width,height);
    if(!frame.temporal.GetBuffer() && !frame.temporal.Create(sizeof(TemporalParameters),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))return false;
    // RTXDI G-buffer resources (recreated with the viewport size).
    frame.rtxdiContext.reset();frame.rtxdiReservoirs.Cleanup();frame.rtxdiNeighborOffsets.Cleanup();
    frame.rtxdiEnabled=false;frame.rtxdiShadingBufferIndex=0;
    const auto rtxdiUsage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
    if(!frame.rtxdiWorldPos.Create(width,height,VK_FORMAT_R32G32_SFLOAT,VK_IMAGE_TILING_OPTIMAL,rtxdiUsage,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
        ||!frame.rtxdiWorldPos.CreateView(VK_FORMAT_R32G32_SFLOAT,VK_IMAGE_ASPECT_COLOR_BIT)
         )return false;
    if(rtxdiWanted && (!frame.viewZPrev.Create(width,height,VK_FORMAT_R32_SFLOAT,VK_IMAGE_TILING_OPTIMAL,rtxdiUsage|VK_IMAGE_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
        ||!frame.viewZPrev.CreateView(VK_FORMAT_R32_SFLOAT,VK_IMAGE_ASPECT_COLOR_BIT)
        ||!frame.normalPrev.Create(width,height,VK_FORMAT_R16G16B16A16_SFLOAT,VK_IMAGE_TILING_OPTIMAL,rtxdiUsage|VK_IMAGE_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
        ||!frame.normalPrev.CreateView(VK_FORMAT_R16G16B16A16_SFLOAT,VK_IMAGE_ASPECT_COLOR_BIT)
        ||!frame.rtxdiConstants.Create(240,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)))return false;
    if(rtxdiSetLayout && !frame.rtxdiDescriptor){
        VkDescriptorSetAllocateInfo rtxdiAlloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        rtxdiAlloc.descriptorPool=rtxdiPool;rtxdiAlloc.descriptorSetCount=1;rtxdiAlloc.pSetLayouts=&rtxdiSetLayout;
        if(vkAllocateDescriptorSets(g_Device,&rtxdiAlloc,&frame.rtxdiDescriptor)!=VK_SUCCESS)return false;
    }
    return true;
}
bool RayTracingViewport::PrepareDenoising(Frame& frame,ViewHistory& history,const std::vector<RayTracingHitInstance>& hits,
    uint64_t serial,uint32_t width,uint32_t height,const glm::mat4& view,const glm::mat4& projection,uint32_t lightCount){
    uint64_t signature=1469598103934665603ull;
    auto mix=[&](uint64_t value){signature=(signature^value)*1099511628211ull;};
    // Changing tiers invalidates each viewport's reconstruction history.
    if(mikan::rt::UseFreshDiffuseExperiment())mix(mikan::rt::GetNeeDiffuseSamples());
    for(const auto& hit:hits){
        mix(hit.entity);mix(reinterpret_cast<uintptr_t>(hit.geometry.get()));mix(reinterpret_cast<uintptr_t>(hit.modelGeometry.get()));
        mix(reinterpret_cast<uintptr_t>(hit.albedoView));mix(reinterpret_cast<uintptr_t>(hit.mrView));mix(hit.materialFlags);
        for(int i=0;i<4;++i){uint32_t bits;std::memcpy(&bits,&hit.materialParams[i],4);mix(bits);}
        for(int i=0;i<4;++i){uint32_t bits;std::memcpy(&bits,&hit.color[i],4);mix(bits);}
    }
    bool reset=history.serial==UINT64_MAX||serial!=history.serial+1||signature!=history.sceneSignature;
    const auto camera=glm::inverse(view),previousCamera=glm::inverse(history.view);
    reset|=glm::distance(glm::vec3(camera[3]),glm::vec3(previousCamera[3]))>5.0f;
    reset|=glm::dot(glm::normalize(glm::vec3(camera[2])),glm::normalize(glm::vec3(previousCamera[2])))<0.5f;
    for(int c=0;c<4;++c)for(int r=0;r<4;++r)reset|=std::abs(projection[c][r]-history.projection[c][r])>1e-4f;
    const auto slots=std::max(1u,g_MainWindowData.ImageCount);
    if(history.width!=width||history.height!=height||history.slots!=slots){
        // Shared history is safe to replace: the frame loop also waits for
        // the previous offscreen submission, independently of the frame slot.
        history.taaHistory[0].Cleanup();history.taaHistory[1].Cleanup();
        history.taaInitialized[0]=history.taaInitialized[1]=false;history.taaRead=0;history.jitterUV=glm::vec2(0);
        history.reservoirs[0].Cleanup();history.reservoirs[1].Cleanup();
        history.giReservoirs[0].Cleanup();history.giReservoirs[1].Cleanup();history.giReservoirValid=false;
        history.reservoirValid=false;history.reservoirRead=0;
        history.rayReconstruction.reset();history.superResolution.reset();history.rrAttempted=history.srAttempted=false;
        history.denoiser.reset();history.specularDenoiser.reset();history.giDenoiser.reset();history.attempted=false;history.width=width;history.height=height;history.slots=slots;reset=true;
    }
    if(!history.reservoirs[0].GetBuffer()){
        const char* option=std::getenv("MIKAN_HWRT_RESTIR");
        history.restirEnabled=(option&&option[0]=='1')||mikan::rt::UseUnbiasedSpatialRestir();
        VkPhysicalDeviceProperties limits{};vkGetPhysicalDeviceProperties(g_PhysicalDevice,&limits);
        const VkDeviceSize fullBytes=VkDeviceSize(width)*height*48u;
        if(fullBytes>limits.limits.maxStorageBufferRange){history.restirEnabled=false;LOGW("[HardwareRT] ReSTIR DI exceeds storage buffer limit: using NEE/MIS");}
        const VkDeviceSize reservoirBytes=history.restirEnabled?fullBytes:48u;
        for(auto& buffer:history.reservoirs){
            if(!buffer.GetBuffer()&&!buffer.Create(reservoirBytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)){
                history.reservoirs[0].Cleanup();history.reservoirs[1].Cleanup();return false;
            }
        }
        LOGI("[HardwareRT] DI reservoir sampling %s; fresh candidate/reuse settings reported separately",history.restirEnabled?"enabled":"disabled (ordinary NEE)");
    }
    if(!history.giReservoirs[0].GetBuffer()){
        const char* option=std::getenv("MIKAN_HWRT_RESTIR_GI");
        history.restirGIEnabled=(option&&option[0]=='1')||mikan::rt::UseUnbiasedSpatialRestir();
        VkPhysicalDeviceProperties limits{};vkGetPhysicalDeviceProperties(g_PhysicalDevice,&limits);
        const VkDeviceSize fullBytes=VkDeviceSize(width)*height*ViewHistory::giReservoirStride;
        if(fullBytes>limits.limits.maxStorageBufferRange){history.restirGIEnabled=false;LOGW("[HardwareRT] ReSTIR GI exceeds storage buffer limit: using original diffuse paths");}
        auto allocateGI=[&](VkDeviceSize bytes){
            for(auto& buffer:history.giReservoirs)if(!buffer.Create(bytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))return false;
            return true;
        };
        if(!allocateGI(history.restirGIEnabled?fullBytes:ViewHistory::giReservoirStride)){
            history.giReservoirs[0].Cleanup();history.giReservoirs[1].Cleanup();
            history.restirGIEnabled=false;
            LOGW("[HardwareRT] ReSTIR GI allocation failed: using original diffuse paths");
            if(!allocateGI(ViewHistory::giReservoirStride)){history.giReservoirs[0].Cleanup();history.giReservoirs[1].Cleanup();return false;}
        }
        LOGI("[HardwareRT] ReSTIR GI buffers %s; sampling mode reported separately, buffers=%.1f MiB/view",
            history.restirGIEnabled?"enabled":"disabled",double(history.giReservoirs[0].GetSize())*2.0/(1024.0*1024.0));
    }
    if(mikan::rt::UseUnbiasedSpatialRestir()&&(!history.restirEnabled||!history.restirGIEnabled)){
        // The corrected path is one coordinated two-pass contract. A failed
        // allocation must not silently activate the old biased GI mode.
        history.restirEnabled=false;history.restirGIEnabled=false;
    }
    // Sampling-cache validity is independent of denoiser history validity.
    // First use, interrupted views, camera cuts and resource changes reset above.
    if(!history.rrAttempted){
        history.rrAttempted=true;history.rayReconstruction=mikan::denoising::CreateDlssRayReconstruction();
        mikan::denoising::Device rrDevice{g_PhysicalDevice,g_Device,g_Allocator,g_QueueFamily,slots};
        if(history.rayReconstruction&&!history.rayReconstruction->Initialize(g_Instance,rrDevice,{width,height},setLayout,history.outputExtent)){
            history.rayReconstruction.reset();LOGW("[DLSS RR] initialization unavailable; retaining NRD/TAA");
        }
    }
    if(!history.rayReconstruction&&!history.srAttempted){
        history.srAttempted=true;history.superResolution=mikan::denoising::CreateDlssSuperResolution();
        mikan::denoising::Device srDevice{g_PhysicalDevice,g_Device,g_Allocator,g_QueueFamily,slots};
        if(history.superResolution&&!history.superResolution->Initialize(g_Instance,srDevice,{width,height},setLayout,history.outputExtent))history.superResolution.reset();
    }
    if(!history.attempted){
        history.attempted=true;history.denoiser=mikan::denoising::CreateDiffuseDenoiser(mikan::denoising::Backend::Nrd);
        mikan::denoising::Device device{g_PhysicalDevice,g_Device,g_Allocator,g_QueueFamily,std::max(1u,g_MainWindowData.ImageCount)};
        if(history.denoiser && !history.denoiser->Initialize(device,{width,height}))history.denoiser.reset();
        history.specularDenoiser=mikan::denoising::CreateDiffuseDenoiser(mikan::denoising::Backend::Nrd,mikan::denoising::Signal::Specular);
        if(history.specularDenoiser && !history.specularDenoiser->Initialize(device,{width,height}))history.specularDenoiser.reset();
        if(!history.specularDenoiser)LOGW("[HardwareRT] NRD specular unavailable: using raw reflections");
        if(!history.denoiser)LOGW("[HardwareRT GI] NRD unavailable: using the current raw current GI signal");
    }
    std::vector<glm::mat4> remapping;remapping.reserve(std::max<size_t>(hits.size(),1));
    std::unordered_map<uint32_t,glm::mat4> nextTransforms;
    for(const auto& hit:hits){
        const auto previous=history.transforms.find(hit.entity);
        remapping.push_back(!reset&&previous!=history.transforms.end()?previous->second*glm::inverse(hit.model):glm::mat4(1));
        nextTransforms.emplace(hit.entity,hit.model);
    }
    std::vector<glm::mat4> primaryModels;primaryModels.reserve(std::max<size_t>(hits.size(),1));
    for(const auto& hit:hits)primaryModels.push_back(hit.model);
    if(primaryModels.empty())primaryModels.emplace_back(1.0f);
    const auto modelBytes=primaryModels.size()*sizeof(glm::mat4);
    if(frame.primaryTransforms.GetSize()<modelBytes){frame.primaryTransforms.Cleanup();if(!frame.primaryTransforms.Create(modelBytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))return false;}
    frame.primaryTransforms.Write(primaryModels.data(),modelBytes);
    if(remapping.empty())remapping.emplace_back(1.0f);
    const auto bytes=remapping.size()*sizeof(glm::mat4);
    VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(g_PhysicalDevice,&properties);
    if(bytes>properties.limits.maxStorageBufferRange)return false;
    if(frame.previousTransforms.GetSize()<bytes){
        frame.previousTransforms.Cleanup();
        if(!frame.previousTransforms.Create(bytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))return false;
    }
    frame.previousTransforms.Write(remapping.data(),bytes);
    auto halton=[](uint32_t index,uint32_t base){float value=0.0f,fraction=1.0f;while(index){fraction/=float(base);value+=fraction*float(index%base);index/=base;}return value;};
    const float ratio=float(std::max(history.outputExtent.width,width))/float(width);
    const uint32_t phases=uint32_t(std::ceil(8.0f*ratio*ratio));
    const uint32_t phase=uint32_t(serial%phases)+1;
    const char* taaSetting=std::getenv("MIKAN_HWRT_TAA");history.taaEnabled=!taaSetting||taaSetting[0]!='0';
    const glm::vec2 jitter=(history.taaEnabled||history.rayReconstruction||history.superResolution)?glm::vec2(halton(phase,2)-.5f,halton(phase,3)-.5f)/glm::vec2(width,height):glm::vec2(0);
    const glm::vec2 previousJitter=reset?jitter:history.jitterUV;
    frame.taaJitter=glm::vec4(jitter,previousJitter);frame.taaReset=reset;
    // Cached secondary radiance depends on every occluder, not just the primary
    // surface. Conservatively invalidate GI reuse when any instance moves.
    uint64_t transportSignature=signature;
    for(const auto& hit:hits)for(int c=0;c<4;++c)for(int r=0;r<4;++r){
        uint32_t bits;std::memcpy(&bits,&hit.model[c][r],4);transportSignature=(transportSignature^bits)*1099511628211ull;
    }
    const bool giReset=reset||!history.giReservoirValid||transportSignature!=history.giTransportSignature;
    history.giTransportSignature=transportSignature;
    // RTXDI SDK path: lazily create the runtime context and GPU resources,
    // then update buffer indices and constants for this frame.
    if(frame.rtxdiEnabled && !frame.rtxdiContext){
        rtxdi::ReSTIRDIStaticParameters rtxdiStatic;
        rtxdiStatic.RenderWidth=width;rtxdiStatic.RenderHeight=height;rtxdiStatic.NeighborOffsetCount=8192;
        frame.rtxdiContext=std::make_unique<rtxdi::ReSTIRDIContext>(rtxdiStatic);
        frame.rtxdiContext->SetResamplingMode(rtxdi::ReSTIRDI_ResamplingMode::Spatial);
        frame.rtxdiContext->SetInitialSamplingParameters(rtxdi::GetDefaultReSTIRDIInitialSamplingParams());
        frame.rtxdiContext->SetTemporalResamplingParameters(rtxdi::GetDefaultReSTIRDITemporalResamplingParams());
        frame.rtxdiContext->SetSpatioTemporalResamplingParameters(rtxdi::GetDefaultReSTIRDISpatioTemporalResamplingParams());
        frame.rtxdiContext->SetBoilingFilterParameters(rtxdi::GetDefaultReSTIRDIBoilingFilterParams());
        LOGI("[RTXDI] context created");
        std::vector<uint8_t> packedOffsets(8192*2);
        rtxdi::FillNeighborOffsetBuffer(packedOffsets.data(),8192);
        std::vector<glm::vec2> neighborOffsets(8192);
        for(size_t i=0;i<neighborOffsets.size();++i)neighborOffsets[i]=glm::vec2(
            float(int8_t(packedOffsets[i*2]))/127.0f,float(int8_t(packedOffsets[i*2+1]))/127.0f);
        const auto neighborBytes=neighborOffsets.size()*sizeof(glm::vec2);
        if(!frame.rtxdiNeighborOffsets.Create(neighborBytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))return false;
        frame.rtxdiNeighborOffsets.Write(neighborOffsets.data(),neighborBytes);
    }
    const char* rtxdiSpEnv=std::getenv("MIKAN_HWRT_RTXDI_SP");
    const bool rtxdiSpEnabled=frame.rtxdiEnabled&&(!rtxdiSpEnv||rtxdiSpEnv[0]!='0');
    uint32_t rtxdiShadingIndex=0,rtxdiRowPitch=0,rtxdiArrayPitch=0;
    if(frame.rtxdiEnabled && frame.rtxdiContext){
        frame.rtxdiContext->SetFrameIndex(uint32_t(serial)&0x3fffffffu);
        const auto bufferIndices=frame.rtxdiContext->GetBufferIndices();
        const auto reservoirParams=frame.rtxdiContext->GetReservoirBufferParameters();
        frame.rtxdiShadingBufferIndex=bufferIndices.shadingInputBufferIndex;
        rtxdiShadingIndex=bufferIndices.shadingInputBufferIndex;
        rtxdiRowPitch=reservoirParams.reservoirBlockRowPitch;rtxdiArrayPitch=reservoirParams.reservoirArrayPitch;
        if(!frame.rtxdiReservoirs.GetBuffer()){
            frame.rtxdiReservoirs.Create(VkDeviceSize(reservoirParams.reservoirArrayPitch)*3u*24u,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        }
        struct RtxdiConstsGPU{uint32_t runtimeParams[4];uint32_t reservoirParams[4];uint32_t bufferIndices[4];uint32_t diParams[4];float thresholds[4];glm::mat4 previousViewProj;glm::vec4 cameraPosition;float extent[4];glm::mat4 inverseRayViewProj;};
        static_assert(sizeof(RtxdiConstsGPU)==240);
        RtxdiConstsGPU consts{};
        consts.runtimeParams[0]=8192u-1u;consts.runtimeParams[1]=0;consts.runtimeParams[2]=uint32_t(serial)&0x3fffffffu;
        consts.runtimeParams[3]=rtxdiSpEnabled?1u:0u;
        consts.cameraPosition=glm::inverse(view)[3];
        consts.reservoirParams[0]=reservoirParams.reservoirBlockRowPitch;consts.reservoirParams[1]=reservoirParams.reservoirArrayPitch;
        consts.bufferIndices[0]=bufferIndices.initialSamplingOutputBufferIndex;
        consts.bufferIndices[1]=bufferIndices.temporalResamplingInputBufferIndex;
        consts.bufferIndices[2]=bufferIndices.spatialResamplingOutputBufferIndex;
        consts.bufferIndices[3]=bufferIndices.shadingInputBufferIndex;
        consts.diParams[0]=lightCount;
        auto qualityOption=[](const char* name,uint32_t fallback){const char* value=std::getenv(name);return value?uint32_t(std::clamp(std::strtol(value,nullptr,10),1L,32L)):fallback;};
        consts.diParams[1]=qualityOption("MIKAN_HWRT_RTXDI_CANDIDATES",16);
        consts.diParams[2]=qualityOption("MIKAN_HWRT_RTXDI_SPATIAL_SAMPLES",8);
        consts.thresholds[0]=0.95f;consts.thresholds[1]=0.05f;
        consts.previousViewProj=reset?projection*view:history.projection*history.view;
        glm::mat4 rayProjection=projection;
        for(int column=0;column<4;++column){
            rayProjection[column][0]-=2.0f*jitter.x*projection[column][3];
            rayProjection[column][1]-=2.0f*jitter.y*projection[column][3];
        }
        consts.inverseRayViewProj=glm::inverse(rayProjection*view);
        consts.extent[0]=float(width);consts.extent[1]=float(height);
        const char* tileOption=std::getenv("MIKAN_HWRT_TILE_CULL");consts.extent[2]=(!tileOption||tileOption[0]!='0')?1.0f:0.0f;
        if(!frame.rtxdiConstants.GetBuffer())frame.rtxdiConstants.Create(sizeof(RtxdiConstsGPU),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        frame.rtxdiConstants.Write(&consts,sizeof(consts));

        if(frame.rtxdiDescriptor){
            VkDescriptorImageInfo rtxdiImages[7];
            rtxdiImages[0]={VK_NULL_HANDLE,frame.viewZ.GetView(),VK_IMAGE_LAYOUT_GENERAL};
            rtxdiImages[1]={VK_NULL_HANDLE,frame.normal.GetView(),VK_IMAGE_LAYOUT_GENERAL};
            rtxdiImages[2]={VK_NULL_HANDLE,frame.rtxdiWorldPos.GetView(),VK_IMAGE_LAYOUT_GENERAL};
            rtxdiImages[3]={VK_NULL_HANDLE,frame.viewZPrev.GetView(),VK_IMAGE_LAYOUT_GENERAL};
            rtxdiImages[4]={VK_NULL_HANDLE,frame.normalPrev.GetView(),VK_IMAGE_LAYOUT_GENERAL};
            rtxdiImages[5]={VK_NULL_HANDLE,frame.material.GetView(),VK_IMAGE_LAYOUT_GENERAL};
            rtxdiImages[6]={VK_NULL_HANDLE,frame.specularMaterial.GetView(),VK_IMAGE_LAYOUT_GENERAL};
            VkDescriptorBufferInfo rtxdiBuffers[3];
            rtxdiBuffers[0]={frame.sceneInputs->emissiveLights.GetBuffer(),0,VK_WHOLE_SIZE};
            rtxdiBuffers[1]={frame.rtxdiReservoirs.GetBuffer(),0,VK_WHOLE_SIZE};
            rtxdiBuffers[2]={frame.rtxdiNeighborOffsets.GetBuffer(),0,VK_WHOLE_SIZE};
            VkDescriptorBufferInfo rtxdiConstsInfo{frame.rtxdiConstants.GetBuffer(),0,sizeof(RtxdiConstsGPU)};
            VkWriteDescriptorSet rtxdiWrites[11]{};
            for(uint32_t i=0;i<6;++i){rtxdiWrites[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};rtxdiWrites[i].dstSet=frame.rtxdiDescriptor;rtxdiWrites[i].dstBinding=1+i;rtxdiWrites[i].descriptorCount=1;rtxdiWrites[i].descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;rtxdiWrites[i].pImageInfo=&rtxdiImages[i];}
            for(uint32_t i=0;i<3;++i){rtxdiWrites[6+i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};rtxdiWrites[6+i].dstSet=frame.rtxdiDescriptor;rtxdiWrites[6+i].dstBinding=7+i;rtxdiWrites[6+i].descriptorCount=1;rtxdiWrites[6+i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;rtxdiWrites[6+i].pBufferInfo=&rtxdiBuffers[i];}
            rtxdiWrites[9]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};rtxdiWrites[9].dstSet=frame.rtxdiDescriptor;rtxdiWrites[9].dstBinding=10;rtxdiWrites[9].descriptorCount=1;rtxdiWrites[9].descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;rtxdiWrites[9].pBufferInfo=&rtxdiConstsInfo;
            rtxdiWrites[10]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};rtxdiWrites[10].dstSet=frame.rtxdiDescriptor;rtxdiWrites[10].dstBinding=11;rtxdiWrites[10].descriptorCount=1;rtxdiWrites[10].descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;rtxdiWrites[10].pImageInfo=&rtxdiImages[6];
            vkUpdateDescriptorSets(g_Device,11,rtxdiWrites,0,nullptr);
            VkDescriptorBufferInfo rtxdiWordsInfo{frame.rtxdiReservoirs.GetBuffer(),0,VK_WHOLE_SIZE};
            VkDescriptorImageInfo rtxdiPosInfo{VK_NULL_HANDLE,frame.rtxdiWorldPos.GetView(),VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet rtxdiMainWrites[2]{};
            rtxdiMainWrites[0]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};rtxdiMainWrites[0].dstSet=frame.descriptor;rtxdiMainWrites[0].dstBinding=32;rtxdiMainWrites[0].descriptorCount=1;rtxdiMainWrites[0].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;rtxdiMainWrites[0].pBufferInfo=&rtxdiWordsInfo;
            rtxdiMainWrites[1]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};rtxdiMainWrites[1].dstSet=frame.descriptor;rtxdiMainWrites[1].dstBinding=33;rtxdiMainWrites[1].descriptorCount=1;rtxdiMainWrites[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;rtxdiMainWrites[1].pImageInfo=&rtxdiPosInfo;
            vkUpdateDescriptorSets(g_Device,2,rtxdiMainWrites,0,nullptr);
        }
    }
    static const float fireflyScale=[](){
        const char* enabled=std::getenv("MIKAN_HWRT_FIREFLY_CLAMP");
        if(enabled && enabled[0]=='0')return 0.0f;
        const char* option=std::getenv("MIKAN_HWRT_FIREFLY_SCALE");
        char* end=nullptr;const float value=option?std::strtof(option,&end):1.0f;
        return std::isfinite(value)&&value>0.0f&&(!option || (end!=option && *end=='\0'))?std::clamp(value,0.01f,100.0f):1.0f;
    }();
    static const bool reportedFirefly=[](){LOGI("[HardwareRT] firefly clamp scale=%.3f, linear-HDR knees DI=16 GI=8 specular=32 times scene exposure",fireflyScale);return true;}();
    (void)reportedFirefly;
    // Diagnostic ablation only. Bits: DI=1, GI=2, GGX=4; ideal mirrors are shared.
    static const float profileMask=[](){
        const char* option=std::getenv("MIKAN_HWRT_PROFILE_MASK");
        if(!option)return 0.0f;
        char* end=nullptr;const long mask=std::strtol(option,&end,10);
        if(!Core::VulkanGpuProfiler::IsRequested() || end==option || *end!='\0' || mask<0 || mask>7){
            LOGW("[HardwareRT][Cost] ignoring mask: requires GPU profiling and integer 0..7");
            return 0.0f;
        }
        LOGW("[HardwareRT][Cost] diagnostic DI/GI/GGX mask=%ld; incomplete image, independent sampling streams; unset MIKAN_HWRT_PROFILE_MASK and restart to restore normal rendering",mask);
        const char* mirrors=std::getenv("MIKAN_HWRT_PROFILE_MIRRORS");
        const bool skipMirrors=mirrors && mirrors[0]=='0';
        LOGW("[HardwareRT][Cost] ideal mirror branches=%s",skipMirrors?"SKIPPED":"ON");
        return float(mask+8+(skipMirrors?16:0));
    }();
    static const bool russianRoulette=[](){
        const char* option=std::getenv("MIKAN_HWRT_RR");
        const bool enabled=!option || option[0]!='0';
        LOGI("[HardwareRT GI] Russian roulette=%s (MIKAN_HWRT_RR=0 disables): preserve first 2 GI hits, minimum survival=0.80, ideal mirror continuations preserved",enabled?"ON":"OFF");
        return enabled;
    }();
    static const bool primaryFourSamples=[](){
        const char* option=std::getenv("MIKAN_HWRT_PRIMARY_GI_4SPP");
        const bool enabled=option && option[0]=='1';
        if(enabled)LOGW("[HardwareRT GI] diagnostic primary GI=4 fresh samples; disable GI reuse for sampling comparison");
        return enabled;
    }();
    const bool freshDiffuse=mikan::rt::UseFreshDiffuseExperiment();
    const uint32_t freshSamples=mikan::rt::GetNeeDiffuseSamples();
    const uint32_t neeDiSamples=mikan::rt::GetNeeDirectSamples();
    static const bool neeR2=[](){const char* v=std::getenv("MIKAN_HWRT_NEE_SAMPLING");return v&&std::strcmp(v,"r2")==0;}();
    static const bool neeStrictShadow=[](){const char* v=std::getenv("MIKAN_HWRT_NEE_STRICT_SHADOW");return v&&v[0]=='1';}();
    static const bool neeLocalLights=[](){const char* v=std::getenv("MIKAN_HWRT_NEE_LIGHT_SELECTION");return v&&std::strcmp(v,"local")==0;}();
    static const bool neeEdgeBudget=[](){const char* v=std::getenv("MIKAN_HWRT_NEE_ADAPTIVE");return v&&std::strcmp(v,"edges")==0;}();
    static bool reportedNeeBudget=false;
    if(freshDiffuse&&!reportedNeeBudget){
        LOGI("[HardwareRT NEE budgets] primary DI=%u GI=%u; sampling=%s; strict emitter shadow=%s; primary MIS uses sample counts",neeDiSamples,freshSamples,neeR2?"randomized R2":"white PRNG",neeStrictShadow?"ON":"OFF");
        LOGI("[HardwareRT NEE lights] selection=%s",neeLocalLights?"position/normal aware power mixture":"global power/area CDF");
        LOGI("[HardwareRT NEE adaptive] geometry-edge GI extra paths=%s (max4, no stochastic stopping)",neeEdgeBudget?"ON":"OFF");
        reportedNeeBudget=true;
    }
    static uint32_t reportedFreshSamples=0;
    if(freshDiffuse&&reportedFreshSamples!=freshSamples){
        LOGI("[HardwareRT NEE] tier=%s; ordinary surfaces and mirror-terminal GI: %u fresh diffuse paths; performance=1 balanced=2 quality=4, default=performance, no ReSTIR DI/GI",freshSamples==4?"quality":(freshSamples==2?"balanced":"performance"),freshSamples);
        reportedFreshSamples=freshSamples;
    }
    const bool unbiasedRestir=mikan::rt::UseUnbiasedSpatialRestir()&&history.restirEnabled&&history.restirGIEnabled&&!frame.rtxdiEnabled;
    static const bool temporalReuseRequested=[](){const char* v=std::getenv("MIKAN_HWRT_RESTIR_TEMPORAL");return v&&v[0]=='1';}();
    // Reuse previous fresh reservoirs only; never recursively accumulated history.
    // Transport changes and camera discontinuities still invalidate history.
    const bool correctedTemporal=unbiasedRestir&&temporalReuseRequested&&!reset&&!giReset&&history.reservoirValid;
    static const uint32_t referenceSamples=[](){
        const char* v=std::getenv("MIKAN_HWRT_REFERENCE_SPP");char* end=nullptr;
        const long n=v?std::strtol(v,&end,10):0;
        return uint32_t(v&&end!=v&&*end=='\0'&&n>=1&&n<=64?n:0);
    }();
    static const uint32_t giCandidates=[](){
        const char* v=std::getenv("MIKAN_HWRT_GI_CANDIDATES");char* end=nullptr;
        const long n=v?std::strtol(v,&end,10):8;
        return uint32_t(v&&(end==v||*end!='\0'||n<1||n>64)?8:n);
    }();
    static const uint32_t sampleSeed=[](){
        const char* v=std::getenv("MIKAN_HWRT_SAMPLE_SEED");char* end=nullptr;
        const long n=v?std::strtol(v,&end,10):0;
        return uint32_t(v&&end!=v&&*end=='\0'&&n>=0&&n<=65535?n:0);
    }();
    static const bool diReuse=[](){
        const char* option=std::getenv("MIKAN_HWRT_DI_REUSE");
        return option && option[0]=='1';
    }();
    static const uint32_t diCandidates=[](){
        const char* option=std::getenv("MIKAN_HWRT_DI_CANDIDATES");
        char* end=nullptr;const long value=option?std::strtol(option,&end,10):8;
        return uint32_t(option && (end==option || *end!='\0' || value<1 || value>31)?8:value);
    }();
    static bool reportDI=false;
    if(!reportDI){
        if(unbiasedRestir)LOGI("[HardwareRT DI/GI] experimental current-frame spatial resampling, visibility/support correction; DI candidates=%u, GI candidates=%u, max current-frame sources=5; temporal mode logged separately, no estimator firefly clamp",diCandidates,giCandidates);
        else if(frame.rtxdiEnabled)LOGI("[HardwareRT DI] active=RTXDI SDK initial RIS + spatial reuse");
        else if(history.restirEnabled)LOGI("[HardwareRT DI] active=experimental fresh RIS, candidates=%u, temporal/spatial reservoir reuse=%s",diCandidates,diReuse?"ON":"OFF");
        else LOGI("[HardwareRT DI] active=ordinary NEE/MIS (default); experimental DI disabled");
        reportDI=true;
        LOGI("[HardwareRT ReSTIR temporal] requested=%s: previous fresh reservoirs only, max age=1 frame, no recursive accumulation; MIKAN_HWRT_RESTIR_TEMPORAL=0 disables",temporalReuseRequested?"ON":"OFF");
    }
    // giOptions.w: diagnostics 0..4, roulette 5, primary 4 spp 6,
    // DI reuse 7, DI count 8..12, corrected spatial 13, reference 14,
    // white PRNG 15, GI/reference count 16..22, one-frame temporal 23.
    // Maximum candidate count64 leaves all bits exact in a float integer.
    TemporalParameters temporal{view,reset?projection*view:history.projection*history.view,
        reset?view:history.view,frame.taaJitter,
        glm::vec4(float(serial&65535),reset?1:0,stbnAvailable?1:0,!freshDiffuse&&history.restirEnabled?1:0),
        glm::vec4(!freshDiffuse&&history.restirGIEnabled?1:0,giReset?1:0,freshDiffuse?0.0f:fireflyScale,profileMask+(russianRoulette?32.0f:0.0f)+(primaryFourSamples?64.0f:0.0f)+(diReuse?128.0f:0.0f)+float(diCandidates*256u)+(unbiasedRestir?8192.0f:0.0f)+(referenceSamples?16384.0f:0.0f)+((freshDiffuse||unbiasedRestir||referenceSamples)?32768.0f:0.0f)+float((unbiasedRestir?giCandidates:referenceSamples)*65536u)+(correctedTemporal?8388608.0f:0.0f)),
        glm::vec4(freshDiffuse?(freshSamples==4?-4.0f:(freshSamples==1?-2.0f:-1.0f)):(frame.rtxdiEnabled?(rtxdiSpEnabled?3.0f:2.0f):(history.restirEnabled?1.0f:0.0f)),float(freshDiffuse?neeDiSamples:rtxdiShadingIndex),float(freshDiffuse?(uint32_t(neeR2)+2u*uint32_t(neeStrictShadow)+4u*uint32_t(neeLocalLights)+8u*uint32_t(neeEdgeBudget)):rtxdiRowPitch),float(frame.rtxdiEnabled?rtxdiArrayPitch:sampleSeed))};
    history.jitterUV=jitter;
    frame.temporal.Write(&temporal,sizeof(temporal));
    const VulkanImage* guides[]={&frame.diffuse,&frame.viewZ,&frame.normal,&frame.motion,&frame.material,&frame.specular,&frame.specularMaterial};
    VkDescriptorImageInfo images[7];VkWriteDescriptorSet writes[9]{};
    for(uint32_t i=0;i<7;++i){
        images[i]={VK_NULL_HANDLE,guides[i]->GetView(),VK_IMAGE_LAYOUT_GENERAL};
        writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[i].dstSet=frame.descriptor;writes[i].dstBinding=i<5?10+i:22+i-5;
        writes[i].descriptorCount=1;writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;writes[i].pImageInfo=&images[i];
    }
    VkDescriptorBufferInfo buffers[]={{frame.temporal.GetBuffer(),0,sizeof(temporal)},{frame.previousTransforms.GetBuffer(),0,bytes}};
    for(uint32_t i=0;i<2;++i){
        writes[7+i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[7+i].dstSet=frame.descriptor;writes[7+i].dstBinding=15+i;
        writes[7+i].descriptorCount=1;writes[7+i].descriptorType=i?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;writes[7+i].pBufferInfo=&buffers[i];
    }
    vkUpdateDescriptorSets(g_Device,9,writes,0,nullptr);
    VkDescriptorBufferInfo primaryBuffers[]={{frame.primaryHits.GetBuffer(),0,VK_WHOLE_SIZE},{frame.primaryTransforms.GetBuffer(),0,modelBytes}};
    VkDescriptorImageInfo primaryPosition{VK_NULL_HANDLE,frame.rtxdiWorldPos.GetView(),VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet primaryWrites[3]{};
    for(uint32_t i=0;i<2;++i){primaryWrites[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};primaryWrites[i].dstSet=frame.descriptor;primaryWrites[i].dstBinding=34+i;primaryWrites[i].descriptorCount=1;primaryWrites[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;primaryWrites[i].pBufferInfo=&primaryBuffers[i];}
    primaryWrites[2]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};primaryWrites[2].dstSet=frame.descriptor;primaryWrites[2].dstBinding=33;primaryWrites[2].descriptorCount=1;primaryWrites[2].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;primaryWrites[2].pImageInfo=&primaryPosition;
    vkUpdateDescriptorSets(g_Device,3,primaryWrites,0,nullptr);
    VkDescriptorBufferInfo tileInfo{frame.activeTiles.GetBuffer(),0,VK_WHOLE_SIZE};
    VkWriteDescriptorSet tileWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    tileWrite.dstSet=frame.descriptor;tileWrite.dstBinding=36;tileWrite.descriptorCount=1;
    tileWrite.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;tileWrite.pBufferInfo=&tileInfo;
    vkUpdateDescriptorSets(g_Device,1,&tileWrite,0,nullptr);
    if(frame.rtxdiEnabled){tileWrite.dstSet=frame.rtxdiDescriptor;tileWrite.dstBinding=12;vkUpdateDescriptorSets(g_Device,1,&tileWrite,0,nullptr);}
    if(reset && history.rayReconstruction)history.rayReconstruction->ResetHistory();
    if(reset && history.denoiser)history.denoiser->ResetHistory();
    if(reset && history.giDenoiser)history.giDenoiser->ResetHistory();
    if(reset && history.specularDenoiser)history.specularDenoiser->ResetHistory();
    history.transforms=std::move(nextTransforms);history.view=view;history.projection=projection;history.serial=serial;history.sceneSignature=signature;
    return true;
}
void RayTracingViewport::DenoiseAndComposite(VkCommandBuffer cmd,Frame& frame,ViewHistory& history,uint32_t slot,uint32_t viewSlot,
    const glm::mat4& view,const glm::mat4& projection,bool valid){
    const std::string timingPrefix="rt.view"+std::to_string(viewSlot)+".";
    frame.rrResolved=VK_NULL_HANDLE;
    const VulkanImage* guides[]={&frame.diffuse,&frame.viewZ,&frame.normal,&frame.motion,&frame.material,&frame.specular,&frame.specularMaterial,&frame.gi};
    VkImageMemoryBarrier barriers[8]{};
    for(uint32_t i=0;i<8;++i){
        auto& b=barriers[i];b={VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=guides[i]->GetImage();
        b.oldLayout=VK_IMAGE_LAYOUT_GENERAL;b.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    }
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,8,barriers);
    if(!valid){if(history.giDenoiser)history.giDenoiser->ResetHistory();if(history.specularDenoiser)history.specularDenoiser->ResetHistory();if(history.denoiser)history.denoiser->ResetHistory();history.serial=UINT64_MAX;return;}
    if(history.rayReconstruction){
        Core::VulkanGpuScope rrTiming(cmd,timingPrefix+"dlss_rr");
        if(!valid)history.rayReconstruction->ResetHistory();
        mikan::denoising::RayReconstructionFrame temporal;
        temporal.view=view;temporal.projection=projection;temporal.jitterUV=glm::vec2(frame.taaJitter);
        temporal.rayTracingSet=frame.descriptor;temporal.resetHistory=frame.taaReset;
        temporal.captureSerial=history.serial;temporal.captureViewSlot=viewSlot;
        auto texture=[](const VulkanImage& image,VkImageLayout layout){return mikan::denoising::Texture{image.GetImage(),image.GetView(),layout};};
        const auto read=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        mikan::denoising::RayReconstructionInputs input{texture(frame.output,VK_IMAGE_LAYOUT_GENERAL),
            texture(frame.diffuse,read),texture(frame.material,read),texture(frame.specular,read),texture(frame.specularMaterial,read),
            texture(frame.taaGuide,VK_IMAGE_LAYOUT_GENERAL),texture(frame.motion,read),texture(frame.viewZ,read),texture(frame.normal,read)};
        VkMemoryBarrier ready{VK_STRUCTURE_TYPE_MEMORY_BARRIER};ready.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;ready.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&ready,0,nullptr,0,nullptr);
        if(history.rayReconstruction->Record(cmd,temporal,input)){
            const auto rrOutput=history.rayReconstruction->Output();
            Core::PipelineCapture::GetInstance().Record(cmd,rrOutput.image,VK_FORMAT_R16G16B16A16_SFLOAT,
                rrOutput.layout,history.outputExtent.width,history.outputExtent.height,history.serial,viewSlot,
                "dlss-rr-resolved-hdr","DLSS RR Preset D (vendor evaluation)","hdr");
            frame.rrResolved=rrOutput.view;return;
        }
    }
    using namespace mikan::denoising;
    Texture raw{frame.diffuse.GetImage(),frame.diffuse.GetView(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},filtered=raw;
    if(history.denoiser){
        Core::VulkanGpuScope diffuseTiming(cmd,timingPrefix+"nrd_diffuse");
        mikan::denoising::Frame temporal;temporal.view=view;temporal.projection=projection;
        temporal.sourceExtent={frame.width,frame.height};temporal.frameSlot=slot;temporal.jitterNdc=glm::vec2(frame.taaJitter)*2.0f;temporal.resetHistory=frame.taaReset;
        DiffuseInputs in;in.radianceHitDistance=raw;
        in.depth={frame.viewZ.GetImage(),frame.viewZ.GetView(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        in.worldNormal={frame.normal.GetImage(),frame.normal.GetView(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        in.motion={frame.motion.GetImage(),frame.motion.GetView(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        in.linearViewDepth=true;in.unpackedWorldNormal=true;
        if(history.denoiser->Record(cmd,temporal,in))filtered=history.denoiser->Output();
    }
    Texture rawSpec{frame.specular.GetImage(),frame.specular.GetView(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},filteredSpec=rawSpec;
    if(history.specularDenoiser){
        Core::VulkanGpuScope specularTiming(cmd,timingPrefix+"nrd_specular");
        mikan::denoising::Frame temporal;temporal.view=view;temporal.projection=projection;
        temporal.sourceExtent={frame.width,frame.height};temporal.frameSlot=slot;temporal.jitterNdc=glm::vec2(frame.taaJitter)*2.0f;temporal.resetHistory=frame.taaReset;
        DiffuseInputs in;in.radianceHitDistance=rawSpec;
        in.depth={frame.viewZ.GetImage(),frame.viewZ.GetView(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        in.worldNormal={frame.normal.GetImage(),frame.normal.GetView(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        in.motion={frame.motion.GetImage(),frame.motion.GetView(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        in.linearViewDepth=true;in.unpackedWorldNormal=true;
        if(history.specularDenoiser->Record(cmd,temporal,in))filteredSpec=history.specularDenoiser->Output();
    }
    auto& capture=Core::PipelineCapture::GetInstance();
    if(filtered.image!=raw.image)capture.Record(cmd,filtered.image,VK_FORMAT_R16G16B16A16_SFLOAT,
        filtered.layout,frame.width,frame.height,history.serial,viewSlot,"nrd-diffuse-hdr","NRD REBLUR_DIFFUSE + denoiser_unpack.comp","hdr");
    if(filteredSpec.image!=rawSpec.image)capture.Record(cmd,filteredSpec.image,VK_FORMAT_R16G16B16A16_SFLOAT,
        filteredSpec.layout,frame.width,frame.height,history.serial,viewSlot,"nrd-specular-hdr","NRD REBLUR_SPECULAR + denoiser_unpack.comp","hdr");
    Core::VulkanGpuScope compositeTiming(cmd,timingPrefix+"composite");
    VkDescriptorImageInfo images[]={{VK_NULL_HANDLE,frame.output.GetView(),VK_IMAGE_LAYOUT_GENERAL},
        {fallbackSampler,filtered.view,filtered.layout},{fallbackSampler,frame.material.GetView(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {fallbackSampler,filteredSpec.view,filteredSpec.layout},{fallbackSampler,frame.specularMaterial.GetView(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet writes[5]{};
    for(uint32_t i=0;i<5;++i){
        writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[i].dstSet=frame.compositeDescriptor;writes[i].dstBinding=i;
        writes[i].descriptorCount=1;writes[i].descriptorType=i?VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;writes[i].pImageInfo=&images[i];
    }
    vkUpdateDescriptorSets(g_Device,5,writes,0,nullptr);
    VkMemoryBarrier ready{VK_STRUCTURE_TYPE_MEMORY_BARRIER};ready.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
    ready.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&ready,0,nullptr,0,nullptr);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,compositePipeline);
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,compositeLayout,0,1,&frame.compositeDescriptor,0,nullptr);
    vkCmdDispatch(cmd,(frame.width+7)/8,(frame.height+7)/8,1);
}
VkImageView RayTracingViewport::ResolveTAA(VkCommandBuffer cmd,Frame& frame,ViewHistory& history,bool valid){
    VkImageMemoryBarrier guide{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};guide.image=frame.taaGuide.GetImage();
    guide.oldLayout=VK_IMAGE_LAYOUT_GENERAL;guide.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    guide.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;guide.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    guide.srcQueueFamilyIndex=guide.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;guide.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&guide);
    if(frame.rrResolved){
        history.taaInitialized[0]=history.taaInitialized[1]=false;return frame.rrResolved;
    }
    if(valid&&history.superResolution){
        mikan::denoising::RayReconstructionFrame temporal;temporal.view=history.view;temporal.projection=history.projection;
        temporal.jitterUV=glm::vec2(frame.taaJitter);temporal.rayTracingSet=frame.descriptor;temporal.resetHistory=frame.taaReset;
        auto read=[](const VulkanImage& image){return mikan::denoising::Texture{image.GetImage(),image.GetView(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};};
        mikan::denoising::RayReconstructionInputs input{read(frame.output),read(frame.diffuse),read(frame.material),read(frame.specular),read(frame.specularMaterial),read(frame.taaGuide),read(frame.motion),read(frame.viewZ),read(frame.normal)};
        if(history.superResolution->Record(cmd,temporal,input)){history.taaInitialized[0]=history.taaInitialized[1]=false;return history.superResolution->Output().view;}
    }
    const char* taaOption=std::getenv("MIKAN_HWRT_TAA");history.taaEnabled=!taaOption||taaOption[0]!='0';
    if(!valid||!history.taaEnabled)return frame.output.GetView();
    if(!history.taaHistory[0].GetImage()){

        for(auto& image:history.taaHistory){
            if(!image.Create(frame.width,frame.height,VK_FORMAT_R16G16B16A16_SFLOAT,VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)||
                !image.CreateView(VK_FORMAT_R16G16B16A16_SFLOAT,VK_IMAGE_ASPECT_COLOR_BIT)){
                history.taaHistory[0].Cleanup();history.taaHistory[1].Cleanup();return frame.output.GetView();
            }
        }
        LOGI("[HardwareRT] TAA %s: 8-phase Halton jitter",history.taaEnabled?"enabled":"disabled");
    }

    const uint32_t previous=history.taaRead,current=1-previous;
    VkImageMemoryBarrier barriers[2]{};
    for(uint32_t i=0;i<2;++i){
        auto& b=barriers[i];b={VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=history.taaHistory[i].GetImage();
        b.oldLayout=history.taaInitialized[i]?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout=i==current?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask=history.taaInitialized[i]?(VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT):0;
        b.dstAccessMask=i==current?VK_ACCESS_SHADER_WRITE_BIT:VK_ACCESS_SHADER_READ_BIT;
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    }
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,2,barriers);
    VkDescriptorImageInfo images[]={
        {fallbackSampler,frame.output.GetView(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {fallbackSampler,history.taaHistory[previous].GetView(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {VK_NULL_HANDLE,history.taaHistory[current].GetView(),VK_IMAGE_LAYOUT_GENERAL},
        {fallbackSampler,frame.taaGuide.GetView(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet writes[4]{};
    for(uint32_t i=0;i<4;++i){
        writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[i].dstSet=frame.taaDescriptor;writes[i].dstBinding=i;
        writes[i].descriptorCount=1;writes[i].descriptorType=i==2?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;writes[i].pImageInfo=&images[i];
    }
    vkUpdateDescriptorSets(g_Device,4,writes,0,nullptr);
    struct Push {glm::vec4 jitter,options;};
    Push push{frame.taaJitter,glm::vec4(frame.taaReset||!history.taaInitialized[previous]?1.0f:0.0f,0,0,0)};
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,taaPipeline);
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,taaLayout,0,1,&frame.taaDescriptor,0,nullptr);
    vkCmdPushConstants(cmd,taaLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),&push);
    vkCmdDispatch(cmd,(frame.width+7)/8,(frame.height+7)/8,1);
    auto& finished=barriers[current];finished.oldLayout=VK_IMAGE_LAYOUT_GENERAL;finished.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    finished.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;finished.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&finished);
    history.taaInitialized[current]=true;history.taaRead=current;
    return history.taaHistory[current].GetView();
}
