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

layout(binding = 0) uniform sampler2D albedoTexture;

void main() {
    // 不依赖实例 textureFlags / alphaMode 标记：无条件按 albedo 透明度镂空。
    // cutoff 仅在 MASK 模式取材质值，其余（OPAQUE/未知）用 0.5 兜底——
    // 无 alpha 通道的贴图 alpha 恒为 1，不会被误 discard。
    // BLEND 由 CPU 跳过，这里再整片 discard 作为管线级保护。
    float alphaMode = pc.subMeshAlpha.y;
    if (alphaMode > 1.5) {
        discard;
    }
    float cutoff = alphaMode > 0.5 ? pc.subMeshAlpha.x : 0.5;
    if (texture(albedoTexture, vTexCoord).a < cutoff) {
        discard;
    }

    float dist = length(vWorldPos - pc.lightPosRange.xyz);
    gl_FragDepth = clamp(dist / max(pc.lightPosRange.w, 1e-4), 0.0, 1.0);
}
