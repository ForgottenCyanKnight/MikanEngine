#pragma once
#include "Platform/Export.h"
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>

class MIKAN_API VulkanShader {
public:
    VulkanShader(VkDevice device);
    ~VulkanShader();

    VulkanShader(const VulkanShader&) = delete;
    VulkanShader& operator=(const VulkanShader&) = delete;

    bool LoadFromSPIRV(const std::string& vertPath, const std::string& fragPath);
    bool LoadFromGLSL(const std::string& vertPath, const std::string& fragPath);

    VkPipelineLayout GetPipelineLayout() const { return m_PipelineLayout; }
    VkDescriptorSetLayout GetDescriptorSetLayout() const { return m_DescriptorSetLayout; }
    VkPipeline GetPipeline() const { return m_Pipeline; }

    bool CreatePipeline(VkRenderPass renderPass, 
                        VkVertexInputBindingDescription bindingDesc,
                        const std::vector<VkVertexInputAttributeDescription>& attributeDescs,
                        VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                        bool depthTest = true,
                        bool depthWrite = true);

    void SetUniformBuffer(uint32_t binding, VkBuffer buffer, VkDeviceSize offset = 0, VkDeviceSize range = VK_WHOLE_SIZE);
    void SetCombinedImageSampler(uint32_t binding, VkImageView imageView, VkSampler sampler, VkImageLayout layout);
    
    void Bind(VkCommandBuffer cmd) const;
    void BindDescriptorSet(VkCommandBuffer cmd, VkDescriptorSet descriptorSet) const;

    void Cleanup();

private:
    VkDevice m_Device = VK_NULL_HANDLE;
    VkShaderModule m_VertShaderModule = VK_NULL_HANDLE;
    VkShaderModule m_FragShaderModule = VK_NULL_HANDLE;
    VkPipelineLayout m_PipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_DescriptorSetLayout = VK_NULL_HANDLE;
    VkPipeline m_Pipeline = VK_NULL_HANDLE;
    VkDescriptorPool m_DescriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_DescriptorSet = VK_NULL_HANDLE;

    std::vector<VkDescriptorSetLayoutBinding> m_LayoutBindings;
    std::vector<VkWriteDescriptorSet> m_DescriptorWrites;
    std::vector<VkDescriptorBufferInfo> m_BufferInfos;
    std::vector<VkDescriptorImageInfo> m_ImageInfos;

    bool CreateDescriptorSetLayout();
    bool CreatePipelineLayout();
    VkShaderModule CreateShaderModule(const std::vector<char>& code);
    std::vector<char> ReadFile(const std::string& filename);
};
