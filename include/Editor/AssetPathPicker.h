#pragma once

#include <string>

namespace Editor {

enum class AssetPathKind {
    Any,
    Texture,
    Model,
    Voxel,
    Motion,
    Audio,
    Tilemap
};

// 将项目内绝对路径转成项目相对路径；项目外文件保留绝对路径。
std::string NormalizeAssetPath(const std::string& path);

// 调用 Windows 系统文件对话框选择资源文件。
std::string PickAssetPath(AssetPathKind kind = AssetPathKind::Any);

// 只读路径框，支持“浏览...”、资产窗口拖拽和“清除”。
// 返回 true 表示路径在本帧发生变化。
bool RenderAssetPathInput(const char* label,
                          std::string& path,
                          AssetPathKind kind = AssetPathKind::Any,
                          bool acceptDrop = true);

} // namespace Editor
