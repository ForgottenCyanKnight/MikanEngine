#pragma once
// SceneTypes.h - scene data types shared between collection and rendering
#include "Platform/Export.h"
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include "ECS/Types.h"

// Renderer-facing material data.  This deliberately mirrors only the
// serializable shading inputs; it is not an ECS component and owns no GPU
// handles.  Keeping this type here lets render backends consume a snapshot
// without retaining a pointer into the live scene.
struct MIKAN_API RenderMaterialData {
    std::string albedoPath;
    std::string normalPath;
    std::string roughnessPath;
    std::string metallicPath;
    std::string aoPath;
    std::string emissivePath;

    int albedoSamplerType = 0;
    int normalSamplerType = 0;
    int roughnessSamplerType = 0;
    int metallicSamplerType = 0;
    int aoSamplerType = 0;
    int emissiveSamplerType = 0;

    glm::vec3 albedoColor = glm::vec3(1.0f);
    float metallic = 0.0f;
    float roughness = 0.5f;
    float ao = 1.0f;
    float emissiveIntensity = 0.0f;

    bool useAlbedoTexture = false;
    bool useNormalTexture = false;
    bool useRoughnessTexture = false;
    bool useMetallicTexture = false;
    bool useAOTexture = false;
    bool useEmissiveTexture = false;
};

// A group of entities that share the same model path (rendered as one batch)
struct MIKAN_API ModelInstanceGroup {
    std::string modelPath;
    std::vector<ECS::Entity> entities;
};

// A group of entities that share the same vox path
struct MIKAN_API VoxInstanceGroup {
    std::string voxPath;
    std::vector<ECS::Entity> entities;
};
