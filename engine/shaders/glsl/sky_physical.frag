#version 450

// 物理天空（大气散射）——Bruneton 风格单次+多重散射，参考 voxel engine sky.inc（Elek-Oskar09）
// 渲染到低分辨率天空 RT：每像素重建视线方向，40 步大气积分，Rayleigh + Mie + 臭氧 + 多重散射 LUT + 太阳圆盘

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D lutCombined; // 合并 LUT（160x64 图集）：左 128x64 透射率（tra2atm）+ 右上 32x32 多重散射（getmultisca）——FMDS RN4 单 LUT 方案

layout(push_constant) uniform PC {
    vec4 sunDir;   // xyz = 世界太阳方向（指向太阳）——天空图与相机无关，只需太阳方向
} pc;

// ================= 工具函数（math.inc） =================
const float PI = 3.1415926;

float pow2(float x) { return x * x; }
vec3 pow2(vec3 x) { return x * x; }
float pow3_2(float x) { return x * sqrt(x); }
vec3 pow3_2(vec3 x) { return x * sqrt(x); }
float pow4(float x) { float x2 = x * x; return x2 * x2; }
float pow5_2(float x) { float x2 = x * x; return x2 * sqrt(x); }
float saturate(float x) { return clamp(x, 0.0, 1.0); }
vec2 saturate(vec2 x) { return clamp(x, vec2(0.0), vec2(1.0)); }
vec3 saturate(vec3 x) { return clamp(x, vec3(0.0), vec3(1.0)); }
float max0(float x) { return max(0.0, x); }
vec3 max0(vec3 x) { return max(vec3(0.0), x); }
float linestep(float a, float b, float x) { return saturate((x - a) / (b - a)); }
float remap(float x, float t1, float t2, float s1, float s2) { return (x - t1) / (t2 - t1) * (s2 - s1) + s1; }
float safeacos(float x) { return acos(clamp(x, -1.0, 1.0)); }

// ================= 大气常量（sky.inc） =================
const float Mie = 0.000001;
const vec3 Rayleigh = vec3(0.175, 0.408, 1.000);
const vec3 Ozen = vec3(0.010, 0.025, 0.001);
const vec3 PM_AIR = vec3(0.15, 0.70, 0.15);   // FMDS RN4 sky.inc:18
const vec3 G_AIR = vec3(-0.15, 0.76, 0.15);
const vec3 PM_FOG = vec3(0.10, 0.80, 0.10);
const vec3 G_FOG = vec3(-0.15, 0.76, 0.15);
const float Den = 0.08;
const float Scale = 0.7;
const vec3 SUNLIG = vec3(1.0) * 12.0;   // 阳光亮度（FMDS RN4 basic.inc:92 原值）
const vec3 MOOLIG = vec3(0.5, 0.7, 1.0) * 4.0; // 月光亮度（FMDS RN4 basic.inc:95 原值）

const float EARTH_R = 6371.0;
const float ATMOSPHERE_H = 80.0;   // 2026-08-11：对齐 FMDS RN4（LUT 按 80km 烘焙；旧值 100）
const float EARTH_R2 = EARTH_R * EARTH_R;
const float ATMOSPHERE_R = EARTH_R + ATMOSPHERE_H;
const float ATMOSPHERE_R2 = ATMOSPHERE_R * ATMOSPHERE_R;

const float sunR = 0.009512;   // 太阳角半径（rad，FMDS RN4 sky.inc）——地平线阴影过渡宽度

// 散射/吸收系数（海拔密度分布）——FMDS RN4 sky.inc:154-160（LUT 按此烘焙，采样端必须一致）
const vec3 LR = vec3(5.8, 13.5, 33.1) * 0.001;  // Rayleigh
const vec3 LM = vec3(4.4) * 0.001;              // Mie（旧值 2.2*0.003=6.6e-3，与 LUT 烘焙不匹配）
const vec3 O3 = vec3(0.00065, 0.001881, 0.000085); // 臭氧（FMDS sky.inc:156）
const vec3 RO = 0.0 + LR;
const float HR = 8.0;   // Rayleigh 标高（km）
const float HM = 1.2;   // Mie 标高（km）
const vec2 HD = vec2(HR, HM);

// 臭氧层密度（2026-08-11 对齐 FMDS RN4 sky.inc:183：峰值 15km、半宽 25km 三角形；旧值 25/15 与 LUT 烘焙不符）
float dozen(float h) {
    return max0(1.0 - (abs(max0(h) - 15.0) / 25.0));
}

