// ScreenshotCapture.cpp - 最终 Swapchain 画面导出与像素诊断

#include "Core/ScreenshotCapture.h"
#include "Core/Utf8Path.h"

#include "Core/ProjectManager.h"
#include "Core/VulkanContext.h"
#include "Core/Log.h"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <vector>

namespace Core {

namespace {

uint32_t FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags required,
                        VkMemoryPropertyFlags* selectedFlags = nullptr)
{
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(g_PhysicalDevice, &properties);

    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) == 0) continue;
        if ((properties.memoryTypes[i].propertyFlags & required) == required) {
            if (selectedFlags) *selectedFlags = properties.memoryTypes[i].propertyFlags;
            return i;
        }
    }
    return std::numeric_limits<uint32_t>::max();
}

const char* FormatName(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_B8G8R8A8_UNORM: return "VK_FORMAT_B8G8R8A8_UNORM";
    case VK_FORMAT_B8G8R8A8_SRGB: return "VK_FORMAT_B8G8R8A8_SRGB";
    case VK_FORMAT_R8G8B8A8_UNORM: return "VK_FORMAT_R8G8B8A8_UNORM";
    case VK_FORMAT_R8G8B8A8_SRGB: return "VK_FORMAT_R8G8B8A8_SRGB";
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32: return "VK_FORMAT_A8B8G8R8_UNORM_PACK32";
    default: return "unsupported-format";
    }
}

bool IsBgra(VkFormat format)
{
    return format == VK_FORMAT_B8G8R8A8_UNORM ||
           format == VK_FORMAT_B8G8R8A8_SRGB;
}

bool IsRgba(VkFormat format)
{
    return format == VK_FORMAT_R8G8B8A8_UNORM ||
           format == VK_FORMAT_R8G8B8A8_SRGB;
}

std::string JsonEscape(const std::string& value)
{
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (const char c : value) {
        switch (c) {
        case '\\': escaped += "\\\\"; break;
        case '"': escaped += "\\\""; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default: escaped += c; break;
        }
    }
    return escaped;
}

std::string Timestamp()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    std::ostringstream stream;
    stream << std::put_time(&local, "%Y-%m-%dT%H:%M:%S");
    return stream.str();
}

} // namespace

ScreenshotCapture& ScreenshotCapture::GetInstance()
{
    static ScreenshotCapture instance;
    return instance;
}

void ScreenshotCapture::Configure(int frame, const std::string& outputPath)
{
    m_targetFrame = std::max(frame, 0);
    m_outputPath = outputPath;
    m_manualRequest = false;
    m_recorded = false;
    m_captured = false;
    m_error = false;
    m_sceneReady = false;
    m_readinessStatus = "starting";
    m_lastPath.clear();
    m_recordedPath.clear();
    ReleaseStagingBuffer();

    if (m_targetFrame > 0) {
        LOGI("[Screenshot] armed for frame %d%s%s",
             m_targetFrame,
             m_outputPath.empty() ? "" : " -> ",
             m_outputPath.empty() ? "" : m_outputPath.c_str());
    }
}

void ScreenshotCapture::Request(const std::string& outputPath)
{
    if (!outputPath.empty()) m_outputPath = outputPath;
    m_manualRequest = true;
    m_recorded = false;
    m_captured = false;
    m_error = false;
    m_lastPath.clear();
    m_recordedPath.clear();
    ReleaseStagingBuffer();
    LOGI("[Screenshot] capture requested for next rendered frame");
}

void ScreenshotCapture::SetSwapchainTransferSupported(bool supported)
{
    m_swapchainTransferSupported = supported;
    LOGI("[Screenshot] swapchain transfer-src %s",
         supported ? "supported" : "unavailable");
}

void ScreenshotCapture::SetSceneReady(bool ready, const std::string& status)
{
    m_sceneReady = ready;
    if (!status.empty()) m_readinessStatus = status;
    LOGI("[Screenshot] runtime readiness: %s (%s)",
         m_sceneReady ? "ready" : "not-ready", m_readinessStatus.c_str());
}

bool ScreenshotCapture::ShouldCapture(uint64_t frame) const
{
    if (m_recorded || m_captured) return false;
    if (m_manualRequest) return true;
    return m_targetFrame > 0 && frame == static_cast<uint64_t>(m_targetFrame);
}

