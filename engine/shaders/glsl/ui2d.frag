#version 450

// 2D Canvas 片元着色器：纹理采样 * 顶点色（无纹理时绑定 1x1 白纹理）
layout(binding = 0) uniform sampler2D tex;

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec4 fragColor;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = texture(tex, fragUV) * fragColor;
}
