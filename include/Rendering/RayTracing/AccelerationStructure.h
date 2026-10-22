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
    VkDeviceSize StorageBytes()const{return storage.GetSize();}
    VkDeviceSize ScratchBytes()const{return scratch.GetSize();}
    // Caller must observe the build submission fence before polling/discarding.
    VkResult ReadCompactedSize(VkDeviceSize& bytes)const;
    void DiscardCompactionQuery();
    bool RecordCompactCopy(VkCommandBuffer cmd,const AccelerationStructure& source,VkDeviceSize bytes);
    void ConfigureDiskCache(std::span<const uint32_t> geometryKey);
    void InheritDiskCache(const AccelerationStructure& source);
    bool RecordLoadDiskCache(VkCommandBuffer cmd);
    bool RecordSaveDiskCache(VkCommandBuffer cmd,uint64_t serial);
    void CompleteDiskCacheAfterFence(uint64_t completedSerial);
private:
    std::string diskPath;
    std::vector<uint32_t> diskKey;
    VulkanBuffer cacheUpload,cacheReadback;
    VkQueryPool cacheQuery=VK_NULL_HANDLE;
    VkDeviceSize cacheBytes=0,cacheOffset=0;
    uint64_t cacheSerial=UINT64_MAX;
    enum class CacheStage { Idle, Query, Ready, Copy, Done } cacheStage=CacheStage::Idle;
    VulkanBuffer storage,scratch;
    VkAccelerationStructureKHR handle=VK_NULL_HANDLE;
    VkDeviceAddress address=0;
    VkAccelerationStructureTypeKHR type=VK_ACCELERATION_STRUCTURE_TYPE_GENERIC_KHR;
    VkBuildAccelerationStructureFlagsKHR flags=0;
    std::vector<uint32_t> primitiveCounts;
    VkQueryPool compactQuery=VK_NULL_HANDLE;
};
// Input writes -> AS build, BLAS -> TLAS, and TLAS -> shader queries are distinct dependencies.
void RayTracingInputBarrier(VkCommandBuffer cmd,VkPipelineStageFlags source,VkAccessFlags access);
void RayTracingBuildBarrier(VkCommandBuffer cmd);
void RayTracingQueryBarrier(VkCommandBuffer cmd);
