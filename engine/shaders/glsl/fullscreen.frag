#version 450
#extension GL_GOOGLE_include_directive : require
#include "atmo_common.glsl"

// 全屏四边形片段着色器（合成 subpass 1）
// 经 input attachment 从 tile 内读取 G-Buffer 颜色0/深度/法线（subpassLoad，像素 1:1，不写回主存）
// 深度 == 远平面（>=0.9999）的像素判定为天空区域：
//   2026-08-11 per-pixel：每像素 GetSkyRadiance（查 transmittance/scattering LUT + 相函数——无 128x64 全景量化）+ 全分辨率太阳盘
// 输出线性 HDR（天空+非天空一致），tonemap/gamma/抖动由链尾 tonemap pass 统一完成
// 注：云层 LUT 混合已移除（2026-08-05，画面黑色排查）；PI 由 atmo_common.glsl 提供（更精确）

layout(location = 0) in vec2 fragTexCoord;

layout(location = 0) out vec4 outColor;   // 附件4 light（合成输出——emissive 从 G-Buffer 材质附件重建，不 MRT 分离）

// 离屏 G-Buffer 采样方式——宏编译分支（CMake -DMIKAN_SUBPASS_LOAD）：
//   移动端（__ANDROID__ 构建）：subpassLoad（tile 内存直接读，省 G-Buffer 写回+读回带宽）
//   桌面：texture 普通采样（本机 NVIDIA 驱动 subpassLoad 返回 0，实测 texture 正常；桌面 IMR 无 tile 优势）
// 注意：input attachment 引用布局（render pass）与 descriptor 布局由 C++ 侧按相同宏分支设置
#ifdef MIKAN_SUBPASS_LOAD
layout(input_attachment_index = 0, set = 0, binding = 0) uniform subpassInput sceneColor;   // G-Buffer 颜色0（albedo）
layout(input_attachment_index = 1, set = 0, binding = 1) uniform subpassInput sceneDepth;   // 深度（天空区域判定）
layout(input_attachment_index = 2, set = 0, binding = 3) uniform subpassInput sceneNormal;  // 法线（R16G16B16A16_SFLOAT 完整 xyz）
layout(input_attachment_index = 3, set = 0, binding = 4) uniform subpassInput sceneMaterial; // 2026-08 材质（附件3：xyz=metallic/roughness/ao，w=自发光强度）
#define MIKAN_SAMPLE_COLOR  subpassLoad(sceneColor)
#define MIKAN_SAMPLE_DEPTH  subpassLoad(sceneDepth).x
#define MIKAN_SAMPLE_NORMAL subpassLoad(sceneNormal)
#define MIKAN_SAMPLE_MATERIAL subpassLoad(sceneMaterial)
#else
layout(set = 0, binding = 0) uniform sampler2D sceneColor;   // G-Buffer 颜色0（albedo）
layout(set = 0, binding = 1) uniform sampler2D sceneDepth;   // 深度（天空区域判定）
layout(set = 0, binding = 3) uniform sampler2D sceneNormal;  // 法线（R16G16B16A16_SFLOAT 完整 xyz）
layout(set = 0, binding = 4) uniform sampler2D sceneMaterial; // 2026-08 材质（附件3：xyz=metallic/roughness/ao，w=自发光强度）
#define MIKAN_SAMPLE_COLOR  texture(sceneColor, fragTexCoord)
#define MIKAN_SAMPLE_DEPTH  texture(sceneDepth, fragTexCoord).x
#define MIKAN_SAMPLE_NORMAL texture(sceneNormal, fragTexCoord)
#define MIKAN_SAMPLE_MATERIAL texture(sceneMaterial, fragTexCoord)
#endif
// 全景天空图（RGBA16F 线性 HDR，普通纹理采样上采样——外部图像，非本 render pass 附件）
layout(set = 0, binding = 2) uniform sampler2D skyRT;   // 2026-08-11：主天空圆柱投影（128×64 LogLuv32）——cubemap IBL 设施走独立着色器
// 2026-08-12：IBL 天空环境 cubemap（各向同性——反射/漫反射方向直接采样，无柱面极区聚集/地平线重影）
layout(set = 0, binding = 8) uniform samplerCube skyCube;
// 2026-08-12：辐照度图（diffuse IBL——半球积分，非金属球环境光均匀无反射感）
layout(set = 0, binding = 9) uniform samplerCube skyIrradiance;
layout(set = 0, binding = 10) uniform ShCoefs { vec4 shCoefs[9]; } shUBO;   // 2026-08-12：SH 辐照度系数（RGB×9，已乘卷积核 A_l×π）

// 2026-08-13：点光源——binding 11 UBO，std140 对齐
// C++ 侧 GpuPointLight = 3×vec4（position_range / color_intensity / shadow_info），32 槽 + int count + padding = 1552B
#define MAX_POINT_LIGHTS 32
struct PointLight {
    vec4 position_range;    // xyz = 世界位置，w = 衰减半径（range）
    vec4 color_intensity;   // rgb = 颜色，w = 强度
    vec4 shadow_info;       // 2026-08-13：x = 阴影槽（-1 无阴影），yz 保留
};
layout(set = 0, binding = 11) uniform PointLightsUBO {
    PointLight lights[MAX_POINT_LIGHTS];
    int count;
} pointLights;

// 2026-08-13 cluster 光源剔除（第二步）：binding 12 SSBO——cluster grid
// 与 cluster_cull.comp 严格一致：12×12 屏幕 tile × 24 深度切片（指数分割），std430
#define CLUSTER_X 12
#define CLUSTER_Y 12
#define CLUSTER_Z 24
#define CLUSTER_COUNT 3456
struct Cluster {
    vec4 minPoint;
    vec4 maxPoint;
    uint count;
    uint pad;
    uint lightIndices[MAX_POINT_LIGHTS];
};
layout(set = 0, binding = 12) readonly buffer ClusterGrid {
    vec4 clusterParams;   // near, far, screenW, screenH
    Cluster clusters[CLUSTER_COUNT];
} clusterGrid;

// 2026-08-13：点光源阴影 cubemap 数组（PointShadowRenderer：MAX_SHADOW_LIGHTS × 6 面，线性深度 dist/range，D16 512²）
// 2026-08-15：改为 shadow sampler（samplerCubeArrayShadow，硬件 2×2 双线性比较 PCF——HSPE 同款）
layout(set = 0, binding = 13) uniform samplerCubeArrayShadow shadowCubeMaps;
#define MAX_SHADOW_LIGHTS 8
#define SHADOW_MAP_SIZE 256.0

// ===== 2026-08-14：CSM 方向光阴影（参考 LimitlessSquareEngine：级联选择 + 边缘混合 + 3×3 高斯 PCF）=====
// binding 14 = 阴影 2D array（每槽 4 层 2048² D16，默认 NDC 深度——正交线性，view 由 C++ 按视口绑定对应槽）
// binding 15 = 级联 UBO（std140，288B：4×mat4 + splitDepths(vec4 存 4 个 far) + params）
// ⚠️ 2026-08-14 回滚：CPU GpuCsmData.splitDepths 是单个 vec4（4 个 far 分量）——shader 必须用 vec4 + 分量索引；
// 曾误改为 vec4[4]（336B）→ 与 CPU 288B 错位 → csmParams 读垃圾 → 染色/阴影全失效
#define MAX_CSM_CASCADES 4
// 2026-08-15：shadow sampler（sampler2DArrayShadow，硬件 2×2 双线性比较 PCF——HSPE 同款；compareOp=LESS（Vulkan 语义 ref < sampled → d > ref → 亮））
layout(set = 0, binding = 14) uniform sampler2DArrayShadow csmShadowMaps;
layout(set = 0, binding = 15) uniform CsmUBO {
    mat4 shadowMatrices[MAX_CSM_CASCADES];   // 世界 → 光 NDC
    vec4 splitDepths;                        // 每级联 far 存分量（view depth；无效级联 = -1）
    vec4 csmParams;                          // x = 有效级联数，y = 启用（>0）
} csm;
// 2026-08-15：split-sum BRDF LUT（Fermion/learnopengl——128×128，x=NoV y=roughness → (A,B)，F·A+B）
layout(set = 0, binding = 16) uniform sampler2D brdfLUT;

// ⚠️ 2026-08-14 排查：最基础单张 shadowmap（LearnOpenGL 式，独立于 CSM 设施）——binding 16/17
// ⚠️ 2026-08-14 调试开关：CSM 级联染色（c0=红 c1=绿 c2=蓝 c3=黄，未选中=灰）——0 关闭 / 1 打开
#define CSM_CASCADE_DEBUG 0

