#include "Rendering/RayTracing/ModelRayTracingGeometry.h"
#include "Rendering/ModelRenderer.h"
#include "Core/VulkanRayTracingDevice.h"
#include "Core/VulkanContext.h"
#include "Core/Log.h"
#include "Core/VulkanGpuProfiler.h"
#include "Core/RenderGlobals.h"
#include "Rendering/TexturePool.h"
#include <glm/packing.hpp>
#include <algorithm>
#include <cstddef>
#include <limits>
#include <cstdlib>
#include <cstring>

bool ModelRayTracingGeometry::Matches(ModelRenderer& renderer,uint32_t index,bool allowPartition) const {
    const auto& mesh=renderer.GetMeshData();const auto& gpu=renderer.GetSubMeshes();
    const auto* texture=index<gpu.size() && g_TexturePool?g_TexturePool->GetTexture(gpu[index].diffuseTexturePath):nullptr;
    const auto classification=texture?texture->alphaClassification:nullptr;
    return subMeshIndex==index && source==&renderer &&
        classification==alphaClassification && alphaLodBias==g_ModelMipLodBias && partitionAllowed==allowPartition &&
        index<mesh.subMeshes.size() && gpu.size()==mesh.subMeshes.size() &&
        sourceVertices==gpu[index].vertexBuffer && sourceIndices==gpu[index].indexBuffer &&
        vertexCount==mesh.subMeshes[index].vertices.size() && indexCount==mesh.subMeshes[index].indices.size();
}
void ModelRayTracingGeometry::ReleaseBuildInputs(){
    vertexStaging.Cleanup();indexStaging.Cleanup();buildIndices.Cleanup();buildIndexStaging.Cleanup();blas->ReleaseScratch();buildCompleted=true;
}
bool ModelRayTracingGeometry::RecordCompaction(VkCommandBuffer cmd,VkDeviceSize& budget,std::shared_ptr<AccelerationStructure>& retired){
    if(!buildCompleted||compactionFinished)return false;
    VkDeviceSize bytes=0;const auto result=blas->ReadCompactedSize(bytes);
    if(result==VK_NOT_READY)return false;
    if(result!=VK_SUCCESS){compactionFinished=true;blas->DiscardCompactionQuery();return false;}
    const auto original=blas->StorageBytes();
    if(!bytes||bytes>=original){compactionFinished=true;blas->DiscardCompactionQuery();return false;}
    if(bytes>budget)return false;
    auto target=std::make_shared<AccelerationStructure>();
    if(!target->RecordCompactCopy(cmd,*blas,bytes)){
        LOGW("[Model BLAS compact] allocation unavailable; retaining original=%llu",(unsigned long long)original);
        compactionFinished=true;blas->DiscardCompactionQuery();return false;
    }
    budget-=bytes;blas->DiscardCompactionQuery();retired=std::move(blas);blas=std::move(target);compactionFinished=true;
    LOGI("[Model BLAS compact] submesh=%u original=%llu compacted=%llu saved=%llu copyPeak=%llu",
        subMeshIndex,(unsigned long long)original,(unsigned long long)bytes,
        (unsigned long long)(original-bytes),(unsigned long long)(original+bytes));
    return true;
}
bool ModelRayTracingGeometry::RecordBuild(VkCommandBuffer cmd,ModelRenderer& renderer,uint32_t subMeshIndex,bool allowPartition){
    static_assert(sizeof(Vertex)==32 && offsetof(Vertex,Position)==0 && offsetof(Vertex,Normal)==12 && offsetof(Vertex,TexCoords)==16);
    const auto& mesh=renderer.GetMeshData();const auto& caps=GetRayTracingDeviceCapabilities();
    if(!cmd || blas->Handle() || !caps.accelerationStructure || mesh.isMmd || !mesh.bones.empty() || subMeshIndex>=mesh.subMeshes.size())return false;
    const auto& sub=mesh.subMeshes[subMeshIndex];
    if(sub.vertices.empty() || sub.indices.empty() || sub.indices.size()%3 || sub.vertices.size()>UINT32_MAX || sub.indices.size()>UINT32_MAX || sub.indices.size()/3>caps.limits.maxPrimitiveCount)return false;
    if(*std::max_element(sub.indices.begin(),sub.indices.end())>=sub.vertices.size())return false;
    VkFormatProperties format{};vkGetPhysicalDeviceFormatProperties(g_PhysicalDevice,VK_FORMAT_R32G32B32_SFLOAT,&format);
    if(!(format.bufferFeatures&VK_FORMAT_FEATURE_ACCELERATION_STRUCTURE_VERTEX_BUFFER_BIT_KHR))return false;
    const auto& gpuSub=renderer.GetSubMeshes()[subMeshIndex];
    const auto* texture=g_TexturePool?g_TexturePool->GetTexture(gpuSub.diffuseTexturePath):nullptr;
    alphaClassification=texture?texture->alphaClassification:nullptr;
    alphaLodBias=g_ModelMipLodBias;
    const bool mask=sub.alphaMode==1 || (sub.alphaMode<=0 && texture && texture->alphaCutout);
    std::vector<uint32_t> indexWords=sub.indices;
    uint32_t opaqueCount=0,transparentCount=0,unknownCount=uint32_t(sub.indices.size()/3);
    static const bool classificationEnabled=[](){
        const char* option=std::getenv("MIKAN_HWRT_PROFILE_NO_ALPHA_CLASS");
        const char* noAlpha=std::getenv("MIKAN_HWRT_PROFILE_NO_ALPHA");
        return !(Core::VulkanGpuProfiler::IsRequested() && ((option && option[0]=='1') || (noAlpha && noAlpha[0]=='1')));
    }();
    if(mask && classificationEnabled && alphaClassification && alphaClassification->width){
        hasAlphaClassification=true;unknownCount=0;
        alphaStates.resize(sub.indices.size()/3);
        VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(g_PhysicalDevice,&properties);
        const uint32_t padding=uint32_t(std::ceil(properties.limits.maxSamplerAnisotropy))+2u;
        const size_t first=indexWords.size();indexWords.resize(first+(sub.indices.size()/3+15)/16,0);
        for(size_t triangle=0;triangle<sub.indices.size()/3;++triangle){
            const auto a=glm::unpackHalf2x16(glm::packUint2x16(sub.vertices[sub.indices[triangle*3]].TexCoords));
            const auto b=glm::unpackHalf2x16(glm::packUint2x16(sub.vertices[sub.indices[triangle*3+1]].TexCoords));
            const auto c=glm::unpackHalf2x16(glm::packUint2x16(sub.vertices[sub.indices[triangle*3+2]].TexCoords));
            // Positive bias may sample lower mips. Region proofs are LOD0 only;
            // whole-texture uniform proofs remain valid at every generated mip.
            const auto state=alphaLodBias<=0?alphaClassification->Classify(a.x,a.y,b.x,b.y,c.x,c.y,padding):alphaClassification->Whole();
            indexWords[first+triangle/16]|=uint32_t(state)<<((triangle%16)*2);
            alphaStates[triangle]=uint8_t(state);
            if(state==TextureAlphaClassification::Opaque)++opaqueCount;
            else if(state==TextureAlphaClassification::Transparent)++transparentCount;
            else ++unknownCount;
        }
        needsAlphaCandidates=opaqueCount!=sub.indices.size()/3;
        if(opaqueCount+transparentCount==0){
            // No proof succeeded: avoid adding a classification read to every
            // foliage candidate, and retain the original compact index buffer.
            hasAlphaClassification=false;indexWords.resize(first);
        }
    }
    // Reorder only the temporary AS build indices. Shading keeps canonical
    // indices and IDs, with a compact map from each AS primitive back to them.
    std::vector<uint32_t> traceIndices,primitiveMap;
    partitionAllowed=allowPartition;
    fullyTransparent=allowPartition && transparentCount==sub.indices.size()/3;
    static const bool splitEnabled=[](){
        const char* option=std::getenv("MIKAN_HWRT_PROFILE_NO_ALPHA_SPLIT");
        return !(Core::VulkanGpuProfiler::IsRequested() && option && option[0]=='1');
    }();
    opacityPartition=allowPartition && splitEnabled && hasAlphaClassification && opaqueCount+unknownCount>0 && opaqueCount<0x20000000u;
    if(opacityPartition){
        opaquePrimitiveCount=opaqueCount;
        traceIndices.reserve(size_t(opaqueCount+unknownCount)*3);
        primitiveMap.reserve(opaqueCount+unknownCount);
        for(const auto state:{TextureAlphaClassification::Opaque,TextureAlphaClassification::Unknown})
            for(uint32_t primitive=0;primitive<alphaStates.size();++primitive)if(alphaStates[primitive]==state){
                primitiveMap.push_back(primitive);
                for(uint32_t corner=0;corner<3;++corner)traceIndices.push_back(sub.indices[primitive*3+corner]);
            }
        indexWords.insert(indexWords.end(),primitiveMap.begin(),primitiveMap.end());
        if(primitiveMap.size()!=size_t(opaqueCount)+unknownCount)return false;
        for(size_t i=0;i<primitiveMap.size();++i){
            const auto original=primitiveMap[i];
            if(original>=sub.indices.size()/3 || alphaStates[original]!=(i<opaqueCount?TextureAlphaClassification::Opaque:TextureAlphaClassification::Unknown))return false;
            for(uint32_t corner=0;corner<3;++corner)if(traceIndices[i*3+corner]!=sub.indices[original*3+corner])return false;
        }
    }
    const VkDeviceSize vertexBytes=sub.vertices.size()*sizeof(Vertex),indexBytes=indexWords.size()*sizeof(uint32_t);
    const auto usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT|VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    const auto host=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if(!vertices.Create(vertexBytes,usage,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) || !indices.Create(indexBytes,usage,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ||
        !vertexStaging.Create(vertexBytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,host) || !indexStaging.Create(indexBytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,host))return false;
    vertexStaging.Write(sub.vertices.data(),vertexBytes);indexStaging.Write(indexWords.data(),indexBytes);
    if(opacityPartition){
        const VkDeviceSize bytes=traceIndices.size()*sizeof(uint32_t);
        if(!buildIndices.Create(bytes,usage,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) || !buildIndexStaging.Create(bytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,host))return false;
        buildIndexStaging.Write(traceIndices.data(),bytes);
    }
    VkMemoryBarrier hostWrites{VK_STRUCTURE_TYPE_MEMORY_BARRIER};hostWrites.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;hostWrites.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&hostWrites,0,nullptr,0,nullptr);
    VkBufferCopy copy{0,0,vertexBytes};vkCmdCopyBuffer(cmd,vertexStaging.GetBuffer(),vertices.GetBuffer(),1,&copy);
    copy.size=indexBytes;vkCmdCopyBuffer(cmd,indexStaging.GetBuffer(),indices.GetBuffer(),1,&copy);
    if(opacityPartition){copy.size=traceIndices.size()*sizeof(uint32_t);vkCmdCopyBuffer(cmd,buildIndexStaging.GetBuffer(),buildIndices.GetBuffer(),1,&copy);}
    VkMemoryBarrier inputs{VK_STRUCTURE_TYPE_MEMORY_BARRIER};inputs.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    // Capture layers may copy build inputs after upload; make those reads visible too.
    inputs.dstAccessMask=VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR|VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&inputs,0,nullptr,0,nullptr);
    VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};geometry.geometryType=VK_GEOMETRY_TYPE_TRIANGLES_KHR;geometry.flags=VK_GEOMETRY_OPAQUE_BIT_KHR;
    auto& triangles=geometry.geometry.triangles;triangles.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    triangles.vertexFormat=VK_FORMAT_R32G32B32_SFLOAT;triangles.vertexData.deviceAddress=vertices.GetDeviceAddress();triangles.vertexStride=sizeof(Vertex);triangles.maxVertex=uint32_t(sub.vertices.size()-1);
    triangles.indexType=VK_INDEX_TYPE_UINT32;triangles.indexData.deviceAddress=opacityPartition?buildIndices.GetDeviceAddress():indices.GetDeviceAddress();
    if(!triangles.vertexData.deviceAddress || !triangles.indexData.deviceAddress)return false;
    VkAccelerationStructureBuildRangeInfoKHR range{uint32_t(sub.indices.size()/3),0,0,0};
    std::vector<VkAccelerationStructureGeometryKHR> geometries;
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
    if(opacityPartition){
        if(opaqueCount){geometries.push_back(geometry);ranges.push_back({opaqueCount,0,0,0});}
        if(unknownCount){
            auto masked=geometry;masked.flags=0;
            masked.geometry.triangles.indexData.deviceAddress+=VkDeviceAddress(opaqueCount)*3*sizeof(uint32_t);
            geometries.push_back(masked);ranges.push_back({unknownCount,0,0,0});
        }
    }else{geometries.push_back(geometry);ranges.push_back(range);}
    const char* compact=std::getenv("MIKAN_MODEL_BLAS_COMPACTION");const auto& functions=GetRayTracingFunctions();
    const bool allow=!fullyTransparent && (!compact||std::strcmp(compact,"0")!=0)&&functions.writeProperties&&functions.copy;
    compactionFinished=!allow;
    if(!blas->RecordBuild(cmd,VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,geometries,ranges,
        VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR|(allow?VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR:0u)))return false;
    source=&renderer;sourceVertices=renderer.GetSubMeshes()[subMeshIndex].vertexBuffer;sourceIndices=renderer.GetSubMeshes()[subMeshIndex].indexBuffer;
    vertexCount=uint32_t(sub.vertices.size());indexCount=uint32_t(sub.indices.size());
    this->subMeshIndex=subMeshIndex;
    if(sub.materialIndex>=0 && size_t(sub.materialIndex)<mesh.materialTextures.size())baseColor=mesh.materialTextures[sub.materialIndex].diffuse;
    LOGI("[HardwareRT] static model BLAS: %s submesh=%u/%zu vertices=%u triangles=%u",renderer.GetModelPath().c_str(),subMeshIndex,mesh.subMeshes.size(),vertexCount,indexCount/3);
    if(mask)LOGI("[AlphaClass] submesh=%u opaque=%u transparent=%u unknown=%u classified=%u",subMeshIndex,opaqueCount,transparentCount,unknownCount,uint32_t(hasAlphaClassification));
    if(opacityPartition)LOGI("[AlphaSplit] submesh=%u opaque=%u mask=%u removed=%u geometries=%zu",subMeshIndex,opaqueCount,unknownCount,transparentCount,geometries.size());
    return true;
}

