#include "Editor/MaterialEditorWindow.h"
#include "Editor/AssetPathPicker.h"
#include "imgui/imgui.h"
#include "ECS/Components.h"
#include "EditorManager.h"
#include "PreviewGenerator.h"
#include "Core/SceneSerializer.h"
#include <filesystem>
#include <cstring>

namespace Editor {

// 辅助函数：获取文件扩展名
static std::string GetFileExtension(const std::string& filename) {
    size_t dotPos = filename.find_last_of('.');
    if (dotPos == std::string::npos) {
        return "";
    }
    std::string ext = filename.substr(dotPos + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext;
}

MaterialEditorWindow& MaterialEditorWindow::GetInstance() {
    static MaterialEditorWindow instance;
    return instance;
}

void MaterialEditorWindow::OpenMaterial(const std::string& materialPath) {
    m_editingMaterialPath = materialPath;
    EditorManager::GetInstance().LoadMaterialFromFile(materialPath, m_tempMaterial);
    
    std::filesystem::path path(materialPath);
    std::string fileName = path.stem().string();
    strncpy(m_materialNameBuffer, fileName.c_str(), sizeof(m_materialNameBuffer) - 1);
    m_materialNameBuffer[sizeof(m_materialNameBuffer) - 1] = '\0';
    
    m_visible = true;
}

void MaterialEditorWindow::OpenNewMaterial() {
    m_editingMaterialPath.clear();
    m_tempMaterial = ECS::MaterialComponent();
    m_tempMaterial.albedoColor = glm::vec3(0.8f);
    m_tempMaterial.metallic = 0.0f;
    m_tempMaterial.roughness = 0.5f;
    m_tempMaterial.ao = 1.0f;
    memset(m_materialNameBuffer, 0, sizeof(m_materialNameBuffer));
    m_visible = true;
}

void MaterialEditorWindow::Render() {
    if (!m_visible) return;

    const char* windowTitle = m_editingMaterialPath.empty() ? "材质编辑器 - 新建材质" : "材质编辑器";
    ImGui::Begin(windowTitle, &m_visible, ImGuiWindowFlags_AlwaysAutoResize);

    ImGui::Text("材质名称:");
    ImGui::InputText("##materialName", m_materialNameBuffer, sizeof(m_materialNameBuffer));

    ImGui::SeparatorText("纹理路径");

    // 旧的手写路径控件保留在源码中作为迁移参考；实际控件统一走 AssetPathPicker。
#if 0
    // 反照率路径 - 支持拖拽
    char albedoBuffer[512];
    strncpy(albedoBuffer, m_tempMaterial.albedoPath.c_str(), sizeof(albedoBuffer) - 1);
    albedoBuffer[sizeof(albedoBuffer) - 1] = '\0';
    if (ImGui::InputText("反照率路径", albedoBuffer, sizeof(albedoBuffer))) {
        m_tempMaterial.albedoPath = albedoBuffer;
        m_tempMaterial.useAlbedoTexture = !m_tempMaterial.albedoPath.empty();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("浏览##albedo")) {
        std::string picked = ECS::SceneSerializer::OpenTextureDialog();
        if (!picked.empty()) {
            strncpy(albedoBuffer, picked.c_str(), sizeof(albedoBuffer) - 1); albedoBuffer[sizeof(albedoBuffer) - 1] = '\0';
            m_tempMaterial.albedoPath = picked;
            m_tempMaterial.useAlbedoTexture = true;
        }
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_ITEM")) {
            std::string assetPath = (const char*)payload->Data;
            std::string fileExt = GetFileExtension(assetPath);
            if (fileExt == "png" || fileExt == "jpg" || fileExt == "jpeg" ||
                fileExt == "tga" || fileExt == "bmp" || fileExt == "dds") {
                m_tempMaterial.albedoPath = assetPath;
                m_tempMaterial.useAlbedoTexture = true;
            }
        }
        ImGui::EndDragDropTarget();
    }

    // 法线路径 - 支持拖拽
    char normalBuffer[512];
    strncpy(normalBuffer, m_tempMaterial.normalPath.c_str(), sizeof(normalBuffer) - 1);
    normalBuffer[sizeof(normalBuffer) - 1] = '\0';
    if (ImGui::InputText("法线路径", normalBuffer, sizeof(normalBuffer))) {
        m_tempMaterial.normalPath = normalBuffer;
        m_tempMaterial.useNormalTexture = !m_tempMaterial.normalPath.empty();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("浏览##normal")) {
        std::string picked = ECS::SceneSerializer::OpenTextureDialog();
        if (!picked.empty()) {
            strncpy(normalBuffer, picked.c_str(), sizeof(normalBuffer) - 1); normalBuffer[sizeof(normalBuffer) - 1] = '\0';
            m_tempMaterial.normalPath = picked;
            m_tempMaterial.useNormalTexture = true;
        }
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_ITEM")) {
            std::string assetPath = (const char*)payload->Data;
            std::string fileExt = GetFileExtension(assetPath);
            if (fileExt == "png" || fileExt == "jpg" || fileExt == "jpeg" ||
                fileExt == "tga" || fileExt == "bmp" || fileExt == "dds") {
                m_tempMaterial.normalPath = assetPath;
                m_tempMaterial.useNormalTexture = true;
            }
        }
        ImGui::EndDragDropTarget();
    }

