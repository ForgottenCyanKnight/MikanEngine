// ===== TAA（时间抗锯齿）——HDR 空间、bloom 之前（2026-08-17 depth-guided 重投影版）=====
// 输入：当前帧 HDR（gtao_apply 输出）+ 历史（上帧 TAA 输出）+ 运动向量 + 深度
// 重投影 = IDKEngine GetResolveData 语义：3×3 邻域选"最近深度像素"的运动向量（bestUv）——
// 遮挡边缘/新出现的表面，中心像素 velocity 不可靠（对应历史不存在），用最近表面邻域的运动替代；
// 3×3 RGB clamp 防鬼影 + α 运动自适应 + 出界回退
#version 450

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uCurr;     // 当前帧 HDR（线性）
layout(set = 0, binding = 1) uniform sampler2D uHistory;  // 上帧 TAA 输出（HDR 线性）
layout(set = 0, binding = 2) uniform sampler2D uMotion;   // G-Buffer 附件3 运动向量（Nearest——逐像素偏移）
layout(set = 0, binding = 3) uniform sampler2D uDepth;    // G-Buffer 深度（Nearest——NDC z 直存，近处小）

layout(push_constant) uniform PC {
    vec4 cameraPos;
    vec4 sunDir;
    vec4 lightColor;
    // y/z = 上一帧 jitter - 当前帧 jitter（NDC）；w = 历史帧是否有效
    vec4 frameInfo;
} pc;

void main()
{
    vec2 texel = fwidth(vUV);   // 全屏 quad 线性 UV → 1/宽, 1/高

    // GetResolveData（IDKEngine）：3×3 邻域——① 累积 RGB min/max ② 选最近深度像素 UV（bestUv）
    float minDepth = 1e30;
    vec2 bestUv = vUV;
    vec3 nMin = vec3(1e30);
    vec3 nMax = vec3(-1e30);
    vec3 curr = vec3(0.0);
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            vec2 uv = vUV + vec2(dx, dy) * texel;
            vec3 c = texture(uCurr, uv).rgb;
            nMin = min(nMin, c);
            nMax = max(nMax, c);
            float d = texture(uDepth, uv).r;
            if (d < minDepth) { minDepth = d; bestUv = uv; }
            if (dx == 0 && dy == 0) curr = c;
        }
    }

    // 重投影：用最近深度像素的运动（遮挡边缘修复——中心 velocity 可能指向不存在的表面）
    vec2 motion = texture(uMotion, bestUv).rg;   // IDKEngine 语义：UV 空间差（model.vert 无 +0.5 偏置）
    vec2 jitterDeltaUV = pc.frameInfo.yz * 0.5;
    vec2 histUV = vUV - motion + jitterDeltaUV;

    // 历史 UV 出界 → 无效（用当前帧）
    float valid = step(0.0, histUV.x) * step(histUV.x, 1.0) *
                  step(0.0, histUV.y) * step(histUV.y, 1.0) * pc.frameInfo.w;

    vec3 hist = texture(uHistory, histUV).rgb;
    // RGB 逐通道 clamp 到邻域范围（无色度泄漏）
    vec3 clamped = clamp(hist, nMin, nMax);

    // α：静止 0.08（强累积）→ 运动 0.5（信任当前帧）；|motion| 是 UV 差（1.0 = 全屏）
    float alpha = mix(0.08, 0.5, clamp(length(motion) * 8.0, 0.0, 1.0));

    vec3 result = mix(clamped, curr, alpha);
    result = mix(curr, result, valid);   // 历史无效 → 当前帧

    outColor = vec4(result, 1.0);
}
