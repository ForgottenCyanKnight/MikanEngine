#version 450

// ===== 统一屏幕空间反射与水面合成 pass =====
// 链位置：gtao_apply 之后、taa/bloom 之前（pass:before 自动前溯）。
// 输入 litTex = 已光照场景；causticsTex = 以世界 XZ 为坐标的动画焦散烘焙图。
// 普通金属表面 SSR 与水面 SSR 在此统一处理。

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

layout(binding = 0) uniform sampler2D litTex;    // pass:gtao_apply（已光照场景色）
layout(binding = 1) uniform sampler2D depthTex;  // 全分辨率深度（水底视距 + SSR 射线步进）
layout(binding = 2) uniform sampler2D skyRT;     // 全景天空（水体雾色；反射改用 skyCube IBL）
layout(binding = 3) uniform sampler2D waterTex;  // WaterTargetRT（R=mask, G=水面线性视距(m), BA=八面体世界法线；Nearest）
layout(binding = 4) uniform samplerCube skyCube; // IBL 天空立方体（已在生成阶段合入体积云）
layout(binding = 5) uniform samplerCube sceneProbe; // 场景反射探针（cubemap；帧尾捕获，读到上一帧；A=场景覆盖掩码）
layout(binding = 6) uniform sampler2D normalTex;  // gbuffer1（八面体世界法线）
layout(binding = 7) uniform sampler2D materialTex; // gbuffer2（metallic/roughness/AO/emissive）
layout(binding = 8) uniform sampler2D albedoTex;  // gbuffer0（线性材质反照率）
layout(binding = 9) uniform sampler2D causticsTex; // world-space 0.5m/4m 焦散图（R/G）

// Camera UBO（binding 10；本 pass 的最高输入位于 slot 9）
layout(binding = 10) uniform CameraUBO {
    vec4 cameraPos;
    mat4 proj;
    mat4 view;
    mat4 prevViewProj;
    mat4 invProj;
    mat4 invView;
} cam;


float safeacos(float x) { return acos(clamp(x, -1.0, 1.0)); }
float pow2(float x) { return x * x; }
float linestep(float a, float b, float x) { return clamp((x - a) / (b - a), 0.0, 1.0); }

const float REFLECTION_PROBE_MAX_DISTANCE = 100.0;

vec3 EnvBRDFApprox(vec3 f0, float roughness, float NoV) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
    vec2 AB = vec2(-1.04, 1.04) * a004 + r.zw;
    return f0 * AB.x + AB.y;
}

vec3 SampleReflectionFallback(vec3 worldPos, vec3 reflectionDir, float roughness) {
    vec3 direction = normalize(reflectionDir);
    vec3 ibl = textureLod(skyCube, direction, clamp(roughness, 0.0, 1.0) * 7.0).rgb;
    float probeWeight = 0.0;
    vec3 probeColor = ibl;
    if (distance(worldPos, cam.cameraPos.xyz) <= REFLECTION_PROBE_MAX_DISTANCE) {
        vec4 probeSample = texture(sceneProbe, direction);
        probeColor = probeSample.rgb;
        probeWeight = clamp(probeSample.a, 0.0, 1.0);
    }
    return mix(ibl, probeColor, probeWeight);
}

// ===== 硬编码水材质（常见清水 PBR 值）=====
const float WATER_F0          = 0.02;                      // ((1-1.33)/(1+1.33))^2
const float WATER_IOR          = 1.333;
const float WATER_ROUGHNESS   = 0.08;
const vec3  WATER_ABSORPTION  = vec3(0.55, 0.13, 0.08);    // 1/m，Beer-Lambert（红光衰减最快→透射偏蓝）
const vec3  WATER_BODY_TINT   = vec3(0.18, 0.50, 0.62);    // 清水水体色（淡蓝青）
const float WATER_MIN_DIST    = 1.2;                       // 视深下限(m)：近岸浅水(cm 级路径长)若不保底，水体色≈0 看起来像裸地形"消失"
const float WATER_SHORE_TOL   = 0.25;                      // 岸线容差(m)：水面比场景深度落后 ≤此值仍合成（浅滩带地形/草略高于水面）
const float WATER_FRESNEL_BOOST = 2.5;                     // 水面 Fresnel 增益（游戏作弊项）：陡视角物理值 ~0.03-0.06 会让中景
                                                           // 倒影（树/岸）几乎不可见；增益后近岸掠射行为不变，仅中景增强
