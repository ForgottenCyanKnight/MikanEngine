// ============================================================================
// atmo_common.glsl — Bruneton 2017 预计算大气散射（mikan 移植）
// 移植自 precomputed_atmospheric_scattering/atmosphere/functions.glsl
// 约定：长度单位=米（m）；3 固定波长 680/550/440nm（R/G/B）；combined 散射 LUT
// （RGB=瑞利+多次散射（除以瑞利相函数）、A=单次 Mie 的 R 通道，B/G 由外推还原）
// 阶段 1：仅单次散射（RGB=单次瑞利、A=单次 Mie.R）；多次散射迭代为阶段 2
// ============================================================================
#ifndef ATMO_COMMON_GLSL
#define ATMO_COMMON_GLSL

// ===== 纹理尺寸（Bruneton 官方） =====
const int TRANSMITTANCE_TEXTURE_WIDTH  = 256;
const int TRANSMITTANCE_TEXTURE_HEIGHT = 64;
const int SCATTERING_TEXTURE_R_SIZE    = 32;
const int SCATTERING_TEXTURE_MU_SIZE   = 128;
const int SCATTERING_TEXTURE_MU_S_SIZE = 32;
const int SCATTERING_TEXTURE_NU_SIZE   = 8;
const int SCATTERING_TEXTURE_WIDTH     = SCATTERING_TEXTURE_NU_SIZE * SCATTERING_TEXTURE_MU_S_SIZE; // 256
const int SCATTERING_TEXTURE_HEIGHT    = SCATTERING_TEXTURE_MU_SIZE;                               // 128
const int SCATTERING_TEXTURE_DEPTH     = SCATTERING_TEXTURE_R_SIZE;                                // 32

const float PI  = 3.14159265358979323846;
const float RAD = 1.0;   // Bruneton 弧度制（definitions.glsl: rad=1.0——Angle 即弧度）；旧 π/180 使太阳圆盘边界放大 64 倍（2026-08-11 修正）

// ===== 大气参数（Bruneton 2017 官方 demo 默认值，3 波长） =====
const float  BOTTOM_RADIUS      = 6371000.0;   // 地球半径 (m) = 6371 km
const float  TOP_RADIUS         = 6431000.0;   // 大气顶 (m)，高 60km
const float  SUN_ANGULAR_RADIUS = 0.004675;    // 太阳角半径 (rad)
const float  MIE_PHASE_FUNCTION_G = 0.8;
const float  MU_S_MIN           = -0.2;        // 太阳天顶角上限 102°
const vec3   SOLAR_IRRADIANCE   = vec3(1.474, 1.8504, 2.3612);        // W/m²/nm（680/550/440nm）
const vec3   RAYLEIGH_SCATTERING = vec3(5.802e-6, 13.558e-6, 33.1e-6);  // m⁻¹
const vec3   MIE_SCATTERING     = vec3(3.996e-6, 3.996e-6, 3.996e-6);   // m⁻¹
const vec3   MIE_EXTINCTION     = vec3(4.4e-6, 4.4e-6, 4.4e-6);         // m⁻¹（含吸收）
const vec3   ABSORPTION_EXTINCTION = vec3(0.65e-6, 1.881e-6, 0.085e-6); // 臭氧 (m⁻¹)
const float  GROUND_ALBEDO = 0.1;   // 地表平均反照率（Bruneton 默认；多次散射地面反弹用）

// ===== 基础工具 =====
float ClampCosine(float x) { return clamp(x, -1.0, 1.0); }
float ClampDistance(float d) { return max(d, 0.0); }
float SafeSqrt(float a) { return sqrt(max(a, 0.0)); }

// 到大气顶的距离（射线 (r,mu) 不碰地）
float DistanceToTopAtmosphereBoundary(float r, float mu) {
    float discriminant = r * r * (mu * mu - 1.0) + TOP_RADIUS * TOP_RADIUS;
    return ClampDistance(-r * mu + SafeSqrt(discriminant));
}
// 到地面/大气底的距离（射线 (r,mu) 碰地）
float DistanceToBottomAtmosphereBoundary(float r, float mu) {
    float discriminant = r * r * (mu * mu - 1.0) + BOTTOM_RADIUS * BOTTOM_RADIUS;
    return ClampDistance(-r * mu - SafeSqrt(discriminant));
}
bool RayIntersectsGround(float r, float mu) {
    return mu < 0.0 && r * r * (mu * mu - 1.0) + BOTTOM_RADIUS * BOTTOM_RADIUS >= 0.0;
}
float DistanceToNearestAtmosphereBoundary(float r, float mu, bool intersectsGround) {
    return intersectsGround ? DistanceToBottomAtmosphereBoundary(r, mu) : DistanceToTopAtmosphereBoundary(r, mu);
}

