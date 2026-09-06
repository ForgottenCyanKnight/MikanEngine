#pragma once

#include "Rendering/ModelLoader.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

// MMD compatibility is deliberately kept as a data normalization layer.
// It knows about MeshData only; it has no dependency on Vulkan, OpenGL,
// Assimp, ECS, editor code, or renderer lifetime/state.
namespace MmdAssetAdapter {

inline bool IsMmdPath(const std::string& pathOrExtension) {
    const size_t dot = pathOrExtension.find_last_of('.');
    std::string extension = dot == std::string::npos
        ? pathOrExtension
        : pathOrExtension.substr(dot);
    if (extension.empty()) return false;
    if (extension.front() != '.') extension.insert(extension.begin(), '.');

    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".pmx" || extension == ".pmd";
}

inline void NormalizeBoneWeights(MeshData& meshData) {
    for (SubMesh& subMesh : meshData.subMeshes) {
        for (Vertex& vertex : subMesh.vertices) {
            const uint32_t total =
                static_cast<uint32_t>(vertex.BoneWeights.x) +
                static_cast<uint32_t>(vertex.BoneWeights.y) +
                static_cast<uint32_t>(vertex.BoneWeights.z) +
                static_cast<uint32_t>(vertex.BoneWeights.w);
            if (total == 0 || total == 255) continue;

            int largestSlot = 0;
            for (int slot = 1; slot < 4; ++slot) {
                if (vertex.BoneWeights[slot] > vertex.BoneWeights[largestSlot]) {
                    largestSlot = slot;
                }
            }

            const int corrected = static_cast<int>(vertex.BoneWeights[largestSlot]) +
                                  255 - static_cast<int>(total);
            vertex.BoneWeights[largestSlot] =
                static_cast<uint8_t>(std::clamp(corrected, 0, 255));
        }
    }
}

inline void NormalizeMaterial(MaterialTextureInfo& material) {
    // Assimp exposes the PMX material diffuse alpha, but does not attach a
    // glTF-style alpha mode. Preserve the existing unknown-mode fallback for
    // texture-only transparency and promote explicit PMX transparency to MASK.
    if (material.alphaMode < 0 && material.diffuse.a < 0.999f) {
        material.alphaMode = 1;
        material.alphaCutoff = 0.5f;
    }
}

inline void ApplyCompatibility(
    MeshData& meshData,
    std::vector<MaterialTextureInfo>& resultMaterials) {
    NormalizeBoneWeights(meshData);

    for (MaterialTextureInfo& material : meshData.materialTextures) {
        NormalizeMaterial(material);
    }
    for (MaterialTextureInfo& material : resultMaterials) {
        NormalizeMaterial(material);
    }

    for (SubMesh& subMesh : meshData.subMeshes) {
        const MaterialTextureInfo* material = nullptr;
        if (subMesh.materialIndex >= 0 &&
            subMesh.materialIndex < static_cast<int>(resultMaterials.size())) {
            material = &resultMaterials[subMesh.materialIndex];
        }
        if (material == nullptr && !subMesh.materialName.empty()) {
            for (const MaterialTextureInfo& candidate : resultMaterials) {
                if (candidate.materialName == subMesh.materialName) {
                    material = &candidate;
                    break;
                }
            }
        }
        if (material == nullptr) continue;

        if (subMesh.alphaMode < 0 && material->alphaMode >= 0) {
            subMesh.alphaMode = material->alphaMode;
            subMesh.alphaCutoff = material->alphaCutoff;
        }
        if (!subMesh.doubleSided && material->doubleSided) {
            subMesh.doubleSided = true;
        }
    }
}

} // namespace MmdAssetAdapter
