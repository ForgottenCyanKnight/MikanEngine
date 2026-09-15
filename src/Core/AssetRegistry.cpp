#include "Core/AssetRegistry.h"

#include "Core/ProjectManager.h"
#include "Core/Utf8Path.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <limits>
#include <system_error>

namespace {

std::string LowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

std::string MakePathTypeKey(const std::string& normalizedPath, AssetType type)
{
    // The type byte is part of the identity. A model and a shader may legally
    // share a filename in different import domains without colliding.
    return std::to_string(static_cast<unsigned int>(type)) + ":" + normalizedPath;
}

AssetId HashPathType(const std::string& key)
{
    // FNV-1a is deterministic across processes and platforms. This first
    // registry slice intentionally uses normalized path identity; content
    // hashes can be added later without changing loader-facing handles.
    constexpr AssetId kOffset = 14695981039346656037ull;
    constexpr AssetId kPrime = 1099511628211ull;
    AssetId hash = kOffset;
    for (unsigned char value : key) {
        hash ^= static_cast<AssetId>(value);
        hash *= kPrime;
    }
    return hash == 0 ? 1 : hash;
}

} // namespace

AssetRegistry& AssetRegistry::GetInstance()
{
    static AssetRegistry registry;
    return registry;
}

std::string AssetRegistry::NormalizePath(const std::string& input)
{
    if (input.empty()) return {};

#ifdef __ANDROID__
    std::string normalized = input;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    return normalized;
#else
    std::string resolved = ProjectManager::GetInstance().ResolveAssetPath(input);
    if (resolved.empty()) resolved = input;

    std::error_code error;
    std::filesystem::path path = Utf8Path(resolved);
    path = std::filesystem::absolute(path, error);
    if (error) {
        error.clear();
        path = Utf8Path(resolved).lexically_normal();
    } else {
        path = path.lexically_normal();
    }

    error.clear();
    if (std::filesystem::exists(path, error) && !error) {
        error.clear();
        const std::filesystem::path canonical =
            std::filesystem::weakly_canonical(path, error);
        if (!error) path = canonical;
    }

    std::string normalized = GenericUtf8String(path);
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
#ifdef _WIN32
    normalized = LowerAscii(std::move(normalized));
#endif
    return normalized;
#endif
}

AssetId AssetRegistry::Register(const std::string& path, AssetType type)
{
    const std::string normalizedPath = NormalizePath(path);
    if (normalizedPath.empty()) return 0;
    const std::string key = MakePathTypeKey(normalizedPath, type);

    std::lock_guard<std::mutex> lock(m_Mutex);
    const auto existing = m_PathTypeToId.find(key);
    if (existing != m_PathTypeToId.end()) return existing->second;

    AssetId id = HashPathType(key);
    while (id == 0) ++id;
    while (true) {
        const auto collision = m_Records.find(id);
        if (collision == m_Records.end()) break;
        if (collision->second.path == normalizedPath && collision->second.type == type) {
            m_PathTypeToId.emplace(key, id);
            return id;
        }
        ++id;
    }

    AssetRecord record;
    record.id = id;
    record.path = normalizedPath;
    record.type = type;
    m_Records.emplace(id, record);
    m_PathTypeToId.emplace(key, id);
    return id;
}

AssetId AssetRegistry::Acquire(const std::string& path, AssetType type)
{
    const AssetId id = Register(path, type);
    if (id == 0 || !AddRef(id)) return 0;
    return id;
}

bool AssetRegistry::AddRef(AssetId id)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    const auto it = m_Records.find(id);
    if (it == m_Records.end() || it->second.refCount == std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    ++it->second.refCount;
    return true;
}

bool AssetRegistry::Release(AssetId id)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    const auto it = m_Records.find(id);
    if (it == m_Records.end() || it->second.refCount == 0) return false;
    --it->second.refCount;
    return true;
}

bool AssetRegistry::BeginLoad(AssetId id)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    const auto it = m_Records.find(id);
    if (it == m_Records.end()) return false;
    if (it->second.state != AssetState::NotLoaded &&
        it->second.state != AssetState::Failed) {
        return false;
    }
    it->second.state = AssetState::Loading;
    it->second.error.clear();
    return true;
}

