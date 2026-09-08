#include "TexturePool.h"
#include "EngineGlobal.h"
#include "Core/EngineConfig.h"
#include "Core/ProjectManager.h"
#include "Core/Log.h"
#include "Rendering/DdsDecoder.h"
#include "Rendering/HeightmapLoader.h"
#include <cctype>
#include <fstream>
#include <sstream>
#include <cstring>
#include <cmath>
#include <glm/glm.hpp>

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_surface.h>
#include <SDL3_image/SDL_image.h>
#include <vulkan/vulkan.h>
#include <ktx.h>
#include <ktxvulkan.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include <array>
#include <cstring>
#include <algorithm>
#include <cmath>

// ===== KTX2 动态加载（ktx.dll；LoadLibrary + GetProcAddress——避�?MSVC lib.exe def 导入库不生成 __imp_ 的问题）=====
#ifdef _WIN32
struct KtxRuntime {
    HMODULE module = nullptr;
    decltype(&ktxTexture2_CreateFromNamedFile) CreateFromNamedFile = nullptr;
    decltype(&ktxTexture2_NeedsTranscoding) NeedsTranscoding = nullptr;
    decltype(&ktxTexture2_TranscodeBasis) TranscodeBasis = nullptr;
    decltype(&ktxTexture2_Destroy) Destroy = nullptr;
    decltype(&ktxErrorString) ErrorString = nullptr;
    decltype(&ktxVulkanDeviceInfo_Construct) VkDeviceInfo_Construct = nullptr;
    decltype(&ktxVulkanDeviceInfo_Destruct) VkDeviceInfo_Destruct = nullptr;
    decltype(&ktxTexture2_VkUpload) VkUpload = nullptr;

    bool Init() {
        if (module) return true;
        module = LoadLibraryA("ktx.dll");
        if (!module) { LOGE("[TexturePool] ktx.dll not found"); return false; }
        CreateFromNamedFile = (decltype(CreateFromNamedFile))GetProcAddress(module, "ktxTexture2_CreateFromNamedFile");
        NeedsTranscoding = (decltype(NeedsTranscoding))GetProcAddress(module, "ktxTexture2_NeedsTranscoding");
        TranscodeBasis = (decltype(TranscodeBasis))GetProcAddress(module, "ktxTexture2_TranscodeBasis");
        Destroy = (decltype(Destroy))GetProcAddress(module, "ktxTexture2_Destroy");
        ErrorString = (decltype(ErrorString))GetProcAddress(module, "ktxErrorString");
        VkDeviceInfo_Construct = (decltype(VkDeviceInfo_Construct))GetProcAddress(module, "ktxVulkanDeviceInfo_Construct");
        VkDeviceInfo_Destruct = (decltype(VkDeviceInfo_Destruct))GetProcAddress(module, "ktxVulkanDeviceInfo_Destruct");
        VkUpload = (decltype(VkUpload))GetProcAddress(module, "ktxTexture2_VkUpload");
        if (!CreateFromNamedFile || !NeedsTranscoding || !TranscodeBasis || !Destroy ||
            !ErrorString || !VkDeviceInfo_Construct || !VkDeviceInfo_Destruct || !VkUpload) {
            LOGE("[TexturePool] ktx.dll symbol resolution failed");
            return false;
        }
        return true;
    }
};
static KtxRuntime g_ktx;
#endif

static uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(g_PhysicalDevice, &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    LOGE("Failed to find suitable memory type!");
    return 0;
}

static std::vector<char> ReadFile(const std::string& filename)
{
    std::string fullPath = EngineConfig::ResolvePlatformPath(filename);

    SDL_IOStream* io = SDL_IOFromFile(fullPath.c_str(), "rb");
    if (io == nullptr) {
        LOGE("Failed to open file: %s (SDL Error: %s)", fullPath.c_str(), SDL_GetError());
        return {};
    }

    Sint64 fileSize = SDL_GetIOSize(io);
    if (fileSize <= 0) {
        SDL_CloseIO(io);
        return {};
    }

    std::vector<char> buffer((size_t)fileSize);
    if (SDL_ReadIO(io, buffer.data(), (size_t)fileSize) != (size_t)fileSize) {
        SDL_CloseIO(io);
        return {};
    }

    SDL_CloseIO(io);
    return buffer;
}

TexturePool::TexturePool(VkDevice device, VkPhysicalDevice physicalDevice, VkCommandPool commandPool, VkQueue queue, VkAllocationCallbacks* allocator)
    : m_Device(device), m_PhysicalDevice(physicalDevice), m_CommandPool(commandPool), m_Queue(queue), m_Allocator(allocator), m_DescriptorPool(VK_NULL_HANDLE)
{
    // 创建描述符池
    VkDescriptorPoolSize poolSize = {};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 100; // 足够容纳多个纹理

    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = 100;

    VkResult err = vkCreateDescriptorPool(m_Device, &poolInfo, m_Allocator, &m_DescriptorPool);
    if (err != VK_SUCCESS) {
        LOGE("Failed to create descriptor pool!");
    }

    // 初始化采样器池
    InitializeSamplerPool();
}

TexturePool::~TexturePool()
{
    Cleanup();
}

bool TexturePool::CreateTextureImage(uint32_t width, uint32_t height, VkFormat format, uint32_t mipLevels, VkImage& image, VkDeviceMemory& memory)
{
    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = mipLevels;   // mip 链（>=2 �
    // blit 生成�?
imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // TRANSFER_SRC �
    // blit 生成 mip；TRANSFER_DST 供初始上�?
imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

    VkResult err = vkCreateImage(m_Device, &imageInfo, m_Allocator, &image);
    if (err != VK_SUCCESS) {
        LOGE("Failed to create texture image");
        return false;
    }

    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(m_Device, image, &memRequirements);

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = FindMemoryType(memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    err = vkAllocateMemory(m_Device, &allocInfo, m_Allocator, &memory);
    if (err != VK_SUCCESS) {
        LOGE("Failed to allocate texture image memory");
        return false;
    }

    vkBindImageMemory(m_Device, image, memory, 0);
    return true;
}

bool TexturePool::CreateImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags, bool isCubemap, uint32_t mipLevels, VkImageView& view)
{
    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = isCubemap ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = aspectFlags;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = mipLevels;   // 全部 mip 级（sampler mip 过滤需要）
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = isCubemap ? 6 : 1;

    VkResult err = vkCreateImageView(m_Device, &viewInfo, m_Allocator, &view);
    if (err != VK_SUCCESS) {
        LOGE("Failed to create image view");
        return false;
    }
    return true;
}

bool TexturePool::CreateSampler(VkFilter magFilter, VkFilter minFilter, VkSamplerAddressMode addressMode, VkSampler& sampler, bool enableMipmap, float mipLodBias)
{
    VkSamplerCreateInfo samplerInfo = {};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = magFilter;
    samplerInfo.minFilter = minFilter;
    samplerInfo.addressModeU = addressMode;
    samplerInfo.addressModeV = addressMode;
    samplerInfo.addressModeW = addressMode;
    
    // 蓝噪声纹理应该使�
    // nearest 过滤且禁用各向异�?    // 对于其他纹理，保持各向异性过�
    // 
if (magFilter == VK_FILTER_NEAREST && minFilter == VK_FILTER_NEAREST) {
        // 对于 nearest 过滤的纹理（如蓝噪声），禁用各向异�
    // 
samplerInfo.anisotropyEnable = VK_FALSE;
        samplerInfo.maxAnisotropy = 1.0f;
    } else {
        // 获取物理设备属性以确定最大各向异�
    // 
VkPhysicalDeviceProperties deviceProperties;
        vkGetPhysicalDeviceProperties(m_PhysicalDevice, &deviceProperties);
        float maxAnisotropy = deviceProperties.limits.maxSamplerAnisotropy;
        
        // Adreno GPU 需要正确处理各向异性过�
    // 
if (maxAnisotropy > 1.0f) {
            samplerInfo.anisotropyEnable = VK_TRUE;
            samplerInfo.maxAnisotropy = maxAnisotropy;
        } else {
            samplerInfo.anisotropyEnable = VK_FALSE;
            samplerInfo.maxAnisotropy = 1.0f;
        }
    }
    
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_WHITE;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    // 2026-08-12 修复：enableMipmap 时启用 mip 过滤（此前 mipmapMode/maxLod 未设——默认 NEAREST + maxLod=0
    // → textureLod 的 lod 恒被 clamp 到 mip0 → 粗糙反射永远采清晰 mip0（用户：粗糙金属还能看到清晰天空）
    if (enableMipmap) {
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.minLod = 0.0f;
        samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
        samplerInfo.mipLodBias = mipLodBias;
    } else {
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.maxLod = 0.0f;
    }
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    // mip 过滤范围：放宽到全部 mip 级（�
    // mip 纹理自动 clamp �?level 0；有 mip 纹理正常使用�?
samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.mipLodBias = mipLodBias;   // 负�
    // 采样偏向高分辨率 mip（拉长过渡距离，更远才切�?mip�?
samplerInfo.minLod = 0.0f;
    // enableMipmap=false（LinearNoMip）：maxLod=0 强制只用 level0（模�
    // mip 开�?g_ModelMipmap=false 时用�?
samplerInfo.maxLod = enableMipmap ? VK_LOD_CLAMP_NONE : 0.0f;   // 1000.0f：允许采样完�?mip 链；0：禁�?mip

    VkResult err = vkCreateSampler(m_Device, &samplerInfo, m_Allocator, &sampler);
    if (err != VK_SUCCESS) {
        LOGE("Failed to create sampler");
        return false;
    }
    return true;
}

bool TexturePool::InitializeSamplerPool()
{
    // 创建所有预定义的采样器
    CreateSampler(VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT, m_SamplerPool[SamplerType::Linear], true, g_ModelMipLodBias);   // 模型 mip：负 LOD 偏置拉长过渡距离
    CreateSampler(VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT, m_SamplerPool[SamplerType::LinearNoMip], false);   // 模型 mip 开关：maxLod=0 强制 level0
    CreateSampler(VK_FILTER_NEAREST, VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_REPEAT, m_SamplerPool[SamplerType::Nearest]);
    CreateSampler(VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT, m_SamplerPool[SamplerType::LinearRepeat]);
    CreateSampler(VK_FILTER_NEAREST, VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_REPEAT, m_SamplerPool[SamplerType::NearestRepeat]);
    CreateSampler(VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, m_SamplerPool[SamplerType::LinearClamp], true, g_ModelMipLodBias);   // 模型纹理默认（CLAMP 消除接缝黑线 + LOD 偏置）
    CreateSampler(VK_FILTER_NEAREST, VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, m_SamplerPool[SamplerType::NearestClamp]);
    
    // 2026-08-15：阴影比较采样器（HSPE 同款硬件 PCF）——shadow sampler 必须 compareEnable + LINEAR（2×2 双线性比较）
    // compareOp=GREATER：采样结果 = (d > ref) ? 1 : 0 ——与 shader 手动比较语义（d + bias > currentDepth ⟺ d > currentDepth - bias）一致
    {
        VkSamplerCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.magFilter = VK_FILTER_LINEAR;
        info.minFilter = VK_FILTER_LINEAR;
        info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        info.maxLod = 0.0f;   // 阴影图无 mip 链（单 level）
        info.compareEnable = VK_TRUE;
        // ⚠️ 2026-08-15 修正：Vulkan compareOp 语义 = "ref OP sampled"（参考值在左）！
        // 需求：d > currentDepth - bias → 亮（ref = currentDepth - bias，sampled = d）→ ref < sampled → 必须 VK_COMPARE_OP_LESS
        // （误用 GREATER = ref > sampled → 比较反转 → 阴影区全白）
        info.compareOp = VK_COMPARE_OP_LESS;
        info.borderColor = VK_BORDER_COLOR_INT_OPAQUE_WHITE;
        info.unnormalizedCoordinates = VK_FALSE;
        info.anisotropyEnable = VK_FALSE;
        info.maxAnisotropy = 1.0f;
        VkResult err = vkCreateSampler(m_Device, &info, m_Allocator, &m_SamplerPool[SamplerType::ShadowCompare]);
        if (err != VK_SUCCESS) {
            LOGE("Failed to create shadow compare sampler");
            m_SamplerPool[SamplerType::ShadowCompare] = VK_NULL_HANDLE;
        }
    }
    
    return true;
}

