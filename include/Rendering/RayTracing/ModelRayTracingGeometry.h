#pragma once
#include "Rendering/RayTracing/AccelerationStructure.h"
#include <memory>
class ModelRenderer;
struct TextureAlphaClassification;

// Static opaque model submesh geometry. One instance covers exactly one
// submesh (index recorded in subMeshIndex); multi-submesh models are
// supported by building one BLAS per opaque submesh and emitting one TLAS
// instance per (entity, submesh). Packed 32-byte vertices are shared by BLAS
// construction and hit shading; only staging and scratch are temporary.
class ModelRayTracingGeometry {
public:
    bool Matches(ModelRenderer& renderer,uint32_t subMeshIndex,bool allowPartition=true) const;
    bool RecordBuild(VkCommandBuffer cmd,ModelRenderer& renderer,uint32_t subMeshIndex,bool allowPartition=true);
    void ReleaseBuildInputs(); // submission fence completed
    bool RecordCompaction(VkCommandBuffer cmd,VkDeviceSize& budget,std::shared_ptr<AccelerationStructure>& retired);
    const std::shared_ptr<AccelerationStructure>& Blas() const {return blas;}
    VkDeviceAddress Address() const {return blas->Address();}
    VkDeviceAddress VertexAddress() const {return vertices.GetDeviceAddress();}
    VkDeviceAddress IndexAddress() const {return indices.GetDeviceAddress();}
    uint32_t VertexCount() const {return vertexCount;}
    uint32_t IndexCount() const {return indexCount;}
    glm::vec4 BaseColor() const {return baseColor;}
    const ModelRenderer* Source() const {return source;}
    uint32_t SubMeshIndex() const {return subMeshIndex;}
    bool CompactionFinished() const {return compactionFinished;}
    bool HasAlphaClassification() const {return hasAlphaClassification;}
    bool NeedsAlphaCandidates() const {return needsAlphaCandidates;}
    bool HasOpacityPartition() const {return opacityPartition;}
    bool FullyTransparent() const {return fullyTransparent;}
    uint32_t OpaquePrimitiveCount() const {return opaquePrimitiveCount;}
    bool DropsPrimitive(uint32_t primitive) const {return (opacityPartition||fullyTransparent) && primitive<alphaStates.size() && alphaStates[primitive]==2;}
private:
    std::shared_ptr<AccelerationStructure> blas=std::make_shared<AccelerationStructure>();
    bool buildCompleted=false,compactionFinished=false;
    VulkanBuffer vertices,indices,vertexStaging,indexStaging;
    const ModelRenderer* source=nullptr;
    VkBuffer sourceVertices=VK_NULL_HANDLE,sourceIndices=VK_NULL_HANDLE;
    uint32_t vertexCount=0,indexCount=0;
    uint32_t subMeshIndex=0;
    glm::vec4 baseColor{1};
    std::shared_ptr<const TextureAlphaClassification> alphaClassification;
    bool hasAlphaClassification=false,needsAlphaCandidates=true;
    float alphaLodBias=0;
    VulkanBuffer buildIndices,buildIndexStaging;
    std::vector<uint8_t> alphaStates;
    bool opacityPartition=false,fullyTransparent=false;
    bool partitionAllowed=true;
    uint32_t opaquePrimitiveCount=0;
};
