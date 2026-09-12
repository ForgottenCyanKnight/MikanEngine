#ifndef VK_ENABLE_BETA_EXTENSIONS
#define VK_ENABLE_BETA_EXTENSIONS
#endif

#include "Core/VulkanPostProcessHistory.h"
#include "Core/VulkanContext.h"
#include "Core/VulkanManager.h"
#include "Core/Log.h"

#include <algorithm>
#include <cstring>

VkImage g_SceneTAAHistory = VK_NULL_HANDLE;
VkDeviceMemory g_SceneTAAHistoryMem = VK_NULL_HANDLE;
VkImageView g_SceneTAAHistoryView = VK_NULL_HANDLE;
VkImage g_GameTAAHistory = VK_NULL_HANDLE;
VkDeviceMemory g_GameTAAHistoryMem = VK_NULL_HANDLE;
VkImageView g_GameTAAHistoryView = VK_NULL_HANDLE;
VkSampler g_TAAHistorySampler = VK_NULL_HANDLE;
uint32_t g_TAAHistoryW = 0, g_TAAHistoryH = 0;
bool g_TAAHistoryNeedsClear = true;   // 创建后首帧 clear（UNDEFINED 内容 → 0，防 NaN 传染）



// ===== 时序 GTAO 历史纹理：Scene/Game 各自尺寸的半分辨率 RGBA8 =====
VkImage g_SceneAOHistory = VK_NULL_HANDLE;
VkDeviceMemory g_SceneAOHistoryMem = VK_NULL_HANDLE;
VkImageView g_SceneAOHistoryView = VK_NULL_HANDLE;
VkImage g_GameAOHistory = VK_NULL_HANDLE;
VkDeviceMemory g_GameAOHistoryMem = VK_NULL_HANDLE;
VkImageView g_GameAOHistoryView = VK_NULL_HANDLE;
VkSampler g_AOHistorySampler = VK_NULL_HANDLE;
uint32_t g_SceneAOHistoryW = 0, g_SceneAOHistoryH = 0;
uint32_t g_GameAOHistoryW = 0, g_GameAOHistoryH = 0;
bool g_SceneAOHistoryNeedsClear = true;
bool g_GameAOHistoryNeedsClear = true;
// SSGI 时间 reblur 历史（RGBA16F 半分辨率，跨帧累积——参考 gtao history 机制）
VkImage g_SceneSSGIHistory = VK_NULL_HANDLE;
VkDeviceMemory g_SceneSSGIHistoryMem = VK_NULL_HANDLE;
VkImageView g_SceneSSGIHistoryView = VK_NULL_HANDLE;
VkImage g_GameSSGIHistory = VK_NULL_HANDLE;
VkDeviceMemory g_GameSSGIHistoryMem = VK_NULL_HANDLE;
VkImageView g_GameSSGIHistoryView = VK_NULL_HANDLE;
VkSampler g_SSGIHistorySampler = VK_NULL_HANDLE;
uint32_t g_SceneSSGIHistoryW = 0, g_SceneSSGIHistoryH = 0;
uint32_t g_GameSSGIHistoryW = 0, g_GameSSGIHistoryH = 0;
bool g_SceneSSGIHistoryNeedsClear = true;
bool g_GameSSGIHistoryNeedsClear = true;
// 体积云独立的半分辨率时域历史；格式与 cloud_view 输出一致，避免和 GTAO/SSGI
// 共享历史时发生语义或生命周期冲突。
VkImage g_SceneCloudHistory = VK_NULL_HANDLE;
VkDeviceMemory g_SceneCloudHistoryMem = VK_NULL_HANDLE;
VkImageView g_SceneCloudHistoryView = VK_NULL_HANDLE;
VkImage g_GameCloudHistory = VK_NULL_HANDLE;
VkDeviceMemory g_GameCloudHistoryMem = VK_NULL_HANDLE;
VkImageView g_GameCloudHistoryView = VK_NULL_HANDLE;
VkSampler g_CloudHistorySampler = VK_NULL_HANDLE;
uint32_t g_SceneCloudHistoryW = 0, g_SceneCloudHistoryH = 0;
uint32_t g_GameCloudHistoryW = 0, g_GameCloudHistoryH = 0;
bool g_SceneCloudHistoryNeedsClear = true;
bool g_GameCloudHistoryNeedsClear = true;