// PCF 阴影因子（统一滤波内核 + 切向基偏移；线性深度约定：存 dist/range）
float ShadowBlurRadius();   // 前向声明（定义在文件后部；点光源在文件前部先调用）
float ShadowFilterImpl(vec2 sampleBase, int layer, float currentDepth, float bias, float texel, float radius,
                       bool isPoint, vec3 baseDir, vec3 T, vec3 B);   // 统一滤波内核（定义在文件中部）
float PointShadowFactor(vec3 worldPos, vec3 lightPos, float range, int lightIndex, vec3 normal) {
    if (lightIndex < 0 || lightIndex >= MAX_SHADOW_LIGHTS || range <= 0.0) return 1.0;
    // ⚠️ 采样方向 = 光源 → 像素（cubemap 以光源为中心向外）；曾写 lightPos-worldPos（反向）→ 阴影投射反
    vec3 dir = worldPos - lightPos;
    float dist = length(dir);
    if (dist >= range) return 1.0;
    dir /= dist;
    float fragDepth = dist / range;
    vec3 T = normalize(cross(dir, vec3(0.0, 1.0, 0.0)) + vec3(0.001, 0.0, 0.0));   // 切向基（防共线）
    vec3 B = cross(dir, T);
    float texel = 1.0 / SHADOW_MAP_SIZE;
    // 2026-08-15：slope-scaled bias（硬件 PCF 后点光源条纹）——256² cubemap 每 texel 深度台阶在掠射角可达 0.01+，
    // 固定 bias 无法覆盖：bias = 基础(D16 量化) + 斜率项（tan 入射角，clamp 20×）
    // 正对(ndl=1)→0.001；45°→0.004；87°掠射→0.061
    float ndl = clamp(dot(normal, -dir), 0.0, 1.0);   // -dir = 像素→光源方向（dir = 光源→像素）
    float slope = sqrt(max(0.0, 1.0 - ndl * ndl)) / max(ndl, 0.05);
    float bias = 0.001 + 0.003 * slope;
    // 2026-08-13：简化 PCSS（无 blocker 搜索）——接收点离光源越远半影越宽（面光源几何近似）：
    // fragDepth = dist/range ∈ [0,1]，半径 1 → 3.5 texel 线性过渡（近锐利远柔和）× 档位模糊半径
    float pcfScale = mix(1.0, 3.5, fragDepth) * ShadowBlurRadius();
    // 2026-08-15：统一滤波内核（模式/半径由编译期宏决定）
    return ShadowFilterImpl(vec2(0.0), lightIndex, fragDepth, bias, texel, pcfScale,
                            true, dir, T, B);
}

// 2026-08-12：3 阶球谐（SH）辐照度求值（与 CPU 投影同约定——归一化实球谐；已乘卷积核）
vec3 shIrradiance(vec3 n) {
    float x = n.x, y = n.y, z = n.z;
    vec3 irr = shUBO.shCoefs[0].rgb * 0.282095;
    irr += shUBO.shCoefs[1].rgb * (-0.488603 * y);
    irr += shUBO.shCoefs[2].rgb * ( 0.488603 * z);
    irr += shUBO.shCoefs[3].rgb * (-0.488603 * x);
    irr += shUBO.shCoefs[4].rgb * ( 1.092548 * x * y);
    irr += shUBO.shCoefs[5].rgb * ( 1.092548 * y * z);
    irr += shUBO.shCoefs[6].rgb * ( 0.315392 * (3.0 * z * z - 1.0));
    irr += shUBO.shCoefs[7].rgb * ( 1.092548 * x * z);
    irr += shUBO.shCoefs[8].rgb * ( 0.546274 * (x * x - y * y));
    return max(irr, vec3(0.0));
}
layout(set = 0, binding = 5) uniform sampler2D galaxyTex;   // 2026-08-11：银河全景（end_sky.png——LogLuv32 编码 4096x2048）
layout(set = 0, binding = 6) uniform sampler2D transmittanceLUT;   // 2026-08-11：透射率 LUT（官方物理太阳 + per-pixel 天空）
layout(set = 0, binding = 7) uniform sampler3D scatteringLUT;   // 2026-08-11 per-pixel：散射 LUT（GetSkyRadiance）

// 当前视口的逆投影视图矩阵（重建视线方向 → 采样全景天空图）+ 相机位置（视线 = 远平面点 - 相机位置）+ 太阳方向
layout(push_constant) uniform PC {
    mat4 invViewProj;
    vec4 cameraPos;
    vec4 sunDir;
    vec4 lightColor;
    mat4 invProj;
    mat4 invView;
    vec4 frameInfo;
} pc;

// PI 由 atmo_common.glsl include 提供（3.14159265358979323846）

// PI 由 atmo_common.glsl include 提供（3.14159265358979323846）

float linestep(float a, float b, float x) { return clamp((x - a) / (b - a), 0.0, 1.0); }
float safeacos(float x) { return acos(clamp(x, -1.0, 1.0)); }
float pow2(float x) { return x * x; }
float saturate(float x) { return clamp(x, 0.0, 1.0); }
float max0(float x) { return max(0.0, x); }

// ⚠️ 2026-08-15：深度直传（附件 = NDC z 直存，Vulkan viewport 深度直存；非 *2-1）——当前无调用（死代码，防误用）
vec3 reconstructWorldPosition(vec2 uv, float depth) {
    uv = clamp(uv, vec2(0.0), vec2(1.0));
    vec4 clip = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 world = pc.invViewProj * clip;
    float invW = 1.0 / max(world.w, 1e-4);
    return world.xyz * invW;
}

// ⚠️ 2026-08-15 阴影采样质量档（编译期 define 切换，参考 HSPE 渊霞流霭阴影滤波思路）：
//   SHADOW_MODE: 0=硬阴影（1 tap） 1=普通 PCF（3×3 高斯） 2=圆盘抖动 PCF（16 tap Poisson 盘 + 每像素 hash 旋转）
//   SHADOW_BLUR_LEVEL: 0=小半径(0.75 texel) 1=中(1.5) 2=大(2.5)——模糊半径 × texel → 远级联自然更软（世界半径随 texel 放大）
#define SHADOW_MODE 2
#define SHADOW_BLUR_LEVEL 1

// ===== 2026-08-14 CSM 方向光阴影采样（参考 LimitlessSquareEngine：级联选择 + 边缘混合 + 3×3 高斯 PCF）=====
// ⚠️ 必须位于 push constant PC 与 saturate 声明之后（GLSL 顺序声明）
float ShadowBlurRadius() {
#if SHADOW_BLUR_LEVEL == 0
    return 0.75;
#elif SHADOW_BLUR_LEVEL == 1
    return 1.5;
#else
    return 2.5;
#endif
}