// ===== 密度剖面（两层：层0 从地面到 width，层1 从 width 到顶） =====
// rayleigh：h<0 → 1（垫层）；h≥0 → exp(-h/8km)
// mie：      h<0 → 1；h≥0 → exp(-h/1.2km)
// ozone：    h<25km → h/15km 线性升；h≥25km → 线性降至 0（层厚 ~15km）
float GetRayleighDensity(float altitude) {
    return altitude < 0.0 ? 1.0 : clamp(exp(-altitude / 8000.0), 0.0, 1.0);
}
float GetMieDensity(float altitude) {
    return altitude < 0.0 ? 1.0 : clamp(exp(-altitude / 1200.0), 0.0, 1.0);
}
float GetAbsorptionDensity(float altitude) {
    return altitude < 25000.0 ? clamp(altitude / 15000.0, 0.0, 1.0) : clamp(-altitude / 15000.0, 0.0, 1.0);
}

// ===== 光学厚度（500 步梯形积分，到大气顶） =====
float ComputeOpticalLengthToTopAtmosphereBoundary(float r, float mu, int profile) {
    const int SAMPLE_COUNT = 500;
    float dx = DistanceToTopAtmosphereBoundary(r, mu) / float(SAMPLE_COUNT);
    float result = 0.0;
    for (int i = 0; i <= SAMPLE_COUNT; ++i) {
        float d_i = float(i) * dx;
        float r_i = sqrt(d_i * d_i + 2.0 * r * mu * d_i + r * r);
        float y_i;
        if (profile == 0) y_i = GetRayleighDensity(r_i - BOTTOM_RADIUS);
        else if (profile == 1) y_i = GetMieDensity(r_i - BOTTOM_RADIUS);
        else y_i = GetAbsorptionDensity(r_i - BOTTOM_RADIUS);
        float weight_i = (i == 0 || i == SAMPLE_COUNT) ? 0.5 : 1.0;
        result += y_i * weight_i * dx;
    }
    return result;
}

// 到大气顶的透射率（光谱）
vec3 ComputeTransmittanceToTopAtmosphereBoundary(float r, float mu) {
    return exp(-(RAYLEIGH_SCATTERING * ComputeOpticalLengthToTopAtmosphereBoundary(r, mu, 0) +
                 MIE_EXTINCTION     * ComputeOpticalLengthToTopAtmosphereBoundary(r, mu, 1) +
                 ABSORPTION_EXTINCTION * ComputeOpticalLengthToTopAtmosphereBoundary(r, mu, 2)));
}

// ===== transmittance LUT 坐标映射（增强地平线采样） =====
float GetTextureCoordFromUnitRange(float x, int textureSize) {
    return 0.5 / float(textureSize) + x * (1.0 - 1.0 / float(textureSize));
}
float GetUnitRangeFromTextureCoord(float u, int textureSize) {
    return (u - 0.5 / float(textureSize)) / (1.0 - 1.0 / float(textureSize));
}
vec2 GetTransmittanceTextureUvFromRMu(float r, float mu) {
    float H = sqrt(TOP_RADIUS * TOP_RADIUS - BOTTOM_RADIUS * BOTTOM_RADIUS);
    float rho = SafeSqrt(r * r - BOTTOM_RADIUS * BOTTOM_RADIUS);
    float d = DistanceToTopAtmosphereBoundary(r, mu);
    float d_min = TOP_RADIUS - r;
    float d_max = rho + H;
    float x_mu = (d - d_min) / (d_max - d_min);
    float x_r = rho / H;
    return vec2(GetTextureCoordFromUnitRange(x_mu, TRANSMITTANCE_TEXTURE_WIDTH),
                GetTextureCoordFromUnitRange(x_r, TRANSMITTANCE_TEXTURE_HEIGHT));
}
void GetRMuFromTransmittanceTextureUv(vec2 uv, out float r, out float mu) {
    float x_mu = GetUnitRangeFromTextureCoord(uv.x, TRANSMITTANCE_TEXTURE_WIDTH);
    float x_r  = GetUnitRangeFromTextureCoord(uv.y, TRANSMITTANCE_TEXTURE_HEIGHT);
    float H = sqrt(TOP_RADIUS * TOP_RADIUS - BOTTOM_RADIUS * BOTTOM_RADIUS);
    float rho = H * x_r;
    r = sqrt(rho * rho + BOTTOM_RADIUS * BOTTOM_RADIUS);
    float d_min = TOP_RADIUS - r;
    float d_max = rho + H;
    float d = d_min + x_mu * (d_max - d_min);
    mu = (d == 0.0) ? 1.0 : (H * H - rho * rho - d * d) / (2.0 * r * d);
    mu = ClampCosine(mu);
}

