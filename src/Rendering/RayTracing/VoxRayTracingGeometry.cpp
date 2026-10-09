#include "Core/CpuStageTrace.h"
#include "Rendering/RayTracing/VoxRayTracingGeometry.h"
#include "Rendering/VoxRenderer.h"
#include "Core/VulkanContext.h"
#include "Core/VulkanRayTracingDevice.h"
#include "Core/EngineConfig.h"
#include <algorithm>
#include <limits>
#include <cstdlib>
#include <cstring>

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
void VoxRayTracingConverter::Record(VkCommandBuffer cmd,VkDescriptorSet set,const VoxRenderer& renderer,const std::vector<VoxRayTracingRange>& ranges,uint32_t encoding) {
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,nullptr);
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;host.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&host,0,nullptr,0,nullptr);
    VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(g_PhysicalDevice,&properties);
    const uint32_t batch=uint32_t(std::min(uint64_t(UINT32_MAX),uint64_t(properties.limits.maxComputeWorkGroupCount[0])*64));
    struct Push{glm::vec4 minSize;glm::uvec4 range;};static_assert(sizeof(Push)==32);
    for(const auto& group:ranges) {
        uint32_t first=group.firstQuad,remaining=group.quadCount;
        while(remaining){const auto count=std::min(remaining,batch);Push push{glm::vec4(renderer.GetMinBounds(),renderer.GetVoxelSize()),glm::uvec4(first,count,group.direction,encoding)};
            vkCmdPushConstants(cmd,layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),&push);vkCmdDispatch(cmd,(count+63)/64,1,1);first+=count;remaining-=count;}
    }
}
void VoxRayTracingGeometry::ReleaseBuildInputs() {
    if(buildSet && converter)converter->Free(buildSet);buildSet=VK_NULL_HANDLE;
    positions.Cleanup();indices.Cleanup();buildTransform.Cleanup();blas->ReleaseScratch();buildCompleted=true;
}
bool VoxRayTracingGeometry::RecordCompaction(VkCommandBuffer cmd,VkDeviceSize& budget,std::shared_ptr<AccelerationStructure>& retired){
    if(!buildCompleted||compactionFinished)return false;
    VkDeviceSize bytes=0;const auto result=blas->ReadCompactedSize(bytes);
    if(result==VK_NOT_READY)return false;
    if(result!=VK_SUCCESS){compactionFinished=true;blas->DiscardCompactionQuery();return false;}
    const auto original=blas->StorageBytes();
    if(!bytes||bytes>=original){
        LOGI("[VOX BLAS compact] original=%llu queried=%llu skipped: no reduction",(unsigned long long)original,(unsigned long long)bytes);
        compactionFinished=true;blas->DiscardCompactionQuery();return false;
    }
    if(bytes>budget)return false; // Retry next frame; never wait or exceed the budget.
    auto target=std::make_shared<AccelerationStructure>();
    if(!target->RecordCompactCopy(cmd,*blas,bytes)){
        LOGI("[VOX BLAS compact] allocation unavailable; retaining original=%llu",(unsigned long long)original);
        compactionFinished=true;blas->DiscardCompactionQuery();return false;
    }
    budget-=bytes;blas->DiscardCompactionQuery();retired=std::move(blas);blas=std::move(target);compactionFinished=true;
    LOGI("[VOX BLAS compact] original=%llu compacted=%llu saved=%llu copyPeak=%llu; old address retained until frame fences",
        (unsigned long long)original,(unsigned long long)bytes,(unsigned long long)(original-bytes),(unsigned long long)(original+bytes));
    return true;
}
bool VoxRayTracingGeometry::RecordBuild(VkCommandBuffer cmd,const VoxRenderer& renderer,VoxRayTracingConverter& decoder) {
    Core::CpuStageTrace cpuStageTrace("vox.blas_prepare_record");
    const auto& cap=GetRayTracingDeviceCapabilities();
    if(!cap.accelerationStructure || !renderer.HasValidQuads() || blas->Handle()){LOGE("[HardwareRT] vox BLAS precheck failed: quads=%zu valid=%d blas=%p",renderer.GetQuads().size(),renderer.HasValidQuads()?1:0,(void*)blas->Handle());return false;}
    const char* compactOption=std::getenv("MIKAN_VOX_RT_COMPACT_BUILD");
    const bool compact=!compactOption||std::strcmp(compactOption,"0")!=0;
    auto supportsVertex=[](VkFormat candidate){VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(g_PhysicalDevice,candidate,&properties);
        return (properties.bufferFeatures&VK_FORMAT_FEATURE_ACCELERATION_STRUCTURE_VERTEX_BUFFER_BIT_KHR)!=0;};
    VkFormat vertexFormat=VK_FORMAT_R32G32B32_SFLOAT;uint32_t vertexStride=12,encoding=0;
    // Half floats exactly represent all block-local integer coordinates 0..256.
    // A hardware build transform restores minBounds + grid * voxelSize; no
    // shader decompression to a second float3 buffer is involved.
    if(compact&&supportsVertex(VK_FORMAT_R16G16B16_SFLOAT)){
        vertexFormat=VK_FORMAT_R16G16B16_SFLOAT;vertexStride=6;encoding=1;
    }else if(compact&&supportsVertex(VK_FORMAT_R16G16B16A16_SFLOAT)){
        vertexFormat=VK_FORMAT_R16G16B16A16_SFLOAT;vertexStride=8;encoding=2;
    }else if(!supportsVertex(vertexFormat)){LOGE("[HardwareRT] vox BLAS: no supported vertex format");return false;}

    {
        rtQuads=renderer.GetQuads();
        rtMaterials=renderer.GetQuadMaterials();
        attributes=renderer.GetSurfaceAttributes();
        for(const auto& g:renderer.GetMeshData().faceGroups)if(g.indexCount)ranges.push_back({uint32_t(g.firstIndex/6),uint32_t(g.indexCount/6),g.faceDirection});
    }
    const VkDeviceSize count=rtQuads.size();
    const bool index16=compact&&count<=16384; // Four vertices per quad; max index 65535.
    const uint32_t indexBytes=index16?2u:4u;if(index16)encoding|=4u;
    const VkDeviceSize positionBytes=count*4*vertexStride,indicesBytes=count*6*indexBytes;
    std::vector<uint32_t> packed;for(auto q:renderer.GetQuads())packed.push_back(q.geometry);AppendVoxPlaneFooter(packed,renderer.GetPlaneRanges());
    VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(g_PhysicalDevice,&properties);
    if(!count||count>UINT32_MAX/24 || positionBytes>properties.limits.maxStorageBufferRange || indicesBytes>properties.limits.maxStorageBufferRange || count*2>cap.limits.maxPrimitiveCount){LOGE("[HardwareRT] vox BLAS limits: quads=%zu maxPrim=%u maxSSBO=%llu",count,cap.limits.maxPrimitiveCount,(unsigned long long)properties.limits.maxStorageBufferRange);return false;}
    const auto host=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const auto input=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT|VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    const char* aabbOption=std::getenv("MIKAN_VOX_HW_QUAD_AABB");
    if(aabbOption&&std::strcmp(aabbOption,"1")==0){
        std::vector<VkAabbPositionsKHR> boxes(count);
        auto sortedPlanes=renderer.GetPlaneRanges();std::sort(sortedPlanes.begin(),sortedPlanes.end(),[](const auto& a,const auto& b){return a.firstQuad<b.firstQuad;});
        for(const auto& group:ranges)for(uint32_t q=group.firstQuad;q<group.firstQuad+group.quadCount;++q){
            const auto plane=std::upper_bound(sortedPlanes.begin(),sortedPlanes.end(),q,[](uint32_t index,const auto& p){return index<p.firstQuad;});
            if(plane==sortedPlanes.begin())return false;
            const auto [g,a]=DecodeVoxQuad(rtQuads[q].geometry,std::prev(plane)->planeDirection);const auto face=group.direction;
            const int axis=face<2?2:(face<4?0:1),u=face<2?0:(face<4?2:0),v=face<4?1:2;
            glm::vec3 lo(float(g&255u),float((g>>8)&255u),float((g>>16)&255u));
            if(face==0||face==3||face==4)lo[axis]+=1;
            glm::vec3 hi=lo;hi[u]+=float((g>>24)+1u);hi[v]+=float((a&255u)+1u);
            lo=renderer.GetMinBounds()+lo*renderer.GetVoxelSize();hi=renderer.GetMinBounds()+hi*renderer.GetVoxelSize();
            // Conservative thin slab; precise plane/range rejection happens in the ray query.
            const float pad=std::max(renderer.GetVoxelSize()*1e-4f,1e-5f);
            lo-=glm::vec3(pad);hi+=glm::vec3(pad);
            boxes[q]={lo.x,lo.y,lo.z,hi.x,hi.y,hi.z};
        }
        if(!quads.Create(packed.size()*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,host)||
           !positions.Create(boxes.size()*sizeof(VkAabbPositionsKHR),input,host))return false;
        quads.Write(packed.data(),packed.size()*4);positions.Write(boxes.data(),boxes.size()*sizeof(VkAabbPositionsKHR));
        std::vector<VkAccelerationStructureGeometryKHR> geometries;
        std::vector<VkAccelerationStructureBuildRangeInfoKHR> buildRanges;
        for(const auto& group:ranges){
            VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
            geometry.geometryType=VK_GEOMETRY_TYPE_AABBS_KHR;geometry.flags=VK_GEOMETRY_OPAQUE_BIT_KHR;
            geometry.geometry.aabbs.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR;
            geometry.geometry.aabbs.data.deviceAddress=positions.GetDeviceAddress()+group.firstQuad*sizeof(VkAabbPositionsKHR);geometry.geometry.aabbs.stride=sizeof(VkAabbPositionsKHR);
            geometries.push_back(geometry);buildRanges.push_back({group.quadCount,0,0,0});
        }
        RayTracingInputBarrier(cmd,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_HOST_WRITE_BIT);
        const char* compactBlas=std::getenv("MIKAN_VOX_BLAS_COMPACTION");const auto& functions=GetRayTracingFunctions();
        const bool allow=(!compactBlas||std::strcmp(compactBlas,"0")!=0)&&functions.writeProperties&&functions.copy;
        compactionFinished=!allow;
        if(!blas->RecordBuild(cmd,VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,geometries,buildRanges,
            VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR|(allow?VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR:0u)))return false;
        LOGI("[VOX AABB BLAS] quads=%zu input=%llu storage=%llu scratch=%llu",count,
            (unsigned long long)(count*sizeof(VkAabbPositionsKHR)),(unsigned long long)blas->StorageBytes(),(unsigned long long)blas->ScratchBytes());
        revision=renderer.GetGeometryRevision();source=&renderer;return true;
    }
    if(!quads.Create(packed.size()*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,host) || !positions.Create(positionBytes,input,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) || !indices.Create(indicesBytes,input,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)){LOGE("[HardwareRT] vox BLAS buffer create failed: quads=%zu",count);return false;}
    if((encoding&3u)!=0u){
        if(!buildTransform.Create(sizeof(VkTransformMatrixKHR),input,host))return false;
        VkTransformMatrixKHR transform{};
        for(uint32_t a=0;a<3;++a){transform.matrix[a][a]=renderer.GetVoxelSize();transform.matrix[a][3]=renderer.GetMinBounds()[a];}
        buildTransform.Write(&transform,sizeof(transform));
        if(!buildTransform.GetDeviceAddress()||buildTransform.GetDeviceAddress()%16u!=0u)return false;
    }
    quads.Write(packed.data(),packed.size()*4);converter=&decoder;
    buildSet=decoder.Allocate(quads.GetBuffer(),positions.GetBuffer(),indices.GetBuffer());if(!buildSet){LOGE("[HardwareRT] vox BLAS descriptor alloc failed");return false;}
    // Validate all build inputs before recording compute so failures can release resources safely.
    std::vector<VkAccelerationStructureGeometryKHR> geometries;
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> buildRanges;
    for(const auto& group:ranges){
        VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};geometry.geometryType=VK_GEOMETRY_TYPE_TRIANGLES_KHR;geometry.flags=VK_GEOMETRY_OPAQUE_BIT_KHR;
        auto& triangles=geometry.geometry.triangles;triangles.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        triangles.vertexFormat=vertexFormat;triangles.vertexData.deviceAddress=positions.GetDeviceAddress();triangles.vertexStride=vertexStride;triangles.maxVertex=uint32_t(count*4-1);
        triangles.indexType=index16?VK_INDEX_TYPE_UINT16:VK_INDEX_TYPE_UINT32;triangles.indexData.deviceAddress=indices.GetDeviceAddress();
        if((encoding&3u)!=0u)triangles.transformData.deviceAddress=buildTransform.GetDeviceAddress();
        if(!triangles.vertexData.deviceAddress || !triangles.indexData.deviceAddress)return false;
        geometries.push_back(geometry);buildRanges.push_back({group.quadCount*2u,group.firstQuad*6u*indexBytes,0,0});
        // Ranges already describe the RT-specific geometry.
    }
    if(geometries.size()>cap.limits.maxGeometryCount){LOGE("[HardwareRT] vox BLAS: geometry count %zu > %u",geometries.size(),cap.limits.maxGeometryCount);return false;}
    decoder.Record(cmd,buildSet,renderer,ranges,encoding);
    RayTracingInputBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_HOST_WRITE_BIT);
    const char* compactBlas=std::getenv("MIKAN_VOX_BLAS_COMPACTION");
    const auto& functions=GetRayTracingFunctions();
    const bool allowCompaction=(!compactBlas||std::strcmp(compactBlas,"0")!=0)&&functions.writeProperties&&functions.copy;
    const auto flags=VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR|(allowCompaction?VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR:0u);
    compactionFinished=!allowCompaction;
    if(!blas->RecordBuild(cmd,VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,geometries,buildRanges,flags)){LOGE("[HardwareRT] vox BLAS RecordBuild failed: quads=%zu",count);return false;}
    LOGI("[VOX BLAS memory] triangles=%zu storage=%llu scratch=%llu compaction=%d",count*2,
        (unsigned long long)blas->StorageBytes(),(unsigned long long)blas->ScratchBytes(),allowCompaction?1:0);
    LOGI("[HardwareRT] vox BLAS inputs: quads=%zu vertexStride=%u indexBits=%u bytes=%llu baselineBytes=%llu transformBytes=%u",
        count,vertexStride,indexBytes*8u,(unsigned long long)(positionBytes+indicesBytes),(unsigned long long)(count*72u),(encoding&3u)?uint32_t(sizeof(VkTransformMatrixKHR)):0u);
    revision=renderer.GetGeometryRevision();source=&renderer;return true;
}
