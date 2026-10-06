#pragma once
#include "Platform/Export.h"
#include "Rendering/RayTracing/VoxRayTracingGeometry.h"
#include "Rendering/RayTracing/ModelRayTracingGeometry.h"
#include <memory>
#include <unordered_map>
#include <string>
struct RenderWorld;
class ModelRenderer;
// The instance custom index addresses this frame's hit table, not an ECS ID truncated to 24 bits.
struct RayTracingHitInstance {
    uint32_t entity=0;
    glm::mat4 model{1};
    glm::vec4 color{1};
    std::shared_ptr<VoxRayTracingGeometry> geometry;
    uint32_t materialFlags=0; // bit 0: ideal mirror (metallic=1, roughness=0); bit 1: emissive, bits[31:16] emissive strength half
    std::shared_ptr<ModelRayTracingGeometry> modelGeometry;
    VkImageView albedoView=VK_NULL_HANDLE;
    VkSampler albedoSampler=VK_NULL_HANDLE;
    VkFormat albedoFormat=VK_FORMAT_UNDEFINED;
    VkImageView mrView=VK_NULL_HANDLE;  // glTF metallicRoughness（g=roughness b=metallic）
    VkSampler mrSampler=VK_NULL_HANDLE;
    VkImageView emissiveView=VK_NULL_HANDLE;
    VkSampler emissiveSampler=VK_NULL_HANDLE;
    VkFormat emissiveFormat=VK_FORMAT_UNDEFINED;
    glm::vec3 emissiveFactor{0};
    glm::vec4 materialParams{0,1,0,0}; // x metallic, y roughness (vox stays diffuse; mirror comes from flags/quad word)
};
class MIKAN_API RayTracingScene {
public:
    RayTracingScene()=default;
    RayTracingScene(const RayTracingScene&)=delete;
    RayTracingScene& operator=(const RayTracingScene&)=delete;
    ~RayTracingScene(){Cleanup();}
    bool SetEnabled(bool value);
    bool IsEnabled()const{return enabled;}
    // frameSlot must have passed its submission fence. Multiple viewports share the same serial.
    void Prepare(VkCommandBuffer cmd,const RenderWorld& world,
        const std::unordered_map<std::string,std::unique_ptr<VoxRenderer>>& renderers,
        const std::unordered_map<std::string,std::unique_ptr<ModelRenderer>>& models,
        uint32_t frameSlot,uint64_t serial);
    VkAccelerationStructureKHR GetTlas(uint32_t frameSlot,uint64_t serial)const;
    const std::vector<RayTracingHitInstance>* GetHitInstances(uint32_t frameSlot,uint64_t serial)const;
    void Cleanup(); // all build/query submissions must have completed
private:
    struct Frame {
        AccelerationStructure tlas;
        VulkanBuffer instances;
        std::vector<VkAccelerationStructureInstanceKHR> cachedInstances;
        std::vector<RayTracingHitInstance> hitInstances;
        std::vector<std::shared_ptr<VoxRayTracingGeometry>> builds;
        std::vector<std::shared_ptr<ModelRayTracingGeometry>> modelBuilds;
        uint64_t serial=UINT64_MAX;
        bool ready=false;
    };
    bool enabled=false;
    bool AppendStaticModels(VkCommandBuffer cmd,const RenderWorld& world,
        const std::unordered_map<std::string,std::unique_ptr<ModelRenderer>>& models,
        Frame& frame,std::vector<VkAccelerationStructureInstanceKHR>& instances);
    VoxRayTracingConverter converter;
    std::unordered_map<std::string,std::shared_ptr<VoxRayTracingGeometry>> assets;
    std::unordered_map<std::string,std::shared_ptr<ModelRayTracingGeometry>> modelAssets;
    std::unordered_map<uint32_t,std::unique_ptr<Frame>> frames;
};
