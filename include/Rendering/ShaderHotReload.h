#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <string>
#include <unordered_map>
#include <vector>

// 后台任务每 0.5 秒检查着色器文件，并在 GLSL 变化时编译 SPIR-V。
// 在渲染线程的 GPU 安全点调用 Poll()，只应用已经完成的更新。
// 约定：热更新只允许改 shader 内部计算逻辑，不得改动 UBO/采样器/push constant 等接口布局。
class ShaderHotReload {
public:
    static ShaderHotReload& GetInstance();

    void Poll();
    void Shutdown();

private:
    ShaderHotReload() = default;

    using FileState = std::pair<uint64_t, uint64_t>;

    struct CompiledShader {
        std::filesystem::path staged;
        std::filesystem::path output;
        std::filesystem::path backup;
        std::string key;
        FileState state;
    };

    struct ScanResult {
        bool spvChanged = false;
        std::filesystem::path transactionDir;
        std::vector<CompiledShader> compiled;
    };

    ScanResult ScanAndCompile(const std::string& glslDir, const std::string& spvDir);
    bool ScanDirectoryForChanges(const std::string& dir);
    void CompileAllSources(const std::string& glslDir, const std::string& spvDir, ScanResult& result);
    bool PublishCompiledShaders(ScanResult& result);
    std::string FindCompiler();
    void LogCompilerMissing();

    // path -> (last_write_time_ms, size)
    std::unordered_map<std::string, FileState> m_FileStates;
    std::chrono::steady_clock::time_point m_LastPoll{};
    std::string m_Compiler;             // 缓存已找到的编译器路径（空 = 未找/未找到）
    bool m_CompilerSearched = false;
    bool m_CompilerMissingLogged = false;
    std::atomic<bool> m_StopRequested{false};
    std::filesystem::path m_CompletedTransaction;
    std::future<ScanResult> m_PendingUpdate;
};
