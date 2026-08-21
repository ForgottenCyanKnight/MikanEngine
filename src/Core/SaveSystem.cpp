// SaveSystem.cpp - 游戏存档/进度 JSON 文件系统(序4: JSON 存档 API)
// 原子写: 临时文件 + 同卷原子替换(MoveFileEx MOVEFILE_REPLACE_EXISTING),
// 断电/崩溃只可能留下 .tmp, 不会损坏旧档。
#include "Core/SaveSystem.h"
#include <SDL3/SDL.h>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {
// slot → 合法文件名(仅字母数字下划线, 防止路径穿越; 长度 ≤ 64)
bool IsValidSlot(const std::string& slot) {
    if (slot.empty() || slot.size() > 64) return false;
    for (char c : slot) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
    }
    return true;
}
} // namespace

namespace Save {

const std::string& GetSaveDir() {
    static const std::string dir = []() {
        std::string base;
        if (const char* p = SDL_GetBasePath()) base = p;
        const std::string d = base + "saves/";
        std::error_code ec;
        std::filesystem::create_directories(d, ec);
        return d;
    }();
    return dir;
}

bool Save(const std::string& slot, const std::string& jsonContent) {
    if (!IsValidSlot(slot)) {
        std::cerr << "[SaveSystem] invalid slot '" << slot << "' (allow [A-Za-z0-9_], len<=64)" << std::endl;
        return false;
    }
    const std::string path = GetSaveDir() + slot + ".json";
    const std::string tmpPath = path + ".tmp";

    // 1. 写临时文件(中断只留 .tmp, 旧档完好)
    {
        std::ofstream ofs(tmpPath, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            std::cerr << "[SaveSystem] cannot open " << tmpPath << std::endl;
            return false;
        }
        ofs << jsonContent;
        ofs.flush();
        if (!ofs) {
            std::cerr << "[SaveSystem] write failed " << tmpPath << std::endl;
            return false;
        }
    }

    // 2. 原子替换
#ifdef _WIN32
    if (MoveFileExA(tmpPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }
    // 极端回退(如跨卷): 删旧档再 rename
    std::error_code ec;
    std::filesystem::remove(path, ec);
    if (std::rename(tmpPath.c_str(), path.c_str()) == 0) return true;
    std::cerr << "[SaveSystem] replace failed: " << path << std::endl;
    return false;
#else
    if (std::rename(tmpPath.c_str(), path.c_str()) == 0) return true;
    // POSIX rename 失败(目标存在且不可替换等)→ 删旧再试
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return std::rename(tmpPath.c_str(), path.c_str()) == 0;
#endif
}

std::optional<std::string> Load(const std::string& slot) {
    if (!IsValidSlot(slot)) return std::nullopt;
    const std::string path = GetSaveDir() + slot + ".json";
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return std::nullopt;
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    if (!ifs && !ifs.eof()) return std::nullopt; // 读取失败(非 EOF 结束)
    return content;
}

bool Delete(const std::string& slot) {
    if (!IsValidSlot(slot)) return false;
    std::error_code ec;
    return std::filesystem::remove(GetSaveDir() + slot + ".json", ec) > 0;
}

bool Exists(const std::string& slot) {
    if (!IsValidSlot(slot)) return false;
    std::error_code ec;
    return std::filesystem::exists(GetSaveDir() + slot + ".json", ec);
}

} // namespace Save
