#include "Editor/AssetsWindow.h"
#include "Editor/MaterialEditorWindow.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "TexturePool.h"
#include "EngineGlobal.h"
#include "PreviewGenerator.h"
#include "RenderTarget.h"
#include "Rendering/RendererBase.h"
#include "ECS/SceneECS.h"
#include "SceneSerializer.h"
#include "Core/ProjectManager.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cwchar>
#include <cstring>
#include <fstream>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#endif

namespace Editor {

AssetsWindow& AssetsWindow::GetInstance() {
    static AssetsWindow instance;
    return instance;
}

AssetsWindow::AssetsWindow() 
    : m_folderIconName("folder_icon"),
      m_folderBackIconName("folder_back_icon"),
      m_fileIconName("file_icon"),
      m_materialIconName("material_icon")
{
}

void AssetsWindow::SetIconNames(const std::string& folderIcon, const std::string& folderBackIcon, 
                               const std::string& fileIcon, const std::string& materialIcon) {
    m_folderIconName = folderIcon;
    m_folderBackIconName = folderBackIcon;
    m_fileIconName = fileIcon;
    m_materialIconName = materialIcon;
}

void AssetsWindow::SetAssetsRootPath(const std::string& path) {
    m_assetsRootPath = path;
    m_currentDirectory = path;
    m_selectedAssetPath.clear();
    m_tempSelectedAssetPath.clear();
    m_imagePreviewPath.clear();
    m_showImagePreview = false;
    m_imagePreviewFit = true;
    m_imagePreviewZoom = 1.0f;
    m_imagePreviewPanX = 0.0f;
    m_imagePreviewPanY = 0.0f;
    
    m_rootNode.name = "资源";
    m_rootNode.path = path;
    m_rootNode.expanded = true;
    RefreshAssetTree();
}

void AssetsWindow::RefreshAssetTree() {
    if (m_assetsRootPath.empty()) return;
    m_rootNode.children.clear();
    std::error_code ec;
    if (!std::filesystem::is_directory(std::filesystem::u8path(m_assetsRootPath), ec)) {
        std::cerr << "[AssetsWindow] asset root is not a directory: "
                  << m_assetsRootPath << std::endl;
        return;
    }
    BuildDirectoryTree(m_assetsRootPath, m_rootNode);
}

bool AssetsWindow::IsImageFilePath(const std::string& path) {
    const std::string fileName = std::filesystem::u8path(path).filename().u8string();
    const std::string extension = GetFileExtension(fileName);
    return extension == "png" || extension == "jpg" || extension == "jpeg" ||
           extension == "bmp" || extension == "tga" || extension == "dds";
}

void AssetsWindow::SetImagePreviewPath(const std::string& path) {
    if (!IsImageFilePath(path)) return;

    std::error_code ec;
    if (!std::filesystem::is_regular_file(std::filesystem::u8path(path), ec)) {
        return;
    }

    if (m_imagePreviewPath != path) {
        m_imagePreviewPath = path;
        m_imagePreviewFit = true;
        m_imagePreviewZoom = 1.0f;
        m_imagePreviewPanX = 0.0f;
        m_imagePreviewPanY = 0.0f;
    }
    m_showImagePreview = true;
}

void AssetsWindow::RenderImagePreviewWindow() {
    if (!m_showImagePreview) return;

    ImGui::SetNextWindowSize(ImVec2(640.0f, 520.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("图片预览", &m_showImagePreview)) {
        ImGui::End();
        return;
    }

    ImGui::TextUnformatted("将资源窗口中的图片拖到预览区，或单击图片自动预览");
    ImGui::Separator();

    if (!m_imagePreviewPath.empty()) {
        ImGui::TextWrapped("文件：%s", m_imagePreviewPath.c_str());
    } else {
        ImGui::TextDisabled("当前未选择图片");
    }

    if (ImGui::Button("适应窗口")) {
        m_imagePreviewFit = true;
        m_imagePreviewPanX = 0.0f;
        m_imagePreviewPanY = 0.0f;
    }
    ImGui::SameLine();
    if (ImGui::Button("100%")) {
        m_imagePreviewFit = false;
        m_imagePreviewZoom = 1.0f;
        m_imagePreviewPanX = 0.0f;
        m_imagePreviewPanY = 0.0f;
    }
    ImGui::SameLine();
    if (ImGui::Button("清除")) {
        m_imagePreviewPath.clear();
        m_imagePreviewFit = true;
        m_imagePreviewZoom = 1.0f;
        m_imagePreviewPanX = 0.0f;
        m_imagePreviewPanY = 0.0f;
    }

    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    const TextureInfo* textureInfo = nullptr;
    if (!m_imagePreviewPath.empty() && m_TexturePool) {
        descriptorSet = m_TexturePool->GetDescriptorSet(m_imagePreviewPath);
        if (descriptorSet == VK_NULL_HANDLE) {
            m_TexturePool->LoadTexture2D(m_imagePreviewPath, m_imagePreviewPath);
            descriptorSet = m_TexturePool->GetDescriptorSet(m_imagePreviewPath);
        }
        textureInfo = m_TexturePool->GetTexture(m_imagePreviewPath);
    }

    if (textureInfo && textureInfo->width > 0 && textureInfo->height > 0) {
        ImGui::SameLine();
        ImGui::Text("尺寸：%u x %u", textureInfo->width, textureInfo->height);
        ImGui::SameLine();
        ImGui::TextDisabled(m_imagePreviewFit ? "缩放：适应窗口" : "滚轮缩放 / 中键平移");
    }

    ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    canvasSize.x = std::max(canvasSize.x, 64.0f);
    canvasSize.y = std::max(canvasSize.y, 64.0f);
    ImGui::BeginChild("ImagePreviewCanvas", canvasSize, true, ImGuiWindowFlags_NoScrollWithMouse);

    const ImVec2 canvasMin = ImGui::GetCursorScreenPos();
    const ImVec2 canvasExtent = ImGui::GetContentRegionAvail();
    const ImVec2 canvasMax(canvasMin.x + canvasExtent.x, canvasMin.y + canvasExtent.y);
    ImGui::InvisibleButton("ImagePreviewDropTarget", canvasExtent);
    const bool canvasHovered = ImGui::IsItemHovered();

    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_ITEM")) {
            if (payload->Data && payload->DataSize > 0) {
                SetImagePreviewPath(static_cast<const char*>(payload->Data));
            }
        }
        ImGui::EndDragDropTarget();
    }

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(canvasMin, canvasMax, IM_COL32(24, 24, 28, 255));

