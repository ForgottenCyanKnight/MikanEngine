#include "Rendering/RayTracing/VoxRayTracingGeometry.h"
#include "Rendering/VoxRenderer.h"
#include "Core/VulkanContext.h"
#include "Core/VulkanRayTracingDevice.h"
#include "Core/EngineConfig.h"
#include <algorithm>
#include <limits>
bool VoxRayTracingConverter::Initialize() {
    if(pipeline)return true;
    if(!GetRayTracingDeviceCapabilities().accelerationStructure)return false;
    VkDescriptorSetLayoutBinding bindings[3]{};
    for(uint32_t i=0;i<3;++i)bindings[i]={i,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};set.bindingCount=3;set.pBindings=bindings;
    if(vkCreateDescriptorSetLayout(g_Device,&set,g_Allocator,&setLayout)!=VK_SUCCESS)return false;
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,3*512};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};poolInfo.flags=VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;poolInfo.maxSets=512;poolInfo.poolSizeCount=1;poolInfo.pPoolSizes=&size;
    if(vkCreateDescriptorPool(g_Device,&poolInfo,g_Allocator,&pool)!=VK_SUCCESS){Cleanup();return false;}
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,32};
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};layoutInfo.setLayoutCount=1;layoutInfo.pSetLayouts=&setLayout;layoutInfo.pushConstantRangeCount=1;layoutInfo.pPushConstantRanges=&push;
    if(vkCreatePipelineLayout(g_Device,&layoutInfo,g_Allocator,&layout)!=VK_SUCCESS){Cleanup();return false;}
    const auto code=RendererUtils::ReadFile(EngineConfig::GetShaderPath("vox_rt_expand.comp.spv"));
    const auto shader=RendererUtils::CreateShaderModule(code,"vox_rt_expand.comp");
    if(!shader){Cleanup();return false;}
    VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};info.layout=layout;
    info.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};info.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;info.stage.module=shader;info.stage.pName="main";
    const auto result=vkCreateComputePipelines(g_Device,VK_NULL_HANDLE,1,&info,g_Allocator,&pipeline);
    vkDestroyShaderModule(g_Device,shader,g_Allocator);if(result!=VK_SUCCESS){Cleanup();return false;}return true;
}
void VoxRayTracingConverter::Cleanup() {
    if(g_Device){if(pipeline)vkDestroyPipeline(g_Device,pipeline,g_Allocator);if(layout)vkDestroyPipelineLayout(g_Device,layout,g_Allocator);if(pool)vkDestroyDescriptorPool(g_Device,pool,g_Allocator);if(setLayout)vkDestroyDescriptorSetLayout(g_Device,setLayout,g_Allocator);}
    pipeline=VK_NULL_HANDLE;layout=VK_NULL_HANDLE;pool=VK_NULL_HANDLE;setLayout=VK_NULL_HANDLE;
}
VkDescriptorSet VoxRayTracingConverter::Allocate(VkBuffer quads,VkBuffer positions,VkBuffer indices) {
    if(!Initialize())return VK_NULL_HANDLE;
    VkDescriptorSet result=VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};alloc.descriptorPool=pool;alloc.descriptorSetCount=1;alloc.pSetLayouts=&setLayout;
    if(vkAllocateDescriptorSets(g_Device,&alloc,&result)!=VK_SUCCESS)return VK_NULL_HANDLE;
    VkDescriptorBufferInfo buffers[]={{quads,0,VK_WHOLE_SIZE},{positions,0,VK_WHOLE_SIZE},{indices,0,VK_WHOLE_SIZE}};
    VkWriteDescriptorSet writes[3]{};
    for(uint32_t i=0;i<3;++i){writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[i].dstSet=result;writes[i].dstBinding=i;writes[i].descriptorCount=1;writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[i].pBufferInfo=&buffers[i];}
    vkUpdateDescriptorSets(g_Device,3,writes,0,nullptr);return result;
}
void VoxRayTracingConverter::Free(VkDescriptorSet set){if(set && pool && g_Device)vkFreeDescriptorSets(g_Device,pool,1,&set);}
void VoxRayTracingConverter::Record(VkCommandBuffer cmd,VkDescriptorSet set,const VoxRenderer& renderer) {
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,nullptr);
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;host.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&host,0,nullptr,0,nullptr);
    VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(g_PhysicalDevice,&properties);
    const uint32_t batch=uint32_t(std::min(uint64_t(UINT32_MAX),uint64_t(properties.limits.maxComputeWorkGroupCount[0])*64));
    struct Push{glm::vec4 minSize;glm::uvec4 range;};static_assert(sizeof(Push)==32);
    for(const auto& group:renderer.GetMeshData().faceGroups) {
        uint32_t first=uint32_t(group.firstIndex/6),remaining=uint32_t(group.indexCount/6);
        while(remaining){const auto count=std::min(remaining,batch);Push push{glm::vec4(renderer.GetMinBounds(),renderer.GetVoxelSize()),glm::uvec4(first,count,group.faceDirection,0)};
            vkCmdPushConstants(cmd,layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),&push);vkCmdDispatch(cmd,(count+63)/64,1,1);first+=count;remaining-=count;}
    }
}
void VoxRayTracingGeometry::ReleaseBuildInputs() {
    if(buildSet && converter)converter->Free(buildSet);buildSet=VK_NULL_HANDLE;
    positions.Cleanup();indices.Cleanup();blas.ReleaseScratch();
}
bool VoxRayTracingGeometry::RecordBuild(VkCommandBuffer cmd,const VoxRenderer& renderer,VoxRayTracingConverter& decoder) {
    const auto& cap=GetRayTracingDeviceCapabilities();
    if(!cap.accelerationStructure || !renderer.HasValidQuads() || blas.Handle()){LOGE("[HardwareRT] vox BLAS precheck failed: quads=%zu valid=%d blas=%p",renderer.GetQuads().size(),renderer.HasValidQuads()?1:0,(void*)blas.Handle());return false;}
    VkFormatProperties format{};vkGetPhysicalDeviceFormatProperties(g_PhysicalDevice,VK_FORMAT_R32G32B32_SFLOAT,&format);
    if(!(format.bufferFeatures&VK_FORMAT_FEATURE_ACCELERATION_STRUCTURE_VERTEX_BUFFER_BIT_KHR)){LOGE("[HardwareRT] vox BLAS: R32G32B32 not AS-vertex capable");return false;}
    const VkDeviceSize count=renderer.GetQuads().size();
    VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(g_PhysicalDevice,&properties);
    if(count>UINT32_MAX/6 || count*48>properties.limits.maxStorageBufferRange || count*2>cap.limits.maxPrimitiveCount){LOGE("[HardwareRT] vox BLAS limits: quads=%zu maxPrim=%u maxSSBO=%llu",count,cap.limits.maxPrimitiveCount,(unsigned long long)properties.limits.maxStorageBufferRange);return false;}
    const auto host=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const auto input=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT|VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    if(!quads.Create(count*8,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,host) || !positions.Create(count*48,input,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) || !indices.Create(count*24,input,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)){LOGE("[HardwareRT] vox BLAS buffer create failed: quads=%zu",count);return false;}
    quads.Write(renderer.GetQuads().data(),count*8);converter=&decoder;
    buildSet=decoder.Allocate(quads.GetBuffer(),positions.GetBuffer(),indices.GetBuffer());if(!buildSet){LOGE("[HardwareRT] vox BLAS descriptor alloc failed");return false;}
    // Validate all build inputs before recording compute so failures can release resources safely.
    std::vector<VkAccelerationStructureGeometryKHR> geometries;
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> buildRanges;
    for(const auto& group:renderer.GetMeshData().faceGroups){if(!group.indexCount)continue;
        VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};geometry.geometryType=VK_GEOMETRY_TYPE_TRIANGLES_KHR;geometry.flags=VK_GEOMETRY_OPAQUE_BIT_KHR;
        auto& triangles=geometry.geometry.triangles;triangles.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        triangles.vertexFormat=VK_FORMAT_R32G32B32_SFLOAT;triangles.vertexData.deviceAddress=positions.GetDeviceAddress();triangles.vertexStride=12;triangles.maxVertex=uint32_t(count*4-1);
        triangles.indexType=VK_INDEX_TYPE_UINT32;triangles.indexData.deviceAddress=indices.GetDeviceAddress();
        if(!triangles.vertexData.deviceAddress || !triangles.indexData.deviceAddress)return false;
        geometries.push_back(geometry);buildRanges.push_back({uint32_t(group.indexCount/3),uint32_t(group.firstIndex*4),0,0});
        ranges.push_back({uint32_t(group.firstIndex/6),uint32_t(group.indexCount/6),group.faceDirection});
    }
    if(geometries.size()>cap.limits.maxGeometryCount){LOGE("[HardwareRT] vox BLAS: geometry count %zu > %u",geometries.size(),cap.limits.maxGeometryCount);return false;}
    decoder.Record(cmd,buildSet,renderer);
    RayTracingInputBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT);
    if(!blas.RecordBuild(cmd,VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,geometries,buildRanges,VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR)){LOGE("[HardwareRT] vox BLAS RecordBuild failed: quads=%zu",count);return false;}
    revision=renderer.GetGeometryRevision();source=&renderer;return true;
}
