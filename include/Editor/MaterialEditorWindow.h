#pragma once

#include "ECS/Components.h"
#include <string>

namespace Editor {

class MaterialEditorWindow {
public:
    static MaterialEditorWindow& GetInstance();

    void SetVisible(bool visible) { m_visible = visible; }
    bool IsVisible() const { return m_visible; }

    void OpenMaterial(const std::string& materialPath);
    void OpenNewMaterial();
    void Render();

private:
    MaterialEditorWindow() = default;
    ~MaterialEditorWindow() = default;

    MaterialEditorWindow(const MaterialEditorWindow&) = delete;
    MaterialEditorWindow& operator=(const MaterialEditorWindow&) = delete;

    bool m_visible = false;
    char m_materialNameBuffer[256] = {};
    ECS::MaterialComponent m_tempMaterial;
    std::string m_editingMaterialPath;
};

} // namespace Editor