#pragma once
#include "Rendering/TexturePool.h"
#include "Platform/Export.h"
#include "RendererBase.h"

class TexturePool;
struct RenderWorld;

struct MIKAN_API SkyboxUniformData {
    glm::mat4 view;
    glm::mat4 proj;
    glm::vec4 tintAndIntensity; // rgb = 颜色调制, a = 亮度
};

class MIKAN_API SkyboxRenderer : public BaseRenderer {
public:
    SkyboxRenderer();
    virtual ~SkyboxRenderer();

    virtual void Init(VkRenderPass renderPass) override;
    virtual void Render(VkCommandBuffer commandBuffer, const glm::mat4& view, const glm::mat4& proj) override;
    virtual void Cleanup() override;
    
    virtual VkPipeline GetPipeline() const override { return m_Pipeline.GetPipeline(); }
    virtual VkPipelineLayout GetPipelineLayout() const override { return m_Pipeline.GetLayout(); }

    VkDescriptorSet GetDescriptorSet() const { return m_DescriptorSet; }

    // ---- 场景树标准化控制（SkyboxComponent 每帧同步）----
    void SyncFromScene();                    // 从 ECS 读取 SkyboxComponent 并应用（无组件时不渲染,显示清屏色）
    void SyncFromRenderWorld(const RenderWorld& world); // 从渲染快照同步（渲染帧路径）
    void SetEnabled(bool enabled) { m_Enabled = enabled; }
    bool IsEnabled() const { return m_Enabled; }
    void SetTexture(const std::string& textureName);  // 切换 cubemap（TexturePool 注册名）
    void SetTint(const glm::vec3& tint) { m_Tint = tint; }
    void SetIntensity(float intensity) { m_Intensity = intensity; }
    const std::string& GetTextureName() const { return m_TextureName; }

private:
    bool CreateVertexBuffer();
    bool CreateIndexBuffer();
    void UpdateDescriptorSet();              // 用当前 m_TextureName 重写 descriptor set
    
    VkBuffer m_VertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_VertexBufferMemory = VK_NULL_HANDLE;
    VkBuffer m_IndexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_IndexBufferMemory = VK_NULL_HANDLE;
    uint32_t m_IndexCount = 0;

    TexturePool* m_TexturePool = nullptr;

    // 场景树同步状态（默认与旧行为一致：启用 + 引擎默认 skybox 纹理 + 无调制）
    bool m_Enabled = true;
    std::string m_TextureName = "skybox";
    glm::vec3 m_Tint = glm::vec3(1.0f);
    float m_Intensity = 1.0f;
};