// ===== transmittance 查询（sampler 版：texture() 自带过滤） =====
vec3 GetTransmittanceToTopAtmosphereBoundary(sampler2D lut, float r, float mu) {
    vec2 uv = GetTransmittanceTextureUvFromRMu(r, mu);
    return texture(lut, uv).rgb;
}
// 两点间透射率（两查询相除；ray_r_mu_intersects_ground 时反转）
vec3 GetTransmittance(sampler2D lut, float r, float mu, float d, bool rayIntersectsGround) {
    float r_d = sqrt(d * d + 2.0 * r * mu * d + r * r);
    r_d = clamp(r_d, BOTTOM_RADIUS, TOP_RADIUS);
    float mu_d = ClampCosine((r * mu + d) / r_d);
    if (rayIntersectsGround) {
        return min(GetTransmittanceToTopAtmosphereBoundary(lut, r_d, -mu_d) /
                   GetTransmittanceToTopAtmosphereBoundary(lut, r, -mu), vec3(1.0));
    } else {
        return min(GetTransmittanceToTopAtmosphereBoundary(lut, r, mu) /
                   GetTransmittanceToTopAtmosphereBoundary(lut, r_d, mu_d), vec3(1.0));
    }
}
// 到太阳的透射率（太阳圆盘部分可见性 smoothstep）
vec3 GetTransmittanceToSun(sampler2D lut, float r, float mu_s) {
    float sin_theta_h = BOTTOM_RADIUS / r;
    float cos_theta_h = -sqrt(max(1.0 - sin_theta_h * sin_theta_h, 0.0));
    return GetTransmittanceToTopAtmosphereBoundary(lut, r, mu_s) *
        smoothstep(-sin_theta_h * SUN_ANGULAR_RADIUS / RAD,
                    sin_theta_h * SUN_ANGULAR_RADIUS / RAD,
                    mu_s - cos_theta_h);
}

// ===== 相函数（渲染时乘回，预计算时省略） =====
vec3 RayleighPhaseFunction(float nu) {
    float k = 3.0 / (16.0 * PI);
    return vec3(k * (1.0 + nu * nu));
}
vec3 MiePhaseFunction(float g, float nu) {
    float k = 3.0 / (8.0 * PI) * (1.0 - g * g) / (2.0 + g * g);
    return vec3(k * (1.0 + nu * nu) / pow(1.0 + g * g - 2.0 * g * nu, 1.5));
}