    const bool hasImage = textureInfo && descriptorSet != VK_NULL_HANDLE &&
                          textureInfo->width > 0 && textureInfo->height > 0;
    if (hasImage) {
        const float fitScale = std::min(
            canvasExtent.x / static_cast<float>(textureInfo->width),
            canvasExtent.y / static_cast<float>(textureInfo->height));

        if (canvasHovered && ImGui::GetIO().MouseWheel != 0.0f) {
            if (m_imagePreviewFit) {
                m_imagePreviewFit = false;
                m_imagePreviewZoom = std::max(fitScale, 0.05f);
            }
            const float zoomFactor = ImGui::GetIO().MouseWheel > 0.0f ? 1.15f : 0.87f;
            m_imagePreviewZoom = std::clamp(m_imagePreviewZoom * zoomFactor, 0.05f, 16.0f);
        }

        if (canvasHovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
            if (m_imagePreviewFit) {
                m_imagePreviewFit = false;
                m_imagePreviewZoom = std::max(fitScale, 0.05f);
            }
            m_imagePreviewPanX += ImGui::GetIO().MouseDelta.x;
            m_imagePreviewPanY += ImGui::GetIO().MouseDelta.y;
        }

        const float scale = m_imagePreviewFit ? fitScale : m_imagePreviewZoom;
        const ImVec2 imageSize(
            textureInfo->width * scale,
            textureInfo->height * scale);
        const ImVec2 canvasCenter(
            (canvasMin.x + canvasMax.x) * 0.5f + m_imagePreviewPanX,
            (canvasMin.y + canvasMax.y) * 0.5f + m_imagePreviewPanY);
        const ImVec2 imageMin(
            canvasCenter.x - imageSize.x * 0.5f,
            canvasCenter.y - imageSize.y * 0.5f);
        const ImVec2 imageMax(imageMin.x + imageSize.x, imageMin.y + imageSize.y);

        drawList->AddImage(
            (ImTextureID)descriptorSet,
            imageMin,
            imageMax,
            ImVec2(0.0f, 1.0f),
            ImVec2(1.0f, 0.0f));
        drawList->AddRect(imageMin, imageMax, IM_COL32(160, 160, 170, 180));
    } else {
        const char* message = m_imagePreviewPath.empty()
            ? "把图片拖到这里"
            : "图片无法加载，或当前纹理系统不可用";
        const ImVec2 textSize = ImGui::CalcTextSize(message);
        drawList->AddText(
            ImVec2(
                (canvasMin.x + canvasMax.x - textSize.x) * 0.5f,
                (canvasMin.y + canvasMax.y - textSize.y) * 0.5f),
            ImGui::GetColorU32(ImGuiCol_TextDisabled),
            message);
    }

    ImGui::EndChild();
    ImGui::End();
}

