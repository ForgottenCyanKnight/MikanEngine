#include "Rendering/CloudNoise3D.h"

#include "Core/EngineConfig.h"
#include "Core/Log.h"
#include "Core/VulkanContext.h"

#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_surface.h>
#include <SDL3_image/SDL_image.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <string>

namespace {

CloudNoise3D g_CloudNoise3D;

constexpr VkFormat kCloudFormat = VK_FORMAT_R8G8B8A8_UNORM;

// SDL_image on the Android build does not include a TGA decoder.  Keep the
// original Nubis TGA slices for the desktop path, but load the PNG copies on
// Android so the same CloudVolume pipeline can initialize on both platforms.
#ifdef __ANDROID__
constexpr const char* kCloudSliceExtension = ".png";
#else
constexpr const char* kCloudSliceExtension = ".tga";
#endif

std::string EngineCloudTexturePath(const std::string& relativePath)
{
    return EngineConfig::GetEngineTexturePath(relativePath.c_str());
}

} // namespace

CloudNoise3D& GetCloudNoise3D()
{
    return g_CloudNoise3D;
}

uint32_t CloudNoise3D::FindMemoryType(uint32_t typeFilter,
                                      VkMemoryPropertyFlags properties) const
{
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(m_PhysicalDevice, &memoryProperties);
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
        if ((typeFilter & (1u << i)) &&
            (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return UINT32_MAX;
}

bool CloudNoise3D::Initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                              VkAllocationCallbacks* allocator)
{
    if (IsInitialized() && m_Device == device && m_PhysicalDevice == physicalDevice) {
        return true;
    }
    Cleanup();

    if (device == VK_NULL_HANDLE || physicalDevice == VK_NULL_HANDLE ||
        g_CommandPool == VK_NULL_HANDLE || g_Queue == VK_NULL_HANDLE) {
        LOGE("[CloudNoise3D] invalid Vulkan context for Nubis texture upload");
        return false;
    }

    m_Device = device;
    m_PhysicalDevice = physicalDevice;
    m_Allocator = allocator;

    if (!LoadBaseTexture() || !LoadDetailTexture() || !LoadCurlTexture() ||
        !LoadHighTexture() || !LoadHighMapTexture() ||
        !CreateSampler()) {
        LOGE("[CloudNoise3D] failed to load Nubis/Meteoros cloud resources");
        Cleanup();
        return false;
    }

    LOGI("[CloudNoise3D] loaded Nubis resources: base=%ux%ux%u detail=%ux%ux%u curl=%ux%u high2d=%ux%u highMap=%ux%u",
         m_Base.width, m_Base.height, m_Base.depth,
         m_Detail.width, m_Detail.height, m_Detail.depth,
         m_Curl.width, m_Curl.height, m_High.width, m_High.height,
         m_HighMap.width, m_HighMap.height);
    return true;
}

bool CloudNoise3D::IsInitialized() const
{
    return m_Device != VK_NULL_HANDLE &&
           m_Sampler != VK_NULL_HANDLE &&
           m_Base.image != VK_NULL_HANDLE && m_Base.view != VK_NULL_HANDLE &&
           m_Detail.image != VK_NULL_HANDLE && m_Detail.view != VK_NULL_HANDLE &&
           m_Curl.image != VK_NULL_HANDLE && m_Curl.view != VK_NULL_HANDLE &&
           m_High.image != VK_NULL_HANDLE && m_High.view != VK_NULL_HANDLE &&
           m_HighMap.image != VK_NULL_HANDLE && m_HighMap.view != VK_NULL_HANDLE;
}

bool CloudNoise3D::LoadSurfaceRgba(const char* path, uint32_t expectedWidth,
                                   uint32_t expectedHeight,
                                   std::vector<uint8_t>& pixels)
{
    if (path == nullptr) return false;

    SDL_Surface* surface = nullptr;
#ifdef __ANDROID__
    SDL_IOStream* io = SDL_IOFromFile(path, "rb");
    if (io == nullptr) {
        LOGE("[CloudNoise3D] failed to open texture: %s", path);
        return false;
    }
    surface = IMG_Load_IO(io, 1);
#else
    surface = IMG_Load(path);
#endif
    if (surface == nullptr) {
        LOGE("[CloudNoise3D] failed to decode texture: %s (%s)", path, SDL_GetError());
        return false;
    }

    if (static_cast<uint32_t>(surface->w) != expectedWidth ||
        static_cast<uint32_t>(surface->h) != expectedHeight) {
        LOGE("[CloudNoise3D] unexpected texture size: %s (%dx%d, expected %ux%u)",
             path, surface->w, surface->h, expectedWidth, expectedHeight);
        SDL_DestroySurface(surface);
        return false;
    }

    SDL_Surface* converted = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA8888);
    SDL_DestroySurface(surface);
    if (converted == nullptr) {
        LOGE("[CloudNoise3D] failed to convert texture: %s (%s)", path, SDL_GetError());
        return false;
    }

    // SDL_PIXELFORMAT_RGBA8888 在小端机器上的内存顺序是 A/B/G/R；Vulkan
    // 的 R8G8B8A8_UNORM 需要实际字节顺序 R/G/B/A。这里故意不翻转 Y，
    // 与 Meteoros 的 stbi_load + memcpy 切片上传顺序完全一致。
    pixels.resize(static_cast<size_t>(expectedWidth) * expectedHeight * 4u);
    const auto* src = static_cast<const uint8_t*>(converted->pixels);
    for (uint32_t y = 0; y < expectedHeight; ++y) {
        for (uint32_t x = 0; x < expectedWidth; ++x) {
            const size_t srcIndex = static_cast<size_t>(y) * converted->pitch +
                                    static_cast<size_t>(x) * 4u;
            const size_t dstIndex = (static_cast<size_t>(y) * expectedWidth + x) * 4u;
            const uint8_t a = src[srcIndex + 0];
            const uint8_t b = src[srcIndex + 1];
            const uint8_t g = src[srcIndex + 2];
            const uint8_t r = src[srcIndex + 3];
            pixels[dstIndex + 0] = r;
            pixels[dstIndex + 1] = g;
            pixels[dstIndex + 2] = b;
            pixels[dstIndex + 3] = a;
        }
    }
    SDL_DestroySurface(converted);
    return true;
}