// ===== scattering LUT 4D→3D 坐标映射 =====
vec4 GetScatteringTextureUvwzFromRMuMuSNu(float r, float mu, float mu_s, float nu,
                                          bool rayIntersectsGround) {
    float H = sqrt(TOP_RADIUS * TOP_RADIUS - BOTTOM_RADIUS * BOTTOM_RADIUS);
    float rho = SafeSqrt(r * r - BOTTOM_RADIUS * BOTTOM_RADIUS);
    float u_r = GetTextureCoordFromUnitRange(rho / H, SCATTERING_TEXTURE_R_SIZE);

    float r_mu = r * mu;
    float discriminant = r_mu * r_mu - r * r + BOTTOM_RADIUS * BOTTOM_RADIUS;
    float u_mu;
    if (rayIntersectsGround) {
        float d = -r_mu - SafeSqrt(discriminant);
        float d_min = r - BOTTOM_RADIUS;
        float d_max = rho;
        u_mu = 0.5 - 0.5 * GetTextureCoordFromUnitRange(d_max == d_min ? 0.0 :
            (d - d_min) / (d_max - d_min), SCATTERING_TEXTURE_MU_SIZE / 2);
    } else {
        float d = -r_mu + SafeSqrt(discriminant + H * H);
        float d_min = TOP_RADIUS - r;
        float d_max = rho + H;
        u_mu = 0.5 + 0.5 * GetTextureCoordFromUnitRange(
            (d - d_min) / (d_max - d_min), SCATTERING_TEXTURE_MU_SIZE / 2);
    }

    float d = DistanceToTopAtmosphereBoundary(BOTTOM_RADIUS, mu_s);
    float d_min = TOP_RADIUS - BOTTOM_RADIUS;
    float d_max = H;
    float a = (d - d_min) / (d_max - d_min);
    float D = DistanceToTopAtmosphereBoundary(BOTTOM_RADIUS, MU_S_MIN);
    float A = (D - d_min) / (d_max - d_min);
    float u_mu_s = GetTextureCoordFromUnitRange(
        max(1.0 - a / A, 0.0) / (1.0 + a), SCATTERING_TEXTURE_MU_S_SIZE);

    float u_nu = (nu + 1.0) / 2.0;
    return vec4(u_nu, u_mu_s, u_mu, u_r);
}
void GetRMuMuSNuFromScatteringTextureUvwz(vec4 uvwz, out float r, out float mu,
                                          out float mu_s, out float nu,
                                          out bool rayIntersectsGround) {
    float H = sqrt(TOP_RADIUS * TOP_RADIUS - BOTTOM_RADIUS * BOTTOM_RADIUS);
    float rho = H * GetUnitRangeFromTextureCoord(uvwz.w, SCATTERING_TEXTURE_R_SIZE);
    r = sqrt(rho * rho + BOTTOM_RADIUS * BOTTOM_RADIUS);

    if (uvwz.z < 0.5) {
        float d_min = r - BOTTOM_RADIUS;
        float d_max = rho;
        float d = d_min + (d_max - d_min) * GetUnitRangeFromTextureCoord(
            1.0 - 2.0 * uvwz.z, SCATTERING_TEXTURE_MU_SIZE / 2);
        mu = (d == 0.0) ? -1.0 : ClampCosine(-(rho * rho + d * d) / (2.0 * r * d));
        rayIntersectsGround = true;
    } else {
        float d_min = TOP_RADIUS - r;
        float d_max = rho + H;
        float d = d_min + (d_max - d_min) * GetUnitRangeFromTextureCoord(
            2.0 * uvwz.z - 1.0, SCATTERING_TEXTURE_MU_SIZE / 2);
        mu = (d == 0.0) ? 1.0 : ClampCosine((H * H - rho * rho - d * d) / (2.0 * r * d));
        rayIntersectsGround = false;
    }

    float x_mu_s = GetUnitRangeFromTextureCoord(uvwz.y, SCATTERING_TEXTURE_MU_S_SIZE);
    float d_min = TOP_RADIUS - BOTTOM_RADIUS;
    float d_max = H;
    float D = DistanceToTopAtmosphereBoundary(BOTTOM_RADIUS, MU_S_MIN);
    float A = (D - d_min) / (d_max - d_min);
    float a = (A - x_mu_s * A) / (1.0 + x_mu_s * A);
    float d = d_min + min(a, A) * (d_max - d_min);
    mu_s = (d == 0.0) ? 1.0 : ClampCosine((H * H - d * d) / (2.0 * BOTTOM_RADIUS * d));

    nu = ClampCosine(uvwz.x * 2.0 - 1.0);
}
// 3D texel → (r,mu,mu_s,nu)：x 坐标解包 ν 层 + μs
void GetRMuMuSNuFromScatteringTextureFragCoord(vec3 fragCoord, out float r, out float mu,
                                               out float mu_s, out float nu,
                                               out bool rayIntersectsGround) {
    const vec4 SCATTERING_TEXTURE_SIZE = vec4(
        float(SCATTERING_TEXTURE_NU_SIZE - 1), float(SCATTERING_TEXTURE_MU_S_SIZE),
        float(SCATTERING_TEXTURE_MU_SIZE), float(SCATTERING_TEXTURE_R_SIZE));
    float frag_coord_nu = floor(fragCoord.x / float(SCATTERING_TEXTURE_MU_S_SIZE));
    float frag_coord_mu_s = mod(fragCoord.x, float(SCATTERING_TEXTURE_MU_S_SIZE));
    vec4 uvwz = vec4(frag_coord_nu, frag_coord_mu_s, fragCoord.y, fragCoord.z) / SCATTERING_TEXTURE_SIZE;
    GetRMuMuSNuFromScatteringTextureUvwz(uvwz, r, mu, mu_s, nu, rayIntersectsGround);
    nu = clamp(nu, mu * mu_s - sqrt((1.0 - mu * mu) * (1.0 - mu_s * mu_s)),
               mu * mu_s + sqrt((1.0 - mu * mu) * (1.0 - mu_s * mu_s)));
}

