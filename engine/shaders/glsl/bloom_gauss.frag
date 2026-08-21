#version 450

// 2026-08-11 Bloom 高斯模糊（参考 FMDS RN4 的分离高斯——单 pass 9-tap 合并版）：
// 归一化 5×5 近似（中心 0.4 + 4 邻 0.1 + 4 对角 0.05）；每级降采样后独立模糊（真实模糊——非纯双线性）

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D inputTex;

void main() {
    vec2 texel = 1.0 / vec2(textureSize(inputTex, 0));
    const float wC = 0.4;
    const float wA = 0.1;
    const float wD = 0.05;
    vec3 c = texture(inputTex, fragTexCoord).rgb * wC;
    c += texture(inputTex, fragTexCoord + vec2( texel.x, 0.0)).rgb * wA;
    c += texture(inputTex, fragTexCoord + vec2(-texel.x, 0.0)).rgb * wA;
    c += texture(inputTex, fragTexCoord + vec2(0.0,  texel.y)).rgb * wA;
    c += texture(inputTex, fragTexCoord + vec2(0.0, -texel.y)).rgb * wA;
    c += texture(inputTex, fragTexCoord + vec2( texel.x,  texel.y)).rgb * wD;
    c += texture(inputTex, fragTexCoord + vec2(-texel.x,  texel.y)).rgb * wD;
    c += texture(inputTex, fragTexCoord + vec2( texel.x, -texel.y)).rgb * wD;
    c += texture(inputTex, fragTexCoord + vec2(-texel.x, -texel.y)).rgb * wD;
    outColor = vec4(c, 1.0);
}
