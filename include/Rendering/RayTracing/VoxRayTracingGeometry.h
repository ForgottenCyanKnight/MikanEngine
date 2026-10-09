#pragma once
#include "Rendering/RayTracing/AccelerationStructure.h"
#include <array>
#include <memory>
#include "Rendering/VoxQuad.h"
class VoxRenderer;
struct VoxRtQuad {uint32_t geometry,appearance;};
struct VoxRayTracingRange { uint32_t firstQuad=0,quadCount=0,direction=0; };
class VoxRayTracingConverter {
public:
    ~VoxRayTracingConverter(){Cleanup();}
    bool Initialize();
    void Cleanup(); // after all geometry build descriptors have been released
    VkDescriptorSet Allocate(VkBuffer quads,VkBuffer positions,VkBuffer indices);
    void Free(VkDescriptorSet set);
    void Record(VkCommandBuffer cmd,VkDescriptorSet set,const VoxRenderer& renderer,const std::vector<VoxRayTracingRange>& ranges,uint32_t encoding);
private:
    VkDescriptorSetLayout setLayout=VK_NULL_HANDLE;
    VkDescriptorPool pool=VK_NULL_HANDLE;
    VkPipelineLayout layout=VK_NULL_HANDLE;
    VkPipeline pipeline=VK_NULL_HANDLE;
};
// One BLAS per unique geometry. Instances share this object, not its temporary build inputs.
class VoxRayTracingGeometry {
public:
    ~VoxRayTracingGeometry(){ReleaseBuildInputs();}
    bool RecordBuild(VkCommandBuffer cmd,const VoxRenderer& renderer,VoxRayTracingConverter& converter);
    void ReleaseBuildInputs(); // call only after the build's submission fence completes
    VkDeviceAddress Address()const{return blas->Address();}
    const std::shared_ptr<AccelerationStructure>& Blas()const{return blas;}
    // Original build fence must have completed; caller retains retired source
    // until the copy submission and every old TLAS consumer finish.
    bool RecordCompaction(VkCommandBuffer cmd,VkDeviceSize& budget,std::shared_ptr<AccelerationStructure>& retired);
    VkBuffer QuadBuffer()const{return quads.GetBuffer();}
    const std::vector<VoxRayTracingRange>& Ranges()const{return ranges;}
    uint64_t Revision()const{return revision;}
    const VoxRenderer* Source()const{return source;}
    const std::vector<VoxQuad>& Quads()const{return rtQuads;}
    const std::vector<uint32_t>& Materials()const{return rtMaterials;}
    const std::vector<uint32_t>& Attributes()const{return attributes;}

private:
    std::shared_ptr<AccelerationStructure> blas=std::make_shared<AccelerationStructure>();
    bool buildCompleted=false,compactionFinished=false;
    VulkanBuffer quads,positions,indices,buildTransform;
    VoxRayTracingConverter* converter=nullptr;
    VkDescriptorSet buildSet=VK_NULL_HANDLE;
    std::vector<VoxRayTracingRange> ranges;
    std::vector<VoxQuad> rtQuads;
    std::vector<uint32_t> rtMaterials,attributes;

    uint64_t revision=0;
    const VoxRenderer* source=nullptr;
};