// ===== 单次散射（50 步梯形积分；省略太阳辐照度/散射系数/相函数——后补） =====
void ComputeSingleScatteringIntegrand(sampler2D lut, float r, float mu, float mu_s,
                                      float nu, float d, bool rayIntersectsGround,
                                      out vec3 rayleigh, out vec3 mie) {
    float r_d = sqrt(d * d + 2.0 * r * mu * d + r * r);
    r_d = clamp(r_d, BOTTOM_RADIUS, TOP_RADIUS);
    float mu_s_d = ClampCosine((r * mu_s + d * nu) / r_d);
    vec3 transmittance = GetTransmittance(lut, r, mu, d, rayIntersectsGround) *
        GetTransmittanceToSun(lut, r_d, mu_s_d);
    rayleigh = transmittance * GetRayleighDensity(r_d - BOTTOM_RADIUS);
    mie = transmittance * GetMieDensity(r_d - BOTTOM_RADIUS);
}
void ComputeSingleScattering(sampler2D lut, float r, float mu, float mu_s, float nu,
                             bool rayIntersectsGround, out vec3 rayleigh, out vec3 mie) {
    const int SAMPLE_COUNT = 50;
    float dx = DistanceToNearestAtmosphereBoundary(r, mu, rayIntersectsGround) / float(SAMPLE_COUNT);
    vec3 rayleigh_sum = vec3(0.0);
    vec3 mie_sum = vec3(0.0);
    for (int i = 0; i <= SAMPLE_COUNT; ++i) {
        float d_i = float(i) * dx;
        vec3 rayleigh_i, mie_i;
        ComputeSingleScatteringIntegrand(lut, r, mu, mu_s, nu, d_i, rayIntersectsGround, rayleigh_i, mie_i);
        float weight_i = (i == 0 || i == SAMPLE_COUNT) ? 0.5 : 1.0;
        rayleigh_sum += rayleigh_i * weight_i;
        mie_sum += mie_i * weight_i;
    }
    rayleigh = rayleigh_sum * dx * SOLAR_IRRADIANCE * RAYLEIGH_SCATTERING;
    mie = mie_sum * dx * SOLAR_IRRADIANCE * MIE_SCATTERING;
}

// ===== 散射查询（3D LUT，ν 方向两次采样插值模拟 4D） =====
// combined 模式：RGB=瑞利+多次（除以瑞利相函数）、A=单次 Mie.R
vec3 GetExtrapolatedSingleMieScattering(vec4 scattering) {
    if (scattering.r <= 0.0) return vec3(0.0);
    return scattering.rgb * scattering.a / scattering.r *
        (RAYLEIGH_SCATTERING.r / MIE_SCATTERING.r) * (MIE_SCATTERING / RAYLEIGH_SCATTERING);
}
void GetCombinedScattering(sampler3D lut, float r, float mu, float mu_s, float nu,
                           bool rayIntersectsGround, out vec3 scattering, out vec3 singleMie) {
    vec4 uvwz = GetScatteringTextureUvwzFromRMuMuSNu(r, mu, mu_s, nu, rayIntersectsGround);
    float tex_coord_x = uvwz.x * float(SCATTERING_TEXTURE_NU_SIZE - 1);
    float tex_x = floor(tex_coord_x);
    float lerp = tex_coord_x - tex_x;
    vec3 uvw0 = vec3((tex_x + uvwz.y) / float(SCATTERING_TEXTURE_NU_SIZE), uvwz.z, uvwz.w);
    vec3 uvw1 = vec3((tex_x + 1.0 + uvwz.y) / float(SCATTERING_TEXTURE_NU_SIZE), uvwz.z, uvwz.w);
    vec4 combined = texture(lut, uvw0) * (1.0 - lerp) + texture(lut, uvw1) * lerp;
    scattering = combined.rgb;
    singleMie = GetExtrapolatedSingleMieScattering(combined);
}

// ===== 地球阴影锥（2026-08-11 移植官方 fragment_shader.txt GetSphereShadowInOut；mikan 地球=半径 sphereRadius 的球，中心原点） =====
// 视线穿过地球在太阳光下的阴影锥的距离段 [dIn, dOut]——传给 GetSkyRadiance 排除该段太阳光散射
// （日落时阴影锥覆盖地平线附近天空 = twilight wedge，太阳每降 1° 阴影带升 1°，产生"落山陷入感"）
void GetSphereShadowInOut(vec3 camera, vec3 viewDirection, vec3 sunDirection,
                          float k, float sphereRadius,
                          out float dIn, out float dOut) {
    float posDotSun = dot(camera, sunDirection);
    float viewDotSun = dot(viewDirection, sunDirection);
    float l = 1.0 + k * k;
    float a = 1.0 - l * viewDotSun * viewDotSun;
    float b = dot(camera, viewDirection) - l * posDotSun * viewDotSun -
        k * sphereRadius * viewDotSun;
    float c = dot(camera, camera) - l * posDotSun * posDotSun -
        2.0 * k * sphereRadius * posDotSun - sphereRadius * sphereRadius;
    float discriminant = b * b - a * c;
    if (discriminant > 0.0) {
        dIn = max(0.0, (-b - sqrt(discriminant)) / a);
        dOut = (-b + sqrt(discriminant)) / a;
        float dBase = -posDotSun / viewDotSun;
        float dApex = -(posDotSun + sphereRadius / k) / viewDotSun;
        if (viewDotSun > 0.0) {
            dIn = max(dIn, dApex);
            dOut = a > 0.0 ? min(dOut, dBase) : dBase;
        } else {
            dIn = a > 0.0 ? max(dIn, dBase) : dBase;
            dOut = min(dOut, dApex);
        }
    } else {
        dIn = 0.0;
        dOut = 0.0;
    }
}

