#version 450

// 反相滤镜（PostProcessChain 测试 pass2）——验证跨 pass 引用（输入 = 前方 pass 输出）

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D inputTex;

void main() {
    vec3 c = texture(inputTex, fragTexCoord).rgb;
    outColor = vec4(1.0 - c, 1.0);
}
