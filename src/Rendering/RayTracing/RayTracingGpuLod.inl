// Included by RayTracingScene.cpp; resources are owned by its fenced view slots.
bool RayTracingScene::EnsureGpuLodPipeline(){
    if(lodPipeline)return true;
    VkDescriptorSetLayoutBinding bindings[2]{};
    for(uint32_t i=0;i<2;++i){bindings[i].binding=i;bindings[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;bindings[i].descriptorCount=1;bindings[i].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;}
    VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};set.bindingCount=2;set.pBindings=bindings;
    if(!lodSetLayout&&vkCreateDescriptorSetLayout(g_Device,&set,g_Allocator,&lodSetLayout)!=VK_SUCCESS)return false;
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT,0,40};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};layout.setLayoutCount=1;layout.pSetLayouts=&lodSetLayout;layout.pushConstantRangeCount=1;layout.pPushConstantRanges=&range;
    if(!lodPipelineLayout&&vkCreatePipelineLayout(g_Device,&layout,g_Allocator,&lodPipelineLayout)!=VK_SUCCESS)return false;
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,256};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pool.maxSets=128;pool.poolSizeCount=1;pool.pPoolSizes=&size;
    if(!lodDescriptorPool&&vkCreateDescriptorPool(g_Device,&pool,g_Allocator,&lodDescriptorPool)!=VK_SUCCESS)return false;
    const auto code=RendererUtils::ReadFile(EngineConfig::GetShaderPath("vox_rt_lod_select.comp.spv"));
    const auto shader=RendererUtils::CreateShaderModule(code,"vox_rt_lod_select.comp.spv");if(!shader)return false;
    VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};info.layout=lodPipelineLayout;
    info.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};info.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;info.stage.module=shader;info.stage.pName="main";
    const auto result=vkCreateComputePipelines(g_Device,VK_NULL_HANDLE,1,&info,g_Allocator,&lodPipeline);
    vkDestroyShaderModule(g_Device,shader,g_Allocator);return result==VK_SUCCESS;
}
bool RayTracingScene::PrepareGpuLodInstances(VkCommandBuffer cmd,Frame& frame,const glm::vec3& camera,
    bool uploadInstances,bool uploadChoices,bool& changed){
    changed=false;if(!mikan::rt::VoxLodEnabled()||frame.lodChoices.empty())return true;
    if(!uploadInstances&&!uploadChoices&&frame.lodCameraValid&&frame.lodCamera==camera)return true;
    if(!EnsureGpuLodPipeline())return false;
    Core::CpuStageTrace recordTrace("rt.lod_gpu_record");
    Core::VulkanGpuScope selectTrace(cmd,"rt.lod_gpu_select");
    const VkDeviceSize choiceBytes=frame.lodChoices.size()*sizeof(GpuLodChoice),instanceBytes=frame.cachedInstances.size()*sizeof(VkAccelerationStructureInstanceKHR);
    bool descriptorsChanged=false;
    if(frame.lodChoiceBuffer.GetSize()<choiceBytes){
        frame.lodChoiceBuffer.Cleanup();frame.lodChoiceUpload.Cleanup();
        if(!frame.lodChoiceBuffer.Create(choiceBytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)||
           !frame.lodChoiceUpload.Create(choiceBytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))return false;
        uploadChoices=true;descriptorsChanged=true;
    }
    if(frame.lodInstanceBuffer.GetSize()<instanceBytes){
        frame.lodInstanceBuffer.Cleanup();
        if(!frame.lodInstanceBuffer.Create(instanceBytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT|VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))return false;
        uploadInstances=true;descriptorsChanged=true;
    }
    if(!frame.lodDescriptor){
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};allocate.descriptorPool=lodDescriptorPool;allocate.descriptorSetCount=1;allocate.pSetLayouts=&lodSetLayout;
        if(vkAllocateDescriptorSets(g_Device,&allocate,&frame.lodDescriptor)!=VK_SUCCESS)return false;descriptorsChanged=true;
    }
    if(descriptorsChanged){
        VkDescriptorBufferInfo buffers[2]={{frame.lodChoiceBuffer.GetBuffer(),0,VK_WHOLE_SIZE},{frame.lodInstanceBuffer.GetBuffer(),0,VK_WHOLE_SIZE}};
        VkWriteDescriptorSet writes[2]{};
        for(uint32_t i=0;i<2;++i){writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[i].dstSet=frame.lodDescriptor;writes[i].dstBinding=i;writes[i].descriptorCount=1;writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[i].pBufferInfo=&buffers[i];}
        vkUpdateDescriptorSets(g_Device,2,writes,0,nullptr);
    }
    VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER};before.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT|VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_SHADER_READ_BIT;before.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_HOST_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&before,0,nullptr,0,nullptr);
    if(uploadChoices){frame.lodChoiceUpload.Write(frame.lodChoices.data(),choiceBytes);VkBufferCopy copy{0,0,choiceBytes};vkCmdCopyBuffer(cmd,frame.lodChoiceUpload.GetBuffer(),frame.lodChoiceBuffer.GetBuffer(),1,&copy);}
    if(uploadInstances){VkBufferCopy copy{0,0,instanceBytes};vkCmdCopyBuffer(cmd,frame.instances.GetBuffer(),frame.lodInstanceBuffer.GetBuffer(),1,&copy);}
    if(uploadChoices||uploadInstances){
        VkMemoryBarrier ready{VK_STRUCTURE_TYPE_MEMORY_BARRIER};ready.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;ready.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&ready,0,nullptr,0,nullptr);
    }
    const auto distances=mikan::rt::VoxLodDistances();
    struct Push {glm::vec4 camera,distances;uint32_t count;int32_t forceLevel;};
    static_assert(sizeof(Push)==40);
    const Push push{glm::vec4(camera,0),glm::vec4(distances[0],distances[1],distances[2],float(mikan::rt::kVoxLodMaxLevel)),uint32_t(frame.lodChoices.size()),mikan::rt::VoxLodForceLevel()};
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,lodPipeline);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,lodPipelineLayout,0,1,&frame.lodDescriptor,0,nullptr);
    vkCmdPushConstants(cmd,lodPipelineLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),&push);vkCmdDispatch(cmd,(push.count+63u)/64u,1,1);
    RayTracingInputBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT);
    frame.lodCamera=camera;frame.lodCameraValid=true;changed=true;return true;
}
bool RayTracingScene::UpdateGpuLodTlas(VkCommandBuffer cmd,Frame& frame,const glm::vec3& camera){
    bool changed=false;if(!PrepareGpuLodInstances(cmd,frame,camera,false,false,changed))return false;
    if(!changed)return true;
    Core::VulkanGpuScope refitTrace(cmd,"rt.lod_gpu_tlas_update");
    RayTracingBuildBarrier(cmd);
    VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};geometry.geometryType=VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.geometry.instances.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;geometry.geometry.instances.data.deviceAddress=frame.lodInstanceBuffer.GetDeviceAddress();
    VkAccelerationStructureBuildRangeInfoKHR range{uint32_t(frame.cachedInstances.size()),0,0,0};
    const auto flags=VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR|VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    return frame.tlas.RecordBuild(cmd,VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,{&geometry,1},{&range,1},flags,true);
}