bool CloudNoise3D::LoadRawRgba(const char* path, uint32_t expectedWidth,
                               uint32_t expectedHeight,
                               std::vector<uint8_t>& pixels)
{
    if (path == nullptr) return false;

    const size_t expectedSize = static_cast<size_t>(expectedWidth) *
                                static_cast<size_t>(expectedHeight) * 4u;
#ifdef __ANDROID__
    // APK assets are exposed through SDL IO rather than the desktop file
    // system.  Keep the raw RGBA layout unchanged; this is the same path used
    // by the PNG loader above when running from an APK.
    SDL_IOStream* io = SDL_IOFromFile(path, "rb");
    if (io == nullptr) {
        LOGE("[CloudNoise3D] failed to open raw texture: %s", path);
        return false;
    }
    const Sint64 fileSize = SDL_GetIOSize(io);
    if (fileSize < 0 || static_cast<size_t>(fileSize) != expectedSize) {
        LOGE("[CloudNoise3D] unexpected raw texture size: %s (%lld, expected %zu)",
             path, static_cast<long long>(fileSize), expectedSize);
        SDL_CloseIO(io);
        return false;
    }
    pixels.resize(expectedSize);
    const size_t readSize = SDL_ReadIO(io, pixels.data(), expectedSize);
    SDL_CloseIO(io);
    if (readSize != expectedSize) {
        LOGE("[CloudNoise3D] failed to read raw texture: %s", path);
        pixels.clear();
        return false;
    }
    return true;
#else
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        LOGE("[CloudNoise3D] failed to open raw texture: %s", path);
        return false;
    }
    const std::streampos end = file.tellg();
    if (end < 0 || static_cast<size_t>(end) != expectedSize) {
        LOGE("[CloudNoise3D] unexpected raw texture size: %s (%lld, expected %zu)",
             path, static_cast<long long>(end), expectedSize);
        return false;
    }
    pixels.resize(expectedSize);
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(pixels.data()),
              static_cast<std::streamsize>(pixels.size()));
    if (!file) {
        LOGE("[CloudNoise3D] failed to read raw texture: %s", path);
        pixels.clear();
        return false;
    }
    return true;
#endif
}

