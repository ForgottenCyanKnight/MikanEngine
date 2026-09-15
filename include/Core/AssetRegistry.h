#pragma once

#include "Platform/Export.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// The registry owns asset identity and lifecycle metadata, not the decoded or
// Vulkan payload.  Loaders can therefore share one path/type identity while
// keeping CPU decoding and GPU ownership in their existing subsystems.
enum class AssetType : std::uint8_t {
    Unknown = 0,
    Texture,
    Model,
    Font,
    Vox,
    Audio,
    Shader,
};

enum class AssetState : std::uint8_t {
    NotLoaded = 0,
    Loading,
    Ready,
    Failed,
    Reloading,
};

using AssetId = std::uint64_t;

struct MIKAN_API AssetRecord {
    AssetId id = 0;
    std::string path;             // normalized, canonical UTF-8 path
    AssetType type = AssetType::Unknown;
    AssetState state = AssetState::NotLoaded;
    std::uint32_t refCount = 0;
    std::uint64_t version = 0;    // increments after each successful load
    std::uint64_t sizeBytes = 0;  // optional decoded/serialized size estimate
    std::string error;
};

class MIKAN_API AssetRegistry {
public:
    static AssetRegistry& GetInstance();

    // Resolve project/engine relative paths and make separators/case
    // deterministic. Empty input returns AssetId 0 and is never registered.
    static std::string NormalizePath(const std::string& path);

    // Register creates a NotLoaded record without taking a reference. Acquire
    // is the owning form used by a payload cache and increments refCount.
    AssetId Register(const std::string& path, AssetType type);
    AssetId Acquire(const std::string& path, AssetType type);
    bool AddRef(AssetId id);
    bool Release(AssetId id);

    // Legal transitions are deliberately explicit so a failed load can be
    // retried without leaving a half-initialized payload visible as Ready.
    bool BeginLoad(AssetId id);
    bool BeginReload(AssetId id);
    bool MarkReady(AssetId id, std::uint64_t sizeBytes = 0);
    bool MarkFailed(AssetId id, const std::string& errorMessage);

    std::optional<AssetRecord> Find(AssetId id) const;
    std::optional<AssetRecord> Find(const std::string& path, AssetType type) const;
    std::vector<AssetRecord> Snapshot() const;
    std::size_t Size() const;

    // Remove only unreferenced, non-loading metadata. Payload destruction is
    // still the loader's responsibility and must happen before Remove.
    bool Remove(AssetId id);
    void Clear();

    static const char* TypeToString(AssetType type);
    static const char* StateToString(AssetState state);

private:
    AssetRegistry() = default;

    mutable std::mutex m_Mutex;
    std::unordered_map<AssetId, AssetRecord> m_Records;
    std::unordered_map<std::string, AssetId> m_PathTypeToId;
};

