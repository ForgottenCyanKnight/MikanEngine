#include "Rendering/RayTracing/RayTracingViewport.h"
#include "Core/VulkanContext.h"
#include "Core/VulkanRayTracingDevice.h"
#include "Core/EngineConfig.h"
#include "Rendering/RayTracing/RayTracingQualityOptions.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include "Core/Log.h"
RayTracingViewport::RayTracingViewport()=default;
RayTracingViewport::~RayTracingViewport(){Cleanup();}

bool RayTracingViewport::Initialize(){
    if(pipeline)return true;if(!GetRayTracingDeviceCapabilities().rayQuery)return false;
    softwareQuadBvhEnabled=[] {const char* v=std::getenv("MIKAN_HWRT_SOFTWARE_QUAD_BVH");return v&&v[0]=='1';}();
    LOGI("[RT traversal] %s",softwareQuadBvhEnabled?"software BVH / direct VOX quads":"hardware ray query / triangles");
    voxelDDAEnabled=[] {const char* v=std::getenv("MIKAN_HWRT_VOXEL_DDA");return v&&v[0]=='1';}();
    if(softwareQuadBvhEnabled)voxelDDAEnabled=false;
    {
        VkPhysicalDeviceProperties vendorProps{};vkGetPhysicalDeviceProperties(g_PhysicalDevice,&vendorProps);
        const char* rtxdiEnv=std::getenv("MIKAN_HWRT_RTXDI");
        // Self ReSTIR is the default quality experiment; SDK is explicit opt-in.
        rtxdiWanted=!softwareQuadBvhEnabled&&!voxelDDAEnabled&&(vendorProps.vendorID==0x10DE||vendorProps.vendorID==0x1002)&&rtxdiEnv&&rtxdiEnv[0]=='1';
    }
    VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(g_PhysicalDevice,&properties);
    if(properties.limits.maxPerStageDescriptorStorageImages<10 || properties.limits.maxPerStageDescriptorStorageBuffers<19 || properties.limits.maxDescriptorSetStorageBuffers<19)return false;
    VkFormatProperties format{};vkGetPhysicalDeviceFormatProperties(g_PhysicalDevice,VK_FORMAT_R16G16B16A16_SFLOAT,&format);
    if(!(format.optimalTilingFeatures&VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))return false;
    vkGetPhysicalDeviceFormatProperties(g_PhysicalDevice,VK_FORMAT_R32G32B32A32_SFLOAT,&format);
    if(!(format.optimalTilingFeatures&VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))return false;
    const VkDescriptorType types[]={VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER};
    VkDescriptorSetLayoutBinding bindings[42]{};
    for(uint32_t i=0;i<10;++i)bindings[i]={i,types[i],i==9?8u:1u,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};for(uint32_t i=10;i<15;++i)bindings[i]={i,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    bindings[15]={15,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    bindings[16]={16,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    bindings[17]={17,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    bindings[18]={18,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    bindings[19]={19,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    bindings[20]={20,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    bindings[21]={21,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,8,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    bindings[22]={22,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    bindings[23]={23,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    bindings[24]={24,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    bindings[25]={25,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    bindings[26]={26,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    bindings[27]={27,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    bindings[28]={28,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    bindings[29]={29,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    bindings[30]={30,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    bindings[31]={31,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    bindings[32]={32,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    bindings[33]={33,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    bindings[34]={34,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    bindings[35]={35,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    bindings[36]={36,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    bindings[37]={37,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    bindings[38]={38,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,8,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};

    bindings[39]={39,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,8,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};

    bindings[40]={40,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    bindings[41]={41,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    set.bindingCount=uint32_t(std::size(bindings));set.pBindings=bindings;
    if(vkCreateDescriptorSetLayout(g_Device,&set,g_Allocator,&setLayout)!=VK_SUCCESS)return false;
    VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,32},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,512},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,448},{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,896},{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,96},{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,768}};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};poolInfo.maxSets=96;poolInfo.poolSizeCount=uint32_t(std::size(sizes));poolInfo.pPoolSizes=sizes;
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
    const char* treeSelection=std::getenv("MIKAN_HWRT_NEE_LIGHT_SELECTION");
    const bool primeDefault=!treeSelection||!*treeSelection;
    const char* diEnv=std::getenv("MIKAN_HWRT_NEE_DI_SPP");char* diEnd=nullptr;
    const long diValue=diEnv?std::strtol(diEnv,&diEnd,10):2;
    const uint32_t diSamples=diEnv&&(diEnd==diEnv||*diEnd!='\0'||(diValue!=1&&diValue!=2&&diValue!=4&&diValue!=8))?2u:uint32_t(diValue);
    const char* strictEnv=std::getenv("MIKAN_HWRT_NEE_STRICT_SHADOW");
    VkPhysicalDeviceProperties sunDevice{};vkGetPhysicalDeviceProperties(g_PhysicalDevice,&sunDevice);
    const bool robustSunOrigin=sunDevice.vendorID==0x1002u;
    LOGI("[HardwareRT Sun] AMD robust shadow origin=%s; minimum offset=%.3f; deterministic sun retained",robustSunOrigin?"ON":"OFF",robustSunOrigin?.01:.001);
    uint32_t neeConstants[]={uint32_t(optionEquals("MIKAN_HWRT_NEE_LIGHT_SELECTION","local")),
        uint32_t(optionEquals("MIKAN_HWRT_NEE_ADAPTIVE","edges")),uint32_t(optionEquals("MIKAN_HWRT_NEE_SAMPLING","r2")),
        uint32_t(strictEnv&&strictEnv[0]=='1'),diSamples,uint32_t(robustSunOrigin),
        uint32_t(primeDefault||optionEquals("MIKAN_HWRT_NEE_LIGHT_SELECTION","tree")||optionEquals("MIKAN_HWRT_NEE_LIGHT_SELECTION","tree_quality")||optionEquals("MIKAN_HWRT_NEE_LIGHT_SELECTION","tree_prime")),
        uint32_t(optionEquals("MIKAN_HWRT_DI_VARIANCE","1")&&optionEquals("MIKAN_HWRT_PROFILE_MASK","1")&&optionEquals("MIKAN_GPU_PROFILE","1")),
        uint32_t(primeDefault||optionEquals("MIKAN_HWRT_NEE_LIGHT_SELECTION","tree_quality")||optionEquals("MIKAN_HWRT_NEE_LIGHT_SELECTION","tree_prime")),
        uint32_t(primeDefault||optionEquals("MIKAN_HWRT_NEE_LIGHT_SELECTION","tree_prime")),
        uint32_t(optionEquals("MIKAN_HWRT_DI_VARIANCE","mis")&&optionEquals("MIKAN_HWRT_PROFILE_MASK","3")&&optionEquals("MIKAN_GPU_PROFILE","1")),uint32_t(voxelDDAEnabled),uint32_t(!optionEquals("MIKAN_HWRT_DDA_SKIP_EMPTY","0")),optionEquals("MIKAN_HWRT_PRIMARY_SUN_ONLY","1")?1u:(optionEquals("MIKAN_HWRT_PRIMARY_SUN_ONLY","2")?2u:0u),uint32_t(softwareQuadBvhEnabled)};
    LOGI("[HardwareRT NEE] light proposal=%s; exact primitive emitter index enabled",neeConstants[0]?"local":(neeConstants[6]?"tree":"cdf"));
    if(neeConstants[7])LOGW("[HardwareRT DI variance] measurement only: primary emitter MIS disabled; use DI-only mask and fixed camera");
    if(neeConstants[8])LOGI("[HardwareRT NEE] tree quality: exact leaf bounds distance with area floor");
    if(neeConstants[9])LOGI("[HardwareRT NEE] prime-inspired power centroid and internal power/distance proposal; 50%% global CDF support mixture");
    if(neeConstants[10])LOGW("[HardwareRT DI variance] measurement only: complete primary emitter MIS; use DI/GI mask=3; excludes sky and secondary lighting");
    if(neeConstants[13])LOGW("[HardwareRT primary-only] mode=%u; no DI/GI/GGX or mirror continuation; NRD/composite bypass; TAA retained",neeConstants[13]);
    VkSpecializationMapEntry neeEntries[15]{};
    for(uint32_t j=0;j<15;++j)neeEntries[j]={j,j*uint32_t(sizeof(uint32_t)),sizeof(uint32_t)};
    VkSpecializationInfo neeSpecialization{15,neeEntries,sizeof(neeConstants),neeConstants};
    for(uint32_t i=0;i<4;++i){
        if(i==2)neeConstants[4]=diEnv?diSamples:4u;
        if(i==3)neeConstants[4]=diEnv?diSamples:1u;
        auto code=RendererUtils::ReadFile(EngineConfig::GetShaderPath(shaders[i]));auto shader=RendererUtils::CreateShaderModule(code,shaders[i]);if(!shader){Cleanup();return false;}
        VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};info.layout=layout;
        info.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};info.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;info.stage.module=shader;info.stage.pName="main";
        if(i!=1)info.stage.pSpecializationInfo=&neeSpecialization;
        auto result=vkCreateComputePipelines(g_Device,VK_NULL_HANDLE,1,&info,g_Allocator,outputs[i]);vkDestroyShaderModule(g_Device,shader,g_Allocator);
        if(result!=VK_SUCCESS){Cleanup();return false;}
    }if(!InitializeComposite()){Cleanup();return false;}
    // RTXDI SDK passes (NVIDIA): enabled when the DXC-compiled SPIR-V is
    // present; otherwise the self-implemented ReSTIR / NEE paths serve every
    // adapter.
    if(rtxdiWanted){
        auto presampleCode=RendererUtils::ReadFile(EngineConfig::GetShaderPath("vox_rtxdi_presample.comp.spv"));
        auto initialCode=RendererUtils::ReadFile(EngineConfig::GetShaderPath("vox_rtxdi_initial.comp.spv"));
        auto stCode=RendererUtils::ReadFile(EngineConfig::GetShaderPath("vox_rtxdi_spatiotemporal.comp.spv"));
        if(presampleCode.size()>8 && initialCode.size()>8 && stCode.size()>8){
            VkDescriptorSetLayoutBinding rtxdiBindings[13]{};
            rtxdiBindings[0]={1,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
            rtxdiBindings[1]={2,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
            rtxdiBindings[2]={3,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
            rtxdiBindings[3]={4,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
            rtxdiBindings[4]={5,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
            rtxdiBindings[5]={6,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
            rtxdiBindings[6]={7,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
            rtxdiBindings[7]={8,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
            rtxdiBindings[8]={9,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
            rtxdiBindings[9]={10,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
            rtxdiBindings[11]={12,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
            rtxdiBindings[12]={13,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
            VkDescriptorSetLayoutCreateInfo rtxdiSet{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            rtxdiBindings[10]={11,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
            rtxdiSet.bindingCount=13;rtxdiSet.pBindings=rtxdiBindings;
            if(vkCreateDescriptorSetLayout(g_Device,&rtxdiSet,g_Allocator,&rtxdiSetLayout)==VK_SUCCESS){
                VkDescriptorPoolSize rtxdiSizes[]={{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,224},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,128},{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,32}};
                VkDescriptorPoolCreateInfo rtxdiPoolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
                rtxdiPoolInfo.maxSets=32;rtxdiPoolInfo.poolSizeCount=3;rtxdiPoolInfo.pPoolSizes=rtxdiSizes;
                if(vkCreateDescriptorPool(g_Device,&rtxdiPoolInfo,g_Allocator,&rtxdiPool)==VK_SUCCESS){
                    VkPipelineLayoutCreateInfo rtxdiLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
                    rtxdiLayoutInfo.setLayoutCount=1;rtxdiLayoutInfo.pSetLayouts=&rtxdiSetLayout;
                    if(vkCreatePipelineLayout(g_Device,&rtxdiLayoutInfo,g_Allocator,&rtxdiLayout)==VK_SUCCESS){
                        const char* rtxdiShaderNames[]={"vox_rtxdi_presample.comp.spv","vox_rtxdi_initial.comp.spv","vox_rtxdi_spatiotemporal.comp.spv"};
                        std::vector<char>* rtxdiCodes[]={&presampleCode,&initialCode,&stCode};
                        rtxdiAvailable=true;
                        for(uint32_t i=0;i<3;++i){
                            auto shader=RendererUtils::CreateShaderModule(*rtxdiCodes[i],rtxdiShaderNames[i]);
                            if(!shader){rtxdiAvailable=false;break;}
                            VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};info.layout=rtxdiLayout;
                            info.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};info.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;info.stage.module=shader;info.stage.pName="main";
                            if(vkCreateComputePipelines(g_Device,VK_NULL_HANDLE,1,&info,g_Allocator,&rtxdiPipelines[i])!=VK_SUCCESS){vkDestroyShaderModule(g_Device,shader,g_Allocator);rtxdiAvailable=false;break;}
                            vkDestroyShaderModule(g_Device,shader,g_Allocator);
                        }
                    }
                }
            }
            LOGI("[HardwareRT] RTXDI SDK passes %s (local-light POWER_RIS presampling)",rtxdiAvailable?"ready":"unavailable: using self-implemented ReSTIR");
        }
    }
    voxelDDAEnabled=[] {const char* v=std::getenv("MIKAN_HWRT_VOXEL_DDA");return v&&v[0]=='1';}();
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
        LOGI("[HardwareRT] voxel DDA traversal enabled (opt-in; MIKAN_HWRT_VOXEL_DDA=1 enables DDA, triangles are default)");
    }
    return true;
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
    LOGI("[RTXDI][dbg] A: enter");
    histories.clear();frames.clear();sceneInputs.clear();latestSceneInputs.reset();nextSceneInputVersion=0;
    LOGI("[RTXDI][dbg] B: frames cleared");
    if(g_Device){
        if(giUpsamplePipeline)vkDestroyPipeline(g_Device,giUpsamplePipeline,g_Allocator);
        if(taaPipeline)vkDestroyPipeline(g_Device,taaPipeline,g_Allocator);
        if(taaLayout)vkDestroyPipelineLayout(g_Device,taaLayout,g_Allocator);
        if(taaSetLayout)vkDestroyDescriptorSetLayout(g_Device,taaSetLayout,g_Allocator);
    }
    giUpsamplePipeline=VK_NULL_HANDLE;taaPipeline=VK_NULL_HANDLE;taaLayout=VK_NULL_HANDLE;taaSetLayout=VK_NULL_HANDLE;
    if(g_Device){
        if(compositePipeline)vkDestroyPipeline(g_Device,compositePipeline,g_Allocator);
        if(compositeLayout)vkDestroyPipelineLayout(g_Device,compositeLayout,g_Allocator);
        if(compositeSetLayout)vkDestroyDescriptorSetLayout(g_Device,compositeSetLayout,g_Allocator);
    }
    compositePipeline=VK_NULL_HANDLE;compositeLayout=VK_NULL_HANDLE;compositeSetLayout=VK_NULL_HANDLE;
    stbnSamples.Cleanup();stbnUpload.Cleanup();stbnAvailable=false;stbnUploaded=false;

    if(ddaDummyView){vkDestroyImageView(g_Device,ddaDummyView,g_Allocator);ddaDummyView=VK_NULL_HANDLE;}

    if(ddaDummyImage){vkDestroyImage(g_Device,ddaDummyImage,g_Allocator);ddaDummyImage=VK_NULL_HANDLE;}

    if(ddaDummyMemory){vkFreeMemory(g_Device,ddaDummyMemory,g_Allocator);ddaDummyMemory=VK_NULL_HANDLE;}
    LOGI("[RTXDI][dbg] C: before pipelines");
    for(auto& rtxdiPipeline:rtxdiPipelines)if(rtxdiPipeline)vkDestroyPipeline(g_Device,rtxdiPipeline,g_Allocator);
    LOGI("[RTXDI][dbg] D: after pipelines");
    for(auto& rtxdiPipelineHandle:rtxdiPipelines)rtxdiPipelineHandle=VK_NULL_HANDLE;
    if(rtxdiLayout)vkDestroyPipelineLayout(g_Device,rtxdiLayout,g_Allocator);
    if(rtxdiPool)vkDestroyDescriptorPool(g_Device,rtxdiPool,g_Allocator);
    if(rtxdiSetLayout)vkDestroyDescriptorSetLayout(g_Device,rtxdiSetLayout,g_Allocator);
    LOGI("[RTXDI][dbg] E: pool/layout destroyed");
    rtxdiLayout=VK_NULL_HANDLE;rtxdiPool=VK_NULL_HANDLE;rtxdiSetLayout=VK_NULL_HANDLE;rtxdiAvailable=false;
    LOGI("[RTXDI][dbg] F: rtxdi teardown done");
    fallbackSky.Cleanup();fallbackIrradiance.Cleanup();fallbackInitialized=false;
    if(g_Device){if(fallbackSampler)vkDestroySampler(g_Device,fallbackSampler,g_Allocator);if(performancePipeline)vkDestroyPipeline(g_Device,performancePipeline,g_Allocator);if(balancedPipeline)vkDestroyPipeline(g_Device,balancedPipeline,g_Allocator);if(skyPipeline)vkDestroyPipeline(g_Device,skyPipeline,g_Allocator);if(pipeline)vkDestroyPipeline(g_Device,pipeline,g_Allocator);if(layout)vkDestroyPipelineLayout(g_Device,layout,g_Allocator);if(pool)vkDestroyDescriptorPool(g_Device,pool,g_Allocator);if(setLayout)vkDestroyDescriptorSetLayout(g_Device,setLayout,g_Allocator);}
    ddaRegistryVersion=0;performancePipeline=VK_NULL_HANDLE;balancedPipeline=VK_NULL_HANDLE;fallbackSampler=VK_NULL_HANDLE;skyPipeline=VK_NULL_HANDLE;pipeline=VK_NULL_HANDLE;layout=VK_NULL_HANDLE;pool=VK_NULL_HANDLE;setLayout=VK_NULL_HANDLE;
}
