#include "ShaderHotReload.h"
#include "Core/ProjectManager.h"
#include "RendererBase.h"

#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

ShaderHotReload& ShaderHotReload::GetInstance() {
    static ShaderHotReload instance;
    return instance;
}

static int64_t FileTimeToInt(const fs::file_time_type& t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
}

// 扫描单个目录中 shader 相关文件的 mtime+size，与上次记录比较，返回是否有变化。
// 首次见到某文件只记基线（启动前就存在的改动不触发，避免每次启动都重载）。
bool ShaderHotReload::ScanDirectoryForChanges(const std::string& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return false;
    }

    bool anyChanged = false;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;

        const std::string ext = entry.path().extension().string();
        if (ext != ".vert" && ext != ".frag" && ext != ".comp" && ext != ".h" && ext != ".spv") {
            continue;
        }

        const std::string key = entry.path().string();
        const fs::file_time_type ftime = fs::last_write_time(entry.path(), ec);
        if (ec) continue;
        const uint64_t mtime = (uint64_t)FileTimeToInt(ftime);
        const uint64_t size = (uint64_t)entry.file_size(ec);

        auto it = m_FileStates.find(key);
        if (it == m_FileStates.end()) {
            m_FileStates[key] = { mtime, size }; // 基线
        } else if (it->second.first != mtime || it->second.second != size) {
            it->second = { mtime, size };
            anyChanged = true;
            fprintf(stderr, "[ShaderHotReload] changed: %s\n", key.c_str());
        }
    }
    return anyChanged;
}

// 在 PATH 与 Vulkan SDK 常见安装目录查找 glslangValidator/glslc，结果缓存。
std::string ShaderHotReload::FindCompiler() {
    if (m_CompilerSearched) return m_Compiler;
    m_CompilerSearched = true;

    // 1) PATH 探测（system 用 cmd /c，返回 0 表示命令可用）
    for (const char* name : { "glslangValidator", "glslc" }) {
        std::string probe = std::string(name) + " --version >nul 2>&1";
        if (std::system(probe.c_str()) == 0) {
            m_Compiler = name;
            return m_Compiler;
        }
    }

    // 2) Vulkan SDK 默认安装目录（C:\VulkanSDK\<version>\bin\glslangValidator.exe）
    std::error_code ec;
    if (fs::exists("C:/VulkanSDK", ec)) {
        for (const auto& d : fs::directory_iterator("C:/VulkanSDK", ec)) {
            if (ec || !d.is_directory(ec)) break;
            const fs::path exe = d.path() / "bin" / "glslangValidator.exe";
            if (fs::exists(exe, ec)) {
                m_Compiler = exe.string();
                return m_Compiler;
            }
        }
    }
    return m_Compiler;
}

void ShaderHotReload::LogCompilerMissing() {
    if (m_CompilerMissingLogged) return;
    m_CompilerMissingLogged = true;
    fprintf(stderr, "[ShaderHotReload] GLSL source changed, but no glslangValidator/glslc found.\n"
                    "                  Run 'build.ps1 -Target CompileShaders' (or install Vulkan SDK tools) to regenerate .spv;\n"
                    "                  the engine will hot-reload once the .spv files update.\n");
}

