#version 450
// 2026-08-13：核切换——默认高斯 3×3（sigma=1.0，各向同性平滑）
// 宏变体：-DBLOOM_KERNEL_DUAL → Kawase dual（扩散 ~2 texel）
//         -DBLOOM_KERNEL_GAUSS5 → 高斯 5×5（sigma=1.0，扩散 ±2 texel，更平滑）
// 双输入融合：当前级 ds 图（全量）+ 上一级 up 图（×0.5 衰减——外层亮度逐级减半）
// 变体 spv：bloom_up2x.frag.spv（默认）/ bloom_up2x_dual.frag.spv / bloom_up2x_gauss5.frag.spv
layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 fragColor;
layout(binding = 0) uniform sampler2D inputTex;    // 当前级降采样图（本输出尺寸）
layout(binding = 1) uniform sampler2D prevTex;     // 上一级升采样图（半尺寸，texel 间距为本级 2 倍）

#ifdef BLOOM_KERNEL_DUAL
// Kawase dual 升采样核：四对角(±1,±1)×2 + 四轴向(±2,0)(0,±2)×1，÷12——有效半径 ~2 texel
vec3 Kernel(sampler2D tex, vec2 uv, vec2 px) {
    vec3 sum = vec3(0.0);
    sum += texture(tex, uv + vec2(-px.x,  px.y)).rgb * 2.0;
    sum += texture(tex, uv + vec2( px.x,  px.y)).rgb * 2.0;
    sum += texture(tex, uv + vec2(-px.x, -px.y)).rgb * 2.0;
    sum += texture(tex, uv + vec2( px.x, -px.y)).rgb * 2.0;
    sum += texture(tex, uv + vec2(0.0,  px.y * 2.0)).rgb;
    sum += texture(tex, uv + vec2(px.x * 2.0, 0.0)).rgb;
    sum += texture(tex, uv + vec2(0.0, -px.y * 2.0)).rgb;
    sum += texture(tex, uv + vec2(-px.x * 2.0, 0.0)).rgb;
    return sum * 0.0833;
}
#elif defined(BLOOM_KERNEL_GAUSS5)
// 高斯 5×5（sigma=1.0，标准归一化核）：中心 0.150、±1 正交 0.095、±1 对角 0.060、
// ±2 正交 0.024、±2/±1 混合 0.015、±2 对角 0.004——扩散 ±2 texel，能量归一 ≈1.0
const float K[25] = float[](
    0.003765, 0.015019, 0.023792, 0.015019, 0.003765,
    0.015019, 0.059912, 0.094907, 0.059912, 0.015019,
    0.023792, 0.094907, 0.150342, 0.094907, 0.023792,
    0.015019, 0.059912, 0.094907, 0.059912, 0.015019,
    0.003765, 0.015019, 0.023792, 0.015019, 0.003765
);
vec3 Kernel(sampler2D tex, vec2 uv, vec2 px) {
    vec3 sum = vec3(0.0);
    for (int y = -2; y <= 2; y++)
        for (int x = -2; x <= 2; x++)
            sum += texture(tex, uv + vec2(float(x), float(y)) * px).rgb
                 * K[(y + 2) * 5 + (x + 2)];
    return sum;
}
#else
// 高斯 3×3（sigma=1.0）：中心 0.195、正交邻 0.123、对角 0.078——扩散 ±1 texel，能量归一 ≈1.0
const float K[9] = float[](
    0.077847, 0.123317, 0.077847,
    0.123317, 0.195346, 0.123317,
    0.077847, 0.123317, 0.077847
);
vec3 Kernel(sampler2D tex, vec2 uv, vec2 px) {
    vec3 sum = vec3(0.0);
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++)
            sum += texture(tex, uv + vec2(float(x), float(y)) * px).rgb
                 * K[(y + 1) * 3 + (x + 1)];
    return sum;
}
#endif

void main() {
    vec2 uv = fragTexCoord;
    vec2 pxCurr = 1.0 / vec2(textureSize(inputTex, 0));   // 本级 texel
    vec2 pxPrev = 1.0 / vec2(textureSize(prevTex, 0));    // 上一级 texel = 本级 texel 的 2 倍（半尺寸图）
    vec3 color = Kernel(inputTex, uv, pxCurr);            // 当前级 ds（新细节，全量）
    color += Kernel(prevTex, uv, pxPrev);                  // 上一级 up（全量——2026-08-13：去掉 ×0.5 衰减，
                                                           //   恢复 CasualBloom 原版 1:1 相加，能量由 BLOOM_STRENGTH 控制）
    fragColor = vec4(color, 1.0);
}