void AssetsWindow::Render(bool& showWindow) {
    if (!showWindow) return;
    
    ImGui::Begin("资源", &showWindow);
    
    ImGui::Columns(2, "AssetsColumns", false);
    // 左侧目录树列宽：原 150px 过窄，长目录名被截断；加宽到 240px 让名字完整显示。
    ImGui::SetColumnWidth(0, 240);
    
    // 左侧目录树边框加粗：BeginChild 第三参 true 显示边框，宽度由 ChildBorderSize 控制（默认 1.0f 偏细）。
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 2.0f);
    ImGui::BeginChild("DirectoryTree", ImVec2(0, 0), true);
    RenderDirectoryTreeNode(m_rootNode);
    ImGui::EndChild();
    ImGui::PopStyleVar();
    
    ImGui::NextColumn();
    
    ImGui::BeginChild("FileList", ImVec2(0, 0), true);

    // 路径文本 + 新建/刷新按钮同一行：路径在左，按钮靠右对齐
    ImGui::Text("路径：%s", m_currentDirectory.c_str());
    const float buttonSpacing = 6.0f;
    const float refreshBtnW = 60.0f;   // "刷新"按钮大致宽度
    const float importBtnW = 60.0f;    // "导入"按钮大致宽度
    const float createBtnW = 60.0f;    // "新建"按钮大致宽度
    float availX = ImGui::GetContentRegionAvail().x;
    float cursorX = ImGui::GetCursorPosX() + availX -
        (refreshBtnW + importBtnW + createBtnW + buttonSpacing * 2.0f);
    ImGui::SameLine(cursorX);
    if (ImGui::Button("新建")) {
        ImGui::OpenPopup("NewAssetMenu");
    }
    ImGui::SameLine(0, buttonSpacing);
    if (ImGui::Button("导入")) {
        ImportFiles();
    }
    ImGui::SameLine(0, buttonSpacing);
    if (ImGui::Button("刷新")) {
        if (ProjectManager::GetInstance().IsManifestProject()) {
            ProjectManager::GetInstance().ReloadManifest();
        }
        RefreshAssetTree();
        CleanupExpiredTextureCache();
    }
    // “新建”下拉浮窗（类似右键菜单）：在当前目录新建文件夹 / 文本文件 / 材质
    if (ImGui::BeginPopup("NewAssetMenu")) {
        if (ImGui::MenuItem("新建文件夹")) {
            CreateNewFolder(m_currentDirectory);
        }
        if (ImGui::MenuItem("新建文本文件")) {
            CreateNewTextFile(m_currentDirectory);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("新建材质")) {
            MaterialEditorWindow::GetInstance().OpenNewMaterial();
        }
        ImGui::EndPopup();
    }
    ImGui::Separator();
    
    static std::string s_lastDirectory = "";
    if (!s_lastDirectory.empty() && s_lastDirectory != m_currentDirectory) {
        CleanupTextureCacheForDirectory(s_lastDirectory);
        printf("[TextureCache] Directory changed from '%s' to '%s', cleaned up old cache", 
              s_lastDirectory.c_str(), m_currentDirectory.c_str());
        
        CleanupExpiredTextureCache();
        
        if (m_TextureDescriptorCache.size() > 100) {
            printf("[TextureCache] Cache size is %d, forcing descriptor pool reset", 
                  m_TextureDescriptorCache.size());
            if (m_TexturePool) {
                m_TexturePool->ResetDescriptorPool();
            }
            m_TextureDescriptorCache.clear();
            printf("[TextureCache] Cache cleared, will reload on demand");
        }
    }
    s_lastDirectory = m_currentDirectory;
    
    std::vector<AssetItem> items = ScanDirectoryFiles(m_currentDirectory);
    
    float cellSize = 100.0f;
    float padding = 12.0f;
    float panelWidth = ImGui::GetContentRegionAvail().x;
    int columnCount = (int)(panelWidth / (cellSize + padding));
    if (columnCount < 1) columnCount = 1;
    
    ImGui::Columns(columnCount, nullptr, false);
    
    for (auto& item : items) {
        ImGui::PushID(item.path.c_str());
        
        ImVec2 buttonSize(cellSize, cellSize);
        
        VkDescriptorSet iconDescriptorSet = VK_NULL_HANDLE;
        if (item.isDirectory) {
            if (item.name == "../") {
                iconDescriptorSet = m_TexturePool ? m_TexturePool->GetDescriptorSet(m_folderBackIconName) : VK_NULL_HANDLE;
            } else {
                iconDescriptorSet = m_TexturePool ? m_TexturePool->GetDescriptorSet(m_folderIconName) : VK_NULL_HANDLE;
            }
        } else if (item.isImage) {
            iconDescriptorSet = m_TexturePool ? m_TexturePool->GetDescriptorSet(item.path) : VK_NULL_HANDLE;
            if (iconDescriptorSet == VK_NULL_HANDLE && m_TexturePool) {
                m_TexturePool->LoadTexture2D(item.path, item.path);
                iconDescriptorSet = m_TexturePool->GetDescriptorSet(item.path);
            }
        } else if (item.isMaterial) {
            std::filesystem::path materialPath = std::filesystem::u8path(item.path);
            std::filesystem::path metaDir = materialPath.parent_path() / ".meta";
            std::string fileNameWithoutExt = materialPath.stem().u8string();
            std::filesystem::path previewPath = metaDir / (fileNameWithoutExt + "_preview.png");
            
            if (std::filesystem::exists(previewPath)) {
                const std::string previewPathUtf8 = previewPath.u8string();
                iconDescriptorSet = m_TexturePool ? m_TexturePool->GetDescriptorSet(previewPathUtf8) : VK_NULL_HANDLE;
                if (iconDescriptorSet == VK_NULL_HANDLE && m_TexturePool) {
                    m_TexturePool->LoadTexture2D(previewPathUtf8, previewPathUtf8);
                    iconDescriptorSet = m_TexturePool->GetDescriptorSet(previewPathUtf8);
                }
            }
            
            if (iconDescriptorSet == VK_NULL_HANDLE) {
                iconDescriptorSet = m_TexturePool ? m_TexturePool->GetDescriptorSet(m_materialIconName) : VK_NULL_HANDLE;
            }
        } else {
            std::string fileExt = GetFileExtension(item.name);
            bool isModelFile = (fileExt == "gltf" || fileExt == "glb" || 
                              fileExt == "obj" || fileExt == "fbx" || fileExt == "dae");
            bool isVoxFile = (fileExt == "vox");
            
            if (isModelFile || isVoxFile) {
                std::filesystem::path filePath = std::filesystem::u8path(item.path);
                std::filesystem::path metaDir = filePath.parent_path() / ".meta";
                std::string fileNameWithoutExt = filePath.stem().u8string();
                std::filesystem::path previewPath = metaDir / (fileNameWithoutExt + "_preview.png");
                
                if (std::filesystem::exists(previewPath)) {
                    const std::string previewPathUtf8 = previewPath.u8string();
                    iconDescriptorSet = m_TexturePool ? m_TexturePool->GetDescriptorSet(previewPathUtf8) : VK_NULL_HANDLE;
                    if (iconDescriptorSet == VK_NULL_HANDLE && m_TexturePool) {
                        m_TexturePool->LoadTexture2D(previewPathUtf8, previewPathUtf8);
                        iconDescriptorSet = m_TexturePool->GetDescriptorSet(previewPathUtf8);
                    }
                }
            }
            
            if (iconDescriptorSet == VK_NULL_HANDLE) {
                iconDescriptorSet = m_TexturePool ? m_TexturePool->GetDescriptorSet(m_fileIconName) : VK_NULL_HANDLE;
            }
        }
        
        bool buttonClicked = false;
        
        if (iconDescriptorSet != VK_NULL_HANDLE && m_TexturePool) {
            ImVec4 bgColor = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
            ImVec4 tintColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
            
            // 缩略图略微缩小并居中显示：图片与按钮边缘留出边距，避免贴边
            const float iconInset = 6.0f;
            ImVec2 iconSize(buttonSize.x - iconInset * 2.0f, buttonSize.y - iconInset * 2.0f);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + iconInset);
            
            // 悬浮/选中状态不使用背景填充（避免圆角填充遮挡缩略图边缘），改用细边框指示
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            
            buttonClicked = ImGui::ImageButton("", (ImTextureID)iconDescriptorSet, iconSize, ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f), bgColor, tintColor);
            
            ImGui::PopStyleColor(3);
            
            // 悬浮/选中指示：无背景填充、无边框，仅轻微提亮图片，与主题统一且不遮挡图片
            if (ImGui::IsItemHovered() || item.path == m_selectedAssetPath) {
                ImVec2 itemMin = ImGui::GetItemRectMin();
                ImVec2 itemMax = ImGui::GetItemRectMax();
                ImVec2 center((itemMin.x + itemMax.x) * 0.5f, (itemMin.y + itemMax.y) * 0.5f);
                ImVec2 rectMin(center.x - iconSize.x * 0.5f, center.y - iconSize.y * 0.5f);
                ImVec2 rectMax(center.x + iconSize.x * 0.5f, center.y + iconSize.y * 0.5f);
                // 轻微提亮图片（半透明白色叠加），让悬浮/选中状态更自然
                ImGui::GetWindowDrawList()->AddRectFilled(rectMin, rectMax, IM_COL32(255, 255, 255, 36), 4.0f);
            }
        } else {
            const char* icon = item.isDirectory ? "[文件夹]" : 
                              item.isImage ? "[图片]" : 
                              item.isMaterial ? "[材质]" : "[文件]";
            
            buttonClicked = ImGui::Button(icon, buttonSize);
        }
        
        if (buttonClicked) {
            float currentTime = ImGui::GetTime();
            
            if (item.path == m_tempSelectedAssetPath && 
                (currentTime - m_lastClickTime) < DOUBLE_CLICK_THRESHOLD) {
                if (item.isDirectory) {
                    std::filesystem::path newPath = std::filesystem::canonical(item.path);
                    std::filesystem::path rootPath = std::filesystem::canonical(m_assetsRootPath);
                    
                    bool isInAssetsRoot = false;
                    std::filesystem::path tempPath = newPath;
                    while (!tempPath.empty()) {
                        if (tempPath == rootPath) {
                            isInAssetsRoot = true;
                            break;
                        }
                        tempPath = tempPath.parent_path();
                    }
                    
                    if (isInAssetsRoot) {
                        m_currentDirectory = item.path;
                        m_selectedAssetPath = item.path;
                        m_tempSelectedAssetPath = item.path;
                    }
                } else {
                    std::string fileExt = GetFileExtension(item.name);
                    if (fileExt == "material") {
                        EditMaterial(item.path);
                    } else if (item.name.size() > 11 &&
                               item.name.compare(item.name.size() - 11, 11, ".prefab.json") == 0) {
                        // 预制体：双击实例化到场景（根实体，选中它方便查看/移动）
                        ECS::SceneSerializer serializer;
                        ECS::Entity root = serializer.InstantiatePrefab(item.path);
                        if (root != ECS::INVALID_ENTITY) {
                            ECS::SceneECS::GetInstance().SetSelectedEntity(root);
                            printf("[AssetsWindow] instantiated prefab %s (root=%u)\n", item.name.c_str(), root);
                        }
                    } else if (fileExt == "cpp" || fileExt == "h" || fileExt == "hpp" ||
                               fileExt == "hxx" || fileExt == "c" || fileExt == "cs") {
                        // 源码/脚本：双击打开外部 IDE（系统默认关联：VS Code / Visual Studio 等）
#ifdef _WIN32
                        ShellExecuteA(nullptr, "open", item.path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                        printf("[AssetsWindow] open in external editor: %s\n", item.path.c_str());
#endif
                    }
                    m_selectedAssetPath = item.path;
                    m_tempSelectedAssetPath = item.path;
                }
                m_isProcessingDoubleClick = true;
            } else {
                m_tempSelectedAssetPath = item.path;
                m_selectedAssetPath = item.path;
                m_lastClickTime = currentTime;
                m_isProcessingDoubleClick = false;
            }
        }

        if (buttonClicked && item.isImage) {
            SetImagePreviewPath(item.path);
        }
        
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
            std::string dragData = item.path;
            ImGui::SetDragDropPayload("ASSET_ITEM", dragData.c_str(), dragData.size() + 1);
            ImGui::Text("拖拽：%s", item.name.c_str());
            ImGui::EndDragDropSource();
        }
        
        if (ImGui::BeginPopupContextItem()) {
            ShowAssetContextMenu(item.path, item.isDirectory);
            ImGui::EndPopup();
        }
        
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            if (item.isDirectory) {
                std::filesystem::path newPath = std::filesystem::canonical(item.path);
                std::filesystem::path rootPath = std::filesystem::canonical(m_assetsRootPath);
                
                bool isInAssetsRoot = false;
                std::filesystem::path tempPath = newPath;
                while (!tempPath.empty()) {
                    if (tempPath == rootPath) {
                        isInAssetsRoot = true;
                        break;
                    }
                    tempPath = tempPath.parent_path();
                }
                
                if (isInAssetsRoot) {
                    m_currentDirectory = item.path;
                    m_selectedAssetPath = item.path;
                    m_tempSelectedAssetPath = item.path;
                }
            }
        }
        
        std::string displayName = item.name;
        if (displayName.length() > 12) {
            displayName = displayName.substr(0, 9) + "...";
        }
        
        ImVec2 textSize = ImGui::CalcTextSize(displayName.c_str());
        float textOffset = (cellSize - textSize.x) * 0.5f;
        if (textOffset < 0) textOffset = 0;
        
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + textOffset);
        ImGui::TextUnformatted(displayName.c_str());
        
        ImGui::NextColumn();
        ImGui::PopID();
    }
    
    ImGui::Columns(1);
    ImGui::EndChild();
    
    ImGui::Columns(1);
    
    if (m_showRenamePopup) {
        ImGui::OpenPopup("重命名");
        m_showRenamePopup = false;
    }
    
    if (ImGui::BeginPopupModal("重命名", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("新名称:");
        ImGui::InputText("##rename", m_renameBuffer, sizeof(m_renameBuffer));
        
        if (ImGui::Button("确定", ImVec2(120, 0))) {
            if (strlen(m_renameBuffer) > 0) {
                RenameFileOrDirectory(m_contextMenuTargetPath, m_renameBuffer);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("取消", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    
    ImGui::End();
    RenderImagePreviewWindow();
}

std::vector<AssetItem> AssetsWindow::ScanDirectoryFiles(const std::string& path) {
    std::vector<AssetItem> items;
    
    try {
        std::filesystem::path currentPath =
            std::filesystem::weakly_canonical(std::filesystem::u8path(path));
        std::filesystem::path rootPath =
            std::filesystem::weakly_canonical(std::filesystem::u8path(m_assetsRootPath));
        
        bool isInAssetsRoot = false;
        std::filesystem::path tempPath = currentPath;
        while (!tempPath.empty()) {
            if (tempPath == rootPath) {
                isInAssetsRoot = true;
                break;
            }
            tempPath = tempPath.parent_path();
        }
        
        if (isInAssetsRoot && currentPath != rootPath) {
            AssetItem parentItem;
            parentItem.name = "../";
            parentItem.path = currentPath.parent_path().u8string();
            parentItem.isDirectory = true;
            parentItem.isImage = false;
            items.push_back(parentItem);
        }
        
        size_t scanFailCount = 0;
        std::vector<std::string> scanFailNames;
        for (const auto& entry :
             std::filesystem::directory_iterator(std::filesystem::u8path(path))) {
            try {
                AssetItem item;
                item.name = entry.path().filename().u8string();   // UTF-8 文件名（imbue 后 string() 同为 UTF-8）
                item.path = entry.path().u8string();
                item.isDirectory = entry.is_directory();
                item.extension = GetFileExtension(item.name);
                item.isImage = false;
                
                if (item.name.empty() || item.name[0] == '.' || 
                    item.name.find("_thumbnail") != std::string::npos ||
                    item.name.find("_bvh.txt") != std::string::npos ||
                    item.extension == "mtl") {
                    continue;
                }

                // 项目化项目只展示 project.json.assets[] 白名单中的文件，
                // 以及通往白名单文件的目录；不再递归扫描整个项目目录。
                if (!ProjectManager::GetInstance().IsProjectAsset(
                        item.path, item.isDirectory)) {
                    continue;
                }
                
                if (!item.isDirectory) {
                    if (item.extension == "png" || item.extension == "jpg" ||
                        item.extension == "jpeg" || item.extension == "bmp" ||
                        item.extension == "tga" || item.extension == "dds") {
                        item.isImage = true;
                    } else if (item.extension == "material") {
                        item.isMaterial = true;
                    }
                }
                
                items.push_back(item);
            } catch (const std::exception& e) {
                // 无法转码的条目跳过（UTF-8 locale 下极少发生），汇总一次提示避免刷屏
                if (scanFailCount < 3) scanFailNames.push_back(entry.path().filename().u8string());
                ++scanFailCount;
            }
        }
        if (scanFailCount > 0) {
            std::cerr << "[AssetsWindow] skipped " << scanFailCount << " file(s) with undecodable names"
                      << (scanFailNames.empty() ? "" : (std::string(" (e.g. '") + scanFailNames[0] + "')").c_str())
                      << std::endl;
        }
        
        std::sort(items.begin(), items.end(), [](const AssetItem& a, const AssetItem& b) {
            if (a.isDirectory != b.isDirectory) {
                return a.isDirectory > b.isDirectory;
            }
            return a.name < b.name;
        });
        
    } catch (const std::exception& e) {
        std::cerr << "Error scanning directory: " << e.what() << std::endl;
    }
    
    return items;
}

void AssetsWindow::BuildDirectoryTree(const std::string& basePath, DirectoryNode& node) {
    try {
        for (const auto& entry :
             std::filesystem::directory_iterator(std::filesystem::u8path(basePath))) {
            if (entry.is_directory()) {
                DirectoryNode child;
                child.name = entry.path().filename().u8string();   // UTF-8（imbue 后 string() 同为 UTF-8）
                child.path = entry.path().u8string();
                if (child.name.empty() || child.name[0] == '.' ||
                    !ProjectManager::GetInstance().IsProjectAsset(child.path, true)) {
                    continue;
                }
                child.expanded = false;
                BuildDirectoryTree(child.path, child);
                node.children.push_back(child);
            }
        }
        
        std::sort(node.children.begin(), node.children.end(), [](const DirectoryNode& a, const DirectoryNode& b) {
            return a.name < b.name;
        });
    } catch (const std::exception& e) {
        std::cerr << "Error building directory tree: " << e.what() << std::endl;
    }
}

std::string AssetsWindow::GetFileExtension(const std::string& filename) {
    size_t dotPos = filename.find_last_of('.');
    if (dotPos != std::string::npos && dotPos < filename.length() - 1) {
        std::string ext = filename.substr(dotPos + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        return ext;
    }
    return "";
}

void AssetsWindow::RenderDirectoryTreeNode(DirectoryNode& node) {
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | 
                               ImGuiTreeNodeFlags_OpenOnDoubleClick;
    
    if (node.children.empty()) {
        flags |= ImGuiTreeNodeFlags_Leaf;
    }
    
    if (node.path == m_currentDirectory) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }
    
    bool nodeOpen = ImGui::TreeNodeEx(node.name.c_str(), flags);
    
    if (ImGui::IsItemClicked()) {
        m_currentDirectory = node.path;
    }
    
    if (nodeOpen) {
        for (auto& child : node.children) {
            RenderDirectoryTreeNode(child);
        }
        ImGui::TreePop();
    }
}

void AssetsWindow::ShowAssetContextMenu(const std::string& path, bool isDirectory) {
    m_contextMenuTargetPath = path;
    
    if (ImGui::MenuItem("复制")) {
        m_clipboardPath = path;
        m_isCutOperation = false;
    }
    
    if (ImGui::MenuItem("剪切")) {
        m_clipboardPath = path;
        m_isCutOperation = true;
    }
    
    bool canPaste = !m_clipboardPath.empty() && isDirectory;
    if (ImGui::MenuItem("粘贴", nullptr, false, canPaste)) {
        PasteFileOrDirectory(path);
    }
    
    ImGui::Separator();
    
    if (ImGui::MenuItem("重命名")) {
        m_showRenamePopup = true;
        std::string currentName =
            std::filesystem::u8path(path).filename().u8string();
        strncpy(m_renameBuffer, currentName.c_str(), sizeof(m_renameBuffer) - 1);
        m_renameBuffer[sizeof(m_renameBuffer) - 1] = '\0';
    }
    
    if (ImGui::MenuItem("删除")) {
        DeleteFileOrDirectory(path);
    }
    
    if (!isDirectory) {
        std::string fileExt = GetFileExtension(
            std::filesystem::u8path(path).filename().u8string());

        if (IsImageFilePath(path)) {
            ImGui::Separator();
            if (ImGui::MenuItem("预览图片")) {
                SetImagePreviewPath(path);
            }
        }

        bool isModelFile = (fileExt == "gltf" || fileExt == "glb" || 
                          fileExt == "obj" || fileExt == "fbx" || fileExt == "dae");
        bool isVoxFile = (fileExt == "vox");
        
        if (isModelFile || isVoxFile) {
            ImGui::Separator();
            if (ImGui::MenuItem("生成预览图")) {
                if (isVoxFile) {
                    GenerateVoxPreview(path);
                } else {
                    GenerateModelPreview(path);
                }
            }
        }
    }
}

std::vector<std::string> AssetsWindow::OpenImportFileDialog() const {
    std::vector<std::string> paths;
#ifdef _WIN32
    // 使用宽字符 API，避免中文项目路径经过系统 ANSI 代码页后失效。
    wchar_t buffer[32768] = {};
    const wchar_t filter[] = L"All Files\0*.*\0\0";
    std::wstring initialDirectory;
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0]));
    ofn.lpstrFilter = filter;
    ofn.nFilterIndex = 1;
    if (!m_currentDirectory.empty()) {
        initialDirectory = std::filesystem::u8path(m_currentDirectory).wstring();
        ofn.lpstrInitialDir = initialDirectory.c_str();
    }
    ofn.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST |
                OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;

    if (!GetOpenFileNameW(&ofn)) return paths;

    const wchar_t* first = buffer;
    const size_t firstLength = std::wcslen(first);
    const wchar_t* next = first + firstLength + 1;
    if (*next == L'\0') {
        paths.push_back(std::filesystem::path(first).u8string());
        return paths;
    }

    const std::filesystem::path directory(first);
    for (const wchar_t* name = next; *name != L'\0';
         name += std::wcslen(name) + 1) {
        paths.push_back((directory / std::filesystem::path(name)).u8string());
    }
#else
    std::cerr << "[AssetsWindow] importing files is currently supported on Windows only"
              << std::endl;
#endif
    return paths;
}

void AssetsWindow::ImportFiles() {
    const std::vector<std::string> sources = OpenImportFileDialog();
    if (sources.empty()) return;

    auto& projectManager = ProjectManager::GetInstance();
    if (!projectManager.IsProjectAsset(m_currentDirectory, true)) {
        std::cerr << "[AssetsWindow] current directory is outside the project asset scope: "
                  << m_currentDirectory << std::endl;
        return;
    }

    const std::filesystem::path targetDirectory =
        std::filesystem::u8path(m_currentDirectory);
    size_t importedCount = 0;
    for (const auto& source : sources) {
        const std::filesystem::path sourcePath =
            std::filesystem::u8path(source);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(sourcePath, ec)) {
            std::cerr << "[AssetsWindow] skipped non-file import: " << source << std::endl;
            continue;
        }

        const std::string sourceName = sourcePath.filename().u8string();
        const std::string uniqueName =
            GetUniqueAssetName(m_currentDirectory, sourceName, false);
        const std::filesystem::path destination =
            targetDirectory / std::filesystem::u8path(uniqueName);

        std::filesystem::copy_file(sourcePath, destination,
                                   std::filesystem::copy_options::none, ec);
        if (ec) {
            std::cerr << "[AssetsWindow] import failed: " << source
                      << " -> " << destination.u8string()
                      << " (" << ec.message() << ")" << std::endl;
            continue;
        }

        if (!projectManager.RegisterProjectAsset(destination.u8string())) {
            std::filesystem::remove(destination, ec);
            std::cerr << "[AssetsWindow] imported file is outside the project manifest: "
                      << destination.u8string() << std::endl;
            continue;
        }

        m_selectedAssetPath = destination.u8string();
        m_tempSelectedAssetPath = m_selectedAssetPath;
        ++importedCount;
        std::cout << "[AssetsWindow] imported: " << source
                  << " -> " << destination.u8string() << std::endl;
    }

    if (importedCount > 0) {
        RefreshAssetTree();
        CleanupExpiredTextureCache();
    }
}