static void DestroyHistoryImage(VkImage& image, VkDeviceMemory& memory, VkImageView& view)
{
    if (view != VK_NULL_HANDLE) vkDestroyImageView(g_Device, view, g_Allocator);
    if (image != VK_NULL_HANDLE) vkDestroyImage(g_Device, image, g_Allocator);
    if (memory != VK_NULL_HANDLE) vkFreeMemory(g_Device, memory, g_Allocator);
    image = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;
    view = VK_NULL_HANDLE;
}

void CleanupAOAndSSGIHistoryTextures()
{
    if (g_Device == VK_NULL_HANDLE) return;
    DestroyHistoryImage(g_SceneAOHistory, g_SceneAOHistoryMem, g_SceneAOHistoryView);
    DestroyHistoryImage(g_GameAOHistory, g_GameAOHistoryMem, g_GameAOHistoryView);
    DestroyHistoryImage(g_SceneSSGIHistory, g_SceneSSGIHistoryMem, g_SceneSSGIHistoryView);
    DestroyHistoryImage(g_GameSSGIHistory, g_GameSSGIHistoryMem, g_GameSSGIHistoryView);
    DestroyHistoryImage(g_SceneCloudHistory, g_SceneCloudHistoryMem, g_SceneCloudHistoryView);
    DestroyHistoryImage(g_GameCloudHistory, g_GameCloudHistoryMem, g_GameCloudHistoryView);
    if (g_AOHistorySampler != VK_NULL_HANDLE) vkDestroySampler(g_Device, g_AOHistorySampler, g_Allocator);
    if (g_SSGIHistorySampler != VK_NULL_HANDLE) vkDestroySampler(g_Device, g_SSGIHistorySampler, g_Allocator);
    if (g_CloudHistorySampler != VK_NULL_HANDLE) vkDestroySampler(g_Device, g_CloudHistorySampler, g_Allocator);
    g_AOHistorySampler = VK_NULL_HANDLE;
    g_SSGIHistorySampler = VK_NULL_HANDLE;
    g_CloudHistorySampler = VK_NULL_HANDLE;
    g_SceneAOHistoryW = g_SceneAOHistoryH = 0;
    g_GameAOHistoryW = g_GameAOHistoryH = 0;
    g_SceneSSGIHistoryW = g_SceneSSGIHistoryH = 0;
    g_GameSSGIHistoryW = g_GameSSGIHistoryH = 0;
    g_SceneCloudHistoryW = g_SceneCloudHistoryH = 0;
    g_GameCloudHistoryW = g_GameCloudHistoryH = 0;
    g_SceneAOHistoryNeedsClear = g_GameAOHistoryNeedsClear = true;
    g_SceneSSGIHistoryNeedsClear = g_GameSSGIHistoryNeedsClear = true;
    g_SceneCloudHistoryNeedsClear = g_GameCloudHistoryNeedsClear = true;
}

// 游戏内设置切换 pass 后，在当前 swapchain 帧 fence 已等待、命令缓冲尚未录制的
// 安全点重建链。必须重建而不是只改执行标志：末端 pass 的 final render pass
// 会随启用状态变化，且被重新启用的 pass 可能原本没有中间附件。



