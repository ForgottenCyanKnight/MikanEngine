#pragma once
#include "Rendering/RendererBase.h"
#include "Rendering/RayTracing/RayTracingScene.h"
#include "Rendering/RayTracing/RayTracingEnvironment.h"
#include "Rendering/Denoising/DiffuseDenoiser.h"
#include "Rendering/Denoising/DlssRayReconstruction.h"
#include "Rendering/Denoising/Fsr2.h"
#include <memory>
#include <unordered_map>
#include <array>
// Immutable scene inputs are shared across frames; images and histories remain per view.

// Bindless model material tables (albedo / metallicRoughness / emissive).
// Bindings 9/21/38 are runtime-sized sampler2D arrays (descriptor indexing,
// Vulkan 1.2 core); the layout declares kModelTextureBindlessCapacity slots
// per table but each frame only writes the unique textures actually used
// (PARTIALLY_BOUND keeps unwritten slots legal). Shader access uses
// nonuniformEXT(slot); the sRGB-decode flag lives in instance slot bit 31.
constexpr uint32_t kModelTextureBindlessCapacity=4096;

class RayTracingViewport {
public:
    RayTracingViewport();
    ~RayTracingViewport();
    VkImageView Record(VkCommandBuffer cmd,RayTracingScene& scene,uint32_t frameSlot,uint64_t serial,
        uint32_t viewSlot,uint32_t width,uint32_t height,const glm::mat4& view,const glm::mat4& proj,
        const glm::vec3& sun,const glm::vec3& radiance,const RayTracingEnvironment& environment);
    void Cleanup(); // device idle
private:
    // Published versions are read-only. Fenced slots retain old versions until GPU completion.
    struct SharedInputBuffer {
        std::shared_ptr<VulkanBuffer> resource;
        VkBuffer GetBuffer()const{return resource?resource->GetBuffer():VK_NULL_HANDLE;}
        VkDeviceSize GetSize()const{return resource?resource->GetSize():0;}
    };
    struct SceneInputs {
        SharedInputBuffer quads,ranges,instances,models,quadMaterials,emissiveLights,softwareBvh;
        std::vector<uint32_t> cachedQuads,cachedRanges,cachedInstances,cachedModels,cachedQuadMaterials,cachedEmissiveLights,cachedSoftwareBvh;
        std::vector<VkDescriptorImageInfo> albedoTextures,mrTextures,emissiveTextures,normalTextures; // bindless tables, sized to unique textures used
        uint64_t serial=UINT64_MAX,lightSignature=1469598103934665603ull;
        uint32_t lightCount=0;
        bool ready=false;
        const RayTracingScene* owner=nullptr;
        std::vector<RayTracingHitInstance> sourceHits;
        uint64_t version=0;
    };
    struct SceneInputSlot {
        uint64_t serial=UINT64_MAX;
        bool checked=false;
        std::shared_ptr<SceneInputs> inputs;
    };
    std::unordered_map<uint32_t,SceneInputSlot> sceneInputs;
    std::shared_ptr<SceneInputs> latestSceneInputs;
    uint64_t nextSceneInputVersion=0;
    struct Frame {
        const RayTracingScene* visibilityOwner=nullptr;
        VkAccelerationStructureKHR visibilityTlas=VK_NULL_HANDLE;
        uint64_t visibilityRevision=0;
        glm::mat4 visibilityViewProjection{1};
        uint32_t visibilityWidth=0,visibilityHeight=0;
        bool visibilityKnown=false,visibilityResult=false;
        bool outputInitialized=false;
        std::unordered_map<uint32_t,std::string> descriptorBindings;
        SceneInputs* sceneInputs=nullptr;
        VulkanImage output, taaGuide;
        VkImageView rrResolved=VK_NULL_HANDLE;
        VulkanImage diffuse, viewZ, normal, motion, material, specular, specularMaterial;
        VulkanBuffer temporal, previousTransforms;
        VulkanBuffer ddaGridBuffer;
        VulkanBuffer environment;

