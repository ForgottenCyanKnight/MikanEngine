#version 450

// 场景反射探针的「解析」片元：把探针视图（一个真正的独立相机视图）已经合成好的
// HDR 结果搬进 cubemap 的某一面，**并按深度写出覆盖掩码 alpha**。
//
// 为什么需要这一趟而不是 vkCmdCopyImage：探针视图走的是与 SceneView/GameView
// 完全相同的管线，输出是 RenderTarget 的 composite 附件（B10G11R11_UFLOAT_PACK32，
// 无 alpha）；而 cubemap 面是 RGBA16F 且需要 alpha 表示「该方向有没有场景内容」
// （水面合成端用 mix(skyRefl, probe.rgb, probe.a) 做回退）。两者 texel 尺寸不同、
// 通道数不同，拷贝/blit 都被格式兼容性挡住，因此用一次采样上屏。
//
// ★覆盖掩码必须按深度区分天空（2026-09-22 修复"水面倒影丢云"）：
//   探针的合成 shader（fullscreen.frag）的绑定表里**没有云纹理** —— 它的天空分支
//   只采纯大气 skyRT（体积云是独立 pass 渲进 cloudRT、再合进 skyCube 的）。所以探针
//   cubemap 的天空方向天生无云。若这里无条件写 alpha=1，水面端就会拿"无云的天空"
//   完全盖掉 skyCube（含云 + 大气 + 8mip GGX 预滤波）⇒ 表现为水面反射里的云消失。
//   正确语义：alpha=1 = 该方向有**场景几何**；天空方向 alpha=0 ⇒ 水面回退 skyCube。
//
// 顶点着色器复用 fullscreen.vert（全屏三角 + fragTexCoord，左上原点）。

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

// 探针视图的合成附件（HDR 线性，未 tonemap；反射需要 HDR 才能保住太阳/天空高光）
layout(binding = 0) uniform sampler2D srcComposite;
// 探针视图的深度附件（布局 DEPTH_STENCIL_READ_ONLY_OPTIMAL，NEAREST 采样）。
// 天空判据与 fullscreen.frag 的天空分支严格一致：深度 clear 值为 1.0、非反向 Z。
layout(binding = 1) uniform sampler2D srcDepth;

void main() {
    // 垂直翻转（2026-09-27 修复探针相对 IBL 上下颠倒）：探针面渲染的合成
    // 附件用 up=(0,-1,0) + Y 翻转投影录制——附件顶行是世界 -Y；而 cubemap
    // 采样约定面 v=0 是世界 +Y。解析时翻 y 把附件底行对到面顶行，否则探针
    // 整体上下颠倒：水面反射在天空方向采样到地面（"穿过水看岸上"）。
    // 只翻转采样 UV，全屏三角形几何/绕序不变，无剔除副作用。
    vec2 resolveUV = vec2(fragTexCoord.x, 1.0 - fragTexCoord.y);
    vec3 color = texture(srcComposite, resolveUV).rgb;
    float depth = texture(srcDepth, resolveUV).x;
    // 天空像素不写覆盖：把该方向让给 skyCube（唯一含体积云的反射来源）。
    float sceneMask = depth >= 0.9999 ? 0.0 : 1.0;
    outColor = vec4(color, sceneMask);
}
