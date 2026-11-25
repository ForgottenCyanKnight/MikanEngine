// TAA input: current HDR color, history, motion vectors, and depth.
// Reprojection uses the nearest-depth motion vector in a 3x3 neighborhood;
// neighborhood color clipping and motion-adaptive blending reduce ghosting.
#version 450

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uCurr;     // 当前帧 HDR（线性）
layout(set = 0, binding = 1) uniform sampler2D uHistory;  // 上帧 TAA 输出（HDR 线性）
layout(set = 0, binding = 2) uniform sampler2D uMotion;   // G-Buffer 附件3 运动向量（Nearest——逐像素偏移）
layout(set = 0, binding = 3) uniform sampler2D uDepth;    // G-Buffer 深度（Nearest——NDC z 直存，近处小）
layout(set = 0, binding = 4) uniform sampler2D uCloudMask; // cloud_view alpha：1=无云，越低表示云越厚

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

    // Accumulate neighborhood color bounds and select the nearest-depth UV.
    float minDepth = 1e30;
    vec2 bestUv = vUV;
    vec3 nMin = vec3(1e30);
    vec3 nMax = vec3(-1e30);
    vec3 curr = vec3(0.0);
    float neighborhoodCloudAmount = 0.0;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            vec2 uv = vUV + vec2(dx, dy) * texel;
            vec3 c = texture(uCurr, uv).rgb;
            neighborhoodCloudAmount = max(
                neighborhoodCloudAmount,
                1.0 - clamp(texture(uCloudMask, uv).a, 0.0, 1.0));
            nMin = min(nMin, c);
            nMax = max(nMax, c);
            float d = texture(uDepth, uv).r;
            if (d < minDepth) { minDepth = d; bestUv = uv; }
            if (dx == 0 && dy == 0) curr = c;
        }
    }

    // 重投影：用最近深度像素的运动（遮挡边缘修复——中心 velocity 可能指向不存在的表面）
    vec2 motion = texture(uMotion, bestUv).rg;   // Motion is stored as a UV-space delta.
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
    // 云没有可靠的几何 motion vector；用当前/邻域云 mask 提高云及云缘
    // 的当前帧权重，避免 TAA 把上一帧的云拖到新位置。
    float cloudAlpha = smoothstep(0.02, 0.35, neighborhoodCloudAmount) * 0.45;
    alpha = max(alpha, cloudAlpha);

    // 云是天空中的独立遮挡层，不是带几何 motion vector 的表面。
    // 云覆盖天空时禁止复用旧天空历史，否则云出现后上一帧的太阳盘会
    // 在当前云下继续可见；cloud_view 自己已经负责云的时域降噪。
    float centerCloudTransmittance = clamp(texture(uCloudMask, vUV).a, 0.0, 1.0);
    bool cloudySky = minDepth >= 0.9999 && centerCloudTransmittance < 0.995;
    if (cloudySky) {
        alpha = 1.0;
        valid = 0.0;
    }

    vec3 result = mix(clamped, curr, alpha);
    result = mix(curr, result, valid);   // 历史无效 → 当前帧

    outColor = vec4(result, 1.0);
}
