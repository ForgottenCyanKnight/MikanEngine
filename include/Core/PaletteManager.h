#pragma once
// PaletteManager.h - 调色板交换(Palette Swap)
// 对源纹理应用配色映射表, 生成变体纹理(如角色换色/敌怪变种/地图主题切换)。
// 实现: IMG_Load 源图 → 像素级颜色替换 → IMG_SavePNG 临时文件 → TexturePool::LoadTexture2D
// (完全复用现有上传路径; 变体与原图同翻转/采样语义)。
#include "Platform/Export.h"
#include <string>

class MIKAN_API PaletteManager {
public:
    static PaletteManager& GetInstance();

    // 生成变体纹理: srcTexturePath 源图; paletteJsonPath 配色表(json: {"map": {"原色RRGGBB": "新色RRGGBB", ...}});
    // outName = 变体纹理注册名(TexturePool); 返回是否成功
    bool ApplyPalette(const std::string& srcTexturePath, const std::string& paletteJsonPath,
                      const std::string& outName);

private:
    PaletteManager() = default;
    ~PaletteManager() = default;
    PaletteManager(const PaletteManager&) = delete;
    PaletteManager& operator=(const PaletteManager&) = delete;
};
