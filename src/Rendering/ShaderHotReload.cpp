#include "ShaderHotReload.h"
#include "Core/CpuStageTrace.h"
#include "Core/EngineConfig.h"
#include "Core/ProjectManager.h"
#include "Core/ShaderBuildPlan.h"
#include "Core/Utf8Path.h"
#include "RendererBase.h"
#include "Core/Log.h"

#include <SDL3/SDL_process.h>

#include <cstdlib>
#include <chrono>
#include <exception>
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

static void RemoveCompileTransaction(const fs::path& dir) {
    if (dir.empty()) return;
    std::error_code ec;
    fs::remove_all(dir, ec);
    if (ec) LOGW("[ShaderHotReload] failed to clean staged shaders: %s", ec.message().c_str());
}

// 递归扫描 shader 文件的 mtime+size，忽略尚未发布的编译事务。
// 首次见到某文件只记基线（启动前就存在的改动不触发，避免每次启动都重载）。
bool ShaderHotReload::ScanDirectoryForChanges(const std::string& dir) {
    std::error_code ec;
    const fs::path directory = Utf8Path(dir);
    if (!fs::is_directory(directory, ec)) {
        return false;
    }

    bool anyChanged = false;
    for (fs::recursive_directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
        const auto& entry = *it;
        const auto filename = Utf8String(entry.path().filename());
        if (filename.starts_with(".shaderhotreload-") || filename.starts_with(".compile-shaders-")) {
            it.disable_recursion_pending();
            continue;
        }
        std::error_code fileError;
        if (!entry.is_regular_file(fileError)) continue;

        const auto ext = entry.path().extension();
        if (ext != ".vert" && ext != ".frag" && ext != ".comp" &&
            ext != ".glsl" && ext != ".h" && ext != ".spv") {
            continue;
        }

        const std::string key = GenericUtf8String(entry.path());
        const fs::file_time_type ftime = entry.last_write_time(fileError);
        if (fileError) continue;
        const uint64_t mtime = (uint64_t)FileTimeToInt(ftime);
        const uint64_t size = (uint64_t)entry.file_size(fileError);
        if (fileError) continue;

        auto state = m_FileStates.find(key);
        if (state == m_FileStates.end()) {
            m_FileStates[key] = { mtime, size }; // 基线
        } else if (state->second.first != mtime || state->second.second != size) {
            state->second = { mtime, size };
            anyChanged = true;
            LOGW("[ShaderHotReload] changed: %s", key.c_str());
        }
    }
    return anyChanged;
}

// 在 PATH 与 Vulkan SDK 常见安装目录查找 glslangValidator/glslc，结果缓存。
std::string ShaderHotReload::FindCompiler() {
    if (m_CompilerSearched) return m_Compiler;
    m_CompilerSearched = true;

#ifdef _WIN32
    std::error_code ec;
    const auto bundled = Utf8Path(ProjectManager::GetInstance().GetEngineRoot()) /
        "tools/glslang/bin/glslangValidator.exe";
    if (fs::is_regular_file(bundled, ec)) {
        m_Compiler = Utf8String(bundled);
        return m_Compiler;
    }
    constexpr const char* redirect = " --version >nul 2>&1";
#else
    constexpr const char* redirect = " --version >/dev/null 2>&1";
#endif
    for (const char* name : { "glslangValidator", "glslc" }) {
        std::string probe = std::string(name) + redirect;
        if (std::system(probe.c_str()) == 0) {
            m_Compiler = name;
            return m_Compiler;
        }
    }

#ifdef _WIN32
    // Vulkan SDK 默认安装目录（C:\VulkanSDK\<version>\bin\glslangValidator.exe）
    if (fs::exists("C:/VulkanSDK", ec)) {
        for (const auto& d : fs::directory_iterator("C:/VulkanSDK", ec)) {
            if (ec || !d.is_directory(ec)) break;
            const fs::path exe = d.path() / "bin" / "glslangValidator.exe";
            if (fs::exists(exe, ec)) {
                m_Compiler = Utf8String(exe);
                return m_Compiler;
            }
        }
    }
#endif
    return m_Compiler;
}

void ShaderHotReload::LogCompilerMissing() {
    if (m_CompilerMissingLogged) return;
    m_CompilerMissingLogged = true;
    LOGW("[ShaderHotReload] GLSL source changed, but no glslangValidator/glslc found.\n"
                    "                  Run 'build.ps1 -Target CompileShaders' (or install Vulkan SDK tools) to regenerate .spv;\n"
                    "                  the engine will hot-reload once the .spv files update.");
}

