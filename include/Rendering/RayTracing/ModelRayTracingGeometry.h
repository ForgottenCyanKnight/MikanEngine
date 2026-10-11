#pragma once
#include "Rendering/RayTracing/AccelerationStructure.h"
class ModelRenderer;

// Static opaque model submesh geometry. One instance covers exactly one
// submesh (index recorded in subMeshIndex); multi-submesh models are
// supported by building one BLAS per opaque submesh and emitting one TLAS
// instance per (entity, submesh). Packed 32-byte vertices are shared by BLAS
// construction and hit shading; only staging and scratch are temporary.
class ModelRayTracingGeometry {
public:
    bool Matches(ModelRenderer& renderer,uint32_t subMeshIndex) const;
    bool RecordBuild(VkCommandBuffer cmd,ModelRenderer& renderer,uint32_t subMeshIndex);
    void ReleaseBuildInputs(); // submission fence completed
    VkDeviceAddress Address() const {return blas.Address();}
    VkDeviceAddress VertexAddress() const {return vertices.GetDeviceAddress();}
    VkDeviceAddress IndexAddress() const {return indices.GetDeviceAddress();}
    uint32_t VertexCount() const {return vertexCount;}
    uint32_t IndexCount() const {return indexCount;}
    glm::vec4 BaseColor() const {return baseColor;}
    const ModelRenderer* Source() const {return source;}
    uint32_t SubMeshIndex() const {return subMeshIndex;}
private:
    AccelerationStructure blas;
    VulkanBuffer vertices,indices,vertexStaging,indexStaging;
    const ModelRenderer* source=nullptr;
    VkBuffer sourceVertices=VK_NULL_HANDLE,sourceIndices=VK_NULL_HANDLE;
    uint32_t vertexCount=0,indexCount=0;
    uint32_t subMeshIndex=0;
    glm::vec4 baseColor{1};
};
