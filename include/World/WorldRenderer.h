#pragma once
// WorldRenderer.h - Vulkan 体素世界渲染器（全量流式版）
// 从 OpenGL 版 (D:\mikan engine) 迁移：
//   每帧：锁内 CollectVisibleFaces 收集可见 chunk 的所有 FaceInstance
//        → memcpy 上传到三个 host-visible 实例缓冲（不透明/植物/透明）
//        → 一次 vkCmdDraw 绘制全部面（quad 在 shader 展开）
//   简单可靠，draw call 最少；上传量为"总可见面数"（与改动无关）。
#include "Platform/Export.h"
#include "RendererBase.h"
#include "World/World.h"

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <vector>

// 世界渲染 push constants（mat4 + 2×vec4，96 字节，C++ 与 GLSL 对齐）
struct WorldPushConstants {
    glm::mat4 projView;    // 64 字节
    glm::vec4 cameraPos;   // 16 字节
    glm::vec4 sunDir;      // 16 字节（归一化方向）
};

// 世界渲染器：把 World 的 FaceInstance 列表用 instanced quad 一次性绘制
class MIKAN_API WorldRenderer : public BaseRenderer {
public:
    WorldRenderer();
    virtual ~WorldRenderer();

    virtual void Init(VkRenderPass renderPass) override;
    virtual void Render(VkCommandBuffer commandBuffer, const glm::mat4& view, const glm::mat4& proj) override;
    virtual void Cleanup() override;

    virtual VkPipeline GetPipeline() const override { return m_Pipeline.GetPipeline(); }
    virtual VkPipelineLayout GetPipelineLayout() const override { return m_Pipeline.GetLayout(); }

    // 关联世界（World::Update 由外部每帧驱动）
    void SetWorld(World* world) { m_World = world; }
    World* GetWorld() const { return m_World; }
    bool IsInitialized() const { return m_Initialized; }

    // 渲染世界（带视锥剔除）；frustumPlanes 用于剔除 chunk
    void RenderWorld(VkCommandBuffer commandBuffer, int width, int height,
                     const glm::mat4& view, const glm::mat4& proj,
                     const glm::vec3& cameraPos, const std::array<Plane, 6>& frustumPlanes);

    // 太阳光方向（归一化；默认 (0.4, 1.0, -0.6) 从上方斜照）
    void SetSunDirection(const glm::vec3& dir) { m_SunDir = glm::normalize(dir); }

private:
    // 退役缓冲（延迟销毁）：扩容时旧缓冲先保留 2 帧再销毁，避免 GPU 悬垂引用
    struct RetiredBuffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
        int framesLeft = 2;
    };

    bool CreatePipeline(VkRenderPass renderPass);
    bool CreateQuadBuffer();
    bool LoadAtlasTexture();
    bool CreateDescriptorSet();
    void EnsureInstanceCapacity(size_t opaqueCount, size_t alphaCount, size_t transparentCount);
    void RecycleRetiredBuffers();

    World* m_World = nullptr;
    bool m_Initialized = false;
    glm::vec3 m_SunDir = glm::normalize(glm::vec3(0.4f, 1.0f, -0.6f));

    // 缓冲
    VkBuffer m_QuadVertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_QuadVertexBufferMemory = VK_NULL_HANDLE;

    // 三个实例缓冲（不透明 / 植物 / 透明），host-visible mapped，动态扩容
    VkBuffer m_InstanceBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_InstanceBufferMemory = VK_NULL_HANDLE;
    VkDeviceSize m_InstanceCapacity = 0;
    void* m_InstanceMapped = nullptr;
    VkBuffer m_AlphaInstanceBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_AlphaInstanceBufferMemory = VK_NULL_HANDLE;
    VkDeviceSize m_AlphaInstanceCapacity = 0;
    void* m_AlphaInstanceMapped = nullptr;
    VkBuffer m_TransparentInstanceBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_TransparentInstanceBufferMemory = VK_NULL_HANDLE;
    VkDeviceSize m_TransparentInstanceCapacity = 0;
    void* m_TransparentInstanceMapped = nullptr;

    // 管线（不透明单面 + 植物双面 + 透明水混合）
    VulkanPipeline m_Pipeline;
    VulkanPipeline m_AlphaPipeline;
    VulkanPipeline m_TransparentPipeline;

    // 图集纹理描述符
    VulkanDescriptor m_Descriptor;
    VkDescriptorSet m_DescriptorSet = VK_NULL_HANDLE;
    VkImage m_AtlasImage = VK_NULL_HANDLE;
    VkDeviceMemory m_AtlasImageMemory = VK_NULL_HANDLE;
    VkImageView m_AtlasImageView = VK_NULL_HANDLE;
    VkSampler m_AtlasSampler = VK_NULL_HANDLE;
    uint32_t m_AtlasWidth = 0, m_AtlasHeight = 0;

    // 每帧暂存（可见面）
    std::vector<Chunk::FaceInstance> m_OpaqueFaces;
    std::vector<Chunk::FaceInstance> m_AlphaFaces;
    std::vector<Chunk::FaceInstance> m_TransparentFaces;

    // 退役缓冲队列
    std::vector<RetiredBuffer> m_RetiredBuffers;
};
