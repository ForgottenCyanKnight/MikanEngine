#pragma once
#include "Platform/Export.h"
#include <vulkan/vulkan.h>
#include <ktx.h>
#include <string>
#include <functional>
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
    ShadowCompare
};

// VMA（vk_mem_alloc）句柄前置声明：TextureInfo 是跨 DLL 共享结构，
// 不把 vk_mem_alloc.h 拖进所有包含者。与 VMA 头中的 typedef 一致，
// 重复声明同名 typedef 指向同一类型是合法的。
struct VmaAllocator_T;
typedef VmaAllocator_T* VmaAllocator;
struct VmaAllocation_T;
typedef VmaAllocation_T* VmaAllocation;
struct VmaStagingBuffer;   // 定义在 TexturePool.cpp（④ RAII staging）

struct MIKAN_API TextureInfo {
    VkImage image = VK_NULL_HANDLE;
    // ④ VMA suballocation：设备内存由 VmaAllocator 统一子分配管理，
    // 一个 VkDeviceMemory 承载多张纹理，分配数 O(纹理数) → O(1)。
    // 外部纹理（RegisterExternalTexture）保持 nullptr。
    VmaAllocation imageAllocation = nullptr;
    VkImageView imageView = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipLevels = 1;   // mip 链层级（LoadTexture2D 自动生成；>=2 时采样器可 mip 过滤）
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    bool isCubemap = false;
    SamplerType samplerType = SamplerType::Linear;
    // 描述符集的可见着色阶段（普通材质 FRAGMENT；高度图 VERTEX|FRAGMENT）。
    // 共享 layout 按 stageFlags 缓存，重置描述符池后按此字段重建。
    VkShaderStageFlags descriptorStages = VK_SHADER_STAGE_FRAGMENT_BIT;
    int refCount = 0;
    float avgLuma = -1.0f;
    float shIrradiance[27] = {0};
};

class MIKAN_API TexturePool {
public:
    TexturePool(VkDevice device, VkPhysicalDevice physicalDevice, VkCommandPool commandPool, VkQueue queue, VkAllocationCallbacks* allocator);
    ~TexturePool();

    // VMA 暂存缓冲 RAII 包装（定义在 TexturePool.cpp），需访问 private 创建/查询接口
    friend struct VmaStagingBuffer;

    bool LoadCubemapFromFaces(const std::string& name, const std::string& basePath);
    bool LoadHDRCubemap(const std::string& name, const std::string& hdrPath, uint32_t faceSize = 512);
    bool GenerateIrradianceMap(const std::string& name, VkImageView srcCubeView, VkSampler srcSampler, uint32_t size = 32);
    bool ProjectSHIrradiance(const std::string& srcName, float shOut[27]);
    bool LoadTexture2D(const std::string& name, const std::string& filePath, SamplerType samplerType = SamplerType::Linear);
    // 加载 16-bit 灰度 PNG 高度图，保持 R16_UNORM 精度，不走普通 RGBA8 图片路径。
    bool LoadHeightmap16(const std::string& name, const std::string& filePath, SamplerType samplerType = SamplerType::LinearClamp);
    // 用内存样本直接建一张 R16_UNORM 高度图（行主序、顶左原点）。供程序化地形
    // （默认平坦高度图）和地形笔刷使用，避免依赖 PNG 编解码。samples 长度须为 width*height。
    bool CreateHeightmap16FromMemory(const std::string& name, uint32_t width, uint32_t height,
                                     const uint16_t* samples,
                                     SamplerType samplerType = SamplerType::LinearClamp);
    // 局部写回已加载的高度图（地形笔刷路径）。x/topRow 为顶左原点坐标，与
    // CreateHeightmap16FromMemory 的样本序一致；fullSamples 是整张高度图的 CPU 镜像，
    // fullWidth 为其行宽，函数只拷贝 [x, topRow, width, height) 区域。
    // 内部负责 顶左原点 → 引擎自底向上上传行序 的转换与 layout 往返。
    // 注意：调用点必须保证没有仍在执行的命令缓冲引用该图（编辑器笔刷在帧边界调用）。
    bool UpdateHeightmapRegion16(const std::string& name,
                                 uint32_t x, uint32_t topRow,
                                 uint32_t width, uint32_t height,
                                 const uint16_t* fullSamples, uint32_t fullWidth);

