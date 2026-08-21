#include "Editor/MaterialEditor.h"
#include "Editor/MaterialEditorWindow.h"
#include <fstream>
#include <cstdio>

namespace Editor {

MaterialEditor& MaterialEditor::GetInstance() {
    static MaterialEditor instance;
    return instance;
}

MaterialEditor::MaterialEditor() {
}

bool MaterialEditor::SaveMaterialToFile(const std::string& filePath, const ECS::MaterialComponent& material) {
    try {
        std::ofstream file(filePath);
        if (file.is_open()) {
            file << "# Material File\n";
            file << "albedoPath=" << material.albedoPath << "\n";
            file << "normalPath=" << material.normalPath << "\n";
            file << "roughnessPath=" << material.roughnessPath << "\n";
            file << "metallicPath=" << material.metallicPath << "\n";
            file << "aoPath=" << material.aoPath << "\n";
            file << "emissivePath=" << material.emissivePath << "\n";
            file << "albedoColor=" << material.albedoColor.x << "," << material.albedoColor.y << "," << material.albedoColor.z << "\n";
            file << "metallic=" << material.metallic << "\n";
            file << "roughness=" << material.roughness << "\n";
            file << "ao=" << material.ao << "\n";
            file << "useAlbedoTexture=" << material.useAlbedoTexture << "\n";
            file << "useNormalTexture=" << material.useNormalTexture << "\n";
            file << "useRoughnessTexture=" << material.useRoughnessTexture << "\n";
            file << "useMetallicTexture=" << material.useMetallicTexture << "\n";
            file << "useAOTexture=" << material.useAOTexture << "\n";
            file << "useEmissiveTexture=" << material.useEmissiveTexture << "\n";
            file << "albedoSamplerType=" << material.albedoSamplerType << "\n";
            file << "normalSamplerType=" << material.normalSamplerType << "\n";
            file << "roughnessSamplerType=" << material.roughnessSamplerType << "\n";
            file << "metallicSamplerType=" << material.metallicSamplerType << "\n";
            file << "aoSamplerType=" << material.aoSamplerType << "\n";
            file << "emissiveSamplerType=" << material.emissiveSamplerType << "\n";
            file.close();
            printf("Material saved to: %s\n", filePath.c_str());
            return true;
        }
    } catch (const std::exception& e) {
        printf("Failed to save material: %s\n", e.what());
    }
    return false;
}

bool MaterialEditor::LoadMaterialFromFile(const std::string& filePath, ECS::MaterialComponent& material) {
    try {
        std::ifstream file(filePath);
        if (file.is_open()) {
            std::string line;
            while (std::getline(file, line)) {
                if (line.empty() || line[0] == '#') continue;
                
                size_t pos = line.find('=');
                if (pos != std::string::npos) {
                    std::string key = line.substr(0, pos);
                    std::string value = line.substr(pos + 1);
                    
                    if (key == "albedoPath") material.albedoPath = value;
                    else if (key == "normalPath") material.normalPath = value;
                    else if (key == "roughnessPath") material.roughnessPath = value;
                    else if (key == "metallicPath") material.metallicPath = value;
                    else if (key == "aoPath") material.aoPath = value;
                    else if (key == "emissivePath") material.emissivePath = value;
                    else if (key == "albedoColor") {
                        size_t comma1 = value.find(',');
                        size_t comma2 = value.find(',', comma1 + 1);
                        if (comma1 != std::string::npos && comma2 != std::string::npos) {
                            material.albedoColor.x = std::stof(value.substr(0, comma1));
                            material.albedoColor.y = std::stof(value.substr(comma1 + 1, comma2 - comma1 - 1));
                            material.albedoColor.z = std::stof(value.substr(comma2 + 1));
                        }
                    }
                    else if (key == "metallic") material.metallic = std::stof(value);
                    else if (key == "roughness") material.roughness = std::stof(value);
                    else if (key == "ao") material.ao = std::stof(value);
                    else if (key == "useAlbedoTexture") material.useAlbedoTexture = (value == "1" || value == "true");
                    else if (key == "useNormalTexture") material.useNormalTexture = (value == "1" || value == "true");
                    else if (key == "useRoughnessTexture") material.useRoughnessTexture = (value == "1" || value == "true");
                    else if (key == "useMetallicTexture") material.useMetallicTexture = (value == "1" || value == "true");
                    else if (key == "useAOTexture") material.useAOTexture = (value == "1" || value == "true");
                    else if (key == "useEmissiveTexture") material.useEmissiveTexture = (value == "1" || value == "true");
                    else if (key == "albedoSamplerType") material.albedoSamplerType = std::stoi(value);
                    else if (key == "normalSamplerType") material.normalSamplerType = std::stoi(value);
                    else if (key == "roughnessSamplerType") material.roughnessSamplerType = std::stoi(value);
                    else if (key == "metallicSamplerType") material.metallicSamplerType = std::stoi(value);
                    else if (key == "aoSamplerType") material.aoSamplerType = std::stoi(value);
                    else if (key == "emissiveSamplerType") material.emissiveSamplerType = std::stoi(value);
                }
            }
            file.close();
            printf("Material loaded from: %s\n", filePath.c_str());
            return true;
        }
    } catch (const std::exception& e) {
        printf("Failed to load material: %s\n", e.what());
    }
    return false;
}

void MaterialEditor::EditMaterial(const std::string& filePath) {
    MaterialEditorWindow::GetInstance().OpenMaterial(filePath);
}

} // namespace Editor