void AssetsWindow::CopyFileOrDirectory(const std::string& src, const std::string& dst) {
    try {
        const std::filesystem::path sourcePath = std::filesystem::u8path(src);
        const std::filesystem::path destinationPath = std::filesystem::u8path(dst);
        if (std::filesystem::is_directory(sourcePath)) {
            std::filesystem::copy(sourcePath, destinationPath,
                                  std::filesystem::copy_options::recursive);
        } else {
            std::filesystem::copy(sourcePath, destinationPath);
        }
    } catch (const std::exception& e) {
        printf("Copy failed: %s\n", e.what());
    }
}

void AssetsWindow::DeleteFileOrDirectory(const std::string& path) {
    const std::filesystem::path targetPath =
        std::filesystem::u8path(path);
    const std::filesystem::path rootPath =
        std::filesystem::u8path(m_assetsRootPath);
    std::error_code ec;
    const std::filesystem::path targetCanonical =
        std::filesystem::weakly_canonical(targetPath, ec);
    if (ec) return;
    ec.clear();
    const std::filesystem::path rootCanonical =
        std::filesystem::weakly_canonical(rootPath, ec);
    if (ec) return;
    ec.clear();
    const bool targetIsDirectory = std::filesystem::is_directory(targetPath, ec);
    if (ec || targetCanonical == rootCanonical ||
        !ProjectManager::GetInstance().IsProjectAsset(path, targetIsDirectory)) {
        std::cerr << "[AssetsWindow] refused to delete outside asset scope: "
                  << path << std::endl;
        return;
    }

    try {
        CleanupTextureCacheForDirectory(path);
        const auto removed = std::filesystem::remove_all(targetPath);
        if (removed > 0 &&
            !ProjectManager::GetInstance().UnregisterProjectAsset(path)) {
            std::cerr << "[AssetsWindow] deleted file but failed to update project manifest: "
                      << path << std::endl;
        }
        RefreshAssetTree();
    } catch (const std::exception& e) {
        printf("Delete failed: %s\n", e.what());
    }
}

