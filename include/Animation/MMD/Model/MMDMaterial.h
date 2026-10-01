#pragma once

#include "Animation/MMD/Base/MMDTypes.h"

#include <string>
#include <vector>

namespace mmd
{
    enum class MMDMaterialFlags : uint8_t
    {
        BothFace = 0x01,
        GroundShadow = 0x02,
        CastSelfShadow = 0x04,
        ReceiveSelfShadow = 0x08,
        DrawEdge = 0x10,
        VertexColor = 0x20,
        DrawPoint = 0x40,
        DrawLine = 0x80,
    };

    enum class MMDSphereMode : uint8_t
    {
        None,
        Mul,
        Add,
        SubTexture,
    };

    enum class MMDToonMode : uint8_t
    {
        Separate,
        Common,
    };

    struct MMDMaterial
    {
        std::string m_name;
        glm::vec4 m_diffuse;
        glm::vec3 m_specular;
        float m_specularPower;
        glm::vec3 m_ambient;
        MMDMaterialFlags m_flags;
        glm::vec4 m_edgeColor;
        float m_edgeSize;
        int32_t m_textureIndex;
        int32_t m_sphereTextureIndex;
        MMDSphereMode m_sphereMode;
        MMDToonMode m_toonMode;
        int32_t m_toonTextureIndex;
        int32_t m_numFaceVertices;
        glm::vec4 m_textureMul{1}, m_textureAdd{0};
        glm::vec4 m_sphereMul{1}, m_sphereAdd{0};
        glm::vec4 m_toonMul{1}, m_toonAdd{0};
    };
}
