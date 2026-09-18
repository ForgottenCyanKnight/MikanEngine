#version 450

// 正交投影下 NDC z 线性，采样端直接比较 clipPos.z 映射后的深度纹理值（+bias），无需线性化
// push constant 与 shadow_depth.vert 兼容（96B：projView + lightPosRange + subMeshAlpha）
// The CSM pipeline reuses shadow_depth.vert for its vertex stage.
precision highp float;

layout(push_constant) uniform PushConstants {
    mat4 projView;
    vec4 lightPosRange;   // 未使用（CSM 无需线性深度）
    vec4 subMeshAlpha;    // x = alphaCutoff，y = alphaMode（-1 unknown, 0 opaque, 1 mask, 2 blend）
} pc;

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
    // 默认 gl_FragDepth（NDC z 经 viewport 映射 [0,1]）
}