std::string AssetsWindow::GetUniqueAssetName(const std::string& dir, const std::string& baseName, bool isFolder) {
    // 生成不冲突的名字：重名时追加 " (2)"、" (3)"…
    // 注意: baseName 是 UTF-8(源码字面量/ImGui 输入)，必须用 u8path 构造，否则中文名会按 ANSI 代码页解码抛异常
    std::filesystem::path dirPath = std::filesystem::u8path(dir);
    std::string candidate = baseName;
    int suffix = 2;
    while (std::filesystem::exists(dirPath / std::filesystem::u8path(candidate))) {
        if (isFolder) {
            candidate = baseName + " (" + std::to_string(suffix) + ")";
        } else {
            std::filesystem::path p = std::filesystem::u8path(baseName);
            candidate = p.stem().u8string() + " (" + std::to_string(suffix) + ")" +
                        p.extension().u8string();
        }
        suffix++;
    }
    return candidate;
}

void AssetsWindow::CreateNewFolder(const std::string& dir) {
    try {
        std::string name = GetUniqueAssetName(dir, "新建文件夹", true);
        std::filesystem::path newPath =
            std::filesystem::u8path(dir) / std::filesystem::u8path(name);
        if (std::filesystem::create_directory(newPath)) {
            if (!ProjectManager::GetInstance().RegisterProjectAsset(newPath.u8string())) {
                std::filesystem::remove(newPath);
                printf("[Assets] Create folder FAILED (manifest update): %s\n",
                       newPath.u8string().c_str());
                return;
            }
            printf("[Assets] Created folder: %s\n", newPath.u8string().c_str());
            // 进入重命名阶段：预填默认名，立即弹出重命名对话框等待用户输入
            m_contextMenuTargetPath = newPath.u8string();
            strncpy(m_renameBuffer, name.c_str(), sizeof(m_renameBuffer) - 1);
            m_renameBuffer[sizeof(m_renameBuffer) - 1] = '\0';
            m_showRenamePopup = true;
        } else {
            printf("[Assets] Create folder FAILED (exists?): %s\n", newPath.u8string().c_str());
        }
        RefreshAssetTree();
    } catch (const std::exception& e) {
        printf("Create folder failed: %s\n", e.what());
    }
}

