#version 450

#define PI 3.14159265359

precision highp float;

layout(location = 0) in vec2 fragTexCoord;

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PC {
    vec4 cameraPos;
    vec4 sunDir;
    vec4 lightColor;
    vec4 frameInfo;
} pc;

layout(binding = 0) uniform sampler2D aoTex;        // pass:gtao（R8 半分辨率）
layout(binding = 1) uniform sampler2D lightTex;     // composite（附件4 light，无 emissive/雾）
layout(binding = 2) uniform sampler2D depthTex;     // 全分辨率深度（边缘引导 + 雾距离）
layout(binding = 3) uniform sampler2D normalTex;    // 全分辨率法线（八面体，边缘引导）
layout(binding = 4) uniform sampler2D materialTex;  // gbuffer2（x=metallic y=roughness z=ao w=emissive）
layout(binding = 5) uniform sampler2D skyRT;        // 全景天空（雾色）
layout(binding = 6) uniform sampler2D albedoTex;    // gbuffer0（albedo，SSR PBR Fresnel 用）
layout(binding = 7) uniform sampler2D cloudTex;     // pass:cloud_view（半分辨率 view-space 云散射/透射）
layout(binding = 8) uniform sampler2D transmittanceLUT; // 太阳/月亮圆盘的物理透射率

// Camera UBO（binding 9；本 pass 比常规后处理多一个 LUT 输入）
layout(binding = 9) uniform CameraUBO {
    vec4 cameraPos;
    mat4 proj;
    mat4 view;
    mat4 prevViewProj;
    mat4 invProj;
    mat4 invView;
} cam;


float safeacos(float x) { return acos(clamp(x, -1.0, 1.0)); }
float pow2(float x) { return x * x; }
float pow4(float x) { float x2 = x * x; return x2 * x2; }
float pow8(float x) { float x2 = x * x; float x4 = x2 * x2; return x4 * x4; }
float linestep(float a, float b, float x) { return clamp((x - a) / (b - a), 0.0, 1.0); }

// Karis split-sum BRDF 解析近似（与合成 pass fullscreen.frag 同函数，保证 SSR/IBL 反射强度一致）
vec3 EnvBRDFApprox(vec3 f0, float roughness, float NoV) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
    vec2 AB = vec2(-1.04, 1.04) * a004 + r.zw;
    return f0 * AB.x + AB.y;
}

vec2 SignNotZero(vec2 v) {
    return vec2(v.x < 0.0 ? -1.0 : 1.0,
                v.y < 0.0 ? -1.0 : 1.0);
}

vec3 OctahedronDecode(vec2 oct) {
    vec3 n = vec3(oct, 1.0 - abs(oct.x) - abs(oct.y));
    if (n.z < 0.0) {
        n.xy = (1.0 - abs(n.yx)) * SignNotZero(n.xy);
    }
    return normalize(n);
}

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
    return max(vRGB, vec3(0.0));
}

vec2 skylutuv(vec3 rayDir, float camAltMeters) {
    const float CAM_ALT = max(camAltMeters, 0.0) / 1000.0;
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
    float expo = mix(4.0, 1.0, clamp(camAltMeters / 60000.0, 0.0, 1.0));
    x = pow(x, 1.0 / expo);
    uv.y = mix(x * (1.0 - t) + t, -x * t + t, float(uv.y < t));
    const float UV_INSET = 0.002;
    uv = uv * (1.0 - 2.0 * UV_INSET) + UV_INSET;
    return uv;
}

// 与 fullscreen.frag 共用的官方透射率 LUT 逆映射。圆盘在本 pass
// 生成，必须继续使用与天空/云光照相同的 6371 km 地球半径。
const float SUN_R_HSPE = 0.012;
const float ATMO_BOTTOM_R = 6371000.0;
const float ATMO_TOP_R = 6431000.0;
const vec3 SOLAR_IRRADIANCE = vec3(1.474, 1.8504, 2.3612);

float TransLUTDistanceToTop(float r, float mu)
{
    float disc = r * r * (mu * mu - 1.0) + ATMO_TOP_R * ATMO_TOP_R;
    return max(-r * mu + sqrt(max(disc, 0.0)), 0.0);
}

vec2 TransLUTUv(float r, float mu)
{
    float H = sqrt(ATMO_TOP_R * ATMO_TOP_R - ATMO_BOTTOM_R * ATMO_BOTTOM_R);
    float rho = sqrt(max(r * r - ATMO_BOTTOM_R * ATMO_BOTTOM_R, 0.0));
    float d = TransLUTDistanceToTop(r, mu);
    float dMin = ATMO_TOP_R - r;
    float dMax = rho + H;
    float xMu = (d - dMin) / (dMax - dMin);
    float xR = rho / H;
    float u = 0.5 / 256.0 + xMu * (1.0 - 1.0 / 256.0);
    float v = 0.5 / 64.0 + xR * (1.0 - 1.0 / 64.0);
    return vec2(u, v);
}

