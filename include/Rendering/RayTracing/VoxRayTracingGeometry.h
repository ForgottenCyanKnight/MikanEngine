#pragma once
#include "Rendering/RayTracing/AccelerationStructure.h"
#include <array>
#include <memory>
#include "Rendering/VoxQuad.h"
#include "Rendering/RayTracing/VoxGeometryKey.h"
#include "Rendering/RayTracing/VoxQuadDag.h"
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
    bool RecordBuild(VkCommandBuffer cmd,const VoxRenderer& renderer,VoxRayTracingConverter& converter,
        mikan::rt::VoxGeometryKey key={},const mikan::rt::VoxDagSurface* subset=nullptr);
    bool RecordShared(const VoxRenderer& renderer,const std::shared_ptr<VoxRayTracingGeometry>& owner,const mikan::rt::VoxDagSurface* subset=nullptr);
    static mikan::rt::VoxGeometryKey MakeGeometryKey(const VoxRenderer& renderer,const mikan::rt::VoxDagSurface* subset=nullptr);
    const mikan::rt::VoxGeometryKey& GeometryKey()const{return geometryKey;}
    void ReleaseBuildInputs(); // call only after the build's submission fence completes
    VkDeviceAddress Address()const{return Blas()->Address();}
    const std::shared_ptr<AccelerationStructure>& Blas()const{return sharedGeometry?sharedGeometry->Blas():blas;}
    // Original build fence must have completed; caller retains retired source
    // until the copy submission and every old TLAS consumer finish.
    bool RecordCompaction(VkCommandBuffer cmd,VkDeviceSize& budget,std::shared_ptr<AccelerationStructure>& retired);
    VkBuffer QuadBuffer()const{return quads.GetBuffer();}
    // Scene shaders use the viewport's immutable surface tables. The standalone
    // validator still reads QuadBuffer(), so releasing it is explicitly opt-in.
    void ReleaseSceneBuildBuffer(){quads.Cleanup();}
    VkDeviceSize ResidentInputBytes()const{return quads.GetSize()+positions.GetSize()+indices.GetSize()+buildTransform.GetSize()+brickBounds.GetSize();}
    const std::vector<VoxRayTracingRange>& Ranges()const{return ranges;}
    uint64_t Revision()const{return revision;}
    const VoxRenderer* Source()const{return source;}
    const std::vector<VoxQuad>& Quads()const{return rtQuads;}
    const std::vector<VoxPlaneRange>& PlaneRanges()const{return planeRanges;}
    std::pair<uint32_t,uint32_t> DecodeQuad(uint32_t index)const;
    const std::vector<uint32_t>& Materials()const{return rtMaterials;}
    const std::vector<uint32_t>& Attributes()const{return attributes;}
    const std::shared_ptr<AccelerationStructure>& BrickBlas()const{return brickBlas;}
    VkDeviceAddress BrickAddress()const{return brickBlas->Address();}
    const std::vector<uint32_t>& BrickWords()const{return brickWords;}
    bool CompactionFinished()const{return sharedGeometry?sharedGeometry->CompactionFinished():compactionFinished;}

private:
    std::shared_ptr<AccelerationStructure> blas=std::make_shared<AccelerationStructure>();
    std::shared_ptr<VoxRayTracingGeometry> sharedGeometry;
    mikan::rt::VoxGeometryKey geometryKey;
    std::shared_ptr<AccelerationStructure> brickBlas=std::make_shared<AccelerationStructure>();
    VulkanBuffer brickBounds;
    std::vector<uint32_t> brickWords;
    bool buildCompleted=false,compactionFinished=false;
    VulkanBuffer quads,positions,indices,buildTransform;
    VoxRayTracingConverter* converter=nullptr;
    VkDescriptorSet buildSet=VK_NULL_HANDLE;
    void SetSurface(const VoxRenderer&,const mikan::rt::VoxDagSurface*);
    std::vector<VoxPlaneRange> planeRanges;
    std::vector<uint32_t> sourceQuads;
    std::vector<VoxRayTracingRange> ranges;
    std::vector<VoxQuad> rtQuads;
    std::vector<uint32_t> rtMaterials,attributes;

    uint64_t revision=0;
    const VoxRenderer* source=nullptr;
};
