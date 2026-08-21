#version 450
// CMAA2 最终采样（2026-08-17 稀疏 apply 版）：compute 把 AA 结果写入 result 图（alpha=1 标记混合像素），
// 本 pass 采样 result + tonemap 原色，按 alpha 标记混合（未混合区域回退 tonemap——result 每帧 clear 为 0）
layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D resultTex;   // cmaa_result（AA 结果 + alpha 标记）
layout(binding = 1) uniform sampler2D toneTex;     // pass:tonemap（原色 fallback）

void main() {
    vec4 r = texture(resultTex, fragTexCoord);
    vec3 t = texture(toneTex, fragTexCoord).rgb;
    outColor = vec4(mix(t, r.rgb, step(0.5, r.a)), 1.0);
}
