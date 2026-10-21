#include "Rendering/RayTracing/RayTracingViewport.h"
#include "Core/VulkanContext.h"
#include "Core/VulkanRayTracingDevice.h"
#include "Core/EngineConfig.h"
#include "Rendering/RayTracing/RayTracingQualityOptions.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include "Core/Log.h"
#include "Rendering/VoxelDDARegistry.h"
#include "Rendering/RayTracing/VoxBrick.h"
RayTracingViewport::RayTracingViewport()=default;
RayTracingViewport::~RayTracingViewport(){Cleanup();}

bool RayTracingViewport::Initialize(){
    if(pipeline)return true;if(!GetRayTracingDeviceCapabilities().rayQuery)return false;
    softwareQuadBvhEnabled=[] {const char* v=std::getenv("MIKAN_HWRT_SOFTWARE_QUAD_BVH");return v&&v[0]=='1';}();
    const char* quadAabbOption=std::getenv("MIKAN_VOX_HW_QUAD_AABB");
    const bool quadAabb=quadAabbOption&&std::strcmp(quadAabbOption,"1")==0;
    const char* brickOption=std::getenv("MIKAN_VOX_HW_BRICK_DDA");const bool brick=brickOption&&brickOption[0]=='1';
    const bool bitset=mikan::render::VoxelDDARegistry::BitsetEnabled();
    const bool naadf=mikan::render::VoxelDDARegistry::NaadfEnabled();
    const bool screenBounds=mikan::render::VoxelDDARegistry::ScreenBoundsEnabled();
    if(bitset&&(brick||quadAabb||softwareQuadBvhEnabled)){LOGE("Bitset DDA cannot be combined with brick/quad traversal switches");return false;}
    if(brick&&(quadAabb||softwareQuadBvhEnabled)){LOGE("Brick DDA cannot be combined with quad traversal switches");return false;}
    if(brick){const auto edge=mikan::rt::VoxBrickEdge();LOGI("[RT traversal] hardware brick AABB / local palette DDA (%ux%ux%u)",edge,edge,edge);}
    else LOGI("[RT traversal] %s",softwareQuadBvhEnabled?"software BVH / direct VOX quads":(quadAabb?"hardware AABB BVH / procedural VOX quads (experimental NEE/MIS path)":"hardware ray query / triangles"));
    voxelDDAEnabled=mikan::render::VoxelDDARegistry::BootEnabled();
    if(softwareQuadBvhEnabled)voxelDDAEnabled=false;
    if(voxelDDAEnabled&&!brick)LOGI("[RT traversal] whole-volume DDA; occupancy=%s; primary screen bounds=%s",naadf?"NAADF 4/4 nested empty cuboids":bitset?"bitset SSBO mips":"3D texture mips",screenBounds?"ON":"OFF");
    VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(g_PhysicalDevice,&properties);
    if(properties.limits.maxPerStageDescriptorStorageImages<9 || properties.limits.maxPerStageDescriptorStorageBuffers<25 || properties.limits.maxDescriptorSetStorageBuffers<25)return false;
    // 3 bindless model material tables x kModelTextureBindlessCapacity combined
    // samplers per stage. Combined image samplers count against BOTH the
    // sampler and sampled-image stage limits.
    if(properties.limits.maxPerStageDescriptorSamplers<3*kModelTextureBindlessCapacity || properties.limits.maxPerStageDescriptorSampledImages<3*kModelTextureBindlessCapacity){
        LOGW("[HardwareRT] per-stage sampler limits (%u/%u) too low for %u-slot bindless model texture tables",
            properties.limits.maxPerStageDescriptorSamplers,properties.limits.maxPerStageDescriptorSampledImages,unsigned(3*kModelTextureBindlessCapacity));
        return false;
    }
    {
        VkPhysicalDeviceDescriptorIndexingFeatures indexing{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};
        VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};features2.pNext=&indexing;
        vkGetPhysicalDeviceFeatures2(g_PhysicalDevice,&features2);
        if(!indexing.shaderSampledImageArrayNonUniformIndexing||!indexing.descriptorBindingPartiallyBound){
            LOGW("[HardwareRT] bindless model textures require nonuniform sampled-image indexing + partially bound sets");
            return false;
        }
    }
    VkFormatProperties format{};vkGetPhysicalDeviceFormatProperties(g_PhysicalDevice,VK_FORMAT_R16G16B16A16_SFLOAT,&format);
    if(!(format.optimalTilingFeatures&VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))return false;
    vkGetPhysicalDeviceFormatProperties(g_PhysicalDevice,VK_FORMAT_R32G32B32A32_SFLOAT,&format);
    if(!(format.optimalTilingFeatures&VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))return false;
    const VkDescriptorType types[]={VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER};
    VkDescriptorSetLayoutBinding bindings[61]{};
    for(uint32_t i=0;i<10;++i)bindings[i]={i,types[i],i==9?kModelTextureBindlessCapacity:1u,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};for(uint32_t i=10;i<15;++i)bindings[i]={i,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    bindings[15]={15,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    bindings[16]={16,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    bindings[17]={17,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    bindings[18]={18,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    bindings[19]={19,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    bindings[20]={20,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    bindings[21]={21,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,kModelTextureBindlessCapacity,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    bindings[22]={22,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    bindings[23]={23,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    bindings[26]={26,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    bindings[28]={28,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    bindings[29]={29,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    bindings[34]={34,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    bindings[35]={35,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    bindings[36]={36,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    bindings[38]={38,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,kModelTextureBindlessCapacity,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};


    bindings[39]={39,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,8,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};


    bindings[40]={40,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    bindings[41]={41,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    for(uint32_t i=42;i<53;++i)bindings[i]={i,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    if(!enhancedPairs.Create(16,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))return false;
    for(uint32_t i=53;i<58;++i)bindings[i]={i,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    bindings[58]={58,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,kModelTextureBindlessCapacity,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    bindings[59]={59,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    bindings[60]={60,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    // Sparse binding numbers preserve the shader ABI without legacy DI/GI/SDK slots.
    std::vector<VkDescriptorSetLayoutBinding> activeBindings;
    for(const auto& binding:bindings)if(binding.descriptorCount)activeBindings.push_back(binding);
    // Bindless tables are PARTIALLY_BOUND: per-frame writes cover only the
    // unique textures in use; unwritten slots are never accessed by shaders.
    std::vector<VkDescriptorBindingFlags> bindingFlags(activeBindings.size(),0);
    for(size_t i=0;i<activeBindings.size();++i)if(activeBindings[i].binding==9||activeBindings[i].binding==21||activeBindings[i].binding==38||activeBindings[i].binding==58)bindingFlags[i]=VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    flagsInfo.bindingCount=uint32_t(bindingFlags.size());flagsInfo.pBindingFlags=bindingFlags.data();
    set.pNext=&flagsInfo;
    set.bindingCount=uint32_t(activeBindings.size());set.pBindings=activeBindings.data();
    if(vkCreateDescriptorSetLayout(g_Device,&set,g_Allocator,&setLayout)!=VK_SUCCESS)return false;
    // Bindless accounting: every set claims 3 x kModelTextureBindlessCapacity
    // combined samplers (partially written per frame). Pool numbers are lazy on
    // desktop drivers; 48 sets is well above the per-frame reuse factor.
    VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,32},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1280},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,448},{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,48*4*kModelTextureBindlessCapacity},{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,96},{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,768}};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};poolInfo.maxSets=48;poolInfo.poolSizeCount=uint32_t(std::size(sizes));poolInfo.pPoolSizes=sizes;
    if(vkCreateDescriptorPool(g_Device,&poolInfo,g_Allocator,&pool)!=VK_SUCCESS){Cleanup();return false;}
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,128};
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};layoutInfo.setLayoutCount=1;layoutInfo.pSetLayouts=&setLayout;layoutInfo.pushConstantRangeCount=1;layoutInfo.pPushConstantRanges=&push;
    if(vkCreatePipelineLayout(g_Device,&layoutInfo,g_Allocator,&layout)!=VK_SUCCESS){Cleanup();return false;}
    // Official RGBA8 STBN slices packed frame-major. Upload once per viewport
    // owner, shared by all frame/view descriptor sets; no image conversion.
    constexpr size_t stbnBytes=128u*128u*64u*4u;
    auto noise=RendererUtils::ReadFile(EngineConfig::GetEngineTexturePath("stbn/stbn_vec2_128x128x64.rgba8.bin"));
    stbnAvailable=noise.size()==stbnBytes;
    const VkDeviceSize noiseBytes=stbnAvailable?stbnBytes:16;
    if(noiseBytes>properties.limits.maxStorageBufferRange ||
        !stbnSamples.Create(noiseBytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ||
        !stbnUpload.Create(noiseBytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)){Cleanup();return false;}
    if(stbnAvailable){stbnUpload.Write(noise.data(),noise.size());LOGI("[HardwareRT GI] STBN 128x128x64 enabled");}
    else{const uint32_t zero[4]={};stbnUpload.Write(zero,sizeof(zero));LOGW("[HardwareRT GI] STBN missing or invalid: using hash sampling");}
    const char* shaders[]={"vox_rt_realtime.comp.spv","vox_rt_sky.comp.spv","vox_rt_realtime.comp.spv","vox_rt_realtime.comp.spv"};
    VkPipeline* outputs[]={&pipeline,&skyPipeline,&balancedPipeline,&performancePipeline};
    auto optionEquals=[](const char* name,const char* value){const char* v=std::getenv(name);return v&&std::strcmp(v,value)==0;};
    const char* diEnv=std::getenv("MIKAN_HWRT_NEE_DI_SPP");char* diEnd=nullptr;
    const long diValue=diEnv?std::strtol(diEnv,&diEnd,10):3;
    const uint32_t diSamples=diEnv&&(diEnd==diEnv||*diEnd!='\0'||(diValue!=1&&diValue!=2&&diValue!=3&&diValue!=4&&diValue!=8))?3u:uint32_t(diValue);
    const char* strictEnv=std::getenv("MIKAN_HWRT_NEE_STRICT_SHADOW");
    VkPhysicalDeviceProperties sunDevice{};vkGetPhysicalDeviceProperties(g_PhysicalDevice,&sunDevice);
    const bool robustSunOrigin=sunDevice.vendorID==0x1002u;
    LOGI("[HardwareRT Sun] AMD robust shadow origin=%s; minimum offset=%.3f; deterministic sun retained",robustSunOrigin?"ON":"OFF",robustSunOrigin?.01:.001);
    uint32_t neeConstants[27]={uint32_t(optionEquals("MIKAN_HWRT_NEE_LIGHT_SELECTION","local")),
        uint32_t(optionEquals("MIKAN_HWRT_NEE_ADAPTIVE","edges")),uint32_t(optionEquals("MIKAN_HWRT_NEE_SAMPLING","r2")),
        uint32_t(strictEnv&&strictEnv[0]=='1'),diSamples,uint32_t(robustSunOrigin),
        0u,
        uint32_t(optionEquals("MIKAN_HWRT_DI_VARIANCE","1")&&optionEquals("MIKAN_HWRT_PROFILE_MASK","1")&&optionEquals("MIKAN_GPU_PROFILE","1")),
        0u,
        0u,
        uint32_t(optionEquals("MIKAN_HWRT_DI_VARIANCE","mis")&&optionEquals("MIKAN_HWRT_PROFILE_MASK","3")&&optionEquals("MIKAN_GPU_PROFILE","1")),uint32_t(voxelDDAEnabled),uint32_t(!optionEquals("MIKAN_HWRT_DDA_SKIP_EMPTY","0")),(optionEquals("MIKAN_HWRT_HYBRID","1")&&optionEquals("MIKAN_GPU_PROFILE","1"))?4u:(optionEquals("MIKAN_HWRT_HYBRID_SUN","1")&&optionEquals("MIKAN_HWRT_PRIMARY_SUN_ONLY","1")&&optionEquals("MIKAN_GPU_PROFILE","1"))?3u:optionEquals("MIKAN_HWRT_PRIMARY_SUN_ONLY","1")?1u:(optionEquals("MIKAN_HWRT_PRIMARY_SUN_ONLY","2")?2u:0u),uint32_t(softwareQuadBvhEnabled),0u,uint32_t(optionEquals("MIKAN_VOX_HW_QUAD_AABB","1")),uint32_t(optionEquals("MIKAN_VOX_QUAD_STATS","1")),uint32_t(!optionEquals("MIKAN_SOFT_QUAD_COMPACT","0")),uint32_t(optionEquals("MIKAN_HWRT_IRRADIANCE_CACHE","1")),uint32_t(optionEquals("MIKAN_HWRT_IRRADIANCE_STATS","1"))};
    neeConstants[11]=uint32_t(voxelDDAEnabled&&!brick);neeConstants[24]=uint32_t(brick);
    neeConstants[25]=uint32_t(bitset);neeConstants[26]=uint32_t(screenBounds);
    neeConstants[9]=uint32_t(naadf);
    neeConstants[8]=uint32_t(screenBounds&&optionEquals("MIKAN_HWRT_DDA_SECONDARY_SCREEN_BOUNDS","1"));
    if(neeConstants[8])LOGI("[HardwareRT DDA] conservative screen-cone rejection for shadow/GI/reflection rays enabled");
    const auto brickEdge=mikan::rt::VoxBrickEdge();neeConstants[6]=brickEdge==8?3u:(brickEdge==4?2u:1u);
    LOGI("[HardwareRT NEE] light proposal=%s; exact primitive emitter index enabled",neeConstants[0]?"local":"cdf");
    if(neeConstants[7])LOGW("[HardwareRT DI variance] measurement only: primary emitter MIS disabled; use DI-only mask and fixed camera");
    if(neeConstants[10])LOGW("[HardwareRT DI variance] measurement only: complete primary emitter MIS; use DI/GI mask=3; excludes sky and secondary lighting");
    if(neeConstants[13]==4u)LOGI("[Hybrid Full] shared complete lighting; MRT primary surfaces and denoiser guides");
    if(neeConstants[13]>0u&&neeConstants[13]<4u)LOGW("[HardwareRT primary-only] mode=%u; no DI/GI/GGX or mirror continuation; NRD/composite bypass; TAA retained",neeConstants[13]);
    VkSpecializationMapEntry neeEntries[27]{};
    for(uint32_t j=0;j<27;++j)neeEntries[j]={j,j*uint32_t(sizeof(uint32_t)),sizeof(uint32_t)};
    VkSpecializationInfo neeSpecialization{27,neeEntries,sizeof(neeConstants),neeConstants};
    std::copy_n(std::begin(neeConstants),ptSpecialization.size(),ptSpecialization.begin());
    ptSpecialization[15]=1u;ptSpecialization[13]=neeConstants[13]==4u?4u:0u;
    for(uint32_t i=0;i<4;++i){
        // Tier variants (no env override): quality pipeline DI=3, balanced DI=2, performance DI=1.
        if(i==2)neeConstants[4]=diEnv?diSamples:2u;
        if(i==3)neeConstants[4]=diEnv?diSamples:1u;
        auto code=RendererUtils::ReadFile(EngineConfig::GetShaderPath(shaders[i]));auto shader=RendererUtils::CreateShaderModule(code,shaders[i]);if(!shader){Cleanup();return false;}
        VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};info.layout=layout;
        info.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};info.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;info.stage.module=shader;info.stage.pName="main";
        if(i!=1)info.stage.pSpecializationInfo=&neeSpecialization;
        auto result=vkCreateComputePipelines(g_Device,VK_NULL_HANDLE,1,&info,g_Allocator,outputs[i]);vkDestroyShaderModule(g_Device,shader,g_Allocator);
        if(result!=VK_SUCCESS){Cleanup();return false;}
    }if(!InitializeComposite()){Cleanup();return false;}
    voxelDDAEnabled=mikan::render::VoxelDDARegistry::BootEnabled();
    if(voxelDDAEnabled){
        // 4x4x4 R8_UINT stand-in bound to unused grid descriptor slots.
        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.imageType=VK_IMAGE_TYPE_3D;imageInfo.format=VK_FORMAT_R8_UINT;
        imageInfo.extent={4,4,4};imageInfo.mipLevels=1;imageInfo.arrayLayers=1;
        imageInfo.samples=VK_SAMPLE_COUNT_1_BIT;imageInfo.tiling=VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage=VK_IMAGE_USAGE_SAMPLED_BIT;imageInfo.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
        if(vkCreateImage(g_Device,&imageInfo,g_Allocator,&ddaDummyImage)==VK_SUCCESS){
            VkMemoryRequirements requirements{};vkGetImageMemoryRequirements(g_Device,ddaDummyImage,&requirements);
            VkMemoryAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocateInfo.allocationSize=requirements.size;
            allocateInfo.memoryTypeIndex=RendererUtils::FindMemoryType(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if(vkAllocateMemory(g_Device,&allocateInfo,g_Allocator,&ddaDummyMemory)==VK_SUCCESS &&
               vkBindImageMemory(g_Device,ddaDummyImage,ddaDummyMemory,0)==VK_SUCCESS){
                VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                viewInfo.image=ddaDummyImage;viewInfo.viewType=VK_IMAGE_VIEW_TYPE_3D;
                viewInfo.format=VK_FORMAT_R8_UINT;viewInfo.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
                vkCreateImageView(g_Device,&viewInfo,g_Allocator,&ddaDummyView);
            }
        }
        if(!ddaDummyView){Cleanup();return false;}
        LOGI("[HardwareRT] voxel grid textures enabled; traversal=%s",brick?"brick local DDA":"whole-volume DDA");
    }
    return true;
}

bool RayTracingViewport::EnsureEnhancedPipelines(){
    constexpr VkDeviceSize bytes=3u*256u*256u*sizeof(uint32_t);
    if(enhancedPairs.GetSize()<bytes){
        auto packed=RendererUtils::ReadFile(EngineConfig::GetEngineTexturePath("restir/paired-neighbors-3-16.u32.bin"));
        if(packed.size()!=bytes){LOGE("[ReSTIR Enhanced] missing reciprocal LUT");return false;}
        enhancedPairs.Cleanup();
        if(!enhancedPairs.Create(bytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))return false;
        enhancedPairs.Write(packed.data(),packed.size());
    }
    if(enhancedPipelines.back())return true;
    const char* name="vox_rt_enhanced.comp.spv";
    auto code=RendererUtils::ReadFile(EngineConfig::GetShaderPath(name));auto shader=RendererUtils::CreateShaderModule(code,name);if(!shader)return false;
    std::array<uint32_t,27> constants{};std::copy(ptSpecialization.begin(),ptSpecialization.end(),constants.begin());constants[21]=1u;
    const char* duplicate=std::getenv("MIKAN_HWRT_RESTIR_PT_DUPLICATE_CAP");constants[23]=!duplicate||duplicate[0]!='0';
    VkSpecializationMapEntry entries[27]{};for(uint32_t i=0;i<27;++i)entries[i]={i,i*uint32_t(sizeof(uint32_t)),sizeof(uint32_t)};
    VkSpecializationInfo specialization{27,entries,sizeof(constants),constants.data()};
    bool ok=true;
    for(uint32_t i=0;i<enhancedPipelines.size();++i){
        if(enhancedPipelines[i])continue;constants[22]=i;
        VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};info.layout=layout;
        info.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};info.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;
        info.stage.module=shader;info.stage.pName="main";info.stage.pSpecializationInfo=&specialization;
        LOGI("[ReSTIR Enhanced] creating stage %u/10",i+1u);
        if(vkCreateComputePipelines(g_Device,VK_NULL_HANDLE,1,&info,g_Allocator,&enhancedPipelines[i])!=VK_SUCCESS){ok=false;break;}
    }
    vkDestroyShaderModule(g_Device,shader,g_Allocator);return ok;
}

bool RayTracingViewport::EnsureFrame(Frame& frame,uint32_t width,uint32_t height){
    if(!frame.descriptor){VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};alloc.descriptorPool=pool;alloc.descriptorSetCount=1;alloc.pSetLayouts=&setLayout;if(vkAllocateDescriptorSets(g_Device,&alloc,&frame.descriptor)!=VK_SUCCESS)return false;}
    if(frame.width!=width || frame.height!=height){
        frame.output.Cleanup();frame.initialized=false;frame.width=frame.height=0;
        if(!frame.output.Create(width,height,VK_FORMAT_R16G16B16A16_SFLOAT,VK_IMAGE_TILING_OPTIMAL,VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) || !frame.output.CreateView(VK_FORMAT_R16G16B16A16_SFLOAT,VK_IMAGE_ASPECT_COLOR_BIT))return false;
        if(!EnsureDenoisingFrame(frame,width,height))return false;frame.width=width;frame.height=height;
    }return true;
}
bool RayTracingViewport::Upload(VulkanBuffer& buffer,std::vector<uint32_t>& cached,const std::vector<uint32_t>& data){
    if(data==cached && buffer.GetBuffer())return true;
    const VkDeviceSize bytes=std::max<size_t>(data.size()*4,16);
    VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(g_PhysicalDevice,&properties);if(bytes>properties.limits.maxStorageBufferRange)return false;
    if(buffer.GetSize()<bytes){buffer.Cleanup();if(!buffer.Create(bytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))return false;}
    if(!data.empty())buffer.Write(data.data(),data.size()*4);cached=data;return true;
}
bool RayTracingViewport::Upload(SharedInputBuffer& buffer,std::vector<uint32_t>& cached,const std::vector<uint32_t>& data){
    if(data==cached && buffer.GetBuffer())return true;
    // Never overwrite an allocation referenced by submitted frames. Unchanged
    // tables retain the previous version's shared allocation; changed tables detach.
    buffer.resource=std::make_shared<VulkanBuffer>();
    return Upload(*buffer.resource,cached,data);
}
void RayTracingViewport::Cleanup(){
    if(const char* dump=std::getenv("MIKAN_DEBUG_PRIMARY_HITS");dump&&g_Device){
        vkDeviceWaitIdle(g_Device);
        for(const auto& [key,frame]:frames)if(frame->primaryHits.GetMemory()){
            void* data=nullptr;if(vkMapMemory(g_Device,frame->primaryHits.GetMemory(),0,frame->primaryHits.GetSize(),0,&data)!=VK_SUCCESS)continue;
            std::ofstream out(std::string(dump)+"_"+std::to_string(key)+".bin",std::ios::binary);
            out.write(static_cast<const char*>(data),std::streamsize(frame->primaryHits.GetSize()));
            vkUnmapMemory(g_Device,frame->primaryHits.GetMemory());
        }
        if(latestSceneInputs){auto write=[&](const char* suffix,const std::vector<uint32_t>& words){std::ofstream out(std::string(dump)+suffix,std::ios::binary);out.write(reinterpret_cast<const char*>(words.data()),std::streamsize(words.size()*4));};
            write("_ranges.bin",latestSceneInputs->cachedRanges);write("_instances.bin",latestSceneInputs->cachedInstances);write("_quads.bin",latestSceneInputs->cachedQuads);write("_materials.bin",latestSceneInputs->cachedQuadMaterials);}
    }
    if(const char* stats=std::getenv("MIKAN_VOX_QUAD_STATS");stats&&std::strcmp(stats,"1")==0&&g_Device){
        vkDeviceWaitIdle(g_Device);
        for(const auto& [key,frame]:frames)if(frame->primaryHits.GetMemory()){
            void* data=nullptr;if(vkMapMemory(g_Device,frame->primaryHits.GetMemory(),0,frame->primaryHits.GetSize(),0,&data)!=VK_SUCCESS)continue;
            const auto* records=reinterpret_cast<const uint32_t*>(static_cast<const char*>(data)+VkDeviceSize(frame->width)*frame->height*24u);
            for(uint32_t category=0;category<3;++category){const auto* totals=records+category*6;const auto* maximum=records+(category+3)*6;
                LOGI("[Quad query stats] slot=%llu category=%u rays=%u candidates=%u accepted=%u maxCandidates=%u raysWithCandidates=%u",
                    (unsigned long long)key,category,totals[0],totals[1],totals[2],maximum[0],maximum[1]);}
            vkUnmapMemory(g_Device,frame->primaryHits.GetMemory());
        }
    }
    histories.clear();frames.clear();sceneInputs.clear();latestSceneInputs.reset();nextSceneInputVersion=0;
    if(g_Device){
        if(taaPipeline)vkDestroyPipeline(g_Device,taaPipeline,g_Allocator);
        if(taaLayout)vkDestroyPipelineLayout(g_Device,taaLayout,g_Allocator);
        if(taaSetLayout)vkDestroyDescriptorSetLayout(g_Device,taaSetLayout,g_Allocator);
    }
    taaPipeline=VK_NULL_HANDLE;taaLayout=VK_NULL_HANDLE;taaSetLayout=VK_NULL_HANDLE;
    if(g_Device){
        if(compositePipeline)vkDestroyPipeline(g_Device,compositePipeline,g_Allocator);
        if(compositeLayout)vkDestroyPipelineLayout(g_Device,compositeLayout,g_Allocator);
        if(compositeSetLayout)vkDestroyDescriptorSetLayout(g_Device,compositeSetLayout,g_Allocator);
    }
    compositePipeline=VK_NULL_HANDLE;compositeLayout=VK_NULL_HANDLE;compositeSetLayout=VK_NULL_HANDLE;
    stbnSamples.Cleanup();stbnUpload.Cleanup();enhancedPairs.Cleanup();stbnAvailable=false;stbnUploaded=false;
    for(auto& p:enhancedPipelines){if(p&&g_Device)vkDestroyPipeline(g_Device,p,g_Allocator);p=VK_NULL_HANDLE;}


    if(ddaDummyView){vkDestroyImageView(g_Device,ddaDummyView,g_Allocator);ddaDummyView=VK_NULL_HANDLE;}


    if(ddaDummyImage){vkDestroyImage(g_Device,ddaDummyImage,g_Allocator);ddaDummyImage=VK_NULL_HANDLE;}


    if(ddaDummyMemory){vkFreeMemory(g_Device,ddaDummyMemory,g_Allocator);ddaDummyMemory=VK_NULL_HANDLE;}
    fallbackSky.Cleanup();fallbackIrradiance.Cleanup();fallbackInitialized=false;
    if(g_Device){if(fallbackSampler)vkDestroySampler(g_Device,fallbackSampler,g_Allocator);if(performancePipeline)vkDestroyPipeline(g_Device,performancePipeline,g_Allocator);if(balancedPipeline)vkDestroyPipeline(g_Device,balancedPipeline,g_Allocator);if(skyPipeline)vkDestroyPipeline(g_Device,skyPipeline,g_Allocator);if(pipeline)vkDestroyPipeline(g_Device,pipeline,g_Allocator);if(layout)vkDestroyPipelineLayout(g_Device,layout,g_Allocator);if(pool)vkDestroyDescriptorPool(g_Device,pool,g_Allocator);if(setLayout)vkDestroyDescriptorSetLayout(g_Device,setLayout,g_Allocator);}
    ddaRegistryVersion=0;performancePipeline=VK_NULL_HANDLE;balancedPipeline=VK_NULL_HANDLE;fallbackSampler=VK_NULL_HANDLE;skyPipeline=VK_NULL_HANDLE;pipeline=VK_NULL_HANDLE;layout=VK_NULL_HANDLE;pool=VK_NULL_HANDLE;setLayout=VK_NULL_HANDLE;
}
