#pragma once
#include "Platform/Export.h"
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>
#include <chrono>

// 通用资产热重载服务（主线程使用，需在 GPU 空闲安全点调用）。
// 每帧 Poll()，内部限频（默认 1.0s，DCC 保存粒度远低于此）：
//   1. 递归扫描项目资源区（ProjectManager::GetAssetsDir()）中纹理/模型扩展名文件的 mtime+size；
//   2. 首见文件只记基线（启动/切换项目前已存在的改动不触发，避免每次启动都重载）；
//   3. 变化文件按扩展名路由给渲染侧注册的 handler（SceneRenderer::Init 注册）：
//        纹理 -> TexturePool::ReloadTextureByPath（模型材质描述符重写 + 编辑器预览缓存失效）；
//        模型 -> ModelLoader CPU 缓存重载 + ModelRenderer 销毁（下一帧懒重建生效）；
//   4. handler 内部失败保留旧资源（回滚语义），本服务只记日志。
// 已知不覆盖：TilemapSystem/Renderer2D 的独立纹理描述符缓存、VoxRenderer(.vox)、
// 天空盒 HDR cubemap（固定名加载，不走 name==path 约定）。
// 契约：Poll() 必须在上一帧 fence 已等待、本帧命令未录制的安全点调用
// （与 ShaderHotReload::Poll 相同位置），销毁旧 VkImage 才无 in-flight 风险。
class MIKAN_API AssetHotReload {
public:
    static AssetHotReload& GetInstance();

    // 纹理/模型重载 handler：参数为变化文件的项目内绝对路径（fs 规范化）。
    using ReloadHandler = std::function<void(const std::string& resolvedPath)>;

    void SetTextureReloadHandler(ReloadHandler handler) { m_TextureHandler = std::move(handler); }
    void SetModelReloadHandler(ReloadHandler handler) { m_ModelHandler = std::move(handler); }

    // 手动触发（编辑器 F6）：只置请求标志，真正的扫描/重载由下一次 Poll()
    // 在 GPU 空闲安全点执行——热键处理处于 ImGui 帧内，不是安全点，
    // 直接销毁 VkImage 会造成 in-flight 销毁（参见退出 DEVICE_LOST 事故）。
    void RequestManualRescan();

    void Poll();

private:
    AssetHotReload() = default;

    // 扫描 assetsDir 下受支持扩展名的文件，与基线比对；返回发生变化的绝对路径列表。
    // 首见文件记基线不触发。扫描失败（目录不存在/无项目）返回空且不改动基线。
    std::vector<std::string> ScanForChanges(const std::string& assetsDir);

    static bool IsTextureExtension(const std::string& ext);
    static bool IsModelExtension(const std::string& ext);
    void Dispatch(const std::string& resolvedPath);

    // 绝对路径 -> (last_write_time_ms, size)
    std::unordered_map<std::string, std::pair<uint64_t, uint64_t>> m_FileStates;
    std::chrono::steady_clock::time_point m_LastPoll{};
    bool m_ManualRequest = false;   // F6 手动重扫请求（主线程内置位，Poll 消费）
    ReloadHandler m_TextureHandler;
    ReloadHandler m_ModelHandler;
};