void TexturePool::CleanupSamplerPool()
{
    for (auto& pair : m_SamplerPool) {
        if (pair.second != VK_NULL_HANDLE) {
            vkDestroySampler(m_Device, pair.second, m_Allocator);
        }
    }
    m_SamplerPool.clear();
}

bool TexturePool::TransitionImageLayout(VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout)
{
    VkCommandBuffer commandBuffer;
    VkCommandBufferAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = m_CommandPool;
    allocInfo.commandBufferCount = 1;

    VkResult err = vkAllocateCommandBuffers(m_Device, &allocInfo, &commandBuffer);
    if (err != VK_SUCCESS) {
        return false;
    }

    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commandBuffer, &beginInfo);

    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags sourceStage;
    VkPipelineStageFlags destinationStage;

    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        // A heightmap is sampled by terrain vertex shaders; keeping fragment
        // visibility as well preserves the existing material texture path.
        destinationStage = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else {
        return false;
    }

    vkCmdPipelineBarrier(commandBuffer, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(commandBuffer);

    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    vkQueueSubmit(m_Queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_Queue);

    vkFreeCommandBuffers(m_Device, m_CommandPool, 1, &commandBuffer);
    return true;
}

bool TexturePool::LoadCubemapFromFaces(const std::string& name, const std::string& basePath)
{
    try {
    if (m_Textures.find(name) != m_Textures.end()) {
        m_Textures[name].refCount++;
        return true;
    }

    std::array<std::string, 6> facePaths = {
        basePath + "/right.jpg",
        basePath + "/left.jpg",
        basePath + "/top.jpg",
        basePath + "/bottom.jpg",
        basePath + "/front.jpg",
        basePath + "/back.jpg"
    };

    int width = 0, height = 0;
    std::vector<unsigned char> allPixels;

    for (int i = 0; i < 6; i++) {
        std::string fullPath = EngineConfig::ResolvePlatformPath(facePaths[i]);

        SDL_Surface* surface = IMG_Load(fullPath.c_str());
        if (surface == nullptr) {
            LOGE("Failed to load texture face: %s", facePaths[i].c_str());
            int fallbackSize = 256;
            if (width == 0) {
                width = fallbackSize;
                height = fallbackSize;
            }
            std::vector<unsigned char> fallbackPixels(width * height * 4);
            for (int p = 0; p < width * height; p++) {
                float u = (float)(p % width) / width;
                float v = (float)(p / width) / height;
                fallbackPixels[p * 4 + 0] = (unsigned char)(u * 255);
                fallbackPixels[p * 4 + 1] = (unsigned char)(v * 255);
                fallbackPixels[p * 4 + 2] = 128;
                fallbackPixels[p * 4 + 3] = 255;
            }
            allPixels.insert(allPixels.end(), fallbackPixels.begin(), fallbackPixels.end());
            continue;
        }

        bool hasAlpha = false;
        SDL_Surface* converted = nullptr;
        if (hasAlpha) {
            converted = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA8888);
        } else {
            converted = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGB24);
        }
        SDL_DestroySurface(surface);

        if (converted == nullptr) {
            continue;
        }

        if (width == 0) {
            width = converted->w;
            height = converted->h;
        }

        unsigned char* pixels = (unsigned char*)converted->pixels;
        int pitch = converted->pitch;

        for (int y = 0; y < height; y++) {
            unsigned char* row = pixels + y * pitch;
            if (hasAlpha) {
                // 修复颜色通道顺序：SDL_PIXELFORMAT_RGBA8888在小端系统上是ABGR布局
                for (int x = 0; x < width; x++) {
                    unsigned char a = row[x * 4 + 0];
                    unsigned char b = row[x * 4 + 1];
                    unsigned char g = row[x * 4 + 2];
                    unsigned char r = row[x * 4 + 3];
                    allPixels.push_back(r);
                    allPixels.push_back(g);
                    allPixels.push_back(b);
                    allPixels.push_back(a);
                }
            } else {
                for (int x = 0; x < width; x++) {
                    unsigned char r = row[x * 3 + 0];
                    unsigned char g = row[x * 3 + 1];
                    unsigned char b = row[x * 3 + 2];
                    allPixels.push_back(r);
                    allPixels.push_back(g);
                    allPixels.push_back(b);
                    allPixels.push_back(255);
                }
            }
        }
        SDL_DestroySurface(converted);
    }

    TextureInfo info;
    info.width = width;
    info.height = height;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.isCubemap = true;
    info.samplerType = SamplerType::Linear;
    info.refCount = 1;

    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 6;
    imageInfo.format = info.format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

    VkResult err = vkCreateImage(m_Device, &imageInfo, m_Allocator, &info.image);
    if (err != VK_SUCCESS) {
        LOGE("Failed to create cubemap image");
        return false;
    }

    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(m_Device, info.image, &memRequirements);

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = FindMemoryType(memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    err = vkAllocateMemory(m_Device, &allocInfo, m_Allocator, &info.imageMemory);
    if (err != VK_SUCCESS) {
        LOGE("Failed to allocate cubemap memory");
        return false;
    }

    vkBindImageMemory(m_Device, info.image, info.imageMemory, 0);

    VkBuffer stagingBuffer;
    VkDeviceMemory stagingBufferMemory;
    VkDeviceSize imageSize = allPixels.size();

    VkBufferCreateInfo bufferInfo = {};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = imageSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    err = vkCreateBuffer(m_Device, &bufferInfo, m_Allocator, &stagingBuffer);
    if (err != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements stagingMemRequirements;
    vkGetBufferMemoryRequirements(m_Device, stagingBuffer, &stagingMemRequirements);

    VkMemoryAllocateInfo stagingAllocInfo = {};
    stagingAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    stagingAllocInfo.allocationSize = stagingMemRequirements.size;
    stagingAllocInfo.memoryTypeIndex = FindMemoryType(stagingMemRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    err = vkAllocateMemory(m_Device, &stagingAllocInfo, m_Allocator, &stagingBufferMemory);
    if (err != VK_SUCCESS) {
        return false;
    }

    vkBindBufferMemory(m_Device, stagingBuffer, stagingBufferMemory, 0);

    void* data;
    vkMapMemory(m_Device, stagingBufferMemory, 0, imageSize, 0, &data);
    memcpy(data, allPixels.data(), (size_t)imageSize);
    vkUnmapMemory(m_Device, stagingBufferMemory);

    VkCommandBuffer commandBuffer;
    VkCommandBufferAllocateInfo cmdAllocInfo = {};
    cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAllocInfo.commandPool = m_CommandPool;
    cmdAllocInfo.commandBufferCount = 1;

    vkAllocateCommandBuffers(m_Device, &cmdAllocInfo, &commandBuffer);
    
    VkCommandBufferBeginInfo cmdBeginInfo = {};
    cmdBeginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cmdBeginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commandBuffer, &cmdBeginInfo);

    std::array<VkImageSubresourceRange, 6> subresourceRanges;
    for (int i = 0; i < 6; i++) {
        subresourceRanges[i].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        subresourceRanges[i].baseMipLevel = 0;
        subresourceRanges[i].levelCount = 1;
        subresourceRanges[i].baseArrayLayer = i;
        subresourceRanges[i].layerCount = 1;
    }

    std::array<VkBufferImageCopy, 6> bufferCopyRegions;
    VkDeviceSize offset = 0;
    for (int i = 0; i < 6; i++) {
        bufferCopyRegions[i].bufferOffset = offset;
        bufferCopyRegions[i].bufferRowLength = 0;  // 0 表示紧密排列的数据
        bufferCopyRegions[i].bufferImageHeight = 0; // 0 表示紧密排列的数据
        bufferCopyRegions[i].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bufferCopyRegions[i].imageSubresource.mipLevel = 0;
        bufferCopyRegions[i].imageSubresource.baseArrayLayer = i;
        bufferCopyRegions[i].imageSubresource.layerCount = 1;
        bufferCopyRegions[i].imageOffset = {0, 0, 0};
        bufferCopyRegions[i].imageExtent = {(uint32_t)width, (uint32_t)height, 1};
        offset += width * height * 4;
    }

    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = info.image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 6;

    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    vkCmdCopyBufferToImage(commandBuffer, stagingBuffer, info.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 6, bufferCopyRegions.data());

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(commandBuffer);

    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    vkQueueSubmit(m_Queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_Queue);

    vkFreeCommandBuffers(m_Device, m_CommandPool, 1, &commandBuffer);
    vkDestroyBuffer(m_Device, stagingBuffer, m_Allocator);
    vkFreeMemory(m_Device, stagingBufferMemory, m_Allocator);

    if (!CreateImageView(info.image, info.format, VK_IMAGE_ASPECT_COLOR_BIT, true, 1, info.imageView)) {   // cubemap：不生成 mip（skybox 全屏无 mip 需求）
        LOGE("[TexturePool] Failed to create cubemap image view: %s", basePath.c_str());
        return false;
    }
    
    // 创建描述符集布局和描述符�
    // 
if (!CreateDescriptorSetLayout(info, info.descriptorSetLayout)) {
        return false;
    }
    if (!CreateDescriptorSet(info, info.descriptorSetLayout, info.descriptorSet)) {
        return false;
    }

    m_Textures[name] = info;
    return true;
    } catch (const std::exception& ex) {
        LOGE("[TexturePool] EXCEPTION in LoadCubemapFromFaces: %s", ex.what());
        return false;
    } catch (...) {
        LOGE("[TexturePool] UNKNOWN EXCEPTION in LoadCubemapFromFaces");
        return false;
    }
}