float Hash12(vec2 p) {   // 圆盘旋转抖动（每像素固定 → 无时间闪烁）
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

// ⚠️ 2026-08-15 统一阴影滤波内核（CSM 2D array 与点光源 cubemap array 共用；SHADOW_MODE/SHADOW_BLUR_LEVEL 编译期决定）
// 硬件 shadow sampler（compareOp=LESS）：每次 texture() = 2×2 双线性比较 PCF（HSPE 同款）；ref = currentDepth - bias
// 参数：sampleBase = CSM 平面 uv（点光源传 vec2(0)）；layer = 级联/光源索引；baseDir/T/B = 点光源方向与切向基（CSM 传空）
float ShadowFilterImpl(vec2 sampleBase, int layer, float currentDepth, float bias, float texel, float radius,
                       bool isPoint, vec3 baseDir, vec3 T, vec3 B) {
    float refDepth = currentDepth - bias;   // d > ref → 亮（compareOp LESS；Vulkan 语义 ref<sampled）
    float shadow = 0.0;
#if SHADOW_MODE == 0
    // —— 硬阴影（1 tap 硬件双线性比较 = 最锐利档；texelFetch 对 cubemap 需 face 索引太麻烦，硬件 2×2 反而抗锯齿）——
    float d = isPoint ? texture(shadowCubeMaps, vec4(baseDir, float(layer)), refDepth)
                      : texture(csmShadowMaps, vec4(sampleBase, float(layer), refDepth));
    return d;   // shadow sampler 直接返回比较结果 {0,1}（硬件 PCF 时 0..1 平滑）
#elif SHADOW_MODE == 1
    // —— 普通 PCF（3×3 高斯；每个 tap 自带硬件 2×2 双线性比较 → 等效 36 采样高质量）——
    const vec2 offsets[9] = vec2[](
        vec2(-1.0, -1.0), vec2(0.0, -1.0), vec2(1.0, -1.0),
        vec2(-1.0,  0.0), vec2(0.0,  0.0), vec2(1.0,  0.0),
        vec2(-1.0,  1.0), vec2(0.0,  1.0), vec2(1.0,  1.0));
    const float weights[9] = float[](
        1.0, 2.0, 1.0,
        2.0, 4.0, 2.0,
        1.0, 2.0, 1.0);
    for (int i = 0; i < 9; i++) {
        vec2 off = offsets[i] * radius * texel;
        float lit = isPoint ? texture(shadowCubeMaps, vec4(normalize(baseDir + T * off.x + B * off.y), float(layer)), refDepth)
                            : texture(csmShadowMaps, vec4(sampleBase + off, float(layer), refDepth));
        shadow += lit * weights[i];
    }
    return shadow / 16.0;
#else
    // —— 圆盘抖动 PCF（16 tap Poisson 盘 + 每像素 hash 旋转；每 tap 硬件双线性 → 软阴影无网格感，HSPE 风格高质量）——
    const vec2 poissonDisk[16] = vec2[](
        vec2(-0.94201624, -0.39906216), vec2(0.94558609, -0.76890725),
        vec2(-0.09418410, -0.92938870), vec2(0.34495938, 0.29387760),
        vec2(-0.91588581, 0.45771432), vec2(-0.81544232, -0.87912464),
        vec2(-0.38277543, 0.27676845), vec2(0.97484398, 0.75648379),
        vec2(0.44323325, -0.97511554), vec2(0.53742981, -0.47373420),
        vec2(-0.26496911, -0.41893023), vec2(0.79197514, 0.19080888),
        vec2(-0.68788861, 0.90801072), vec2(-0.05469978, 0.52422490),
        vec2(-0.43356592, 0.68885758), vec2(0.07138003, 0.90180533));
    float angle = Hash12(gl_FragCoord.xy) * 6.28318530718;
    float s = sin(angle), c = cos(angle);
    for (int i = 0; i < 16; i++) {
        vec2 disk = poissonDisk[i] * radius * texel;
        vec2 off = vec2(c * disk.x - s * disk.y, s * disk.x + c * disk.y);
        float lit = isPoint ? texture(shadowCubeMaps, vec4(normalize(baseDir + T * off.x + B * off.y), float(layer)), refDepth)
                            : texture(csmShadowMaps, vec4(sampleBase + off, float(layer), refDepth));
        shadow += lit;
    }
    return shadow / 16.0;
#endif
}

// 单级联阴影采样（深度比较 + bias；模式/模糊半径由 SHADOW_MODE/SHADOW_BLUR_LEVEL 编译期决定）
float SampleCsmCascadeAtIndex(int cascadeIndex, vec3 worldPos, float ndl) {
    vec4 clipPos = csm.shadowMatrices[cascadeIndex] * vec4(worldPos, 1.0);
    if (abs(clipPos.w) <= 0.000001) return 1.0;
    vec3 ndc = clipPos.xyz / clipPos.w;
    // ⚠️ 2026-08-14 最终定稿（LimitlessSquareEngine 同式）：uv 同式 0.5+、curD = ndc.z*0.5+0.5——
    // uv 与 curD 同源（*2-1 重建 worldPos，RenderDoc 手算验证数学精确）
    vec2 localUv = vec2(ndc.x * 0.5 + 0.5, 0.5 + ndc.y * 0.5);
    // ⚠️ 2026-08-15 修复：currentDepth 必须与 CSM shadowmap 附件语义一致——
    // C++ 侧 lightProjection 已改 ZERO_TO_ONE ortho + Vulkan viewport 深度直存（z_fb = z_ndc）→ 附件 = ndc.z ∈ [0,1]。
    // 曾用 ndc.z*0.5+0.5（-1..1 假设）→ 与附件直存不匹配 → 深度比较全错（阴影异常）
    float currentDepth = ndc.z;
    // ⚠️ 2026-08-14 越界语义恢复（阴影实现保持原样）：越界判为无阴影（return 1.0）
    if (localUv.x < 0.0 || localUv.x > 1.0 || localUv.y < 0.0 || localUv.y > 1.0) return 1.0;

    float bias = mix(0.002, 0.0001, ndl);   // N·L 相关 bias（掠射角加大防 acne）
    vec2 texelSize = 1.0 / vec2(textureSize(csmShadowMaps, 0).xy);
    float radius = ShadowBlurRadius();   // 多级模糊半径（texel 单位，SHADOW_BLUR_LEVEL 档位）
    // 2026-08-15：统一滤波内核（模式/半径由编译期宏决定）
    return ShadowFilterImpl(localUv, cascadeIndex, currentDepth, bias, texelSize.x, radius,
                            false, vec3(0.0), vec3(0.0), vec3(0.0));
}

// 与 CPU CSM 使用完全相同的安全方向：白天使用太阳方向，太阳落到地平线下
// 立即使用月光方向。不做插值，避免阴影贴图间隔刷新时方向与矩阵不同步。
vec3 ComputeCsmLightDirection(vec3 sunDirection) {
    float lengthSquared = dot(sunDirection, sunDirection);
    if (!(lengthSquared > 1e-8)) return vec3(0.0, 1.0, 0.0);

    vec3 sun = sunDirection / sqrt(lengthSquared);
    return sun.y >= 0.0 ? sun : -sun;
}

// 级联选择（view 空间视线深度 viewZ——原版 Lit.frag:529 -vViewPos.z 同式）+ 级联边缘 15% 平滑混合 + 末级联淡出
// 2026-08-14 用户指示：不再依赖 view 空间反投影（invProj 路径脆弱）；worldPos 已由用户分屏染色验证正确
// 2026-08-15：viewDepth 改用精确视线深度 viewZ（斜视像素世界距离 > 视线深度 → 级联边界扭曲/切换抖动）
float SampleDirectionalShadow(vec3 worldPos, vec3 normalDir, vec3 lightDir, float viewDepth, out int cascadeIndex) {
    cascadeIndex = -1;   // 无阴影/未选中时 -1（debug 染色用）
    if (csm.csmParams.y < 0.5) return 1.0;
    int cascadeCount = int(csm.csmParams.x + 0.5);
    if (cascadeCount <= 0) return 1.0;

    if (viewDepth <= 0.0) return 1.0;   // 2026-08-15：viewDepth = 精确视线深度（原版 -vViewPos.z 同式）

    int selected = -1;
    for (int i = 0; i < cascadeCount; i++) {
        if (viewDepth <= csm.splitDepths[i]) { selected = i; break; }
    }
    if (selected < 0) return 1.0;
    cascadeIndex = selected;   // debug 染色：当前像素所属级联

    float ndl = saturate(dot(normalDir, lightDir));
    float shadowCurrent = SampleCsmCascadeAtIndex(selected, worldPos, ndl);

    if (selected == cascadeCount - 1) {
        // 末级联：视锥远边界淡出（阴影渐隐，防远距硬切）
        float prevFar = (selected > 0) ? csm.splitDepths[selected - 1] : 0.0;
        float fadeWidth = max((csm.splitDepths[selected] - prevFar) * 0.15, 0.05);   // 原版同式
        float fadeT = 1.0 - smoothstep(csm.splitDepths[selected] - fadeWidth, csm.splitDepths[selected], viewDepth);
        return mix(1.0, shadowCurrent, fadeT);
    }

    // 级联边缘 15% 宽度内双级联混合（当前 + 下一级联）——原版同式（Lit.frag:562）
    float prevFar = (selected > 0) ? csm.splitDepths[selected - 1] : 0.0;
    float blendWidth = max((csm.splitDepths[selected] - prevFar) * 0.15, 0.05);
    if (viewDepth < csm.splitDepths[selected] - blendWidth) return shadowCurrent;
    float shadowNext = SampleCsmCascadeAtIndex(selected + 1, worldPos, ndl);
    float blendT = smoothstep(csm.splitDepths[selected] - blendWidth, csm.splitDepths[selected], viewDepth);
    return mix(shadowCurrent, shadowNext, blendT);
}
// 2026-08-09 线性空间：G-Buffer 颜色附件存 SRGB 编码值（8bit UNORM 精度更高），合成时转线性做 PBR，tonemap pass 转回
vec3 sRGBToLinear(vec3 c) { return pow(c, vec3(2.2)); }

// 2026-08-11 八面体解码（Cigolle 2014 对称版，编码逆）——R16G16_SNORM 存 [-1,1]² → 完整世界法线
vec3 OctahedronDecode(vec2 oct) {
    vec3 n = vec3(oct, 1.0 - abs(oct.x) - abs(oct.y));
    if (n.z < 0.0) {
        n.xy = (1.0 - abs(n.yx)) * sign(n.xy);
    }
    return normalize(n);
}

// ===== PBR 光照（2026-08-11 参考 glTF-Sample-Viewer 金属-粗糙度工作流）=====
// D_GGX + V_SmithGGXCorrelated（含 1/(4·NoV·NoL)）+ F_Schlick + EnvBRDFApprox（Karis split-sum 解析近似）
// 2026-08-17 对齐官方 glTF 语义：alphaRoughness = perceptualRoughness²（粗糙度输入为感知值，D/V 内部平方——0.75 → 0.5625）

float D_GGX(float NoH, float perceptualRoughness) {
    float alpha = perceptualRoughness * perceptualRoughness;
    float alphaSq = alpha * alpha;
    float f = NoH * NoH * (alphaSq - 1.0) + 1.0;
    return alphaSq / (PI * f * f);
}

float V_SmithGGXCorrelated(float NoV, float NoL, float perceptualRoughness) {
    float alpha = perceptualRoughness * perceptualRoughness;
    float alphaSq = alpha * alpha;
    float GGXV = NoL * sqrt(NoV * NoV * (1.0 - alphaSq) + alphaSq);
    float GGXL = NoV * sqrt(NoL * NoL * (1.0 - alphaSq) + alphaSq);
    return 0.5 / (GGXV + GGXL);
}

vec3 F_Schlick(vec3 f0, float VoH) {
    float f = pow(1.0 - VoH, 5.0);
    return f0 + (1.0 - f0) * f;
}

// 环境 BRDF 解析近似（Karis 2013——免 2D LUT 纹理）
vec3 EnvBRDFApprox(vec3 f0, float roughness, float NoV) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
    vec2 AB = vec2(-1.04, 1.04) * a004 + r.zw;
    return f0 * AB.x + AB.y;
}

