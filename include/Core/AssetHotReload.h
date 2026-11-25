#pragma once
#include "Platform/Export.h"
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>
#include <chrono>
#include <cstdint>
#include <future>

// 通用资产热重载服务（主线程使用，需在 GPU 空闲安全点调用）。
// 每帧 Poll()，内部限频（默认 1.0s，DCC 保存粒度远低于此）：
//   1. 在后台递归扫描项目资源区和链接资产中纹理/模型文件的 mtime+size；
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

    // 手动触发（编辑器 F6）：当前扫描结束后启动一次新的后台扫描，
    // 结果由 Poll() 在 GPU 安全点应用。
    void RequestManualRescan();

    void Poll();
    // 编辑器退出时等待后台扫描结束并丢弃结果。
    void Shutdown();

private:
    AssetHotReload() = default;

    struct ChangedAsset {
        std::string path;
        std::string linkedRoot;
    };

    // 仅在后台执行。首见文件记基线，不触发重载。
    void ScanForChanges(const std::string& root, bool linkedAsset,
                        std::vector<ChangedAsset>& changed);

    static bool IsTextureExtension(const std::string& ext);
    static bool IsModelExtension(const std::string& ext);
    void Dispatch(const std::string& resolvedPath);

    // 由后台扫描串行更新。绝对路径 -> (last_write_time_ms, size)
    std::unordered_map<std::string, std::pair<uint64_t, uint64_t>> m_FileStates;
    std::string m_ScanProjectRoot;
    std::string m_ScanAssetsDir;
    std::chrono::steady_clock::time_point m_NextPoll{};
    bool m_ManualRequest = false;
    ReloadHandler m_TextureHandler;
    ReloadHandler m_ModelHandler;
    // 先销毁 future 等待后台扫描，再销毁文件基线。
    std::future<std::vector<ChangedAsset>> m_Scan;
};
