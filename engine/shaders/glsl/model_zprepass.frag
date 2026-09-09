#version 450

// Model depth pre-pass with the same alpha semantics as model.frag.
// Masked pixels are discarded before the geometry pass; opaque and blended
// materials do not sample alpha. The pass writes depth only.
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
            // Imported assets without an explicit alpha mode use a 0.5 cutoff.
            vec4 texColor = texture(albedoTexture, fragTexCoord);
            if (texColor.a < 0.5) {
                discard;
            }
        }
        // OPAQUE：不 discard（无 alpha 语义）
    }
}