bool CloudNoise3D::LoadBaseTexture()
{
    const uint32_t size = kBaseResolution;
    const size_t sliceSize = static_cast<size_t>(size) * size * 4u;
    std::vector<uint8_t> pixels(sliceSize * size);

    for (uint32_t z = 0; z < size; ++z) {
        const std::string path = EngineCloudTexturePath(
            "cloud_nubis/LowFrequency/LowFrequency(" +
            std::to_string(z + 1u) + ")" + kCloudSliceExtension);
        std::vector<uint8_t> slice;
        if (!LoadSurfaceRgba(path.c_str(), size, size, slice)) return false;
        std::memcpy(pixels.data() + static_cast<size_t>(z) * sliceSize,
                    slice.data(), sliceSize);
    }

    if (!CreateImageResource(m_Base, size, size, size) ||
        !CreateImageView(m_Base) || !UploadImage(m_Base, pixels)) {
        return false;
    }
    return true;
}

bool CloudNoise3D::LoadDetailTexture()
{
    const uint32_t size = kDetailResolution;
    const size_t sliceSize = static_cast<size_t>(size) * size * 4u;
    std::vector<uint8_t> pixels(sliceSize * size);

    for (uint32_t z = 0; z < size; ++z) {
        const std::string path = EngineCloudTexturePath(
            "cloud_nubis/HighFrequency/HighFrequency(" +
            std::to_string(z + 1u) + ")" + kCloudSliceExtension);
        std::vector<uint8_t> slice;
        if (!LoadSurfaceRgba(path.c_str(), size, size, slice)) return false;
        std::memcpy(pixels.data() + static_cast<size_t>(z) * sliceSize,
                    slice.data(), sliceSize);
    }

    if (!CreateImageResource(m_Detail, size, size, size) ||
        !CreateImageView(m_Detail) || !UploadImage(m_Detail, pixels)) {
        return false;
    }
    return true;
}

bool CloudNoise3D::LoadCurlTexture()
{
    const std::string path = EngineCloudTexturePath("cloud_nubis/curlNoise.png");
    std::vector<uint8_t> pixels;
    if (!LoadSurfaceRgba(path.c_str(), kCurlResolution, kCurlResolution, pixels)) {
        return false;
    }

    if (!CreateImageResource(m_Curl, kCurlResolution, kCurlResolution, 1) ||
        !CreateImageView(m_Curl) || !UploadImage(m_Curl, pixels)) {
        return false;
    }
    return true;
}

bool CloudNoise3D::LoadHighTexture()
{
    // Nubis 的高层云不是程序化随机 tile，而是专门制作的三通道云景图：
    // R=cirrus，G=cirrostratus，B=cirrocumulus。
    const std::string path = EngineCloudTexturePath("cloud_nubis/CirrusLutRev.png");
    std::vector<uint8_t> pixels;
    if (!LoadSurfaceRgba(path.c_str(), kHighResolution, kHighResolution, pixels)) {
        return false;
    }

    if (!CreateImageResource(m_High, kHighResolution, kHighResolution, 1) ||
        !CreateImageView(m_High) || !UploadImage(m_High, pixels)) {
        return false;
    }
    return true;
}

bool CloudNoise3D::LoadHighMapTexture()
{
    // Revelation/Nubis 风格高层覆盖图是 RGBA8 原始纹理：
    // RGB 为 Ci/Cs/Cc 覆盖，A 为共享覆盖率。
    const std::string path = EngineCloudTexturePath("cloud_nubis/CloudMapHigh.bin");
    std::vector<uint8_t> pixels;
    if (!LoadRawRgba(path.c_str(), kHighMapResolution, kHighMapResolution, pixels)) {
        return false;
    }

    if (!CreateImageResource(m_HighMap, kHighMapResolution, kHighMapResolution, 1) ||
        !CreateImageView(m_HighMap) || !UploadImage(m_HighMap, pixels)) {
        return false;
    }
    return true;
}

