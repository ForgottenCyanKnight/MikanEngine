#pragma once
#include "Rendering/RendererBase.h"
#include <span>
class AccelerationStructure {
public:
    AccelerationStructure()=default;
    ~AccelerationStructure(){Cleanup();}
    AccelerationStructure(const AccelerationStructure&)=delete;
    AccelerationStructure& operator=(const AccelerationStructure&)=delete;
    // Recording only; call outside render passes on a graphics/compute-capable queue.
    // Reuse/update and Cleanup require completion of all previous consumers.
    bool RecordBuild(VkCommandBuffer cmd,VkAccelerationStructureTypeKHR type,
        std::span<const VkAccelerationStructureGeometryKHR> geometries,
        std::span<const VkAccelerationStructureBuildRangeInfoKHR> ranges,
        VkBuildAccelerationStructureFlagsKHR flags,bool update=false);
    void ReleaseScratch(); // after build completion
    void Cleanup();
    VkAccelerationStructureKHR Handle()const{return handle;}
    VkDeviceAddress Address()const{return address;}
private:
    VulkanBuffer storage,scratch;
    VkAccelerationStructureKHR handle=VK_NULL_HANDLE;
    VkDeviceAddress address=0;
    VkAccelerationStructureTypeKHR type=VK_ACCELERATION_STRUCTURE_TYPE_GENERIC_KHR;
    VkBuildAccelerationStructureFlagsKHR flags=0;
    std::vector<uint32_t> primitiveCounts;
};
// Input writes -> AS build, BLAS -> TLAS, and TLAS -> shader queries are distinct dependencies.
void RayTracingInputBarrier(VkCommandBuffer cmd,VkPipelineStageFlags source,VkAccessFlags access);
void RayTracingBuildBarrier(VkCommandBuffer cmd);
void RayTracingQueryBarrier(VkCommandBuffer cmd);