// ============ 2026-08-12 静态 HDR 天空盒（IBL）——.hdr RGBE → equirect → cubemap SFLOAT + mip 链 ============
// RGBE (.hdr) 解码 → 线性 float RGB（参考 stb_image 的 hdr 读取逻辑）
static bool LoadRGBEToLinear(const std::string& path, std::vector<float>& outRGB, int& outW, int& outH) {
#ifdef __ANDROID__
    // Android：APK assets 不是真实文件系统，std::ifstream 读不到；SDL_IOFromFile 相对路径 fallback 到 assets://
    std::string mem;
    {
        SDL_IOStream* io = SDL_IOFromFile(path.c_str(), "rb");
        if (io == nullptr) { LOGE("[TexturePool] LoadHDRCubemap: 无法打开 %s", path.c_str()); return false; }
        Sint64 sz = SDL_GetIOSize(io);
        if (sz <= 0) { LOGE("[TexturePool] LoadHDRCubemap: 空文件 %s", path.c_str()); SDL_CloseIO(io); return false; }
        mem.resize((size_t)sz);
        if (SDL_ReadIO(io, mem.data(), (size_t)sz) != (size_t)sz) { LOGE("[TexturePool] LoadHDRCubemap: 读取失败 %s", path.c_str()); SDL_CloseIO(io); return false; }
        SDL_CloseIO(io);
    }
    std::istringstream f(mem, std::ios::binary);
#else
    std::ifstream f(path, std::ios::binary);
    if (!f) { LOGE("[TexturePool] LoadHDRCubemap: 无法打开 %s", path.c_str()); return false; }
#endif
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) break;   // 头部结束（空行）——FORMAT=32-bit_rle_rgbe 在头部内
    }
    std::getline(f, line);   // 分辨率行：-Y H +X W
    int w = 0, h = 0;
    if (sscanf(line.c_str(), "-Y %d +X %d", &h, &w) != 2 || w <= 0 || h <= 0) {
        LOGE("[TexturePool] LoadHDRCubemap: 分辨率行解析失败: %s", line.c_str());
        return false;
    }
    std::vector<unsigned char> rgbe((size_t)w * h * 4);
    std::vector<unsigned char> row((size_t)w * 4);
    for (int y = 0; y < h; y++) {
        unsigned char header[4];
        f.read((char*)header, 4);
        if (header[0] == 2 && header[1] == 2 && header[2] == (unsigned char)(w >> 8) && header[3] == (unsigned char)(w & 0xFF)) {
            for (int c = 0; c < 4; c++) {
                int x = 0;
                while (x < w) {
                    unsigned char count = 0;
                    f.read((char*)&count, 1);
                    if (count > 128) {
                        unsigned char val = 0;
                        f.read((char*)&val, 1);
                        for (int i = 0; i < (int)count - 128 && x < w; i++) row[(size_t)x++ * 4 + c] = val;
                    } else {
                        for (int i = 0; i < (int)count && x < w; i++) f.read((char*)&row[(size_t)x++ * 4 + c], 1);
                    }
                }
            }
            memcpy(&rgbe[(size_t)y * w * 4], row.data(), (size_t)w * 4);
        } else {
            memcpy(&rgbe[(size_t)y * w * 4], header, 4);
            f.read((char*)&rgbe[(size_t)y * w * 4 + 4], (size_t)w * 4 - 4);
        }
    }
    outRGB.resize((size_t)w * h * 3);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        float r = rgbe[i*4+0], g = rgbe[i*4+1], b = rgbe[i*4+2], e = rgbe[i*4+3];
        float scale = ldexpf(1.0f, (int)e - 128);
        outRGB[i*3+0] = r * scale / 256.0f;
        outRGB[i*3+1] = g * scale / 256.0f;
        outRGB[i*3+2] = b * scale / 256.0f;
    }
    outW = w; outH = h;
    return true;
}

// cube 面方向（Vulkan 约定——与 atmo_sky_cube.comp FaceDir 一致）
static glm::vec3 CubeFaceDir(int face, float u, float v) {
    switch (face) {
        case 0: return glm::normalize(glm::vec3( 1.0f, -v, -u));
        case 1: return glm::normalize(glm::vec3(-1.0f, -v,  u));
        case 2: return glm::normalize(glm::vec3( u,  1.0f,  v));
        case 3: return glm::normalize(glm::vec3( u, -1.0f, -v));
        case 4: return glm::normalize(glm::vec3( u, -v,  1.0f));
        default: return glm::normalize(glm::vec3(-u, -v, -1.0f));
    }
}

static uint16_t FloatToHalf(float f) {
    uint32_t x; memcpy(&x, &f, 4);
    uint16_t sign = (uint16_t)((x >> 16) & 0x8000);
    int32_t exp = ((x >> 23) & 0xFF) - 127 + 15;
    uint32_t mant = x & 0x7FFFFF;
    if (exp <= 0) return sign;               // 0 / 亚规格化丢弃
    if (exp >= 31) return (uint16_t)(sign | 0x7C00);   // 饱和
    return (uint16_t)(sign | ((uint32_t)exp << 10) | (mant >> 13));
}

// 2026-08-12：3 阶实球谐投影（前置声明——定义在文件后部，LoadHDRCubemap 需调用）
static void ProjectSHFromFaces(const std::vector<float>& faces, uint32_t fs, float sh[27]);

bool TexturePool::LoadHDRCubemap(const std::string& name, const std::string& hdrPath, uint32_t faceSize) {
    try {
    if (m_Textures.find(name) != m_Textures.end()) { m_Textures[name].refCount++; return true; }

    std::vector<float> equirect;
    int ew = 0, eh = 0;
    if (!LoadRGBEToLinear(EngineConfig::ResolvePlatformPath(hdrPath), equirect, ew, eh)) return false;

    // equirect → 6 面（CPU 双线性——IDKEngine SampleSphericalMap 约定：atan(z,x)/asin(y)）
    std::vector<float> faces((size_t)faceSize * faceSize * 6 * 3);
    const glm::vec2 invAtan(0.1591f, 0.3183f);
    for (uint32_t face = 0; face < 6; face++) {
        for (uint32_t y = 0; y < faceSize; y++) {
            for (uint32_t x = 0; x < faceSize; x++) {
                float u = ((float)x + 0.5f) / (float)faceSize * 2.0f - 1.0f;
                float v = ((float)y + 0.5f) / (float)faceSize * 2.0f - 1.0f;
                glm::vec3 dir = CubeFaceDir((int)face, u, v);
                glm::vec2 uv = glm::vec2(std::atan2(dir.z, dir.x), std::asin(-glm::clamp(dir.y, -1.0f, 1.0f))) * invAtan + 0.5f;   // -asin：Y 翻转（equirect 顶部=天——2026-08-12 用户反馈 y 反）
                float fx = uv.x * ew - 0.5f, fy = uv.y * eh - 0.5f;
                int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
                float tx = fx - x0, ty = fy - y0;
                int x0w = (x0 % ew + ew) % ew, x1w = ((x0 + 1) % ew + ew) % ew;
                int y0c = glm::clamp(y0, 0, eh - 1), y1c = glm::clamp(y0 + 1, 0, eh - 1);
                auto P = [&](int px, int py) { const float* p = &equirect[((size_t)py * ew + px) * 3]; return glm::vec3(p[0], p[1], p[2]); };
                glm::vec3 c00 = P(x0w, y0c), c10 = P(x1w, y0c), c01 = P(x0w, y1c), c11 = P(x1w, y1c);
                glm::vec3 col = glm::mix(glm::mix(c00, c10, tx), glm::mix(c01, c11, tx), ty);
                float* dst = &faces[(((size_t)face * faceSize + y) * faceSize + x) * 3];
                dst[0] = col.r; dst[1] = col.g; dst[2] = col.b;
            }
        }
    }

    const uint32_t mips = 7;   // 512² → 1
    TextureInfo info;
    info.width = faceSize; info.height = faceSize;
    info.mipLevels = mips;
    info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    info.isCubemap = true;
    info.samplerType = SamplerType::Linear;
    info.refCount = 1;

    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = info.format;
    imageInfo.extent = { faceSize, faceSize, 1 };
    imageInfo.mipLevels = mips;
    imageInfo.arrayLayers = 6;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    VkResult err = vkCreateImage(m_Device, &imageInfo, m_Allocator, &info.image);
    if (err != VK_SUCCESS) { LOGE("[TexturePool] LoadHDRCubemap: vkCreateImage failed %d", (int)err); return false; }
    VkMemoryRequirements memReq; vkGetImageMemoryRequirements(m_Device, info.image, &memReq);
    VkMemoryAllocateInfo allocInfo = {}; allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = FindMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(m_Device, &allocInfo, m_Allocator, &info.imageMemory) != VK_SUCCESS) return false;
    vkBindImageMemory(m_Device, info.image, info.imageMemory, 0);

    // staging：mip0 6 面（RGBA half）——mip1-6 由 GPU blit 生成（2026-08-12 回退：用户判定资源侧无问题）
    VkDeviceSize imageSize = (VkDeviceSize)faceSize * faceSize * 6 * 8;
    VkBuffer stagingBuffer; VkDeviceMemory stagingBufferMemory;
    VkBufferCreateInfo bufferInfo = {}; bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = imageSize; bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateBuffer(m_Device, &bufferInfo, m_Allocator, &stagingBuffer) != VK_SUCCESS) return false;
    VkMemoryRequirements stagingMemReq; vkGetBufferMemoryRequirements(m_Device, stagingBuffer, &stagingMemReq);
    VkMemoryAllocateInfo stagingAllocInfo = {}; stagingAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    stagingAllocInfo.allocationSize = stagingMemReq.size;
    stagingAllocInfo.memoryTypeIndex = FindMemoryType(stagingMemReq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(m_Device, &stagingAllocInfo, m_Allocator, &stagingBufferMemory) != VK_SUCCESS) return false;
    vkBindBufferMemory(m_Device, stagingBuffer, stagingBufferMemory, 0);
    void* data = nullptr; vkMapMemory(m_Device, stagingBufferMemory, 0, imageSize, 0, &data);
    uint16_t* dst16 = (uint16_t*)data;
    for (size_t i = 0; i < faces.size() / 3; i++) {
        dst16[i*4+0] = FloatToHalf(faces[i*3+0]);
        dst16[i*4+1] = FloatToHalf(faces[i*3+1]);
        dst16[i*4+2] = FloatToHalf(faces[i*3+2]);
        dst16[i*4+3] = 0x3C00;   // 1.0
    }
    vkUnmapMemory(m_Device, stagingBufferMemory);

    // 上传 mip0 + GPU blit mip 链（box 平均——线性空间）
    VkCommandBufferAllocateInfo cmdAlloc = {}; cmdAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAlloc.commandPool = m_CommandPool; cmdAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cmdAlloc.commandBufferCount = 1;
    VkCommandBuffer commandBuffer; vkAllocateCommandBuffers(m_Device, &cmdAlloc, &commandBuffer);
    VkCommandBufferBeginInfo cmdBegin = {}; cmdBegin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cmdBegin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commandBuffer, &cmdBegin);

    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = info.image;
    barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 6 };
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    std::array<VkBufferImageCopy, 6> copyRegions;
    for (int i = 0; i < 6; i++) {
        copyRegions[i].bufferOffset = (VkDeviceSize)i * faceSize * faceSize * 8;
        copyRegions[i].bufferRowLength = 0; copyRegions[i].bufferImageHeight = 0;
        copyRegions[i].imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)i, 1 };
        copyRegions[i].imageOffset = {0, 0, 0};
        copyRegions[i].imageExtent = { faceSize, faceSize, 1 };
    }
    vkCmdCopyBufferToImage(commandBuffer, stagingBuffer, info.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 6, copyRegions.data());

    // blit mip 链（1..6——box 平均；每级：mip m-1 DST→SRC 读，mip m 保持 DST 写）
    for (uint32_t m = 1; m < mips; m++) {
        VkImageMemoryBarrier b = barrier;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, m - 1, 1, 0, 6 };
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        VkImageBlit blit = {};
        blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, m - 1, 0, 6 };
        blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 6 };
        uint32_t sw = faceSize >> (m - 1), dw = faceSize >> m;
        blit.srcOffsets[1] = { (int32_t)sw, (int32_t)sw, 1 };
        blit.dstOffsets[1] = { (int32_t)dw, (int32_t)dw, 1 };
        vkCmdBlitImage(commandBuffer, info.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, info.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    }

    VkImageMemoryBarrier finalBarrier = barrier;
    finalBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    finalBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    finalBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    finalBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    finalBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 6 };
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &finalBarrier);

    vkEndCommandBuffer(commandBuffer);
    VkSubmitInfo submitInfo = {}; submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1; submitInfo.pCommandBuffers = &commandBuffer;
    vkQueueSubmit(m_Queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_Queue);
    vkFreeCommandBuffers(m_Device, m_CommandPool, 1, &commandBuffer);
    vkDestroyBuffer(m_Device, stagingBuffer, m_Allocator);
    vkFreeMemory(m_Device, stagingBufferMemory, m_Allocator);

    if (!CreateImageView(info.image, info.format, VK_IMAGE_ASPECT_COLOR_BIT, true, mips, info.imageView)) return false;
    if (!CreateDescriptorSetLayout(info, info.descriptorSetLayout)) return false;
    if (!CreateDescriptorSet(info, info.descriptorSetLayout, info.descriptorSet)) return false;
    // 2026-08-12：SH 辐照度投影（CPU——RGB×9 系数，替代/对比 irradiance 卷积）
    ProjectSHFromFaces(faces, faceSize, info.shIrradiance);
    m_Textures[name] = info;
    LOGI("[TexturePool] LoadHDRCubemap: %s (%ux%u, %u mip, SFLOAT)", name.c_str(), faceSize, faceSize, mips);
    // 2026-08-12：自动生成辐照度图（diffuse IBL——用户指出缺失）——LinearNoMip sampler（强制 mip0 采样，防隐式 lod 模糊）
    GenerateIrradianceMap("sky_hdr_irr", info.imageView, GetSamplerByType(SamplerType::LinearNoMip));
    return true;
    } catch (const std::exception& ex) {
        LOGE("[TexturePool] EXCEPTION in LoadHDRCubemap: %s", ex.what());
        return false;
    } catch (...) {
        LOGE("[TexturePool] UNKNOWN EXCEPTION in LoadHDRCubemap");
        return false;
    }
}