// ===== SSR（composite_3.fsh 屏幕空间步进 + mikan [0,1] 深度直比）=====

float interleaved_gradientNoise() {
    vec2 coord = gl_FragCoord.xy;
    return fract(52.9829189 * fract(0.06711056 * coord.x + 0.00583715 * coord.y));
}

// V2P：view space → NDC（参考 composite_3.fsh，照抄）
// 用 proj 对角线 + 第三列平移（标准透视，w=-z）
vec3 V2P(vec3 p1) {
    return (vec3(cam.proj[0][0], cam.proj[1][1], cam.proj[2][2]) * p1 + cam.proj[3].xyz) / -p1.z;
}
vec3 P2V(vec3 p0) {
    vec4 p1 = vec4(cam.invProj[0].x, cam.invProj[1].y, cam.invProj[2].zw) * p0.xyzz + cam.invProj[3];
    return p1.xyz / p1.w;
}

// 云是半分辨率 prepass。这里只做线性升采样，不再额外进行十字模糊；
// 云的降噪交给 cloud_view 的时域历史，避免云缘被连续两次抹平。
vec4 SampleCloud(vec2 uv)
{
    return texture(cloudTex, uv);
}

float CloudAtmosphericVisualFade(vec3 skyDirection)
{
    // Use the same spherical coordinates as cloud_view.  The LUT value is the
    // air visibility along the sky ray; it gives distant horizon clouds a
    // continuous aerial-perspective fade instead of an arbitrary y cutoff.
    vec3 cameraHspe = vec3(cam.cameraPos.x * 0.0010000000474974513,
                           6371.0 + (cam.cameraPos.y * 0.0010000000474974513),
                           cam.cameraPos.z * 0.0010000000474974513);
    float cameraRadius = clamp(length(cameraHspe) * 1000.0,
                               ATMO_BOTTOM_R, ATMO_TOP_R);
    vec3 cameraUp = normalize(cameraHspe);
    float skyMu = dot(cameraUp, skyDirection);
    if (skyMu <= 0.0)
    {
        return 0.0;
    }
    vec3 skyTransmittance = texture(
        transmittanceLUT,
        TransLUTUv(cameraRadius, clamp(skyMu, -1.0, 1.0))).rgb;
    float airVisibility = dot(skyTransmittance,
                              vec3(0.2125999927520751953125,
                                   0.715200006961822509765625,
                                   0.072200000286102294921875));
    return smoothstep(0.01500000059604644775390625,
                      0.3499999940395355224609375,
                      airVisibility);
}


// SSR_V2P：view space → screen [0,1]（参考原版：V2P * 0.5 + 0.5）
vec3 SSR_V2P(vec3 p) {
    return vec3(V2P(p).xy * 0.5 + 0.5,V2P(p).z);
}

// Do_Raytracing_2DSP（参考 composite_3.fsh，完全照抄原版逻辑）
// hitDist：命中点距离射线起点的屏幕 UV 距离（用于反射置信度边缘过渡）
void DoSSR(out vec2 hit_coord, out float hitDist, out bool hit_flag,
    vec3 startPoint, vec3 rayDirection, sampler2D depthSampler, float dither, float ssrSteps) {
    const float sqrt3 = 1.73205080757;
    const float near = 0.1, far = 500.0;

    hitDist = 1.0;   // 未命中时给 1（置信度 0）

    // 剔除背面射线：反射方向朝相机后方（视空间 +z）不投影到场景，丢弃
    if (rayDirection.z > 0.0) { hit_flag = false; return; }

    vec3 projPos = SSR_V2P(startPoint);
    float raylen = (startPoint.z + rayDirection.z * far * sqrt3 > -near)
        ? -(near + startPoint.z) / rayDirection.z : far * sqrt3;

    vec3 projDir = SSR_V2P(startPoint + rayDirection * raylen) - projPos;
    projDir = normalize(projDir);

    vec3 maxlen = (step(0.0, projDir) - projPos) / projDir;
    // AABB
    float steplen = min(min(maxlen.x, maxlen.y), maxlen.z) * (1.0 / ssrSteps);
    vec3 tracstep = steplen * projDir;

    vec3 testpoint = projPos + tracstep * dither;
    vec2 depthrange = vec2(projPos.z, testpoint.z + tracstep.z * 0.5);
    float depth_const0 = -2e-5 / near;
    float depth_const1 = (near - far) * depth_const0;
    float depth_const2 = (far + near) * depth_const0;

    float sampledepth = 1.0;
    bool ishit = false;
    for (float i = 0.0; i < ssrSteps; i++) {
        sampledepth = texture(depthSampler, testpoint.xy).x;
        ishit = sampledepth >= min(depthrange.x, depthrange.y)
             && sampledepth <= max(depthrange.x, depthrange.y);
        if (ishit || clamp(testpoint.xy, -0.05, 1.05) != testpoint.xy) break;
        testpoint += tracstep;
        depthrange = depthrange.yy + vec2(testpoint.z * depth_const1 + depth_const2, tracstep.z);
    }
    hit_flag = ishit && testpoint.z > 0.0 && sampledepth < 0.9999;
    hit_coord = testpoint.xy;
    hitDist = length(testpoint.xy - projPos.xy);   // 命中点到起点的屏幕距离
}

