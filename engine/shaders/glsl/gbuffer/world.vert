#version 450
#extension GL_GOOGLE_include_directive : require
#include "voxel/voxel_world_tint.glsl"
#include "voxel/voxel_world_face.glsl"

// Voxel world vertex shader. Each instance supplies one chunk face and packed metadata.

precision highp float;
precision highp int;

layout(push_constant) uniform PushConstants {
    mat4 projView;
} pc;

layout(location = 0) in vec3 aPos;          // Unit quad position.
layout(location = 1) in vec3 aFacePos;      // Face origin in world space.
layout(location = 2) in uint aPackedData;   // Face, block type, UV index, and face size.

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec3 outTintColor;
layout(location = 3) out vec2 outTexCoord;
layout(location = 4) flat out vec2 outTexScale;
layout(location = 5) flat out int outFaceIndex;
layout(location = 6) flat out int outBlockType;
layout(location = 7) flat out int outTextureIndex;

void main() {
    uint faceDir = aPackedData & uint(0xFF);
    uint blockType = (aPackedData >> 8) & uint(0xFF);
    uint uvIndex = (aPackedData >> 16) & uint(0xFF);
    vec2 faceSize = unpackSizeFromPacked(aPackedData);

    int faceIndex = int(faceDir & uint(0xFF));
    outFaceIndex = faceIndex;
    outBlockType = int(blockType);
    outTextureIndex = int(uvIndex);
    outTexScale = faceSize;
    outTexCoord = vec2(gl_VertexIndex % 2, 1.0 - float((gl_VertexIndex / 2) % 2));
    outNormal = faceNormals[faceIndex];
    outTintColor = vec3(1.0);

    vec3 rpos = aPos * vec3(faceSize, 1.0);
    vec3 rotatedPos = rotatePosition(faceIndex, rpos);

    // 植物（玫瑰/草丛）：CPU 生成两个交叉面（face 0 和 face 2），
        // Use the face index to orient the two crossed vegetation planes.
    if (blockType == 13u || blockType == 14u) {
        bool useZX = faceIndex > 0;
        vec2 p = useZX ? rpos.zx : rpos.xz;
        rotatedPos.xz = mat2(0.7071, 0.7071, -0.7071, 0.7071) * p * 1.41414;
        if (blockType == 14u) outTintColor = vec3(MIKAN_VOXEL_WORLD_GRASS_TINT);
    }

    // Tint the top face of grass blocks.
    if (blockType == 3u && faceIndex == 4) {
        outTintColor = vec3(MIKAN_VOXEL_WORLD_GRASS_TINT);
    }
    // 树叶染色
    if (blockType == 5u) {
        outTintColor = vec3(MIKAN_VOXEL_WORLD_GRASS_TINT) * MIKAN_VOXEL_WORLD_LEAF_TINT_SCALE;
    }

    vec3 worldPos = rotatedPos + aFacePos;
    outWorldPos = worldPos;
    gl_Position = pc.projView * vec4(worldPos, 1.0);
}
