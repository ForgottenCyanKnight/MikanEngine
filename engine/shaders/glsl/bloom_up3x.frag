#version 450
// 双三次 B 样条放大（每级只放大 3 倍，折痕小）+ 同级 ds 层回加（dual 累积，亮度连续分布）
// 各级贡献逐级融合——消除「4 级叠加」的可见台阶
layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 fragColor;
layout(binding = 0) uniform sampler2D inputTex;   // 上一级 us 输出（小 3 倍）
layout(binding = 1) uniform sampler2D detailTex;  // 同尺寸 ds 层（dual 累积）

vec3 BiCubicBSpline(sampler2D tex, vec2 P) {
    const float inv6 = 1.0 / 6.0;
    vec2 texSize = vec2(textureSize(tex, 0));
    vec2 InvSize = 1.0 / texSize;
    vec2 UV = P * texSize;
    vec2 tc = floor(UV - 0.5) + 0.5;
    vec2 f = UV - tc;
    vec2 f2 = f * f;
    vec2 f3 = f2 * f;
    vec2 of = 1.0 - f;
    vec2 of2 = of * of;
    vec2 of3 = of2 * of;
    vec2 w0 = inv6 * of3;
    vec2 w1 = inv6 * (4.0 + 3.0 * f3 - 6.0 * f2);
    vec2 w2 = inv6 * (4.0 + 3.0 * of3 - 6.0 * of2);
    vec2 w3 = inv6 * f3;
    vec4 Weight;
    vec4 Sample;
    Weight.xy = w0 + w1;
    Weight.zw = w2 + w3;
    Sample.xy = tc - 1.0 + w1 / Weight.xy;
    Sample.zw = tc + 1.0 + w3 / Weight.zw;
    Sample *= InvSize.xyxy;
    vec4 sampleWeight = Weight.xzxz * Weight.yyww;
    vec3 Ctl = texture(tex, Sample.xy).rgb * sampleWeight.x;
    vec3 Ctr = texture(tex, Sample.zy).rgb * sampleWeight.y;
    vec3 Cbl = texture(tex, Sample.xw).rgb * sampleWeight.z;
    vec3 Cbr = texture(tex, Sample.zw).rgb * sampleWeight.w;
    return Ctl + Ctr + Cbl + Cbr;
}

void main() {
    vec3 bloom = BiCubicBSpline(inputTex, fragTexCoord);          // 3x 升采样放大（双三次平滑）
    bloom += texture(detailTex, fragTexCoord).rgb * 0.5;          // dual 累积：同级 ds 回加
    fragColor = vec4(bloom, 1.0);
}