// ================= 相位函数 =================
float getpr(float c) {
    const vec2 mulAdd = vec2(0.1, 0.28) * (1.0 / PI);
    return c * mulAdd.x + mulAdd.y;
}

float getpm3(float c, const vec3 pm3, const vec3 g3) {
    vec3 tmp = (3.0 / (8.0 * PI)) * ((1.0 - g3 * g3) * (1.0 + c * c)) / ((2.0 + g3 * g3) * pow3_2(1.0 + g3 * g3 - 2.0 * g3 * c));
    return dot(tmp, pm3);
}

float getpmHG(float c, float g) {
    return ((1.0 - g * g) / pow3_2((1.0 + g * g - 2.0 * g * c))) / (4.0 * PI);
}

float getpm3HG(float c, const vec3 pm3, const vec3 g3) {
    vec3 tmp = ((1.0 - g3 * g3) / pow3_2((1.0 + g3 * g3 - 2.0 * g3 * c))) / (4.0 * PI);
    return dot(tmp, pm3);
}

// 2026-08-11：FMDS RN4 LUT.png（192x32 图集）——透射率区 = 左 128x32，多散射区 = 右 32x32（x∈[160,192)）
// 采样端 clamp 到各区半像素内（FMDS sky.inc:214）
const vec2 LUT_SIZE = vec2(192.0, 32.0);
const vec2 LUT_T_SCL = vec2(128.0, 32.0) / LUT_SIZE;    // 透射率区缩放
const vec2 LUT_T_CLAMP = vec2(0.5 / 128.0, 0.5 / 32.0); // 透射率区半像素下限
const vec2 LUT_T_CLAMP_MAX = vec2(127.5 / 128.0, 31.5 / 32.0);
const vec2 LUT_G_OFF = vec2(160.0, 0.0) / LUT_SIZE;     // 多散射区偏移（右列）
const vec2 LUT_G_SCL = vec2(32.0, 32.0) / LUT_SIZE;

// 2026-08-11：LUT 采样手动翻转 y 轴（用户确认）——
// mikan 纹理上传固定翻转（TexturePool.cpp L932 flippedY → GPU v=0 = 图像底部），
// atmo_lut_combined.png = FMDS 原版（v=0 地面在图像顶部）→ 采样端必须 1.0-v 让 v=0 取到地面（暗）。

float getdaylum(vec3 zen) {
    // FMDS RN4 原算法（sky.inc:102-104）：晨昏（太阳低于地平线）pow20 指数增亮（最多 500x）
    return clamp(remap(zen.y, 0.0, 0.15, 4.0, 1.0) * pow(max0(-zen.y) + 0.99, 20.0), 1.0, 500.0);
}

// ================= 透射率表（FMDS LUT 左部 128x32） =================
vec3 tra2atm(vec3 p, vec3 dir) {
    vec3 up = normalize(p);
    float cos_theta = dot(up, dir);
    float r = length(p);

    float H = sqrt(ATMOSPHERE_R2 - EARTH_R2);
    float rho = sqrt(r * r - EARTH_R2);

    float discriminant = r * r * (cos_theta * cos_theta - 1.0) + ATMOSPHERE_R2;
    float d = max0(sqrt(max0(discriminant)) - r * cos_theta);

    float d_min = ATMOSPHERE_R - r;
    float d_max = rho + H;
    if (d < d_min) return vec3(1.0);
    if (d > d_max) return vec3(0.0);

    float u = (d - d_min) / max0(d_max - d_min);
    float v = rho / H;
    vec2 lutUV = clamp(vec2(u, 1.0 - v) * LUT_T_SCL, LUT_T_CLAMP, LUT_T_CLAMP_MAX);   // 手动翻转 y：FMDS 原版 LUT 的 v=0 在图像顶部（mikan 上传后 v=0=底部）
    vec3 transmittance = texture(lutCombined, lutUV).rgb;

    // 地平线地球阴影（FMDS RN4 基础 + 2026-08-11 调整）：阴影只从"地平线以下"开始渐入——
    // 原版 smoothstep(-w,w,x) 在地平线处（x=0）就给 0.5 衰减 → 地平线发暗；
    // 现实空气密度：水平视线穿过大气路径最长 → 散射累积最多 → 地平线应最亮（发白），
    // 低于地平线后太阳光被地球遮挡才平滑变暗。shadow=1 直到视线低于地平线 ~3° 开始过渡。
    float sin_theta_h = EARTH_R / r;
    float cos_theta_h = -sqrt(max0(1.0 - sin_theta_h * sin_theta_h));
    float x = cos_theta - cos_theta_h;   // >0 视线高于地平线
    float h = sin_theta_h * sunR * 6.0;  // 过渡半宽（~3.3°），可调
    float shadow = 1.0 - smoothstep(0.0, h, -x);
    return transmittance * shadow;
}