    // 粗糙度路径 - 支持拖拽
    char roughnessBuffer[512];
    strncpy(roughnessBuffer, m_tempMaterial.roughnessPath.c_str(), sizeof(roughnessBuffer) - 1);
    roughnessBuffer[sizeof(roughnessBuffer) - 1] = '\0';
    if (ImGui::InputText("粗糙度路径", roughnessBuffer, sizeof(roughnessBuffer))) {
        m_tempMaterial.roughnessPath = roughnessBuffer;
        m_tempMaterial.useRoughnessTexture = !m_tempMaterial.roughnessPath.empty();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("浏览##rough")) {
        std::string picked = ECS::SceneSerializer::OpenTextureDialog();
        if (!picked.empty()) {
            strncpy(roughnessBuffer, picked.c_str(), sizeof(roughnessBuffer) - 1); roughnessBuffer[sizeof(roughnessBuffer) - 1] = '\0';
            m_tempMaterial.roughnessPath = picked;
            m_tempMaterial.useRoughnessTexture = true;
        }
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_ITEM")) {
            std::string assetPath = (const char*)payload->Data;
            std::string fileExt = GetFileExtension(assetPath);
            if (fileExt == "png" || fileExt == "jpg" || fileExt == "jpeg" ||
                fileExt == "tga" || fileExt == "bmp" || fileExt == "dds") {
                m_tempMaterial.roughnessPath = assetPath;
                m_tempMaterial.useRoughnessTexture = true;
            }
        }
        ImGui::EndDragDropTarget();
    }

    // 金属度路径 - 支持拖拽
    char metallicBuffer[512];
    strncpy(metallicBuffer, m_tempMaterial.metallicPath.c_str(), sizeof(metallicBuffer) - 1);
    metallicBuffer[sizeof(metallicBuffer) - 1] = '\0';
    if (ImGui::InputText("金属度路径", metallicBuffer, sizeof(metallicBuffer))) {
        m_tempMaterial.metallicPath = metallicBuffer;
        m_tempMaterial.useMetallicTexture = !m_tempMaterial.metallicPath.empty();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("浏览##metal")) {
        std::string picked = ECS::SceneSerializer::OpenTextureDialog();
        if (!picked.empty()) {
            strncpy(metallicBuffer, picked.c_str(), sizeof(metallicBuffer) - 1); metallicBuffer[sizeof(metallicBuffer) - 1] = '\0';
            m_tempMaterial.metallicPath = picked;
            m_tempMaterial.useMetallicTexture = true;
        }
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_ITEM")) {
            std::string assetPath = (const char*)payload->Data;
            std::string fileExt = GetFileExtension(assetPath);
            if (fileExt == "png" || fileExt == "jpg" || fileExt == "jpeg" ||
                fileExt == "tga" || fileExt == "bmp" || fileExt == "dds") {
                m_tempMaterial.metallicPath = assetPath;
                m_tempMaterial.useMetallicTexture = true;
            }
        }
        ImGui::EndDragDropTarget();
    }

    // AO路径 - 支持拖拽
    char aoBuffer[512];
    strncpy(aoBuffer, m_tempMaterial.aoPath.c_str(), sizeof(aoBuffer) - 1);
    aoBuffer[sizeof(aoBuffer) - 1] = '\0';
    if (ImGui::InputText("AO路径", aoBuffer, sizeof(aoBuffer))) {
        m_tempMaterial.aoPath = aoBuffer;
        m_tempMaterial.useAOTexture = !m_tempMaterial.aoPath.empty();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("浏览##ao")) {
        std::string picked = ECS::SceneSerializer::OpenTextureDialog();
        if (!picked.empty()) {
            strncpy(aoBuffer, picked.c_str(), sizeof(aoBuffer) - 1); aoBuffer[sizeof(aoBuffer) - 1] = '\0';
            m_tempMaterial.aoPath = picked;
            m_tempMaterial.useAOTexture = true;
        }
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_ITEM")) {
            std::string assetPath = (const char*)payload->Data;
            std::string fileExt = GetFileExtension(assetPath);
            if (fileExt == "png" || fileExt == "jpg" || fileExt == "jpeg" ||
                fileExt == "tga" || fileExt == "bmp" || fileExt == "dds") {
                m_tempMaterial.aoPath = assetPath;
                m_tempMaterial.useAOTexture = true;
            }
        }
        ImGui::EndDragDropTarget();
    }

    // 自发光路径 - 支持拖拽
    char emissiveBuffer[512];
    strncpy(emissiveBuffer, m_tempMaterial.emissivePath.c_str(), sizeof(emissiveBuffer) - 1);
    emissiveBuffer[sizeof(emissiveBuffer) - 1] = '\0';
    if (ImGui::InputText("自发光路径", emissiveBuffer, sizeof(emissiveBuffer))) {
        m_tempMaterial.emissivePath = emissiveBuffer;
        m_tempMaterial.useEmissiveTexture = !m_tempMaterial.emissivePath.empty();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("浏览##emissive")) {
        std::string picked = ECS::SceneSerializer::OpenTextureDialog();
        if (!picked.empty()) {
            strncpy(emissiveBuffer, picked.c_str(), sizeof(emissiveBuffer) - 1); emissiveBuffer[sizeof(emissiveBuffer) - 1] = '\0';
            m_tempMaterial.emissivePath = picked;
            m_tempMaterial.useEmissiveTexture = true;
        }
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_ITEM")) {
            std::string assetPath = (const char*)payload->Data;
            std::string fileExt = GetFileExtension(assetPath);
            if (fileExt == "png" || fileExt == "jpg" || fileExt == "jpeg" ||
                fileExt == "tga" || fileExt == "bmp" || fileExt == "dds") {
                m_tempMaterial.emissivePath = assetPath;
                m_tempMaterial.useEmissiveTexture = true;
            }
        }
        ImGui::EndDragDropTarget();
    }