// Dynamic BLAS implementation intentionally lives beside the static builder,
// but uses its own class, slot cache, buffers and ALLOW_UPDATE build policy.
#include "Rendering/RayTracing/DynamicModelRayTracingGeometry.h"

bool DynamicModelRayTracingGeometry::MatchesDynamic(ModelRenderer& renderer,uint32_t index) const {
    const auto& mesh=renderer.GetMeshData();const auto& gpu=renderer.GetSubMeshes();
    return source==&renderer&&subMeshIndex==index&&index<mesh.subMeshes.size()&&index<gpu.size()&&
        sourceVertices==gpu[index].vertexBuffer&&sourceIndices==gpu[index].indexBuffer&&
        vertexCount==mesh.subMeshes[index].vertices.size()&&indexCount==mesh.subMeshes[index].indices.size();
}
bool DynamicModelRayTracingGeometry::RecordPose(VkCommandBuffer cmd,ModelRenderer& renderer,uint32_t index,
    const std::shared_ptr<const std::vector<Vertex>>& current,
    const std::shared_ptr<const std::vector<Vertex>>& previous,bool& boundsChanged){
    if(!cmd||!current||!previous||current->empty()||current->size()!=previous->size())return false;
    const auto& mesh=renderer.GetMeshData();const auto& gpu=renderer.GetSubMeshes();
    if(index>=mesh.subMeshes.size()||index>=gpu.size())return false;
    const auto& sub=mesh.subMeshes[index];const auto& caps=GetRayTracingDeviceCapabilities();
    if(!caps.accelerationStructure||current->size()!=sub.vertices.size()||sub.indices.empty()||sub.indices.size()%3||
       current->size()>UINT32_MAX||sub.indices.size()>UINT32_MAX||sub.indices.size()/3>caps.limits.maxPrimitiveCount)return false;
    if(*std::max_element(sub.indices.begin(),sub.indices.end())>=current->size())return false;
    const bool first=!blas->Handle(),changed=first||currentCpu!=current,previousChanged=first||previousCpu!=previous;
    if(!changed&&!previousChanged)return true;
    const VkDeviceSize vertexBytes=current->size()*sizeof(Vertex),indexBytes=sub.indices.size()*sizeof(uint32_t);
    const auto usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT|VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    const auto host=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if(first){
        if(!vertices.Create(vertexBytes,usage,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)||!previousVertices.Create(vertexBytes,usage,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)||
           !indices.Create(indexBytes,usage,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)||
           !vertexStaging.Create(vertexBytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,host)||!previousStaging.Create(vertexBytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,host)||
           !indexStaging.Create(indexBytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,host))return false;
        indexStaging.Write(sub.indices.data(),indexBytes);
    }
    glm::vec3 lo(std::numeric_limits<float>::max()),hi(-std::numeric_limits<float>::max());
    if(changed){
        for(const auto& v:*current){
            if(!std::isfinite(v.Position.x)||!std::isfinite(v.Position.y)||!std::isfinite(v.Position.z))return false;
            lo=glm::min(lo,v.Position);hi=glm::max(hi,v.Position);
        }
        vertexStaging.Write(current->data(),vertexBytes);
    }
    if(previousChanged)previousStaging.Write(previous->data(),vertexBytes);
    VkMemoryBarrier hostWrites{VK_STRUCTURE_TYPE_MEMORY_BARRIER};hostWrites.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;hostWrites.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&hostWrites,0,nullptr,0,nullptr);
    VkBufferCopy copy{0,0,vertexBytes};
    if(changed)vkCmdCopyBuffer(cmd,vertexStaging.GetBuffer(),vertices.GetBuffer(),1,&copy);
    if(previousChanged)vkCmdCopyBuffer(cmd,previousStaging.GetBuffer(),previousVertices.GetBuffer(),1,&copy);
    if(first){copy.size=indexBytes;vkCmdCopyBuffer(cmd,indexStaging.GetBuffer(),indices.GetBuffer(),1,&copy);}
    VkMemoryBarrier inputs{VK_STRUCTURE_TYPE_MEMORY_BARRIER};inputs.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    // Capture layers may copy build inputs after upload; make those reads visible too.
    inputs.dstAccessMask=VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR|VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&inputs,0,nullptr,0,nullptr);
    if(changed){
        VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};geometry.geometryType=VK_GEOMETRY_TYPE_TRIANGLES_KHR;geometry.flags=VK_GEOMETRY_OPAQUE_BIT_KHR;
        auto& triangles=geometry.geometry.triangles;triangles.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        triangles.vertexFormat=VK_FORMAT_R32G32B32_SFLOAT;triangles.vertexData.deviceAddress=vertices.GetDeviceAddress();triangles.vertexStride=sizeof(Vertex);
        triangles.maxVertex=uint32_t(current->size()-1);triangles.indexType=VK_INDEX_TYPE_UINT32;triangles.indexData.deviceAddress=indices.GetDeviceAddress();
        VkAccelerationStructureBuildRangeInfoKHR range{uint32_t(sub.indices.size()/3),0,0,0};
        Core::VulkanGpuScope timing(cmd,first?"rt.dynamic_blas_build":"rt.dynamic_blas_refit");
        RayTracingBuildBarrier(cmd);
        if(!blas->RecordBuild(cmd,VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,{&geometry,1},{&range,1},
           VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR|VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR,!first))return false;
        boundsChanged=true;boundsMin=lo;boundsMax=hi;
    }
    source=&renderer;sourceVertices=gpu[index].vertexBuffer;sourceIndices=gpu[index].indexBuffer;
    vertexCount=uint32_t(current->size());indexCount=uint32_t(sub.indices.size());subMeshIndex=index;
    // Dynamic UV morphs invalidate static alpha proofs: keep every triangle and
    // let MASK candidates sample the current deformed UVs in the existing shader.
    compactionFinished=true;hasAlphaClassification=false;opacityPartition=false;needsAlphaCandidates=true;
    if(sub.materialIndex>=0&&size_t(sub.materialIndex)<mesh.materialTextures.size())baseColor=mesh.materialTextures[sub.materialIndex].diffuse;
    currentCpu=current;previousCpu=previous;
    if(first)LOGI("[HardwareRT] dynamic model BLAS: %s submesh=%u vertices=%u triangles=%u (slot-local refit)",renderer.GetModelPath().c_str(),index,vertexCount,indexCount/3);
    return true;
}