// 2026-08-11 用户拍板：HSPE SP V5 太阳实现（HSPEAtmos.inc sunWithBloom）——盘内恒亮 ×128 + 盘外高斯 bloom 光晕。
// 放弃物理太阳盘（SOLAR_IRRADIANCE/π × transmittance LUT × soft-edge——AgX 高光压缩下与亮天空无对比、白天不可见）。
const float SUN_R_HSPE = 0.012;   // 太阳角半径 1.38°（月亮同值）——2026-08-11 用户要求 1.5 倍（原 0.016）
const float SUN_EXPOSURE = 4.0;   // 太阳盘曝光（与 AtmosphereLUT.cpp sky 曝光 pc.sunDir.w 同步）
// SOLAR_IRRADIANCE 由 atmo_common.glsl include 提供（vec3(1.474, 1.8504, 2.3612)——同值）
// transmittance LUT 逆映射（官方 GetTransmittanceTextureUvFromRMu——距离参数化 + texcoord 中心修正）
const float ATMO_BOTTOM_R = 6371000.0;
const float ATMO_TOP_R = 6431000.0;
float TransLUT_DistanceToTop(float r, float mu) {
    float disc = r * r * (mu * mu - 1.0) + ATMO_TOP_R * ATMO_TOP_R;
    return max(-r * mu + sqrt(max(disc, 0.0)), 0.0);
}
vec2 TransLUT_Uv(float r, float mu) {
    float H = sqrt(ATMO_TOP_R * ATMO_TOP_R - ATMO_BOTTOM_R * ATMO_BOTTOM_R);
    float rho = sqrt(max(r * r - ATMO_BOTTOM_R * ATMO_BOTTOM_R, 0.0));
    float d = TransLUT_DistanceToTop(r, mu);
    float d_min = ATMO_TOP_R - r;
    float d_max = rho + H;
    float x_mu = (d - d_min) / (d_max - d_min);
    float x_r = rho / H;
    float u = 0.5 / 256.0 + x_mu * (1.0 - 1.0 / 256.0);
    float v = 0.5 / 64.0 + x_r * (1.0 - 1.0 / 64.0);
    return vec2(clamp(u, 0.0, 1.0), clamp(v, 0.0, 1.0));
}

// 世界方向 → 全景天空图 UV（与 sky_physical.frag 的 skylutdir 互为逆；FMDS sky.inc skylutuv）
// 2026-08-11：camAltMeters 为相机海拔（m）——与 atmo_sky.comp 生成端 campos 同值（用户约定 max(0, 相机y+200)）
// 圆柱投影 + pow4 动态映射（海拔参数化——低海拔地平线细腻、高海拔 t 随海拔移动放大有效区域）
// 与 atmo_sky.comp skylutdir 互为逆（同一 altitudeMeters）
vec2 skylutuv(vec3 rayDir, float camAltMeters) {
    const float CAM_ALT = max(camAltMeters, 0.0) / 1000.0;   // m → km（生成端 campos 单位一致）
    const float EARTH_R = 6371.0;
    const float EARTH_R2 = EARTH_R * EARTH_R;
    float r2 = pow2(EARTH_R + CAM_ALT);
    float r = sqrt(r2);

    float horizonAngle = safeacos(sqrt(r2 - EARTH_R2) / r);
    float altitudeAngle = horizonAngle - acos(clamp(rayDir.y, -1.0, 1.0));
    float v = 0.5 + 0.5 * sign(altitudeAngle) * abs(altitudeAngle) * 2.0 / PI;

    vec2 uv = vec2(atan(-rayDir.x, -rayDir.z) / (2.0 * PI) + 0.5, v);

    float s = EARTH_R / r;
    float y = -sqrt(1.0 - s * s);
    float t = clamp(y * 0.5 + 0.5, 0.0, 1.0);
    float x = mix(linestep(t, 1.0, uv.y), linestep(t, 0.0, uv.y), float(uv.y < t));
    // 2026-08-11 海拔自适应 pow4 逆（与 atmo_sky.comp skylutdir 同一 expo——互逆）
    float expo = mix(4.0, 1.0, clamp(camAltMeters / 60000.0, 0.0, 1.0));
    x = pow(x, 1.0 / expo);
    uv.y = mix(x * (1.0 - t) + t, -x * t + t, float(uv.y < t));
    // 北极/经度环绕边界接缝收敛（全景图采样通用技巧）
    const float UV_INSET = 0.002;
    uv = uv * (1.0 - 2.0 * UV_INSET) + UV_INSET;
    return uv;
}

// 8x8 Bayer 抖动已移至 tonemap pass（链尾）——本 shader 只输出线性 HDR

// ===== LogLuv32 解码（2026-08-11 修正版——FMDS basic.inc colors_LogLuv32ToSRGB）：
// 解码输出为「线性 sRGB/Rec.709 色域 RGB」——名字里的 SRGB 指色域非 gamma 编码！
// 直接当线性用（勿再 sRGBToLinear——此前偏色根因：二次 gamma）=====
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

// ===== LogLuv32 编码（2026-08-11：pass 链——合成输出→composite 也 LogLuv32 编码，tonemap 端解码；FMDS colors_sRGBToLogLuv32）=====
const mat3 COLORS_LOGLUV32_M = mat3(
    0.2209, 0.3390, 0.4184,
    0.1138, 0.6780, 0.7319,
    0.0102, 0.1130, 0.2969
);

vec4 colors_sRGBToLogLuv32(in vec3 vRGB) {
    if (all(lessThanEqual(vRGB, vec3(0.0)))) {
        return vec4(0.0);
    }
    vec3 Xp_Y_XYZp = COLORS_LOGLUV32_M * vRGB;
    Xp_Y_XYZp = max(Xp_Y_XYZp, vec3(1e-6));
    vec2 xy = Xp_Y_XYZp.xy / Xp_Y_XYZp.z;
    float Le = 2.0 * log2(Xp_Y_XYZp.y) + 127.0;
    float w = fract(Le);
    float z = (Le - floor(w * 255.0) / 255.0) / 255.0;
    return vec4(xy, z, w);
}

