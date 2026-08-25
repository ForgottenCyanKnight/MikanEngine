#pragma once
#include "Platform/Export.h"
#include <vulkan/vulkan.h>
#include <ktx.h>
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>

enum class SamplerType {
    Linear,
    Nearest,
    LinearRepeat,
    NearestRepeat,
    LinearClamp,
    NearestClamp,
    LinearNoMip,   // 线性但禁用 mip（maxLod=0，强制 level0；模型 mip 开关 g_ModelMipmap=false 时用）
    ShadowCompare   // 2026-08-15：阴影比较采样器（HSPE 同款硬件 PCF）——LINEAR + compareOp=LESS（Vulkan 语义：ref < sampled → d > ref → 亮）+ CLAMP
};

struct MIKAN_API TextureInfo {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory imageMemory = VK_NULL_HANDLE;
    VkImageView imageView = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipLevels = 1;   // mip 链层级（LoadTexture2D 自动生成；>=2 时采样器可 mip 过滤）
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    bool isCubemap = false;
    SamplerType samplerType = SamplerType::Linear;
    int refCount = 0;
    float avgLuma = -1.0f;   // 2026-08-15：内容平均亮度（ktx2 加载时 RGBA32 统计；-1=未统计）——黑纹理检测（MR 容错回退）
    float shIrradiance[27] = {0};   // 2026-08-12：3 阶 SH 辐照度系数（RGB×9——CPU 投影，替代 irradiance 卷积）
};

class MIKAN_API TexturePool {
public:
    TexturePool(VkDevice device, VkPhysicalDevice physicalDevice, VkCommandPool commandPool, VkQueue queue, VkAllocationCallbacks* allocator);
    ~TexturePool();

    bool LoadCubemapFromFaces(const std::string& name, const std::string& basePath);
    // 2026-08-12：静态 HDR 天空盒（.hdr RGBE → equirect → cubemap R16G16B16A16_SFLOAT + mip 链）——IBL 预滤波源
    bool LoadHDRCubemap(const std::string& name, const std::string& hdrPath, uint32_t faceSize = 512);
    // 2026-08-12：辐照度图（irradiance map——漫反射半球积分）——diffuse IBL 用（用户指出缺失）
    bool GenerateIrradianceMap(const std::string& name, VkImageView srcCubeView, VkSampler srcSampler, uint32_t size = 32);
    // 2026-08-12：3 阶球谐（SH）辐照度系数（RGB×9 = 27 float——CPU 投影 + 卷积核 A_l×π——替代 irradiance 卷积）
    bool ProjectSHIrradiance(const std::string& srcName, float shOut[27]);
    bool LoadTexture2D(const std::string& name, const std::string& filePath, SamplerType samplerType = SamplerType::Linear);
    // 加载 16-bit 灰度 PNG 高度图，保持 R16_UNORM 精度，不走普通 RGBA8 图片路径。
    bool LoadHeightmap16(const std::string& name, const std::string& filePath, SamplerType samplerType = SamplerType::LinearClamp);
    // 加载 KTX2 压缩纹理（BasisU 超压缩 → 按设备转码 BC7/ASTC → VkUpload 含内嵌 mip）
    bool LoadTextureKtx2(const std::string& name, const std::string& filePath, SamplerType samplerType = SamplerType::Linear);
    bool RegisterExternalTexture(const std::string& name, VkImage image, VkImageView imageView, uint32_t width, uint32_t height, VkFormat format, SamplerType samplerType = SamplerType::Linear);
    bool UpdateTextureSampler(const std::string& name, SamplerType newSamplerType);

    const TextureInfo* GetTexture(const std::string& name) const;
    VkDescriptorSet GetDescriptorSet(const std::string& name) const;
    VkImageView GetImageView(const std::string& name) const;
    VkSampler GetSampler(const std::string& name) const;
    VkSampler GetSamplerByType(SamplerType type) const;

    void AddRef(const std::string& name);
    void Release(const std::string& name);
    
    // 强制重置描述符池（释放所有描述符集）
    void ResetDescriptorPool();
    
    // 获取当前描述符池使用情况
    size_t GetTextureCount() const { return m_Textures.size(); }

    void Cleanup();

private:
    bool CreateTextureImage(uint32_t width, uint32_t height, VkFormat format, uint32_t mipLevels, VkImage& image, VkDeviceMemory& memory);
    bool CreateImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags, bool isCubemap, uint32_t mipLevels, VkImageView& view);
    bool CreateSampler(VkFilter magFilter, VkFilter minFilter, VkSamplerAddressMode addressMode, VkSampler& sampler, bool enableMipmap = true, float mipLodBias = 0.0f);
    bool CreateDescriptorSetLayout(const TextureInfo& info, VkDescriptorSetLayout& layout,
                                   VkShaderStageFlags stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT);
    bool CreateDescriptorSet(const TextureInfo& info, VkDescriptorSetLayout layout, VkDescriptorSet& descriptorSet);
    bool TransitionImageLayout(VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout);
    bool InitializeSamplerPool();
    void CleanupSamplerPool();

    // 选择 BasisU 转码目标格式（桌面 BC7 / 移动 ASTC / 兜底 BC3）
    ktx_transcode_fmt_e SelectTranscodeFormat();

    VkDevice m_Device;
    VkPhysicalDevice m_PhysicalDevice;
    VkCommandPool m_CommandPool;
    VkQueue m_Queue;
    VkAllocationCallbacks* m_Allocator;
    VkDescriptorPool m_DescriptorPool;
    int m_DescriptorPoolMaxSets = 100;   // 当前 descriptor 池容量（满时自动重建 2 倍）

    std::unordered_map<std::string, TextureInfo> m_Textures;
    std::unordered_map<SamplerType, VkSampler> m_SamplerPool;
};
