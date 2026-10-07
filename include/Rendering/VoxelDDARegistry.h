#pragma once
#include "Platform/Export.h"
#ifndef VOXEL_DDA_REGISTRY_H
#define VOXEL_DDA_REGISTRY_H
// Scene-level registry of dense voxel grids for the compute-shader DDA
// traversal path (MIKAN_HWRT_VOXEL_DDA=1). VoxRenderer registers each merged
// vox volume after its Texture3D upload; RayTracingScene emits per-entity
// world matrices so the shader can transform rays into grid space. The RT
// viewport reads the registry each frame to write its grid descriptors, so
// late registration is picked up on the next frame without restart.

#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace mikan::render {

struct VoxelDDAGrid {
    // Stable per-process grid id; also the index into the shader's grid array.
    uint32_t id = 0;
    // Texture3D metadata (image/view owned by the entity's VoxelTexture3DManager;
    // lifetime is tied to the VoxRenderer, cleared on unregister).
    void* imageView = nullptr;   // VkImageView
    uint32_t sizeX = 0, sizeY = 0, sizeZ = 0;
    glm::mat4 gridToLocal{1}; // raw VOX xyz boundary coordinates -> centered mesh xyz
    std::string name;
    // Palette for hit shading: rgba = (r,g,b,flags), flags bit0 mirror bit1 emissive —
    // same bit layout as the quad material words.
    std::vector<glm::vec4> palette;
};

class MIKAN_API VoxelDDARegistry {
public:
    static VoxelDDARegistry& Get();
    // Registers/updates a grid and returns its shader id. View/palette pointers
    // are copied; the caller keeps the underlying resources alive until Unregister.
    uint32_t Register(const std::string& name, void* imageView,
                      uint32_t sizeX, uint32_t sizeY, uint32_t sizeZ,
                      const std::vector<glm::vec4>& palette, const glm::mat4& gridToLocal=glm::mat4(1));
    void Unregister(const std::string& name);
    void Clear();
    const std::vector<VoxelDDAGrid>& Grids() const { return m_Grids; }
    // Bumped on every Register/Unregister so consumers can invalidate caches.
    uint32_t Version() const { return m_Version; }
    static bool BootEnabled();
private:
    std::vector<VoxelDDAGrid> m_Grids;
    uint32_t m_Version = 0;
};

}
#endif
