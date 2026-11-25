#pragma once
#include "Platform/Export.h"

#include <string>
#include <SDL3/SDL_filesystem.h>
#include "Core/ProjectManager.h"

namespace EngineConfig
{
    // 默认引擎启动窗口尺寸；可在项目管理器“设置”中保存为 engine_settings.json。
    constexpr int WINDOW_WIDTH = EngineDisplaySettings::kDefaultEngineWidth;
    constexpr int WINDOW_HEIGHT = EngineDisplaySettings::kDefaultEngineHeight;
    constexpr const char* WINDOW_TITLE = "Mikan Engine - Vulkan";

    // Project resources are resolved by ProjectManager after a project is
    // selected. There is deliberately no desktop-wide assets/ fallback.
    // Mobile targets keep package-relative paths for SDL's asset reader.
    constexpr const char* ASSETS_BASE_PATH = "";

    constexpr const char* SHADERS_PATH = "shaders/";
    constexpr const char* SHADERS_SPV_PATH = "shaders/spv/";
    constexpr const char* SHADERS_GLSL_PATH = "shaders/glsl/";

    constexpr const char* TEXTURES_PATH = "textures/";
    constexpr const char* SKYBOX_PATH = "textures/skybox";

    constexpr const char* FONTS_PATH = "fonts/";
    constexpr const char* DEFAULT_FONT = "fonts/simhei.ttf";

    constexpr float FOV = 45.0f;
    constexpr float NEAR_PLANE = 0.1f;
    constexpr float FAR_PLANE = 500.0f;

    inline std::string GetFullPath(const char* relativePath)
    {
#if defined(__ANDROID__) || defined(MIKAN_OPENHARMONY)
        return std::string(relativePath);
#else
        return ProjectManager::GetInstance().ResolveAssetPath(relativePath);
#endif
    }

    // 平台路径解析（统一各 ReadFile/加载器的 #ifdef 分支）：
    //   Android / HarmonyOS: 直通返回（SDL3 按相对路径读取包内资源）
    //   桌面端 : 绝对路径直通；项目相对路径交给 ProjectManager
    inline std::string ResolvePlatformPath(const std::string& filename)
    {
#if defined(__ANDROID__) || defined(MIKAN_OPENHARMONY)
        return filename;
#else
        return ProjectManager::GetInstance().ResolveAssetPath(filename);
#endif
    }

    // Android 专用：移除开头的 assets/ 前缀（SDL3 Android 从 assets 读取时不需要该前缀）
    inline std::string StripAssetPrefix(const std::string& path)
    {
#ifdef __ANDROID__
        if (path.find("assets/") == 0) return path.substr(7);
#endif
        return path;
    }

    // Accepts a path relative to shaders/spv or a legacy bare filename.
    MIKAN_API std::string GetShaderPath(const char* shaderName);

    // Default engine textures (skybox, blue noise, ...)
    inline std::string GetEngineTexturePath(const char* textureName)
    {
        return ProjectManager::GetInstance().GetEngineAssetPath(TEXTURES_PATH) + textureName;
    }

    // Explicit font assets; Windows default UI/text fonts are obtained from DirectWrite.
    inline std::string GetFontPath(const char* fontName)
    {
        return ProjectManager::GetInstance().GetEngineAssetPath(FONTS_PATH) + fontName;
    }
}
