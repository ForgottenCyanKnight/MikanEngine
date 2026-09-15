#include "Editor/PreviewGeneratorHelper.h"
#include "Core/Log.h"

#include "PreviewGenerator.h"
#include <filesystem>

namespace Editor {

PreviewGeneratorHelper& PreviewGeneratorHelper::GetInstance() {
    static PreviewGeneratorHelper instance;
    return instance;
}

PreviewGeneratorHelper::PreviewGeneratorHelper() {
}

void PreviewGeneratorHelper::GenerateModelPreview(const std::string& modelPath) {
#ifdef __ANDROID__
    LOGW("Preview generation skipped on Android");
    return;
#endif
    
    try {
        std::filesystem::path modelPathObj(modelPath);
        std::string fileNameWithoutExt = modelPathObj.stem().string();
        
        std::filesystem::path metaDir = modelPathObj.parent_path() / ".meta";
        if (!std::filesystem::exists(metaDir)) {
            std::filesystem::create_directories(metaDir);
        }
        
        std::filesystem::path previewPath = metaDir / (fileNameWithoutExt + "_preview.png");
        
        LOGI("Generating preview for: %s -> %s", modelPath.c_str(), previewPath.string().c_str());
        
        if (PreviewGenerator::GetInstance().GeneratePreview(modelPath, previewPath.string())) {
            if (m_assetCacheUpdateCallback) {
                m_assetCacheUpdateCallback();
            }
        }
        
    } catch (const std::exception& e) {
        LOGE("Failed to generate preview: %s", e.what());
    }
}

void PreviewGeneratorHelper::GenerateVoxPreview(const std::string& voxPath) {
#ifdef __ANDROID__
    LOGW("Vox preview generation skipped on Android");
    return;
#endif
    
    try {
        std::filesystem::path voxPathObj(voxPath);
        std::string fileNameWithoutExt = voxPathObj.stem().string();
        
        std::filesystem::path metaDir = voxPathObj.parent_path() / ".meta";
        if (!std::filesystem::exists(metaDir)) {
            std::filesystem::create_directories(metaDir);
        }
        
        std::filesystem::path previewPath = metaDir / (fileNameWithoutExt + "_preview.png");
        
        LOGI("Generating vox preview for: %s -> %s", voxPath.c_str(), previewPath.string().c_str());
        
        if (PreviewGenerator::GetInstance().GenerateVoxPreview(voxPath, previewPath.string())) {
            if (m_assetCacheUpdateCallback) {
                m_assetCacheUpdateCallback();
            }
        }
        
    } catch (const std::exception& e) {
        LOGE("Failed to generate vox preview: %s", e.what());
    }
}

} // namespace Editor