void main() {
    float centerDepth = texture(depthTex, fragTexCoord).r;
    vec3 lit = texture(lightTex, fragTexCoord).rgb;
    if (centerDepth < 0.999999) {
        float ao = texture(aoTex, fragTexCoord).r;
        float emissiveStrength = texture(materialTex, fragTexCoord).w;
        lit *= emissiveStrength>0.01 ? 1.0 : ao;

        // ===== SSGI 间接光（半分辨率 + 升采样）：环境反射光叠加 =====
        // 间接光 = ssgi 输出 × 表面 albedo（反射介质是 albedo）——低频光，弱受 AO
        vec3 albedo = texture(albedoTex, fragTexCoord).rgb;

        // 深度直传 [0,1]（与 fullscreen.frag 一致）
        vec3 viewPos = P2V(vec3(fragTexCoord*2.0-1.0,centerDepth));
        vec3 worldPos = (cam.invView * vec4(viewPos, 1.0)).xyz;
        vec3 viewDir = normalize(worldPos - cam.cameraPos.xyz);
        vec3 normal = OctahedronDecode(texture(normalTex, fragTexCoord).xy);

        // ===== 2D SSR（轻量后处理：SSR 反射 = 已光照 composite × split-sum BRDF 权重）=====
        // ⚠️ 蒙版测试：仅金属像素计算 SSR（非金属无镜面反射，省全屏开销）
        float metallicMask = texture(materialTex, fragTexCoord).x;
        if (metallicMask > 0.05) {
            vec3 V = normalize(viewPos);
            vec3 N = normalize(mat3(cam.view) * normal);
            vec3 R = reflect(V, N);

            // PBR 材质
            vec4 mat = texture(materialTex, fragTexCoord);
            float metallic = mat.x;
            float roughness = clamp(mat.y, 0.04, 1.0);
            float matAO = mat.z;

            float dither = interleaved_gradientNoise()*0.5+0.5;
            vec2 hitUV; float hitDist; bool isssr;
            DoSSR(hitUV, hitDist, isssr, viewPos, R, depthTex, dither, 32.0);
            if (isssr) {
                // 反射源 = 已光照 composite —— 反射有光照的物体
                vec3 reflColor = texture(lightTex, clamp(hitUV, 0.001, 0.999)).rgb;

                // 屏幕边缘淡出（柔和：边缘/落点越靠近屏幕边界，反射越弱）
                vec2 edge = clamp(hitUV, 0.0, 1.0) * 2.0 - 1.0;
                float edgeFade = 1.0 - pow(max(abs(edge.x), abs(edge.y)), 4.0);

                // 落点置信度：射线走越远，可信度越低 → 反射渐隐（远处边缘过渡柔和）
                float distFade = 1.0 - smoothstep(0.05, 0.5, hitDist);

                // split-sum BRDF 权重（Karis 解析 EnvBRDF，与合成 pass IBL 同函数同物理衰减）
                // 反射能量 = FssEss = f0*A+B（roughness 相关），保证 SSR 与 IBL 强度一致
                float NoV = clamp(dot(N, V), 0.0, 1.0);
                vec3 F0 = mix(vec3(0.04), albedo, metallic);
                vec3 brdf = EnvBRDFApprox(F0, roughness, NoV);   // split-sum 反射系数

                float reflStrength = clamp(max(max(brdf.r, brdf.g), brdf.b), 0.0, 1.0);
                float reflectIntensity = reflStrength * edgeFade * distFade;
                lit = mix(lit, reflColor, reflectIntensity);
            }
        }

        // ===== 雾 =====
        vec3 dirH = normalize(vec3(viewDir.x, 0.0, viewDir.z));
        if (length(dirH) < 0.001) dirH = vec3(0.0, 0.0, 1.0);
        vec3 fogColor = colors_LogLuv32ToSRGB(texture(skyRT, skylutuv(dirH, max(cam.cameraPos.y + 200.0, 0.0))));
        float dist = length(viewDir);
        float fogAmount = 1.0 - exp(-dist * 0.0005);
        lit = mix(lit, fogColor, clamp(fogAmount, 0.0, 1.0));

   
        //lit=ssgi;
    }

    // ===== 云透射率（所有天空光照项共用） =====
    // cloud_view 输出 premultiplied scattering + transmittance：
    //   rgb = 云散射光，a = 背景透射率（1 = 无云）
    // 先读取它，再把太阳盘、Godray 和天空背景统一放到同一个透射层后面。
    vec4 cloud = SampleCloud(fragTexCoord);
    float cloudTransmittance = clamp(cloud.a, 0.0, 1.0);

    // ===== 全分辨率太阳/月亮圆盘（云合成前） =====
    // fullscreen 合成只负责生成无圆盘的天空；圆盘在这里加入，随后与
    // cloud_view 的透射率一起合成，因而云可以真正遮挡圆盘，而不会走
    // 一条晚于云的独立绘制路径。
    float cloudVisualFade = 1.0;
    vec3 sunMoonDisk = vec3(0.0);
    if (centerDepth >= 0.999999) {
        vec2 skyNdc = fragTexCoord * 2.0 - 1.0;
        vec3 skyDirCam = normalize((cam.invProj * vec4(skyNdc, 1.0, 1.0)).xyz);
        vec3 skyDir = normalize(mat3(cam.invView) * skyDirCam);
        // 这是远景云的视觉/大气透视淡出，不参与 cloud.a。
        // cloud.a 必须保持真实透射率，供太阳/月亮圆盘遮挡使用。
        cloudVisualFade = CloudAtmosphericVisualFade(skyDir);
        vec3 sunDirection = normalize(pc.sunDir.xyz);
        float camAlt = max(cam.cameraPos.y + 200.0, 0.0);
        float horizonY = -sqrt(max(1.0 - pow2(ATMO_BOTTOM_R / (ATMO_BOTTOM_R + camAlt)), 0.0));
        float horizonMask = smoothstep(horizonY - SUN_R_HSPE, horizonY, skyDir.y);
        float minSunCosTheta = 1.0 - 0.5 * SUN_R_HSPE * SUN_R_HSPE;
        float cosTheta = dot(skyDir, sunDirection);

        if (cosTheta >= minSunCosTheta) {
            vec2 sunUV = TransLUTUv(ATMO_BOTTOM_R + camAlt,
                                    clamp(sunDirection.y, -1.0, 1.0));
            vec3 sunTrans = texture(transmittanceLUT, sunUV).rgb;
            vec3 sunDisk = (SOLAR_IRRADIANCE / PI) * sunTrans * horizonMask * 6.0;
            lit += sunDisk;
            sunMoonDisk += sunDisk;
        }
        if (cosTheta <= -minSunCosTheta) {
            vec2 moonUV = TransLUTUv(ATMO_BOTTOM_R + camAlt,
                                     clamp(-sunDirection.y, -1.0, 1.0));
            vec3 moonTrans = texture(transmittanceLUT, moonUV).rgb;
            vec3 moonDisk = vec3(0.2, 0.3, 0.6) * 10.0 * moonTrans * horizonMask;
            lit += moonDisk;
            sunMoonDisk += moonDisk;
        }
    }

    // ===== 体积光 + Godray（半分辨率升采样 + 参与雾）=====
    // aoTex 现为 RGBA8：.r=AO, .g=体积光散射, .b=Godray 光束（升采样由纹理过滤完成）
    vec3 volTex = texture(aoTex, fragTexCoord).rgb;
    float volScatter = volTex.g;
    float godray = volTex.b;
    vec3 sunColor = colors_LogLuv32ToSRGB(texture(skyRT, skylutuv(normalize(pc.sunDir.xyz), max(cam.cameraPos.y + 200.0, 0.0))));
    //lit += sunColor * (volScatter * 0.3);   // 光柱/受光体积加亮
    //lit += sunColor * (godray * 0.3);        // Godray 光束（天空/地面同太阳色）

    // ===== 体积云合成（必须是所有太阳方向附加光的最后一道天空遮挡） =====
    // 云 prepass 已用 sceneDepth 截断到不透明几何体，因此这里对全屏应用不会覆盖场景物体。
    // 对几何像素 cloudTransmittance=1、cloud.rgb=0，不改变原有物体光照。
    // 普通天空使用远景视觉透视淡出，但太阳/月亮圆盘必须使用原始云透射率，
    // 不能因为视觉淡出而重新穿透云层。
    vec3 litWithoutDisk = lit - sunMoonDisk;
    float visualCloudTransmittance = mix(
        1.0, cloudTransmittance, cloudVisualFade);
    lit = litWithoutDisk * visualCloudTransmittance
        + sunMoonDisk * cloudTransmittance
        + cloud.rgb * cloudVisualFade;
    outColor = vec4(lit, 1.0);
}
