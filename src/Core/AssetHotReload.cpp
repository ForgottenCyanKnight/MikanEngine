#include "Core/AssetHotReload.h"
#include "Core/CpuStageTrace.h"
#include "Core/ProjectManager.h"
#include "Core/Log.h"
#include "Core/Utf8Path.h"

#include <algorithm>
#include <cctype>
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

void AssetHotReload::ScanForChanges(const std::string& root, bool linkedAsset,
                                    std::vector<ChangedAsset>& changed) {
    const fs::path scanPath = Utf8Path(root);
    auto inspect = [&](const fs::directory_entry& entry) {
        std::error_code ec;
        if (!entry.is_regular_file(ec)) return;

        const std::string ext = entry.path().extension().string();
        if (!IsTextureExtension(ext) && !IsModelExtension(ext)) return;

        const fs::file_time_type ftime = entry.last_write_time(ec);
        if (ec) return;
        const uint64_t mtime = static_cast<uint64_t>(FileTimeMs(ftime));
        const uint64_t size = static_cast<uint64_t>(entry.file_size(ec));
        if (ec) return;

        std::string key = Utf8String(entry.path());
        auto it = m_FileStates.find(key);
        if (it == m_FileStates.end()) {
            m_FileStates.emplace(std::move(key), std::make_pair(mtime, size));
        } else if (it->second.first != mtime || it->second.second != size) {
            changed.push_back({std::move(key), linkedAsset ? root : std::string{}});
            it->second = { mtime, size };
        }
    };
    std::error_code ec;
    if (fs::is_regular_file(scanPath, ec)) {
        const fs::directory_entry entry(scanPath, ec);
        if (!ec) inspect(entry);
    } else {
        for (fs::recursive_directory_iterator it(scanPath,
                 fs::directory_options::skip_permission_denied, ec), end;
             it != end && !ec; it.increment(ec)) inspect(*it);
    }
}

void AssetHotReload::Dispatch(const std::string& resolvedPath) {
    const std::string ext = Utf8Path(resolvedPath).extension().string();
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
    m_ManualRequest = true;
    LOGI("[AssetHotReload] manual rescan requested (F6)");
}

void AssetHotReload::Poll() {
    if (m_Scan.valid() && m_Scan.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return;
    }

    const ProjectManager& project = ProjectManager::GetInstance();
    const std::string& projectRoot = project.GetProjectRoot();
    if (m_Scan.valid()) {
        try {
            auto changed = m_Scan.get();
            if (!changed.empty() && m_ScanProjectRoot == projectRoot &&
                m_ScanAssetsDir == project.GetAssetsDir()) {
                const auto& manifestAssets = project.GetManifest().assets;
                for (const auto& asset : changed) {
                    if (!asset.linkedRoot.empty() &&
                        std::find(manifestAssets.begin(), manifestAssets.end(), asset.linkedRoot) ==
                            manifestAssets.end()) {
                        continue;
                    }
                    LOGI("[AssetHotReload] detected change: %s", asset.path.c_str());
                    Dispatch(asset.path);
                }
            }
        } catch (const std::exception& ex) {
            LOGE("[AssetHotReload] scan failed: %s", ex.what());
        }
    }

    const auto now = std::chrono::steady_clock::now();
    if (!m_ManualRequest && now < m_NextPoll) return;

    std::string assetsDir = project.GetAssetsDir();
    if (assetsDir.empty()) return;
    std::string scanProjectRoot = projectRoot;

    std::vector<std::string> linkedAssets;
    for (const auto& asset : project.GetManifest().assets) {
        if (Utf8Path(asset).is_absolute() && project.IsLinkedAsset(asset)) {
            linkedAssets.push_back(asset);
        }
    }

    const bool resetBaseline = m_ScanProjectRoot != projectRoot || m_ScanAssetsDir != assetsDir;
    m_NextPoll = now + std::chrono::seconds(1);
    m_ManualRequest = false;
    try {
        m_Scan = std::async(std::launch::async,
            [this, assetsDir, linkedAssets = std::move(linkedAssets), resetBaseline] {
                Core::CpuStageTrace trace("hot_reload.asset_scan");
                if (resetBaseline) m_FileStates.clear();
                std::vector<ChangedAsset> changed;
                ScanForChanges(assetsDir, false, changed);
                for (const auto& path : linkedAssets) ScanForChanges(path, true, changed);
                return changed;
            });
        m_ScanProjectRoot = std::move(scanProjectRoot);
        m_ScanAssetsDir = std::move(assetsDir);
    } catch (const std::exception& ex) {
        LOGE("[AssetHotReload] could not start scan: %s", ex.what());
    }
}

void AssetHotReload::Shutdown() {
    if (m_Scan.valid()) m_Scan.wait();
    m_Scan = {};
    m_FileStates.clear();
    m_ScanProjectRoot.clear();
    m_ScanAssetsDir.clear();
    m_NextPoll = {};
    m_ManualRequest = false;
}
