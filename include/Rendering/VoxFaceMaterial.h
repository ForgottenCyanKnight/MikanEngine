#pragma once
#include "Rendering/VoxLoader.h"
#include <glm/gtc/packing.hpp>

// Shared CPU material encoding for renderer and offline surface validation.
// 每面材质字（与 m_Faces/m_Quads 平行）：bit0 理想镜面（MATT 金属），
// bit1 自发光（MATT emissive），bit3 透射玻璃（MATT glass，折射路径），
// bits[15:8] 调色板索引，bits[31:16] 自发光强度 half。
// 无 MATT 或普通调色板索引保持 0：金属度 0、自发光 0。
inline uint32_t PackVoxFaceMaterial(const VoxFormat::VoxData& voxData, int colorIndex)
{
    if (colorIndex <= 0 || colorIndex >= 256 || !voxData.hasMaterials)return 0;
    const auto& material = voxData.materials[colorIndex];
    uint32_t word = uint32_t(colorIndex) << 8;
    if (material.type == 1)word |= 1u;
    if (material.type == 2)word |= 8u; // glass: dielectric transmission in the RT path
    if (material.type == 4) {
        // bit2 PBR; bits[15:8] roughness UNORM8; high half metallic.
        word = 4u | (uint32_t(glm::clamp(material.rough, 0.0f, 1.0f) * 255.0f + 0.5f) << 8) |
            (uint32_t(glm::packHalf1x16(material.metallic)) << 16);
    }
    if (material.type == 3) {
        word |= 2u;
        // MagicaVoxel 的 emissive glow 可能为 0，发光材质至少取 1 以可见。
        constexpr float kVoxEmissionScale = 128.0f; // Preview gain; preserve source VOX material values.
        const float power = (material.power > 0.0f ? material.power : 1.0f) * kVoxEmissionScale;
        word |= uint32_t(glm::packHalf1x16(power)) << 16;
    }
    return word;
}
