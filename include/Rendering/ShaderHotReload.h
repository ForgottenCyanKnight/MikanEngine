#pragma once
#include <string>
#include <unordered_map>
#include <chrono>

// Shader 热更新服务（主线程使用）。
// 每帧调用 Poll()，内部限频（默认 0.5s）：
//   1. 扫描 engine/shaders/glsl/（.vert/.frag/.comp/.h）与 engine/shaders/spv/（.spv）的 mtime+size；
//   2. glsl 源变化 -> 若找到 glslangValidator/glslc 则自动重编（找不到打一次日志提示，等待外部编译）；
//   3. spv 变化（无论自动还是外部编译）-> 调用 VulkanPipeline::ReloadAllPipelines() 重建全部已登记管线；
//   4. 编译或重建失败时保留旧管线（回滚），画面不黑。
// 约定：热更新只允许改 shader 内部计算逻辑，不得改动 UBO/采样器/push constant 等接口布局。
class ShaderHotReload {
public:
    static ShaderHotReload& GetInstance();

    void Poll();

private:
    ShaderHotReload() = default;

    bool ScanDirectoryForChanges(const std::string& dir);
    bool CompileAllSources(const std::string& glslDir, const std::string& spvDir);
    std::string FindCompiler();
    void LogCompilerMissing();

    // path -> (last_write_time_ms, size)
    std::unordered_map<std::string, std::pair<uint64_t, uint64_t>> m_FileStates;
    std::chrono::steady_clock::time_point m_LastPoll{};
    std::string m_Compiler;             // 缓存已找到的编译器路径（空 = 未找/未找到）
    bool m_CompilerSearched = false;
    bool m_CompilerMissingLogged = false;
};
