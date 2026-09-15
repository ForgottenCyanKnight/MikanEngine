// AutosaveService.cpp - 编辑器场景定时快照（实现）
// 见 AutosaveService.h 的设计说明：快照只写 <引擎根>/out/autosave/，
// 与项目场景文件（project.json 的 scene 字段）完全隔离。
#include "Core/AutosaveService.h"

#include "Core/Log.h"
#include "Core/ProjectManager.h"
#include "Core/Utf8Path.h"
#include "SceneSerializer.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

namespace fs = std::filesystem;

// 与 ProjectManager/SceneSerializer 相同的原子写：先落临时文件再整体替换，
// 断电/崩溃不会留下半截 JSON 覆盖掉上一份可用快照。
bool ReplaceFileWithContents(const fs::path& destination, const std::string& contents) {
    if (destination.empty()) return false;

    fs::path temporary = destination;
    temporary += ".tmp." + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());

    const auto removeTemporary = [&]() {
        std::error_code cleanupError;
        fs::remove(temporary, cleanupError);
    };

    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output.is_open()) return false;
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        output.flush();
        const bool written = output.good();
        output.close();
        if (!written) {
            removeTemporary();
            return false;
        }
    }

#ifdef _WIN32
    // MoveFileEx 就地替换：目标不存在时创建，存在时原子覆盖。
    if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        removeTemporary();
        return false;
    }
    return true;
#else
    std::error_code renameError;
    fs::rename(temporary, destination, renameError);
    if (renameError) {
        removeTemporary();
        return false;
    }
    return true;
#endif
}

// 取路径最后一段（忽略尾部斜杠），用于项目未在清单里写 name 时兜底。
std::string LastPathComponent(const std::string& path) {
    std::string trimmed = path;
    while (!trimmed.empty() && (trimmed.back() == '/' || trimmed.back() == '\\')) {
        trimmed.pop_back();
    }
    const size_t separator = trimmed.find_last_of("/\\");
    return separator == std::string::npos ? trimmed : trimmed.substr(separator + 1);
}

// 只清掉文件系统不允许的字符，保留中文等 Unicode 字符：引擎路径全程 UTF-8
// （见 Utf8Path.h），中文项目名可以安全用作文件名。若在这里做 ASCII 净化，
// 中文名会被整段清空，导致所有中文名项目退化成同一个快照文件而互相覆盖。
std::string SanitizeFileComponent(const std::string& value) {
    constexpr const char* kIllegal = "<>:\"/\\|?*";
    std::string sanitized;
    sanitized.reserve(value.size());
    for (const char c : value) {
        // 逐字节判定安全：UTF-8 续字节都 >= 0x80，不会与 ASCII 非法字符相撞。
        const unsigned char byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || std::strchr(kIllegal, c) != nullptr) {
            sanitized.push_back('_');
        } else {
            sanitized.push_back(c);
        }
    }
    // Windows 上首尾的空格与点会让文件名不可用
    while (!sanitized.empty() && (sanitized.front() == ' ' || sanitized.front() == '.')) {
        sanitized.erase(0, 1);
    }
    while (!sanitized.empty() && (sanitized.back() == ' ' || sanitized.back() == '.')) {
        sanitized.pop_back();
    }
    return sanitized;
}

} // namespace

AutosaveService& AutosaveService::GetInstance() {
    static AutosaveService instance;
    return instance;
}

void AutosaveService::SetIntervalSeconds(double seconds) {
    m_intervalSeconds = seconds < 5.0 ? 5.0 : seconds;
}

double AutosaveService::GetSecondsUntilNextSnapshot() const {
    const double remaining = m_intervalSeconds - m_elapsedSeconds;
    return remaining > 0.0 ? remaining : 0.0;
}

