#pragma once

#include "Platform/Export.h"
#include "AABB.h"
#include "RendererBase.h"
#include "ECS/Components.h"
#include "ECS/Types.h"

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct RenderWorld;

// 地形采用“固定 patch 顶点 + chunk 实例”的输入布局。
// Y 由顶点着色器从 16-bit heightmap 采样得到，避免每个 chunk 重复存储高度顶点。
// UV 直接由 position 推导；LOD 边界在顶点着色器折叠，不生成地下裙边，因此顶点保持 8 bytes。
struct MIKAN_API TerrainVertex {
    glm::vec2 position = glm::vec2(0.0f); // patch 内归一化 X/Z 坐标 [0, 1]
};

// 每个实例代表一个可见 terrain chunk。
// originSize.xy 是地形局部空间的 X/Z 原点，originSize.zw 是 chunk 尺寸。
// uvRect.xy 保存整数 chunk 网格坐标 (x, z)，uvRect.z 保存 chunkCount；
// 顶点着色器用它重建全局 [0, 1] 坐标，避免相邻 chunk 分别做
// "原点 + 尺寸" 浮点运算后在边界产生不同结果。
struct MIKAN_API TerrainChunkInstance {
    glm::vec4 originSize = glm::vec4(0.0f); // localOrigin.x, localOrigin.z, size.x, size.z
    glm::vec4 uvRect = glm::vec4(0.0f);     // chunkX, chunkZ, chunkCount, unused
    glm::vec4 params = glm::vec4(0.0f);     // lod, packed edge LOD deltas, edge intervals, reserved
};

static_assert(sizeof(TerrainVertex) == sizeof(float) * 2, "TerrainVertex must stay 8 bytes");
static_assert(sizeof(TerrainChunkInstance) == sizeof(float) * 12, "TerrainChunkInstance must stay 48 bytes");

struct MIKAN_API TerrainChunk {
    int x = 0;
    int z = 0;
    glm::vec2 origin = glm::vec2(0.0f);
    glm::vec2 size = glm::vec2(0.0f);
    glm::vec2 uvOrigin = glm::vec2(0.0f);
    glm::vec2 uvSize = glm::vec2(0.0f);
    AABB localBounds;
};

// CPU 侧 chunk 管理：规则网格、距离/视锥裁剪和三档 LOD 分类。
// chunk 本身不持有 GPU 资源，因此可以在编辑器属性改变时低成本重建。
class MIKAN_API TerrainChunkManager {
public:
    void Configure(const glm::vec2& worldSize, int chunkCount,
                   float minHeight, float maxHeight,
                   float viewDistance, float lod0Distance, float lod1Distance,
                   int maxLod);

    void UpdateVisibility(const glm::vec3& cameraPosition,
                          const glm::mat4& modelMatrix,
                          const std::array<Plane, 6>& frustumPlanes,
                          bool useFrustumCulling);

    const std::vector<TerrainChunk>& GetChunks() const { return m_Chunks; }
    const std::vector<TerrainChunkInstance>& GetVisible(int lod) const;
    size_t GetVisibleCount() const;
    int GetChunkCount() const { return m_ChunkCount; }

private:
    std::vector<TerrainChunk> m_Chunks;
    std::array<std::vector<TerrainChunkInstance>, 3> m_Visible;
    glm::vec2 m_WorldSize = glm::vec2(1.0f);
    int m_ChunkCount = 1;
    float m_ViewDistance = 2000.0f;
    float m_Lod0Distance = 200.0f;
    float m_Lod1Distance = 500.0f;
    int m_MaxLod = 2;
};

struct MIKAN_API TerrainPatch {
    VulkanBuffer vertexBuffer;
    VulkanBuffer indexBuffer;
    uint32_t indexCount = 0;
    uint32_t resolution = 0;

    void Cleanup() {
        vertexBuffer.Cleanup();
        indexBuffer.Cleanup();
        indexCount = 0;
        resolution = 0;
    }
};

// Terrain UBO：当前/上一帧矩阵和预计算法线矩阵用于稳定运动矢量与非均匀缩放，
// 高度/材质参数保持 16-byte 对齐。
struct MIKAN_API TerrainUniformData {
    glm::mat4 projView = glm::mat4(1.0f);
    glm::mat4 prevProjView = glm::mat4(1.0f);
    glm::mat4 model = glm::mat4(1.0f);
    glm::mat4 prevModel = glm::mat4(1.0f);
    glm::mat4 normalMatrix = glm::mat4(1.0f);
    glm::vec4 heightParams = glm::vec4(1.0f, 0.0f, 1.0f, 1.0f);
    glm::vec4 materialParams = glm::vec4(1.0f, 0.0f, 1.0f, 0.0f);
    glm::vec4 cameraPosition = glm::vec4(0.0f);
    glm::vec4 taaJitter = glm::vec4(0.0f);
};

static_assert(sizeof(TerrainUniformData) % 16 == 0, "TerrainUniformData must be 16-byte aligned");

class MIKAN_API TerrainRenderer {
public:
    TerrainRenderer() = default;
    ~TerrainRenderer();
    TerrainRenderer(const TerrainRenderer&) = delete;
    TerrainRenderer& operator=(const TerrainRenderer&) = delete;
    TerrainRenderer(TerrainRenderer&&) = delete;
    TerrainRenderer& operator=(TerrainRenderer&&) = delete;

    // renderPass 必须是当前 SceneRenderTarget 的 render pass；swapchain 重建时可重复调用。
    void Init(VkRenderPass renderPass);
    void Cleanup();