void AssetsWindow::CreateNewTextFile(const std::string& dir) {
    try {
        std::string name = GetUniqueAssetName(dir, "新建文本文件.txt", false);
        std::filesystem::path newPath =
            std::filesystem::u8path(dir) / std::filesystem::u8path(name);
        std::ofstream ofs(newPath, std::ios::out);
        if (ofs) {
            ofs.close();
            if (!ProjectManager::GetInstance().RegisterProjectAsset(newPath.u8string())) {
                std::error_code ec;
                std::filesystem::remove(newPath, ec);
                printf("[Assets] Create text file FAILED (manifest update): %s\n",
                       newPath.u8string().c_str());
                return;
            }
            printf("[Assets] Created text file: %s\n", newPath.u8string().c_str());
            // 进入重命名阶段：预填默认名，立即弹出重命名对话框等待用户输入
            m_contextMenuTargetPath = newPath.u8string();
            strncpy(m_renameBuffer, name.c_str(), sizeof(m_renameBuffer) - 1);
            m_renameBuffer[sizeof(m_renameBuffer) - 1] = '\0';
            m_showRenamePopup = true;
        } else {
            printf("[Assets] Create text file FAILED: %s\n", newPath.u8string().c_str());
        }
        RefreshAssetTree();
    } catch (const std::exception& e) {
        printf("Create text file failed: %s\n", e.what());
    }
}

