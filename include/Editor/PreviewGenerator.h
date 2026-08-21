#pragma once

#include <vulkan/vulkan.h>
#include <string>
#include <memory>
#include "ECS/Components.h"

class PreviewModelRenderer;
class PreviewVoxRenderer;
class TexturePool;

class PreviewGenerator {
public:
    static PreviewGenerator& GetInstance();
    
    void Init();
    void Cleanup();
    
    bool GeneratePreview(const std::string& modelPath, const std::string& outputPath);
    bool GenerateMaterialPreview(const ECS::MaterialComponent& material, const std::string& outputPath);
    bool GenerateVoxPreview(const std::string& voxPath, const std::string& outputPath);
    
private:
    PreviewGenerator() = default;
    ~PreviewGenerator() = default;
    
    PreviewGenerator(const PreviewGenerator&) = delete;
    PreviewGenerator& operator=(const PreviewGenerator&) = delete;
    
    void CreateRenderTarget();
    void DestroyRenderTarget();
    
    bool m_Initialized = false;
    
    VkRenderPass m_RenderPass = VK_NULL_HANDLE;
    VkFramebuffer m_Framebuffer = VK_NULL_HANDLE;
    VkImage m_ColorImage = VK_NULL_HANDLE;
    VkDeviceMemory m_ColorImageMemory = VK_NULL_HANDLE;
    VkImageView m_ColorImageView = VK_NULL_HANDLE;
    VkImage m_DepthImage = VK_NULL_HANDLE;
    VkDeviceMemory m_DepthImageMemory = VK_NULL_HANDLE;
    VkImageView m_DepthImageView = VK_NULL_HANDLE;
    
    std::unique_ptr<PreviewModelRenderer> m_PreviewRenderer;
    std::unique_ptr<PreviewVoxRenderer> m_VoxRenderer;
    TexturePool* m_TexturePool = nullptr;
    
    static const uint32_t PREVIEW_SIZE = 256;
};