// ===== 天空辐射度（零实时积分；shadow_length=0 常规天空） =====
vec3 GetSkyRadiance(sampler2D transmittanceLUT, sampler3D scatteringLUT,
                    vec3 camera, vec3 viewRay, float shadowLength,
                    vec3 sunDirection, out vec3 transmittance) {
    float r = length(camera);
    float rmu = dot(camera, viewRay);
    float distanceToTop = -rmu - sqrt(rmu * rmu - r * r + TOP_RADIUS * TOP_RADIUS);
    if (distanceToTop > 0.0) {
        camera = camera + viewRay * distanceToTop;
        r = TOP_RADIUS;
        rmu += distanceToTop;
    } else if (r > TOP_RADIUS) {
        transmittance = vec3(1.0);
        return vec3(0.0);
    }
    float mu = rmu / r;
    float mu_s = dot(camera, sunDirection) / r;
    float nu = dot(viewRay, sunDirection);
    bool rayIntersectsGround = RayIntersectsGround(r, mu);

    transmittance = rayIntersectsGround ? vec3(0.0) :
        GetTransmittanceToTopAtmosphereBoundary(transmittanceLUT, r, mu);

    vec3 singleMie;
    vec3 scattering;
    if (shadowLength == 0.0) {
        GetCombinedScattering(scatteringLUT, r, mu, mu_s, nu, rayIntersectsGround, scattering, singleMie);
    } else {
        float d = shadowLength;
        float r_p = clamp(sqrt(d * d + 2.0 * r * mu * d + r * r), BOTTOM_RADIUS, TOP_RADIUS);
        float mu_p = (r * mu + d) / r_p;
        float mu_s_p = (r * mu_s + d * nu) / r_p;
        GetCombinedScattering(scatteringLUT, r_p, mu_p, mu_s_p, nu, rayIntersectsGround, scattering, singleMie);
        vec3 shadowTransmittance = GetTransmittance(transmittanceLUT, r, mu, shadowLength, rayIntersectsGround);
        scattering *= shadowTransmittance;
        singleMie *= shadowTransmittance;
    }
    return scattering * RayleighPhaseFunction(nu) +
           singleMie * MiePhaseFunction(MIE_PHASE_FUNCTION_G, nu);
}

#endif // ATMO_COMMON_GLSL

// ============================================================================
// 多次散射（Bruneton 2017 functions.glsl 移植；阶段 2）
// 迭代约定（严格 n-1 阶版，2026-08-11）：
//  - scatteringLUT.RGB = 单次瑞利（除相函数）+ Σ各阶多重（除瑞利相函数）；A = 单次 Mie.R
//  - deltaMultiLUT（3D，每阶覆盖）= 第 n 阶纯多次散射（含相函数）——下一阶的入射场
//  - deltaIrrLUT（2D 64×16，每阶覆盖）= 第 n 阶间接辐照度（纯）——下一阶的地面反弹场
//  - irradianceLUT（2D 64×16，逐阶累加）= 累计地面间接辐照度（渲染用，含 ≥1 bounce）
//  - 入射场：order==2 → 单次散射（combined 除相函数 × 相函数）；order>=3 → 纯上一阶 deltaMulti
//    （Neumann 级数第 n 阶 = 纯第 n-1 阶再散射一次；旧累计近似使各阶形状相同、观感像叠加）
// ============================================================================

