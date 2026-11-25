// EngineAssets.cpp - 引擎必需资源校验实现
// 校验规则：
//   1. 固定必需文件（字体/引擎纹理/后处理配置），按 EngineConfig 路径解析，逐个 exists
//   2. shaders/spv 完整性：源码和 SPIR-V 保留相同的相对路径，编译清单同时记录变体
//   3. 编辑器图标（Editor.dll 已加载时）：资产窗口 4 个图标纹理
// 缺失项以 "类别: 文件名" 形式返回；空列表 = 全部就绪。

#include "Core/EngineAssets.h"

#include "Core/EngineConfig.h"
#include "Core/ProjectManager.h"
#include "Core/Log.h"
#include "Core/ShaderBuildPlan.h"
#include "Core/Utf8Path.h"

#include <SDL3/SDL.h>

#include <filesystem>
#include <span>
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
#ifndef _WIN32
    { "font",    "simhei.ttf"           }, // TextRenderer 默认字体（EngineMain.cpp:763）
#endif
    { "texture", "atmo_lut1.png"        }, // 物理天空 LUT（AtmosphereRenderer.cpp:40）
    { "texture", "atmo_lut2.png"        }, // 物理天空 LUT（AtmosphereRenderer.cpp:41；用户已从 ktx2 换成 png 版）
    { "texture", "atmo_lut_combined.png" },
    { "texture", "end_sky.png"         },
    { "texture", "white.png"            }, // 无纹理材质占位（binding 0/1 fallback；material.png 是棋盘格调试纹理，不能当 white 用）
    { "texture", "material.png"         }, // 编辑器文件图标 / 棋盘格调试纹理
    { "texture", "black.png"            },
    { "texture", "smaa_area.png"        },
    { "texture", "smaa_search.png"      },
    { "config",  "postprocess_chain.json" }, // 后处理链配置（VulkanManager.cpp:669，GetEngineAssetPath → engine/ 根）
    // 注：bluenoise.png / skybox/ / Blocks.png 是"可选引擎资源"（无 shader 消费或按特性启用），
    //     不在此必需清单——发布包可裁剪，引擎侧缺失容错（见 SkyboxRenderer/WorldRenderer）。
};

const FixedEntry kVoxelPrototypeFiles[] = {
    { "font", "simhei.ttf" },
    { "texture", "Blocks.png" },
    { "texture", "white.png" },
    { "texture", "black.png" },
    { "config", "postprocess_raster.json" },
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

void CheckShaders(std::vector<std::string>& missing) {
    const std::string spvDir  = ProjectManager::GetInstance().GetEngineAssetPath(EngineConfig::SHADERS_SPV_PATH);
    const std::string planPath = spvDir + ShaderBuildPlan::kFilename;
    const auto plan = ShaderBuildPlan::Read(planPath);
    if (plan.empty()) missing.push_back("shader-plan: " + planPath);

#if !defined(__ANDROID__) && !defined(MIKAN_OPENHARMONY)
    const fs::path glslDir = Utf8Path(ProjectManager::GetInstance().GetEngineAssetPath(EngineConfig::SHADERS_GLSL_PATH));
    std::error_code ec;
    const auto checkOutput = [&](const std::string& name) {
        std::error_code fileError;
        if (!fs::is_regular_file(Utf8Path(spvDir + name), fileError)) {
            missing.push_back("shader: " + name);
        }
    };
    for (fs::recursive_directory_iterator it(glslDir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const auto ext = it->path().extension();
        if (ext != ".vert" && ext != ".frag" && ext != ".comp") continue;
        checkOutput(GenericUtf8String(it->path().lexically_relative(glslDir)) + ".spv");
    }
    if (ec) missing.push_back("shader-dir: " + Utf8String(glslDir));
    for (const auto& entry : plan) {
        if (entry.output != entry.source + ".spv") checkOutput(entry.output);
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

bool IsAndroidVoxelPrototype() {
#ifdef __ANDROID__
    static const bool enabled = AndroidPathExists("mikan_android_voxel_prototype.txt", SDL_PATHTYPE_FILE);
    return enabled;
#else
    return false;
#endif
}

std::vector<std::string> ValidateEngineAssets() {
    std::vector<std::string> missing;

    const auto fixedFiles = IsAndroidVoxelPrototype()
        ? std::span<const FixedEntry>{kVoxelPrototypeFiles}
        : std::span<const FixedEntry>{kFixedFiles};
    for (const FixedEntry& e : fixedFiles) {
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
