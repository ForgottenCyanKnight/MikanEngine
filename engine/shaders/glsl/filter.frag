#version 450

// 黑白滤镜（PostProcessChain 测试 pass1）——读合成结果，Rec.601 luma

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D inputTex;

void main() {
    vec3 c = texture(inputTex, fragTexCoord).rgb;
    float luma = dot(c, vec3(0.299, 0.587, 0.114));
    outColor = vec4(c, 1.0);
}