// ===== 地面辐照度 LUT（64×16；u=(μs+1)/2、v=(r-bottom)/(top-bottom)，texcoord range 修正） =====
const int IRRADIANCE_TEXTURE_WIDTH  = 64;
const int IRRADIANCE_TEXTURE_HEIGHT = 16;
vec2 GetIrradianceTextureUvFromRMuS(float r, float mu_s) {
    float x_r = (r - BOTTOM_RADIUS) / (TOP_RADIUS - BOTTOM_RADIUS);
    float x_mu_s = mu_s * 0.5 + 0.5;
    return vec2(GetTextureCoordFromUnitRange(x_mu_s, IRRADIANCE_TEXTURE_WIDTH),
                GetTextureCoordFromUnitRange(x_r, IRRADIANCE_TEXTURE_HEIGHT));
}
vec3 GetIrradiance(sampler2D lut, float r, float mu_s) {
    vec2 uv = GetIrradianceTextureUvFromRMuS(r, mu_s);
    return texture(lut, uv).rgb;
}
void GetRMuSFromIrradianceTextureUv(vec2 uv, out float r, out float mu_s) {
    float x_mu_s = GetUnitRangeFromTextureCoord(uv.x, IRRADIANCE_TEXTURE_WIDTH);
    float x_r  = GetUnitRangeFromTextureCoord(uv.y, IRRADIANCE_TEXTURE_HEIGHT);
    r = BOTTOM_RADIUS + x_r * (TOP_RADIUS - BOTTOM_RADIUS);
    mu_s = ClampCosine(2.0 * x_mu_s - 1.0);
}

// 3D LUT 直查（density/multiple 纹理用；ν 方向两次采样插值模拟 4D）
vec3 GetScattering3D(sampler3D lut, float r, float mu, float mu_s, float nu,
                     bool rayIntersectsGround) {
    vec4 uvwz = GetScatteringTextureUvwzFromRMuMuSNu(r, mu, mu_s, nu, rayIntersectsGround);
    float texCoordX = uvwz.x * float(SCATTERING_TEXTURE_NU_SIZE - 1);
    float texX = floor(texCoordX);
    float lerp = texCoordX - texX;
    vec3 uvw0 = vec3((texX + uvwz.y) / float(SCATTERING_TEXTURE_NU_SIZE), uvwz.z, uvwz.w);
    vec3 uvw1 = vec3((texX + 1.0 + uvwz.y) / float(SCATTERING_TEXTURE_NU_SIZE), uvwz.z, uvwz.w);
    return texture(lut, uvw0).rgb * (1.0 - lerp) + texture(lut, uvw1).rgb * lerp;
}

// 严格 n-1 阶入射辐射度（Bruneton GetScattering(order) 语义，官方 order==1 → 单次、order>=2 → deltaMulti）：
//   order==1 → 单次散射（combined 除相函数存储 × 相函数还原）；order>=2 → 纯第 order 阶 deltaMulti（含相函数存储，直接用）
vec3 GetIncidentRadiance(sampler3D singleScatLUT, sampler3D deltaMultiLUT,
                         float r, float mu, float mu_s, float nu, bool rayIntersectsGround,
                         int order) {
    if (order == 1) {
        vec3 s, mie;
        GetCombinedScattering(singleScatLUT, r, mu, mu_s, nu, rayIntersectsGround, s, mie);
        return s * RayleighPhaseFunction(nu) + mie * MiePhaseFunction(MIE_PHASE_FUNCTION_G, nu);
    }
    return GetScattering3D(deltaMultiLUT, r, mu, mu_s, nu, rayIntersectsGround);
}

