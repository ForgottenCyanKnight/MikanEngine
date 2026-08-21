#pragma once
#include "Platform/Export.h"

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <string>
#include <vector>

class MIKAN_API ComputeShader {
public:
    ComputeShader() = default;
    ~ComputeShader() = default;

    // 初始化计算着色器
    bool Init(VkDevice device, VkPhysicalDevice physicalDevice, const std::string& shaderPath, uint32_t width, uint32_t height);
    
    // 清理资源
    void Cleanup();

    // 绑定计算着色器
    void Bind(VkCommandBuffer commandBuffer);
    
    // 分发计算任务
    void Dispatch(VkCommandBuffer commandBuffer, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ = 1);

    // 执行计算着色器
    void Execute(VkCommandBuffer commandBuffer);

    // 设置 uniforms
    void SetUniformBuffer(VkBuffer buffer, uint32_t binding = 0);
    
    // 设置输入纹理
    void SetInputTexture(VkImageView imageView, VkSampler sampler, uint32_t binding = 0);
    
    // 设置SSBO（着色器存储缓冲区）
    void SetStorageBuffer(VkBuffer buffer, VkDeviceSize size, uint32_t binding);
    
    // 获取输出图像
    VkImageView GetOutputImageView() const { return m_OutputImageView; }
    VkImage GetOutputImage() const { return m_OutputImage; }
    VkImageLayout GetOutputImageLayout() const { return m_OutputImageLayout; }
    void SetOutputImageLayout(VkImageLayout layout) { m_OutputImageLayout = layout; }
    
    // 更新大小
    void UpdateSize(uint32_t width, uint32_t height, VkPhysicalDevice physicalDevice);
    
    // 光线追踪相关方法
    void SetModelTransforms(VkBuffer buffer, VkDeviceSize size);
    void SetModelMetadata(VkBuffer buffer, VkDeviceSize size);
    void SetModelOffsets(VkBuffer buffer, VkDeviceSize size);
    void SetVertices(VkBuffer buffer, VkDeviceSize size);
    void SetTriangles(VkBuffer buffer, VkDeviceSize size);
    void SetBVHNodes(VkBuffer buffer, VkDeviceSize size);
    void SetModelTextureData(VkBuffer buffer, VkDeviceSize size);
    void SetForwardTexture0(VkImageView imageView, VkSampler sampler);
    void SetForwardTexture1(VkImageView imageView, VkSampler sampler);
    void SetForwardTexture2(VkImageView imageView, VkSampler sampler);
    void SetForwardTexture3(VkImageView imageView, VkSampler sampler);
    void SetDepthTexture(VkImageView imageView, VkSampler sampler);
    void SetDepthImage(VkImage image);
    void SetSkybox(VkImageView imageView, VkSampler sampler);
    void SetBlueNoise(VkImageView imageView, VkSampler sampler);
    void SetTextureArray(const std::vector<VkDescriptorSet>& textureDescriptorSets, uint32_t binding);
    void SetModelTextures(VkImageView imageView, VkSampler sampler);
    void SetHistoryTexture0(VkImageView imageView, VkSampler sampler);
    void SetHistoryTexture(VkImageView imageView, VkSampler sampler);
    void SetRaytracingOutput(VkImageView imageView, VkSampler sampler);
    void SetAlbedoTexture(VkImageView imageView, VkSampler sampler);
    
    // 设置推常量
    void SetPushConstants(VkCommandBuffer commandBuffer, const void* cameraData, size_t cameraDataSize, const void* lightData, size_t lightDataSize, const void* sceneData, size_t sceneDataSize);

private:
    VkDevice m_Device = VK_NULL_HANDLE;
    uint32_t m_Width = 0;
    uint32_t m_Height = 0;
    bool m_IsRaytracingShader = false; // 标记是否是光线追踪着色器
    std::string m_ShaderPath; // 存储着色器路径
    
    VkShaderModule m_ShaderModule = VK_NULL_HANDLE;
    VkPipelineLayout m_PipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_ComputePipeline = VK_NULL_HANDLE;
    
    // 资源绑定
    VkDescriptorSetLayout m_DescriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_DescriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_DescriptorSet = VK_NULL_HANDLE;
    
    // 输出图像资源
    VkImage m_OutputImage = VK_NULL_HANDLE;
    VkDeviceMemory m_OutputImageMemory = VK_NULL_HANDLE;
    VkImageView m_OutputImageView = VK_NULL_HANDLE;
    VkImageLayout m_OutputImageLayout = VK_IMAGE_LAYOUT_UNDEFINED; // 跟踪输出图像的当前布局
    
    // 深度图像（用于计算着色器读取）
    VkImage m_DepthImage = VK_NULL_HANDLE;

    // 加载着色器模块
    bool LoadShaderModule(const std::string& filePath, VkShaderModule& shaderModule);
    
    // 创建描述符集
    bool CreateDescriptorSet();
    
    // 创建输出图像
    bool CreateOutputImage(VkPhysicalDevice physicalDevice);
    
    // 清理输出图像
    void CleanupOutputImage();
};