    // 在 geometry pass 前完成场景遍历、资源准备和 chunk 可见性更新。
    void Prepare(const std::vector<ECS::Entity>& rootEntities,
                const glm::vec3& cameraPosition,
                const std::array<Plane, 6>& frustumPlanes,
                bool useFrustumCulling);

    // Snapshot-driven preparation used by the frame renderer.  The legacy
    // rootEntities overload remains for tools that still own ECS extraction.
    void Prepare(const RenderWorld& world,
                 const glm::vec3& cameraPosition,
                 const std::array<Plane, 6>& frustumPlanes,
                 bool useFrustumCulling);

    // z-prepass 早于 SceneRenderer::PrepareFrame，因此提供独立的场景收集入口。
    void PrepareFromScene(const glm::vec3& cameraPosition,
                          const std::array<Plane, 6>& frustumPlanes,
                          bool useFrustumCulling);

    void Render(VkCommandBuffer commandBuffer, int width, int height,
                const glm::mat4& projView,
                const glm::mat4& prevProjView,
                const glm::vec3& cameraPosition);

    void RenderDepthPrepass(VkCommandBuffer commandBuffer, int width, int height,
                            const glm::mat4& projView,
                            const glm::vec3& cameraPosition);

    // CSM depth-only caster path. The CSM render pass is owned by
    // CascadeShadowRenderer, so this pipeline is created lazily for that pass.
    bool EnsureCsmDepthPipeline(VkRenderPass shadowRenderPass);
    void RenderCsmDepth(VkCommandBuffer commandBuffer, int width, int height,
                        const glm::mat4& shadowProjView,
                        const glm::vec3& cameraPosition);

    bool IsInitialized() const { return m_Pipeline.GetPipeline() != VK_NULL_HANDLE; }
    size_t GetTerrainCount() const { return m_Resources.size(); }
    size_t GetVisibleChunkCount() const { return m_VisibleChunkCount; }

private:
    static constexpr uint32_t kFramesInFlight = 3;

    struct Resource {
        ECS::Entity entity = ECS::INVALID_ENTITY;
        ECS::TerrainComponent settings;
        TerrainChunkManager chunks;

        glm::mat4 model = glm::mat4(1.0f);
        glm::mat4 previousModel = glm::mat4(1.0f);
        bool hasPreviousModel = false;

        std::array<TerrainPatch, 3> patches;
        std::array<VulkanBuffer, kFramesInFlight> instanceBuffers;
        // CSM commands are recorded before the main geometry pass. Keep a
        // separate instance stream so the later main-pass upload cannot
        // overwrite the terrain caster data before GPU execution.
        std::array<VulkanBuffer, kFramesInFlight> csmInstanceBuffers;
        std::array<std::unique_ptr<VulkanBuffer>, kFramesInFlight> uniformBuffers;
        std::array<VkDescriptorSet, kFramesInFlight> descriptorSets{};
        size_t instanceCapacity = 0;
        size_t csmInstanceCapacity = 0;

        std::string heightmapKey;
        std::array<std::string, 4> layerKeys{};
        std::string controlKey;
        bool useControlMap = false;
        std::vector<std::string> ownedTextureKeys;
    };

    void CollectTerrainEntities(ECS::Entity entity, std::vector<ECS::Entity>& entities) const;
    Resource* EnsureResource(ECS::Entity entity, const ECS::TerrainComponent& settings);
    std::unique_ptr<Resource> CreateResource(ECS::Entity entity, const ECS::TerrainComponent& settings);
    void DestroyResource(Resource& resource);
    bool SettingsEqual(const ECS::TerrainComponent& lhs, const ECS::TerrainComponent& rhs) const;

    bool EnsureWhiteFallback();
    bool CreateDescriptorResources();
    void DestroyDescriptorResources();
    bool CreatePipelines();
    bool BuildPatch(TerrainPatch& patch, uint32_t resolution);
    bool CreateInstanceBuffers(Resource& resource, size_t capacity);
    bool EnsureCsmInstanceCapacity(Resource& resource, size_t visibleCount);
    bool CreateUniformBuffers(Resource& resource);
    bool CreateDescriptorSets(Resource& resource);
    bool EnsureInstanceCapacity(Resource& resource, size_t visibleCount);

    void UpdateUniform(Resource& resource,
                       const glm::mat4& projView,
                       const glm::mat4& prevProjView,
                       const glm::vec3& cameraPosition);
    void RenderInternal(VkCommandBuffer commandBuffer, int width, int height,
                        const glm::mat4& projView,
                        const glm::mat4& prevProjView,
                        const glm::vec3& cameraPosition,
                        bool depthOnly);

    static std::string MakeTextureKey(ECS::Entity entity, const char* slot);
    static const std::string& GetLayerPath(const ECS::TerrainComponent& settings, int layer);

    VkRenderPass m_RenderPass = VK_NULL_HANDLE;
    VulkanPipeline m_Pipeline;
    VulkanPipeline m_WireframePipeline;
    VulkanPipeline m_DepthPipeline;
    VulkanPipeline m_CsmDepthPipeline;
    VkRenderPass m_CsmRenderPass = VK_NULL_HANDLE;

    VkDescriptorSetLayout m_DescriptorLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_DescriptorPool = VK_NULL_HANDLE;
    bool m_OwnedWhiteFallback = false;

    std::unordered_map<ECS::Entity, std::unique_ptr<Resource>> m_Resources;
    std::vector<Resource*> m_PreparedResources;
    size_t m_VisibleChunkCount = 0;
    bool m_PrimitiveRestartSupported = false;
};
