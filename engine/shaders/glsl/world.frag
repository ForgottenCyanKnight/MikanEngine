#version 450

// Voxel world fragment shader: atlas sampling, alpha test, and directional light.
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
layout(location = 2) out vec2 outMotionVector;
layout(location = 3) out vec4 outMaterial;

layout(binding = 0) uniform sampler2D textureAtlas;

layout(push_constant) uniform PushConstants {
    mat4 projView;
    vec4 cameraPos;
    vec4 sunDir;
} pc;

vec2 getAtlasCoords(int textureID, vec2 uv, vec2 size) {
    const int atlasSize = 16;
    const float texelSize = 1.0 / float(atlasSize);
    const float padding = 0.5 * texelSize;
    ivec2 tilePos = ivec2(textureID % atlasSize, textureID / atlasSize);
    vec2 scaledUV = uv * size;
    vec2 repeatedUV = fract(scaledUV);
    repeatedUV = padding + repeatedUV * (1.0 - 2.0 * padding);
    return (vec2(tilePos) + repeatedUV) * texelSize;
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
    vec2 atlasUV = getAtlasCoords(fragTextureIndex, fragTexCoord, fragTexScale);
    vec4 color = texture(textureAtlas, atlasUV);

    // 半透明方块（树叶/玻璃/植物）：alpha 剔除
    if (fragBlockType == 5 || fragBlockType == 6 || fragBlockType == 13 || fragBlockType == 14) {
        if (color.a < 0.1) discard;
    } else {
        if (color.a < 0.1) color.rgb = fragTintColor.rgb * 0.2;
    }

    // 水：强制不透明（透明混合由透明管线处理，这里只给 albedo）
    if (fragBlockType == 12) {
        color.a = 1.0;
    }

    vec3 albedo = color.rgb * fragTintColor.rgb;
    vec3 N = normalize(fragNormal);

    // 简单方向光：环境光 + 漫反射
    vec3 sunDir = normalize(pc.sunDir.xyz);
    float diffuse = max(dot(N, sunDir), 0.0);
    vec3 ambient = vec3(0.55);
    vec3 lighting = ambient + 0.45 * diffuse;
    vec3 litColor = albedo * lighting;

    outColor = vec4(litColor, 1.0);
    outNormal = vec4(OctahedronEncode(N), 0.0, 0.0);
    outMotionVector = vec2(0.0);
    // material: (roughness, metallic, ao, useAlbedoTexture=1)
    outMaterial = vec4(0.9, 0.0, 1.0, 1.0);
}