bool AssetRegistry::BeginReload(AssetId id)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    const auto it = m_Records.find(id);
    if (it == m_Records.end()) return false;
    if (it->second.state == AssetState::NotLoaded) {
        it->second.state = AssetState::Loading;
        it->second.error.clear();
        return true;
    }
    if (it->second.state != AssetState::Ready &&
        it->second.state != AssetState::Failed) {
        return false;
    }
    it->second.state = AssetState::Reloading;
    it->second.error.clear();
    return true;
}

bool AssetRegistry::MarkReady(AssetId id, std::uint64_t sizeBytes)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    const auto it = m_Records.find(id);
    if (it == m_Records.end() ||
        (it->second.state != AssetState::Loading &&
         it->second.state != AssetState::Reloading)) {
        return false;
    }
    it->second.state = AssetState::Ready;
    it->second.error.clear();
    it->second.sizeBytes = sizeBytes;
    if (it->second.version < std::numeric_limits<std::uint64_t>::max()) {
        ++it->second.version;
    }
    return true;
}

bool AssetRegistry::MarkFailed(AssetId id, const std::string& errorMessage)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    const auto it = m_Records.find(id);
    if (it == m_Records.end() ||
        (it->second.state != AssetState::Loading &&
         it->second.state != AssetState::Reloading)) {
        return false;
    }
    it->second.state = AssetState::Failed;
    it->second.error = errorMessage.empty() ? "asset load failed" : errorMessage;
    return true;
}

std::optional<AssetRecord> AssetRegistry::Find(AssetId id) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    const auto it = m_Records.find(id);
    return it == m_Records.end() ? std::nullopt : std::optional<AssetRecord>(it->second);
}

std::optional<AssetRecord> AssetRegistry::Find(const std::string& path, AssetType type) const
{
    const std::string normalizedPath = NormalizePath(path);
    if (normalizedPath.empty()) return std::nullopt;
    const std::string key = MakePathTypeKey(normalizedPath, type);
    std::lock_guard<std::mutex> lock(m_Mutex);
    const auto idIt = m_PathTypeToId.find(key);
    if (idIt == m_PathTypeToId.end()) return std::nullopt;
    const auto recordIt = m_Records.find(idIt->second);
    return recordIt == m_Records.end()
        ? std::nullopt
        : std::optional<AssetRecord>(recordIt->second);
}

std::vector<AssetRecord> AssetRegistry::Snapshot() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    std::vector<AssetRecord> records;
    records.reserve(m_Records.size());
    for (const auto& [id, record] : m_Records) records.push_back(record);
    std::sort(records.begin(), records.end(), [](const AssetRecord& left, const AssetRecord& right) {
        if (left.path != right.path) return left.path < right.path;
        if (left.type != right.type) {
            return static_cast<unsigned int>(left.type) < static_cast<unsigned int>(right.type);
        }
        return left.id < right.id;
    });
    return records;
}

std::size_t AssetRegistry::Size() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_Records.size();
}

bool AssetRegistry::Remove(AssetId id)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    const auto it = m_Records.find(id);
    if (it == m_Records.end() || it->second.refCount != 0 ||
        it->second.state == AssetState::Loading ||
        it->second.state == AssetState::Reloading) {
        return false;
    }
    const std::string key = MakePathTypeKey(it->second.path, it->second.type);
    m_PathTypeToId.erase(key);
    m_Records.erase(it);
    return true;
}

void AssetRegistry::Clear()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_PathTypeToId.clear();
    m_Records.clear();
}

const char* AssetRegistry::TypeToString(AssetType type)
{
    switch (type) {
        case AssetType::Texture: return "Texture";
        case AssetType::Model: return "Model";
        case AssetType::Font: return "Font";
        case AssetType::Vox: return "Vox";
        case AssetType::Audio: return "Audio";
        case AssetType::Shader: return "Shader";
        default: return "Unknown";
    }
}

const char* AssetRegistry::StateToString(AssetState state)
{
    switch (state) {
        case AssetState::Loading: return "Loading";
        case AssetState::Ready: return "Ready";
        case AssetState::Failed: return "Failed";
        case AssetState::Reloading: return "Reloading";
        default: return "NotLoaded";
    }
}

