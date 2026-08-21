#include "Editor/TextureCacheManager.h"

#include "TexturePool.h"

namespace Editor {

TextureCacheManager& TextureCacheManager::GetInstance() {
    static TextureCacheManager instance;
    return instance;
}

TextureCacheManager::TextureCacheManager() {
}

void TextureCacheManager::SetTexturePool(TexturePool* pool) {
    m_TexturePool = pool;
}

void TextureCacheManager::SetIconNames(const std::string& folderIcon, const std::string& folderBackIcon,
                                       const std::string& fileIcon, const std::string& materialIcon) {
    m_folderIconName = folderIcon;
    m_folderBackIconName = folderBackIcon;
    m_fileIconName = fileIcon;
    m_materialIconName = materialIcon;
}

VkDescriptorSet TextureCacheManager::GetCachedTextureDescriptor(const std::string& texturePath) {
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

void TextureCacheManager::CleanupExpiredTextureCache() {
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

void TextureCacheManager::CleanupTextureCacheForDirectory(const std::string& directoryPath) {
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

void TextureCacheManager::ClearCache() {
    m_TextureDescriptorCache.clear();
    m_CurrentFrame = 0;
}

} // namespace Editor
