#version 450
// 2026-08-12：demo CutTex 式提取（第一 pass——四小块/多尺度区域采样）→ 半尺寸
// 2026-08-12 用户：雾蒙蒙根因=无 threshold 全屏模糊——加亮部提取（HDR 光晕感；暗部不参与模糊）
// 2026-08-13：移除 CutTex 四区域拼贴——区域采样破坏提取均匀性；
// 全图采样 + 阈值提取（多尺度由下游真实金字塔提供）
#define BLOOM_THRESHOLD 0.4   // 线性 HDR 阈值（2026-08-12 用户：自发光要明显泛光——0.4 让 emissive≈albedo 参与；可调）
layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 fragColor;
layout(binding = 0) uniform sampler2D inputTex;   // composite（全尺寸线性 HDR）

void main() {
    vec3 color = texture(inputTex, fragTexCoord).rgb;   // 全图归一化采样（CutTex 区域拼贴已移除）
    //color = max(color - vec3(BLOOM_THRESHOLD), 0.0);    // 亮部提取（soft knee 简化：硬阈值）
    fragColor = vec4(color, 1.0);
}
