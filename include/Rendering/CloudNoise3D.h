#pragma once

#include "Platform/Export.h"

#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

// Nubis/Meteoros 体积云资源，以及 Nubis 风格高层 2D 云景/覆盖纹理。
//
// 资源布局严格对应 Meteoros 的三张预计算纹理：
//   cloudBaseShapeTexture : 128^3 RGBA8，R=Perlin-Worley，G/B/A=Worley FBM
//   cloudDetailsTexture   :  32^3 RGBA8，RGB=递增频率 Worley
//   cloudMotionTexture    : 128^2 RGBA8，RG=curl noise
//
// 高层云使用两张独立的 2D 数据：CirrusLutRev 是 RGB 三种云景形状，
// CloudMapHigh 是 RGBA 覆盖/类型场。切片按 LowFrequency(1..128) /
// HighFrequency(1..32) 直接写入 Vulkan 3D image，并用线性 Repeat sampler
// 供 cloud_view 采样；物理化不应改变低层 Nubis 云形状。
class MIKAN_API CloudNoise3D {
public:
    static constexpr uint32_t kBaseResolution = 128;
    static constexpr uint32_t kDetailResolution = 32;
    static constexpr uint32_t kCurlResolution = 128;
    static constexpr uint32_t kHighResolution = 1024;
    static constexpr uint32_t kHighMapResolution = 512;

    CloudNoise3D() = default;
    CloudNoise3D(const CloudNoise3D&) = delete;
    CloudNoise3D& operator=(const CloudNoise3D&) = delete;

    bool Initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                    VkAllocationCallbacks* allocator);

    // 必须在 VkDevice 销毁前调用。
    void Cleanup();

    bool IsInitialized() const;

    // 兼容原有 cloud_noise source 名称：这是 base shape 3D texture。
    VkImage GetImage() const { return m_Base.image; }
    VkImageView GetImageView() const { return m_Base.view; }
    VkImageView GetDetailImageView() const { return m_Detail.view; }
    VkImageView GetCurlImageView() const { return m_Curl.view; }
    VkImageView GetHighImageView() const { return m_High.view; }
    VkImageView GetHighMapImageView() const { return m_HighMap.view; }
    VkSampler GetSampler() const { return m_Sampler; }
    VkSampler GetDetailSampler() const { return m_Sampler; }
    VkSampler GetCurlSampler() const { return m_Sampler; }

private:
    struct ImageResource {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t depth = 0;
    };

    bool LoadBaseTexture();
    bool LoadDetailTexture();
    bool LoadCurlTexture();
    bool LoadHighTexture();
    bool LoadHighMapTexture();
    bool LoadSurfaceRgba(const char* path, uint32_t expectedWidth,
                         uint32_t expectedHeight, std::vector<uint8_t>& pixels);
    bool LoadRawRgba(const char* path, uint32_t expectedWidth,
                     uint32_t expectedHeight, std::vector<uint8_t>& pixels);
    bool CreateImageResource(ImageResource& resource, uint32_t width,
                             uint32_t height, uint32_t depth);
    bool CreateImageView(ImageResource& resource);
    bool UploadImage(const ImageResource& resource, const std::vector<uint8_t>& pixels);
    bool CreateSampler();
    void DestroyImageResource(ImageResource& resource);
    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;

    VkDevice m_Device = VK_NULL_HANDLE;
    VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
    VkAllocationCallbacks* m_Allocator = nullptr;
    VkSampler m_Sampler = VK_NULL_HANDLE;
    ImageResource m_Base;
    ImageResource m_Detail;
    ImageResource m_Curl;
    ImageResource m_High;
    ImageResource m_HighMap;
};

// PostProcessChain 和 VulkanManager 共用同一份持久 Nubis 资源。
MIKAN_API CloudNoise3D& GetCloudNoise3D();
