#pragma once
// SceneTypes.h - scene data types shared between collection and rendering
#include "Platform/Export.h"
#include <string>
#include <vector>
#include "ECS/Types.h"

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