// 2026-08-12：辐照度图（irradiance map）——diffuse IBL（漫反射半球积分，learnopengl 风格）
bool TexturePool::GenerateIrradianceMap(const std::string& name, VkImageView srcCubeView, VkSampler srcSampler, uint32_t size) {
    try {
    if (m_Textures.find(name) != m_Textures.end()) { m_Textures[name].refCount++; return true; }
    TextureInfo info;
    info.width = size; info.height = size;
    info.mipLevels = 1;
    info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    info.isCubemap = true;
    info.samplerType = SamplerType::Linear;
    info.refCount = 1;
    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = info.format;
    imageInfo.extent = { size, size, 1 };
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 6;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    if (vkCreateImage(m_Device, &imageInfo, m_Allocator, &info.image) != VK_SUCCESS) return false;
    VkMemoryRequirements memReq; vkGetImageMemoryRequirements(m_Device, info.image, &memReq);
    VkMemoryAllocateInfo allocInfo = {}; allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = FindMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(m_Device, &allocInfo, m_Allocator, &info.imageMemory) != VK_SUCCESS) return false;
    vkBindImageMemory(m_Device, info.image, info.imageMemory, 0);
    if (!CreateImageView(info.image, info.format, VK_IMAGE_ASPECT_COLOR_BIT, true, 1, info.imageView)) return false;

    // compute pipeline（ibl_irradiance.comp.spv）
    std::string spvPath = EngineConfig::GetShaderPath("ibl_irradiance.comp.spv");
    std::vector<uint32_t> code;
#ifdef __ANDROID__
    {
        // Android：APK assets 不是真实文件系统，fopen 读不到；SDL_IOFromFile 相对路径 fallback 到 assets://
        SDL_IOStream* io = SDL_IOFromFile(spvPath.c_str(), "rb");
        if (io == nullptr) { LOGE("[TexturePool] GenerateIrradianceMap: shader not found %s", spvPath.c_str()); return false; }
        Sint64 sz = SDL_GetIOSize(io);
        if (sz <= 0) { LOGE("[TexturePool] GenerateIrradianceMap: 空 shader %s", spvPath.c_str()); SDL_CloseIO(io); return false; }
        code.resize((size_t)sz / 4);
        if (SDL_ReadIO(io, code.data(), (size_t)sz) != (size_t)sz) { SDL_CloseIO(io); return false; }
        SDL_CloseIO(io);
    }
    const size_t sz = code.size() * 4;
#else
    FILE* f = fopen(spvPath.c_str(), "rb");
    if (!f) { LOGE("[TexturePool] GenerateIrradianceMap: shader not found %s", spvPath.c_str()); return false; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    code.resize((size_t)sz / 4);
    if (fread(code.data(), 4, code.size(), f) != code.size()) { fclose(f); return false; }
    fclose(f);
#endif
    VkShaderModule module;
    VkShaderModuleCreateInfo smci = {}; smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = sz; smci.pCode = code.data();
    if (vkCreateShaderModule(m_Device, &smci, m_Allocator, &module) != VK_SUCCESS) return false;
    VkDescriptorSetLayoutBinding bindings[2] = {
        { 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
        { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
    };
    VkDescriptorSetLayoutCreateInfo dsci = {}; dsci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsci.bindingCount = 2; dsci.pBindings = bindings;
    VkDescriptorSetLayout setLayout;
    if (vkCreateDescriptorSetLayout(m_Device, &dsci, m_Allocator, &setLayout) != VK_SUCCESS) return false;
    VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 12 };
    VkPipelineLayoutCreateInfo plci = {}; plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1; plci.pSetLayouts = &setLayout;
    plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
    VkPipelineLayout pipeLayout;
    if (vkCreatePipelineLayout(m_Device, &plci, m_Allocator, &pipeLayout) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo cpci = {}; cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpci.stage.module = module; cpci.stage.pName = "main";
    cpci.layout = pipeLayout;
    VkPipeline pipeline;
    if (vkCreateComputePipelines(m_Device, VK_NULL_HANDLE, 1, &cpci, m_Allocator, &pipeline) != VK_SUCCESS) return false;
    VkDescriptorPoolSize poolSizes[2] = {
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 },
    };
    VkDescriptorPoolCreateInfo dpci = {}; dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 1; dpci.poolSizeCount = 2; dpci.pPoolSizes = poolSizes;
    VkDescriptorPool pool;
    if (vkCreateDescriptorPool(m_Device, &dpci, m_Allocator, &pool) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo dsai = {}; dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = pool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &setLayout;
    VkDescriptorSet set;
    if (vkAllocateDescriptorSets(m_Device, &dsai, &set) != VK_SUCCESS) return false;
    VkDescriptorImageInfo outInfo = { VK_NULL_HANDLE, info.imageView, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo srcInfo = { srcSampler, srcCubeView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet writes[2] = {};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[0].dstSet = set; writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1; writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; writes[0].pImageInfo = &outInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[1].dstSet = set; writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1; writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[1].pImageInfo = &srcInfo;
    vkUpdateDescriptorSets(m_Device, 2, writes, 0, nullptr);

    VkCommandBufferAllocateInfo cmdAlloc = {}; cmdAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAlloc.commandPool = m_CommandPool; cmdAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cmdAlloc.commandBufferCount = 1;
    VkCommandBuffer cmd; vkAllocateCommandBuffers(m_Device, &cmdAlloc, &cmd);
    VkCommandBufferBeginInfo begin = {}; begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = info.image;
    barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6 };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 1, &set, 0, nullptr);
    struct PC { int32_t size[2]; uint32_t sampleCount; } pc = { { (int32_t)size, (int32_t)size }, 256 };
    vkCmdPushConstants(cmd, pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, (size + 7) / 8, (size + 7) / 8, 6);
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si = {}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    vkQueueSubmit(m_Queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_Queue);
    vkFreeCommandBuffers(m_Device, m_CommandPool, 1, &cmd);
    vkDestroyPipeline(m_Device, pipeline, m_Allocator);
    vkDestroyPipelineLayout(m_Device, pipeLayout, m_Allocator);
    vkDestroyDescriptorSetLayout(m_Device, setLayout, m_Allocator);
    vkDestroyDescriptorPool(m_Device, pool, m_Allocator);
    vkDestroyShaderModule(m_Device, module, m_Allocator);
    if (!CreateDescriptorSetLayout(info, info.descriptorSetLayout)) return false;
    if (!CreateDescriptorSet(info, info.descriptorSetLayout, info.descriptorSet)) return false;
    m_Textures[name] = info;
    LOGI("[TexturePool] GenerateIrradianceMap: %s (%ux%u)", name.c_str(), size, size);
    return true;
    } catch (const std::exception& ex) {
        LOGE("[TexturePool] EXCEPTION in GenerateIrradianceMap: %s", ex.what());
        return false;
    } catch (...) {
        LOGE("[TexturePool] UNKNOWN EXCEPTION in GenerateIrradianceMap");
        return false;
    }
}

// 2026-08-12：3 阶实球谐投影（环境光——cubemap 6 面等权立体角）+ irradiance 卷积核（A_l×π）
static void ProjectSHFromFaces(const std::vector<float>& faces, uint32_t fs, float sh[27]) {
    const float PI = 3.14159265359f;
    const float solidAngle = 4.0f * PI / (6.0f * fs * fs);
    memset(sh, 0, 27 * sizeof(float));
    for (uint32_t face = 0; face < 6; face++) {
        for (uint32_t y = 0; y < fs; y++) {
            for (uint32_t x = 0; x < fs; x++) {
                float u = ((float)x + 0.5f) / fs * 2.0f - 1.0f;
                float v = ((float)y + 0.5f) / fs * 2.0f - 1.0f;
                glm::vec3 d = CubeFaceDir((int)face, u, v);
                const float* c = &faces[(((size_t)face * fs + y) * fs + x) * 3];
                float Y[9];
                Y[0] = 0.282095f;
                Y[1] = -0.488603f * d.y;
                Y[2] =  0.488603f * d.z;
                Y[3] = -0.488603f * d.x;
                Y[4] =  1.092548f * d.x * d.y;
                Y[5] =  1.092548f * d.y * d.z;
                Y[6] =  0.315392f * (3.0f * d.z * d.z - 1.0f);
                Y[7] =  1.092548f * d.x * d.z;
                Y[8] =  0.546274f * (d.x * d.x - d.y * d.y);
                for (int i = 0; i < 9; i++) {
                    sh[i*3+0] += c[0] * Y[i] * solidAngle;
                    sh[i*3+1] += c[1] * Y[i] * solidAngle;
                    sh[i*3+2] += c[2] * Y[i] * solidAngle;
                }
            }
        }
    }
    // irradiance 卷积核（A_l×π）：A_0=π, A_1=2π/3, A_2=π/4——与辐照度图卷积（含 π）量级一致
    float A[3] = { PI, 2.0f * PI / 3.0f, PI / 4.0f };
    for (int i = 0; i < 9; i++) {
        int l = (i == 0) ? 0 : (i <= 3 ? 1 : 2);
        for (int ch = 0; ch < 3; ch++) sh[i*3+ch] *= A[l];
    }
}

bool TexturePool::ProjectSHIrradiance(const std::string& srcName, float shOut[27]) {
    auto it = m_Textures.find(srcName);
    if (it == m_Textures.end()) return false;
    memcpy(shOut, it->second.shIrradiance, 27 * sizeof(float));
    return true;
}

bool TexturePool::RegisterExternalTexture(const std::string& name, VkImage image, VkImageView imageView, uint32_t width, uint32_t height, VkFormat format, SamplerType samplerType) {
    if (m_Textures.find(name) != m_Textures.end()) {
        m_Textures[name].refCount++;
        return true;
    }

    TextureInfo info;
    info.image = image;
    info.imageMemory = VK_NULL_HANDLE; // 外部纹理不管理内�
    // 
info.imageView = imageView;
    info.width = width;
    info.height = height;
    info.format = format;
    info.isCubemap = false;
    info.samplerType = samplerType;
    info.refCount = 1;

    if (!CreateDescriptorSetLayout(info, info.descriptorSetLayout)) {
        return false;
    }

    if (!CreateDescriptorSet(info, info.descriptorSetLayout, info.descriptorSet)) {
        return false;
    }

    m_Textures[name] = info;
    LOGD("[TexturePool] Registered external texture: %s (%ux%u)", name.c_str(), width, height);
    return true;
}

bool TexturePool::LoadHeightmap16(const std::string& name,
                                  const std::string& filePath,
                                  SamplerType samplerType) {
    if (m_Textures.find(name) != m_Textures.end()) {
        m_Textures[name].refCount++;
        return true;
    }

    HeightmapPixels16 pixels;
    std::string errorMessage;
    if (!HeightmapLoader::LoadPng16(filePath, pixels, &errorMessage)) {
        LOGE("[TexturePool] Failed to load 16-bit heightmap '%s': %s",
             filePath.c_str(), errorMessage.c_str());
        return false;
    }

    VkFormatProperties formatProperties{};
    vkGetPhysicalDeviceFormatProperties(m_PhysicalDevice,
                                        VK_FORMAT_R16_UNORM,
                                        &formatProperties);
    if ((formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0) {
        LOGE("[TexturePool] VK_FORMAT_R16_UNORM is not sampleable on this device: %s",
             filePath.c_str());
        return false;
    }

    TextureInfo info;
    info.width = pixels.width;
    info.height = pixels.height;
    info.mipLevels = 1;
    info.format = VK_FORMAT_R16_UNORM;
    info.isCubemap = false;
    info.samplerType = samplerType;
    info.refCount = 1;

    if (!CreateTextureImage(info.width, info.height, info.format, 1,
                            info.image, info.imageMemory)) {
        LOGE("[TexturePool] Failed to create R16 heightmap image: %s", filePath.c_str());
        return false;
    }

    auto cleanupImage = [&]() {
        if (info.imageView != VK_NULL_HANDLE) {
            vkDestroyImageView(m_Device, info.imageView, m_Allocator);
            info.imageView = VK_NULL_HANDLE;
        }
        if (info.descriptorSetLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(m_Device, info.descriptorSetLayout, m_Allocator);
            info.descriptorSetLayout = VK_NULL_HANDLE;
        }
        if (info.image != VK_NULL_HANDLE) {
            vkDestroyImage(m_Device, info.image, m_Allocator);
            info.image = VK_NULL_HANDLE;
        }
        if (info.imageMemory != VK_NULL_HANDLE) {
            vkFreeMemory(m_Device, info.imageMemory, m_Allocator);
            info.imageMemory = VK_NULL_HANDLE;
        }
    };

    if (!TransitionImageLayout(info.image, info.format,
                               VK_IMAGE_LAYOUT_UNDEFINED,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)) {
        cleanupImage();
        return false;
    }

    const VkDeviceSize rowBytes = static_cast<VkDeviceSize>(info.width) * sizeof(uint16_t);
    const VkDeviceSize imageSize = rowBytes * static_cast<VkDeviceSize>(info.height);

    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = imageSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(m_Device, &bufferInfo, m_Allocator, &stagingBuffer) != VK_SUCCESS) {
        cleanupImage();
        return false;
    }

    VkMemoryRequirements memoryRequirements{};
    vkGetBufferMemoryRequirements(m_Device, stagingBuffer, &memoryRequirements);
    VkMemoryAllocateInfo allocationInfo{};
    allocationInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocationInfo.allocationSize = memoryRequirements.size;
    allocationInfo.memoryTypeIndex = FindMemoryType(
        memoryRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (vkAllocateMemory(m_Device, &allocationInfo, m_Allocator, &stagingMemory) != VK_SUCCESS) {
        vkDestroyBuffer(m_Device, stagingBuffer, m_Allocator);
        cleanupImage();
        return false;
    }
    vkBindBufferMemory(m_Device, stagingBuffer, stagingMemory, 0);

    void* mapped = nullptr;
    if (vkMapMemory(m_Device, stagingMemory, 0, imageSize, 0, &mapped) != VK_SUCCESS) {
        vkDestroyBuffer(m_Device, stagingBuffer, m_Allocator);
        vkFreeMemory(m_Device, stagingMemory, m_Allocator);
        cleanupImage();
        return false;
    }

    // Keep the engine's existing texture convention: upload rows bottom-up.
    // The decoder itself deliberately keeps the source PNG top-left oriented.
    auto* uploadPixels = static_cast<uint8_t*>(mapped);
    for (uint32_t y = 0; y < info.height; ++y) {
        const uint32_t sourceY = info.height - 1u - y;
        std::memcpy(uploadPixels + static_cast<size_t>(y) * static_cast<size_t>(rowBytes),
                    pixels.samples.data() + static_cast<size_t>(sourceY) * info.width,
                    static_cast<size_t>(rowBytes));
    }
    vkUnmapMemory(m_Device, stagingMemory);

    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo commandAllocateInfo{};
    commandAllocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    commandAllocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandAllocateInfo.commandPool = m_CommandPool;
    commandAllocateInfo.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(m_Device, &commandAllocateInfo, &commandBuffer) != VK_SUCCESS) {
        vkDestroyBuffer(m_Device, stagingBuffer, m_Allocator);
        vkFreeMemory(m_Device, stagingMemory, m_Allocator);
        cleanupImage();
        return false;
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
        vkFreeCommandBuffers(m_Device, m_CommandPool, 1, &commandBuffer);
        vkDestroyBuffer(m_Device, stagingBuffer, m_Allocator);
        vkFreeMemory(m_Device, stagingMemory, m_Allocator);
        cleanupImage();
        return false;
    }

    VkBufferImageCopy copyRegion{};
    copyRegion.bufferOffset = 0;
    copyRegion.bufferRowLength = 0;
    copyRegion.bufferImageHeight = 0;
    copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyRegion.imageSubresource.mipLevel = 0;
    copyRegion.imageSubresource.baseArrayLayer = 0;
    copyRegion.imageSubresource.layerCount = 1;
    copyRegion.imageExtent = {info.width, info.height, 1};
    vkCmdCopyBufferToImage(commandBuffer, stagingBuffer, info.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

    if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
        vkFreeCommandBuffers(m_Device, m_CommandPool, 1, &commandBuffer);
        vkDestroyBuffer(m_Device, stagingBuffer, m_Allocator);
        vkFreeMemory(m_Device, stagingMemory, m_Allocator);
        cleanupImage();
        return false;
    }

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    const VkResult submitResult = vkQueueSubmit(m_Queue, 1, &submitInfo, VK_NULL_HANDLE);
    const VkResult waitResult = submitResult == VK_SUCCESS ? vkQueueWaitIdle(m_Queue) : submitResult;
    vkFreeCommandBuffers(m_Device, m_CommandPool, 1, &commandBuffer);
    vkDestroyBuffer(m_Device, stagingBuffer, m_Allocator);
    vkFreeMemory(m_Device, stagingMemory, m_Allocator);
    if (waitResult != VK_SUCCESS) {
        cleanupImage();
        return false;
    }

    if (!TransitionImageLayout(info.image, info.format,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)) {
        cleanupImage();
        return false;
    }

    if (!CreateImageView(info.image, info.format, VK_IMAGE_ASPECT_COLOR_BIT,
                         false, 1, info.imageView)) {
        cleanupImage();
        return false;
    }

    // Heightmaps are normally sampled in the terrain vertex shader. Existing
    // material textures remain fragment-only; only this descriptor layout is
    // made visible to both stages.
    if (!CreateDescriptorSetLayout(info, info.descriptorSetLayout,
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)) {
        cleanupImage();
        return false;
    }
    if (!CreateDescriptorSet(info, info.descriptorSetLayout, info.descriptorSet)) {
        cleanupImage();
        return false;
    }

    m_Textures[name] = info;
    LOGD("[TexturePool] Loaded 16-bit heightmap: %s (%ux%u, R16_UNORM)",
         filePath.c_str(), info.width, info.height);
    return true;
}

bool TexturePool::LoadTexture2D(const std::string& name, const std::string& filePath, SamplerType samplerType) {
    try {
    if (m_Textures.find(name) != m_Textures.end()) {
        m_Textures[name].refCount++;
        return true;
    }

    std::string fullPath;
    
#ifdef __ANDROID__
    // Android: 直接使用传入的路径（相对�
    // assets 目录�?
fullPath = filePath;
    LOGD("[TexturePool] Android loading texture: %s", filePath.c_str());
#else
    fullPath = ProjectManager::GetInstance().ResolveAssetPath(filePath);
    if (fullPath.empty()) {
        LOGE("[TexturePool] Cannot resolve project texture: %s", filePath.c_str());
        return false;
    }
#endif

    // KTX2 压缩纹理自动优先�
//  - 路径已是 .ktx2 �
    // 直接�?BasisU 转码路径（桌�?BC7 / 真机 ASTC；内�?mip�?    //  - 否则探测同名 .ktx2�?png/.jpg/.jpeg �?.ktx2），存在则走压缩路径
    // 注：扩展名用 find_last_of('.') 定位�?ktx2/.jpeg �?5 字符，旧代码固定�?4 字符会生成错误候选）
    const std::string fullPathExt = fullPath.size() >= 5 ? fullPath.substr(fullPath.size() - 5) : "";
    if (fullPathExt == ".ktx2") {
        return LoadTextureKtx2(name, fullPath, samplerType);
    }
    std::string ktx2Candidate = fullPath;
    const size_t dotPos = ktx2Candidate.find_last_of('.');
    if (dotPos != std::string::npos) {
        ktx2Candidate = ktx2Candidate.substr(0, dotPos) + ".ktx2";
    } else {
        ktx2Candidate += ".ktx2";
    }
    FILE* probe = fopen(ktx2Candidate.c_str(), "rb");
    if (probe) {
        fclose(probe);
        return LoadTextureKtx2(name, ktx2Candidate, samplerType);
    }

    SDL_Surface* surface = nullptr;
    // DDS 解码像素缓冲：解码输出即 [R,G,B,A] 最终布局，直接构造 imageData（函数级，供下方公共路径使用）
    std::vector<uint8_t> ddsRGBA;
    int ddsW = 0, ddsH = 0;

    // DDS 解码路径：SDL_image 裁剪版无 DDS loader（实测 DXT1 也报 Unsupported image format）
    // DXT1/3/5 + ATI2(BC5)；BC7/BC6H 不支持（提示转 KTX2——引擎 KTX2 路径覆盖）
#ifndef __ANDROID__
    const size_t dotPosExt = fullPath.find_last_of('.');
    if (dotPosExt != std::string::npos) {
        std::string ext = fullPath.substr(dotPosExt);
        for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext == ".dds") {
            std::vector<uint8_t> ddsBytes;
            {
                FILE* f = fopen(fullPath.c_str(), "rb");
                if (!f) {
                    LOGE("[TexturePool] Failed to open DDS: %s", filePath.c_str());
                    return false;
                }
                fseek(f, 0, SEEK_END);
                const long len = ftell(f);
                fseek(f, 0, SEEK_SET);
                if (len <= 0) { fclose(f); LOGE("[TexturePool] Empty DDS: %s", filePath.c_str()); return false; }
                ddsBytes.resize(static_cast<size_t>(len));
                const size_t rd = fread(ddsBytes.data(), 1, ddsBytes.size(), f);
                fclose(f);
                if (rd != ddsBytes.size()) { LOGE("[TexturePool] Failed to read DDS: %s", filePath.c_str()); return false; }
            }
            if (!DdsDecoder::Decode(ddsBytes.data(), ddsBytes.size(), ddsW, ddsH, ddsRGBA)) {
                LOGE("[TexturePool] Unsupported DDS format (BC7/BC6H/BC4?): %s - please convert to KTX2 (engine BasisU path supports BC7/ASTC)", filePath.c_str());
                return false;
            }
            LOGD("[TexturePool] DDS decoded: %s (%dx%d)", filePath.c_str(), ddsW, ddsH);
            // DDS 解码器输出 = 小端 RGBA8888 字节序 [R,G,B,A]（top-down）——直接构造最终布局，
            // 不走 ConvertSurface/通道修复链（那套假设 [A,B,G,R]，会重排成彩色斑点）
        }
    }
#endif
    
#ifdef __ANDROID__
    // Android: 使用SDL_IOStream加载assets中的文件
    SDL_IOStream* io = SDL_IOFromFile(fullPath.c_str(), "rb");
    if (io == nullptr) {
        LOGE("[TexturePool] Failed to open file: %s", fullPath.c_str());
        LOGE("[TexturePool] SDL Error: %s", SDL_GetError());
        return false;
    }
    
    surface = IMG_Load_IO(io, 1); // 1 = 关闭 IO 流
    if (surface == nullptr) {
        LOGE("[TexturePool] Failed to load texture from IO: %s", filePath.c_str());
        LOGE("[TexturePool] SDL Error: %s", SDL_GetError());
        return false;
    }
#else
    // DDS 分支已解码时跳过 IMG_Load（SDL_image �?DDS loader，会覆盖成失败）
    if (ddsRGBA.empty()) {
        surface = IMG_Load(fullPath.c_str());
        if (surface == nullptr) {
            LOGE("[TexturePool] Failed to load texture: %s", filePath.c_str());
            LOGE("[TexturePool] Full path: %s", fullPath.c_str());
            LOGE("[TexturePool] SDL Error: %s", SDL_GetError());
            return false;
        }
    }
#endif
    
    const bool ddsDecoded = !ddsRGBA.empty();
    LOGD("[TexturePool] Successfully loaded texture: %s (%dx%d)", filePath.c_str(),
         ddsDecoded ? ddsW : surface->w, ddsDecoded ? ddsH : surface->h);

    TextureInfo info;
    info.width = ddsDecoded ? ddsW : surface->w;
    info.height = ddsDecoded ? ddsH : surface->h;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.isCubemap = false;
    info.samplerType = samplerType;
    info.refCount = 1;
    // mip 链层级：完整链（minification 带宽优化�
    // x1 纹理 mip=1；Nearest 采样纹理如蓝噪声不生�?mip——mip 混合会破坏精确采�?抖动模式�?
info.mipLevels = 1;
    if ((info.width > 1 || info.height > 1) && samplerType != SamplerType::Nearest) {
        info.mipLevels = 1 + static_cast<uint32_t>(std::floor(std::log2(static_cast<double>(std::max(info.width, info.height)))));
    }

    bool hasAlpha = false;
    std::vector<unsigned char> imageData;
    if (ddsDecoded) {
        // DDS：解码输出已�
    // [R,G,B,A]（top-down），仅需 Y 翻转（Vulkan 原点左下�?
imageData.resize((size_t)ddsW * ddsH * 4);
        for (int y = 0; y < ddsH; y++) {
            const unsigned char* src = ddsRGBA.data() + (size_t)y * ddsW * 4;
            unsigned char* dst = imageData.data() + (size_t)(ddsH - 1 - y) * ddsW * 4;
            memcpy(dst, src, (size_t)ddsW * 4);
        }
    } else {
        SDL_Surface* converted = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA8888);
        SDL_DestroySurface(surface);

        if (converted == nullptr) {
            LOGE("[TexturePool] Failed to convert surface: %s", SDL_GetError());
            return false;
        }

        // 修复颜色通道顺序：SDL_PIXELFORMAT_RGBA8888在小端系统上是ABGR布局
        // 需要转换为RGBA布局以匹配VK_FORMAT_R8G8B8A8_UNORM
        unsigned char* pixels = (unsigned char*)converted->pixels;
        int width = converted->w;
        int height = converted->h;
        int pitch = converted->pitch;

        imageData.resize((size_t)width * height * 4);

        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                // SDL_PIXELFORMAT_RGBA8888在小端系统上的内存布局�? A B G R
                unsigned char a = pixels[y * pitch + x * 4 + 0];
                unsigned char b = pixels[y * pitch + x * 4 + 1];
                unsigned char g = pixels[y * pitch + x * 4 + 2];
                unsigned char r = pixels[y * pitch + x * 4 + 3];

                // 翻转y轴：Vulkan纹理原点是左下角，SDL图像原点是左上角
                int flippedY = height - 1 - y;

                // 转换为RGBA布局: R G B A
                imageData[(flippedY * width + x) * 4 + 0] = r;
                imageData[(flippedY * width + x) * 4 + 1] = g;
                imageData[(flippedY * width + x) * 4 + 2] = b;
                imageData[(flippedY * width + x) * 4 + 3] = a;
            }
        }

        SDL_DestroySurface(converted);
    }

    if (!CreateTextureImage(info.width, info.height, info.format, info.mipLevels, info.image, info.imageMemory)) {
        return false;
    }

    if (!TransitionImageLayout(info.image, info.format, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)) {
        return false;
    }

    VkBuffer stagingBuffer;
    VkDeviceMemory stagingBufferMemory;
    VkDeviceSize imageSize = imageData.size();

    VkBufferCreateInfo bufferInfo = {};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = imageSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult err = vkCreateBuffer(m_Device, &bufferInfo, m_Allocator, &stagingBuffer);
    if (err != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(m_Device, stagingBuffer, &memRequirements);

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = FindMemoryType(memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    err = vkAllocateMemory(m_Device, &allocInfo, m_Allocator, &stagingBufferMemory);
    if (err != VK_SUCCESS) {
        return false;
    }

    vkBindBufferMemory(m_Device, stagingBuffer, stagingBufferMemory, 0);

    void* data;
    vkMapMemory(m_Device, stagingBufferMemory, 0, imageSize, 0, &data);
    memcpy(data, imageData.data(), (size_t)imageSize);
    vkUnmapMemory(m_Device, stagingBufferMemory);

    VkCommandBuffer commandBuffer;
    VkCommandBufferAllocateInfo cmdAllocInfo = {};
    cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAllocInfo.commandPool = m_CommandPool;
    cmdAllocInfo.commandBufferCount = 1;

    vkAllocateCommandBuffers(m_Device, &cmdAllocInfo, &commandBuffer);

    VkCommandBufferBeginInfo cmdBeginInfo2 = {};
    cmdBeginInfo2.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cmdBeginInfo2.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commandBuffer, &cmdBeginInfo2);

    VkBufferImageCopy region = {};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;  // 0 表示紧密排列的数据
    region.bufferImageHeight = 0; // 0 表示紧密排列的数据
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {info.width, info.height, 1};

    vkCmdCopyBufferToImage(commandBuffer, stagingBuffer, info.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // ===== 生成 mip 链（GPU blit 逐级 2x 缩小；纹理带宽优化核心）=====
    if (info.mipLevels > 1) {
        // level0: TRANSFER_DST �?TRANSFER_SRC（作为后�?blit 源）
        VkImageMemoryBarrier srcBarrier = {};
        srcBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        srcBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        srcBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        srcBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        srcBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        srcBarrier.image = info.image;
        srcBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        srcBarrier.subresourceRange.baseMipLevel = 0;
        srcBarrier.subresourceRange.levelCount = 1;
        srcBarrier.subresourceRange.baseArrayLayer = 0;
        srcBarrier.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &srcBarrier);

        for (uint32_t mip = 1; mip < info.mipLevels; mip++) {
            const int32_t srcW = std::max(1, static_cast<int32_t>(info.width) >> (mip - 1));
            const int32_t srcH = std::max(1, static_cast<int32_t>(info.height) >> (mip - 1));
            const int32_t dstW = std::max(1, static_cast<int32_t>(info.width) >> mip);
            const int32_t dstH = std::max(1, static_cast<int32_t>(info.height) >> mip);

            // dst mip: UNDEFINED �
    // TRANSFER_DST（blit 目标�?
VkImageMemoryBarrier dstBarrier = {};
            dstBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            dstBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            dstBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            dstBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            dstBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            dstBarrier.image = info.image;
            dstBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            dstBarrier.subresourceRange.baseMipLevel = mip;
            dstBarrier.subresourceRange.levelCount = 1;
            dstBarrier.subresourceRange.baseArrayLayer = 0;
            dstBarrier.subresourceRange.layerCount = 1;
            vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &dstBarrier);

            VkImageBlit blit = {};
            blit.srcOffsets[0] = {0, 0, 0};
            blit.srcOffsets[1] = {srcW, srcH, 1};
            blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.srcSubresource.mipLevel = mip - 1;
            blit.srcSubresource.baseArrayLayer = 0;
            blit.srcSubresource.layerCount = 1;
            blit.dstOffsets[0] = {0, 0, 0};
            blit.dstOffsets[1] = {dstW, dstH, 1};
            blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.dstSubresource.mipLevel = mip;
            blit.dstSubresource.baseArrayLayer = 0;
            blit.dstSubresource.layerCount = 1;
            vkCmdBlitImage(commandBuffer, info.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           info.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        }
    }

    // ===== 最终布局转换：全�?mip �?SHADER_READ_ONLY =====
    // level0 �?TRANSFER_SRC（blit 后），level1+ �?TRANSFER_DST——分两次 barrier
    if (info.mipLevels > 1) {
        // level0: SRC �?READ_ONLY
        VkImageMemoryBarrier b0 = {};
        b0.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b0.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b0.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b0.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b0.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b0.image = info.image;
        b0.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b0.subresourceRange.baseMipLevel = 0;
        b0.subresourceRange.levelCount = 1;
        b0.subresourceRange.baseArrayLayer = 0;
        b0.subresourceRange.layerCount = 1;

        // level1..N: DST �?READ_ONLY
        VkImageMemoryBarrier b1 = {};
        b1.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b1.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b1.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b1.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b1.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b1.image = info.image;
        b1.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b1.subresourceRange.baseMipLevel = 1;
        b1.subresourceRange.levelCount = info.mipLevels - 1;
        b1.subresourceRange.baseArrayLayer = 0;
        b1.subresourceRange.layerCount = 1;

        VkImageMemoryBarrier finalBarriers[2] = {b0, b1};
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2, finalBarriers);
    } else {
        // �?mip�?x1 等）：单 barrier DST �?READ_ONLY
        VkImageMemoryBarrier barrier = {};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = info.image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;

        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    vkEndCommandBuffer(commandBuffer);

    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    vkQueueSubmit(m_Queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_Queue);

    vkFreeCommandBuffers(m_Device, m_CommandPool, 1, &commandBuffer);
    vkDestroyBuffer(m_Device, stagingBuffer, m_Allocator);
    vkFreeMemory(m_Device, stagingBufferMemory, m_Allocator);

    // 确保图像布局完全转换后再创建图像视图
    // 为Adreno GPU添加额外的同�
    // 
VkCommandBuffer syncCommandBuffer;
    VkCommandBufferAllocateInfo syncCmdAllocInfo = {};
    syncCmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    syncCmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    syncCmdAllocInfo.commandPool = m_CommandPool;
    syncCmdAllocInfo.commandBufferCount = 1;
    vkAllocateCommandBuffers(m_Device, &syncCmdAllocInfo, &syncCommandBuffer);

    VkCommandBufferBeginInfo syncCmdBeginInfo = {};
    syncCmdBeginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    syncCmdBeginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(syncCommandBuffer, &syncCmdBeginInfo);

    // 添加内存屏障确保布局转换完成
    VkImageMemoryBarrier syncBarrier = {};
    syncBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    syncBarrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    syncBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    syncBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    syncBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    syncBarrier.image = info.image;
    syncBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    syncBarrier.subresourceRange.baseMipLevel = 0;
    syncBarrier.subresourceRange.levelCount = 1;
    syncBarrier.subresourceRange.baseArrayLayer = 0;
    syncBarrier.subresourceRange.layerCount = 1;

    vkCmdPipelineBarrier(syncCommandBuffer, 
                        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 
                        0, 0, nullptr, 0, nullptr, 1, &syncBarrier);

    vkEndCommandBuffer(syncCommandBuffer);

    VkSubmitInfo syncSubmitInfo = {};
    syncSubmitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    syncSubmitInfo.commandBufferCount = 1;
    syncSubmitInfo.pCommandBuffers = &syncCommandBuffer;

    vkQueueSubmit(m_Queue, 1, &syncSubmitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_Queue);

    vkFreeCommandBuffers(m_Device, m_CommandPool, 1, &syncCommandBuffer);

    // 创建 image view（必须在此：descriptor 写入时视图必须有效，否则 GPU 读空视图不渲染）
    if (!CreateImageView(info.image, info.format, VK_IMAGE_ASPECT_COLOR_BIT, false, info.mipLevels, info.imageView)) {
        LOGE("[TexturePool] Failed to create image view: %s", filePath.c_str());
        return false;
    }

    // 创建描述符集布局和描述符集
    if (!CreateDescriptorSetLayout(info, info.descriptorSetLayout)) {
        return false;
    }
    if (!CreateDescriptorSet(info, info.descriptorSetLayout, info.descriptorSet)) {
        return false;
    }

    m_Textures[name] = info;
    return true;
    } catch (const std::exception& ex) {
        LOGE("[TexturePool] EXCEPTION in LoadTexture2D(%s): %s", filePath.c_str(), ex.what());
        return false;
    } catch (...) {
        LOGE("[TexturePool] UNKNOWN EXCEPTION in LoadTexture2D(%s)", filePath.c_str());
        return false;
    }
}

