#include "Editor/AssetPathPicker.h"

#include "Core/ProjectManager.h"
#include <imgui/imgui.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#endif

namespace Editor {
namespace {

std::string LowerExtension(const std::string& path) {
    std::string ext = std::filesystem::u8path(path).extension().u8string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

bool IsAcceptedDrop(const std::string& path, AssetPathKind kind) {
    if (kind == AssetPathKind::Any) return true;
    const std::string ext = LowerExtension(path);
    switch (kind) {
    case AssetPathKind::Texture:
        return ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
               ext == ".tga" || ext == ".bmp" || ext == ".dds" || ext == ".ktx2";
    case AssetPathKind::Model:
        return ext == ".obj" || ext == ".fbx" || ext == ".gltf" ||
               ext == ".glb" || ext == ".pmx" || ext == ".pmd" ||
               ext == ".dae" || ext == ".x";
    case AssetPathKind::Voxel:
        return ext == ".vox";
    case AssetPathKind::Motion:
        return ext == ".vmd" || ext == ".vpd";
    case AssetPathKind::Audio:
        return ext == ".wav" || ext == ".mp3" || ext == ".ogg" || ext == ".flac";
    case AssetPathKind::Tilemap:
        return ext == ".tmx" || ext == ".tsx" || ext == ".json";
    case AssetPathKind::Any:
        break;
    }
    return true;
}

#ifdef _WIN32
const wchar_t* FilterForKind(AssetPathKind kind) {
    static constexpr wchar_t kAll[] = L"所有文件 (*.*)\0*.*\0\0";
    static constexpr wchar_t kTexture[] =
        L"纹理文件 (*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.dds;*.ktx2)\0"
        L"*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.dds;*.ktx2\0"
        L"所有文件 (*.*)\0*.*\0\0";
    static constexpr wchar_t kModel[] =
        L"模型文件 (*.obj;*.fbx;*.gltf;*.glb;*.pmx;*.pmd;*.dae;*.x)\0"
        L"*.obj;*.fbx;*.gltf;*.glb;*.pmx;*.pmd;*.dae;*.x\0"
        L"所有文件 (*.*)\0*.*\0\0";
    static constexpr wchar_t kVoxel[] = L"体素文件 (*.vox)\0*.vox\0所有文件 (*.*)\0*.*\0\0";
    static constexpr wchar_t kMotion[] = L"动作文件 (*.vmd;*.vpd)\0*.vmd;*.vpd\0所有文件 (*.*)\0*.*\0\0";
    static constexpr wchar_t kAudio[] =
        L"音频文件 (*.wav;*.mp3;*.ogg;*.flac)\0*.wav;*.mp3;*.ogg;*.flac\0"
        L"所有文件 (*.*)\0*.*\0\0";
    static constexpr wchar_t kTilemap[] =
        L"瓦片地图 (*.tmx;*.tsx;*.json)\0*.tmx;*.tsx;*.json\0"
        L"所有文件 (*.*)\0*.*\0\0";
    switch (kind) {
    case AssetPathKind::Texture: return kTexture;
    case AssetPathKind::Model: return kModel;
    case AssetPathKind::Voxel: return kVoxel;
    case AssetPathKind::Motion: return kMotion;
    case AssetPathKind::Audio: return kAudio;
    case AssetPathKind::Tilemap: return kTilemap;
    case AssetPathKind::Any: return kAll;
    }
    return kAll;
}
#endif

} // namespace

std::string NormalizeAssetPath(const std::string& path) {
    if (path.empty()) return {};
    const std::filesystem::path input = std::filesystem::u8path(path);
    if (input.is_absolute()) {
        const std::string projectRelative =
            ProjectManager::GetInstance().GetProjectRelativePath(path);
        if (!projectRelative.empty()) return projectRelative;
    }
    return input.lexically_normal().generic_u8string();
}

std::string PickAssetPath(AssetPathKind kind) {
#ifdef _WIN32
    std::array<wchar_t, 32768> fileName{};
    const std::string assetsUtf8 = ProjectManager::GetInstance().GetAssetsDir();
    const std::wstring initialDir = assetsUtf8.empty()
        ? std::wstring()
        : std::filesystem::u8path(assetsUtf8).wstring();

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = GetActiveWindow();
    dialog.lpstrFile = fileName.data();
    dialog.nMaxFile = static_cast<DWORD>(fileName.size());
    dialog.lpstrFilter = FilterForKind(kind);
    dialog.nFilterIndex = 1;
    dialog.lpstrInitialDir = initialDir.empty() ? nullptr : initialDir.c_str();
    dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
                   OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&dialog)) {
        return NormalizeAssetPath(std::filesystem::path(fileName.data()).u8string());
    }
#else
    (void)kind;
#endif
    return {};
}

bool RenderAssetPathInput(const char* label,
                          std::string& path,
                          AssetPathKind kind,
                          bool acceptDrop) {
    if (!label) label = "路径";
    ImGui::PushID(static_cast<const void*>(&path));
    ImGui::TextUnformatted(label);
    ImGui::SameLine();

    const ImGuiStyle& style = ImGui::GetStyle();
    const float spacing = style.ItemSpacing.x;
    const float browseWidth = ImGui::CalcTextSize("浏览...").x + style.FramePadding.x * 2.0f;
    const float clearWidth = ImGui::CalcTextSize("清除").x + style.FramePadding.x * 2.0f;
    const float clearReserve = path.empty() ? 0.0f : clearWidth + spacing;
    const float inputWidth = std::max(80.0f,
        ImGui::GetContentRegionAvail().x - browseWidth - spacing - clearReserve);

    std::array<char, 4096> buffer{};
    std::strncpy(buffer.data(), path.c_str(), buffer.size() - 1);
    ImGui::SetNextItemWidth(inputWidth);
    ImGui::InputText("##asset_path", buffer.data(), buffer.size(),
                     ImGuiInputTextFlags_ReadOnly);

    bool changed = false;
    if (acceptDrop && ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_ITEM")) {
            const char* data = static_cast<const char*>(payload->Data);
            size_t length = payload->DataSize;
            if (length > 0 && data[length - 1] == '\0') --length;
            const std::string dropped(data, length);
            if (IsAcceptedDrop(dropped, kind)) {
                path = NormalizeAssetPath(dropped);
                changed = true;
            }
        }
        ImGui::EndDragDropTarget();
    }

    ImGui::SameLine();
    if (ImGui::Button("浏览...")) {
        const std::string selected = PickAssetPath(kind);
        if (!selected.empty()) {
            path = selected;
            changed = true;
        }
    }
    if (!path.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("清除")) {
            path.clear();
            changed = true;
        }
    }
    ImGui::PopID();
    return changed;
}

} // namespace Editor