void main() {
    float depth = MIKAN_SAMPLE_DEPTH;

    vec3 color;
    if (depth >= 0.9999) {   // 天空区域：按视线方向采样全景天空图（视线 = 远平面点 - 相机位置；Scene/Game 视口各自相机）
        // 天空区域：按视线方向采样全景天空图（视线 = 远平面点 - 相机位置；Scene/Game 视口各自相机）
        // 2026-08-11 高海拔精度（全链路）：相机空间重建 + invView 旋转——避免 invViewProj远平面点-cameraPos
        // 在 1e5 量级 float32 大数相减（方向误差 ~0.2° → 太阳锯齿 + 银河/skyRT 采样错位断断续续）
        vec2 ndc = fragTexCoord * 2.0 - 1.0;
        vec3 dirCam = normalize((pc.invProj * vec4(ndc, 1.0, 1.0)).xyz);
        vec3 dir = normalize(mat3(pc.invView) * dirCam);
        // 2026-08-11 per-pixel：天空 = 每像素 GetSkyRadiance（查 transmittance/scattering LUT + 瑞利/米氏相函数——
        // 2026-08-17 全物理基准：去掉 ×SUN_EXPOSURE（与 skyRT 物理亮度一致——原 ×12 显示曝光只在天顶，物理化后 skyRT 同亮度）
        // 【外太空测试 2026-08-11】campos 抬到 1000km 高空：GetSkyRadiance 物理处理（视线穿大气=辉光、太空方向=纯黑）；
        // 正常地面视角应改回 vec3(0.0, BOTTOM_RADIUS + 200.0, 0.0)
        const float CAM_ALT_PER_PIXEL = 200.0;   // 地表相机高度 200m（2026-08-11 用户定稿：与 skyRT 生成一致统一 200m）
        // 2026-08-11 用户拍板（最终）：恢复 skyRT 做天空 + 环境光（IBL cubemap mip 设施保留——供反射/预滤波）；
        // 直接光物理亮度（2026-08-16 去 lightScale/SPECULAR_SCALE 历史缩放）
        // 2026-08-12 北极条带修复：天顶方向改用 per-pixel GetSkyRadiance（skyRT 顶部行稀疏——10°/行 → 条带）；
        // 2026-08-12 用户：不要硬截断——smoothstep 平滑过渡（70° 起混入 per-pixel，78° 以上完全 per-pixel）
        // ⚠️ 2026-08-12 性能：GetSkyRadiance 必须只在 zenithBlend>0（天顶区域）才计算——全屏无条件算（1080p 天空 50 万像素×LUT 采样）是 GPU 耗时大头
        vec3 skyRTColor = colors_LogLuv32ToSRGB(texture(skyRT, skylutuv(dir, max(pc.cameraPos.y + 200.0, 0.0))));   // 天空（圆柱投影 pow4 动态映射）
        vec3 skyColor = skyRTColor;
        /*float zenithBlend = smoothstep(0.94, 0.98, dir.y);   // 仰角 70°→78° 平滑过渡（dir.y = sin(仰角)）
    
        if (zenithBlend > 0.0) {
            vec3 trans;
            vec3 skyPixColor = GetSkyRadiance(transmittanceLUT, scatteringLUT,
                                              vec3(0.0, BOTTOM_RADIUS + max(pc.cameraPos.y + 200.0, 0.0), 0.0), dir, 0.0,
                                              pc.sunDir.xyz, trans);   // 2026-08-17 物理（去 SUN_EXPOSURE）
            skyColor = mix(skyRTColor, skyPixColor, zenithBlend);
        }*/
        /* per-pixel 备选（无限分辨率/外太空——需 scatteringLUT binding 7）：
        vec3 trans;
        skyColor = GetSkyRadiance(transmittanceLUT, scatteringLUT,
                                  vec3(0.0, BOTTOM_RADIUS + CAM_ALT_PER_PIXEL, 0.0), dir, 0.0,
                                  pc.sunDir.xyz, trans) * SUN_EXPOSURE;
        */
        // 银河（2026-08-11：FMDS main TEXTURE_6 采样方式——sky.inc galaxy_logluv2srgb/getgal）：
        // ① 绕太阳方向旋转（银河带与太阳参考系对齐）② equatorial spherical 投影（ra=atan(y,x), dec=asin(z)）
        // ③ 夜晚因子 saturate((-sun.y - 0.1)*5)（白天不可见）④ FlipY（mikan png 加载 Y 翻转——FMDS MC 约定 v=0 顶部）
        vec3 gaxis = cross(pc.sunDir.xyz, vec3(0.0, 0.0, 1.0));
        vec3 gnew = dir;
        float glen2 = dot(gaxis, gaxis);
        if (glen2 > 1e-8) {
            float gcos = pc.sunDir.z;
            float gcsc2 = 1.0 / glen2;
            gnew = gcos * dir + cross(gaxis, dir) + (gcsc2 - gcsc2 * gcos) * dot(gaxis, dir) * gaxis;
        }
        vec2 gUV = vec2(atan(gnew.y, gnew.x) * (-0.5 / 3.14159265) + 0.5,
                        asin(clamp(gnew.z, -1.0, 1.0)) / 3.14159265 + 0.5);
        gUV.y = 1.0 - gUV.y;   // FlipY（mikan png 加载 Y 翻转——FMDS MC 约定）
        vec3 galaxy = colors_LogLuv32ToSRGB(texture(galaxyTex, gUV));   // LogLuv32 解码
        galaxy *= clamp((-pc.sunDir.y - 0.1) * 5.0, 0.0, 1.0);          // 夜晚因子（白天银河被天空淹没）
        galaxy *= smoothstep(0.0, 0.3, dir.y);   // 地平线遮挡（2026-08-11 用户反馈：过渡更大更慢——大气消光：地平线上 0~17° 银河渐显，地平线下严格不可见）
        skyColor += galaxy * 0.03;   // 2026-08-11：银河亮度 0.3 → 0.15（夜晚再黑一档——银河是夜晚最大亮度源）
        color = skyColor;   // 线性 HDR（LogLuv32 解码输出即线性 sRGB 色域——2026-08-11 修正，无二次 gamma）
    } else {
        // ===== 2026-08-11 PBR 链路（参考 glTF-Sample-Viewer 金属-粗糙度工作流）=====
        vec4 gBufferColor = MIKAN_SAMPLE_COLOR;
        vec3 albedo = sRGBToLinear(gBufferColor.rgb);
        // model.frag 用 G-buffer color.a=0.5~0.75 编码明确声明的
        // KHR_materials_diffuse_transmission；0.5 表示系数为 0。
        // alpha-mask + 双面本身不再隐式产生透射，其它几何的 alpha 仍为 1。
        float thinSheet = step(0.49, gBufferColor.a) * (1.0 - step(0.99, gBufferColor.a));
        float diffuseTransmissionFactor = thinSheet > 0.5
            ? clamp((gBufferColor.a - 0.5) * 4.0, 0.0, 1.0)
            : 0.0;
        vec4 mat = MIKAN_SAMPLE_MATERIAL;   // xyz=metallic/roughness/ao, w=emissive 强度
        float metallic = clamp(mat.x, 0.0, 1.0);
        float roughness = clamp(mat.y, 0.04, 1.0);   // 0.04 下限防除零
        float ao = mat.z;
        float emissiveStrength = mat.w;   // 2026-08-13：自发光强度（model.frag 输出：自发光材质=1，普通材质=0）

        // 视线方向（相机空间重建 + invView 旋转——高海拔精度；法线 z 符号选择 + 雾方位用）
        vec2 ndcV = fragTexCoord * 2.0 - 1.0;
        // ⚠️ 2026-08-15 定稿（readback 实测）：深度附件 = NDC z 直存（D24 上近=0 远=1，非 *0.5+0.5 映射）——
        // 反投影必须直传 z_in = depth（点光源实测正常）；*2-1 会镜像 z → 点光源完全失效。
        // GTAO 的 *2-1 是历史自洽错误（AO 对系统 z 误差鲁棒，视觉正常）——勿再改回
        vec4 worldPosH = pc.invViewProj * vec4(ndcV, depth, 1.0);
        vec3 worldPos = worldPosH.xyz / worldPosH.w;
        // ⚠️ 2026-08-15 对齐原版（LimitlessSquare Lit.frag:529 viewDepth = -vViewPos.z）：
        // 级联选择用精确视线深度（view 空间反投影），不用世界欧几里得距离（斜视像素偏差 → 级联边界扭曲/切换抖动）
        vec4 vpZ = pc.invProj * vec4(ndcV, depth, 1.0);
        float viewZ = -vpZ.z / vpZ.w;   // 视线正深度（不 clamp，原版同式）
        vec3 dirCamV = normalize((pc.invProj * vec4(ndcV, 1.0, 1.0)).xyz);
        vec3 viewDir = -normalize(mat3(pc.invView) * dirCamV);
        // 法线（R16G16_SNORM 八面体编码）——2026-08-11 用户选业界主流 octahedral：
        // 完整世界法线含朝向（解码无符号歧义），保持 32bit 带宽；不再 z 重建恒正+相机翻转（受光面随相机漂移的根源）
        vec3 n = OctahedronDecode(MIKAN_SAMPLE_NORMAL.xy);

        // 直接光（方向光）
        vec3 L = normalize(pc.sunDir.xyz);   // 太阳方向（场景 Directional Light 或回退）
        vec3 csmLightDir = ComputeCsmLightDirection(pc.sunDir.xyz);
        float NoV = clamp(dot(n, viewDir), 1e-4, 1.0);
        float NoL = clamp(dot(n, L), 0.0, 1.0);
        vec3 H = normalize(viewDir + L);
        float NoH = clamp(dot(n, H), 0.0, 1.0);
        float VoH = clamp(dot(viewDir, H), 0.0, 1.0);
        vec3 F0 = mix(vec3(0.04), albedo, metallic);

        // 2026-08-16 用户：去掉历史遗留的显示亮度缩放（lightScale/SPECULAR_SCALE）——直接光走物理亮度
        // 2026-08-12 用户拍板：直接光颜色 = 太阳圆盘物理色（SOLAR_IRRADIANCE × 大气透射率——正午暖白/落山红）
        // ⚠️ 必须在光照处对所有像素统一计算（天空 if 块内赋值只对天空像素生效——物体像素会是回退白，用户实测"全天白"）
        float lightCamAlt = max(pc.cameraPos.y + 200.0, 0.0);
        vec2 lightSunUV = TransLUT_Uv(ATMO_BOTTOM_R + lightCamAlt, clamp(pc.sunDir.xyz.y, -1.0, 1.0));
        vec3 lightSunTrans = texture(transmittanceLUT, lightSunUV).rgb;
        vec3 sunDiskColor = (SOLAR_IRRADIANCE / PI) * lightSunTrans;   // 2026-08-17 物理修正：去掉色相归一化（原来 /=max 把物理 0.47-0.75 归一成 1 且抹掉落山透射衰减——太阳落山不变暗，非物理；r11g11b10 存 0.75 不会溢出）
        // 2026-08-12 用户拍板：直接光 = 太阳 + 月光两端合成——太阳落山后贡献→0、月光主导；黎明反之
        float sunAlt = pc.sunDir.xyz.y;
        float dayFactor = smoothstep(-0.05, 0.05, sunAlt);            // 白天因子（太阳过地平线过渡）
        // 月光（用户参考 atmo 月光散射语义）：nightFade = 1-smoothstep(-0.5,-0.1,sun.y)；
        // 颜色 = 月亮方向天空散射色（skyRT 含月光散射），强度 (0.01+0.01×nightFade) 弱——避免过强
        float nightFade = 1.0 - smoothstep(-0.5, -0.1, sunAlt);
        vec3 moonDir = -normalize(pc.sunDir.xyz);                     // 月亮方向 = 太阳反方向
        float moonAlt = moonDir.y;
        vec3 moonSky = colors_LogLuv32ToSRGB(texture(skyRT, skylutuv(moonDir, lightCamAlt))).rgb;   // 月亮方向天空色
        // ⚠️ 2026-08-15 修复月光可见性：moonSky 是 skyRT 夜晚散射（~0.01-0.05 量级），原权重 0.01-0.02 → 总贡献 ~1e-4 夜晚全黑。
        // 夜晚权重 moonSky×1.0 + 固定蓝色 0.05 保底（月夜轮廓可见）；白天月亮弱贡献（×0.2，被太阳淹没）
        vec3 moonLight = (moonSky * 1.0 + vec3(0.2, 0.3, 0.6) * 0.05)
                       * (0.2 + 0.8 * nightFade) * smoothstep(-0.05, 0.05, moonAlt);
        vec3 sunLight = sunDiskColor * dayFactor;        // 太阳贡献（落山→0；2026-08-16 去 lightScale——物理亮度）

        // 直接光 BRDF（2026-08-15 Fermion 对齐：kS = fresnelSchlick(LoH) + Burley 漫反射 + Smith 相关可见性）
        // ⚠️ 2026-08-14：worldPosCsm 统一用 main 前部的 worldPos——与点光源同源
        // ⚠️ 2026-08-14 用户指示：CSM 采样点沿法线方向外扩（normal offset 防自阴影）
        vec3 worldPosCsm = worldPos + n * 0.01;
        int debugCascade = -1;
        // ⚠️ 2026-08-14 用户指示：级联选择不依赖 view 空间——内部用 length(worldPos - cameraPos) 世界距离（D:\mikan engine 同式）
        float shadowFactor = SampleDirectionalShadow(worldPosCsm, n, csmLightDir, viewZ, debugCascade);   // 太阳/月光阴影（无 CSM 数据时 = 1.0）
        float D = D_GGX(NoH, roughness);
        float Vis = V_SmithGGXCorrelated(NoV, NoL, roughness);
        float LoH = clamp(dot(L, H), 0.0, 1.0);
        vec3 kS = F_Schlick(F0, LoH);
        // ⚠️ 2026-08-15 用户：合成 pass 太阳高光不够亮——SPECULAR_SCALE 0.1 → 0.3（显示亮度语义适配；溢色由 ACES 管住）
        vec3 specular = D * Vis * kS * sunLight * NoL * shadowFactor;   // 2026-08-16 去 SPECULAR_SCALE 0.3——物理亮度
        // Burley 漫反射（Fd_Burley——更物理的边缘散射；kD 能量守恒）
        float f90 = 0.5 + 2.0 * roughness * LoH * LoH;
        float lightScatter = 1.0 + (f90 - 1.0) * pow(1.0 - NoL, 5.0);
        float viewScatter = 1.0 + (f90 - 1.0) * pow(1.0 - NoV, 5.0);
        vec3 diffuse = (vec3(1.0) - kS) * (1.0 - metallic) * albedo * (lightScatter * viewScatter * (1.0 / 3.14159265)) * sunLight * NoL * shadowFactor;

        // 薄片的背光透射：普通实体仍只使用 NoL=max(dot(n,L),0)，
        // alpha-mask 双面叶片在太阳位于观察面背后时使用另一侧入射余弦。
        // 这里使用双界面 Fresnel + Beer-Lambert 的有限厚度薄片近似：
        // 光线先从背面进入、在叶片内被吸收/散射，再从观察面出射；
        // 不修改 albedo，也不把环境/SH 当作方向光阴影处理。
        float NoLBack = clamp(-dot(n, L), 0.0, 1.0);
        if (thinSheet > 0.5 && diffuseTransmissionFactor > 0.0 && NoLBack > 0.0) {
            // 入射/出射两次界面透射率；金属不走叶片透射模型。
            vec3 F_entry = F_Schlick(F0, NoLBack);
            vec3 F_exit = F_Schlick(F0, NoV);
            vec3 interfaceTransmission =
                (vec3(1.0) - F_entry) * (vec3(1.0) - F_exit) * (1.0 - metallic);

            // PBR albedo 不是“亮度补丁”：仅将其作为单位法向单程光学透射率，
            // 由 Beer-Lambert 反推出吸收系数，并按入射/出射角增加实际路径长度。
            // NoLBack=NoV=1 时 transmittance=albedo；掠射角会自然变暗并增强颜色过滤。
            vec3 leafColor = clamp(albedo, vec3(1e-3), vec3(1.0));
            vec3 absorption = -log(leafColor);
            float opticalPath = 0.5 *
                (1.0 / max(NoLBack, 1e-3) + 1.0 / max(NoV, 1e-3));
            vec3 leafTransmittance = exp(-absorption * opticalPath);

            // sunLight 是入射太阳辐照度；没有额外的太阳色/人工亮度乘数。
            diffuse += diffuseTransmissionFactor * interfaceTransmission * leafTransmittance * sunLight *
                       (NoLBack * (1.0 / 3.14159265)) * shadowFactor;
        }

        // 月光 diffuse（2026-08-12：弱光无显著高光——只 diffuse；月亮方向单独 NoL——夜晚月光主导）
        float NoL_moon = clamp(dot(n, moonDir), 0.0, 1.0);
        diffuse += (vec3(1.0) - kS) * (1.0 - metallic) * albedo * moonLight * NoL_moon * shadowFactor;



// ==================== IBL 天空来源宏（2026-08-12 用户拍板）====================
// 1 = 静态 HDR 天空盒（sky_hdr——线性 SFLOAT，无需解码；已停用——binding 8 现在是 atmo cube，必须 LogLuv 解码）
// 0 = 物理大气 IBL（atmo cubemap——LogLuv32 编码必须解码；diffuse = SH（物理投影）；默认）
#define USE_STATIC_SKY_IBL 0

        // IBL（2026-08-12 对齐 learnopengl Diffuse irradiance：https://learnopengl-cn.github.io/07%20PBR/03%20IBL/01%20Diffuse%20irradiance）
#if USE_STATIC_SKY_IBL
        // ---- 静态天空盒 IBL（固定环境——不受物理天空/太阳方向影响）----
        // diffuse = kD · irradiance · albedo（kD = (1-F)(1-metallic)——FresnelSchlickRoughness）
        // 2026-08-12 定稿：SH 辐照度（3 阶——与辐照图视觉无差，省卷积/纹理；A/B 对比保留注释：
        // 左=shIrradiance(n)，右=texture(skyIrradiance, n).rgb）
        vec3 irradiance = shIrradiance(n);
        vec3 F_ibl = F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(1.0 - NoV, 5.0);   // FresnelSchlickRoughness（learnopengl）
        vec3 kD = (1.0 - F_ibl) * (1.0 - metallic);
        vec3 diffuseIBL = irradiance * albedo * kD;
        vec3 reflectDir = reflect(-viewDir, n);
        vec3 prefiltered = textureLod(skyCube, reflectDir, roughness * 7.0).rgb;   // 高光环境（用户定制：金属/光滑→低 lod 清晰；learnopengl 标准为 roughness×maxLod）——2026-08-15 去掉 /10：skyCube 与 skyRT/SH 同 ×10 显示语义（SH 投影输入 ×10 cube）——/10 让反射比背景/漫反射暗 10 倍（用户：反射亮度太低）
        vec3 brdfLUT = EnvBRDFApprox(F0, roughness, NoV);   // Karis 解析近似（split-sum 的 F·G 项）
        // 2026-08-12 用户拍板：反射高光在合成端直接加（太阳/月亮盘——不依赖 cube 预滤波；反射方向对准盘时叠加）
        {
            vec3 sunDirN = normalize(pc.sunDir.xyz);
            float sunAng = SUN_R_HSPE;
            float minCos = 1.0 - sunAng * sunAng * 0.5;
            float roughFade = pow(1.0 - roughness, 3.0);
            float reflHorizon = smoothstep(0.0, 0.02, reflectDir.y);   // 地平线遮挡：反射方向指向地平线下不显示高光（2026-08-12 用户）   // 粗糙衰减：粗糙物体反射扩散——高光随 roughness 减弱（2026-08-12 用户：太刻意）
            // 太阳反射高光（与太阳盘同色：×20 饱和 + 透射；×brdfLUT 即 Fresnel 反射率）
            float cosRS = dot(reflectDir, sunDirN);
            if (cosRS >= minCos) {
                vec2 rsUV = TransLUT_Uv(ATMO_BOTTOM_R + lightCamAlt, clamp(sunDirN.y, -1.0, 1.0));
                vec3 rsTrans = texture(transmittanceLUT, rsUV).rgb;
                prefiltered += (SOLAR_IRRADIANCE / PI) * SUN_EXPOSURE * 20.0 * rsTrans * brdfLUT * roughFade * reflHorizon;   // 粗糙衰减：粗糙物体反射扩散——高光强度/锐度随 roughness 降
            }
            // 月亮反射高光
            vec3 moonDirN = -sunDirN;
            float cosRM = dot(reflectDir, moonDirN);
            if (cosRM >= minCos) {
                vec2 rmUV = TransLUT_Uv(ATMO_BOTTOM_R + lightCamAlt, clamp(moonDirN.y, -1.0, 1.0));
                vec3 rmTrans = texture(transmittanceLUT, rmUV).rgb;
                prefiltered += vec3(0.2, 0.3, 0.6) * 10.0 * rmTrans * brdfLUT * roughFade * reflHorizon;
            }
        }
        // 2026-08-12 用户：粗暴遮挡不合适已恢复——改修柱面环绕拼接（sampler REPEAT）
        vec3 specularIBL = prefiltered * brdfLUT;
        // ⚠️ 2026-08-15 物理观感：粗糙材质"全反射感"来自 specular 不分材质叠加（模糊天空≈均匀增亮）——
        // 按 gloss 衰减（pow(1-r)²）：r=0.75→0.06 哑光、r=0.3→0.49 减半、金属(metallic=1)保持全反射
        float glossAtten = mix(pow(1.0 - roughness, 2.0), 1.0, metallic);
        // ⚠️ 2026-08-12 显示适配：learnopengl 物理量级（π）在 tonemap 后过亮（用户：看不出阴影面）——ambient ×0.5
        vec3 ambient = diffuseIBL * ao * 0.5 + specularIBL * ao * glossAtten;   // 2026-08-12：0.5 只压 diffuse（防阴影面过亮）——specular 反射保持全亮
#else
        // ---- 物理大气 IBL（2026-08-15 Fermion 对齐：split-sum——GGX 预滤波 cube + BRDF LUT）----
        // diffuse：SH 辐照度（系数来自 atmo cubemap 的两级归约 compute 投影）
        vec3 irradiance = shIrradiance(n);   // 2026-08-17 全物理基准（SH 系数物理，cube 去 ×10 后无缩放）
        // 宏观 Fresnel 只由 F0 和观察角决定；粗糙度只进入 GGX、预滤波和 BRDF LUT。
        // 不能用 max(1-roughness,F0) 人为压低掠射角反射，否则 roughness=1 会变成
        // “消除白边”的材质 hack，而不是物理结果。
        vec3 F_ibl = F_Schlick(F0, NoV);
        vec3 kS_ibl = F_ibl;
        vec3 kD_ibl = (vec3(1.0) - kS_ibl) * (1.0 - metallic);
        vec3 diffuseIBL = irradiance * albedo / PI;   // 2026-08-17 物理修正：Lambert 出射 radiance = 辐照度×albedo/π（与直接光 617 行 Burley 1/π 一致；原来缺 /π → 间接光偏亮 π 倍）
        vec3 reflectDir = reflect(-viewDir, n);
        // ⚠️ atmo cube 是线性 R16G16B16A16_SFLOAT（2026-08-12 弃 LogLuv32——直读，无需解码）
        // 2026-08-15：GGX 预滤波 mip 链（Fermion IBLPrefilter 移植——替代 blit box 平均；lod = roughness × maxLod）
        // IBL 必须保留 skyCube 的完整球面方向；不在地平线处截断或重映射反射。
        vec3 prefiltered = textureLod(skyCube, reflectDir, roughness * 7.0).rgb;   // 完整环境反射
        // 2026-08-12 用户：夜晚 IBL 反射也加一点点银河（与天空分支同语义——夜晚因子 × 地平线遮挡 × 弱亮度）
        {
            vec3 gaxisI = cross(pc.sunDir.xyz, vec3(0.0, 0.0, 1.0));
            vec3 gnewI = reflectDir;
            float glen2I = dot(gaxisI, gaxisI);
            if (glen2I > 1e-8) {
                float gcosI = pc.sunDir.z;
                float gcsc2I = 1.0 / glen2I;
                gnewI = gcosI * reflectDir + cross(gaxisI, reflectDir) + (gcsc2I - gcsc2I * gcosI) * dot(gaxisI, reflectDir) * gaxisI;
            }
            vec2 gUVI = vec2(atan(gnewI.y, gnewI.x) * (-0.5 / 3.14159265) + 0.5, asin(clamp(gnewI.z, -1.0, 1.0)) / 3.14159265 + 0.5);
            gUVI.y = 1.0 - gUVI.y;   // FlipY（与天空分支一致）
            vec3 galaxyIBL = colors_LogLuv32ToSRGB(texture(galaxyTex, gUVI));
            galaxyIBL *= clamp((-pc.sunDir.y - 0.1) * 5.0, 0.0, 1.0);   // 夜晚因子（白天被淹没）
            galaxyIBL *= smoothstep(0.0, 0.3, reflectDir.y);             // 地平线遮挡
            prefiltered += galaxyIBL * 0.15;
        }
        // 2026-08-15：split-sum specular（F·A + B——BRDF LUT 精确积分，替代 Karis 近似）+ specOcclusion（AO 对反射的遮挡修正）
        vec2 brdfL = texture(brdfLUT, vec2(NoV, roughness)).rg;
        float specOcclusion = clamp(pow(NoV + ao, exp2(-16.0 * roughness - 1.0)) - 1.0 + ao, 0.0, 1.0);
        // 2026-08-16 官方（Khronos glTF-Sample-Renderer getIBLGGXFresnel 完整版）语义对齐：
        // FssEss = k_S·A + B（split-sum specular 系数）+ FmsEms（Fdez-Aguera 多重散射补偿——
        // 粗糙/低 F0 表面（如黑色金属面罩）能量守恒增强：单次散射丢失的能量补回；
        // 官方 pbr.frag 金属（f_metal_fresnel_ibl）与非金属（f_dielectric_fresnel_ibl）都用完整版；
        // specularWeight=1（mikan 无 KHR_materials_specular）
        // BRDF LUT 的 A/B 是按 F = F0(1-Fc)+Fc 积分得到的，必须使用材质 F0；
        // 将已经带观察角/粗糙度的 F_ibl 再代入会重复修改 Fresnel 能量。
        vec3 FssEss = F0 * brdfL.x + brdfL.y;
        float Ems = (1.0 - (brdfL.x + brdfL.y));
        vec3 F_avg = F0 + (1.0 - F0) / 21.0;
        vec3 FmsEms = Ems * FssEss * F_avg / max(1.0 - F_avg * Ems, 1e-4);
        vec3 FssEssTotal = FssEss + FmsEms;
        vec3 prefilteredSpec = prefiltered * specOcclusion;
        vec3 specularIBL = FssEssTotal * prefilteredSpec;
        // 恢复完整环境光：SH 漫反射和 cubemap 镜面反射都不受方向光 CSM 衰减。
        // shadowFactor 只控制太阳/月光直射；不修改 albedo。
        vec3 ambient = (kD_ibl * diffuseIBL + specularIBL) * ao;
        // 注：官方另有多重散射补偿（FmsEms——Fdez-Aguera）会增强粗糙表面反射亮度——
        // 与当前"降反射"诉求相反，留待完整 glTF 渲染阶段再加
#endif

        // 自发光（2026-08-13：强度从 mat 解析——model.frag 输出贴图亮度 0-1；×10 在合成端放大）：
        // ⚠️ 8bit 材质附件 clamp 到 1，model.frag 内放大无效——合成端 composite 是 r11g11b10 HDR 才放大得了。
        // 效果：发光体 composite 亮度 = albedo×strength×10（亮部 2~10，跨 soft-knee 阈值 1.0 触发 bloom；
        // 暗纹理仍被 soft-knee 过滤）。系数 10 可调（↓光晕弱 ↑更强）
        vec3 emissive = albedo * emissiveStrength * 2.0;

        // ===== 点光源（2026-08-13）：≤16 个走全遍历（少量光源 cluster 是净负收益：dispatch 开销 + 深度切片硬边）；
        // >8 个走 cluster 分块剔除（12×12×24，指数深度切片）=====
        // ⚠️ 2026-08-14：worldPos 已统一在 main 前部定义（*2-1）——点光源共用（原直传重建删除）
        if (pointLights.count <= 16) {
            // ---- 全遍历（第一步代码保留）：无 cluster 开销/边界瑕疵 ----
            for (int i = 0; i < pointLights.count; i++) {
                vec3 plPos = pointLights.lights[i].position_range.xyz;
                float plRange = pointLights.lights[i].position_range.w;
                vec3 plColor = pointLights.lights[i].color_intensity.rgb;
                float plIntensity = pointLights.lights[i].color_intensity.w;
                if (plRange <= 0.0) continue;
                vec3 toLight = plPos - worldPos;
                float plDist = length(toLight);
                if (plDist > plRange) continue;
                vec3 Lp = toLight / plDist;
                float NoLp = clamp(dot(n, Lp), 0.0, 1.0);
                if (NoLp <= 0.0) continue;
                float att = clamp(1.0 - plDist / plRange, 0.0, 1.0);
                att *= att;   // 平滑衰减（1-d/r）²
                vec3 Hp = normalize(viewDir + Lp);
                float NoHp = clamp(dot(n, Hp), 0.0, 1.0);
                float VoHp = clamp(dot(viewDir, Hp), 0.0, 1.0);
                float Dp = D_GGX(NoHp, roughness);
                float VisP = V_SmithGGXCorrelated(NoV, NoLp, roughness);
                vec3 Fp = F_Schlick(F0, VoHp);
                vec3 plLight = plColor * plIntensity * att * PointShadowFactor(worldPos, plPos, plRange, int(pointLights.lights[i].shadow_info.x), n);   // 2026-08-13：+点光源阴影（PCF）
                diffuse += (1.0 - Fp) * (1.0 - metallic) * albedo * plLight * NoLp;
                specular += Dp * VisP * Fp * plLight * NoLp;   // 2026-08-16 去 SPECULAR_SCALE 0.3（同 sunLight——物理亮度）
            }
        } else {
        // ---- cluster 查询（光源多时）：像素 → cluster → 只遍历 lightIndices[] ----
        vec4 cpar = clusterGrid.clusterParams;
        float nearP = cpar.x, farP = cpar.y;
        float scrW = cpar.z, scrH = cpar.w;
        vec4 vpZ = pc.invProj * vec4(ndcV, depth, 1.0);   // 2026-08-15 定稿：与 worldPos 同式（附件 = NDC z 直存）
        float viewZ = max(-vpZ.z / vpZ.w, nearP);   // 正深度
        uint tx = uint(min(gl_FragCoord.x / max(scrW / float(CLUSTER_X), 1.0), float(CLUSTER_X - 1)));
        uint ty = uint(min(gl_FragCoord.y / max(scrH / float(CLUSTER_Y), 1.0), float(CLUSTER_Y - 1)));
        float zFrac = clamp(log(viewZ / nearP) / log(farP / nearP), 0.0, 0.999999);
        uint tz = uint(zFrac * float(CLUSTER_Z));
        uint ci = tx + ty * uint(CLUSTER_X) + tz * uint(CLUSTER_X * CLUSTER_Y);
        Cluster cl = clusterGrid.clusters[ci];
        for (uint k = 0; k < cl.count; k++) {
            uint i = cl.lightIndices[k];
            vec3 plPos = pointLights.lights[i].position_range.xyz;
            float plRange = pointLights.lights[i].position_range.w;
            vec3 plColor = pointLights.lights[i].color_intensity.rgb;
            float plIntensity = pointLights.lights[i].color_intensity.w;
            if (plRange <= 0.0) continue;
            vec3 toLight = plPos - worldPos;
            float plDist = length(toLight);
            if (plDist > plRange) continue;
            vec3 Lp = toLight / plDist;
            float NoLp = clamp(dot(n, Lp), 0.0, 1.0);
            if (NoLp <= 0.0) continue;
            float att = clamp(1.0 - plDist / plRange, 0.0, 1.0);
            att *= att;   // 平滑衰减（1-d/r）²
            vec3 Hp = normalize(viewDir + Lp);
            float NoHp = clamp(dot(n, Hp), 0.0, 1.0);
            float VoHp = clamp(dot(viewDir, Hp), 0.0, 1.0);
            float Dp = D_GGX(NoHp, roughness);
            float VisP = V_SmithGGXCorrelated(NoV, NoLp, roughness);
            vec3 Fp = F_Schlick(F0, VoHp);
            vec3 plLight = plColor * plIntensity * att * PointShadowFactor(worldPos, plPos, plRange, int(pointLights.lights[i].shadow_info.x), n);   // 2026-08-13：+点光源阴影（PCF）
            diffuse += (1.0 - Fp) * (1.0 - metallic) * albedo * plLight * NoLp;
            specular += Dp * VisP * Fp * plLight * NoLp;   // 2026-08-16 去 SPECULAR_SCALE 0.3（同 sunLight——物理亮度）
        }
        }   // else：cluster 查询分支结束

        // 2026-08-13 用户方案：自发光加回合成输出（light 含 emissive）——gtao_apply 用 (1-emissiveStrength) 削弱 AO，发光区域不被遮挡
        color = diffuse + specular + ambient + emissive;
    }

    outColor = vec4(color, 1.0);   // 2026-08-11 用户拍板：合成直出线性 HDR（r11g11b10 附件）——不 LogLuv32 编码（8bit 编码+tonemap 解码精度损失）
}