const TextureInfo* TexturePool::GetTexture(const std::string& name) const
{
    auto it = m_Textures.find(name);
    if (it != m_Textures.end()) {
        return &it->second;
    }
    return nullptr;
}

VkDescriptorSet TexturePool::GetDescriptorSet(const std::string& name) const
{
    auto it = m_Textures.find(name);
    if (it != m_Textures.end()) {
        return it->second.descriptorSet;
    }
    return VK_NULL_HANDLE;
}

VkImageView TexturePool::GetImageView(const std::string& name) const
{
    auto it = m_Textures.find(name);
    if (it != m_Textures.end()) {
        return it->second.imageView;
    }
    return VK_NULL_HANDLE;
}

VkSampler TexturePool::GetSampler(const std::string& name) const
{
    auto it = m_Textures.find(name);
    if (it != m_Textures.end()) {
        auto samplerIt = m_SamplerPool.find(it->second.samplerType);
        if (samplerIt != m_SamplerPool.end()) {
            return samplerIt->second;
        }
    }
    return VK_NULL_HANDLE;
}

VkSampler TexturePool::GetSamplerByType(SamplerType type) const
{
    auto it = m_SamplerPool.find(type);
    if (it != m_SamplerPool.end()) {
        return it->second;
    }
    return VK_NULL_HANDLE;
}

