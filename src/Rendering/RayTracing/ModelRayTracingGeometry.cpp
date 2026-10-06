#include "Rendering/RayTracing/ModelRayTracingGeometry.h"
#include "Rendering/ModelRenderer.h"
#include "Core/VulkanRayTracingDevice.h"
#include "Core/VulkanContext.h"
#include "Core/Log.h"
#include <algorithm>
#include <cstddef>
#include <limits>

bool ModelRayTracingGeometry::Matches(ModelRenderer& renderer) const {
    const auto& mesh=renderer.GetMeshData();const auto& gpu=renderer.GetSubMeshes();
    return source==&renderer && mesh.subMeshes.size()==1 && gpu.size()==1 &&
        sourceVertices==gpu[0].vertexBuffer && sourceIndices==gpu[0].indexBuffer &&
        vertexCount==mesh.subMeshes[0].vertices.size() && indexCount==mesh.subMeshes[0].indices.size();
}
void ModelRayTracingGeometry::ReleaseBuildInputs(){
    vertexStaging.Cleanup();indexStaging.Cleanup();blas.ReleaseScratch();
}
bool ModelRayTracingGeometry::RecordBuild(VkCommandBuffer cmd,ModelRenderer& renderer){
    static_assert(sizeof(Vertex)==32 && offsetof(Vertex,Position)==0 && offsetof(Vertex,Normal)==12 && offsetof(Vertex,TexCoords)==16);
    const auto& mesh=renderer.GetMeshData();const auto& caps=GetRayTracingDeviceCapabilities();
    if(!cmd || blas.Handle() || !caps.accelerationStructure || mesh.isMmd || !mesh.bones.empty() || mesh.subMeshes.size()!=1)return false;
    const auto& sub=mesh.subMeshes[0];
    if(sub.vertices.empty() || sub.indices.empty() || sub.indices.size()%3 || sub.vertices.size()>UINT32_MAX || sub.indices.size()>UINT32_MAX || sub.indices.size()/3>caps.limits.maxPrimitiveCount)return false;
    if(*std::max_element(sub.indices.begin(),sub.indices.end())>=sub.vertices.size())return false;
    VkFormatProperties format{};vkGetPhysicalDeviceFormatProperties(g_PhysicalDevice,VK_FORMAT_R32G32B32_SFLOAT,&format);
    if(!(format.bufferFeatures&VK_FORMAT_FEATURE_ACCELERATION_STRUCTURE_VERTEX_BUFFER_BIT_KHR))return false;
    const VkDeviceSize vertexBytes=sub.vertices.size()*sizeof(Vertex),indexBytes=sub.indices.size()*sizeof(uint32_t);
    const auto usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT|VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    const auto host=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if(!vertices.Create(vertexBytes,usage,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) || !indices.Create(indexBytes,usage,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ||
        !vertexStaging.Create(vertexBytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,host) || !indexStaging.Create(indexBytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,host))return false;
    vertexStaging.Write(sub.vertices.data(),vertexBytes);indexStaging.Write(sub.indices.data(),indexBytes);
    VkMemoryBarrier hostWrites{VK_STRUCTURE_TYPE_MEMORY_BARRIER};hostWrites.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;hostWrites.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&hostWrites,0,nullptr,0,nullptr);
    VkBufferCopy copy{0,0,vertexBytes};vkCmdCopyBuffer(cmd,vertexStaging.GetBuffer(),vertices.GetBuffer(),1,&copy);
    copy.size=indexBytes;vkCmdCopyBuffer(cmd,indexStaging.GetBuffer(),indices.GetBuffer(),1,&copy);
    VkMemoryBarrier inputs{VK_STRUCTURE_TYPE_MEMORY_BARRIER};inputs.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    inputs.dstAccessMask=VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR|VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&inputs,0,nullptr,0,nullptr);
    VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};geometry.geometryType=VK_GEOMETRY_TYPE_TRIANGLES_KHR;geometry.flags=VK_GEOMETRY_OPAQUE_BIT_KHR;
    auto& triangles=geometry.geometry.triangles;triangles.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    triangles.vertexFormat=VK_FORMAT_R32G32B32_SFLOAT;triangles.vertexData.deviceAddress=vertices.GetDeviceAddress();triangles.vertexStride=sizeof(Vertex);triangles.maxVertex=uint32_t(sub.vertices.size()-1);
    triangles.indexType=VK_INDEX_TYPE_UINT32;triangles.indexData.deviceAddress=indices.GetDeviceAddress();
    if(!triangles.vertexData.deviceAddress || !triangles.indexData.deviceAddress)return false;
    VkAccelerationStructureBuildRangeInfoKHR range{uint32_t(sub.indices.size()/3),0,0,0};
    if(!blas.RecordBuild(cmd,VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,{&geometry,1},{&range,1},VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR))return false;
    source=&renderer;sourceVertices=renderer.GetSubMeshes()[0].vertexBuffer;sourceIndices=renderer.GetSubMeshes()[0].indexBuffer;
    vertexCount=uint32_t(sub.vertices.size());indexCount=uint32_t(sub.indices.size());
    if(sub.materialIndex>=0 && size_t(sub.materialIndex)<mesh.materialTextures.size())baseColor=mesh.materialTextures[sub.materialIndex].diffuse;
    LOGI("[HardwareRT] static model BLAS: %s vertices=%u triangles=%u",renderer.GetModelPath().c_str(),vertexCount,indexCount/3);
    return true;
}