// ================= 多重散射表（FMDS LUT 右列 32x32） =================
vec3 getmultisca(vec3 marpos, vec3 sunvec, float r2) {
    vec2 d = saturate(exp((-sqrt(r2) + EARTH_R) / HD));
    vec3 sigma = (LR) * d.x + (LM) * d.y;

    float PoL = dot(normalize(marpos), sunvec);
    vec2 uv = vec2(PoL * 0.5 + 0.5, 1.0 - (sqrt(r2) - EARTH_R) / ATMOSPHERE_H);   // 手动翻转 y（同 tra2atm）
    // 2026-08-11：FMDS LUT 烘焙约定——无 /25（旧自烘焙 LUT 才有），采样端须与 sky.inc:272/274 完全一致
    vec3 G_ALL = texture(lutCombined, LUT_G_OFF + uv * LUT_G_SCL).rgb;
    return G_ALL * sigma * (vec3(5.8, 13.5, 33.1) + 4.4) * 0.001 / 0.95;
}

// ================= 物理天空主函数（40 步大气积分） =================
vec3 physicalsky(vec3 skylig, vec3 marpos, vec3 marvec, vec3 sunvec) {
    float VoL = dot(marvec, sunvec);
    float PoL = dot(marpos, -sunvec);
    float PoV = dot(marpos, -marvec);
    float r2 = dot(marpos, marpos);

    vec2 t = vec2(PoV * PoV + ATMOSPHERE_R2 - r2, PoV * PoV + EARTH_R2 - r2);
    vec2 aid = t.x < 0.0 ? vec2(-1.0) : (vec2(-1.0, 1.0) * sqrt(t.x) + PoV);
    vec2 pid = t.y < 0.0 ? vec2(-1.0) : (vec2(-1.0, 1.0) * sqrt(t.y) + PoV);

    bool pd = pid.y >= 0.0;
    float l = ((pd && pid.x > 0.0) ? pid.x : aid.y) - ((pd && pid.x < 0.0) ? pid.y : max0(aid.x));
    float dl = l * (0.999 / 40.0);   // VIEWSAM = 40

    vec3 dp = marvec * dl;

    vec3 d0 = vec3(0.0);
    vec3 d1 = saturate(vec3(exp((-sqrt(r2) + EARTH_R) / HD), dozen(sqrt(r2) - EARTH_R)));
    vec3 d = vec3(0.0);

    vec3 ext1 = (-RO) * d1.x + (-LM) * d1.y + (-O3) * d1.z;
    vec3 ext = vec3(0.0);

    vec3 s_t1 = tra2atm(marpos, sunvec);
    vec3 m_t1 = tra2atm(marpos, -sunvec);
    vec3 t2 = vec3(1.0);

    vec3 s_inscar = vec3(0.0);
    vec3 s_inscar0 = vec3(0.0);
    vec3 s_inscar1 = s_t1 * d1.x;
    vec3 s_inscam = vec3(0.0);
    vec3 s_inscam0 = vec3(0.0);
    vec3 s_inscam1 = s_t1 * d1.y;
    vec3 m_inscar = vec3(0.0);
    vec3 m_inscar0 = vec3(0.0);
    vec3 m_inscar1 = m_t1 * d1.x;
    vec3 m_inscam = vec3(0.0);
    vec3 m_inscam0 = vec3(0.0);
    vec3 m_inscam1 = m_t1 * d1.y;
    vec3 s_multisca = vec3(0.0);
    vec3 m_multisca = vec3(0.0);

    for (float i = 0.0; i < 40.0; ++i) {
        marpos += dp;
        r2 = dot(marpos, marpos);

        d0 = d1;
        d1 = saturate(vec3(exp((-sqrt(r2) + EARTH_R) / HD), dozen(sqrt(r2) - EARTH_R)));
        d = (d0 + d1) * 0.5;

        ext = (-LR) * d.x + (-LM) * d.y + (-O3) * d.z;
        vec3 samtra = exp(ext * dl);

        s_t1 = tra2atm(marpos, sunvec);
        m_t1 = tra2atm(marpos, -sunvec);
        t2 *= samtra;

        s_inscar0 = s_inscar1;
        s_inscar1 = s_t1 * t2 * d1.x;
        s_inscar += (s_inscar0 + s_inscar1);

        s_inscam0 = s_inscam1;
        s_inscam1 = s_t1 * t2 * d1.y;
        s_inscam += (s_inscam0 + s_inscam1);

        m_inscar0 = m_inscar1;
        m_inscar1 = s_t1 * t2 * d1.x;
        m_inscar += (m_inscar0 + m_inscar1);

        m_inscam0 = m_inscam1;
        m_inscam1 = m_t1 * t2 * d1.y;
        m_inscam += (m_inscam0 + m_inscam1);

        s_multisca += t2 * getmultisca(marpos, sunvec, r2);
        m_multisca += t2 * getmultisca(marpos, -sunvec, r2);
    }

    s_inscar *= 0.5 * dl;
    s_inscam *= 0.5 * dl;
    s_multisca *= dl;
    m_inscar *= 0.5 * dl;
    m_inscam *= 0.5 * dl;
    m_multisca *= dl;

    float s_pr = getpr(VoL);
    float s_pm = getpm3(VoL, PM_AIR, G_AIR);
    float m_pr = getpr(-VoL);
    float m_pm = getpm3(-VoL, PM_AIR, G_AIR);

    vec3 insca = (s_inscar * LR * s_pr + s_inscam * LM * s_pm + s_multisca) * SUNLIG * getdaylum(sunvec);
    insca += (m_inscar * LR * m_pr + m_inscam * LM * m_pm + m_multisca) * MOOLIG;
    vec3 outsca = (pid.x >= 0.0 ? vec3(0.0) : skylig) * t2;

    return max0(insca + outsca);
}