void TexturePool::AddRef(const std::string& name)
{
    auto it = m_Textures.find(name);
    if (it != m_Textures.end()) {
        it->second.refCount++;
    }
}

void TexturePool::Release(const std::string& name)
{
    auto it = m_Textures.find(name);
    if (it != m_Textures.end()) {
        it->second.refCount--;
        if (it->second.refCount <= 0) {
            // 释放描述符集布局
            if (it->second.descriptorSetLayout != VK_NULL_HANDLE) {
                vkDestroyDescriptorSetLayout(m_Device, it->second.descriptorSetLayout, m_Allocator);
            }
            // 释放图像视图
            if (it->second.imageView != VK_NULL_HANDLE) {
                vkDestroyImageView(m_Device, it->second.imageView, m_Allocator);
            }
            // 释放图像
            if (it->second.image != VK_NULL_HANDLE) {
                vkDestroyImage(m_Device, it->second.image, m_Allocator);
            }
            // 释放设备内存
            if (it->second.imageMemory != VK_NULL_HANDLE) {
                vkFreeMemory(m_Device, it->second.imageMemory, m_Allocator);
            }
            // �
    // map 中移�?
m_Textures.erase(it);
        }
    }
}

void TexturePool::Cleanup()
{
    for (auto& pair : m_Textures) {
        TextureInfo& info = pair.second;
        if (info.descriptorSetLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(m_Device, info.descriptorSetLayout, m_Allocator);
        }
        if (info.imageView != VK_NULL_HANDLE) {
            vkDestroyImageView(m_Device, info.imageView, m_Allocator);
        }
        if (info.image != VK_NULL_HANDLE) {
            vkDestroyImage(m_Device, info.image, m_Allocator);
        }
        if (info.imageMemory != VK_NULL_HANDLE) {
            vkFreeMemory(m_Device, info.imageMemory, m_Allocator);
        }
    }
    m_Textures.clear();
    
    CleanupSamplerPool();
    
    if (m_DescriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_Device, m_DescriptorPool, m_Allocator);
        m_DescriptorPool = VK_NULL_HANDLE;
    }
}