void AssetsWindow::RenameFileOrDirectory(const std::string& oldPath, const std::string& newName) {
    if (newName.empty() || newName == "." || newName == ".." ||
        newName.find('/') != std::string::npos ||
        newName.find('\\') != std::string::npos) {
        printf("Rename failed: invalid asset name\n");
        return;
    }

    try {
        const std::filesystem::path oldPathObj =
            std::filesystem::u8path(oldPath);
        const bool isDirectory = std::filesystem::is_directory(oldPathObj);
        if (!ProjectManager::GetInstance().IsProjectAsset(oldPath, isDirectory)) {
            printf("Rename failed: asset is outside project scope\n");
            return;
        }
        // newName 来自 ImGui 输入(UTF-8)，必须用 u8path，否则中文名会按 ANSI 代码页解码抛异常
        std::filesystem::path newPath = oldPathObj.parent_path() / std::filesystem::u8path(newName);
        if (ProjectManager::GetInstance().GetProjectRelativePath(
                newPath.u8string()).empty()) {
            printf("Rename failed: destination is outside project scope\n");
            return;
        }
        if (std::filesystem::exists(newPath)) {
            printf("Rename failed: destination already exists\n");
            return;
        }
        std::filesystem::rename(oldPathObj, newPath);
        if (!ProjectManager::GetInstance().RenameProjectAsset(
                oldPath, newPath.u8string())) {
            std::error_code rollbackError;
            std::filesystem::rename(newPath, oldPathObj, rollbackError);
            printf("Rename failed: project manifest update failed\n");
            return;
        }
        if (m_selectedAssetPath == oldPath) {
            m_selectedAssetPath = newPath.u8string();
        }
        if (m_tempSelectedAssetPath == oldPath) {
            m_tempSelectedAssetPath = newPath.u8string();
        }
        m_contextMenuTargetPath = newPath.u8string();
        RefreshAssetTree();
    } catch (const std::exception& e) {
        printf("Rename failed: %s\n", e.what());
    }
}

void AssetsWindow::PasteFileOrDirectory(const std::string& targetDir) {
    if (m_clipboardPath.empty()) return;
    
    try {
        const std::filesystem::path srcPath =
            std::filesystem::u8path(m_clipboardPath);
        const bool sourceIsDirectory = std::filesystem::is_directory(srcPath);
        if (!ProjectManager::GetInstance().IsProjectAsset(
                m_clipboardPath, sourceIsDirectory) ||
            !ProjectManager::GetInstance().IsProjectAsset(targetDir, true)) {
            printf("Paste failed: source or target is outside project scope\n");
            return;
        }

        const std::string fileName =
            GetUniqueAssetName(targetDir, srcPath.filename().u8string(),
                               sourceIsDirectory);
        const std::filesystem::path dstPath =
            std::filesystem::u8path(targetDir) / std::filesystem::u8path(fileName);
        
        CopyFileOrDirectory(m_clipboardPath, dstPath.u8string());
        if (!std::filesystem::exists(dstPath)) return;
        if (!ProjectManager::GetInstance().RegisterProjectAsset(dstPath.u8string())) {
            std::error_code ec;
            std::filesystem::remove_all(dstPath, ec);
            printf("Paste failed: project manifest update failed\n");
            return;
        }
        
        if (m_isCutOperation) {
            CleanupTextureCacheForDirectory(m_clipboardPath);
            std::filesystem::remove_all(srcPath);
            if (!ProjectManager::GetInstance().UnregisterProjectAsset(m_clipboardPath)) {
                printf("Paste warning: source removed but manifest update failed\n");
            }
            m_clipboardPath.clear();
            m_isCutOperation = false;
        }
        RefreshAssetTree();
    } catch (const std::exception& e) {
        printf("Paste failed: %s\n", e.what());
    }
}

void AssetsWindow::EditMaterial(const std::string& filePath) {
    MaterialEditorWindow::GetInstance().OpenMaterial(filePath);
}

void AssetsWindow::GenerateModelPreview(const std::string& modelPath) {
#ifdef __ANDROID__
    printf("Preview generation skipped on Android\n");
    return;
#endif
    
    try {
        std::filesystem::path modelPathObj = std::filesystem::u8path(modelPath);
        std::string fileNameWithoutExt = modelPathObj.stem().u8string();
        
        std::filesystem::path metaDir = modelPathObj.parent_path() / ".meta";
        if (!std::filesystem::exists(metaDir)) {
            std::filesystem::create_directories(metaDir);
        }
        
        std::filesystem::path previewPath = metaDir / (fileNameWithoutExt + "_preview.png");
        
        const std::string previewPathUtf8 = previewPath.u8string();
        printf("Generating preview for: %s -> %s\n", modelPath.c_str(), previewPathUtf8.c_str());
        
        if (PreviewGenerator::GetInstance().GeneratePreview(modelPath, previewPathUtf8)) {
            UpdateAssetCache();
        }
        
    } catch (const std::exception& e) {
        printf("Failed to generate preview: %s\n", e.what());
    }
}

void AssetsWindow::GenerateVoxPreview(const std::string& voxPath) {
#ifdef __ANDROID__
    printf("Vox preview generation skipped on Android\n");
    return;
#endif
    
    try {
        std::filesystem::path voxPathObj = std::filesystem::u8path(voxPath);
        std::string fileNameWithoutExt = voxPathObj.stem().u8string();
        
        std::filesystem::path metaDir = voxPathObj.parent_path() / ".meta";
        if (!std::filesystem::exists(metaDir)) {
            std::filesystem::create_directories(metaDir);
        }
        
        std::filesystem::path previewPath = metaDir / (fileNameWithoutExt + "_preview.png");
        
        const std::string previewPathUtf8 = previewPath.u8string();
        printf("Generating vox preview for: %s -> %s\n", voxPath.c_str(), previewPathUtf8.c_str());
        
        if (PreviewGenerator::GetInstance().GenerateVoxPreview(voxPath, previewPathUtf8)) {
            UpdateAssetCache();
        }
        
    } catch (const std::exception& e) {
        printf("Failed to generate vox preview: %s\n", e.what());
    }
}

