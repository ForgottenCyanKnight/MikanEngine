#include "Rendering/RayTracing/RayTracingViewport.h"
#include "Rendering/RayTracing/RayTracingQualityOptions.h"
#include "Rendering/VoxelDDARegistry.h"
#include "Core/PipelineCapture.h"
#include "Core/VulkanContext.h"
#include "Core/EngineConfig.h"
#include "Core/Log.h"
#include "Core/VulkanGpuProfiler.h"
#include <algorithm>
#include <cstring>
#include <cmath>
#include <cstdlib>

namespace {
bool RawLightingOutput(){
    // Raw inspection is opt-in; default output uses reconstruction.
    static const bool enabled=[](){const char* v=std::getenv("MIKAN_HWRT_RAW_OUTPUT");return v&&v[0]=='1';}();
    return enabled;
}
float FireflyClampScale(){
    static const float scale=[](){
        const char* v=std::getenv("MIKAN_HWRT_FIREFLY_SCALE");
        if(!v)return 1.0f;
        char* end=nullptr;float value=std::strtof(v,&end);
        return end!=v&&*end=='\0'&&std::isfinite(value)&&value>=0.0f?value:1.0f;
    }();
    return scale;
}
uint32_t& NeeDiffuseSamples(){
    static uint32_t samples=[](){
        const char* value=std::getenv("MIKAN_HWRT_FRESH_DIFFUSE_SPP");
        if(value && (!std::strcmp(value,"1")||!std::strcmp(value,"2")||!std::strcmp(value,"3")))return uint32_t(value[0]-'0');
        return 1u;
    }();
    return samples;
}
}
namespace mikan::rt {
bool& RestirPathTracingState(){
    static bool value=[](){const char* v=std::getenv("MIKAN_HWRT_RESTIR_PT");return v&&v[0]=='1';}();
    return value;
}
bool GetRestirPathTracing(){return RestirPathTracingState();}
void SetRestirPathTracing(bool enabled){
    if(enabled&&!RestirPathTracingState()&&!std::getenv("MIKAN_HWRT_RESTIR_TEMPORAL"))SetRestirTemporalReuse(true);
    RestirPathTracingState()=enabled;
}

uint32_t GetNeeDiffuseSamples(){return NeeDiffuseSamples();}
uint32_t GetNeeDirectSamples(){
    static const uint32_t overrideSamples=[](){
        const char* v=std::getenv("MIKAN_HWRT_NEE_DI_SPP");if(!v)return 0u;
        char* end=nullptr;const long n=std::strtol(v,&end,10);
        return uint32_t(end==v||*end!='\0'||(n!=1&&n!=2&&n!=3&&n!=4&&n!=8)?2:n);
    }();
    // Tier DI budget now equals the GI budget (perf 1, balanced 2, quality 3);
    // the env override still wins for A/B regressions.
    return overrideSamples?overrideSamples:GetNeeDiffuseSamples();
}
void SetNeeDiffuseSamples(uint32_t samples){
    if(samples==1u||samples==2u||samples==3u)NeeDiffuseSamples()=samples;
}
bool& RestirTemporalReuseState(){
    // Boot default from env; the control panel flips this at runtime.
    static const bool initial=[](){const char* v=std::getenv("MIKAN_HWRT_RESTIR_TEMPORAL");return v?v[0]=='1':GetRestirPathTracing();}();
    static bool value=initial;
    return value;
}
// Runtime temporal reuse follows the control panel state.
bool GetRestirTemporalReuse(){return RestirTemporalReuseState();}
void SetRestirTemporalReuse(bool enabled){RestirTemporalReuseState()=enabled;}
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
    const char* statsOption=std::getenv("MIKAN_VOX_QUAD_STATS");
    const bool queryStats=statsOption&&std::strcmp(statsOption,"1")==0;
    const VkDeviceSize primaryBytes=(VkDeviceSize(width)*height+(queryStats?6u:0u))*24u;
    const auto primaryMemory=(queryStats||std::getenv("MIKAN_DEBUG_PRIMARY_HITS"))?
        (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT):VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    if(primaryBytes>primaryLimits.limits.maxStorageBufferRange || !frame.primaryHits.Create(primaryBytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,primaryMemory))return false;
    LOGI("[HardwareRT GI] full-resolution combined DI/GI NRD: %ux%u",width,height);
    if(!frame.temporal.GetBuffer() && !frame.temporal.Create(sizeof(TemporalParameters),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))return false;
    return true;
}
bool RayTracingViewport::PrepareDenoising(Frame& frame,ViewHistory& history,const std::vector<RayTracingHitInstance>& hits,
    uint64_t serial,uint32_t width,uint32_t height,const glm::mat4& view,const glm::mat4& projection,uint32_t lightCount){
    uint64_t signature=1469598103934665603ull;
    auto mix=[&](uint64_t value){signature=(signature^value)*1099511628211ull;};
    // Changing tiers invalidates each viewport's reconstruction history.
    mix(mikan::rt::GetNeeDiffuseSamples());
    for(const auto& hit:hits){
        mix(hit.HistoryKey());mix(reinterpret_cast<uintptr_t>(hit.geometry.get()));mix(reinterpret_cast<uintptr_t>(hit.modelGeometry.get()));
        mix(reinterpret_cast<uintptr_t>(hit.emissiveView));mix(reinterpret_cast<uintptr_t>(hit.emissiveSampler));mix(uint64_t(hit.emissiveFormat));for(int c=0;c<3;++c){uint32_t bits;std::memcpy(&bits,&hit.emissiveFactor[c],4);mix(bits);}mix(reinterpret_cast<uintptr_t>(hit.albedoView));mix(reinterpret_cast<uintptr_t>(hit.mrView));mix(hit.materialFlags);
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
        history.lightingValid=false;history.ptRead=0;
        for(auto& b:history.ptHistory)b.Cleanup();
        for(auto& b:history.ptScratch)b.Cleanup();
        history.enhancedQueue.Cleanup();history.enhancedWork.Cleanup();history.enhancedDuplicates.Cleanup();
        history.ptValid=false;history.ptActive=false;
        for(auto& b:history.irradianceCache)b.Cleanup();
        history.irradianceLocks.Cleanup();history.irradianceValid=false;history.irradianceActive=false;
        history.irradianceRead=0;
        history.rayReconstruction.reset();history.superResolution.reset();history.rrAttempted=history.srAttempted=false;
        history.denoiser.reset();history.specularDenoiser.reset();history.attempted=false;history.width=width;history.height=height;history.slots=slots;reset=true;
    }
    // Sampling-cache validity is independent of denoiser history validity.
    // First use, interrupted views, camera cuts and resource changes reset above.
    if(!RawLightingOutput()&&!history.rrAttempted){
        history.rrAttempted=true;history.rayReconstruction=mikan::denoising::CreateDlssRayReconstruction();
        mikan::denoising::Device rrDevice{g_PhysicalDevice,g_Device,g_Allocator,g_QueueFamily,slots};
        if(history.rayReconstruction&&!history.rayReconstruction->Initialize(g_Instance,rrDevice,{width,height},setLayout,history.outputExtent)){
            history.rayReconstruction.reset();LOGW("[DLSS RR] initialization unavailable; retaining NRD/TAA");
        }
    }
    if(!RawLightingOutput()&&!history.rayReconstruction&&!history.srAttempted){
        history.srAttempted=true;history.superResolution=mikan::denoising::CreateDlssSuperResolution();
        mikan::denoising::Device srDevice{g_PhysicalDevice,g_Device,g_Allocator,g_QueueFamily,slots};
        if(history.superResolution&&!history.superResolution->Initialize(g_Instance,srDevice,{width,height},setLayout,history.outputExtent))history.superResolution.reset();
    }
    if(!RawLightingOutput()&&!history.attempted){
        history.attempted=true;history.denoiser=mikan::denoising::CreateDiffuseDenoiser(mikan::denoising::Backend::Nrd);
        mikan::denoising::Device device{g_PhysicalDevice,g_Device,g_Allocator,g_QueueFamily,std::max(1u,g_MainWindowData.ImageCount)};
        if(history.denoiser && !history.denoiser->Initialize(device,{width,height}))history.denoiser.reset();
        history.specularDenoiser=mikan::denoising::CreateDiffuseDenoiser(mikan::denoising::Backend::Nrd,mikan::denoising::Signal::Specular);
        if(history.specularDenoiser && !history.specularDenoiser->Initialize(device,{width,height}))history.specularDenoiser.reset();
        if(!history.specularDenoiser)LOGW("[HardwareRT] NRD specular unavailable: using raw reflections");
        if(!history.denoiser)LOGW("[HardwareRT GI] NRD unavailable: using the current raw current GI signal");
    }
    std::vector<glm::mat4> remapping;remapping.reserve(std::max<size_t>(hits.size(),1));
    std::unordered_map<uint64_t,glm::mat4> nextTransforms;
    for(const auto& hit:hits){
        const auto previous=history.transforms.find(hit.HistoryKey());
        remapping.push_back(!reset&&previous!=history.transforms.end()?previous->second*glm::inverse(hit.model):glm::mat4(1));
        nextTransforms.emplace(hit.HistoryKey(),hit.model);
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
    const char* taaSetting=std::getenv("MIKAN_HWRT_TAA");history.taaEnabled=!RawLightingOutput()&&(!taaSetting||taaSetting[0]!='0');
    const glm::vec2 jitter=(history.taaEnabled||history.rayReconstruction||history.superResolution)?glm::vec2(halton(phase,2)-.5f,halton(phase,3)-.5f)/glm::vec2(width,height):glm::vec2(0);
    const glm::vec2 previousJitter=reset?jitter:history.jitterUV;
    frame.taaJitter=glm::vec4(jitter,previousJitter);frame.taaReset=reset;
    // Cached secondary radiance depends on every occluder, not just the primary
    // surface. Conservatively invalidate GI reuse when any instance moves.
    uint64_t transportSignature=signature;
    for(const auto& hit:hits)for(int c=0;c<4;++c)for(int r=0;r<4;++r){
        uint32_t bits;std::memcpy(&bits,&hit.model[c][r],4);transportSignature=(transportSignature^bits)*1099511628211ull;
    }
    const bool transportChanged=transportSignature!=history.giTransportSignature;
    const bool giReset=reset||transportChanged;
    history.giTransportSignature=transportSignature;
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
    const bool pathTracingRequested=mikan::rt::GetRestirPathTracing();

    const bool freshDiffuse=true;
    const uint32_t freshSamples=mikan::rt::GetNeeDiffuseSamples();
    const uint32_t neeDiSamples=mikan::rt::GetNeeDirectSamples();
    static const bool neeR2=[](){const char* v=std::getenv("MIKAN_HWRT_NEE_SAMPLING");return v&&std::strcmp(v,"r2")==0;}();
    static const bool neeStrictShadow=[](){const char* v=std::getenv("MIKAN_HWRT_NEE_STRICT_SHADOW");return v&&v[0]=='1';}();
    static const bool neeLocalLights=[](){const char* v=std::getenv("MIKAN_HWRT_NEE_LIGHT_SELECTION");return v&&std::strcmp(v,"local")==0;}();
    static const bool neeEdgeBudget=[](){const char* v=std::getenv("MIKAN_HWRT_NEE_ADAPTIVE");return v&&std::strcmp(v,"edges")==0;}();
    static bool reportedNeeBudget=false;
    if(freshDiffuse&&!pathTracingRequested&&!reportedNeeBudget){
        LOGI("[HardwareRT NEE budgets] primary DI=%u GI=%u; sampling=%s; strict emitter shadow=%s; primary MIS uses sample counts",neeDiSamples,freshSamples,neeR2?"randomized R2":"white PRNG",neeStrictShadow?"ON":"OFF");
        LOGI("[HardwareRT NEE lights] selection=%s",neeLocalLights?"position/normal aware power mixture":([](){const char* v=std::getenv("MIKAN_HWRT_NEE_LIGHT_SELECTION");return !v||!*v||std::strncmp(v,"tree",4)==0;}()?"power-distance tree":"global power/area CDF"));
        LOGI("[HardwareRT NEE adaptive] geometry-edge GI extra paths=%s (max4, no stochastic stopping)",neeEdgeBudget?"ON":"OFF");
        reportedNeeBudget=true;
    }
    static uint32_t reportedFreshSamples=0;
    if(freshDiffuse&&!pathTracingRequested&&reportedFreshSamples!=freshSamples){
        LOGI("[HardwareRT NEE] tier=%s; ordinary surfaces and mirror-terminal GI: %u fresh diffuse paths; performance=1 balanced=2 quality=3, default=performance, no ReSTIR DI/GI",freshSamples>=3?"quality":(freshSamples==2?"balanced":"performance"),freshSamples);
        reportedFreshSamples=freshSamples;
    }
    const bool enhancedRequested=pathTracingRequested;
    bool pathTracing=pathTracingRequested&&!hits.empty()&&EnsureEnhancedPipelines();
    VkPhysicalDeviceProperties ptLimits{};vkGetPhysicalDeviceProperties(g_PhysicalDevice,&ptLimits);
    const VkDeviceSize ptBytes=VkDeviceSize(width)*height*160u;
    if(pathTracing&&(ptBytes>ptLimits.limits.maxStorageBufferRange||(enhancedRequested&&VkDeviceSize(width)*height*384u>ptLimits.limits.maxStorageBufferRange))){
        LOGW("[ReSTIR PT] viewport exceeds SSBO range; using NEE/MIS");pathTracing=false;
    }
    const VkDeviceSize requiredPtBytes=pathTracing?ptBytes:64u;
    // Shared view histories can still be referenced by another in-flight frame.
    // Wait only on the rare lazy allocation/profile upgrade, before destroying
    // buffers that were previously placed in a submitted descriptor set.
    bool ptStorageGrows=false;
    for(auto* bank:{&history.ptHistory[0],&history.ptHistory[1],&history.ptScratch[0],&history.ptScratch[1]})
        ptStorageGrows|=bank->GetSize()<requiredPtBytes;
    if(enhancedRequested&&pathTracing)
        ptStorageGrows|=history.enhancedWork.GetSize()<VkDeviceSize(width)*height*384u
            ||history.enhancedQueue.GetSize()<16u+VkDeviceSize(width)*height*12u
            ||history.enhancedDuplicates.GetSize()<VkDeviceSize(width)*height*44u;
    if(ptStorageGrows&&vkDeviceWaitIdle(g_Device)!=VK_SUCCESS)return false;
    for(auto* bank:{&history.ptHistory[0],&history.ptHistory[1],&history.ptScratch[0],&history.ptScratch[1]}){
        if(bank->GetSize()<requiredPtBytes){
            bank->Cleanup();
            if(!bank->Create(requiredPtBytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))return false;
            history.ptValid=false;
        }
    }
    const bool enhanced=pathTracing&&enhancedRequested;
    const VkDeviceSize workBytes=enhanced?VkDeviceSize(width)*height*3u*128u:128u;
    const VkDeviceSize queueBytes=enhanced?16u+VkDeviceSize(width)*height*3u*4u:16u;
    if(workBytes>ptLimits.limits.maxStorageBufferRange)return false;
    if(history.enhancedWork.GetSize()<workBytes){history.enhancedWork.Cleanup();
        if(!history.enhancedWork.Create(workBytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))return false;}
    if(history.enhancedQueue.GetSize()<queueBytes){history.enhancedQueue.Cleanup();
        if(!history.enhancedQueue.Create(queueBytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))return false;}
    // DupMap uint plus two exact 20-byte primary identity/barycentric banks.
    const VkDeviceSize duplicateBytes=enhanced?VkDeviceSize(width)*height*44u:4u;
    if(history.enhancedDuplicates.GetSize()<duplicateBytes){history.enhancedDuplicates.Cleanup();
        if(!history.enhancedDuplicates.Create(duplicateBytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))return false;}
    if(history.ptActive!=pathTracing){reset=true;history.ptValid=false;history.ptActive=pathTracing;}
    if(reset||transportChanged)history.ptValid=false;
    static const bool cacheRequested=[](){const char* v=std::getenv("MIKAN_HWRT_IRRADIANCE_CACHE");return v&&std::strcmp(v,"1")==0;}();
    const bool cacheActive=cacheRequested&&freshDiffuse&&!pathTracing&&profileMask==0.0f;
    if(cacheActive!=history.irradianceActive){history.irradianceValid=false;history.irradianceActive=cacheActive;}
    // Shader serial is 16-bit; clear at wrap so ancient untouched slots cannot revive.
    if(reset||transportChanged||(serial&65535u)==0u)history.irradianceValid=false;
    const VkDeviceSize cacheBytes=cacheActive?ViewHistory::irradianceBytes:64u;
    for(auto& bank:history.irradianceCache){if(bank.GetSize()<cacheBytes){
        bank.Cleanup();if(!bank.Create(cacheBytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))return false;
        history.irradianceValid=false;
    }}
    const VkDeviceSize lockBytes=cacheActive?(65536u+4u)*4u:4u;
    if(history.irradianceLocks.GetSize()<lockBytes){history.irradianceLocks.Cleanup();
        if(!history.irradianceLocks.Create(lockBytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))return false;}
    static const bool cacheStats=[](){const char* v=std::getenv("MIKAN_HWRT_IRRADIANCE_STATS");return v&&std::strcmp(v,"1")==0;}();
    if(cacheActive&&cacheStats){
        if(!frame.irradianceReadback.GetBuffer()&&!frame.irradianceReadback.Create(16,VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))return false;
        frame.irradianceReadback.Map();if(!frame.irradianceReadback.GetMappedPtr())return false;
        if(frame.irradianceStatsPending){const auto* c=static_cast<const uint32_t*>(frame.irradianceReadback.GetMappedPtr());
            LOGI("[Diffuse IC][stats] completed-slot queries=%u hits=%u refresh=%u writes=%u",c[0],c[1],c[2],c[3]);frame.irradianceStatsPending=false;}
    }
    static bool reportedCache=false;
    if(cacheActive&&!reportedCache){LOGI("[Diffuse IC] ACTIVE: NEE only, secondary diffuse E/pi, 65536 slots, cell=0.5, one writer/slot/frame, refresh=1/8, age<=64, EMA cap=32; approximate biased reuse");reportedCache=true;}
    frame.taaReset=reset;
    static const uint32_t referenceSamples=[](){
        const char* v=std::getenv("MIKAN_HWRT_REFERENCE_SPP");char* end=nullptr;
        const long n=v?std::strtol(v,&end,10):0;
        return uint32_t(v&&end!=v&&*end=='\0'&&n>=1&&n<=64?n:0);
    }();
    static const uint32_t sampleSeed=[](){
        const char* v=std::getenv("MIKAN_HWRT_SAMPLE_SEED");char* end=nullptr;
        const long n=v?std::strtol(v,&end,10):0;
        return uint32_t(v&&end!=v&&*end=='\0'&&n>=0&&n<=65535?n:0);
    }();
    TemporalParameters temporal{view,reset?projection*view:history.projection*history.view,
        reset?view:history.view,frame.taaJitter,
        glm::vec4(float(serial&65535),reset?1:0,stbnAvailable?1:0,0),
        glm::vec4(0,giReset?1:0,FireflyClampScale(),profileMask+(russianRoulette?32.0f:0.0f)+(primaryFourSamples?64.0f:0.0f)
            +(referenceSamples?16384.0f:0.0f)+32768.0f+float(referenceSamples*65536u)),
        glm::vec4(freshSamples==4?-4.0f:(freshSamples==1?-2.0f:-1.0f),float(neeDiSamples),
            float(uint32_t(neeR2)+2u*uint32_t(neeStrictShadow)+4u*uint32_t(neeLocalLights)+8u*uint32_t(neeEdgeBudget)),float(sampleSeed))};
    history.jitterUV=jitter;
    if(pathTracing){
        // Enhanced is the only built-in ReSTIR estimator.
        temporal.options.w=0;
        temporal.giOptions.x=0;
        temporal.giOptions.w=float(uint32_t(temporal.giOptions.w)|32768u);
        static const bool enhancedSpatial=[](){const char* v=std::getenv("MIKAN_HWRT_RESTIR_PT_SPATIAL");return !v||v[0]!='0';}();
        temporal.samplingParams=glm::vec4(4.0f,
            mikan::rt::GetRestirTemporalReuse()&&history.ptValid?1.0f:0.0f,float(uint32_t(enhancedSpatial)+2u*history.ptRead),float(sampleSeed));
    }
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
    VkWriteDescriptorSet primaryWrites[2]{};
    for(uint32_t i=0;i<2;++i){primaryWrites[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};primaryWrites[i].dstSet=frame.descriptor;primaryWrites[i].dstBinding=34+i;primaryWrites[i].descriptorCount=1;primaryWrites[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;primaryWrites[i].pBufferInfo=&primaryBuffers[i];}
    vkUpdateDescriptorSets(g_Device,2,primaryWrites,0,nullptr);
    VkDescriptorBufferInfo tileInfo{frame.activeTiles.GetBuffer(),0,VK_WHOLE_SIZE};
    VkWriteDescriptorSet tileWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    tileWrite.dstSet=frame.descriptor;tileWrite.dstBinding=36;tileWrite.descriptorCount=1;
    tileWrite.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;tileWrite.pBufferInfo=&tileInfo;
    vkUpdateDescriptorSets(g_Device,1,&tileWrite,0,nullptr);
    if(reset && history.rayReconstruction)history.rayReconstruction->ResetHistory();
    if(reset && history.denoiser)history.denoiser->ResetHistory();
    if(reset && history.specularDenoiser)history.specularDenoiser->ResetHistory();
    history.transforms=std::move(nextTransforms);history.view=view;history.projection=projection;history.serial=serial;history.sceneSignature=signature;
    return true;
}
void RayTracingViewport::DenoiseAndComposite(VkCommandBuffer cmd,Frame& frame,ViewHistory& history,uint32_t slot,uint32_t viewSlot,
    const glm::mat4& view,const glm::mat4& projection,bool valid){
    const std::string timingPrefix="rt.view"+std::to_string(viewSlot)+".";
    frame.rrResolved=VK_NULL_HANDLE;
    const VulkanImage* guides[]={&frame.diffuse,&frame.viewZ,&frame.normal,&frame.motion,&frame.material,&frame.specular,&frame.specularMaterial};
    VkImageMemoryBarrier barriers[7]{};
    for(uint32_t i=0;i<7;++i){
        auto& b=barriers[i];b={VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=guides[i]->GetImage();
        b.oldLayout=VK_IMAGE_LAYOUT_GENERAL;b.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    }
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,7,barriers);
    static const bool primarySunOnly=[] {const char* v=std::getenv("MIKAN_HWRT_PRIMARY_SUN_ONLY");return v&&(v[0]=='1'||v[0]=='2');}();
    if(primarySunOnly)return; // Deterministic direct sun is already in base HDR; no NRD or composite dispatch.
    if(!valid){if(history.specularDenoiser)history.specularDenoiser->ResetHistory();if(history.denoiser)history.denoiser->ResetHistory();history.serial=UINT64_MAX;return;}
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
    const char* taaOption=std::getenv("MIKAN_HWRT_TAA");history.taaEnabled=!RawLightingOutput()&&(!taaOption||taaOption[0]!='0');
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
