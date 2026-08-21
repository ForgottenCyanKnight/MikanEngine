#version 450
// 简单 boxblur（5×5 方形核，半径 2 texel）——2026-08-11 验证 PostProcessChain 链条可用
layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 fragColor;
layout(binding = 0) uniform sampler2D inputTex;

void main() {
    vec2 texel = 1.0 / vec2(textureSize(inputTex, 0));
    vec3 c = vec3(0.0);
    for (int i = -2; i <= 2; i++)
        for (int j = -2; j <= 2; j++)
            c += texture(inputTex, fragTexCoord + vec2(texel.x * float(i), texel.y * float(j))).rgb;
    fragColor = vec4(c / 25.0, 1.0);
}
