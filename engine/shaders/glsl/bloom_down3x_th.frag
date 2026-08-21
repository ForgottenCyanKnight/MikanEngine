#version 450
// 2026-08-13：3x 降采样 + 亮部阈值提取（合并原全屏 extract pass——composite 已在 HDR 空间，直接降采样时提取）
// 9-tap box 平均后硬阈值（原 bloom_extract.frag 语义：max(c - THRESHOLD, 0)）
#define BLOOM_THRESHOLD 0.4   // 线性 HDR 阈值（自发光要明显泛光；可调）
layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 fragColor;
layout(binding = 0) uniform sampler2D inputTex;   // composite（全尺寸线性 HDR）

void main() {
    ivec2 iuv = ivec2(gl_FragCoord.xy) * 3 + 1;   // 3x3 块中心（整数纹素坐标）
    vec2 texSize = vec2(textureSize(inputTex, 0));
    vec3 sum = vec3(0.0);
    sum += texture(inputTex, (vec2(iuv + ivec2( 0,  0)) + 0.5) / texSize).rgb;
    sum += texture(inputTex, (vec2(iuv + ivec2( 1,  0)) + 0.5) / texSize).rgb;
    sum += texture(inputTex, (vec2(iuv + ivec2(-1,  0)) + 0.5) / texSize).rgb;
    sum += texture(inputTex, (vec2(iuv + ivec2( 0,  1)) + 0.5) / texSize).rgb;
    sum += texture(inputTex, (vec2(iuv + ivec2( 0, -1)) + 0.5) / texSize).rgb;
    sum += texture(inputTex, (vec2(iuv + ivec2( 1,  1)) + 0.5) / texSize).rgb;
    sum += texture(inputTex, (vec2(iuv + ivec2(-1, -1)) + 0.5) / texSize).rgb;
    sum += texture(inputTex, (vec2(iuv + ivec2( 1, -1)) + 0.5) / texSize).rgb;
    sum += texture(inputTex, (vec2(iuv + ivec2(-1,  1)) + 0.5) / texSize).rgb;
    vec3 color = sum * (1.0 / 9.0);
    color = max(color - vec3(BLOOM_THRESHOLD), 0.0);   // 亮部提取（HDR 空间）
    fragColor = vec4(color, 1.0);
}
