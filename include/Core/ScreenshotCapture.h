#pragma once

// ScreenshotCapture.h - 最终 Swapchain 画面导出
//
// 这是面向 AI Native 工作流的轻量级视觉检查入口：
// 1. 在最终 UI 叠加完成后把当前 Swapchain image 拷贝到 staging buffer；
// 2. GPU 完成提交后输出 PNG；
// 3. 同时输出 JSON 元数据和基础像素统计，供外部 AI/Agent 做画面断言。

#include "Platform/Export.h"
#include <vulkan/vulkan.h>
#include <cstdint>
#include <string>

namespace Core {

class MIKAN_API ScreenshotCapture {
public:
    static ScreenshotCapture& GetInstance();

    // 配置一次性自动截图。frame 使用主循环的一基帧编号；0 表示不自动截图。
    void Configure(int frame, const std::string& outputPath);

    // 请求下一帧截图。未传路径时输出到 <project>/out/ai-inspection/。
    void Request(const std::string& outputPath = {});

    // Swapchain 创建/重建后由 VulkanManager 通知能力，避免在不支持
    // VK_IMAGE_USAGE_TRANSFER_SRC_BIT 的设备上录入非法 copy 命令。
    void SetSwapchainTransferSupported(bool supported);

    // 在最终画面仍处于当前 command buffer 时录入 copy。返回 false 只代表本次
    // 没有录入或准备失败，不影响正常渲染；自动化结果可通过 WasCaptured/HasError 查询。
    bool RecordSwapchainImage(VkCommandBuffer commandBuffer,
                              VkImage image,
                              VkFormat format,
                              uint32_t width,
                              uint32_t height,
                              uint64_t frame);

    // 在提交完成后调用。函数只在确有待处理截图时等待队列并写文件。
    bool Finalize();

    // 启动场景、玩法模块和首帧渲染路径准备完成后由 EngineMain 标记。
    // 该状态会写入截图旁的 JSON，供 Agent 区分“有像素”与“运行时已就绪”。
    void SetSceneReady(bool ready, const std::string& status = {});
    bool IsSceneReady() const { return m_sceneReady; }

    bool WasCaptured() const { return m_captured; }
    bool HasError() const { return m_error; }
    const std::string& GetLastPath() const { return m_lastPath; }

private:
    ScreenshotCapture() = default;
    ~ScreenshotCapture() = default;
    ScreenshotCapture(const ScreenshotCapture&) = delete;
    ScreenshotCapture& operator=(const ScreenshotCapture&) = delete;

    bool ShouldCapture(uint64_t frame) const;
    bool CreateStagingBuffer(VkDeviceSize size);
    void ReleaseStagingBuffer();
    bool SaveStagingBuffer();
    std::string ResolveOutputPath(uint64_t frame) const;
    std::string ResolveMetadataPath(const std::string& imagePath) const;

    int m_targetFrame = 0;
    std::string m_outputPath;
    bool m_manualRequest = false;
    bool m_swapchainTransferSupported = false;
    bool m_recorded = false;
    bool m_captured = false;
    bool m_error = false;
    bool m_sceneReady = false;
    std::string m_readinessStatus = "starting";

    VkBuffer m_stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_stagingMemory = VK_NULL_HANDLE;
    VkDeviceSize m_stagingSize = 0;
    bool m_stagingHostCoherent = false;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    VkFormat m_format = VK_FORMAT_UNDEFINED;
    uint64_t m_recordedFrame = 0;
    std::string m_recordedPath;
    std::string m_lastPath;
};

} // namespace Core