void EnsureAOHistoryTexture(bool sceneHistory, uint32_t w, uint32_t h)
{
    VkImage& image = sceneHistory ? g_SceneAOHistory : g_GameAOHistory;
    VkDeviceMemory& memory = sceneHistory ? g_SceneAOHistoryMem : g_GameAOHistoryMem;
    VkImageView& view = sceneHistory ? g_SceneAOHistoryView : g_GameAOHistoryView;
    uint32_t& currentW = sceneHistory ? g_SceneAOHistoryW : g_GameAOHistoryW;
    uint32_t& currentH = sceneHistory ? g_SceneAOHistoryH : g_GameAOHistoryH;
    bool& needsClear = sceneHistory ? g_SceneAOHistoryNeedsClear : g_GameAOHistoryNeedsClear;
    if (image != VK_NULL_HANDLE && currentW == w && currentH == h) return;
    DestroyHistoryImage(image, memory, view);
    currentW = w;
    currentH = h;
    VkImageCreateInfo ii = {};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;   // 2026-：随 gtao 链输出改 RGBA8（R=AO G=体积光 B=Godray），历史拷贝格式匹配
    ii.extent = { w, h, 1 };
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vkCreateImage(g_Device, &ii, g_Allocator, &image);
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(g_Device, image, &mr);
    VkMemoryAllocateInfo ai = {};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = 0;
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g_PhysicalDevice, &mp);
    for (uint32_t m = 0; m < mp.memoryTypeCount; m++) {
        if ((mr.memoryTypeBits & (1u << m)) && (mp.memoryTypes[m].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { ai.memoryTypeIndex = m; break; }
    }
    vkAllocateMemory(g_Device, &ai, g_Allocator, &memory);
    vkBindImageMemory(g_Device, image, memory, 0);
    VkImageViewCreateInfo vi = {};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCreateImageView(g_Device, &vi, g_Allocator, &view);
    needsClear = true;
    if (g_AOHistorySampler == VK_NULL_HANDLE) {
        VkSamplerCreateInfo si = {};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = VK_FILTER_LINEAR;
        si.minFilter = VK_FILTER_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vkCreateSampler(g_Device, &si, g_Allocator, &g_AOHistorySampler);
    }
}

// 帧首：新历史先从 UNDEFINED 清为 AO=1/体积光=0；已有历史从上帧 TRANSFER_DST 转为采样。
void PrepareAOHistoryForRead(VkCommandBuffer cmd, VkImage history, bool& needsClear)
{
    if (!history) return;
    if (needsClear) {
        VkImageMemoryBarrier toTransfer = {};
        toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = history;
        toTransfer.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toTransfer);
        const VkClearColorValue clear = { { 1.0f, 0.0f, 0.0f, 0.0f } };
        vkCmdClearColorImage(cmd, history, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear,
            1, &toTransfer.subresourceRange);
        needsClear = false;
    }
    VkImageMemoryBarrier b = {};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = history;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// 链后：gtao pass 输出 → 历史（下帧累积用）
void CopyAOHistory(VkCommandBuffer cmd, VkImage gtaoImg, VkImage history, uint32_t width, uint32_t height)
{
    static int s_gtaoDiag = 0;
    if (s_gtaoDiag < 3) {
        s_gtaoDiag++;
        LOGI("[GTAO-DIAG] CopyAOHistory gtaoImg=%p history=%p (w=%u h=%u)",
            (void*)gtaoImg, (void*)history, width, height);
    }
    if (!gtaoImg || !history) {
        printf("[AOHistory] SKIP gtaoImg=%p history=%p\n", (void*)gtaoImg, (void*)history);
        return;
    }
    // static int dbgCount = 0;
    // 诊断打印已注释（每帧输出 [AOHistory] copy WxH 刷屏）
    // if ((++dbgCount % 120) == 1) printf("[AOHistory] copy %ux%u\n", width, height);
    VkImageMemoryBarrier bs[2] = {};
    bs[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bs[0].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bs[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    bs[0].srcQueueFamilyIndex = bs[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bs[0].image = gtaoImg;
    bs[0].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    bs[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    bs[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    bs[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bs[1].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bs[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bs[1].srcQueueFamilyIndex = bs[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bs[1].image = history;
    bs[1].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    bs[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    bs[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, bs);
    VkImageCopy region = {};
    region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.extent = { width, height, 1 };
    vkCmdCopyImage(cmd, gtaoImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, history, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // 历史图保持 TRANSFER_DST，下一帧 PrepareAOHistoryForRead 会从该布局转为采样布局；
    // GTAO pass 输出则必须恢复为 SHADER_READ_ONLY，否则下一帧链会以错误的旧布局开始。
    VkImageMemoryBarrier tail = {};
    tail.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    tail.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    tail.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    tail.srcQueueFamilyIndex = tail.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    tail.image = gtaoImg;
    tail.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    tail.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    tail.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &tail);
}

void EnsureSSGIHistoryTexture(bool sceneHistory, uint32_t w, uint32_t h)
{
    VkImage& image = sceneHistory ? g_SceneSSGIHistory : g_GameSSGIHistory;
    VkDeviceMemory& memory = sceneHistory ? g_SceneSSGIHistoryMem : g_GameSSGIHistoryMem;
    VkImageView& view = sceneHistory ? g_SceneSSGIHistoryView : g_GameSSGIHistoryView;
    uint32_t& currentW = sceneHistory ? g_SceneSSGIHistoryW : g_GameSSGIHistoryW;
    uint32_t& currentH = sceneHistory ? g_SceneSSGIHistoryH : g_GameSSGIHistoryH;
    bool& needsClear = sceneHistory ? g_SceneSSGIHistoryNeedsClear : g_GameSSGIHistoryNeedsClear;
    if (image != VK_NULL_HANDLE && currentW == w && currentH == h) return;
    DestroyHistoryImage(image, memory, view);
    currentW = w;
    currentH = h;
    VkImageCreateInfo ii = {};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R16G16B16A16_SFLOAT;   // SSGI 历史：间接光 HDR 线性，保精度防溢色
    ii.extent = { w, h, 1 };
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vkCreateImage(g_Device, &ii, g_Allocator, &image);
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(g_Device, image, &mr);
    VkMemoryAllocateInfo ai = {};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = 0;
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g_PhysicalDevice, &mp);
    for (uint32_t m = 0; m < mp.memoryTypeCount; m++) {
        if ((mr.memoryTypeBits & (1u << m)) && (mp.memoryTypes[m].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { ai.memoryTypeIndex = m; break; }
    }
    vkAllocateMemory(g_Device, &ai, g_Allocator, &memory);
    vkBindImageMemory(g_Device, image, memory, 0);
    VkImageViewCreateInfo vi = {};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCreateImageView(g_Device, &vi, g_Allocator, &view);
    needsClear = true;
    if (g_SSGIHistorySampler == VK_NULL_HANDLE) {
        VkSamplerCreateInfo si = {};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = VK_FILTER_LINEAR;
        si.minFilter = VK_FILTER_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vkCreateSampler(g_Device, &si, g_Allocator, &g_SSGIHistorySampler);
    }
}

void PrepareSSGIHistoryForRead(VkCommandBuffer cmd, VkImage history, bool& needsClear)
{
    if (!history) return;
    if (needsClear) {
        VkImageMemoryBarrier toTransfer = {};
        toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = history;
        toTransfer.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toTransfer);
        const VkClearColorValue clear = {};
        vkCmdClearColorImage(cmd, history, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear,
            1, &toTransfer.subresourceRange);
        needsClear = false;
    }
    VkImageMemoryBarrier b = {};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = history;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// 链后：ssgi pass 输出 → 历史（下帧累积用）
void CopySSGIHistory(VkCommandBuffer cmd, VkImage ssgiImg, VkImage history, uint32_t width, uint32_t height)
{
    if (!ssgiImg || !history) return;
    VkImageMemoryBarrier bs[2] = {};
    bs[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bs[0].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bs[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    bs[0].srcQueueFamilyIndex = bs[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bs[0].image = ssgiImg;
    bs[0].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    bs[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    bs[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    bs[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bs[1].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bs[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bs[1].srcQueueFamilyIndex = bs[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bs[1].image = history;
    bs[1].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    bs[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    bs[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, bs);
    VkImageCopy region = {};
    region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.extent = { width, height, 1 };
    vkCmdCopyImage(cmd, ssgiImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, history, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

void EnsureCloudHistoryTexture(bool sceneHistory, uint32_t w, uint32_t h)
{
    if (w == 0 || h == 0) return;
    VkImage& image = sceneHistory ? g_SceneCloudHistory : g_GameCloudHistory;
    VkDeviceMemory& memory = sceneHistory ? g_SceneCloudHistoryMem : g_GameCloudHistoryMem;
    VkImageView& view = sceneHistory ? g_SceneCloudHistoryView : g_GameCloudHistoryView;
    uint32_t& currentW = sceneHistory ? g_SceneCloudHistoryW : g_GameCloudHistoryW;
    uint32_t& currentH = sceneHistory ? g_SceneCloudHistoryH : g_GameCloudHistoryH;
    bool& needsClear = sceneHistory ? g_SceneCloudHistoryNeedsClear : g_GameCloudHistoryNeedsClear;
    if (image != VK_NULL_HANDLE && currentW == w && currentH == h) return;

    DestroyHistoryImage(image, memory, view);
    currentW = w;
    currentH = h;

    VkImageCreateInfo ii = {};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    ii.extent = { w, h, 1 };
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vkCreateImage(g_Device, &ii, g_Allocator, &image);

    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(g_Device, image, &mr);
    VkMemoryAllocateInfo ai = {};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = 0;
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g_PhysicalDevice, &mp);
    for (uint32_t m = 0; m < mp.memoryTypeCount; ++m) {
        if ((mr.memoryTypeBits & (1u << m)) &&
            (mp.memoryTypes[m].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            ai.memoryTypeIndex = m;
            break;
        }
    }
    vkAllocateMemory(g_Device, &ai, g_Allocator, &memory);
    vkBindImageMemory(g_Device, image, memory, 0);

    VkImageViewCreateInfo vi = {};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCreateImageView(g_Device, &vi, g_Allocator, &view);

    needsClear = true;
    if (g_CloudHistorySampler == VK_NULL_HANDLE) {
        VkSamplerCreateInfo si = {};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = VK_FILTER_LINEAR;
        si.minFilter = VK_FILTER_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vkCreateSampler(g_Device, &si, g_Allocator, &g_CloudHistorySampler);
    }
}

void PrepareCloudHistoryForRead(VkCommandBuffer cmd, VkImage history, bool& needsClear)
{
    if (history == VK_NULL_HANDLE) return;
    if (needsClear) {
        VkImageMemoryBarrier toTransfer = {};
        toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = history;
        toTransfer.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &toTransfer);
        // RGB=0、A=1 表示“当前没有历史云”，正好是 cloud_view 的 clear 值。
        const VkClearColorValue clear = { { 0.0f, 0.0f, 0.0f, 1.0f } };
        vkCmdClearColorImage(cmd, history, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &clear, 1, &toTransfer.subresourceRange);
        needsClear = false;
    }

    VkImageMemoryBarrier b = {};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = history;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);
}

void CopyCloudHistory(VkCommandBuffer cmd, VkImage cloudImg, VkImage history,
                             uint32_t width, uint32_t height)
{
    static bool s_loggedCloudHistoryCopy = false;
    if (!s_loggedCloudHistoryCopy) {
        s_loggedCloudHistoryCopy = true;
        LOGI("[CloudTemporal] history copy source=%p history=%p extent=%ux%u",
             (void*)cloudImg, (void*)history, width, height);
    }
    if (cloudImg == VK_NULL_HANDLE || history == VK_NULL_HANDLE || width == 0 || height == 0) return;

    VkImageMemoryBarrier bs[2] = {};
    bs[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bs[0].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bs[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    bs[0].srcQueueFamilyIndex = bs[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bs[0].image = cloudImg;
    bs[0].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    bs[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    bs[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    bs[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bs[1].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bs[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bs[1].srcQueueFamilyIndex = bs[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bs[1].image = history;
    bs[1].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    bs[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    bs[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 2, bs);

    VkImageCopy region = {};
    region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.extent = { width, height, 1 };
    vkCmdCopyImage(cmd, cloudImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   history, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // cloud_view 是后续帧的输入，也可能是本帧其它 pass 的输入，恢复其布局。
    VkImageMemoryBarrier tail = {};
    tail.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    tail.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    tail.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    tail.srcQueueFamilyIndex = tail.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    tail.image = cloudImg;
    tail.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    tail.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    tail.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &tail);
}

void CopyTAAHistory(VkCommandBuffer cmd, VkImage taaImg, VkImage history);   // 前向声明（PrepareTAAHistoryForRead 先使用）
void EnsureTAAHistoryTexture(uint32_t w, uint32_t h)
{
    if (g_SceneTAAHistory != VK_NULL_HANDLE && g_TAAHistoryW == w && g_TAAHistoryH == h) return;
    if (g_SceneTAAHistory != VK_NULL_HANDLE) {
        vkDestroyImageView(g_Device, g_SceneTAAHistoryView, g_Allocator);
        vkDestroyImage(g_Device, g_SceneTAAHistory, g_Allocator);
        vkFreeMemory(g_Device, g_SceneTAAHistoryMem, g_Allocator);
        vkDestroyImageView(g_Device, g_GameTAAHistoryView, g_Allocator);
        vkDestroyImage(g_Device, g_GameTAAHistory, g_Allocator);
        vkFreeMemory(g_Device, g_GameTAAHistoryMem, g_Allocator);
        g_SceneTAAHistory = g_GameTAAHistory = VK_NULL_HANDLE;
    }
    g_TAAHistoryW = w; g_TAAHistoryH = h;
    g_TAAHistoryNeedsClear = true;
    VkImageCreateInfo ii = {};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    ii.extent = { w, h, 1 };
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    for (int i = 0; i < 2; i++) {
        VkImage* img = i == 0 ? &g_SceneTAAHistory : &g_GameTAAHistory;
        VkDeviceMemory* mem = i == 0 ? &g_SceneTAAHistoryMem : &g_GameTAAHistoryMem;
        VkImageView* view = i == 0 ? &g_SceneTAAHistoryView : &g_GameTAAHistoryView;
        vkCreateImage(g_Device, &ii, g_Allocator, img);
        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(g_Device, *img, &mr);
        VkMemoryAllocateInfo ai = {};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = 0;
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(g_PhysicalDevice, &mp);
        for (uint32_t m = 0; m < mp.memoryTypeCount; m++) {
            if ((mr.memoryTypeBits & (1u << m)) && (mp.memoryTypes[m].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { ai.memoryTypeIndex = m; break; }
        }
        vkAllocateMemory(g_Device, &ai, g_Allocator, mem);
        vkBindImageMemory(g_Device, *img, *mem, 0);
        VkImageViewCreateInfo vi = {};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = *img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCreateImageView(g_Device, &vi, g_Allocator, view);
    }
    if (g_TAAHistorySampler == VK_NULL_HANDLE) {
        VkSamplerCreateInfo si = {};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = VK_FILTER_LINEAR;
        si.minFilter = VK_FILTER_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vkCreateSampler(g_Device, &si, g_Allocator, &g_TAAHistorySampler);
    }
}

// 帧首：上帧 taa 输出 → 历史（同 command buffer 串行——3 帧 in-flight 下帧 N 的 barrier
// 会等待帧 N-1 的写入完成（同 queue 提交有序），消除帧末 copy 的跨帧竞态）；创建后首帧额外 clear
void PrepareTAAHistoryForRead(VkCommandBuffer cmd, VkImage history, VkImage prevTaaOutput)
{
    if (!history) return;
    if (g_TAAHistoryNeedsClear) {
        // 首帧：UNDEFINED → clear → SHADER_READ_ONLY（无上帧输出）
        VkImageMemoryBarrier b = {};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        // vkCmdClearColorImage 要求目标已经处于 TRANSFER_DST_OPTIMAL；
        // 这里若直接标成 SHADER_READ_ONLY，会让清除操作与实际 layout 不一致，
        // 首帧历史内容在移动 GPU 上可能变成未定义数据。
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = history;
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.srcAccessMask = 0;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        VkClearColorValue cc = {};
        VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdClearColorImage(cmd, history, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &rng);
        g_TAAHistoryNeedsClear = false;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    } else {
        // 常规：上帧 taa 输出 → 历史（CopyTAAHistory 自带 barrier + 尾部转回 SHADER_READ_ONLY）
        CopyTAAHistory(cmd, prevTaaOutput, history);
    }
}

// 链后：taa pass 输出 → 历史（下帧累积用）
void CopyTAAHistory(VkCommandBuffer cmd, VkImage taaImg, VkImage history)
{
    // [TAA-DIAG] 首 3 次打印链路状态；使用 LOGI 以便 Android logcat 能看到句柄是否有效。
    static int s_taaDiag = 0;
    if (s_taaDiag < 3) {
        s_taaDiag++;
        LOGI("[TAA-DIAG] CopyTAAHistory taaImg=%p history=%p (w=%u h=%u needsClear=%d)",
            (void*)taaImg, (void*)history, g_TAAHistoryW, g_TAAHistoryH, g_TAAHistoryNeedsClear ? 1 : 0);
    }
    if (!taaImg || !history) return;
    VkImageMemoryBarrier bs[2] = {};
    bs[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bs[0].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bs[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    bs[0].srcQueueFamilyIndex = bs[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bs[0].image = taaImg;
    bs[0].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    bs[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    bs[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    bs[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bs[1].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bs[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bs[1].srcQueueFamilyIndex = bs[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bs[1].image = history;
    bs[1].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    bs[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    bs[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, bs);
    VkImageCopy region = {};
    region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.extent = { g_TAAHistoryW, g_TAAHistoryH, 1 };
    vkCmdCopyImage(cmd, taaImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, history, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    // 两张图都恢复为 SHADER_READ_ONLY：history 供下一帧 TAA 采样，taaImg
    // 作为链中的中间附件在下一帧仍会被同一个 pass 复用，不能残留 TRANSFER_SRC 布局。
    VkImageMemoryBarrier tail[2] = {};
    tail[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    tail[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    tail[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    tail[0].srcQueueFamilyIndex = tail[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    tail[0].image = taaImg;
    tail[0].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    tail[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    tail[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    tail[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    tail[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    tail[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    tail[1].srcQueueFamilyIndex = tail[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    tail[1].image = history;
    tail[1].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    tail[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    tail[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 2, tail);
}


