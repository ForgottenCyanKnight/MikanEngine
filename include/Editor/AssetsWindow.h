#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <unordered_map>
#include <vulkan/vulkan.h>
#include "ECS/ECS.h"
#include "GizmoMode.h"

class TexturePool;
class RenderTarget;

namespace Editor {

struct AssetItem {
    std::string name;
    std::string path;
    bool isDirectory;
    std::string extension;
    bool isImage;
    bool isMaterial = false;
};

struct DirectoryNode {
    std::string name;
    std::string path;
    std::vector<DirectoryNode> children;
    bool expanded;
};

class AssetsWindow {
public:
    static AssetsWindow& GetInstance();
    
    void Render(bool& showWindow);
    void SetAssetsRootPath(const std::string& path);
    std::string GetSelectedAssetPath() const { return m_selectedAssetPath; }
    std::string GetCurrentDirectory() const { return m_currentDirectory; }
    bool IsImagePreviewVisible() const { return m_showImagePreview; }
    void SetImagePreviewVisible(bool visible) { m_showImagePreview = visible; }
    
    void SetTexturePool(TexturePool* pool) { m_TexturePool = pool; }
    TexturePool* GetTexturePool() const { return m_TexturePool; }
    
    void UpdateAssetCache();
    
    void SetIconNames(const std::string& folderIcon, const std::string& folderBackIcon, 
                     const std::string& fileIcon, const std::string& materialIcon);
    
    void EditMaterial(const std::string& filePath);
    void GenerateModelPreview(const std::string& modelPath);
    void GenerateVoxPreview(const std::string& voxPath);

private:
    AssetsWindow();
    ~AssetsWindow() = default;
    
    AssetsWindow(const AssetsWindow&) = delete;
    AssetsWindow& operator=(const AssetsWindow&) = delete;
    
    std::vector<AssetItem> ScanDirectoryFiles(const std::string& path);
    void BuildDirectoryTree(const std::string& basePath, DirectoryNode& node);
    std::string GetFileExtension(const std::string& filename);
    void RenderDirectoryTreeNode(DirectoryNode& node);
    void ShowAssetContextMenu(const std::string& path, bool isDirectory);
    void ImportFiles();
    std::vector<std::string> OpenImportFileDialog() const;
    void RefreshAssetTree();
    void RenderImagePreviewWindow();
    void SetImagePreviewPath(const std::string& path);
    bool IsImageFilePath(const std::string& path);
    
    // “新建”菜单辅助：生成不冲突的名字，并在当前目录创建资源
    std::string GetUniqueAssetName(const std::string& dir, const std::string& baseName, bool isFolder);
    void CreateNewFolder(const std::string& dir);
    void CreateNewTextFile(const std::string& dir);
    
    void CopyFileOrDirectory(const std::string& src, const std::string& dst);
    void DeleteFileOrDirectory(const std::string& path);
    void RenameFileOrDirectory(const std::string& oldPath, const std::string& newName);
    void PasteFileOrDirectory(const std::string& targetDir);
    
    void SaveRenderTargetToPNG(class RenderTarget& renderTarget, const std::string& filePath);
    
    std::string m_assetsRootPath;
    std::string m_currentDirectory;
    std::vector<AssetItem> m_cachedAssetItems;
    std::string m_cachedAssetItemsDirectory;
    bool m_assetItemsCacheDirty = true;
    std::string m_selectedAssetPath;
    std::string m_tempSelectedAssetPath;
    std::string m_imagePreviewPath;
    bool m_showImagePreview = false;
    bool m_imagePreviewFit = true;
    float m_imagePreviewZoom = 1.0f;
    float m_imagePreviewPanX = 0.0f;
    float m_imagePreviewPanY = 0.0f;
    
    DirectoryNode m_rootNode;
    bool m_directoryTreeInitialized = false;
    
    bool m_showRenamePopup = false;
    char m_renameBuffer[256] = {};
    
    std::string m_contextMenuTargetPath;
    std::string m_clipboardPath;
    bool m_isCutOperation = false;
    
    float m_lastClickTime = 0.0f;
    const float DOUBLE_CLICK_THRESHOLD = 0.3f;
    bool m_isProcessingDoubleClick = false;
    
    TexturePool* m_TexturePool = nullptr;
    
    std::string m_folderIconName;
    std::string m_folderBackIconName;
    std::string m_fileIconName;
    std::string m_materialIconName;
    
    struct TextureCacheItem {
        VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
        uint64_t lastUsedFrame = 0;
        uint32_t useCount = 0;
    };
    std::unordered_map<std::string, TextureCacheItem> m_TextureDescriptorCache;
    uint64_t m_CurrentFrame = 0;
    uint32_t m_CacheCleanupInterval = 300;
    
    VkDescriptorSet GetCachedTextureDescriptor(const std::string& texturePath);
    void CleanupExpiredTextureCache();
    void CleanupTextureCacheForDirectory(const std::string& directoryPath);
};

} // namespace Editor
