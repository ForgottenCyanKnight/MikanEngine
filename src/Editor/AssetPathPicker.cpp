#include "Editor/AssetPathPicker.h"
#include "Core/I18n.h"
#include "Core/Utf8Path.h"

#include "Core/ProjectManager.h"
#include "Rendering/ModelLoader.h"
#include <imgui/imgui.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#endif

namespace Editor {
namespace {

std::string LowerExtension(const std::string& path) {
    std::string ext = Utf8String(Utf8Path(path).extension());
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
               ext == ".dae" || ext == ".x" || ext == ".blend";
    case AssetPathKind::Voxel:
        return ext == ".vox" || ext == ".voxmesh" || ext == ".cxw";
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

#if defined(__linux__) && !defined(__ANDROID__)
std::array<char, 4096> s_dialogPath{};
const char* s_dialogError = nullptr;
#endif

#ifdef _WIN32
const wchar_t* FilterForKind(AssetPathKind kind) {
    static constexpr wchar_t kAll[] = L"所有文件 (*.*)\0*.*\0\0";
    static constexpr wchar_t kTexture[] =
        L"纹理文件 (*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.dds;*.ktx2)\0"
        L"*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.dds;*.ktx2\0"
        L"所有文件 (*.*)\0*.*\0\0";
    static constexpr wchar_t kModel[] =
        L"模型文件 (*.obj;*.fbx;*.gltf;*.glb;*.pmx;*.pmd;*.dae;*.x;*.blend)\0"
        L"*.obj;*.fbx;*.gltf;*.glb;*.pmx;*.pmd;*.dae;*.x;*.blend\0"
        L"所有文件 (*.*)\0*.*\0\0";
    static constexpr wchar_t kVoxel[] = L"体素文件 (*.vox;*.voxmesh;*.cxw)\0*.vox;*.voxmesh;*.cxw\0所有文件 (*.*)\0*.*\0\0";
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
    if (LowerExtension(path) == ".blend") {
        const auto compiled = ModelLoader::PrepareModelAsset(path);
        return compiled.empty() ? std::string{} : NormalizeAssetPath(compiled);
    }
    const std::filesystem::path input = Utf8Path(path);
    if (input.is_absolute()) {
        const std::string projectRelative =
            ProjectManager::GetInstance().GetProjectRelativePath(path);
        if (!projectRelative.empty()) return projectRelative;
    }
    return GenericUtf8String(input.lexically_normal());
}

std::string PickAssetPath(AssetPathKind kind) {
#ifdef _WIN32
    std::array<wchar_t, 32768> fileName{};
    const std::string assetsUtf8 = ProjectManager::GetInstance().GetAssetsDir();
    const std::wstring initialDir = assetsUtf8.empty()
        ? std::wstring()
        : Utf8Path(assetsUtf8).wstring();

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
        return NormalizeAssetPath(Utf8String(std::filesystem::path(fileName.data())));
    }
#else
    (void)kind;
#endif
    return {};
}

std::string PickFolderPath(const char* title) {
#ifdef _WIN32
    const std::wstring titleWide = mikanpath::Utf8ToWide(
        title ? std::string(title) : std::string("选择文件夹"));

    BROWSEINFOW dialog{};
    dialog.hwndOwner = GetActiveWindow();
    dialog.lpszTitle = titleWide.c_str();
    dialog.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;

    LPITEMIDLIST selected = SHBrowseForFolderW(&dialog);
    if (!selected) return {};

    std::array<wchar_t, 32768> folderPath{};
    std::string result;
    if (SHGetPathFromIDListW(selected, folderPath.data())) {
        result = Utf8String(std::filesystem::path(folderPath.data()));
    }
    CoTaskMemFree(selected);
    return result;
#else
    (void)title;
    return {};
#endif
}

#if defined(__linux__) && !defined(__ANDROID__)
void OpenPathDialog(const char* popup, const std::string& initialPath) {
    std::strncpy(s_dialogPath.data(), initialPath.c_str(), s_dialogPath.size() - 1);
    s_dialogPath.back() = '\0';
    s_dialogError = nullptr;
    ImGui::OpenPopup(popup);
}

std::string RenderPathDialog(const char* popup, const std::string& baseDirectory,
                             bool directory, AssetPathKind kind) {
    if (!ImGui::IsPopupOpen(popup)) return {};
    ImGui::SetNextWindowSize(ImVec2(590, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(popup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return {};
    }

    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1.0f);
    const bool submitted = ImGui::InputTextWithHint(
        "##path", Tr("路径"), s_dialogPath.data(), s_dialogPath.size(),
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    if (s_dialogError) ImGui::TextWrapped("%s", Tr(s_dialogError));

    std::string selected;
    if (ImGui::Button(Tr("选择")) || submitted) {
        const auto input = Utf8Path(s_dialogPath.data());
        std::error_code error;
        const auto path = std::filesystem::canonical(Utf8Path(baseDirectory) / input, error);
        const bool exists = !error && (directory ? std::filesystem::is_directory(path, error)
                                                : std::filesystem::is_regular_file(path, error));
        if (input.empty() || !exists) {
            s_dialogError = directory ? "文件夹不存在" : "文件不存在";
        } else if (!directory && !IsAcceptedDrop(Utf8String(path), kind)) {
            s_dialogError = "文件类型不受支持";
        } else {
            selected = Utf8String(path);
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr("取消"))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return selected;
}
#endif

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
    const float browseWidth = ImGui::CalcTextSize(Tr("浏览...")).x + style.FramePadding.x * 2.0f;
    const float clearWidth = ImGui::CalcTextSize(Tr("清除")).x + style.FramePadding.x * 2.0f;
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
                const auto prepared = NormalizeAssetPath(dropped);
                if (!prepared.empty()) {
                    path = prepared;
                    changed = true;
                }
            }
        }
        ImGui::EndDragDropTarget();
    }

    ImGui::SameLine();
#if defined(__linux__) && !defined(__ANDROID__)
    const char* popup = "选择文件###asset-path-dialog";
    const auto& project = ProjectManager::GetInstance();
    if (ImGui::Button(Tr("浏览..."))) {
        const std::string initialPath = path.empty() ? project.GetAssetsDir() : path;
        OpenPathDialog(popup, initialPath);
    }
    const auto selected = RenderPathDialog(popup, project.GetProjectRoot(), false, kind);
    if (!selected.empty()) {
        const auto prepared = NormalizeAssetPath(selected);
        if (!prepared.empty()) {
            path = prepared;
            changed = true;
        }
    }
#else
    if (ImGui::Button(Tr("浏览..."))) {
        const std::string selected = PickAssetPath(kind);
        if (!selected.empty()) {
            path = selected;
            changed = true;
        }
    }
#endif
    if (!path.empty()) {
        ImGui::SameLine();
        if (ImGui::Button(Tr("清除"))) {
            path.clear();
            changed = true;
        }
    }
    ImGui::PopID();
    return changed;
}

} // namespace Editor
