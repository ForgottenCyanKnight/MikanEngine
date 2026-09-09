// EngineAssets.cpp - 引擎必需资源校验实现
// 校验规则：
//   1. 固定必需文件（字体/引擎纹理/后处理配置），按 EngineConfig 路径解析，逐个 exists
//   2. shaders/spv 完整性：glsl 源目录每个 .vert/.frag/.comp 都必须有对应 .spv 产物
//      （spv 是构建产物；glsl 是源——以源为准做一一对应，新增 shader 自动覆盖，无需改清单）
//   3. 编辑器图标（Editor.dll 已加载时）：资产窗口 4 个图标纹理
// 缺失项以 "类别: 文件名" 形式返回；空列表 = 全部就绪。

#include "Core/EngineAssets.h"

#include "Core/EngineConfig.h"
#include "Core/ProjectManager.h"
#include "Core/Log.h"

#include <SDL3/SDL.h>

#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace EngineAssets {

namespace {

// 固定必需文件表：kind 决定用哪条 EngineConfig 路径解析
struct FixedEntry {
    const char* kind; // "font" | "texture" | "config"
    const char* name;
};

const FixedEntry kFixedFiles[] = {
    { "font",    "simhei.ttf"           }, // TextRenderer 默认字体（EngineMain.cpp:763）
    { "texture", "atmo_lut1.png"        }, // 物理天空 LUT（AtmosphereRenderer.cpp:40）
    { "texture", "atmo_lut2.png"        }, // 物理天空 LUT（AtmosphereRenderer.cpp:41；用户已从 ktx2 换成 png 版）
    { "texture", "atmo_lut_combined.png" },
    { "texture", "end_sky.png"         },
    { "texture", "material.png"         }, // 无纹理材质占位（ModelRenderer.cpp:808；用户删 white.png 后默认指向 material.png）
    { "texture", "black.png"            },
    { "texture", "smaa_area.png"        },
    { "texture", "smaa_search.png"      },
    { "config",  "postprocess_chain.json" }, // 后处理链配置（VulkanManager.cpp:669，GetEngineAssetPath → engine/ 根）
    // 注：bluenoise.png / skybox/ / Blocks.png 是"可选引擎资源"（无 shader 消费或按特性启用），
    //     不在此必需清单——发布包可裁剪，引擎侧缺失容错（见 SkyboxRenderer/WorldRenderer）。
};

const char* kEditorIcons[] = {
    "folder1.png", "folder2.png", "file.png", "material.png", // EditorManager.cpp:98-101
};

std::string ResolveFixed(const FixedEntry& e) {
    std::string kind = e.kind;
    if (kind == "font")    return EngineConfig::GetFontPath(e.name);
    if (kind == "texture") return EngineConfig::GetEngineTexturePath(e.name);
    return ProjectManager::GetInstance().GetEngineAssetPath(e.name); // config：engine/ 根
}

#ifdef __ANDROID__
// Android：APK 内 assets 不是真实文件系统，std::filesystem 读不到。
// 改用 SDL API（SDL 对相对路径自动 fallback 到 assets:// 从 APK assets 根解析）。
// 注意：APK assets 已拍平到根（无 engine/ 嵌套），GetEngineAssetPath 返回相对路径直接从根解析。
static bool AndroidPathExists(const std::string& p, SDL_PathType requiredType) {
    SDL_PathInfo info{};
    if (!SDL_GetPathInfo(p.c_str(), &info)) return false;
    if (requiredType == SDL_PATHTYPE_OTHER) // 不区分类型，存在即可
        return info.type == SDL_PATHTYPE_FILE || info.type == SDL_PATHTYPE_DIRECTORY;
    return info.type == requiredType;
}
#endif

// glsl 源目录每个非 .h 着色器都必须有对应 .spv 产物
void CheckShaders(std::vector<std::string>& missing) {
    const std::string glslDir = ProjectManager::GetInstance().GetEngineAssetPath(EngineConfig::SHADERS_GLSL_PATH);
    const std::string spvDir  = ProjectManager::GetInstance().GetEngineAssetPath(EngineConfig::SHADERS_SPV_PATH);
#ifdef __ANDROID__
    // Android 只打包 spv（无 glsl 源，sync_assets.ps1 已排除）——直接校验 spv 目录存在即可
    if (!AndroidPathExists(spvDir, SDL_PATHTYPE_DIRECTORY)) {
        missing.push_back("shader-dir: " + spvDir);
    }
#else
    std::error_code ec;
    if (!fs::exists(glslDir, ec)) {
        missing.push_back("shader-dir: " + glslDir);
        return;
    }
    for (const auto& entry : fs::directory_iterator(glslDir, ec)) {
        if (!entry.is_regular_file()) continue;
        const std::string ext = entry.path().extension().string();
        if (ext != ".vert" && ext != ".frag" && ext != ".comp") continue;
        const std::string stem = entry.path().stem().string();
        const std::string spvName = stem + ext + ".spv"; // e.g. fullscreen.frag.spv
        if (!fs::exists(spvDir + spvName, ec)) {
            missing.push_back("shader: " + spvName);
        }
    }
#endif
}

// 编辑器图标：Editor.dll 已加载（编辑器模式）才校验
void CheckEditorIcons(std::vector<std::string>& missing) {
#ifdef _WIN32
    if (GetModuleHandleA("Editor.dll") == nullptr) return;
    for (const char* icon : kEditorIcons) {
        const std::string p = EngineConfig::GetEngineTexturePath(icon);
        std::error_code ec;
        if (!fs::exists(p, ec)) missing.push_back(std::string("editor-icon: ") + icon);
    }
#else
    (void)missing;
#endif
}

} // namespace

std::vector<std::string> ValidateEngineAssets() {
    std::vector<std::string> missing;

    for (const FixedEntry& e : kFixedFiles) {
#ifdef __ANDROID__
        if (!AndroidPathExists(ResolveFixed(e), SDL_PATHTYPE_FILE)) {
            missing.push_back(std::string(e.kind) + ": " + e.name);
        }
#else
        std::error_code ec;
        if (!fs::exists(ResolveFixed(e), ec)) {
            missing.push_back(std::string(e.kind) + ": " + e.name);
        }
#endif
    }

    CheckShaders(missing);
    CheckEditorIcons(missing);

    return missing;
}

} // namespace EngineAssets