    // ===== 材质权重图（control map，RGBA8）=====
    // 地形材质笔刷的写入目标。与高度图同一套路：纹理在内存里带一份 CPU 镜像，
    // 涂抹只改镜像再局部回写，不需要图像编码器，也不触发资源重建。
    // rgbaPixels 为行主序、顶左原点，长度须为 width*height*4，通道序 R,G,B,A，
    // 分别对应地形图层 0..3 的权重。
    bool CreateControlMap8FromMemory(const std::string& name, uint32_t width, uint32_t height,
                                     const uint8_t* rgbaPixels,
                                     SamplerType samplerType = SamplerType::LinearClamp);
    // 控制图局部回写，语义与 UpdateHeightmapRegion16 完全一致（顶左原点坐标 +
    // 整张 CPU 镜像 + 行宽），内部同样做行序翻转与 layout 往返。
    bool UpdateControlMapRegion8(const std::string& name,
                                 uint32_t x, uint32_t topRow,
                                 uint32_t width, uint32_t height,
                                 const uint8_t* fullSamples, uint32_t fullWidth);
    // 把控制图文件（PNG/JPG 等，经 SDL_image 解码）读成顶左原点 RGBA8 CPU 像素，
    // 供地形材质笔刷建立镜像。只解码、不入池。
    bool LoadControlMapPixels8(const std::string& filePath,
                               uint32_t& outWidth, uint32_t& outHeight,
                               std::vector<uint8_t>& outRgba);

    // ===== 草密度图（grass mask，R8 单通道）=====
    // 草地笔刷的写入目标：每 texel 0..255 表示该处的草密度，0 = 无草。
    // 与控制图同一套路（CPU 镜像 + 局部回写），但会被草地顶点着色器间接
    // 消费（CPU 散布实例时读取镜像，不在 GPU 采样），所以可见阶段保持
    // VERTEX|FRAGMENT 以防后续改为 GPU 采样时踩 stage 越界。
    bool CreateGrassMask8FromMemory(const std::string& name, uint32_t width, uint32_t height,
                                    const uint8_t* samples,
                                    SamplerType samplerType = SamplerType::LinearClamp);
    // 草密度图局部回写，语义与 UpdateControlMapRegion8 完全一致。
    bool UpdateGrassMaskRegion8(const std::string& name,
                                uint32_t x, uint32_t topRow,
                                uint32_t width, uint32_t height,
                                const uint8_t* fullSamples, uint32_t fullWidth);

    // 加载 KTX2 压缩纹理（BasisU 超压缩 → 按设备转码 BC7/ASTC → VkUpload 含内嵌 mip）
    bool LoadTextureKtx2(const std::string& name, const std::string& filePath, SamplerType samplerType = SamplerType::Linear);
    bool RegisterExternalTexture(const std::string& name, VkImage image, VkImageView imageView, uint32_t width, uint32_t height, VkFormat format, SamplerType samplerType = SamplerType::Linear);
    bool UpdateTextureSampler(const std::string& name, SamplerType newSamplerType);

    // ===== 资产热重载（AssetHotReload 路由至此）=====
    // name 已在池中：先以临时 key 加载新副本（期间旧资源保持有效），成功后交换，
    // 旧 GPU 资源挂入延迟销毁队列（③，帧循环安全点 DrainPendingDestroy 推进），
    // 失败回滚保留旧纹理。契约：在 GPU 安全点调用（帧循环 fence 已等待处）。
    bool ReloadTexture2D(const std::string& name, const std::string& filePath);
    // 按"解析后的绝对路径"匹配已加载纹理（name==项目相对路径的加载约定），命中才重载。
    // 未加载过返回 false（首次加载自然取到新文件，无需重载）。
    bool ReloadTextureByPath(const std::string& resolvedPath);
    // 重载成功后的通知（参数为纹理 name）；编辑器预览缓存等订阅此接口失效自己的句柄缓存。
    void AddReloadListener(std::function<void(const std::string& name)> listener) {
        m_ReloadListeners.push_back(std::move(listener));
    }

    const TextureInfo* GetTexture(const std::string& name) const;
    VkDescriptorSet GetDescriptorSet(const std::string& name) const;
    VkImageView GetImageView(const std::string& name) const;
    VkSampler GetSampler(const std::string& name) const;
    VkSampler GetSamplerByType(SamplerType type) const;

    void AddRef(const std::string& name);
    void Release(const std::string& name);

    // ===== 延迟销毁（③ Release 帧飞行保护）=====
    // 引用归零 / 热重载换下的纹理不立即销毁，先进 m_PendingDestroy；
    // 每帧在 fence 等待安全点（VulkanFrameLoop 中 fence 已等待、命令录制前）
    // 调用本函数推进队列，存活满 kDeferredDestroyFrames 个 drain 才真正销毁，
    // 避免仍在执行的命令缓冲引用已销毁的 VkImage/ImageView/DeviceMemory。
    void DrainPendingDestroy();
    size_t GetPendingDestroyCount() const { return m_PendingDestroy.size(); }