// 编译清单包含基础着色器及其变体，全部成功后再在渲染安全点发布。
void ShaderHotReload::CompileAllSources(const std::string& glslDir, const std::string& spvDir,
                                       ScanResult& result) {
    const std::string compiler = FindCompiler();
    if (compiler.empty()) {
        LogCompilerMissing();
        return;
    }

    const auto entries = ShaderBuildPlan::Read(spvDir + ShaderBuildPlan::kFilename);
    if (entries.empty()) {
        LOGW("[ShaderHotReload] shader build plan missing or invalid; run CompileShaders");
        return;
    }

    const fs::path sourceDir = Utf8Path(glslDir);
    const fs::path outputDir = Utf8Path(spvDir);
    const auto token = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const fs::path transactionDir = outputDir /
        (".shaderhotreload-" + std::to_string(token));
    std::error_code ec;
    if (!fs::create_directory(transactionDir, ec)) {
        LOGE("[ShaderHotReload] cannot create compile transaction: %s", ec.message().c_str());
        return;
    }
    result.transactionDir = transactionDir;

    std::vector<CompiledShader> compiled;
    compiled.reserve(entries.size());
    for (const auto& entry : entries) {
        if (m_StopRequested.load(std::memory_order_relaxed)) return;
        const fs::path relativeOutput = Utf8Path(entry.output);
        CompiledShader commit;
        commit.staged = transactionDir / "new" / relativeOutput;
        commit.output = outputDir / relativeOutput;
        fs::create_directories(commit.staged.parent_path(), ec);
        if (!ec) fs::create_directories(commit.output.parent_path(), ec);
        if (ec) {
            LOGE("[ShaderHotReload] cannot create output directory for %s: %s",
                 entry.output.c_str(), ec.message().c_str());
            return;
        }

        const std::string sourcePath = Utf8String(sourceDir / Utf8Path(entry.source));
        const std::string stagedPath = Utf8String(commit.staged);
        std::vector<const char*> args = {compiler.c_str()};
        if (compiler == "glslc") {
            if (entry.target == "vulkan1.2") {
                args.push_back("--target-env=vulkan1.2");
            } else {
                args.insert(args.end(), {"--target-env=vulkan1.1", "--target-spv=spv1.3"});
            }
        } else {
            args.insert(args.end(), {"-V", "--target-env", entry.target.c_str()});
        }
        for (const auto& define : entry.defines) args.push_back(define.c_str());
        args.insert(args.end(), {sourcePath.c_str(), "-o", stagedPath.c_str(), nullptr});
        SDL_Process* process = SDL_CreateProcess(args.data(), false);
        int rc = -1;
        if (process) {
            if (!SDL_WaitProcess(process, true, &rc)) rc = -1;
            SDL_DestroyProcess(process);
        } else {
            LOGE("[ShaderHotReload] cannot start compiler: %s", SDL_GetError());
        }

        const auto size = fs::file_size(commit.staged, ec);
        if (rc != 0 || ec || size == 0) {
            LOGE("[ShaderHotReload] FAILED to compile: %s -> %s (exit=%d)",
                 entry.source.c_str(), entry.output.c_str(), rc);
            return;
        }
        const auto mtime = fs::last_write_time(commit.staged, ec);
        if (ec) {
            LOGE("[ShaderHotReload] cannot inspect staged SPIR-V: %s", ec.message().c_str());
            return;
        }
        commit.state = {static_cast<uint64_t>(FileTimeToInt(mtime)), size};
        commit.key = GenericUtf8String(commit.output);

        const bool hadOriginal = fs::exists(commit.output, ec);
        if (ec) {
            LOGE("[ShaderHotReload] cannot inspect existing SPIR-V: %s", ec.message().c_str());
            return;
        }
        if (hadOriginal) {
            commit.backup = transactionDir / ".old" / relativeOutput;
            fs::create_directories(commit.backup.parent_path(), ec);
            if (!ec) fs::copy_file(commit.output, commit.backup, fs::copy_options::none, ec);
            if (ec) {
                LOGE("[ShaderHotReload] cannot stage rollback copy for %s: %s",
                     commit.key.c_str(), ec.message().c_str());
                return;
            }
        }
        compiled.push_back(std::move(commit));
    }
    result.compiled = std::move(compiled);
}

