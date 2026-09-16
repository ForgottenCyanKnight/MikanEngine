#include "Core/AssetHotReload.h"
#include "Core/ProjectManager.h"
#include "Core/Log.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <utility>

namespace fs = std::filesystem;

AssetHotReload& AssetHotReload::GetInstance() {
    static AssetHotReload instance;
    return instance;
}

// 路由表与 ShaderHotReload 的扩展名策略保持一致的写法：小写比较，逐条匹配。
namespace {

const char* const kTextureExtensions[] = {
    ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".hdr", ".dds", ".ktx2",
};
const char* const kModelExtensions[] = {
    ".gltf", ".glb", ".fbx", ".obj", ".pmx",
};

std::string ToLowerAscii(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

int64_t FileTimeMs(const fs::file_time_type& t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
}

} // namespace

bool AssetHotReload::IsTextureExtension(const std::string& ext) {
    const std::string lower = ToLowerAscii(ext);
    for (const char* candidate : kTextureExtensions) {
        if (lower == candidate) return true;
    }
    return false;
}

bool AssetHotReload::IsModelExtension(const std::string& ext) {
    const std::string lower = ToLowerAscii(ext);
    for (const char* candidate : kModelExtensions) {
        if (lower == candidate) return true;
    }
    return false;
}

std::vector<std::string> AssetHotReload::ScanForChanges(const std::string& assetsDir) {
    std::vector<std::string> changed;

    std::error_code ec;
    if (assetsDir.empty() || !fs::is_directory(assetsDir, ec)) {
        return changed;
    }

    for (const auto& entry : fs::recursive_directory_iterator(assetsDir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;

        const std::string ext = entry.path().extension().string();
        if (!IsTextureExtension(ext) && !IsModelExtension(ext)) continue;

        const fs::file_time_type ftime = fs::last_write_time(entry.path(), ec);
        if (ec) continue;
        const uint64_t mtime = static_cast<uint64_t>(FileTimeMs(ftime));
        const uint64_t size = static_cast<uint64_t>(entry.file_size(ec));

        const std::string key = entry.path().string();
        auto it = m_FileStates.find(key);
        if (it == m_FileStates.end()) {
            m_FileStates[key] = { mtime, size };   // 基线：首次只记录
        } else if (it->second.first != mtime || it->second.second != size) {
            it->second = { mtime, size };
            changed.push_back(key);
            LOGI("[AssetHotReload] detected change: %s", key.c_str());
        }
    }
    return changed;
}

void AssetHotReload::Dispatch(const std::string& resolvedPath) {
    const std::string ext = fs::path(resolvedPath).extension().string();
    try {
        if (IsTextureExtension(ext)) {
            if (m_TextureHandler) m_TextureHandler(resolvedPath);
        } else if (IsModelExtension(ext)) {
            if (m_ModelHandler) m_ModelHandler(resolvedPath);
        }
    } catch (const std::exception& ex) {
        LOGE("[AssetHotReload] handler exception for %s: %s", resolvedPath.c_str(), ex.what());
    } catch (...) {
        LOGE("[AssetHotReload] unknown handler exception for %s", resolvedPath.c_str());
    }
}

void AssetHotReload::RequestManualRescan() {
    // 只置标志：热键处理在 ImGui 帧内，销毁 VkImage 必须等到 Poll() 的安全点。
    m_ManualRequest = true;
    LOGI("[AssetHotReload] manual rescan requested (F6)");
}

void AssetHotReload::Poll() {
    const auto now = std::chrono::steady_clock::now();
    const bool forced = m_ManualRequest;
    m_ManualRequest = false;
    if (!forced && m_LastPoll.time_since_epoch().count() != 0) {
        const double elapsed = std::chrono::duration<double>(now - m_LastPoll).count();
        if (elapsed < 1.0) return;
    }
    m_LastPoll = now;

    ProjectManager& project = ProjectManager::GetInstance();
    const std::string assetsDir = project.GetAssetsDir();
    if (assetsDir.empty()) return;   // 无项目打开：不扫描

    const std::vector<std::string> changed = ScanForChanges(assetsDir);
    for (const std::string& path : changed) {
        Dispatch(path);
    }
}