const float WATER_FRESNEL_MAX = 0.7;                       // 增益后的混合权重上限
const float WATER_BODY_FADE   = 0.18;                      // 水体雾色浓度 1-exp(-k·d) 的 k（0.5m→~0.09，2m→~0.30，4m→~0.51）

// GGX specular D（Smith visibility 与 Fresnel 在调用处组合）
float D_GGX(float NoH, float a) {
    float a2 = a * a;
    float d = NoH * NoH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
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

float interleaved_gradientNoise() {
    vec2 coord = gl_FragCoord.xy;
    return fract(52.9829189 * fract(0.06711056 * coord.x + 0.00583715 * coord.y));
}

// View-space to NDC projection.
// 用 proj 对角线 + 第三列平移（标准透视，w=-z）
vec3 V2P(vec3 p1) {
    return (vec3(cam.proj[0][0], cam.proj[1][1], cam.proj[2][2]) * p1 + cam.proj[3].xyz) / -p1.z;
}
vec3 P2V(vec3 p0) {
    vec4 p1 = vec4(cam.invProj[0].x, cam.invProj[1].y, cam.invProj[2].zw) * p0.xyzz + cam.invProj[3];
    return p1.xyz / p1.w;
}

// Project a Snell-refracted ray back into the current color buffer. At grazing
// angles its endpoint can travel far beyond the viewport; hard rejection or
// clamping then creates a visible cutoff at the screen edge. Keep the target
// bounded and smoothly fade the screen-space warp near borders and for very
// large displacements. confidence also lets callers blend to an environment
// fallback when the refracted ray leaves the current view.
vec2 ProjectRefractionUV(vec2 sourceUV, vec3 surfaceView, vec3 refractedView,
                         float travelDistance, out float confidence) {
    confidence = 0.0;
    if (refractedView.z >= -1e-4) return sourceUV;

    vec3 endpointView = surfaceView + normalize(refractedView) * max(travelDistance, 0.0);
    if (endpointView.z >= -1e-4) return sourceUV;
    vec2 rawUV = V2P(endpointView).xy * 0.5 + 0.5;
    if (any(isnan(rawUV)) || any(isinf(rawUV))) return sourceUV;

    vec2 safeUV = clamp(rawUV, vec2(0.002), vec2(0.998));
    vec2 resolution = max(vec2(textureSize(litTex, 0)), vec2(1.0));
    float shiftPixels = length((rawUV - sourceUV) * resolution);
    float maxShiftPixels = max(32.0, min(resolution.x, resolution.y) * 0.08);
    float shiftFade = 1.0 - smoothstep(maxShiftPixels, maxShiftPixels * 1.5, shiftPixels);

    float borderDistance = min(min(safeUV.x, safeUV.y),
                               min(1.0 - safeUV.x, 1.0 - safeUV.y));
    float borderFade = smoothstep(0.0, 0.035, borderDistance);
    float outsideDistance = max(max(max(-rawUV.x, rawUV.x - 1.0),
                                    max(-rawUV.y, rawUV.y - 1.0)), 0.0);
    float outsideFade = 1.0 - smoothstep(0.0, 0.04, outsideDistance);
    confidence = clamp(shiftFade * borderFade * outsideFade, 0.0, 1.0);
    return safeUV;
}

vec2 CausticMapUV(vec2 worldXZ, out float inBounds) {
    vec2 mapSize = vec2(textureSize(causticsTex, 0));
    vec2 mapSpan = vec2(384.0, 384.0 * mapSize.y / max(mapSize.x, 1.0));
    // Must match water_caustics.frag: stable in a world tile, with only
    // whole-map shifts when the camera crosses a tile boundary.
    vec2 mapCenter = floor(cam.cameraPos.xz / mapSpan + 0.5) * mapSpan;
    vec2 uv = (worldXZ - mapCenter) / mapSpan + 0.5;
    inBounds = step(0.0, uv.x) * step(0.0, uv.y) *
               step(uv.x, 1.0) * step(uv.y, 1.0);
    return uv;
}

float SampleCausticDepth(vec4 depthSlices, float waterDepth) {
    if (waterDepth <= 0.5) {
        return depthSlices.r * clamp(waterDepth / 0.5, 0.0, 1.0);
    }
    if (waterDepth <= 1.5) {
        return mix(depthSlices.r, depthSlices.g, waterDepth - 0.5);
    }
    if (waterDepth <= 2.5) {
        return mix(depthSlices.g, depthSlices.b, waterDepth - 1.5);
    }
    if (waterDepth <= 4.0) {
        return mix(depthSlices.b, depthSlices.a, (waterDepth - 2.5) / 1.5);
    }
    return depthSlices.a * exp(-0.22 * (waterDepth - 4.0));
}

// View-space to screen-space projection.
vec3 SSR_V2P(vec3 p) {
    return vec3(V2P(p).xy * 0.5 + 0.5,V2P(p).z);
}

// Screen-space reflection ray march.
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

vec3 EvaluateWaterFog() {
    vec3 L = normalize(pc.sunDir.xyz);
    vec3 sunLight = max(pc.lightColor.rgb, vec3(0.0));
    vec3 skyAmb = colors_LogLuv32ToSRGB(
        texture(skyRT, skylutuv(normalize(vec3(0.2, 1.0, 0.1)), max(cam.cameraPos.y, 0.0))));
    float NoL_water = max(L.y, 0.0);
    vec3 waterFog = WATER_BODY_TINT * (skyAmb + sunLight * pc.sunDir.w * NoL_water * 0.08) * 0.7;
    return max(waterFog, vec3(0.03, 0.08, 0.10) * pc.sunDir.w);
}

void ApplyUnderwaterFogAndCaustics(inout vec3 color, vec2 uv, float sceneDepth,
                                   bool terrainWater) {
    bool hasGeometry = sceneDepth < 0.999999;
    float viewDistance = hasGeometry
        ? length(P2V(vec3(uv * 2.0 - 1.0, sceneDepth)))
        : max(24.0, abs(pc.cameraPos.w) + 24.0);

    // The caustics atlas is world-XZ locked and only valid for painted terrain
    // water. Reconstruct receiver depth from the camera's submerged depth;
    // generic WaterComponent volumes deliberately get fog but no terrain bake.
    if (terrainWater && hasGeometry) {
        vec3 receiverView = P2V(vec3(uv * 2.0 - 1.0, sceneDepth));
        vec3 receiverWorld = (cam.invView * vec4(receiverView, 1.0)).xyz;
        float receiverWaterDepth = cam.cameraPos.y - pc.cameraPos.w - receiverWorld.y;
        if (receiverWaterDepth > 0.02 && receiverWaterDepth <= 8.0) {
            float inBounds = 0.0;
            vec2 causticUV = CausticMapUV(receiverWorld.xz, inBounds);
            if (inBounds > 0.5) {
                vec4 depthSlices = texture(causticsTex, causticUV);
                float focus = max(SampleCausticDepth(depthSlices, receiverWaterDepth), 0.0);
                vec3 receiverNormal = OctahedronDecode(texture(normalTex, uv).xy);
                vec3 receiverAlbedo = texture(albedoTex, uv).rgb;
                float metallic = clamp(texture(materialTex, uv).x, 0.0, 1.0);
                vec3 L = normalize(pc.sunDir.xyz);
                float NoL = max(dot(receiverNormal, L), 0.0);
                vec3 causticRadiance = receiverAlbedo * ((1.0 - metallic) / PI) *
                    max(pc.lightColor.rgb, vec3(0.0)) *
                    (pc.sunDir.w * NoL * focus);
                color += causticRadiance * exp(-WATER_ABSORPTION *
                    max(viewDistance, WATER_MIN_DIST));
            }
        }
    }

    float fogAmount = 1.0 - exp(-WATER_BODY_FADE * max(viewDistance, 0.0));
    vec3 transmittance = exp(-WATER_ABSORPTION * max(viewDistance, 0.0));
    color = color * transmittance + EvaluateWaterFog() * fogAmount;
}

vec3 ShadeUnderwaterSurface(vec2 uv, vec4 waterSample, float surfaceDist,
                            vec3 backgroundColor) {
    vec3 surfaceView = normalize(P2V(vec3(uv * 2.0 - 1.0, 0.5))) * surfaceDist;
    vec3 surfaceWorld = (cam.invView * vec4(surfaceView, 1.0)).xyz;
    vec3 incidentWorld = normalize(surfaceWorld - cam.cameraPos.xyz);
    vec3 N = OctahedronDecode(waterSample.ba);
    float incidentNoN = dot(incidentWorld, N);
    vec3 faceN = incidentNoN < 0.0 ? N : -N;
    float eta = incidentNoN < 0.0 ? (1.0 / WATER_IOR) : WATER_IOR;
    vec3 refractedWorld = refract(incidentWorld, faceN, eta);
    bool totalInternalReflection = dot(refractedWorld, refractedWorld) < 1e-6;

    vec3 transmitted = backgroundColor;
    if (!totalInternalReflection) {
        vec3 refractedView = mat3(cam.view) * normalize(refractedWorld);
        float screenWeight = 0.0;
        vec2 refractedUV = ProjectRefractionUV(uv, surfaceView, refractedView,
            max(surfaceDist + 16.0, 24.0), screenWeight);
        vec3 screenTransmission = texture(litTex, refractedUV).rgb;
        vec3 environmentTransmission = textureLod(skyCube, normalize(refractedWorld), 0.0).rgb;
        transmitted = mix(environmentTransmission, screenTransmission, screenWeight);
    }

    float NoV = clamp(abs(dot(N, normalize(cam.cameraPos.xyz - surfaceWorld))), 0.0, 1.0);
    float fresnel = WATER_F0 + (1.0 - WATER_F0) * pow(1.0 - NoV, 5.0);
    vec3 reflectedWorld = normalize(reflect(incidentWorld, faceN));
    vec3 reflection = SampleReflectionFallback(surfaceWorld, reflectedWorld, WATER_ROUGHNESS);
    vec3 reflectedView = mat3(cam.view) * reflectedWorld;
    vec2 hitUV;
    float hitDist;
    bool hitSSR;
    DoSSR(hitUV, hitDist, hitSSR, surfaceView, reflectedView, depthTex,
          interleaved_gradientNoise() * 0.5 + 0.5, 48.0);
    if (hitSSR) {
        vec3 ssrColor = texture(litTex, clamp(hitUV, 0.001, 0.999)).rgb;
        vec2 edge = clamp(hitUV, 0.0, 1.0) * 2.0 - 1.0;
        float edgeFade = 1.0 - pow(max(abs(edge.x), abs(edge.y)), 4.0);
        float distFade = 1.0 - smoothstep(0.2, 1.5, hitDist);
        reflection = mix(reflection, ssrColor, clamp(edgeFade * distFade, 0.0, 1.0));
    }

    float reflectionWeight = totalInternalReflection ? 1.0 : fresnel;
    vec3 surfaceColor = mix(transmitted, reflection, reflectionWeight);
    float pathLength = max(surfaceDist, 0.0);
    return surfaceColor * exp(-WATER_ABSORPTION * pathLength) +
           EvaluateWaterFog() * (1.0 - exp(-WATER_BODY_FADE * pathLength));
}

void main() {
    vec3 lit = texture(litTex, fragTexCoord).rgb;
    float centerDepth = texture(depthTex, fragTexCoord).r;

    // ===== 水面合成（deferred water compositing 第二阶段）=====
    // waterTex: R=mask, G=水面线性视距(m), BA=八面体世界法线（Nearest）。
    // 覆盖判定 = mask ∧ surfaceDist < bottomDist：岸边越界/被遮挡水面直通场景色。
    // 深度重建注意：只有 XY 做 *2-1（Vulkan NDC XY ∈ [-1,1]），主深度 z 保持
    // [0,1] 原值进 P2V；水面视距不经过 NDC。同一像元射线上的点共线于原点，
    // 射线方向 = normalize(P2V(任意 z))，视距差 = 水下路径长（精确，无近似）。
    // 折射 = 背景色 × Beer-Lambert 吸收 + 淡蓝 in-scatter；反射 = Fresnel(0.02) × 天空 + GGX 太阳高光。
    vec4 w = texture(waterTex, fragTexCoord);
    bool hasOpaqueDepth = centerDepth < 0.999999;
    float surfaceDist = w.g;
    float bottomDist = hasOpaqueDepth
        ? length(P2V(vec3(fragTexCoord * 2.0 - 1.0, centerDepth))) : 1e30;
    bool visibleWaterSurface = w.r > 0.5 &&
        (!hasOpaqueDepth || surfaceDist - bottomDist < WATER_SHORE_TOL);
    bool cameraUnderwater = abs(pc.cameraPos.w) > 0.001;

    if (cameraUnderwater && visibleWaterSurface) {
        lit = ShadeUnderwaterSurface(fragTexCoord, w, surfaceDist, lit);
    } else if (cameraUnderwater) {
        ApplyUnderwaterFogAndCaustics(lit, fragTexCoord, centerDepth,
                                     pc.cameraPos.w < -0.001);
    } else if (visibleWaterSurface) {
        // No opaque object behind the surface: still shade the interface against
        // the sky rather than dropping Fresnel/refraction for the whole pixel.
        if (!hasOpaqueDepth) bottomDist = surfaceDist + 8.0;
        vec2 ndcXY = fragTexCoord * 2.0 - 1.0;
        vec3 rayDir = normalize(P2V(vec3(ndcXY, 0.5)));
        // 覆盖判定：水面在场景几何之前 → 合成；水面只落后 ≤WATER_SHORE_TOL
        // （岸线浅滩：地形/草比水面高几厘米）→ 仍合成，路径长由
        // max(·, WATER_MIN_DIST) 地板接管呈现浅水雾；落后更多（真正的
        // 沙滩/岸上几何）→ 直通场景色。
        if (surfaceDist - bottomDist < WATER_SHORE_TOL) {
            vec3 surfaceView = rayDir * surfaceDist;
            vec3 bottomView = rayDir * bottomDist;

            vec3 worldPos = (cam.invView * vec4(surfaceView, 1.0)).xyz;
            vec3 V = normalize(cam.cameraPos.xyz - worldPos);
            vec3 N = OctahedronDecode(w.ba);   // 世界空间（水面朝上）

            // --- 折射项：Beer-Lambert 吸收 + 水雾/水面基色 ---
            // 视深下限：涂刷水深普遍 <0.5m，真实路径长下水色几乎不可见。
            // 水体表达 = mix(透射水底, 受光雾色, bodyFade)：浅水看得见底，
            // 随视深逼近不透明的淡蓝"水雾"。雾色亮度 = 天空环境 + 太阳直射
            //（水面朝上 NoL），并带可见量级的保底——坑底常年在阴影里，
            // HDR 场景色 ≈1，in-scatter 小于 0.1 在 tonemap 后完全不可见
            //（高光是几十的 HDR 值所以能看见，基色必须给到同量级）。
            // 真实水深（带符号）：岸线容差带内 surfaceDist 可能 > bottomDist → 记 0。
            float underwaterDist = max(length(bottomView - surfaceView), WATER_MIN_DIST);
            vec3 transmittance = exp(-WATER_ABSORPTION * underwaterDist);
            float bodyFade = 1.0 - exp(-WATER_BODY_FADE * underwaterDist);

            // --- 水底折射采样：Snell 折射方向投影到屏幕空间 ---
            // 近掠射时投影可能落到屏幕外；折射偏移会在屏幕边缘平滑收敛，
            // 避免直接拒绝/钳制 UV 产生底边截断。水面 mask 仍阻止跨岸采样。
            vec3 incidentWorld = normalize(worldPos - cam.cameraPos.xyz);
            float incidentNoN = dot(incidentWorld, N);
            vec3 refractionNormal = incidentNoN < 0.0 ? N : -N;
            float eta = incidentNoN < 0.0 ? (1.0 / WATER_IOR) : WATER_IOR;
            vec3 refractedWorld = refract(incidentWorld, refractionNormal, eta);
            float refractionWeight = 0.0;
            vec2 refrUV = fragTexCoord;
            if (dot(refractedWorld, refractedWorld) > 1e-6) {
                vec3 refractedView = mat3(cam.view) * normalize(refractedWorld);
                vec2 candidateUV = ProjectRefractionUV(fragTexCoord, surfaceView,
                    refractedView, underwaterDist, refractionWeight);
                if (texture(waterTex, candidateUV).r < 0.5) refractionWeight = 0.0;
                refrUV = mix(fragTexCoord, candidateUV, refractionWeight);
            }
            vec3 bottom = texture(litTex, refrUV).rgb * transmittance;

            vec3 L = normalize(pc.sunDir.xyz);
            vec3 sunLight = max(pc.lightColor.rgb, vec3(0.0));

            // 焦散图在水面波形对应的世界 XZ 空间烘焙，不跟随摄像机像素。
            // 使用折射落点的世界坐标与局部水深采样，实体平面水不套用地形波焦散。
            float causticFocus = 0.0;
            float refractedDepth = texture(depthTex, refrUV).r;
            vec4 refractedWater = texture(waterTex, refrUV);
            float causticWaterDepth = 0.0;
            float causticViewDepth = 0.0;
            vec3 causticBottomWorld = vec3(0.0);
            float causticMapInBounds = 0.0;
            vec2 causticMapUV = vec2(0.5);
            if (refractedWater.r > 1.5 && refractedDepth < 0.999999) {
                vec2 refractedNdc = refrUV * 2.0 - 1.0;
                vec3 causticBottomView = P2V(vec3(refractedNdc, refractedDepth));
                float causticBottomDist = length(causticBottomView);
                causticViewDepth = max(causticBottomDist - refractedWater.g, 0.0);
                causticBottomWorld = (cam.invView * vec4(causticBottomView, 1.0)).xyz;
                vec3 causticSurfaceView = normalize(P2V(vec3(refractedNdc, 0.5))) * refractedWater.g;
                vec3 causticSurfaceWorld = (cam.invView * vec4(causticSurfaceView, 1.0)).xyz;
                causticWaterDepth = max(causticSurfaceWorld.y - causticBottomWorld.y, 0.0);
                causticMapUV = CausticMapUV(causticBottomWorld.xz, causticMapInBounds);
                if (causticMapInBounds > 0.5) {
                    vec4 depthSlices = texture(causticsTex, causticMapUV);
                    causticFocus = max(SampleCausticDepth(depthSlices, causticWaterDepth), 0.0);
                }
            }
            if (causticFocus > 0.0) {
                vec3 receiverNormal = OctahedronDecode(texture(normalTex, refrUV).xy);
                vec3 receiverAlbedo = texture(albedoTex, refrUV).rgb;
                float receiverMetallic = clamp(texture(materialTex, refrUV).x, 0.0, 1.0);
                float receiverNoL = max(dot(receiverNormal, L), 0.0);
                vec3 causticRadiance = receiverAlbedo * ((1.0 - receiverMetallic) / PI) *
                    sunLight * (pc.sunDir.w * receiverNoL * causticFocus);
                vec3 causticTransmittance = exp(-WATER_ABSORPTION *
                    max(causticViewDepth, WATER_MIN_DIST));
                bottom += causticRadiance * causticTransmittance;
            }

            vec3 waterFog = EvaluateWaterFog();
            vec3 refraction = mix(bottom, waterFog, bodyFade);

            // --- 反射项：Fresnel(0.02) × [SSR 屏幕反射, 天空回退] + GGX 太阳高光 ---
            float NoV = max(dot(N, V), 1e-4);
            float fresnel = WATER_F0 + (1.0 - WATER_F0) * pow(1.0 - NoV, 5.0);

            vec3 R = reflect(-V, N);
            // SSR 落空或置信度不足时，100m 探针范围内回退到场景探针；
            // 超出范围直接使用天空 IBL。探针 alpha=覆盖率，空洞仍由 IBL 补齐。
            vec3 environmentRefl = SampleReflectionFallback(worldPos, R, WATER_ROUGHNESS);

            // --- 水面 SSR：复用 DoSSR 射线步进 ---
            // 水面不写主深度 → 深度缓冲里只有水底/岸上几何，命中即"反射该有的
            // 东西"（岸边草地/树/山体），不存在水面自命中；射线落空或落在屏幕
            // 边缘/走步过远（置信度低）→ 按权重回退天空反射。
            vec3 reflColor = environmentRefl;
            {
                vec3 Nv = mat3(cam.view) * N;              // 法线转视空间
                vec3 Vv = normalize(surfaceView);          // 相机→水面点（视空间）
                vec3 Rv = reflect(Vv, Nv);
                vec2 hitUV; float hitDist; bool hitssr;
                DoSSR(hitUV, hitDist, hitssr, surfaceView, Rv, depthTex,
                      interleaved_gradientNoise() * 0.5 + 0.5, 64.0);
                if (hitssr) {
                    // 反射源 = 已光照 litTex（gtao_apply 输出），钳回屏内防 wrap 垃圾
                    vec3 ssrColor = texture(litTex, clamp(hitUV, 0.001, 0.999)).rgb;
                    // 屏幕边缘淡出照旧；走步置信度淡出必须放宽：水面反射的命中点
                    // （岸上的树/草）天然离反射点很远，metallic 的近距判据
                    // (0.05,0.5) 会把权重压到 0 全回退天空 → 树倒影不可见。
                    vec2 edge = clamp(hitUV, 0.0, 1.0) * 2.0 - 1.0;
                    float edgeFade = 1.0 - pow(max(abs(edge.x), abs(edge.y)), 4.0);
                    float distFade = 1.0 - smoothstep(0.2, 1.5, hitDist);
                    reflColor = mix(environmentRefl, ssrColor, clamp(edgeFade * distFade, 0.0, 1.0));
                }
            }

            float NoL = max(dot(N, L), 0.0);
            float a = WATER_ROUGHNESS * WATER_ROUGHNESS;
            vec3 H = normalize(V + L);
            float NoH = max(dot(N, H), 0.0);
            float VoH = max(dot(V, H), 0.0);
            // Smith-GGX 简化 visibility（k = a/2）
            float k = a * 0.5;
            float Vis = 1.0 / (NoL * sqrt(NoV * NoV * (1.0 - k) + k) +
                               NoV * sqrt(NoL * NoL * (1.0 - k) + k) + 1e-4);
            float FH = WATER_F0 + (1.0 - WATER_F0) * pow(1.0 - VoH, 5.0);
            vec3 sunSpec = sunLight * NoL * D_GGX(NoH, a) * Vis * FH;
            // 水面法线近乎平面时 GGX 峰值可达数千 HDR，bloom 一吃就是占半屏的
            // 白斑；tonemap 后 ~10 即纯白，钳到 8 保留一条亮 glint 不炸屏。
            // GGX 峰值随法线接近平面可达数千 raw 量纲；×pc.sunDir.w 进入显示 HDR 域后，
            // 封顶同步 ×pc.sunDir.w（tonemap ÷pc.sunDir.w 还原）→ 显示结果与旧标定逐像素一致。
            sunSpec = min(sunSpec * pc.sunDir.w, vec3(8.0 * pc.sunDir.w));

            // Fresnel 增益 + 封顶：中景倒影可见性作弊项（见常量注释）
            float reflWeight = clamp(fresnel * WATER_FRESNEL_BOOST, 0.0, WATER_FRESNEL_MAX);
            lit = mix(refraction, reflColor, reflWeight) + sunSpec;
        }
    }

    // ===== 普通（金属）表面的统一 SSR =====
    // 水面由上方水面合成分支处理；其余金属像素沿用 G-buffer PBR 参数，
    // SSR 未命中/置信度衰减时近处回探针，超出探针捕获距离回退 IBL。
    if (centerDepth < 0.999999 && w.r <= 0.5) {
        vec4 material = texture(materialTex, fragTexCoord);
        float metallic = clamp(material.x, 0.0, 1.0);
        if (metallic > 0.05) {
            float roughness = clamp(material.y, 0.04, 1.0);
            vec3 albedo = texture(albedoTex, fragTexCoord).rgb;
            vec3 viewPos = P2V(vec3(fragTexCoord * 2.0 - 1.0, centerDepth));
            vec3 worldPos = (cam.invView * vec4(viewPos, 1.0)).xyz;
            vec3 N = OctahedronDecode(texture(normalTex, fragTexCoord).xy);
            vec3 Nv = normalize(mat3(cam.view) * N);
            vec3 Vv = normalize(viewPos);
            vec3 Rv = normalize(reflect(Vv, Nv));
            vec3 worldR = normalize(mat3(cam.invView) * Rv);

            vec2 hitUV;
            float hitDist;
            bool hitSSR;
            DoSSR(hitUV, hitDist, hitSSR, viewPos, Rv, depthTex,
                  interleaved_gradientNoise() * 0.5 + 0.5, 32.0);
            float ssrConfidence = 0.0;
            vec3 ssrColor = vec3(0.0);
            if (hitSSR) {
                ssrColor = texture(litTex, clamp(hitUV, 0.001, 0.999)).rgb;
                vec2 edge = clamp(hitUV, 0.0, 1.0) * 2.0 - 1.0;
                float edgeFade = 1.0 - pow(max(abs(edge.x), abs(edge.y)), 4.0);
                float distFade = 1.0 - smoothstep(0.05, 0.5, hitDist);
                ssrConfidence = clamp(edgeFade * distFade, 0.0, 1.0);
            }
            vec3 reflection = ssrColor;
            if (ssrConfidence < 0.999) {
                vec3 fallbackRefl = SampleReflectionFallback(worldPos, worldR, roughness);
                reflection = mix(fallbackRefl, ssrColor, ssrConfidence);
            }

            vec3 V = normalize(cam.cameraPos.xyz - worldPos);
            float NoV = clamp(dot(N, V), 0.0, 1.0);
            vec3 F0 = mix(vec3(0.04), albedo, metallic);
            vec3 brdf = EnvBRDFApprox(F0, roughness, NoV);
            float reflectionWeight = clamp(max(max(brdf.r, brdf.g), brdf.b), 0.0, 1.0);
            lit = mix(lit, reflection, reflectionWeight);
        }
    }

    outColor = vec4(lit, 1.0);
}
