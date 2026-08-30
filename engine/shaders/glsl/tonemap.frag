#version 450

// tonemap pass（PostProcessChain）——合成 subpass 输出（LogLuv32 编码 → 解码回线性 HDR）→ tonemap → LDR
// 2026-08-11 用户拍板：同时保留三种写实 tonemap，切换查看效果：
//   0 = AgX（Godot 4.4 生产级：EaryChow sigmoid 多项式 + Rec2020 合并矩阵 + pow2.4）
//   1 = FMDS ACES2（完整 ACES：ACESInputMat + RRT/ODT fit + ACESOutputMat）
//   2 = Bruneton 官方 demo 曲线（demo.glsl：pow(1-exp(-x*exposure), 1/2.2)——物理风格早饱和、无胶片肩部）
// 切换：改 TONEMAP_MODE 0/1/2
// 2026-08-16：FXAA 3.11 合并进本 pass（FMDS RN4 v0.11 post_fxaa.fsh 移植）——
//   不单独开 FXAA pass（省一次全屏读写）；边缘检测/步进 luma 仅用"非 bloom"颜色
//   （tonemap(composite)），取色用完整 LDR（含 bloom）——边缘由 composite 决定、bloom 光晕保留。

#define TONEMAP_MODE 0   // 2026-08-11 PBR 溢出排查：切回 AgX（大值收敛无伪色——ACES2 大值色度分裂嫌疑）

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

// 2026-08-13：CasualBloom 式串行链最终图（binding 0 = bloom_up1，0.5x）+ composite（binding 1 = gtao_apply 全分辨率）
layout(binding = 0) uniform sampler2D inputTex;
layout(binding = 1) uniform sampler2D bloomTex;

// ===== LogLuv32 解码（FMDS basic.inc colors_LogLuv32ToSRGB——2026-08-11 修正版；解码输出线性 sRGB 色域）=====
const mat3 COLORS_LOGLUV32_INVERSE_M = mat3(
    6.0014, -2.7008, -1.7996,
    -1.3320, 3.1029, -5.7721,
    0.3008, -1.0882, 5.6268
);

vec3 colors_LogLuv32ToSRGB(in vec4 vLogLuv) {
    if (all(lessThanEqual(vLogLuv, vec4(0.0)))) {
        return vec3(0.0);
    }
    float Le = vLogLuv.z * 255.0 + vLogLuv.w;
    vec3 Xp_Y_XYZp;
    Xp_Y_XYZp.y = exp2((Le - 127.0) / 2.0);
    Xp_Y_XYZp.z = Xp_Y_XYZp.y / vLogLuv.y;
    Xp_Y_XYZp.x = vLogLuv.x * Xp_Y_XYZp.z;
    vec3 vRGB = COLORS_LOGLUV32_INVERSE_M * Xp_Y_XYZp;
    return max(vRGB, vec3(0.0));   // 线性 RGB（sRGB 色域）
}

// ===== AgX（Godot 4.4 tonemap.glsl tonemap_agx——EaryChow AgX_LUT_Gen 的 sigmoid 多项式近似；Blender AgX 同源）=====
// 2026-08-17：AGX_EXPOSURE——IBL 全链路物理化后输入为物理辐照度（~0.05-2，太阳 1 / 天空 0.5 / 阴影 0.1）。
// 曝光 3 为物理中灰基准（太阳→0.87 亮、天空→0.62 中亮、阴影→0.46 中暗，对比保留）；
// 勿用 10（把阴影 0.1 拉到 0.62 全画面亮、直射饱和——对比压平）；勿用 1（天空偏暗）。3 偏暗 → 5（用户拍板）
const float AGX_EXPOSURE = 5.0;
vec3 agx_contrast_approx(vec3 x) {
    vec3 x2 = x * x;
    vec3 x4 = x2 * x2;
    return 0.021 * x + 4.0111 * x2 - 25.682 * x2 * x + 70.359 * x4 - 74.778 * x4 * x + 27.069 * x4 * x2;
}
vec3 tonemap_agx(vec3 color) {
    // 2026-08-17：曝光在 log2 前应用（Godot AgX exposure 语义）——物理亮度 × AGX_EXPOSURE 回显示量级
    color = max(color * AGX_EXPOSURE, 2e-10);   // 防 log2(0)；负值防 inset 后变暗
    // 合并矩阵：线性 sRGB→Rec2020 + Blender AgX inset（行和≈1 防转置）
    const mat3 srgb_to_rec2020_agx_inset_matrix = mat3(
        0.54490813676363087053, 0.14044005884001287035, 0.088827411851915368603,
        0.37377945959812267119, 0.75410959864013760045, 0.17887712465043811023,
        0.081384976686407536266, 0.10543358536857773485, 0.73224999956948382528);
    // 合并矩阵：AgX outset 逆 + Rec2020→线性 sRGB
    const mat3 agx_outset_rec2020_to_srgb_matrix = mat3(
        1.9645509602733325934, -0.29932243390911083839, -0.16436833806080403409,
        -0.85585845117807513559, 1.3264510741502356555, -0.23822464068860595117,
        -0.10886710826831608324, -0.027084020983874825605, 1.402665347143271889);
    // MIDDLE_GRAY=0.18 曝光基准
    const float min_ev = -12.4739311883324;   // log2(pow(2,-10)*0.18)
    const float max_ev = 4.02606881166759;    // log2(pow(2,+6.5)*0.18)

    color = max(color, 2e-10);   // 防 log2(0)；负值防 inset 后变暗
    color = srgb_to_rec2020_agx_inset_matrix * color;
    color = clamp(log2(color), min_ev, max_ev);
    color = (color - min_ev) / (max_ev - min_ev);
    color = agx_contrast_approx(color);
    color = pow(color, vec3(2.4));   // 关键：sigmoid 输出是对数空间，展开回线性
    color = agx_outset_rec2020_to_srgb_matrix * color;
    return color;
}

