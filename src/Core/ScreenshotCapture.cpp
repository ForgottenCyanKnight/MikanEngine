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
#include <array>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>

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

struct ScreenshotCapture::VideoReadbackState {
    struct Slot {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        void* mapped = nullptr;
        bool coherent = false, busy = false;
        VkDeviceSize size = 0;
        uint32_t width = 0, height = 0;
        VkFormat format = VK_FORMAT_UNDEFINED;
    };
    std::array<Slot, 3> slots;
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<size_t> pending;
    size_t recording = 0;
    bool stop = false;
    std::thread worker;
};

bool ScreenshotCapture::ConfigureVideo(const std::string& encoder, const std::string& output,
                                       int fps, int frames, int warmup, bool asynchronous)
{
#ifdef _WIN32
    if ((fps != 60 && fps != 120) || frames < 1 || warmup < 0 ||
        !std::filesystem::is_regular_file(Utf8Path(encoder)) || std::filesystem::exists(Utf8Path(output)))
        return false;
    std::error_code error;
    std::filesystem::create_directories(Utf8Path(output).parent_path(), error);
    if (error) return false;
    m_video = true;
    m_encoder = encoder;
    m_videoOutput = output;
    m_videoFps = fps;
    m_videoFrames = frames;
    m_videoWarmup = warmup;
    m_videoWritten = 0;
    m_videoCaptured = 0;
    if (asynchronous) {
        m_readback = new VideoReadbackState;
        m_readback->worker = std::thread([this] {
            auto& state = *m_readback;
            for (;;) {
                size_t index;
                {
                    std::unique_lock lock(state.mutex);
                    state.ready.wait(lock, [&] { return state.stop || !state.pending.empty(); });
                    if (state.pending.empty()) break;
                    index = state.pending.front();
                    state.pending.pop_front();
                }
                auto& slot = state.slots[index];
                bool valid = vkWaitForFences(g_Device, 1, &slot.fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
                if (valid && !slot.coherent) {
                    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
                    range.memory = slot.memory;
                    range.size = VK_WHOLE_SIZE;
                    valid = vkInvalidateMappedMemoryRanges(g_Device, 1, &range) == VK_SUCCESS;
                }
                if (!valid || (!m_error && !EncodeVideoPixels(static_cast<const uint8_t*>(slot.mapped),
                        slot.width, slot.height, slot.format))) m_error = true;
                {
                    std::lock_guard lock(state.mutex);
                    slot.busy = false;
                }
                state.ready.notify_all();
            }
        });
    }
    LOGI("[Video] readback=%s slots=%d", asynchronous ? "async" : "sync", asynchronous ? 3 : 1);
    LOGI("[Video] export %d frames at %d FPS after %d warmup frames -> %s", frames, fps, warmup, output.c_str());
    return true;
#else
    LOGE("[Video] currently supported on Windows only");
    return false;
#endif
}

bool ScreenshotCapture::FinishVideo()
{
    if (!m_video) return true;
    if (m_readback) {
        {
            std::lock_guard lock(m_readback->mutex);
            m_readback->stop = true;
        }
        m_readback->ready.notify_all();
        m_readback->worker.join();
        for (auto& slot : m_readback->slots) {
            if (slot.mapped) vkUnmapMemory(g_Device, slot.memory);
            if (slot.buffer) vkDestroyBuffer(g_Device, slot.buffer, g_Allocator);
            if (slot.memory) vkFreeMemory(g_Device, slot.memory, g_Allocator);
            if (slot.fence) vkDestroyFence(g_Device, slot.fence, g_Allocator);
        }
        delete m_readback;
        m_readback = nullptr;
    }
    bool success = !m_error && m_videoWritten == m_videoFrames;
#ifdef _WIN32
    if (m_videoPipe) CloseHandle(static_cast<HANDLE>(m_videoPipe));
    m_videoPipe = nullptr;
    if (m_videoProcess) {
        const DWORD wait = WaitForSingleObject(static_cast<HANDLE>(m_videoProcess), 60000);
        DWORD result = 1;
        if (wait == WAIT_OBJECT_0) GetExitCodeProcess(static_cast<HANDLE>(m_videoProcess), &result);
        else TerminateProcess(static_cast<HANDLE>(m_videoProcess), 1);
        success = success && result == 0;
        CloseHandle(static_cast<HANDLE>(m_videoProcess));
        m_videoProcess = nullptr;
    } else success = false;
#endif
    ReleaseStagingBuffer();
    LOGI("[Video] %s: %d/%d frames -> %s", success ? "complete" : "FAILED",
        m_videoWritten.load(), m_videoFrames, m_videoOutput.c_str());
    m_video = false;
    return success;
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
    if (m_video) return !m_recorded && !m_error && frame > static_cast<uint64_t>(m_videoWarmup)
        && m_videoCaptured < m_videoFrames;
    if (m_recorded || m_captured) return false;
    if (m_manualRequest) return true;
    return m_targetFrame > 0 && frame == static_cast<uint64_t>(m_targetFrame);
}

bool ScreenshotCapture::CreateStagingBuffer(VkDeviceSize size)
{
    if (m_stagingBuffer != VK_NULL_HANDLE && m_stagingSize == size) return true;
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
    if (!(m_readback ? PrepareVideoSlot(size) : CreateStagingBuffer(size))) {
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
    const VkBuffer captureBuffer = m_readback ? m_readback->slots[m_readback->recording].buffer : m_stagingBuffer;
    vkCmdCopyImageToBuffer(commandBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           captureBuffer, 1, &region);

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
    if (m_readback) {
        auto& slot = m_readback->slots[m_readback->recording];
        slot.width = width;
        slot.height = height;
        slot.format = format;
    }
    if (!m_video) LOGI("[Screenshot] recorded frame %llu (%ux%u, %s) -> %s",
         static_cast<unsigned long long>(frame), width, height, FormatName(format),
         m_recordedPath.c_str());
    return true;
}

bool ScreenshotCapture::EncodeVideoPixels(const uint8_t* pixels, uint32_t width, uint32_t height, VkFormat format)
{

#ifdef _WIN32
        if (!m_videoProcess) {
            SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
            HANDLE input = nullptr, output = nullptr;
            if (!CreatePipe(&input, &output, &security, 0)) {
                return false;
            }
            SetHandleInformation(output, HANDLE_FLAG_INHERIT, 0);
            auto quote = [](std::wstring value) {
                // Windows argv quoting, without a shell or command expansion.
                std::wstring result = L"\"";
                size_t slashes = 0;
                for (wchar_t c : value) {
                    if (c == L'\\') { ++slashes; continue; }
                    result.append(slashes * (c == L'\"' ? 2 : 1), L'\\');
                    slashes = 0;
                    if (c == L'\"') result += L'\\';
                    result += c;
                }
                result.append(slashes * 2, L'\\');
                return result + L"\"";
            };
            const auto encoder = Utf8Path(m_encoder).wstring();
            std::wstring command = quote(encoder) + L" -hide_banner -loglevel warning -nostdin -n"
                L" -f rawvideo -pixel_format " + std::wstring(IsBgra(format) ? L"bgra" : L"rgba") +
                L" -video_size " + std::to_wstring(width) + L"x" + std::to_wstring(height) +
                L" -framerate " + std::to_wstring(m_videoFps) + L" -i pipe:0 -an -c:v libx264"
                L" -preset veryfast -crf 18 -threads 2 -pix_fmt yuv420p -movflags +faststart " +
                quote(Utf8Path(m_videoOutput).wstring());
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            startup.dwFlags = STARTF_USESTDHANDLES;
            startup.hStdInput = input;
            startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
            startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
            PROCESS_INFORMATION process{};
            const BOOL started = CreateProcessW(encoder.c_str(), command.data(), nullptr, nullptr,
                TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
            CloseHandle(input);
            if (!started) {
                CloseHandle(output);
                LOGE("[Video] cannot start encoder (%lu)", GetLastError());
                return false;
            }
            CloseHandle(process.hThread);
            m_videoProcess = process.hProcess;
            m_videoPipe = output;
        }
        if (width != 1920 || height != 1080) {
            LOGE("[Video] unexpected output extent %ux%u", width, height);
            return false;
        }
        size_t remaining = static_cast<size_t>(width) * height * 4;
        const uint8_t* cursor = pixels;
        while (remaining) {
            DWORD written = 0;
            if (!WriteFile(static_cast<HANDLE>(m_videoPipe), cursor,
                    static_cast<DWORD>(std::min<size_t>(remaining, 1024 * 1024)), &written, nullptr) || !written) {
                LOGE("[Video] encoder pipe failed");
                return false;
            }
            remaining -= written;
            cursor += written;
        }
        ++m_videoWritten;
        if (m_videoWritten == 1 || m_videoWritten % 60 == 0)
            LOGI("[Video] encoded %d/%d frames", m_videoWritten.load(), m_videoFrames);
        return true;
#else
        return false;
#endif
}

bool ScreenshotCapture::PrepareVideoSlot(VkDeviceSize size)
{
    auto& state = *m_readback;
    const size_t index = static_cast<size_t>(m_videoCaptured) % state.slots.size();
    auto& slot = state.slots[index];
    {
        std::unique_lock lock(state.mutex);
        state.ready.wait(lock, [&] { return !slot.busy || m_error.load(); });
        if (m_error) return false;
    }
    if (!slot.buffer) {
        if (!CreateStagingBuffer(size)) return false;
        slot.buffer = m_stagingBuffer;
        slot.memory = m_stagingMemory;
        slot.size = m_stagingSize;
        slot.coherent = m_stagingHostCoherent;
        m_stagingBuffer = VK_NULL_HANDLE;
        m_stagingMemory = VK_NULL_HANDLE;
        m_stagingSize = 0;
        VkFenceCreateInfo create{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkCreateFence(g_Device, &create, g_Allocator, &slot.fence) != VK_SUCCESS ||
            vkMapMemory(g_Device, slot.memory, 0, VK_WHOLE_SIZE, 0, &slot.mapped) != VK_SUCCESS)
            return false;
    }
    if (slot.size != size) return false;
    {
        std::lock_guard lock(state.mutex);
        slot.busy = true;
    }
    state.recording = index;
    return true;
}

void ScreenshotCapture::NotifySubmitted()
{
    if (!m_readback || !m_recorded) return;
    auto& state = *m_readback;
    auto& slot = state.slots[state.recording];
    // Ordered after the draw submission on the same queue, this fence signals
    // once its copy is complete. Only the render thread submits/resets fences.
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    const bool submitted = vkResetFences(g_Device, 1, &slot.fence) == VK_SUCCESS &&
        vkQueueSubmit(g_Queue, 1, &submit, slot.fence) == VK_SUCCESS;
    {
        std::lock_guard lock(state.mutex);
        if (submitted) state.pending.push_back(state.recording);
        else { slot.busy = false; m_error = true; }
    }
    if (submitted) ++m_videoCaptured;
    m_recorded = false;
    state.ready.notify_all();
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
    if (m_video) {
        const bool saved = EncodeVideoPixels(source, m_width, m_height, m_format);
        vkUnmapMemory(g_Device, m_stagingMemory);
        if (saved) ++m_videoCaptured;
        return saved;
    }
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
    if (m_readback) return !m_error;
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
    if (!m_video) ReleaseStagingBuffer();
    m_recorded = false;
    if (!saved) {
        m_error = true;
        return false;
    }
    m_captured = true;
    if (!m_video) LOGI("[Screenshot] saved final frame -> %s (metadata: %s)",
         m_lastPath.c_str(), ResolveMetadataPath(m_lastPath).c_str());
    return true;
}

} // namespace Core
