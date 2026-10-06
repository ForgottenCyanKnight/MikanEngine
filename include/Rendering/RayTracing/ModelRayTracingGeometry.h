#pragma once
#include "Rendering/RayTracing/AccelerationStructure.h"
class ModelRenderer;

// Static opaque single-submesh geometry. Packed 32-byte vertices are shared by
// BLAS construction and hit shading; only staging and scratch are temporary.
class ModelRayTracingGeometry {
public:
    bool Matches(ModelRenderer& renderer) const;
    bool RecordBuild(VkCommandBuffer cmd,ModelRenderer& renderer);
    void ReleaseBuildInputs(); // submission fence completed
    VkDeviceAddress Address() const {return blas.Address();}
    VkDeviceAddress VertexAddress() const {return vertices.GetDeviceAddress();}
    VkDeviceAddress IndexAddress() const {return indices.GetDeviceAddress();}
    uint32_t VertexCount() const {return vertexCount;}
    uint32_t IndexCount() const {return indexCount;}
    glm::vec4 BaseColor() const {return baseColor;}
    const ModelRenderer* Source() const {return source;}
private:
    AccelerationStructure blas;
    VulkanBuffer vertices,indices,vertexStaging,indexStaging;
    const ModelRenderer* source=nullptr;
    VkBuffer sourceVertices=VK_NULL_HANDLE,sourceIndices=VK_NULL_HANDLE;
    uint32_t vertexCount=0,indexCount=0;
    glm::vec4 baseColor{1};
};
