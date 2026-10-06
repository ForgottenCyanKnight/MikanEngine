#include "Rendering/RayTracing/AccelerationStructure.h"
#include "Core/VulkanRayTracingDevice.h"
#include "Core/VulkanContext.h"
#include <algorithm>
namespace {
void Barrier(VkCommandBuffer cmd,VkPipelineStageFlags src,VkPipelineStageFlags dst,VkAccessFlags read,VkAccessFlags write) {
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=read;barrier.dstAccessMask=write;
    vkCmdPipelineBarrier(cmd,src,dst,0,1,&barrier,0,nullptr,0,nullptr);
}
}
void RayTracingInputBarrier(VkCommandBuffer cmd,VkPipelineStageFlags source,VkAccessFlags access){Barrier(cmd,source,VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,access,VK_ACCESS_SHADER_READ_BIT);}
void RayTracingBuildBarrier(VkCommandBuffer cmd){Barrier(cmd,VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR|VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);}
void RayTracingQueryBarrier(VkCommandBuffer cmd){Barrier(cmd,VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);}
void AccelerationStructure::Cleanup() {
    if(handle && g_Device && GetRayTracingFunctions().destroy)GetRayTracingFunctions().destroy(g_Device,handle,g_Allocator);
    handle=VK_NULL_HANDLE;address=0;storage.Cleanup();scratch.Cleanup();primitiveCounts.clear();
}
void AccelerationStructure::ReleaseScratch(){scratch.Cleanup();}
bool AccelerationStructure::RecordBuild(VkCommandBuffer cmd,VkAccelerationStructureTypeKHR requestedType,
    std::span<const VkAccelerationStructureGeometryKHR> geometries,std::span<const VkAccelerationStructureBuildRangeInfoKHR> ranges,
    VkBuildAccelerationStructureFlagsKHR requestedFlags,bool update) {
    const auto& cap=GetRayTracingDeviceCapabilities();const auto& fn=GetRayTracingFunctions();
    if(!cap.accelerationStructure || !cmd || geometries.empty() || geometries.size()!=ranges.size() || geometries.size()>cap.limits.maxGeometryCount)return false;
    std::vector<uint32_t> counts;uint64_t total=0;
    for(const auto& range:ranges){counts.push_back(range.primitiveCount);total+=range.primitiveCount;}
    if(!total || (requestedType==VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR && total>cap.limits.maxPrimitiveCount) ||
        (requestedType==VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR && total>cap.limits.maxInstanceCount))return false;
    if(update && (!handle || !(flags&VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR) || type!=requestedType || flags!=requestedFlags || counts!=primitiveCounts))return false;
    VkAccelerationStructureBuildGeometryInfoKHR build{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    build.type=requestedType;build.flags=requestedFlags;build.mode=update?VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR:VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    build.geometryCount=uint32_t(geometries.size());build.pGeometries=geometries.data();
    VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    fn.getBuildSizes(g_Device,VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,&build,counts.data(),&sizes);
    if(!update) {
        Cleanup();
        if(!storage.Create(sizes.accelerationStructureSize,VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))return false;
        VkAccelerationStructureCreateInfoKHR create{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};create.buffer=storage.GetBuffer();create.size=sizes.accelerationStructureSize;create.type=requestedType;
        if(fn.create(g_Device,&create,g_Allocator,&handle)!=VK_SUCCESS){Cleanup();return false;}
    }
    const VkDeviceSize alignment=std::max(1u,cap.limits.minAccelerationStructureScratchOffsetAlignment);
    const VkDeviceSize needed=update?sizes.updateScratchSize:sizes.buildScratchSize;
    if(scratch.GetSize()<needed+alignment){scratch.Cleanup();if(!scratch.Create(needed+alignment,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))return false;}
    const auto raw=scratch.GetDeviceAddress();if(!raw)return false;
    build.scratchData.deviceAddress=((raw+alignment-1)/alignment)*alignment;
    build.dstAccelerationStructure=handle;build.srcAccelerationStructure=update?handle:VK_NULL_HANDLE;
    std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> rangePointers{ranges.data()};
    fn.build(cmd,1,&build,rangePointers.data());
    VkAccelerationStructureDeviceAddressInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};info.accelerationStructure=handle;
    address=fn.getAddress(g_Device,&info);type=requestedType;flags=requestedFlags;primitiveCounts=std::move(counts);
    return address!=0;
}