bool ShaderHotReload::PublishCompiledShaders(ScanResult& result) {
    if (result.compiled.empty()) return false;

    // 渲染安全点发布已准备好的文件，避免读取中的模块与替换冲突或混用版本。
    std::error_code ec;
    std::size_t committedCount = 0;
    for (const auto& commit : result.compiled) {
        fs::rename(commit.staged, commit.output, ec);
        if (ec) {
            LOGE("[ShaderHotReload] failed to commit %s: %s",
                    commit.key.c_str(), ec.message().c_str());
            break;
        }
        ++committedCount;
    }

    if (committedCount != result.compiled.size()) {
        bool rollbackOk = true;
        for (std::size_t i = 0; i < committedCount; ++i) {
            const auto& commit = result.compiled[i];
            std::error_code rollbackEc;
            if (!commit.backup.empty()) {
                fs::rename(commit.backup, commit.output, rollbackEc);
            } else {
                fs::remove(commit.output, rollbackEc);
            }
            if (rollbackEc) {
                LOGE("[ShaderHotReload] failed to restore %s: %s",
                     commit.key.c_str(), rollbackEc.message().c_str());
                rollbackOk = false;
            }
        }
        if (rollbackOk) {
            LOGW("[ShaderHotReload] commit rolled back; existing SPIR-V kept");
        } else {
            LOGE("[ShaderHotReload] recovery files kept in %s", Utf8String(result.transactionDir).c_str());
            result.transactionDir.clear();
        }
        return false;
    }

    for (auto& commit : result.compiled) {
        m_FileStates.insert_or_assign(std::move(commit.key), commit.state);
    }
    LOGI("[ShaderHotReload] committed %zu shader(s), ok", committedCount);
    return true;
}

ShaderHotReload::ScanResult ShaderHotReload::ScanAndCompile(const std::string& glslDir, const std::string& spvDir) {
    ScanResult result;
    bool glslChanged = false;
    {
        Core::CpuStageTrace trace("hot_reload.shader_scan");
        glslChanged = ScanDirectoryForChanges(glslDir);
        result.spvChanged = ScanDirectoryForChanges(spvDir);
    }
    if (!glslChanged || m_StopRequested.load(std::memory_order_relaxed)) {
        return result;
    }
    Core::CpuStageTrace trace("hot_reload.shader_compile");
    try {
        CompileAllSources(glslDir, spvDir, result);
    } catch (const std::exception& ex) {
        LOGW("[ShaderHotReload] background compile failed: %s", ex.what());
    }
    return result;
}

void ShaderHotReload::Poll() {
    if (m_PendingUpdate.valid()) {
        if (m_PendingUpdate.wait_for(std::chrono::seconds::zero()) != std::future_status::ready) {
            return;
        }
        bool reload = false;
        try {
            auto result = m_PendingUpdate.get();
            reload = PublishCompiledShaders(result) || result.spvChanged;
            m_CompletedTransaction = std::move(result.transactionDir);
        } catch (const std::exception& ex) {
            LOGW("[ShaderHotReload] background scan failed: %s", ex.what());
        }
        if (reload) {
            const uint32_t reloaded = VulkanPipeline::ReloadAllPipelines();
            LOGW("[ShaderHotReload] shader changed -> reloaded %u pipeline(s)", reloaded);
        }
    }

    const auto now = std::chrono::steady_clock::now();
    if (now - m_LastPoll < std::chrono::milliseconds(500)) return;
    m_LastPoll = now;

    auto& project = ProjectManager::GetInstance();
    try {
        m_PendingUpdate = std::async(std::launch::async,
            [this, glslDir = project.GetEngineAssetPath(EngineConfig::SHADERS_GLSL_PATH),
                   spvDir = project.GetEngineAssetPath(EngineConfig::SHADERS_SPV_PATH),
                   completed = m_CompletedTransaction] {
                RemoveCompileTransaction(completed);
                return ScanAndCompile(glslDir, spvDir);
            });
        m_CompletedTransaction.clear();
    } catch (const std::exception& ex) {
        LOGW("[ShaderHotReload] cannot start background scan: %s", ex.what());
    }
}

void ShaderHotReload::Shutdown() {
    m_StopRequested.store(true, std::memory_order_relaxed);
    if (m_PendingUpdate.valid()) {
        try {
            RemoveCompileTransaction(m_PendingUpdate.get().transactionDir);
        } catch (const std::exception& ex) {
            LOGW("[ShaderHotReload] background scan failed: %s", ex.what());
        }
    }
    RemoveCompileTransaction(m_CompletedTransaction);
    m_CompletedTransaction.clear();
    m_FileStates.clear();
    m_LastPoll = {};
    m_StopRequested.store(false, std::memory_order_relaxed);
}