bool CloudNoise3D::CreateImageResource(ImageResource& resource, uint32_t width,
                                       uint32_t height, uint32_t depth)
{
    resource.width = width;
    resource.height = height;
    resource.depth = depth;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = depth > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    imageInfo.format = kCloudFormat;
    imageInfo.extent = { width, height, depth };
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkResult result = vkCreateImage(m_Device, &imageInfo, m_Allocator, &resource.image);
    if (result != VK_SUCCESS) {
        LOGE("[CloudNoise3D] vkCreateImage failed: %d", static_cast<int>(result));
        return false;
    }

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(m_Device, resource.image, &requirements);
    const uint32_t memoryType = FindMemoryType(
        requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (memoryType == UINT32_MAX) {
        LOGE("[CloudNoise3D] no device-local memory for cloud image");
        DestroyImageResource(resource);
        return false;
    }

    VkMemoryAllocateInfo allocationInfo{};
    allocationInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocationInfo.allocationSize = requirements.size;
    allocationInfo.memoryTypeIndex = memoryType;
    result = vkAllocateMemory(m_Device, &allocationInfo, m_Allocator, &resource.memory);
    if (result != VK_SUCCESS) {
        LOGE("[CloudNoise3D] vkAllocateMemory failed: %d", static_cast<int>(result));
        DestroyImageResource(resource);
        return false;
    }

    result = vkBindImageMemory(m_Device, resource.image, resource.memory, 0);
    if (result != VK_SUCCESS) {
        LOGE("[CloudNoise3D] vkBindImageMemory failed: %d", static_cast<int>(result));
        DestroyImageResource(resource);
        return false;
    }
    return true;
}

bool CloudNoise3D::CreateImageView(ImageResource& resource)
{
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = resource.image;
    viewInfo.viewType = resource.depth > 1 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = kCloudFormat;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    const VkResult result = vkCreateImageView(
        m_Device, &viewInfo, m_Allocator, &resource.view);
    if (result != VK_SUCCESS) {
        LOGE("[CloudNoise3D] vkCreateImageView failed: %d", static_cast<int>(result));
        DestroyImageResource(resource);
        return false;
    }
    return true;
}

bool CloudNoise3D::UploadImage(const ImageResource& resource,
                               const std::vector<uint8_t>& pixels)
{
    const VkDeviceSize imageSize = pixels.size();
    const VkDeviceSize expectedSize = static_cast<VkDeviceSize>(resource.width) *
                                      resource.height * resource.depth * 4u;
    if (imageSize != expectedSize || imageSize == 0) {
        LOGE("[CloudNoise3D] invalid upload size: got=%llu expected=%llu",
             static_cast<unsigned long long>(imageSize),
             static_cast<unsigned long long>(expectedSize));
        return false;
    }

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = imageSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    VkResult result = vkCreateBuffer(m_Device, &bufferInfo, m_Allocator, &staging);
    if (result != VK_SUCCESS) {
        LOGE("[CloudNoise3D] staging buffer creation failed: %d", static_cast<int>(result));
        return false;
    }

    VkMemoryRequirements bufferRequirements{};
    vkGetBufferMemoryRequirements(m_Device, staging, &bufferRequirements);
    const uint32_t memoryType = FindMemoryType(
        bufferRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (memoryType == UINT32_MAX) {
        LOGE("[CloudNoise3D] no host-visible memory for staging buffer");
        vkDestroyBuffer(m_Device, staging, m_Allocator);
        return false;
    }

    VkMemoryAllocateInfo allocationInfo{};
    allocationInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocationInfo.allocationSize = bufferRequirements.size;
    allocationInfo.memoryTypeIndex = memoryType;
    result = vkAllocateMemory(m_Device, &allocationInfo, m_Allocator, &stagingMemory);
    if (result != VK_SUCCESS) {
        LOGE("[CloudNoise3D] staging memory allocation failed: %d", static_cast<int>(result));
        vkDestroyBuffer(m_Device, staging, m_Allocator);
        return false;
    }
    vkBindBufferMemory(m_Device, staging, stagingMemory, 0);

    void* mapped = nullptr;
    result = vkMapMemory(m_Device, stagingMemory, 0, imageSize, 0, &mapped);
    if (result != VK_SUCCESS || mapped == nullptr) {
        LOGE("[CloudNoise3D] staging memory map failed: %d", static_cast<int>(result));
        vkFreeMemory(m_Device, stagingMemory, m_Allocator);
        vkDestroyBuffer(m_Device, staging, m_Allocator);
        return false;
    }
    std::memcpy(mapped, pixels.data(), static_cast<size_t>(imageSize));
    vkUnmapMemory(m_Device, stagingMemory);

    VkCommandBufferAllocateInfo commandAllocateInfo{};
    commandAllocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    commandAllocateInfo.commandPool = g_CommandPool;
    commandAllocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandAllocateInfo.commandBufferCount = 1;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    result = vkAllocateCommandBuffers(m_Device, &commandAllocateInfo, &commandBuffer);
    if (result != VK_SUCCESS) {
        LOGE("[CloudNoise3D] upload command buffer allocation failed: %d", static_cast<int>(result));
        vkFreeMemory(m_Device, stagingMemory, m_Allocator);
        vkDestroyBuffer(m_Device, staging, m_Allocator);
        return false;
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = vkBeginCommandBuffer(commandBuffer, &beginInfo);
    if (result != VK_SUCCESS) {
        LOGE("[CloudNoise3D] upload command buffer begin failed: %d", static_cast<int>(result));
        vkFreeCommandBuffers(m_Device, g_CommandPool, 1, &commandBuffer);
        vkFreeMemory(m_Device, stagingMemory, m_Allocator);
        vkDestroyBuffer(m_Device, staging, m_Allocator);
        return false;
    }

    VkImageMemoryBarrier toTransfer{};
    toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = resource.image;
    toTransfer.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    toTransfer.srcAccessMask = 0;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(commandBuffer,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toTransfer);

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = { 0, 0, 0 };
    region.imageExtent = { resource.width, resource.height, resource.depth };
    vkCmdCopyBufferToImage(commandBuffer, staging, resource.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier toRead{};
    toRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toRead.image = resource.image;
    toRead.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toRead);

    result = vkEndCommandBuffer(commandBuffer);
    if (result == VK_SUCCESS) {
        // Initialize is called while the frame command buffer is still being
        // recorded. This one-time upload uses the same queue but a separate
        // command buffer and completes before descriptors are exposed.
        vkQueueWaitIdle(g_Queue);
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;
        result = vkQueueSubmit(g_Queue, 1, &submitInfo, VK_NULL_HANDLE);
        if (result == VK_SUCCESS) result = vkQueueWaitIdle(g_Queue);
    }

    vkFreeCommandBuffers(m_Device, g_CommandPool, 1, &commandBuffer);
    vkDestroyBuffer(m_Device, staging, m_Allocator);
    vkFreeMemory(m_Device, stagingMemory, m_Allocator);
    if (result != VK_SUCCESS) {
        LOGE("[CloudNoise3D] cloud texture upload failed: %d", static_cast<int>(result));
        return false;
    }
    return true;
}

bool CloudNoise3D::CreateSampler()
{
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    // 保持原有 Nubis 形状采样：物理化只改变光照，不改变噪声寻址或形状。
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.mipLodBias = 0.0f;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;

    const VkResult result = vkCreateSampler(
        m_Device, &samplerInfo, m_Allocator, &m_Sampler);
    if (result != VK_SUCCESS) {
        LOGE("[CloudNoise3D] repeat sampler creation failed: %d", static_cast<int>(result));
        return false;
    }
    return true;
}

void CloudNoise3D::DestroyImageResource(ImageResource& resource)
{
    if (m_Device == VK_NULL_HANDLE) return;
    if (resource.view != VK_NULL_HANDLE) {
        vkDestroyImageView(m_Device, resource.view, m_Allocator);
    }
    if (resource.image != VK_NULL_HANDLE) {
        vkDestroyImage(m_Device, resource.image, m_Allocator);
    }
    if (resource.memory != VK_NULL_HANDLE) {
        vkFreeMemory(m_Device, resource.memory, m_Allocator);
    }
    resource = ImageResource{};
}

void CloudNoise3D::Cleanup()
{
    if (m_Device == VK_NULL_HANDLE) return;

    // The upload path waits for the queue, so all three images are safe to
    // destroy here even when the first-use initialization happened in Execute.
    vkDeviceWaitIdle(m_Device);
    if (m_Sampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_Device, m_Sampler, m_Allocator);
    }
    m_Sampler = VK_NULL_HANDLE;
    DestroyImageResource(m_Base);
    DestroyImageResource(m_Detail);
    DestroyImageResource(m_Curl);
    DestroyImageResource(m_High);
    DestroyImageResource(m_HighMap);
    m_Device = VK_NULL_HANDLE;
    m_PhysicalDevice = VK_NULL_HANDLE;
    m_Allocator = nullptr;
}
