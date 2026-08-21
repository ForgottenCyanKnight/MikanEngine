#version 450

// 跨 pass 跳转测试：输出输入纹理的红色通道（引用 pass:grayscale 时 = 亮度）
layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D inputTex;

void main() {
    vec3 c = texture(inputTex, fragUV).rgb;
    outColor = vec4(c, 1.0);
}