#endif
    auto TexturePathInput = [&](const char* label, std::string& path, bool& useTexture) {
        if (RenderAssetPathInput(label, path, AssetPathKind::Texture)) {
            useTexture = !path.empty();
        }
    };
    TexturePathInput("反照率路径", m_tempMaterial.albedoPath, m_tempMaterial.useAlbedoTexture);
    TexturePathInput("法线路径", m_tempMaterial.normalPath, m_tempMaterial.useNormalTexture);
    TexturePathInput("粗糙度路径", m_tempMaterial.roughnessPath, m_tempMaterial.useRoughnessTexture);
    TexturePathInput("金属度路径", m_tempMaterial.metallicPath, m_tempMaterial.useMetallicTexture);
    TexturePathInput("AO路径", m_tempMaterial.aoPath, m_tempMaterial.useAOTexture);
    TexturePathInput("自发光路径", m_tempMaterial.emissivePath, m_tempMaterial.useEmissiveTexture);

    ImGui::SeparatorText("材质参数");

    ImGui::ColorEdit3("反照率颜色", &m_tempMaterial.albedoColor.x);
    ImGui::DragFloat("金属度", &m_tempMaterial.metallic, 0.01f, 0.0f, 1.0f);
    ImGui::DragFloat("粗糙度", &m_tempMaterial.roughness, 0.01f, 0.0f, 1.0f);
    ImGui::DragFloat("AO", &m_tempMaterial.ao, 0.01f, 0.0f, 1.0f);

    ImGui::Separator();

    if (ImGui::Button("保存", ImVec2(120, 0))) {
        if (strlen(m_materialNameBuffer) > 0) {
            std::string fileName = m_materialNameBuffer;
            if (fileName.find(".material") == std::string::npos) {
                fileName += ".material";
            }
            std::string filePath;
            if (!m_editingMaterialPath.empty()) {
                filePath = m_editingMaterialPath;
            } else {
                filePath = EditorManager::GetInstance().GetCurrentDirectory() + "/" + fileName;
            }
            EditorManager::GetInstance().SaveMaterialToFile(filePath, m_tempMaterial);

            std::filesystem::path materialPath(filePath);
            std::string previewPath = materialPath.parent_path().string() + "/.meta/" + materialPath.stem().string() + "_preview.png";
            PreviewGenerator::GetInstance().GenerateMaterialPreview(m_tempMaterial, previewPath);
            EditorManager::GetInstance().UpdateAssetCache();

            m_editingMaterialPath.clear();
            m_visible = false;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("取消", ImVec2(120, 0))) {
        m_editingMaterialPath.clear();
        m_visible = false;
    }

    ImGui::End();
}

} // namespace Editor
