#pragma once
#include "Platform/Export.h"

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <vector>
#include <string>

class MIKAN_API VulkanBuffer {
public:
    VulkanBuffer() = default;
    ~VulkanBuffer() { Cleanup(); }
    
    bool Create(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties);
    void Cleanup();
    
    VkBuffer GetBuffer() const { return m_Buffer; }
    VkDeviceMemory GetMemory() const { return m_Memory; }
    void* GetMappedPtr() const { return m_Mapped; }
    VkDeviceSize GetSize() const { return m_Size; }
    
    void Map();
    void Unmap();
    void Write(const void* data, VkDeviceSize size, VkDeviceSize offset = 0);

private:
    VkBuffer m_Buffer = VK_NULL_HANDLE;
    VkDeviceMemory m_Memory = VK_NULL_HANDLE;
    void* m_Mapped = nullptr;
    VkDeviceSize m_Size = 0;
};

class MIKAN_API VulkanImage {
public:
    VulkanImage() = default;
    ~VulkanImage() { Cleanup(); }
    
    bool Create(uint32_t width, uint32_t height, VkFormat format, 
        VkImageTiling tiling, VkImageUsageFlags usage, VkMemoryPropertyFlags properties);
    bool CreateView(VkFormat format, VkImageAspectFlags aspectFlags);
    void Cleanup();
    
    VkImage GetImage() const { return m_Image; }
    VkImageView GetView() const { return m_View; }
    VkDeviceMemory GetMemory() const { return m_Memory; }

private:
    VkImage m_Image = VK_NULL_HANDLE;
    VkImageView m_View = VK_NULL_HANDLE;
    VkDeviceMemory m_Memory = VK_NULL_HANDLE;
};

class MIKAN_API VulkanDescriptor {
public:
    VulkanDescriptor() = default;
    ~VulkanDescriptor() { Cleanup(); }
    
    void AddBinding(uint32_t binding, VkDescriptorType type, VkShaderStageFlags stageFlags, uint32_t count = 1);
    bool CreateLayout();
    bool CreatePool(uint32_t maxSets);
    bool AllocateSet(VkDescriptorSet& set);
    void Cleanup();
    
    VkDescriptorSetLayout GetLayout() const { return m_Layout; }
    VkDescriptorPool GetPool() const { return m_Pool; }

private:
    std::vector<VkDescriptorSetLayoutBinding> m_Bindings;
    VkDescriptorSetLayout m_Layout = VK_NULL_HANDLE;
    VkDescriptorPool m_Pool = VK_NULL_HANDLE;
};

struct MIKAN_API PipelineConfig {
    std::string vertShader;
    std::string fragShader;
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL;
    VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT;
    VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    bool depthTest = true;
    bool depthWrite = true;
    VkCompareOp depthCompareOp = VK_COMPARE_OP_LESS;
    bool blending = false;
    // 混合因子（默认 ONE/ZERO = 不混合，与旧行为一致；半透明需设置 SRC_ALPHA/ONE_MINUS_SRC_ALPHA）
    VkBlendFactor srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    VkBlendFactor dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    VkBlendFactor srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    VkBlendFactor dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    uint32_t colorAttachmentCount = 1;  // MRT 支持多个颜色附件
    // render pass subpass 索引（0=z-prepass depth-only / 几何单 subpass；1=MRT 几何；2=合成）
    uint32_t subpass = 0;
    std::vector<VkVertexInputBindingDescription> vertexBindings;
    std::vector<VkVertexInputAttributeDescription> vertexAttributes;
    std::vector<VkDynamicState> dynamicStates = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPushConstantRange pushConstantRange = {};
    bool usePushConstants = false;
    // 2026-08-14：depth bias（CSM 方向光阴影防 acne；默认关闭）
    bool depthBiasEnable = false;
    float depthBiasConstantFactor = 0.0f;
    float depthBiasClamp = 0.0f;
    float depthBiasSlopeFactor = 0.0f;
};

class MIKAN_API VulkanPipeline {
public:
    VulkanPipeline() = default;
    ~VulkanPipeline() { Cleanup(); }
    
    bool Create(VkRenderPass renderPass, VkDescriptorSetLayout descriptorLayout, PipelineConfig config);
    void Cleanup();
    
    VkPipeline GetPipeline() const { return m_Pipeline; }
    VkPipelineLayout GetLayout() const { return m_Layout; }

    // ---- Shader 热更新 ----
    // Create() 成功后自动登记到全局注册表；Cleanup() 自动注销。
    // ReloadAllPipelines(): 用各自保存的 (renderPass/descriptorLayout/config) 重建所有已登记管线。
    // 返回成功重建数量。调用时机必须是 GPU 空闲时（如 vkWaitForFences 之后、命令缓冲录制之前）。
    static uint32_t ReloadAllPipelines();

private:
    bool Reload();
    void RegisterForReload(VkRenderPass renderPass, VkDescriptorSetLayout descriptorLayout, const PipelineConfig& config);

    VkPipeline m_Pipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_Layout = VK_NULL_HANDLE;

    // 热更新重建参数（Create 成功后保存；接口不变约定：shader 改动不得改 UBO/采样器/push constant 布局）
    PipelineConfig m_ReloadConfig;
    VkRenderPass m_ReloadRenderPass = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_ReloadDescriptorLayout = VK_NULL_HANDLE;
    bool m_ReloadRegistered = false;
};

class MIKAN_API IRenderer {
public:
    virtual ~IRenderer() = default;
    
    virtual void Init(VkRenderPass renderPass) = 0;
    virtual void Render(VkCommandBuffer commandBuffer, const glm::mat4& view, const glm::mat4& proj) = 0;
    virtual void Cleanup() = 0;
    
    virtual VkPipeline GetPipeline() const = 0;
    virtual VkPipelineLayout GetPipelineLayout() const = 0;
};

class MIKAN_API BaseRenderer : public IRenderer {
public:
    BaseRenderer() = default;
    virtual ~BaseRenderer() { Cleanup(); }
    
    virtual void Cleanup() override;

protected:
    VulkanBuffer m_UniformBuffer;
    VulkanDescriptor m_Descriptor;
    VulkanPipeline m_Pipeline;
    VkDescriptorSet m_DescriptorSet = VK_NULL_HANDLE;
    
    bool CreateUniformBuffer(VkDeviceSize size);
    bool CreateDescriptorSet();
    void UpdateUniformBuffer(const void* data, VkDeviceSize size);
};

namespace RendererUtils {
    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);
    std::vector<char> ReadFile(const std::string& filename);
    VkShaderModule CreateShaderModule(const std::vector<char>& code, const char* shaderName = "unknown");
}