bool AutosaveService::Tick(double deltaSeconds, bool editable) {
    if (!editable) {
        // 离开可编辑状态就清零：反过来也保证"刚停止游戏"不会立刻写快照，
        // 而要重新攒满一个完整间隔。
        m_elapsedSeconds = 0.0;
        return false;
    }
    if (deltaSeconds <= 0.0) return false;

    m_elapsedSeconds += deltaSeconds;
    if (m_elapsedSeconds < m_intervalSeconds) return false;
    m_elapsedSeconds = 0.0;

    return !WriteSnapshot(false).empty();
}

std::string AutosaveService::WriteSnapshotNow() {
    return WriteSnapshot(true);
}

std::string AutosaveService::WriteSnapshot(bool force) {
    // 快照目录固定挂在引擎根下：/out/ 已被 .gitignore 忽略，且不在任何项目
    // 目录内，因此不会污染项目场景文件，也不会让 git status 变脏。
    const std::string engineRoot = ProjectManager::GetInstance().GetEngineRoot();
    if (engineRoot.empty()) {
        LOGW("[Autosave] 引擎根不可用，跳过本次快照");
        return {};
    }
    const fs::path snapshotDir = Utf8Path(engineRoot) / "out" / "autosave";
    {
        std::error_code ec;
        fs::create_directories(snapshotDir, ec);
        if (!fs::is_directory(snapshotDir)) {
            LOGE("[Autosave] 无法创建快照目录: %s", Utf8String(snapshotDir).c_str());
            return {};
        }
    }

    ECS::SceneSerializer serializer;
    const std::string contents = serializer.SerializeScene();
    if (contents.empty()) {
        LOGW("[Autosave] 场景序列化为空，跳过本次快照");
        return {};
    }

    const std::size_t hash = std::hash<std::string>{}(contents);
    if (!force && m_hasLastSceneHash && hash == m_lastSceneHash) {
        LOGD("[Autosave] 场景自上次快照后无变化，跳过写盘");
        return {};
    }

    // 快照文件名优先取项目目录名：它在 projects/ 下唯一，中文名照常保留，
    // 因此不同项目各有各的快照，不会互相覆盖。清单显示名仅作兜底。
    const auto& projectManager = ProjectManager::GetInstance();
    std::string baseName =
        SanitizeFileComponent(LastPathComponent(projectManager.GetProjectRoot()));
    if (baseName.empty() && projectManager.HasManifest()) {
        baseName = SanitizeFileComponent(projectManager.GetManifest().name);
    }
    if (baseName.empty()) baseName = "scene";

    RotateGenerations(Utf8String(snapshotDir), baseName);
    const fs::path target = snapshotDir / Utf8Path(baseName + ".autosave.json");
    if (!ReplaceFileWithContents(target, contents)) {
        LOGE("[Autosave] 快照写入失败: %s", Utf8String(target).c_str());
        return {};
    }

    m_lastSceneHash = hash;
    m_hasLastSceneHash = true;
    m_lastSnapshotPath = Utf8String(target);
    LOGI("[Autosave] 场景快照已写出: %s (%zu 字节)",
         m_lastSnapshotPath.c_str(), contents.size());
    return m_lastSnapshotPath;
}

void AutosaveService::RotateGenerations(const std::string& snapshotDir,
                                        const std::string& baseName) {
    const fs::path directory = Utf8Path(snapshotDir);
    const auto slotPath = [&](int index) {
        std::string name = baseName + ".autosave";
        if (index > 0) name += "." + std::to_string(index);
        name += ".json";
        return directory / Utf8Path(name);
    };

    std::error_code ec;
    // 最旧一代直接丢弃，其余整体后移一位，最后写出 slot 0。
    fs::remove(slotPath(kGenerationCount - 1), ec);
    for (int index = kGenerationCount - 1; index >= 1; --index) {
        const fs::path from = slotPath(index - 1);
        ec.clear();
        if (!fs::exists(from, ec)) continue;
        ec.clear();
        fs::rename(from, slotPath(index), ec);
    }
}
