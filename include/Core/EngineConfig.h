#pragma once
#include "Platform/Export.h"

#include <string>
#include <SDL3/SDL_filesystem.h>
#include "Core/ProjectManager.h"

namespace EngineConfig
{
    // 默认窗口横向铺满 1920 屏幕，但为 Windows 标题栏/边框留出顶部空间。
    constexpr int WINDOW_WIDTH = 1920;
    constexpr int WINDOW_HEIGHT = 1040;
    constexpr const char* WINDOW_TITLE = "Mikan Engine - Vulkan";

#ifdef __ANDROID__
    constexpr const char* ASSETS_BASE_PATH = "";
#else
    constexpr const char* ASSETS_BASE_PATH = "../../../assets/";
#endif

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
#ifdef __ANDROID__
        return std::string(relativePath);
#else
        return ProjectManager::GetInstance().ResolveAssetPath(relativePath);
#endif
    }

    // 平台路径解析（统一各 ReadFile/加载器的 #ifdef 分支）：
    //   Android: 直通返回（SDL3 从 APK assets 按相对路径读取）
    //   桌面端 : 绝对路径直通；相对路径拼 exe 所在目录
    inline std::string ResolvePlatformPath(const std::string& filename)
    {
#ifdef __ANDROID__
        return filename;
#else
        bool isAbs = (!filename.empty() && (filename[0] == '/' || filename[0] == '\\' || (filename.size() > 1 && filename[1] == ':')));
        if (isAbs) return filename;
        const char* basePath = SDL_GetBasePath();
        return basePath ? std::string(basePath) + filename : filename;
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

    // Shaders are engine assets (compiled with the engine, not project data)
    inline std::string GetShaderPath(const char* shaderName)
    {
        return ProjectManager::GetInstance().GetEngineAssetPath(SHADERS_SPV_PATH) + shaderName;
    }

    // Default engine textures (skybox, blue noise, ...)
    inline std::string GetEngineTexturePath(const char* textureName)
    {
        return ProjectManager::GetInstance().GetEngineAssetPath(TEXTURES_PATH) + textureName;
    }

    // Fonts ship with the engine
    inline std::string GetFontPath(const char* fontName)
    {
        return ProjectManager::GetInstance().GetEngineAssetPath(FONTS_PATH) + fontName;
    }
}