// ===== FMDS RN4 v0.11 ACES tonemap（basic.inc L1118-1143，原样照抄）=====
// 输入非负线性 Rec.709，输出线性 Rec.709 [0,1]（saturate）
const mat3 ACESInputMat = mat3(
    0.59719, 0.35458, 0.04823,
    0.07600, 0.90834, 0.01566,
    0.02840, 0.13383, 0.83777
);

// ODT_SAT => XYZ => D60_2_D65 => sRGB
const mat3 ACESOutputMat = mat3(
     1.60475, -0.53108, -0.07367,
    -0.10208,  1.10813, -0.00605,
    -0.00327, -0.07276,  1.07602
);

vec3 RRTAndODTFit(vec3 v) {
    vec3 a = v * (v + 0.0245786) - 0.0;
    vec3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
    return a / b;
}

vec3 tonemapACES2(vec3 color) {
    color = color * ACESInputMat;
    color = RRTAndODTFit(color);
    color = color * ACESOutputMat;
    return clamp(color, 0.0, 1.0);   // saturate（GLSL 无内置）
}

// ===== Bruneton 官方 demo 曲线（demo.glsl 末行：pow(1-exp(-x*exposure), 1/2.2)）=====
const float BRUNETON_EXPOSURE = 1.5;   // mikan 曝光系数（skyRT 已乘 pc.sunDir.w=10，此处只需轻度）

vec3 tonemap_bruneton(vec3 color) {
    return vec3(1.0) - exp(-color * BRUNETON_EXPOSURE);
}

// ===== sRGB EOTF（标准分段编码——AgX 分支用）=====
vec3 linear_to_srgb(vec3 color) {
    color = clamp(color, vec3(0.0), vec3(1.0));
    const vec3 a = vec3(0.055f);
    return mix((vec3(1.0f) + a) * pow(color.rgb, vec3(1.0f / 2.4f)) - a, 12.92f * color.rgb, lessThan(color.rgb, vec3(0.0031308f)));
}

// ===== HSPE 式 bloom 合成（2026-08-13）：4 级并行双三次采样 + 递减权重
// 权重 0.4/0.3/0.2/0.1（近级中心强、远级外围弱）——连续剖面且由强到弱，无扁平平台
#define BLOOM_STRENGTH 0.2   // 2026-08-13：无阈值全图 bloom——能量累积大，合成亮度调低（0.4 起步，按观感微调）

vec3 getbloom(vec2 uv) {
    vec3 comp = texture(bloomTex, uv).rgb;   // composite 原色（binding 1）
    // 串行 up 链最终 0.5x 图：双线性放大到全屏（仅 2 倍放大，无需双三次——双三次是 HSPE 一步放大 27-81 倍的遗留）
    vec3 bloom = texture(inputTex, uv).rgb;
    // 2026-08-13：bloom 保持 HDR 直加（不单独 tonemap/钳制——颜色空间留给最终 AgX 统一处理；
    // up 链已在 bloom_up2x 内 ×0.5 控制能量累积，不会溢出）
    return comp + bloom * BLOOM_STRENGTH;
}

// ===== 线性 HDR → LDR（tonemap + gamma 编码；FXAA 复用——邻域像素现场计算，省独立 FXAA pass）=====
vec3 tonemapLinear(vec3 linear) {
    vec3 mapped;
#if TONEMAP_MODE == 0
    mapped = tonemap_agx(linear);                  // AgX 写实 tonemap（胶片肩部 + 色度映射）
    mapped = linear_to_srgb(mapped);               // sRGB EOTF 编码
#elif TONEMAP_MODE == 1
    mapped = tonemapACES2(linear);                 // FMDS ACES2（完整 ACES RRT/ODT）
    mapped = pow(mapped, vec3(1.0 / 2.2));         // lin2gam（FMDS GAMMA=2.2）
#else
    mapped = tonemap_bruneton(linear);             // Bruneton 官方 1-exp（物理风格）
    mapped = pow(mapped, vec3(1.0 / 2.2));         // lin2gam
#endif
    return clamp(mapped, 0.0, 1.0);
}

float bayer2(vec2 a) { a = floor(a); return fract(a.x / 2.0 + a.y * a.y * 0.75); }
float bayer4(vec2 a) { return bayer2(0.5 * a) * 0.25 + bayer2(a); }
float bayer8(vec2 a) { return bayer4(0.5 * a) * 0.25 + bayer2(a); }

void main() {
    vec3 linear = getbloom(fragTexCoord);   // 2026-08-11：FMDS BLOOM_4 合成（composite + 5 octave 金字塔模糊）
    vec3 mapped = tonemapLinear(linear);

    // 2026-08-11 顺序修正：抖色在 clamp 后会把高光边界（mapped=1.0）的 +0.5/255 截断——抖动不对称。
    // 抖色后再 clamp（UNORM 附件自动钳制——边界抖动完整）
    mapped += (bayer8(gl_FragCoord.xy) - 0.5) * (1.0 / 255.0);
    mapped = clamp(mapped, 0.0, 1.0);
    outColor = vec4(mapped, 1.0);
}
