#pragma once

#include <string>
#include <functional>

namespace Editor {

class PreviewGeneratorHelper {
public:
    static PreviewGeneratorHelper& GetInstance();
    
    void GenerateModelPreview(const std::string& modelPath);
    void GenerateVoxPreview(const std::string& voxPath);
    
    void SetAssetCacheUpdateCallback(std::function<void()> callback) {
        m_assetCacheUpdateCallback = callback;
    }

private:
    PreviewGeneratorHelper();
    ~PreviewGeneratorHelper() = default;
    
    PreviewGeneratorHelper(const PreviewGeneratorHelper&) = delete;
    PreviewGeneratorHelper& operator=(const PreviewGeneratorHelper&) = delete;
    
    std::function<void()> m_assetCacheUpdateCallback;
};

} // namespace Editor