bool ScreenshotCapture::CreateStagingBuffer(VkDeviceSize size)
{
    if (g_Device == VK_NULL_HANDLE || g_PhysicalDevice == VK_NULL_HANDLE || size == 0)
        return false;

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(g_Device, &bufferInfo, g_Allocator, &m_stagingBuffer) != VK_SUCCESS)
        return false;

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(g_Device, m_stagingBuffer, &requirements);

    VkMemoryPropertyFlags selectedFlags = 0;
    uint32_t memoryType = FindMemoryType(
        requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
        &selectedFlags);
    if (memoryType == std::numeric_limits<uint32_t>::max()) {
        ReleaseStagingBuffer();
        return false;
    }

    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memoryType;
    if (vkAllocateMemory(g_Device, &allocation, g_Allocator, &m_stagingMemory) != VK_SUCCESS) {
        ReleaseStagingBuffer();
        return false;
    }
    if (vkBindBufferMemory(g_Device, m_stagingBuffer, m_stagingMemory, 0) != VK_SUCCESS) {
        ReleaseStagingBuffer();
        return false;
    }

    m_stagingSize = size;
    m_stagingHostCoherent = (selectedFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    return true;
}

void ScreenshotCapture::ReleaseStagingBuffer()
{
    if (g_Device != VK_NULL_HANDLE && m_stagingBuffer != VK_NULL_HANDLE)
        vkDestroyBuffer(g_Device, m_stagingBuffer, g_Allocator);
    if (g_Device != VK_NULL_HANDLE && m_stagingMemory != VK_NULL_HANDLE)
        vkFreeMemory(g_Device, m_stagingMemory, g_Allocator);
    m_stagingBuffer = VK_NULL_HANDLE;
    m_stagingMemory = VK_NULL_HANDLE;
    m_stagingSize = 0;
    m_stagingHostCoherent = false;
}

std::string ScreenshotCapture::ResolveOutputPath(uint64_t frame) const
{
    std::filesystem::path path;
    if (!m_outputPath.empty()) {
        path = Utf8Path(m_outputPath);
        if (path.extension().empty()) path += ".png";
        if (path.is_relative()) {
            std::string projectRoot = ProjectManager::GetInstance().GetProjectRoot();
            path = Utf8Path(projectRoot.empty() ? "." : projectRoot) / path;
        }
    } else {
        std::string projectRoot = ProjectManager::GetInstance().GetProjectRoot();
        if (projectRoot.empty()) projectRoot = Utf8String(std::filesystem::current_path());
        path = Utf8Path(projectRoot) / "out" / "ai-inspection" /
               ("frame-" + std::to_string(frame) + ".png");
    }
    return Utf8String(path.lexically_normal());
}

std::string ScreenshotCapture::ResolveMetadataPath(const std::string& imagePath) const
{
    std::filesystem::path path = Utf8Path(imagePath);
    path.replace_extension(".json");
    return Utf8String(path);
}

