#version 450
#extension GL_GOOGLE_include_directive : require
#include "../common/triangle_debug_begin.glsl"

// Voxel world G-buffer output: atlas albedo, normal and surface parameters.
precision highp float;
precision highp int;

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec3 fragTintColor;
layout(location = 3) in vec2 fragTexCoord;
layout(location = 4) flat in vec2 fragTexScale;
layout(location = 5) flat in int fragFaceIndex;
layout(location = 6) flat in int fragBlockType;
layout(location = 7) flat in int fragTextureIndex;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outNormal;
// 与 model.frag/terrain.frag 统一：2=材质(RGBA8)、3=运动矢量(RG16F)。
// 此前两者互换——体素世界把运动矢量写进材质附件、材质写进运动附件（vec4 写
// RG16F 丢 GB 分量），管线预览的材质通道在体素场景下显示错误。
layout(location = 2) out vec4 outMaterial;
layout(location = 3) out vec2 outMotionVector;

layout(binding = 0) uniform sampler2D textureAtlas;

layout(constant_id = 0) const bool alphaTestLeaves = true;

const int atlasSize = 16;
const float atlasTileSize = 1.0 / float(atlasSize);
const float atlasTilePadding = 0.5 * atlasTileSize;
const float atlasTileSpan = 1.0 - 2.0 * atlasTilePadding;

vec2 getAtlasCoords(int textureID, vec2 unwrappedUV) {
    ivec2 tilePos = ivec2(textureID % atlasSize, textureID / atlasSize);
    vec2 tileUV = atlasTilePadding + fract(unwrappedUV) * atlasTileSpan;
    return (vec2(tilePos) + tileUV) * atlasTileSize;
}

vec2 SignNotZero(vec2 v) {
    return vec2(v.x < 0.0 ? -1.0 : 1.0,
                v.y < 0.0 ? -1.0 : 1.0);
}

vec2 OctahedronEncode(vec3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    if (n.z < 0.0) {
        n.xy = (1.0 - abs(n.yx)) * SignNotZero(n.xy);
    }
    return n.xy;
}

void main() {
    vec2 unwrappedUV = fragTexCoord * fragTexScale;
    vec2 atlasUV = getAtlasCoords(fragTextureIndex, unwrappedUV);
    // Use gradients before wrapping so crossing a repeated tile does not inflate the mip level.
    vec2 atlasDx = dFdx(unwrappedUV) * (atlasTileSpan * atlasTileSize);
    vec2 atlasDy = dFdy(unwrappedUV) * (atlasTileSpan * atlasTileSize);
    // The chunk mesher moves distant leaves into the opaque draw list.
    bool crossPlant = fragBlockType == 13 || fragBlockType == 14;
    bool alphaTested = (fragBlockType == 5 && alphaTestLeaves) ||
                       fragBlockType == 6 || crossPlant;
    vec4 color = crossPlant ? textureLod(textureAtlas, atlasUV, 0.0)
                            : textureGrad(textureAtlas, atlasUV, atlasDx, atlasDy);

    // 半透明方块（树叶/玻璃/植物）：alpha 剔除
    if (alphaTested) {
        if (color.a < 0.1) discard;
    } else {
        if (color.a < 0.1) color.rgb = fragTintColor.rgb * 0.2;
    }

    vec3 albedo = color.rgb * fragTintColor.rgb;
    vec3 N = normalize(fragNormal);

    // The deferred composite decodes albedo and applies scene lighting.
    outColor = vec4(albedo, 1.0);
    outNormal = vec4(OctahedronEncode(N), 0.0, 0.0);
    outMotionVector = vec2(0.0);
    // G-buffer material: metallic, roughness, ambient occlusion, emissive strength.
    outMaterial = vec4(0.0, 0.9, 1.0, 0.0);
}

#include "../common/triangle_debug_end.glsl"