// ================= 全景天空图投影（FMDS sky.inc skylutdir 移植） =================
// 与 fullscreen.frag 的 skylutuv 互为逆（同一 CAM_ALT=0.2、EARTH_R 球面投影 + pow4 扭曲）：
//  - 方位角：u 线性（FMDS 约定，与采样端 atan(-x,-z)/(2π)+0.5 自洽，模 2π）
//  - 高度角：先经 pow4 扭曲段（adjV）再解 altitudeAngle，含地平线 -π/2 近似（误差 <0.5°，FMDS 原版设计）
vec3 skylutdir(vec2 uv) {
    const float CAM_ALT = 0.2;          // 地表相机高度（km，与 fullscreen.frag skylutuv 一致）
    float r = EARTH_R + CAM_ALT;
    float r2 = r * r;

    float s = EARTH_R / r;
    float y = -sqrt(1.0 - s * s);
    float t = saturate(y * 0.5 + 0.5);

    float x = mix(linestep(t, 1.0, uv.y), linestep(t, 0.0, uv.y), float(uv.y < t));
    x = pow4(x);                        // skylutuv 的 sqrt(sqrt(·)) 逆

    float azimuthAngle = uv.x * 2.0 * PI;
    float adjV = mix((x * (1.0 - t) + t), (-x * t + t), float(uv.y < t)) * 2.0 - 1.0;

    float horizonAngle = safeacos(sqrt(r2 - EARTH_R2) / r) - 0.5 * PI;
    float altitudeAngle = adjV * 0.5 * PI - horizonAngle;

    float cosAltitude = cos(altitudeAngle);
    return vec3(cosAltitude * sin(azimuthAngle), sin(altitudeAngle), cosAltitude * cos(azimuthAngle));
}

// ================= LogLuv32 编码（FMDS basic.inc colors_sRGBToLogLuv32，ERI07） =================
// fullscreen.frag 按 FMDS sky LUT 约定用 colors_LogLuv32ToSRGB 解码天空 RT（还原线性 HDR），
// 故写入端必须做同款编码；天空 RT 为 RGBA16F，LogLuv 的 0-255 整数范围在 16F 中精确可表示
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

// ================= main =================
// 全景天空图（FMDS skylutuv/skylutdir 球面投影）：天空颜色只取决于视线方向（物理天空为方向函数），
// 与相机无关 → 所有视口共用一张全景图；输出 LogLuv32 编码，合成阶段解码后 + 全分辨率太阳 + 云
void main() {
    vec3 skyvec = skylutdir(fragUV);

    // 地表相机（固定在大气层内 0.2km 高度；全景图不依赖具体相机位置，与 skylutdir 的 CAM_ALT 一致）
    vec3 campos = vec3(0.0, EARTH_R + 0.2, 0.0);

    vec3 sun = normalize(pc.sunDir.xyz);

    // 太阳圆盘不在天空 RT 内绘制（低分辨率 <1px 无意义）：由 fullscreen.frag 全分辨率绘制
    vec3 color = physicalsky(vec3(0.0), campos, skyvec, sun);

    outColor = colors_sRGBToLogLuv32(color);
}
