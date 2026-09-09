#version 450

// 采样端约定：fragment 只须 texelFetch(cubeArray, vec4(dir, lightIndex)) 与 dist/range 比较——无需光源投影矩阵
// push constant 与顶点着色器共用（lightPosRange @64）
precision highp float;

layout(push_constant) uniform PushConstants {
    mat4 projView;
    vec4 lightPosRange;   // xyz = 光源世界位置，w = range
    vec4 subMeshAlpha;    // x = alphaCutoff，y = alphaMode（-1 unknown, 0 opaque, 1 mask, 2 blend）
} pc;

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec2 vTexCoord;
layout(location = 2) flat in vec4 vTextureFlags;

layout(binding = 0) uniform sampler2D albedoTexture;

void main() {
    // 阴影必须与主模型/深度预通过滤同一套 alpha 语义：
    // MASK 使用 glTF cutoff，未知模式保留旧资产的 alpha<0.5 行为；OPAQUE 不采样。
    // BLEND 由 CPU 跳过，这里再 discard 一次作为管线级保护。
    float alphaMode = pc.subMeshAlpha.y;
    if (alphaMode > 1.5) {
        discard;
    }
    if (vTextureFlags.y > 0.5 && (alphaMode > 0.5 || alphaMode < -0.5)) {
        float cutoff = alphaMode > 0.5 ? pc.subMeshAlpha.x : 0.5;
        if (texture(albedoTexture, vTexCoord).a < cutoff) {
            discard;
        }
    }

    float dist = length(vWorldPos - pc.lightPosRange.xyz);
    gl_FragDepth = clamp(dist / max(pc.lightPosRange.w, 1e-4), 0.0, 1.0);
}
