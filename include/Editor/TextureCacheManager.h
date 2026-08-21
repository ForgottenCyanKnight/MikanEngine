#pragma once

#include <string>
#include <unordered_map>
#include <vulkan/vulkan.h>
#include <memory>

class TexturePool;

namespace Editor {

class TextureCacheManager {
public:
    static TextureCacheManager& GetInstance();
    
    void SetTexturePool(TexturePool* pool);
    TexturePool* GetTexturePool() const { return m_TexturePool; }
    
    VkDescriptorSet GetCachedTextureDescriptor(const std::string& texturePath);
    void CleanupExpiredTextureCache();
    void CleanupTextureCacheForDirectory(const std::string& directoryPath);
    void ClearCache();
    
    void SetIconNames(const std::string& folderIcon, const std::string& folderBackIcon,
                     const std::string& fileIcon, const std::string& materialIcon);
    
    size_t GetCacheSize() const { return m_TextureDescriptorCache.size(); }

private:
    TextureCacheManager();
    ~TextureCacheManager() = default;
    
    TextureCacheManager(const TextureCacheManager&) = delete;
    TextureCacheManager& operator=(const TextureCacheManager&) = delete;
    
    struct TextureCacheItem {
        VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
        uint64_t lastUsedFrame = 0;
        uint32_t useCount = 0;
    };
    
    std::unordered_map<std::string, TextureCacheItem> m_TextureDescriptorCache;
    uint64_t m_CurrentFrame = 0;
    uint32_t m_CacheCleanupInterval = 300;
    const uint64_t FRAMES_BEFORE_EXPIRY = 600;
    
    TexturePool* m_TexturePool = nullptr;
    
    std::string m_folderIconName;
    std::string m_folderBackIconName;
    std::string m_fileIconName;
    std::string m_materialIconName;
};

} // namespace Editor