void TexturePool::ResetDescriptorPool()
{
    if (m_DescriptorPool == VK_NULL_HANDLE) {
        return;
    }
    
    // 重置描述符池，释放所有描述符�
    // 
VkResult result = vkResetDescriptorPool(m_Device, m_DescriptorPool, 0);
    if (result != VK_SUCCESS) {
        // 如果重置失败，尝试销毁并重新创建
        vkDestroyDescriptorPool(m_Device, m_DescriptorPool, m_Allocator);
        m_DescriptorPool = VK_NULL_HANDLE;
        
        // 重新创建描述符池
        VkDescriptorPoolSize poolSizes[] = {
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 }
        };
        
        VkDescriptorPoolCreateInfo poolInfo = {};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = poolSizes;
        poolInfo.maxSets = 1000;
        
        result = vkCreateDescriptorPool(m_Device, &poolInfo, m_Allocator, &m_DescriptorPool);
        if (result != VK_SUCCESS) {
            // 记录错误
        }
    }
    
    // 清空所有纹理的描述符集引用（因为它们已经被重置了）
    for (auto& pair : m_Textures) {
        pair.second.descriptorSet = VK_NULL_HANDLE;
    }
}

bool TexturePool::CreateDescriptorSetLayout(const TextureInfo& info,
                                            VkDescriptorSetLayout& layout,
                                            VkShaderStageFlags stageFlags) {
    VkDescriptorSetLayoutBinding binding = {};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = stageFlags;
    binding.pImmutableSamplers = nullptr;

    VkDescriptorSetLayoutCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    createInfo.bindingCount = 1;
    createInfo.pBindings = &binding;

    VkResult err = vkCreateDescriptorSetLayout(m_Device, &createInfo, m_Allocator, &layout);
    if (err != VK_SUCCESS) {
        return false;
    }

    return true;
}

bool TexturePool::CreateDescriptorSet(const TextureInfo& info, VkDescriptorSetLayout layout, VkDescriptorSet& descriptorSet) {
    if (m_DescriptorPool == VK_NULL_HANDLE) {
        return false;
    }

    // 分配描述符集
    VkDescriptorSetAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_DescriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &layout;

    VkResult err = vkAllocateDescriptorSets(m_Device, &allocInfo, &descriptorSet);
    if (err != VK_SUCCESS) {
        // 池满（VK_ERROR_OUT_OF_POOL_MEMORY）：销毁重建 2 倍容量池并重试一次
        const int newSets = m_DescriptorPoolMaxSets * 2;
        LOGD("[TexturePool] Descriptor pool full (%d sets), rebuilding to %d", m_DescriptorPoolMaxSets, newSets);
        vkDestroyDescriptorPool(m_Device, m_DescriptorPool, m_Allocator);
        VkDescriptorPoolSize growSize = {};
        growSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        growSize.descriptorCount = newSets;
        VkDescriptorPoolCreateInfo growInfo = {};
        growInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        growInfo.poolSizeCount = 1;
        growInfo.pPoolSizes = &growSize;
        growInfo.maxSets = newSets;
        if (vkCreateDescriptorPool(m_Device, &growInfo, m_Allocator, &m_DescriptorPool) != VK_SUCCESS) {
            m_DescriptorPool = VK_NULL_HANDLE;
            return false;
        }
        m_DescriptorPoolMaxSets = newSets;
        allocInfo.descriptorPool = m_DescriptorPool;
        err = vkAllocateDescriptorSets(m_Device, &allocInfo, &descriptorSet);
        if (err != VK_SUCCESS) {
            return false;
        }
    }

    // 更新描述符集
    VkDescriptorImageInfo imageInfo = {};
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo.imageView = info.imageView;
    imageInfo.sampler = m_SamplerPool.at(info.samplerType);

    VkWriteDescriptorSet descriptorWrite = {};
    descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrite.dstSet = descriptorSet;
    descriptorWrite.dstBinding = 0;
    descriptorWrite.dstArrayElement = 0;
    descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorWrite.descriptorCount = 1;
    descriptorWrite.pImageInfo = &imageInfo;

    vkUpdateDescriptorSets(m_Device, 1, &descriptorWrite, 0, nullptr);

    return true;
}

bool TexturePool::UpdateTextureSampler(const std::string& name, SamplerType newSamplerType)
{
    auto it = m_Textures.find(name);
    if (it == m_Textures.end()) {
        return false;
    }

    TextureInfo& info = it->second;
    
    // 如果采样器类型没有变化，直接返回
    if (info.samplerType == newSamplerType) {
        return true;
    }

    // 更新采样器类�
    // 
info.samplerType = newSamplerType;

    // 更新描述符集
    if (info.descriptorSet != VK_NULL_HANDLE) {
        VkDescriptorImageInfo imageInfo = {};
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfo.imageView = info.imageView;
        imageInfo.sampler = m_SamplerPool.at(newSamplerType);

        VkWriteDescriptorSet descriptorWrite = {};
        descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.dstSet = info.descriptorSet;
        descriptorWrite.dstBinding = 0;
        descriptorWrite.dstArrayElement = 0;
        descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.pImageInfo = &imageInfo;

        vkUpdateDescriptorSets(m_Device, 1, &descriptorWrite, 0, nullptr);
    }

    return true;
}

// 选择 BasisU 转码目标格式（按设备硬件解码能力）：桌面 BC7 / 移动 ASTC / 兜底 BC3
ktx_transcode_fmt_e TexturePool::SelectTranscodeFormat() {
    VkFormatProperties props;
    vkGetPhysicalDeviceFormatProperties(m_PhysicalDevice, VK_FORMAT_BC7_UNORM_BLOCK, &props);
    if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) {
        return KTX_TTF_BC7_RGBA;   // 桌面 NVIDIA/AMD/Intel
    }
    vkGetPhysicalDeviceFormatProperties(m_PhysicalDevice, VK_FORMAT_ASTC_4x4_UNORM_BLOCK, &props);
    if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) {
        return KTX_TTF_ASTC_4x4_RGBA;   // 移动 Mali/Adreno（BC7 不支持时）
}
    return KTX_TTF_BC3_RGBA;   // 兜底（BC1/BC3 广泛支持）
}



