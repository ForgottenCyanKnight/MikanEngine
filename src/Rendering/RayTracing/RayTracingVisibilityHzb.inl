#include "Rendering/HiZHistory.h"
bool RayTracingViewport::CullVisibilityHzb(VkCommandBuffer cmd,Frame& frame,const std::vector<RayTracingHitInstance>& hits,
    const std::vector<VkDrawIndirectCommand>& commands,const std::vector<VkDrawIndexedIndirectCommand>& strips,bool strip,uint32_t width,uint32_t height){
    const char* enabled=std::getenv("MIKAN_VISIBILITY_HZB");if(!enabled||enabled[0]!='1')return true;
    if(!frame.visibilityHzb){frame.visibilityHzb=std::make_unique<HiZComputeShader>();if(!frame.visibilityHzb->Init(g_Device,g_PhysicalDevice,width,height))return false;}
    // Stochastic BLEND coverage is not a stable temporal occluder.
    const auto& instanceWords=frame.sceneInputs->tables->cachedInstances;
    for(size_t i=0;i+7u<instanceWords.size();i+=12u)if(instanceWords[i+6u]==1u&&(instanceWords[i+5u]&16u)!=0u)return true;
    const uint64_t revision=HiZHistory::OccluderRevision(g_SceneRenderer.GetRenderWorld())^frame.sceneInputs->version;
    if(!HiZHistory::CanReuse(frame.visibilityViewProjection,frame.visibilityHzbStable,revision,frame.visibilityHzbRevision,frame.visibilityHzbValid)||!frame.visibilityHzb->HasValidCullingData())return true;
    const uint32_t count=uint32_t(strip?strips.size():commands.size());if(!count)return true;
    Core::VulkanGpuScope timing(cmd,"visibility.hzb_cull");
    if(!visibilityHzbPipeline){
        VkDescriptorSetLayoutBinding bindings[]={{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},{1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},{2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
        VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};set.bindingCount=3;set.pBindings=bindings;
        if(vkCreateDescriptorSetLayout(g_Device,&set,g_Allocator,&visibilityHzbSetLayout)!=VK_SUCCESS)return false;
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT,0,80};VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};layoutInfo.setLayoutCount=1;layoutInfo.pSetLayouts=&visibilityHzbSetLayout;layoutInfo.pushConstantRangeCount=1;layoutInfo.pPushConstantRanges=&range;
        if(vkCreatePipelineLayout(g_Device,&layoutInfo,g_Allocator,&visibilityHzbLayout)!=VK_SUCCESS)return false;
        auto shader=RendererUtils::CreateShaderModule(RendererUtils::ReadFile(EngineConfig::GetShaderPath("visibility_hzb_cull.comp.spv")),"visibility_hzb_cull.comp.spv");if(!shader)return false;
        VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};info.layout=visibilityHzbLayout;info.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};info.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;info.stage.module=shader;info.stage.pName="main";
        auto result=vkCreateComputePipelines(g_Device,VK_NULL_HANDLE,1,&info,g_Allocator,&visibilityHzbPipeline);vkDestroyShaderModule(g_Device,shader,g_Allocator);if(result!=VK_SUCCESS)return false;
    }
    struct Bounds {glm::vec4 minimum,maximum;};std::vector<Bounds> bounds(count);
    for(uint32_t i=0;i<count;++i){const uint32_t instance=(strip?strips[i].firstInstance:commands[i].firstInstance)/256u;const auto& hit=hits[instance];bounds[i].minimum.w=-1;
        if(!hit.geometry||!hit.geometry->Source())continue;
        auto* source=hit.geometry->Source();glm::vec3 lo(std::numeric_limits<float>::max()),hi(-std::numeric_limits<float>::max());
        for(uint32_t corner=0;corner<8;++corner){glm::vec3 p((corner&1)?source->GetMaxBounds().x:source->GetMinBounds().x,(corner&2)?source->GetMaxBounds().y:source->GetMinBounds().y,(corner&4)?source->GetMaxBounds().z:source->GetMinBounds().z);
            p=glm::vec3(hit.model*glm::vec4(p,1));lo=glm::min(lo,p);hi=glm::max(hi,p);}
        bounds[i]={glm::vec4(lo,0),glm::vec4(hi,0)};
    }
    const char* statsOption=std::getenv("MIKAN_VISIBILITY_HZB_STATS");const bool stats=statsOption&&statsOption[0]=='1'&&VkDeviceSize(count)*(strip?20u:16u)+8u<=1024u*1024u;
    const VkDeviceSize bytes=bounds.size()*sizeof(Bounds)+16u;
    if(frame.visibilityHzbBounds.GetSize()<bytes){frame.visibilityHzbBounds.Cleanup();if(!frame.visibilityHzbBounds.Create(bytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))return false;}
    if(stats&&frame.visibilityHzbBounds.GetMappedPtr()){static uint32_t reports=0;if((reports++%60u)==0u)LOGI("[Visibility HZB] previous-slot culled draws=%u elements=%u currentCandidates=%u",*static_cast<const uint32_t*>(frame.visibilityHzbBounds.GetMappedPtr()),static_cast<const uint32_t*>(frame.visibilityHzbBounds.GetMappedPtr())[1],count);}
    frame.visibilityHzbBounds.Write(bounds.data(),bytes-16u,16u);
    if(stats)vkCmdFillBuffer(cmd,frame.visibilityRaw.GetBuffer(),VkDeviceSize(width)*height*24u+VkDeviceSize(count)*(strip?20u:16u),8u,0u);
    if(!frame.visibilityHzbDescriptor){VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};allocate.descriptorPool=pool;allocate.descriptorSetCount=1;allocate.pSetLayouts=&visibilityHzbSetLayout;if(vkAllocateDescriptorSets(g_Device,&allocate,&frame.visibilityHzbDescriptor)!=VK_SUCCESS)return false;}
    VkDescriptorBufferInfo buffers[]={{frame.visibilityHzbBounds.GetBuffer(),0,VK_WHOLE_SIZE},{frame.visibilityRaw.GetBuffer(),0,VK_WHOLE_SIZE}};
    VkDescriptorImageInfo image{frame.visibilityHzb->GetSampler(),frame.visibilityHzb->GetHiZTextureViewForCulling(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet writes[3]{};for(uint32_t i=0;i<3;++i){writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[i].dstSet=frame.visibilityHzbDescriptor;writes[i].dstBinding=i;writes[i].descriptorCount=1;writes[i].descriptorType=i==1?VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;if(i==1)writes[i].pImageInfo=&image;else writes[i].pBufferInfo=&buffers[i==0?0:1];}
    vkUpdateDescriptorSets(g_Device,3,writes,0,nullptr);
    VkMemoryBarrier ready{VK_STRUCTURE_TYPE_MEMORY_BARRIER};ready.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;ready.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_HOST_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&ready,0,nullptr,0,nullptr);
    struct Push {glm::mat4 clip;glm::uvec4 options;} push{frame.visibilityHzbClip,{count,width*height*6u,strip?5u:4u,frame.visibilityHzb->GetCullingMipLevels()|(stats?0x80000000u:0u)}};
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,visibilityHzbPipeline);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,visibilityHzbLayout,0,1,&frame.visibilityHzbDescriptor,0,nullptr);vkCmdPushConstants(cmd,visibilityHzbLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),&push);vkCmdDispatch(cmd,(count+63u)/64u,1,1);
    ready.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;ready.dstAccessMask=VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,0,1,&ready,0,nullptr,0,nullptr);
    if(stats){
        ready.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_SHADER_READ_BIT;ready.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&ready,0,nullptr,0,nullptr);
        VkBufferCopy copy{VkDeviceSize(width)*height*24u+VkDeviceSize(count)*(strip?20u:16u),0,8};vkCmdCopyBuffer(cmd,frame.visibilityRaw.GetBuffer(),frame.visibilityHzbBounds.GetBuffer(),1,&copy);
        ready.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;ready.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&ready,0,nullptr,0,nullptr);
    }
    return true;
}
