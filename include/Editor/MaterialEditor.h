#pragma once

#include <string>
#include "ECS/ECS.h"

namespace Editor {

class MaterialEditor {
public:
    static MaterialEditor& GetInstance();
    
    bool SaveMaterialToFile(const std::string& filePath, const ECS::MaterialComponent& material);
    bool LoadMaterialFromFile(const std::string& filePath, ECS::MaterialComponent& material);
    void EditMaterial(const std::string& filePath);

private:
    MaterialEditor();
    ~MaterialEditor() = default;
    
    MaterialEditor(const MaterialEditor&) = delete;
    MaterialEditor& operator=(const MaterialEditor&) = delete;
};

} // namespace Editor
