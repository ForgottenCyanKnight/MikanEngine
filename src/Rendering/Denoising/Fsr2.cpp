#include "Rendering/Denoising/Fsr2.h"
#include "Rendering/RendererBase.h"
#include "Core/EngineConfig.h"
#include "Core/Log.h"
#include <ffx_fsr2.h>
#include <vk/ffx_fsr2_vk.h>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <vector>

namespace mikan::denoising {
namespace {
class Fsr2 final : public IRayReconstruction {
    Device device{}; VkExtent2D size{};
    FfxFsr2Context context{}; bool created=false,reset=true,initialized=false;
    std::vector<char> scratch;
    VulkanImage output,depth,motion;
    VkDescriptorSetLayout setLayout=VK_NULL_HANDLE;
    VkPipelineLayout layout=VK_NULL_HANDLE;
    VkPipeline pipeline=VK_NULL_HANDLE;
    VkDescriptorPool pool=VK_NULL_HANDLE;
    VkSampler sampler=VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> sets; uint64_t frame=0;
    std::chrono::steady_clock::time_point previousTime{};
public:
    ~Fsr2() override {
        if(created)ffxFsr2ContextDestroy(&context);
        if(pipeline)vkDestroyPipeline(device.logical,pipeline,nullptr);
        if(layout)vkDestroyPipelineLayout(device.logical,layout,nullptr);
        if(pool)vkDestroyDescriptorPool(device.logical,pool,nullptr);
        if(setLayout)vkDestroyDescriptorSetLayout(device.logical,setLayout,nullptr);
        if(sampler)vkDestroySampler(device.logical,sampler,nullptr);
    }
    bool Initialize(VkInstance,const Device& d,VkExtent2D s,VkDescriptorSetLayout,VkExtent2D) override {
        device=d;size=s;
        auto image=[&](VulkanImage& i,VkFormat f){return i.Create(s.width,s.height,f,VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)&&i.CreateView(f,VK_IMAGE_ASPECT_COLOR_BIT);};
        if(!image(output,VK_FORMAT_R16G16B16A16_SFLOAT)||!image(depth,VK_FORMAT_R32_SFLOAT)||!image(motion,VK_FORMAT_R16G16_SFLOAT))return false;
        scratch.resize(ffxFsr2GetScratchMemorySizeVK(d.physical));
        FfxFsr2ContextDescription desc{};
        desc.flags=FFX_FSR2_ENABLE_HIGH_DYNAMIC_RANGE|FFX_FSR2_ENABLE_AUTO_EXPOSURE;
        desc.maxRenderSize={s.width,s.height};desc.displaySize={s.width,s.height};desc.device=ffxGetDeviceVK(d.logical);
        if(ffxFsr2GetInterfaceVK(&desc.callbacks,scratch.data(),scratch.size(),d.physical,vkGetDeviceProcAddr)!=FFX_OK)return false;
        if(ffxFsr2ContextCreate(&context,&desc)!=FFX_OK)return false;created=true;
        VkDescriptorSetLayoutBinding b[3]={{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},{1,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},{2,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
        VkDescriptorSetLayoutCreateInfo si{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};si.bindingCount=3;si.pBindings=b;
        if(vkCreateDescriptorSetLayout(d.logical,&si,nullptr,&setLayout)!=VK_SUCCESS)return false;
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,16};
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};li.setLayoutCount=1;li.pSetLayouts=&setLayout;li.pushConstantRangeCount=1;li.pPushConstantRanges=&push;
        if(vkCreatePipelineLayout(d.logical,&li,nullptr,&layout)!=VK_SUCCESS)return false;
        auto bytes=RendererUtils::ReadFile(EngineConfig::GetShaderPath("fsr2_prepare.comp.spv"));
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};mi.codeSize=bytes.size();mi.pCode=reinterpret_cast<const uint32_t*>(bytes.data());VkShaderModule module{};
        if(vkCreateShaderModule(d.logical,&mi,nullptr,&module)!=VK_SUCCESS)return false;
        VkComputePipelineCreateInfo pi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};pi.layout=layout;pi.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};pi.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;pi.stage.module=module;pi.stage.pName="main";
        auto result=vkCreateComputePipelines(d.logical,VK_NULL_HANDLE,1,&pi,nullptr,&pipeline);vkDestroyShaderModule(d.logical,module,nullptr);if(result!=VK_SUCCESS)return false;
        uint32_t count=std::max(1u,d.framesInFlight);
        VkDescriptorPoolSize ps[]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,count},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,count*2}};
        VkDescriptorPoolCreateInfo pc{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pc.maxSets=count;pc.poolSizeCount=2;pc.pPoolSizes=ps;
        if(vkCreateDescriptorPool(d.logical,&pc,nullptr,&pool)!=VK_SUCCESS)return false;
        sets.resize(count);std::vector<VkDescriptorSetLayout> layouts(count,setLayout);
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=pool;ai.descriptorSetCount=count;ai.pSetLayouts=layouts.data();if(vkAllocateDescriptorSets(d.logical,&ai,sets.data())!=VK_SUCCESS)return false;
        VkSamplerCreateInfo sc{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};sc.magFilter=sc.minFilter=VK_FILTER_NEAREST;sc.addressModeU=sc.addressModeV=sc.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if(vkCreateSampler(d.logical,&sc,nullptr,&sampler)!=VK_SUCCESS)return false;
        LOGI("[FSR2] official SDK 2.2.1 ready: %ux%u native AA, NRD input, sharpening=0.2",s.width,s.height);return true;
    }
    bool Record(VkCommandBuffer cmd,const RayReconstructionFrame& f,const RayReconstructionInputs& in) override {
        VkImageMemoryBarrier barriers[3]{};VulkanImage* images[]={&depth,&motion,&output};
        for(uint32_t i=0;i<3;++i){auto& b=barriers[i];b={VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=images[i]->GetImage();b.oldLayout=initialized?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED;b.newLayout=VK_IMAGE_LAYOUT_GENERAL;b.srcAccessMask=initialized?VK_ACCESS_SHADER_READ_BIT:0;b.dstAccessMask=VK_ACCESS_SHADER_WRITE_BIT;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};}
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,3,barriers);
        auto set=sets[frame++%sets.size()];
        VkDescriptorImageInfo info[]={{sampler,in.primaryMotionDepth.view,in.primaryMotionDepth.layout},{VK_NULL_HANDLE,depth.GetView(),VK_IMAGE_LAYOUT_GENERAL},{VK_NULL_HANDLE,motion.GetView(),VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet writes[3]{};for(uint32_t i=0;i<3;++i){auto& w=writes[i];w={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=set;w.dstBinding=i;w.descriptorCount=1;w.descriptorType=i?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w.pImageInfo=&info[i];}vkUpdateDescriptorSets(device.logical,3,writes,0,nullptr);
        glm::vec4 p(f.projection[2][2],f.projection[3][2],f.projection[2][3],f.projection[3][3]);
        vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,nullptr);vkCmdPushConstants(cmd,layout,VK_SHADER_STAGE_COMPUTE_BIT,0,16,&p);vkCmdDispatch(cmd,(size.width+7)/8,(size.height+7)/8,1);
        for(uint32_t i=0;i<2;++i){barriers[i].oldLayout=VK_IMAGE_LAYOUT_GENERAL;barriers[i].newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;barriers[i].srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barriers[i].dstAccessMask=VK_ACCESS_SHADER_READ_BIT;}
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,2,barriers);
        auto resource=[&](VkImage im,VkImageView v,VkFormat format,const wchar_t* name,FfxResourceStates state=FFX_RESOURCE_STATE_COMPUTE_READ){return ffxGetTextureResourceVK(&context,im,v,size.width,size.height,format,name,state);};
        FfxFsr2DispatchDescription dispatch{};dispatch.commandList=ffxGetCommandListVK(cmd);
        dispatch.color=resource(in.baseColor.image,in.baseColor.view,VK_FORMAT_R16G16B16A16_SFLOAT,L"NRD composite");dispatch.depth=resource(depth.GetImage(),depth.GetView(),VK_FORMAT_R32_SFLOAT,L"Depth");dispatch.motionVectors=resource(motion.GetImage(),motion.GetView(),VK_FORMAT_R16G16_SFLOAT,L"Motion");dispatch.output=resource(output.GetImage(),output.GetView(),VK_FORMAT_R16G16B16A16_SFLOAT,L"FSR2 output",FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        const auto now=std::chrono::steady_clock::now();
        float deltaMs=previousTime.time_since_epoch().count()?std::chrono::duration<float,std::milli>(now-previousTime).count():16.666667f;
        previousTime=now;
        dispatch.jitterOffset={-f.jitterUV.x*size.width,-f.jitterUV.y*size.height};dispatch.motionVectorScale={-float(size.width),-float(size.height)};dispatch.renderSize={size.width,size.height};dispatch.enableSharpening=true;dispatch.sharpness=.2f;dispatch.frameTimeDelta=std::clamp(deltaMs,1.0f,100.0f);dispatch.preExposure=1;dispatch.reset=reset||f.resetHistory;
        float a=f.projection[2][2],b=f.projection[3][2];dispatch.cameraNear=b/a;dispatch.cameraFar=b/(a+1.0f);dispatch.cameraFovAngleVertical=2.0f*std::atan(1.0f/std::abs(f.projection[1][1]));dispatch.viewSpaceToMetersFactor=1;
        auto result=ffxFsr2ContextDispatch(&context,&dispatch);
        // SDK leaves this output in its declared unordered-access state.
        barriers[2].oldLayout=VK_IMAGE_LAYOUT_GENERAL;barriers[2].newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;barriers[2].srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barriers[2].dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&barriers[2]);initialized=true;
        if(result!=FFX_OK){LOGW("[FSR2] dispatch failed: %d; using TAA",int(result));reset=true;return false;}
        if(reset)LOGI("[FSR2] first dispatch succeeded; replacing TAA after NRD");
        reset=false;return true;
    }
    Texture Output() const override {return {output.GetImage(),output.GetView(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};}
    void ResetHistory() override {reset=true;}
};
}
std::unique_ptr<IRayReconstruction> CreateFsr2(){const char* value=std::getenv("MIKAN_HWRT_FSR2");if(value&&value[0]=='0')return {};return std::make_unique<Fsr2>();}
}
