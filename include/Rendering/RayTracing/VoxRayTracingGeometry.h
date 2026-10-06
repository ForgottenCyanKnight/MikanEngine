#pragma once
#include "Rendering/RayTracing/AccelerationStructure.h"
#include <array>
class VoxRenderer;
class VoxRayTracingConverter {
public:
    ~VoxRayTracingConverter(){Cleanup();}
    bool Initialize();
    void Cleanup(); // after all geometry build descriptors have been released
    VkDescriptorSet Allocate(VkBuffer quads,VkBuffer positions,VkBuffer indices);
    void Free(VkDescriptorSet set);
    void Record(VkCommandBuffer cmd,VkDescriptorSet set,const VoxRenderer& renderer);
private:
    VkDescriptorSetLayout setLayout=VK_NULL_HANDLE;
    VkDescriptorPool pool=VK_NULL_HANDLE;
    VkPipelineLayout layout=VK_NULL_HANDLE;
    VkPipeline pipeline=VK_NULL_HANDLE;
};
struct VoxRayTracingRange { uint32_t firstQuad=0,quadCount=0,direction=0; };
// One BLAS per unique geometry. Instances share this object, not its temporary build inputs.
class VoxRayTracingGeometry {
public:
    ~VoxRayTracingGeometry(){ReleaseBuildInputs();}
    bool RecordBuild(VkCommandBuffer cmd,const VoxRenderer& renderer,VoxRayTracingConverter& converter);
    void ReleaseBuildInputs(); // call only after the build's submission fence completes
    VkDeviceAddress Address()const{return blas.Address();}
    VkBuffer QuadBuffer()const{return quads.GetBuffer();}
    const std::vector<VoxRayTracingRange>& Ranges()const{return ranges;}
    uint64_t Revision()const{return revision;}
    const VoxRenderer* Source()const{return source;}
private:
    AccelerationStructure blas;
    VulkanBuffer quads,positions,indices;
    VoxRayTracingConverter* converter=nullptr;
    VkDescriptorSet buildSet=VK_NULL_HANDLE;
    std::vector<VoxRayTracingRange> ranges;
    uint64_t revision=0;
    const VoxRenderer* source=nullptr;
};