        VkDescriptorSet descriptor=VK_NULL_HANDLE;
        VkDescriptorSet compositeDescriptor=VK_NULL_HANDLE, taaDescriptor=VK_NULL_HANDLE;
        glm::vec4 taaJitter{0};
        bool taaReset=true;
        uint32_t width=0,height=0;
        bool initialized=false;
        VulkanBuffer irradianceReadback;
        bool irradianceStatsPending=false;
        VulkanBuffer primaryHits, primaryTransforms, activeTiles; // indirect command + compact 8x8 tile IDs
    };
    struct ViewHistory {
        uint64_t denoiseSceneSignature=0;
        uint32_t resolutionConfig=UINT32_MAX;
        std::unique_ptr<mikan::denoising::IDiffuseDenoiser> denoiser, specularDenoiser;
        std::unique_ptr<mikan::denoising::IRayReconstruction> rayReconstruction, superResolution;
        bool rrAttempted=false,srAttempted=false;VkExtent2D outputExtent{};
        std::unordered_map<uint64_t,glm::mat4> transforms;
        glm::mat4 view{1}, projection{1};
        uint64_t serial=UINT64_MAX, sceneSignature=0;
        uint32_t width=0,height=0,slots=0;
        bool attempted=false;
        // Two persistent accepted-frame banks plus generation/temporal scratch.
        // std430 PTReservoir: ten 16-byte records including the Enhanced chart.
        VulkanBuffer ptHistory[2], ptScratch[2];
        bool ptActive=false, ptValid=false;
        VulkanBuffer enhancedQueue, enhancedWork, enhancedDuplicates;
        // Immutable previous bank + copied write bank; one writer per hash slot.
        VulkanBuffer irradianceCache[2], irradianceLocks;
        bool irradianceActive=false, irradianceValid=false;
        uint32_t irradianceRead=0;
        static constexpr VkDeviceSize irradianceBytes=65536u*64u;
        uint32_t ptRead=0;
        uint64_t transportLightSignature=0;
        bool lightingValid=false;
        uint64_t giTransportSignature=0, lightingSignature=0;
        VulkanImage taaHistory[2];
        bool taaInitialized[2]={false,false};
        uint32_t taaRead=0;
        bool taaEnabled=true;
        glm::vec2 jitterUV{0};
    };
    struct alignas(16) TemporalParameters {
        glm::mat4 view{1}, previousViewProjection{1}, previousView{1};
        glm::vec4 jitter{0}; // current and previous sample offsets in UV
        glm::vec4 options{0};
        glm::vec4 giOptions{0}; // x ReSTIR GI enabled; y GI history reset
        glm::vec4 samplingParams{0}; // NEE tier/budget or Enhanced temporal/spatial/history bank, seed
    };
    static_assert(sizeof(TemporalParameters)==256);
    bool InitializeComposite();
    VkImageView ResolveTAA(VkCommandBuffer cmd,Frame& frame,ViewHistory& history,bool valid);
    VkDescriptorSetLayout taaSetLayout=VK_NULL_HANDLE;
    VkPipelineLayout taaLayout=VK_NULL_HANDLE;
    VkPipeline taaPipeline=VK_NULL_HANDLE;
    bool EnsureDenoisingFrame(Frame& frame,uint32_t width,uint32_t height);
    bool PrepareDenoising(Frame& frame,ViewHistory& history,const std::vector<RayTracingHitInstance>& hits,
        uint64_t serial,uint32_t width,uint32_t height,const glm::mat4& view,const glm::mat4& projection,uint32_t lightCount);
    void DenoiseAndComposite(VkCommandBuffer cmd,Frame& frame,ViewHistory& history,uint32_t slot,uint32_t viewSlot,
        const glm::mat4& view,const glm::mat4& projection,bool valid);
    std::unordered_map<uint32_t,std::unique_ptr<ViewHistory>> histories;
    VkDescriptorSetLayout compositeSetLayout=VK_NULL_HANDLE;
    VkPipelineLayout compositeLayout=VK_NULL_HANDLE;
    VkPipeline compositePipeline=VK_NULL_HANDLE;
    bool Initialize();
    bool EnsureEnhancedPipelines();
    std::array<uint32_t,27> ptSpecialization{};
    bool EnsureEnvironment(VkCommandBuffer cmd);
    bool PrepareModelInputs(SceneInputs& frame,const std::vector<RayTracingHitInstance>& hits,
        std::vector<uint32_t>& instances,std::vector<VkDescriptorImageInfo>& textures,
        std::vector<VkDescriptorImageInfo>& mrTextures,std::vector<VkDescriptorImageInfo>& emissiveTextures,std::vector<VkDescriptorImageInfo>& normalTextures);
    bool EnsureFrame(Frame& frame,uint32_t width,uint32_t height);
    bool Upload(VulkanBuffer& buffer,std::vector<uint32_t>& cached,const std::vector<uint32_t>& data);
    bool Upload(SharedInputBuffer& buffer,std::vector<uint32_t>& cached,const std::vector<uint32_t>& data);
    VkDescriptorSetLayout setLayout=VK_NULL_HANDLE;
    VkDescriptorPool pool=VK_NULL_HANDLE;
    VkPipelineLayout layout=VK_NULL_HANDLE;
    VkPipeline pipeline=VK_NULL_HANDLE;
    VkPipeline balancedPipeline=VK_NULL_HANDLE;
    VkPipeline performancePipeline=VK_NULL_HANDLE;
    VkPipeline skyPipeline=VK_NULL_HANDLE;
    std::array<VkPipeline,10> enhancedPipelines{};
    VulkanBuffer enhancedPairs;


    bool voxelDDAEnabled=false;


    bool softwareQuadBvhEnabled=false;
    uint32_t ddaRegistryVersion=0;


    VkImage ddaDummyImage=VK_NULL_HANDLE;


    VkDeviceMemory ddaDummyMemory=VK_NULL_HANDLE;


    VkImageView ddaDummyView=VK_NULL_HANDLE;
    VulkanBuffer stbnSamples,stbnUpload;
    bool stbnUploaded=false;
    bool stbnAvailable=false;
    VulkanImage fallbackSky;
    VulkanBuffer fallbackIrradiance;
    VkSampler fallbackSampler=VK_NULL_HANDLE;
    bool fallbackInitialized=false;
    std::unordered_map<uint64_t,std::unique_ptr<Frame>> frames;
};