    // 强制重置描述符池（释放所有描述符集）
    void ResetDescriptorPool();
    
    // 获取当前描述符池使用情况
    size_t GetTextureCount() const { return m_Textures.size(); }

    void Cleanup();

private:
    // ④ VMA 统一子分配。VMA 实现只在 TexturePool.cpp 展开（VMA_IMPLEMENTATION）。
    bool CreateImageVMA(const VkImageCreateInfo& ci, VkImage& outImage, VmaAllocation& outAllocation);
    // CPU 可写传输源缓冲，MAPPED 持久映射（mapped 返回常驻映射指针）。
    bool CreateStagingBufferVMA(VkDeviceSize size, VkBuffer& outBuffer, VmaAllocation& outAllocation, void*& outMapped);
    // allocation == nullptr 时不销毁 image（外部纹理，image 所有权在别处）。
    void DestroyImageVMA(VkImage image, VmaAllocation allocation);
    VmaAllocator GetVmaAllocator() const { return m_Vma; }

    bool CreateTextureImage(uint32_t width, uint32_t height, VkFormat format, uint32_t mipLevels, VkImage& image, VmaAllocation& allocation);
    bool CreateImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags, bool isCubemap, uint32_t mipLevels, VkImageView& view);
    bool CreateSampler(VkFilter magFilter, VkFilter minFilter, VkSamplerAddressMode addressMode, VkSampler& sampler, bool enableMipmap = true, float mipLodBias = 0.0f);
    // 全池共享的描述符集布局，按 stageFlags 缓存（普通纹理 FRAGMENT，高度图 VERTEX|FRAGMENT）。
    // 所有纹理布局本就等价（1 binding combined image sampler），不再逐纹理创建。
    VkDescriptorSetLayout GetSharedLayout(VkShaderStageFlags stageFlags);
    bool CreateDescriptorSet(const TextureInfo& info, VkDescriptorSetLayout layout, VkDescriptorSet& descriptorSet);
    bool TransitionImageLayout(VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout);
    // 内存像素 → 单 mip 采样图（行序翻转 + VMA staging + SHADER_READ_ONLY + 描述符集）。
    // 高度图（R16_UNORM/2字节）与控制图（R8G8B8A8_UNORM/4字节）共用同一实现，
    // 只有格式/通道字节数/可见阶段/日志标签不同。label 用于日志前缀。
    bool CreateImage2DFromMemory(const std::string& name, uint32_t width, uint32_t height,
                                 VkFormat format, uint32_t bytesPerPixel, const void* pixels,
                                 SamplerType samplerType, VkShaderStageFlags descriptorStages,
                                 const char* label);
    // 局部回写已加载的单 mip 采样图；调用点必须保证没有仍在执行的命令缓冲引用该图。
    bool UpdateImage2DRegion(const std::string& name, VkFormat requiredFormat,
                             uint32_t bytesPerPixel,
                             uint32_t x, uint32_t topRow, uint32_t width, uint32_t height,
                             const void* fullSamples, uint32_t fullWidth, const char* label);
    bool InitializeSamplerPool();
    void CleanupSamplerPool();

    // 延迟销毁：把一组 GPU 资源句柄挂入待销毁队列（全部为空则忽略）。
    void QueueTextureDestroy(VkImage image, VkImageView imageView, VmaAllocation allocation);

    // 延迟销毁条目。framesWaited 由每帧一次的 DrainPendingDestroy 递增，
    // 不依赖帧序号，加载页/暂停等早退帧同样正确推进。
    struct PendingTextureDestroy {
        VkImage image = VK_NULL_HANDLE;
        VkImageView imageView = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        uint32_t framesWaited = 0;
    };
    // >= 帧飞行数的安全裕量：本引擎离屏提交由 g_LastOffscreenFrameFence 串行化，
    // 2 个 drain 周期后引用它的提交必然已完成。
    static constexpr uint32_t kDeferredDestroyFrames = 2;

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
    std::unordered_map<uint32_t, VkDescriptorSetLayout> m_SharedLayouts;   // key = stageFlags
    std::vector<std::function<void(const std::string&)>> m_ReloadListeners;
    std::vector<PendingTextureDestroy> m_PendingDestroy;   // 延迟销毁队列（③）
    // ④ VMA 设备内存子分配器。刻意追加在类尾：不改已有成员偏移，
    // 头文件内联访问器（GetTextureCount/AddReloadListener）的旧机器码保持有效，
    // 只需重编分配点（EngineMain）与本文件。
    VmaAllocator m_Vma = nullptr;
};
