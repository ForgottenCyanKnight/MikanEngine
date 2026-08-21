#version 450

// 2D Canvas SDF/MSDF 片元着色器：距离场阈值采样（screenPxRange 精确 + fwidth 兜底混合）
// 图集纹理 RGBA：
//   SDF  ：R/G/B 三通道同值存距离（0-1，字形边缘在 0.5 = onedge 128/255）
//   MSDF ：R/G/B 三通道存多通道距离场（median 重建真距离，角点锐利）
// 抗锯齿（双模式混合，按 screenPxRange 权重）：
//   大字号：screenPxRange ≥ 1 屏幕像素 → 精确公式 alpha = clamp(range*(sd-0.5)+0.5)，固定 1px 边缘
//   小字号：screenPxRange < 1 时纹素放大，精确公式失效 → fwidth 屏幕导数自适应，防止模糊
layout(binding = 0) uniform sampler2D tex;

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec4 fragColor;
layout(location = 2) in float fragScreenPxRange;

layout(location = 0) out vec4 outColor;

float median(float r, float g, float b) {
    return max(min(r, g), min(max(r, g), b));
}

void main() {
    vec3 msd = texture(tex, fragUV).rgb;
    float sd = median(msd.r, msd.g, msd.b);   // SDF 图集 RGB 同值 → median = 原值，兼容

    // 精确模式：过渡带 = 1 屏幕像素（screenPxRange ≥ 1 时有效）
    // 修复：screenPxRange < 1（小字号）时原公式在 sd=0（字形外背景）处 alpha=0.5-range/2 残留半透明，
    //       MSDF quad 含 padding 过渡带 → 整块淡色。钳到 ≥1 使背景 alpha 恒为 0。
    float effectiveRange = max(fragScreenPxRange, 1.0);
    float preciseAlpha = clamp(effectiveRange * (sd - 0.5) + 0.5, 0.0, 1.0);
    // 兜底模式：fwidth 屏幕导数自适应（纹素放大时防模糊）
    float w = fwidth(sd);
    float fallback = smoothstep(0.5 - w, 0.5 + w, sd);
    // 混合权重：range ≤ 0.5 全 fwidth，≥ 1.5 全精确，中间平滑过渡
    float k = smoothstep(0.5, 1.5, fragScreenPxRange);
    float alpha = mix(fallback, preciseAlpha, k);

    outColor = vec4(fragColor.rgb, fragColor.a * alpha);
}