// 加载 KTX2 压缩纹理：BasisU 超压�?�?按设备转码（BC7/ASTC）→ VkUpload（自动建 image/上传/布局 SHADER_READ_ONLY，含内嵌 mip�?// Flip KTX2 image data vertically (block-row swap) BEFORE VkUpload.
// KTX2 files store rows bottom-up (KTX orientation = rd, OpenGL convention);
// the engine's PNG path (SDL_image) is top-left, so compressed data must be
// flipped to match. All transcode targets (BC7/ASTC/BC3) are 4x4 blocks (16B),
// so vertical flip = reversing block rows per level (mip levels flipped too).
// Note: ktx create --convert-texcoord-origin top-left is a no-op on 4.4.2
// (output stays rd), so this engine-side flip is the reliable fix.
static void FlipKtx2Vertically(ktxTexture2* tex) {
    if (tex->numDimensions != 2) {
        LOGE("[TexturePool] FlipKtx2Vertically: only 2D supported (dims=%u), skip", (unsigned)tex->numDimensions);
        return;
    }
    ktx_uint8_t* data = tex->pData;
    if (!data) return;
    const ktx_uint32_t blockSize = 16; // 4x4 block: BC7 / ASTC_4x4 / BC3
    for (ktx_uint32_t l = 0; l < tex->numLevels; ++l) {
        ktx_size_t offset = 0;
        ktxTexture_GetImageOffset((ktxTexture*)tex, l, 0, 0, &offset);
        ktx_uint32_t w = tex->baseWidth >> l;   if (w < 1) w = 1;
        ktx_uint32_t h = tex->baseHeight >> l;  if (h < 1) h = 1;
        const ktx_uint32_t wAligned = (w + 3) & ~3u;
        const ktx_uint32_t hAligned = (h + 3) & ~3u;
        const ktx_uint32_t rowBlocks = wAligned / 4;
        const ktx_size_t rowBytes = (ktx_size_t)rowBlocks * blockSize;
        const ktx_uint32_t rowCount = hAligned / 4;
        ktx_uint8_t* lvl = data + offset;
        for (ktx_uint32_t r = 0; r < rowCount / 2; ++r) {
            ktx_uint8_t* a = lvl + (ktx_size_t)r * rowBytes;
            ktx_uint8_t* b = lvl + (ktx_size_t)(rowCount - 1 - r) * rowBytes;
            for (ktx_size_t i = 0; i < rowBytes; ++i) {
                ktx_uint8_t t = a[i]; a[i] = b[i]; b[i] = t;
            }
        }
    }
    LOGD("[TexturePool] FlipKtx2Vertically: %u levels flipped (KTX2 bottom-left -> top-left)", (unsigned)tex->numLevels);
}

bool TexturePool::LoadTextureKtx2(const std::string& name, const std::string& filePath, SamplerType samplerType) {
#ifdef _WIN32
    if (m_Textures.find(name) != m_Textures.end()) {
        m_Textures[name].refCount++;
        return true;
    }

    std::string fullPath;
#ifdef __ANDROID__
    fullPath = filePath;
#else
    fullPath = ProjectManager::GetInstance().ResolveAssetPath(filePath);
    if (fullPath.empty()) {
        LOGE("[TexturePool] Cannot resolve project KTX2 texture: %s", filePath.c_str());
        return false;
    }
#endif

    // 0. 动态加�?ktx.dll API
    if (!g_ktx.Init()) {
        return false;
    }

    // 1. 读 KTX2（含全部 mip 数据）
    ktxTexture2* ktxTex = nullptr;
    KTX_error_code err = g_ktx.CreateFromNamedFile(fullPath.c_str(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &ktxTex);
    if (err != KTX_SUCCESS) {
        LOGE("[TexturePool] Failed to load KTX2: %s (%s)", fullPath.c_str(), g_ktx.ErrorString(err));
        return false;
    }

    // 2a. 黑纹理检测（2026-08-15 引擎容错）——RGBA32 转码统计 mip0 平均亮度：
    // 全黑 MR 纹理（如 IDKEngine Sponza 资产）→ avgLuma≈0 → ModelRenderer 回退默认参数（防 roughness=0 全镜面）
    float avgLuma = -1.0f;
    if (g_ktx.NeedsTranscoding(ktxTex)) {
        KTX_error_code statErr = g_ktx.TranscodeBasis(ktxTex, KTX_TTF_RGBA32, 0);
        if (statErr == KTX_SUCCESS) {
            unsigned char* data = ktxTex->pData;   // ktxTexture2 继承 ktxTexture 的 pData（动态加载 ktx.dll，无 GetData 导出链接）
            ktx_size_t sz = ktxTex->dataSize;
            uint32_t w = ktxTex->baseWidth, h = ktxTex->baseHeight;
            double sum = 0.0; uint64_t cnt = 0;
            if (data && w > 0 && h > 0) {
                for (uint32_t y = 0; y < h; y += 16) {
                    for (uint32_t x = 0; x < w; x += 16) {
                        size_t off = ((size_t)y * w + x) * 4;
                        if (off + 3 < sz) {
                            sum += (data[off] + data[off + 1] + data[off + 2]) / 3.0 / 255.0;
                            cnt++;
                        }
                    }
                }
                if (cnt) avgLuma = (float)(sum / cnt);
            }
            // ⚠️ ktx 不允许转码后再转码（Operation not allowed）——销毁后重新加载原始数据，下方 2 正常转 BC7
            g_ktx.Destroy(ktxTex);
            err = g_ktx.CreateFromNamedFile(fullPath.c_str(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &ktxTex);
            if (err != KTX_SUCCESS) {
                LOGE("[TexturePool] KTX2 reload (after stat) failed: %s", g_ktx.ErrorString(err));
                return false;
            }
        }
    }

    // 2. BasisU 超压�?�?设备硬件格式转码
    if (g_ktx.NeedsTranscoding(ktxTex)) {
        err = g_ktx.TranscodeBasis(ktxTex, SelectTranscodeFormat(), 0);
        if (err != KTX_SUCCESS) {
            LOGE("[TexturePool] KTX2 transcode failed: %s", g_ktx.ErrorString(err));
            g_ktx.Destroy(ktxTex);
            return false;
        }
    }

    // 3. VkUpload 前翻转 Y（KTX2 bottom-left -> top-left，定义见文件级 FlipKtx2Vertically）
    FlipKtx2Vertically(ktxTex);

    // 3. VkUpload：创建压缩格�?image + 上传（含内嵌 mip�? 布局 �?SHADER_READ_ONLY
    ktxVulkanDeviceInfo vdi;
    g_ktx.VkDeviceInfo_Construct(&vdi, m_PhysicalDevice, m_Device, m_Queue, m_CommandPool, m_Allocator);
    ktxVulkanTexture vkTex;
    err = g_ktx.VkUpload(ktxTex, &vdi, &vkTex);
    g_ktx.VkDeviceInfo_Destruct(&vdi);
    if (err != KTX_SUCCESS) {
        LOGE("[TexturePool] KTX2 VkUpload failed: %s", g_ktx.ErrorString(err));
        g_ktx.Destroy(ktxTex);
        return false;
    }

    TextureInfo info;
    info.image = vkTex.image;
    info.imageMemory = vkTex.deviceMemory;
    info.width = ktxTex->baseWidth;
    info.height = ktxTex->baseHeight;
    info.mipLevels = vkTex.levelCount;   // 内嵌 mip（离线生成，质量优于运行�
    // blit�?
info.format = vkTex.imageFormat;     // 压缩格式（BC7/ASTC/...�
    // 
info.isCubemap = (ktxTex->isCubemap == KTX_TRUE);
    info.samplerType = samplerType;
    info.refCount = 1;
    info.avgLuma = avgLuma;   // 2026-08-15：黑纹理检测（-1=未统计/非 ktx2）

    // 4. 创建 image view（覆盖全�
    // mip�?
if (!CreateImageView(info.image, info.format, VK_IMAGE_ASPECT_COLOR_BIT, info.isCubemap, info.mipLevels, info.imageView)) {
        g_ktx.Destroy(ktxTex);
        return false;
    }

    g_ktx.Destroy(ktxTex);

    // 5b. 运行时 mipmap 生成：KTX2 离线无 mip 时自动生成（blit 逐级缩小）
    if (info.mipLevels <= 1 && info.width > 1 && info.height > 1
        && info.format != VK_FORMAT_BC7_SRGB_BLOCK && info.format != VK_FORMAT_BC7_UNORM_BLOCK
        && info.format != VK_FORMAT_BC7_SRGB_BLOCK && info.format != VK_FORMAT_ASTC_4x4_SRGB_BLOCK
        && info.format != VK_FORMAT_ASTC_4x4_UNORM_BLOCK) {
        uint32_t mipCount = 1 + static_cast<uint32_t>(std::floor(std::log2(static_cast<double>(std::max(info.width, info.height)))));
        // 重建 image（mipCount 级）+ 复制 level0 + blit 逐级
        VkImage newImage; VkDeviceMemory newMem;
        CreateTextureImage(info.width, info.height, info.format, mipCount, newImage, newMem);
        TransitionImageLayout(newImage, info.format, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        // copy level0
        VkImageCopy copyRegion = {};
        copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.srcSubresource.mipLevel = 0;
        copyRegion.srcSubresource.baseArrayLayer = 0;
        copyRegion.srcSubresource.layerCount = 1;
        copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.dstSubresource.mipLevel = 0;
        copyRegion.dstSubresource.baseArrayLayer = 0;
        copyRegion.dstSubresource.layerCount = 1;
        copyRegion.extent = {info.width, info.height, 1};
        VkCommandBuffer cmd;
        VkCommandBufferAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, m_CommandPool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        vkAllocateCommandBuffers(m_Device, &allocInfo, &cmd);
        VkCommandBufferBeginInfo beginInfo = {};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cmd, &beginInfo);
        vkCmdCopyImage(cmd, info.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, newImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);
        // blit mips
        for (uint32_t mip = 1; mip < mipCount; mip++) {
            VkImageMemoryBarrier barrier = {};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.image = newImage;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.baseMipLevel = mip - 1;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.baseArrayLayer = 0;
            barrier.subresourceRange.layerCount = 1;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
            uint32_t mw = std::max(1u, info.width >> mip), mh = std::max(1u, info.height >> mip);
            uint32_t pw = std::max(1u, info.width >> (mip - 1)), ph = std::max(1u, info.height >> (mip - 1));
            VkImageBlit blit = {};
            blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.srcSubresource.mipLevel = mip - 1;
            blit.srcSubresource.baseArrayLayer = 0;
            blit.srcSubresource.layerCount = 1;
            blit.srcOffsets[0] = {0, 0, 0};
            blit.srcOffsets[1] = {(int32_t)pw, (int32_t)ph, 1};
            blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.dstSubresource.mipLevel = mip;
            blit.dstSubresource.baseArrayLayer = 0;
            blit.dstSubresource.layerCount = 1;
            blit.dstOffsets[0] = {0, 0, 0};
            blit.dstOffsets[1] = {(int32_t)mw, (int32_t)mh, 1};
            vkCmdBlitImage(cmd, newImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, newImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
            // barrier for next mip as src
            barrier.subresourceRange.baseMipLevel = mip;
            barrier.subresourceRange.levelCount = 1;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        }
        vkEndCommandBuffer(cmd);
        VkSubmitInfo submitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmd;
        vkQueueSubmit(m_Queue, 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(m_Queue);
        vkFreeCommandBuffers(m_Device, m_CommandPool, 1, &cmd);
        // 替换旧 image
        vkDestroyImageView(m_Device, info.imageView, m_Allocator);
        vkDestroyImage(m_Device, info.image, m_Allocator);
        vkFreeMemory(m_Device, info.imageMemory, m_Allocator);
        info.image = newImage;
        info.imageMemory = newMem;
        info.mipLevels = mipCount;
        CreateImageView(info.image, info.format, VK_IMAGE_ASPECT_COLOR_BIT, false, info.mipLevels, info.imageView);
        TransitionImageLayout(info.image, info.format, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        LOGD("[TexturePool] Runtime mipmap generated: %s (%ux%u, %u mips)", name.c_str(), info.width, info.height, info.mipLevels);
    }

    // 5. descriptor set（复用现有材�
    // layout/绑定�?
if (!CreateDescriptorSetLayout(info, info.descriptorSetLayout)) {
        return false;
    }
    if (!CreateDescriptorSet(info, info.descriptorSetLayout, info.descriptorSet)) {
        return false;
    }

    m_Textures[name] = info;
    LOGD("[TexturePool] Loaded KTX2 texture: %s (%ux%u, %u mips, fmt=%d)", name.c_str(), info.width, info.height, info.mipLevels, (int)info.format);
    return true;
#else
    LOGE("[TexturePool] KTX2 texture loading not supported on this platform: %s", name.c_str());
    return false;
#endif
}


