#pragma once
#include "Rendering/ModelLoader.h"
#include <span>
#include <memory>
#include <algorithm>
#include <vulkan/vulkan.h>
class ModelRenderer;
#include <cmath>

namespace mikan::rt {
// Match model.vert: packed UNORM weights, clamped bone IDs, linear blend,
// and the same normal/tangent transform. No dependency on raster GPU buffers.
inline Vertex SkinRayTracingVertex(const Vertex& source,std::span<const glm::mat4> bones){
    Vertex result=source;
    const auto weights=glm::vec4(source.BoneWeights)*(1.0f/255.0f);
    if(bones.empty()||glm::dot(weights,glm::vec4(1))<=0.001f)return result;
    glm::mat4 skin(0);
    for(size_t i=0;i<4;++i)if(weights[i]>0){
        const auto index=std::min(size_t(source.BoneIDs[i]),std::min(size_t(MAX_BONES),bones.size())-1);
        skin+=weights[i]*bones[index];
    }
    result.Position=glm::vec3(skin*glm::vec4(source.Position,1));
    const auto n=UnpackSnorm3(source.Normal),t=UnpackSnorm3(source.Tangent);
    auto safeNormalize=[](glm::vec3 v,glm::vec3 fallback){return glm::dot(v,v)>1e-12f?glm::normalize(v):fallback;};
    const auto newN=safeNormalize(glm::mat3(skin)*n,n);
    const auto newT=safeNormalize(glm::mat3(skin)*t,t);
    const auto b=glm::cross(n,t)*(source.Tangent.w>=0?1.0f:-1.0f);
    const auto newB=glm::mat3(skin)*b;
    result.Normal=PackSnorm3(newN);
    result.Tangent=PackSnorm3(newT,glm::dot(glm::cross(newN,newT),newB)>=0?1.0f:-1.0f);
    result.BoneIDs=glm::u8vec4(255);result.BoneWeights=glm::u8vec4(0);
    return result;
}
struct RayTracingDeformedPose {
    const ModelRenderer* source=nullptr;
    const ModelRenderer* poseSource=nullptr;
    VkBuffer sourceBuffer=VK_NULL_HANDLE;
    uint64_t serial=UINT64_MAX,mmdRevision=UINT64_MAX,revision=0;
    std::vector<glm::mat4> matrices;
    std::vector<std::shared_ptr<const std::vector<Vertex>>> current,previous;
};
}
