#version 450
// 采样点落在纹素中心 → 与 texelFetch 等价，但走 sampler（Linear）语义一致、更平滑
layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 fragColor;
layout(binding = 0) uniform sampler2D inputTex;

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
    fragColor = vec4(sum * (1.0 / 9.0), 1.0);
}