void AssetsWindow::UpdateAssetCache() {
    RefreshAssetTree();
    
    std::vector<AssetItem> items = ScanDirectoryFiles(m_currentDirectory);
    for (const auto& item : items) {
        if (item.isImage) {
            if (m_TexturePool) {
                m_TexturePool->Release(item.path);
                m_TexturePool->LoadTexture2D(item.path, item.path);
            }
        } else if (item.isMaterial) {
            std::filesystem::path materialPath = std::filesystem::u8path(item.path);
            std::filesystem::path metaDir = materialPath.parent_path() / ".meta";
            std::string fileNameWithoutExt = materialPath.stem().u8string();
            std::filesystem::path previewPath = metaDir / (fileNameWithoutExt + "_preview.png");
            
            if (std::filesystem::exists(previewPath)) {
                std::string previewPathStr = previewPath.u8string();
                if (m_TexturePool) {
                    m_TexturePool->Release(previewPathStr);
                    m_TexturePool->LoadTexture2D(previewPathStr, previewPathStr);
                }
            }
        } else {
            std::string fileExt = GetFileExtension(item.name);
            bool isModelFile = (fileExt == "gltf" || fileExt == "glb" || 
                              fileExt == "obj" || fileExt == "fbx" || fileExt == "dae");
            
            if (isModelFile) {
                std::filesystem::path modelPath = std::filesystem::u8path(item.path);
                std::filesystem::path metaDir = modelPath.parent_path() / ".meta";
                std::string fileNameWithoutExt = modelPath.stem().u8string();
                std::filesystem::path previewPath = metaDir / (fileNameWithoutExt + "_preview.png");
                
                if (std::filesystem::exists(previewPath)) {
                    std::string previewPathStr = previewPath.u8string();
                    if (m_TexturePool) {
                        m_TexturePool->Release(previewPathStr);
                        m_TexturePool->LoadTexture2D(previewPathStr, previewPathStr);
                    }
                }
            }
        }
    }
}

VkDescriptorSet AssetsWindow::GetCachedTextureDescriptor(const std::string& texturePath) {
    m_CurrentFrame++;
    
    auto it = m_TextureDescriptorCache.find(texturePath);
    if (it != m_TextureDescriptorCache.end()) {
        it->second.lastUsedFrame = m_CurrentFrame;
        it->second.useCount++;
        return it->second.descriptorSet;
    }
    
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    if (m_TexturePool) {
        descriptorSet = m_TexturePool->GetDescriptorSet(texturePath);
        if (descriptorSet == VK_NULL_HANDLE) {
            m_TexturePool->LoadTexture2D(texturePath, texturePath);
            descriptorSet = m_TexturePool->GetDescriptorSet(texturePath);
        }
    }
    
    if (descriptorSet != VK_NULL_HANDLE) {
        TextureCacheItem item;
        item.descriptorSet = descriptorSet;
        item.lastUsedFrame = m_CurrentFrame;
        item.useCount = 1;
        m_TextureDescriptorCache[texturePath] = item;
    }
    
    if (m_CurrentFrame % m_CacheCleanupInterval == 0) {
        CleanupExpiredTextureCache();
    }
    
    return descriptorSet;
}

void AssetsWindow::CleanupExpiredTextureCache() {
    const uint64_t FRAMES_BEFORE_EXPIRY = 600;
    
    std::vector<std::string> toRemove;
    
    for (auto& pair : m_TextureDescriptorCache) {
        if ((m_CurrentFrame - pair.second.lastUsedFrame) > FRAMES_BEFORE_EXPIRY) {
            toRemove.push_back(pair.first);
        }
    }
    
    for (const auto& path : toRemove) {
        m_TextureDescriptorCache.erase(path);
        if (m_TexturePool) {
            m_TexturePool->Release(path);
        }
    }
    
    if (!toRemove.empty()) {
        printf("[TextureCache] Cleaned up %d expired descriptors (cached: %d)", 
              toRemove.size(), m_TextureDescriptorCache.size());
    }
}

void AssetsWindow::CleanupTextureCacheForDirectory(const std::string& directoryPath) {
    std::vector<std::string> toRemove;
    
    std::string normalizedDir = directoryPath;
    if (!normalizedDir.empty() && normalizedDir.back() != '\\' && normalizedDir.back() != '/') {
        normalizedDir += "\\";
    }
    
    for (auto& pair : m_TextureDescriptorCache) {
        const std::string& texturePath = pair.first;
        
        if (texturePath == m_folderIconName || texturePath == m_folderBackIconName ||
            texturePath == m_fileIconName || texturePath == m_materialIconName) {
            continue;
        }
        
        if (texturePath.find(normalizedDir) != std::string::npos ||
            texturePath.find(directoryPath) != std::string::npos) {
            toRemove.push_back(texturePath);
        }
    }
    
    for (const auto& path : toRemove) {
        m_TextureDescriptorCache.erase(path);
        if (m_TexturePool) {
            m_TexturePool->Release(path);
        }
    }
    
    if (!toRemove.empty()) {
        printf("[TextureCache] Cleaned up %d descriptors for directory: %s (remaining: %d)", 
              toRemove.size(), directoryPath.c_str(), m_TextureDescriptorCache.size());
    }
}

void AssetsWindow::SaveRenderTargetToPNG(RenderTarget& renderTarget, const std::string& filePath) {
    VkDevice device = g_Device;
    VkPhysicalDevice physicalDevice = g_PhysicalDevice;
    VkQueue queue = g_Queue;
    VkCommandPool commandPool = g_CommandPool;
    
    uint32_t width = renderTarget.GetWidth();
    uint32_t height = renderTarget.GetHeight();
    VkImage srcImage = renderTarget.GetColorImage();
    
    VkDeviceSize imageSize = width * height * 4;
    
    VulkanBuffer stagingBuffer;
    stagingBuffer.Create(imageSize, 
                         VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    
    VkCommandBufferAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    
    VkCommandBuffer commandBuffer;
    vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer);
    
    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commandBuffer, &beginInfo);
    
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = srcImage;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    
    vkCmdPipelineBarrier(commandBuffer,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
    
    VkBufferImageCopy region = {};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {width, height, 1};
    
    vkCmdCopyImageToBuffer(commandBuffer, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           stagingBuffer.GetBuffer(), 1, &region);
    
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    
    vkCmdPipelineBarrier(commandBuffer,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
    
    vkEndCommandBuffer(commandBuffer);
    
    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    
    vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    
    vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
    
    void* data;
    vkMapMemory(device, stagingBuffer.GetMemory(), 0, imageSize, 0, &data);
    
    SDL_Surface* surface = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGBA32);
    if (surface) {
        memcpy(surface->pixels, data, imageSize);
        
        SDL_SaveBMP(surface, filePath.c_str());
        SDL_DestroySurface(surface);
        
        printf("Render target saved to: %s\n", filePath.c_str());
    }
    
    vkUnmapMemory(device, stagingBuffer.GetMemory());
}

} // namespace Editor