// 用 glslangValidator 全量重编 glsl 目录（.vert/.frag/.comp -> spv 目录），与 CMake 参数一致。
// 编译先写入 .shaderhotreload-* 临时目录；只有全部产物有效时才覆盖正式 .spv。
// 这样语法错误、编译器异常或单个产物写入失败都不会破坏上一版 shader。
bool ShaderHotReload::CompileAllSources(const std::string& glslDir, const std::string& spvDir) {
    const std::string compiler = FindCompiler();
    if (compiler.empty()) {
        LogCompilerMissing();
        return false;
    }

    std::error_code ec;
    if (!fs::is_directory(glslDir, ec)) return false;
    fs::create_directories(spvDir, ec);
    if (ec) {
        fprintf(stderr, "[ShaderHotReload] cannot create SPIR-V directory: %s\n", ec.message().c_str());
        return false;
    }

    std::vector<std::pair<fs::path, fs::path>> shaderOutputs;
    for (const auto& entry : fs::directory_iterator(glslDir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;

        const std::string ext = entry.path().extension().string();
        if (ext != ".vert" && ext != ".frag" && ext != ".comp") continue;

        const fs::path output = fs::path(spvDir) /
            (entry.path().stem().string() + ext + ".spv");
        shaderOutputs.emplace_back(entry.path(), output);
    }
    if (ec) {
        fprintf(stderr, "[ShaderHotReload] failed to enumerate GLSL directory: %s\n", ec.message().c_str());
        return false;
    }
    if (shaderOutputs.empty()) {
        fprintf(stderr, "[ShaderHotReload] no GLSL sources found; keeping existing SPIR-V\n");
        return false;
    }

    const auto token = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const fs::path transactionDir = fs::path(spvDir) /
        (".shaderhotreload-" + std::to_string(token));
    fs::remove_all(transactionDir, ec);
    ec.clear();
    fs::create_directories(transactionDir, ec);
    if (ec) {
        fprintf(stderr, "[ShaderHotReload] cannot create compile transaction: %s\n", ec.message().c_str());
        return false;
    }

    bool allOk = true;
    int compiledCount = 0;
    for (const auto& shaderOutput : shaderOutputs) {
        const fs::path& source = shaderOutput.first;
        const fs::path staged = transactionDir / shaderOutput.second.filename();
        const std::string cmd = "\"" + compiler + "\" -V --target-env spirv1.3 \"" + source.string() + "\" -o \"" + staged.string() + "\"";
        const int rc = std::system(cmd.c_str());
        std::error_code fileEc;
        const bool stagedValid = rc == 0 && fs::is_regular_file(staged, fileEc) &&
            fs::file_size(staged, fileEc) > 0;
        if (!stagedValid || fileEc) {
            fprintf(stderr, "[ShaderHotReload] FAILED to compile: %s (exit=%d%s)\n",
                    source.string().c_str(), rc, fileEc ? ", invalid output" : "");
            allOk = false;
        } else {
            compiledCount++;
        }
    }

    if (!allOk) {
        fprintf(stderr, "[ShaderHotReload] compile transaction aborted; existing SPIR-V kept\n");
        fs::remove_all(transactionDir, ec);
        return false;
    }

    struct CommitEntry {
        fs::path staged;
        fs::path output;
        fs::path backup;
        bool hadOriginal = false;
    };
    std::vector<CommitEntry> commits;
    commits.reserve(shaderOutputs.size());
    const fs::path backupDir = transactionDir / ".old";
    fs::create_directories(backupDir, ec);
    if (ec) {
        fprintf(stderr, "[ShaderHotReload] cannot prepare rollback directory: %s\n", ec.message().c_str());
        fs::remove_all(transactionDir, ec);
        return false;
    }

    for (const auto& shaderOutput : shaderOutputs) {
        CommitEntry commit;
        commit.staged = transactionDir / shaderOutput.second.filename();
        commit.output = shaderOutput.second;
        commit.hadOriginal = fs::is_regular_file(commit.output, ec);
        if (ec) {
            fprintf(stderr, "[ShaderHotReload] cannot inspect existing SPIR-V: %s\n", ec.message().c_str());
            fs::remove_all(transactionDir, ec);
            return false;
        }
        if (commit.hadOriginal) {
            commit.backup = backupDir / commit.output.filename();
            fs::copy_file(commit.output, commit.backup,
                          fs::copy_options::overwrite_existing, ec);
            if (ec) {
                fprintf(stderr, "[ShaderHotReload] cannot stage rollback copy for %s: %s\n",
                        commit.output.string().c_str(), ec.message().c_str());
                fs::remove_all(transactionDir, ec);
                return false;
            }
        }
        commits.push_back(std::move(commit));
    }

    bool commitOk = true;
    for (const auto& commit : commits) {
        fs::copy_file(commit.staged, commit.output,
                      fs::copy_options::overwrite_existing, ec);
        if (ec) {
            fprintf(stderr, "[ShaderHotReload] failed to commit %s: %s\n",
                    commit.output.string().c_str(), ec.message().c_str());
            commitOk = false;
            break;
        }
    }

    if (!commitOk) {
        std::error_code rollbackEc;
        for (const auto& commit : commits) {
            if (commit.hadOriginal) {
                fs::copy_file(commit.backup, commit.output,
                              fs::copy_options::overwrite_existing, rollbackEc);
            } else {
                fs::remove(commit.output, rollbackEc);
            }
            rollbackEc.clear();
        }
        fprintf(stderr, "[ShaderHotReload] commit rolled back; existing SPIR-V kept\n");
        fs::remove_all(transactionDir, ec);
        return false;
    }

    fs::remove_all(transactionDir, ec);
    printf("[ShaderHotReload] committed %d shader(s), ok\n", compiledCount);
    fflush(stdout);
    return true;
}

void ShaderHotReload::Poll() {
    const auto now = std::chrono::steady_clock::now();
    if (now - m_LastPoll < std::chrono::milliseconds(500)) {
        return;
    }
    m_LastPoll = now;

    const std::string glslDir = ProjectManager::GetInstance().GetEngineAssetPath("shaders/glsl/");
    const std::string spvDir = ProjectManager::GetInstance().GetEngineAssetPath("shaders/spv/");

    const bool glslChanged = ScanDirectoryForChanges(glslDir);
    const bool spvChanged = ScanDirectoryForChanges(spvDir);

    if (!glslChanged && !spvChanged) {
        return;
    }

    if (glslChanged) {
        // glsl 源变化：尝试自动重编。编译失败时保留旧管线（等外部编译后靠 spv 扫描再触发）。
        if (!CompileAllSources(glslDir, spvDir)) {
            return;
        }
        // 编译成功会改写 spv 文件——立即刷新 spv 基线，避免下一轮 Poll 把"刚编译的产物"误判为外部变化而重复重载。
        ScanDirectoryForChanges(spvDir);
    }

    // 编译成功（spv 已更新）或仅 spv 变化（外部编译）：重建全部已登记管线
    const uint32_t reloaded = VulkanPipeline::ReloadAllPipelines();
    fprintf(stderr, "[ShaderHotReload] shader changed -> reloaded %u pipeline(s)\n", reloaded);
    fflush(stderr);
}
