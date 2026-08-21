// PaletteManager.cpp - 调色板交换(Palette Swap)
// 像素替换 + 临时 PNG + 现有 LoadTexture2D 上传; 变体纹理与原图同翻转语义。
#include "Core/PaletteManager.h"
#include "Core/ProjectManager.h"
#include "Core/RenderGlobals.h"
#include "Rendering/Renderer2D.h"
#include "Rendering/TexturePool.h"
#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <json.hpp>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unordered_map>

namespace {
// "#RRGGBB" 或 "RRGGBB" → RGB(0-255); 失败返回 false
bool ParseColor(const std::string& s, unsigned& r, unsigned& g, unsigned& b) {
    std::string h = s;
    if (!h.empty() && h.front() == '#') h = h.substr(1);
    if (h.size() != 6) return false;
    for (char c : h) {
        if (!std::isxdigit((unsigned char)c)) return false;
    }
    auto hex = [&](size_t i) -> unsigned {
        char c = h[i];
        if (c >= '0' && c <= '9') return c - '0';
        return (c >= 'a' && c <= 'f') ? c - 'a' + 10 : c - 'A' + 10;
    };
    r = hex(0) * 16 + hex(1);
    g = hex(2) * 16 + hex(3);
    b = hex(4) * 16 + hex(5);
    return true;
}
} // namespace

PaletteManager& PaletteManager::GetInstance() {
    static PaletteManager instance;
    return instance;
}

bool PaletteManager::ApplyPalette(const std::string& srcTexturePath, const std::string& paletteJsonPath,
                                  const std::string& outName) {
    // 1. 读配色表
    const std::string palPath = ProjectManager::GetInstance().ResolveAssetPath(paletteJsonPath);
    std::ifstream ifs(palPath);
    if (!ifs) {
        std::cerr << "[Palette] cannot open palette: " << palPath << std::endl;
        return false;
    }
    nlohmann::json j;
    try { ifs >> j; } catch (...) {
        std::cerr << "[Palette] invalid palette json: " << palPath << std::endl;
        return false;
    }
    struct RGB { unsigned r, g, b; };
    std::unordered_map<unsigned, RGB> map; // 原色(RGB 打包) → 新色
    if (j.contains("map") && j["map"].is_object()) {
        for (auto& [from, to] : j["map"].items()) {
            unsigned fr = 0, fg = 0, fb = 0, tr = 0, tg = 0, tb = 0;
            if (ParseColor(from, fr, fg, fb) && to.is_string() && ParseColor(to.get<std::string>(), tr, tg, tb)) {
                map[(fr << 16) | (fg << 8) | fb] = { tr, tg, tb };
            }
        }
    }
    if (map.empty()) {
        std::cerr << "[Palette] empty color map in " << palPath << std::endl;
        return false;
    }

    // 2. 加载源图
    const std::string srcPath = ProjectManager::GetInstance().ResolveAssetPath(srcTexturePath);
    SDL_Surface* surface = IMG_Load(srcPath.c_str());
    if (!surface) {
        std::cerr << "[Palette] cannot load source: " << srcPath << " (" << SDL_GetError() << ")" << std::endl;
        return false;
    }
    SDL_Surface* rgba = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA8888);
    SDL_DestroySurface(surface);
    if (!rgba) {
        std::cerr << "[Palette] convert failed: " << SDL_GetError() << std::endl;
        return false;
    }

    // 3. 像素替换(注意: SDL_PIXELFORMAT_RGBA8888 小端内存布局为 A B G R)
    unsigned char* px = (unsigned char*)rgba->pixels;
    const int pitch = rgba->pitch;
    const int h = rgba->h;
    const int w = rgba->w;
    int replaced = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            unsigned char* p = px + y * pitch + x * 4;
            const unsigned char a = p[0], b = p[1], g = p[2], r = p[3];
            auto it = map.find((r << 16) | (g << 8) | b);
            if (it != map.end()) {
                p[3] = (unsigned char)it->second.r;
                p[2] = (unsigned char)it->second.g;
                p[1] = (unsigned char)it->second.b;
                p[0] = a;
                ++replaced;
            }
        }
    }

    // 4. 临时 PNG → 复用 LoadTexture2D 上传(目录先确保存在, 与 SaveSystem 同策略)
    std::string base;
    if (const char* p = SDL_GetBasePath()) base = p;
    const std::string tmpDir = base + "saves/";
    std::error_code ec;
    std::filesystem::create_directories(tmpDir, ec);
    const std::string tmpPath = tmpDir + "_palette_tmp.png";
    const bool saved = IMG_SavePNG(rgba, tmpPath.c_str());
    SDL_DestroySurface(rgba);
    if (!saved) {
        std::cerr << "[Palette] IMG_SavePNG failed: " << SDL_GetError() << std::endl;
        return false;
    }

    const bool ok = Renderer2D::GetInstance().LoadTexture(outName, tmpPath);
    std::remove(tmpPath.c_str());
    if (ok) {
        std::cout << "[Palette] applied " << map.size() << " color swaps -> '" << outName
                  << "' (" << replaced << " pixels)" << std::endl;
    }
    return ok;
}
