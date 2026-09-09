#version 450

// model z-prepass 片元着色器（subpass 0，depth-only）——带 alpha test
// 与 model.frag 的 discard 逻辑保持一致：alpha-mask 材质（Sponza/Bistro 树叶/栅栏/帘子）
// 的镂空像素在 z-prepass 阶段即丢弃，避免"z-prepass 写入深度但几何 pass discard"
// 导致镂空区域错误遮挡后面物体。无颜色输出，仅深度。
// RenderDepthOnly 每 subMesh push；-1 未知回退旧 0.5 行为；OPAQUE/BLEND 不采样不 discard）。
layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragTextureFlags;   // y=useAlbedoTexture（与 model.frag 一致）

layout(push_constant) uniform PC_Alpha {
    layout(offset = 160) vec4 subMeshAlpha;
};

layout(binding = 0) uniform sampler2D albedoTexture;

void main() {
    float alphaMode = subMeshAlpha.y;
    if (fragTextureFlags.y > 0.5) {
        if (alphaMode > 1.5) {
            // BLEND：不写深度（RenderDepthOnly 已跳过，双保险）
        } else if (alphaMode > 0.5) {
            // MASK：按材质 alphaCutoff 镂空（与 model.frag 一致）
            vec4 texColor = texture(albedoTexture, fragTexCoord);
            if (texColor.a < subMeshAlpha.x) {
                discard;
            }
        } else if (alphaMode < -0.5) {
            // 未知（assimp/FBX 路径）：旧行为——alpha<0.5 镂空
            vec4 texColor = texture(albedoTexture, fragTexCoord);
            if (texColor.a < 0.5) {
                discard;
            }
        }
        // OPAQUE：不 discard（无 alpha 语义）
    }
}