// ===== 第 n 阶散射密度（512 方向积分；输出含相函数） =====// 输入：transmittance + 单次散射 LUT（order=2 入射场）+ deltaMulti（order>=3 入射场）+ deltaIrr（地面反弹，纯上一阶）
vec3 ComputeScatteringDensity(sampler2D transLUT, sampler3D singleScatLUT,
                              sampler3D deltaMultiLUT, sampler2D deltaIrrLUT,
                              float r, float mu, float mu_s, float nu, int order) {
    vec3 zenith = vec3(0.0, 0.0, 1.0);
    vec3 omega = vec3(sqrt(max(1.0 - mu * mu, 0.0)), 0.0, mu);
    float sunDirX = (omega.x == 0.0) ? 0.0 : (nu - mu * mu_s) / omega.x;
    vec3 omegaS = vec3(sunDirX, sqrt(max(1.0 - sunDirX * sunDirX - mu_s * mu_s, 0.0)), mu_s);

    const int SAMPLE_COUNT = 16;
    float dphi = PI / float(SAMPLE_COUNT);
    float dtheta = PI / float(SAMPLE_COUNT);
    vec3 rayleighMie = vec3(0.0);

    for (int l = 0; l < SAMPLE_COUNT; ++l) {
        float theta = (float(l) + 0.5) * dtheta;
        float cosTheta = cos(theta);
        float sinTheta = sin(theta);
        bool rayIntersectsGround = RayIntersectsGround(r, cosTheta);

        float distanceToGround = 0.0;
        vec3 transmittanceToGround = vec3(0.0);
        float groundAlbedo = 0.0;
        if (rayIntersectsGround) {
            distanceToGround = DistanceToBottomAtmosphereBoundary(r, cosTheta);
            transmittanceToGround = GetTransmittance(transLUT, r, cosTheta, distanceToGround, true);
            groundAlbedo = GROUND_ALBEDO;
        }

        for (int m = 0; m < 2 * SAMPLE_COUNT; ++m) {
            float phi = (float(m) + 0.5) * dphi;
            vec3 omegaI = vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
            float dOmega = dtheta * dphi * sinTheta;   // rad=1

            // 入射辐射（严格 n-1 阶：order=2 用单次散射、order>=3 用纯上一阶 deltaMulti）
            float nu1 = dot(omegaS, omegaI);
            vec3 incident = GetIncidentRadiance(singleScatLUT, deltaMultiLUT,
                                                r, omegaI.z, mu_s, nu1,
                                                rayIntersectsGround, order);
            // 地面反弹路径（透射 × albedo × 1/π × 上一阶 deltaIrr 辐照度）
            vec3 groundNormal = normalize(zenith * r + omegaI * distanceToGround);
            vec3 groundIrradiance = GetIrradiance(deltaIrrLUT, BOTTOM_RADIUS, dot(groundNormal, omegaS));
            incident += transmittanceToGround * groundAlbedo * (1.0 / PI) * groundIrradiance;

            // 向 -omega 方向的散射
            float nu2 = dot(omega, omegaI);
            float rayleighDensity = GetRayleighDensity(r - BOTTOM_RADIUS);
            float mieDensity = GetMieDensity(r - BOTTOM_RADIUS);
            rayleighMie += incident * (
                RAYLEIGH_SCATTERING * rayleighDensity * RayleighPhaseFunction(nu2) +
                MIE_SCATTERING * mieDensity * MiePhaseFunction(MIE_PHASE_FUNCTION_G, nu2)) * dOmega;
        }
    }
    return rayleighMie;
}

// ===== 第 n 阶多重散射（50 步视线积分；输出含相函数） =====
vec3 ComputeMultipleScattering(sampler2D transLUT, sampler3D densityLUT,
                               float r, float mu, float mu_s, float nu,
                               bool rayIntersectsGround) {
    const int SAMPLE_COUNT = 50;
    float dx = DistanceToNearestAtmosphereBoundary(r, mu, rayIntersectsGround) / float(SAMPLE_COUNT);
    vec3 sum = vec3(0.0);
    for (int i = 0; i <= SAMPLE_COUNT; ++i) {
        float d_i = float(i) * dx;
        float r_i = clamp(sqrt(d_i * d_i + 2.0 * r * mu * d_i + r * r), BOTTOM_RADIUS, TOP_RADIUS);
        float mu_i = ClampCosine((r * mu + d_i) / r_i);
        float mu_s_i = ClampCosine((r * mu_s + d_i * nu) / r_i);
        vec3 density = GetScattering3D(densityLUT, r_i, mu_i, mu_s_i, nu, rayIntersectsGround);
        sum += density * GetTransmittance(transLUT, r, mu, d_i, rayIntersectsGround) * dx *
               ((i == 0 || i == SAMPLE_COUNT) ? 0.5 : 1.0);
    }
    return sum;
}

// ===== 地面间接辐照度（半球 2048 方向积分；输出含相函数散射的积分） =====
// 入射场同 density（严格 n-1 阶：order=2 单次、order>=3 纯上一阶 deltaMulti）；输出第 n 阶间接辐照度（纯）
vec3 ComputeIndirectIrradiance(sampler3D singleScatLUT, sampler3D deltaMultiLUT,
                               float r, float mu_s, int order) {
    const int SAMPLE_COUNT = 32;
    float dphi = PI / float(SAMPLE_COUNT);
    float dtheta = PI / float(SAMPLE_COUNT);
    vec3 result = vec3(0.0);
    vec3 omegaS = vec3(sqrt(max(1.0 - mu_s * mu_s, 0.0)), 0.0, mu_s);
    for (int j = 0; j < SAMPLE_COUNT / 2; ++j) {
        float theta = (float(j) + 0.5) * dtheta;
        for (int i = 0; i < 2 * SAMPLE_COUNT; ++i) {
            float phi = (float(i) + 0.5) * dphi;
            vec3 omega = vec3(cos(phi) * sin(theta), sin(phi) * sin(theta), cos(theta));
            float dOmega = dtheta * dphi * sin(theta);
            float nu = dot(omega, omegaS);
            result += GetIncidentRadiance(singleScatLUT, deltaMultiLUT,
                                          r, omega.z, mu_s, nu, false, order) *
                      omega.z * dOmega;
        }
    }
    return result;
}
