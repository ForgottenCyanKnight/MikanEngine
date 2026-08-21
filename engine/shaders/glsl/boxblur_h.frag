#version 450
// boxblur 横向 pass（17-tap ±8 texel）+ threshold（提取 HDR 亮部——只模糊高光）——2026-08-11
layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 fragColor;
layout(binding = 0) uniform sampler2D inputTex;

#define BLOOM_THRESHOLD 1.0   // 线性 HDR 亮部阈值（>1 的高光）

void main() {
    vec2 texel = 1.0 / vec2(textureSize(inputTex, 0));
    vec3 c = vec3(0.0);
    for (int i = -8; i <= 8; i++)
        c += max(texture(inputTex, fragTexCoord + vec2(texel.x * float(i), 0.0)).rgb - BLOOM_THRESHOLD, 0.0);
    fragColor = vec4(c / 17.0, 1.0);
}
