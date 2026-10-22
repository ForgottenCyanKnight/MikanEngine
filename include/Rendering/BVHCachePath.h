#pragma once
#include "Platform/Export.h"
#ifndef BVH_CACHE_PATH_H
#define BVH_CACHE_PATH_H
// Single managed disk-cache location for every auto-generated BVH/TLAS file:
//
//     <engineRoot>/cache/bvh/
//
// The directory is disposable: deleting it only costs a rebuild on the next
// model load. Filenames embed an FNV-1a hash of the normalized absolute
// source path, so same-stem models from different directories cannot collide
// (the old scheme wrote "<source dir>/bvh/" and polluted asset folders).
// Android keeps the legacy assets-relative paths because APK assets are
// read-only and caches may be shipped inside the APK.

#include "Core/ProjectManager.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace mikan::render {

// FNV-1a 64-bit of the normalized absolute source path, printed as 16 hex chars.
inline std::string BvhCacheKey(const std::filesystem::path& normalized) {
    uint64_t hash = 1469598103934665603ull;
    for (char c : normalized.string()) {
        hash ^= static_cast<uint8_t>(c);
        hash *= 1099511628211ull;
    }
    char digits[17];
    for (int i = 15; i >= 0; --i) {
        digits[i] = "0123456789abcdef"[hash & 0xF];
        hash >>= 4;
    }
    digits[16] = '\0';
    return std::string(digits);
}

// Desktop: <engineRoot>/cache/bvh/<stem>_<pathhash>_<tag><extension>
// Android: legacy <source dir>/bvh/<stem>_<tag><extension> (assets read-only)
inline std::string BvhCachePathFor(const std::string& sourcePath, const char* tag, const char* extension) {
    const std::filesystem::path source(sourcePath);
    const std::string stem = source.stem().string();
#ifdef __ANDROID__
    return (source.parent_path() / "bvh" / (stem + "_" + tag + extension)).string();
#else
    std::filesystem::path dir =
        std::filesystem::path(ProjectManager::GetInstance().GetEngineRoot()) / "cache" / "bvh";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path normalized =
        std::filesystem::exists(source) ? std::filesystem::absolute(source).lexically_normal()
                                        : std::filesystem::weakly_canonical(source);
    const std::string file = stem + "_" + BvhCacheKey(normalized) + "_" + tag + extension;
    return (dir / file).string();
#endif
}

}
#endif
