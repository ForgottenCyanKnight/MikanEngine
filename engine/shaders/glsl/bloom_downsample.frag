#version 450

// 双线性过滤降采样（scale 由链配置），输入 = 上一级 bloom（非原始 composite——保证逐级累积）

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D inputTex;

void main() {
    vec3 c = texture(inputTex, fragTexCoord).rgb;
    outColor = vec4(c, 1.0);
}
