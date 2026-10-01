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
layout(binding = 4) uniform sampler2D materialTex;  // gbuffer2（x=metallic y=roughness z=ao w=emissive）
layout(binding = 5) uniform sampler2D skyRT;        // 全景天空（雾色）
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

// Transmittance LUT inverse mapping shared with fullscreen.frag. The sun disk
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

void main() {
    float centerDepth = texture(depthTex, fragTexCoord).r;
    vec3 lit = texture(lightTex, fragTexCoord).rgb;
    if (centerDepth < 0.999999) {
        float ao = texture(aoTex, fragTexCoord).r;
        float emissiveStrength = texture(materialTex, fragTexCoord).w;
        lit *= emissiveStrength>0.01 ? 1.0 : ao;

        // 深度直传 [0,1]（与 fullscreen.frag 一致）
        vec3 viewPos = P2V(vec3(fragTexCoord*2.0-1.0,centerDepth));
        vec3 worldPos = (cam.invView * vec4(viewPos, 1.0)).xyz;
        vec3 viewDir = normalize(worldPos - cam.cameraPos.xyz);

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
    // cloud_view 输出 canonical physical scattering + transmittance：
    //   rgb = 未曝光的 premultiplied 云散射光，a = 背景透射率（1 = 无云）
    // skyRT 已经带场景曝光，因此这里只在最终合成处给云 RGB 乘一次同样的曝光。
    vec4 cloud = centerDepth >= 0.999999
        ? SampleCloud(fragTexCoord) : vec4(0.0, 0.0, 0.0, 1.0);
    vec2 skyNdc = fragTexCoord * 2.0 - 1.0;
    vec3 skyDirCam = normalize((cam.invProj * vec4(skyNdc, 1.0, 1.0)).xyz);
    vec3 skyDir = normalize(mat3(cam.invView) * skyDirCam);
    float camAlt = max(cam.cameraPos.y + 200.0, 0.0);
    // Stable equivalent of sqrt(1 - (R/(R+h))^2): avoid subtracting
    // almost equal floats for ground-level cameras.
    float horizonY = -sqrt(camAlt * (2.0 * ATMO_BOTTOM_R + camAlt))
        / (ATMO_BOTTOM_R + camAlt);
    // Fade across both sides of the curved horizon, without a y>=0 branch.
    // Fade the entire premultiplied cloud layer toward the identity layer,
    // keeping scattering and background/sun transmittance consistent.
    float horizonCloudWeight = smoothstep(horizonY - 0.02, horizonY + 0.06, skyDir.y);
    cloud.rgb *= horizonCloudWeight;
    cloud.a = mix(1.0, cloud.a, horizonCloudWeight);
    float cloudTransmittance = clamp(cloud.a, 0.0, 1.0);

    // ===== 全分辨率太阳/月亮圆盘（云合成前） =====
    // fullscreen 合成只负责生成无圆盘的天空；圆盘在这里加入，随后与
    // cloud_view 的透射率一起合成，因而云可以真正遮挡圆盘，而不会走
    // 一条晚于云的独立绘制路径。
    if (centerDepth >= 0.999999) {
        vec3 sunDirection = normalize(pc.sunDir.xyz);
        float horizonMask = smoothstep(horizonY - SUN_R_HSPE, horizonY, skyDir.y);
        float minSunCosTheta = 1.0 - 0.5 * SUN_R_HSPE * SUN_R_HSPE;
        float cosTheta = dot(skyDir, sunDirection);

        if (cosTheta >= minSunCosTheta) {
            vec2 sunUV = TransLUTUv(ATMO_BOTTOM_R + camAlt,
                                    clamp(sunDirection.y, -1.0, 1.0));
            vec3 sunTrans = texture(transmittanceLUT, sunUV).rgb;
            // 日盘：物理太阳辐亮度 × 场景平行光(lightColor) × 场景曝光(pc.sunDir.w)——
            // 与 fullscreen.frag 直射光同源同调制（调平行光时盘同步变化）。
            vec3 sunDisk = (SOLAR_IRRADIANCE / PI) * sunTrans * horizonMask * pc.lightColor.rgb * pc.sunDir.w;
            lit += sunDisk;
        }
        if (cosTheta <= -minSunCosTheta) {
            vec2 moonUV = TransLUTUv(ATMO_BOTTOM_R + camAlt,
                                     clamp(-sunDirection.y, -1.0, 1.0));
            vec3 moonTrans = texture(transmittanceLUT, moonUV).rgb;
            // 月盘：原硬编码 ×10 恰与 kSceneExposure 同值，改绑 pc.sunDir.w 成为同一标尺（数值不变，净零）。
            vec3 moonDisk = vec3(0.2, 0.3, 0.6) * pc.sunDir.w * moonTrans * horizonMask;
            lit += moonDisk;
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

    // ===== 体积云合成/太阳盘遮挡 =====
    // Horizon presentation fade is applied once above to the whole cloud layer.
    // Cloud history retains physical scattering/transmittance; final sky and
    // sun occlusion share the same smoothly faded layer.
    float sceneExposure = max(pc.sunDir.w, 0.0);
    vec3 cloudScattering = max(cloud.rgb, vec3(0.0)) * sceneExposure;
    lit = lit * cloudTransmittance + cloudScattering;
    outColor = vec4(lit, 1.0);
}