bool ScreenshotCapture::RecordSwapchainImage(VkCommandBuffer commandBuffer,
                                             VkImage image,
                                             VkFormat format,
                                             uint32_t width,
                                             uint32_t height,
                                             uint64_t frame)
{
    if (!ShouldCapture(frame)) return false;
    if (!m_swapchainTransferSupported) {
        LOGE("[Screenshot] requested frame %llu but swapchain does not support transfer-src",
             static_cast<unsigned long long>(frame));
        m_error = true;
        m_manualRequest = false;
        if (m_targetFrame == static_cast<int>(frame)) m_targetFrame = 0;
        return false;
    }
    if (commandBuffer == VK_NULL_HANDLE || image == VK_NULL_HANDLE ||
        width == 0 || height == 0 || (!IsBgra(format) && !IsRgba(format))) {
        LOGE("[Screenshot] cannot capture frame %llu: invalid command/image/size/format (%ux%u, %s)",
             static_cast<unsigned long long>(frame), width, height, FormatName(format));
        m_error = true;
        m_manualRequest = false;
        if (m_targetFrame == static_cast<int>(frame)) m_targetFrame = 0;
        return false;
    }

    const VkDeviceSize size = static_cast<VkDeviceSize>(width) * height * 4u;
    if (!CreateStagingBuffer(size)) {
        LOGE("[Screenshot] failed to create %llu-byte staging buffer",
             static_cast<unsigned long long>(size));
        m_error = true;
        m_manualRequest = false;
        if (m_targetFrame == static_cast<int>(frame)) m_targetFrame = 0;
        return false;
    }

    VkImageMemoryBarrier toTransfer{};
    toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransfer.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toTransfer.image = image;
    toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toTransfer.subresourceRange.baseMipLevel = 0;
    toTransfer.subresourceRange.levelCount = 1;
    toTransfer.subresourceRange.baseArrayLayer = 0;
    toTransfer.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(commandBuffer,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
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
    region.imageExtent = { width, height, 1 };
    vkCmdCopyImageToBuffer(commandBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           m_stagingBuffer, 1, &region);

    VkImageMemoryBarrier toPresent = toTransfer;
    toPresent.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toPresent.dstAccessMask = 0;
    toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(commandBuffer,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toPresent);

    m_recorded = true;
    m_manualRequest = false;
    m_width = width;
    m_height = height;
    m_format = format;
    m_recordedFrame = frame;
    m_recordedPath = ResolveOutputPath(frame);
    LOGI("[Screenshot] recorded frame %llu (%ux%u, %s) -> %s",
         static_cast<unsigned long long>(frame), width, height, FormatName(format),
         m_recordedPath.c_str());
    return true;
}

bool ScreenshotCapture::SaveStagingBuffer()
{
    if (g_Device == VK_NULL_HANDLE || m_stagingMemory == VK_NULL_HANDLE ||
        m_stagingSize == 0 || m_width == 0 || m_height == 0)
        return false;

    void* mapped = nullptr;
    if (vkMapMemory(g_Device, m_stagingMemory, 0, m_stagingSize, 0, &mapped) != VK_SUCCESS)
        return false;
    if (!m_stagingHostCoherent) {
        VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = m_stagingMemory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        if (vkInvalidateMappedMemoryRanges(g_Device, 1, &range) != VK_SUCCESS) {
            vkUnmapMemory(g_Device, m_stagingMemory);
            return false;
        }
    }

    const auto* source = static_cast<const uint8_t*>(mapped);
    std::vector<uint8_t> rgba(static_cast<size_t>(m_width) * m_height * 4u);
    const bool bgra = IsBgra(m_format);
    for (uint32_t y = 0; y < m_height; ++y) {
        for (uint32_t x = 0; x < m_width; ++x) {
            const size_t index = (static_cast<size_t>(y) * m_width + x) * 4u;
            const uint8_t c0 = source[index + 0];
            const uint8_t c1 = source[index + 1];
            const uint8_t c2 = source[index + 2];
            const uint8_t c3 = source[index + 3];
            rgba[index + 0] = bgra ? c2 : c0;
            rgba[index + 1] = c1;
            rgba[index + 2] = bgra ? c0 : c2;
            rgba[index + 3] = c3;
        }
    }
    vkUnmapMemory(g_Device, m_stagingMemory);

    std::error_code ec;
    const std::filesystem::path output = Utf8Path(m_recordedPath);
    const std::filesystem::path parent = output.parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    if (ec) {
        LOGE("[Screenshot] cannot create output directory: %s", ec.message().c_str());
        return false;
    }

    SDL_Surface* surface = SDL_CreateSurface(
        static_cast<int>(m_width), static_cast<int>(m_height), SDL_PIXELFORMAT_RGBA32);
    if (!surface) {
        LOGE("[Screenshot] SDL_CreateSurface failed: %s", SDL_GetError());
        return false;
    }
    for (uint32_t y = 0; y < m_height; ++y) {
        auto* destination = static_cast<uint8_t*>(surface->pixels) +
                            static_cast<size_t>(y) * surface->pitch;
        const auto* sourceRow = rgba.data() + static_cast<size_t>(y) * m_width * 4u;
        std::copy(sourceRow, sourceRow + static_cast<size_t>(m_width) * 4u, destination);
    }

    const bool saved = IMG_SavePNG(surface, m_recordedPath.c_str());
    SDL_DestroySurface(surface);
    if (!saved) {
        LOGE("[Screenshot] IMG_SavePNG failed: %s", SDL_GetError());
        return false;
    }

    uint64_t nonBlackPixels = 0;
    uint64_t alphaPixels = 0;
    uint64_t sumRgb = 0;
    uint8_t minChannel = 255;
    uint8_t maxChannel = 0;
    for (size_t i = 0; i < rgba.size(); i += 4) {
        const uint8_t r = rgba[i + 0];
        const uint8_t g = rgba[i + 1];
        const uint8_t b = rgba[i + 2];
        const uint8_t a = rgba[i + 3];
        if (a > 0) ++alphaPixels;
        if (static_cast<uint16_t>(r) + g + b > 9 && a > 0) ++nonBlackPixels;
        sumRgb += static_cast<uint64_t>(r) + g + b;
        minChannel = std::min(minChannel, std::min(r, std::min(g, b)));
        maxChannel = std::max(maxChannel, std::max(r, std::max(g, b)));
    }
    const double pixelCount = static_cast<double>(m_width) * m_height;
    const double nonBlackRatio = pixelCount > 0.0 ? nonBlackPixels / pixelCount : 0.0;
    const bool mostlyBlack = nonBlackRatio < 0.001;
    const char* visualStatus = m_sceneReady
        ? (mostlyBlack ? "runtime-ready-but-black" : "ready")
        : (mostlyBlack ? "startup-black-screen" : "not-runtime-ready");
    const std::string metadataPath = ResolveMetadataPath(m_recordedPath);
    std::ofstream metadata(Utf8Path(metadataPath), std::ios::binary);
    bool metadataSaved = false;
    if (metadata.is_open()) {
        metadata << std::fixed << std::setprecision(6);
        metadata << "{\n"
                 << "  \"kind\": \"frame-screenshot\",\n"
                 << "  \"image\": \"" << JsonEscape(m_recordedPath) << "\",\n"
                 << "  \"frame\": " << m_recordedFrame << ",\n"
                 << "  \"width\": " << m_width << ",\n"
                 << "  \"height\": " << m_height << ",\n"
                 << "  \"format\": \"" << FormatName(m_format) << "\",\n"
                 << "  \"capturedAt\": \"" << Timestamp() << "\",\n"
                 << "  \"runtimeReady\": " << (m_sceneReady ? "true" : "false") << ",\n"
                 << "  \"readinessStatus\": \"" << JsonEscape(m_readinessStatus) << "\",\n"
                 << "  \"visualStatus\": \"" << visualStatus << "\",\n"
                 << "  \"nonBlackRatio\": " << nonBlackRatio << ",\n"
                 << "  \"alphaRatio\": " << (pixelCount > 0.0 ? alphaPixels / pixelCount : 0.0) << ",\n"
                 << "  \"meanRgb\": " << (pixelCount > 0.0 ? static_cast<double>(sumRgb) / (pixelCount * 3.0) : 0.0) << ",\n"
                 << "  \"minChannel\": " << static_cast<int>(minChannel) << ",\n"
                 << "  \"maxChannel\": " << static_cast<int>(maxChannel) << "\n"
                 << "}\n";
        metadata.close();
        metadataSaved = metadata.good();
    } else {
        LOGW("[Screenshot] image saved but metadata could not be opened: %s", metadataPath.c_str());
    }

    if (!metadataSaved) {
        LOGE("[Screenshot] image saved but required metadata could not be written: %s",
             metadataPath.c_str());
        return false;
    }

    m_lastPath = m_recordedPath;
    return true;
}

bool ScreenshotCapture::Finalize()
{
    if (!m_recorded) return !m_error;
    if (g_Device == VK_NULL_HANDLE || g_Queue == VK_NULL_HANDLE) {
        LOGE("[Screenshot] cannot finalize without Vulkan device/queue");
        m_error = true;
        ReleaseStagingBuffer();
        m_recorded = false;
        return false;
    }

    const VkResult waitResult = vkQueueWaitIdle(g_Queue);
    if (waitResult != VK_SUCCESS) {
        LOGE("[Screenshot] vkQueueWaitIdle failed: %d", static_cast<int>(waitResult));
        m_error = true;
        ReleaseStagingBuffer();
        m_recorded = false;
        return false;
    }

    const bool saved = SaveStagingBuffer();
    ReleaseStagingBuffer();
    m_recorded = false;
    if (!saved) {
        m_error = true;
        return false;
    }
    m_captured = true;
    LOGI("[Screenshot] saved final frame -> %s (metadata: %s)",
         m_lastPath.c_str(), ResolveMetadataPath(m_lastPath).c_str());
    return true;
}

} // namespace Core
