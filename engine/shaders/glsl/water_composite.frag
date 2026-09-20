#version 450

// ===== 独立水面合成 pass（deferred water compositing 第二阶段）=====
// 从 gtao_apply 拆出：AMD 780M 驱动在编译 CameraUBO 位于 binding 10 的
// gtao_apply 管线时 vkCreateGraphicsPipelines CPU 侧崩溃（0xC0000409 /
// 0xC0000005）。拆分后本 pass 仅 4 个 sampler（slot 0-3），maxInputs=8，
// UBO 落在 binding 8，远离触发区间。
// 链位置：gtao_apply 之后、taa/bloom 之前（pass:before 自动前溯）。
// 输入 litTex = gtao_apply 输出（已含 AO/emissive 待遇、metallic SSR、
// 雾、云透射），折射偏移采样直接复用其待遇，无需重复乘 AO。

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

layout(binding = 0) uniform sampler2D litTex;    // pass:gtao_apply（HDR 合成结果）
layout(binding = 1) uniform sampler2D depthTex;  // 全分辨率深度（水底视距 + SSR 射线步进）
layout(binding = 2) uniform sampler2D skyRT;     // 全景天空（水雾环境色 + 反射）
layout(binding = 3) uniform sampler2D waterTex;  // WaterTargetRT（R=mask, G=水面线性视距(m), BA=八面体世界法线；Nearest）

// Camera UBO（binding 8；maxInputs = max(8, maxSlot+1) = 8）
layout(binding = 8) uniform CameraUBO {
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

// ===== 硬编码水材质（常见清水 PBR 值）=====
const float WATER_F0          = 0.02;                      // ((1-1.33)/(1+1.33))^2
const float WATER_ROUGHNESS   = 0.08;
const vec3  WATER_ABSORPTION  = vec3(0.55, 0.13, 0.08);    // 1/m，Beer-Lambert（红光衰减最快→透射偏蓝）
const vec3  WATER_BODY_TINT   = vec3(0.18, 0.50, 0.62);    // 清水水体色（淡蓝青）
const float WATER_MIN_DIST    = 1.2;                       // 视深下限(m)：近岸浅水(cm 级路径长)若不保底，水体色≈0 看起来像裸地形"消失"
const float WATER_SHORE_TOL   = 0.25;                      // 岸线容差(m)：水面比场景深度落后 ≤此值仍合成（浅滩带地形/草略高于水面）
const float WATER_FRESNEL_BOOST = 2.5;                     // 水面 Fresnel 增益（游戏作弊项）：陡视角物理值 ~0.03-0.06 会让中景
                                                           // 倒影（树/岸）几乎不可见；增益后近岸掠射行为不变，仅中景增强
const float WATER_FRESNEL_MAX = 0.7;                       // 增益后的混合权重上限
const float WATER_BODY_FADE   = 0.18;                      // 水体雾色浓度 1-exp(-k·d) 的 k（0.5m→~0.09，2m→~0.30，4m→~0.51）
const float WATER_REFRACT     = 0.035;                     // 折射 UV 扰动系数：水底色采样点按波纹法线 xz 偏移（×视深，5m 封顶）

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
    if (w.r > 0.5 && centerDepth < 0.999999) {
        vec2 ndcXY = fragTexCoord * 2.0 - 1.0;
        vec3 rayDir = normalize(P2V(vec3(ndcXY, 0.5)));
        float surfaceDist = w.g;
        float bottomDist = length(P2V(vec3(ndcXY, centerDepth)));
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

            // --- 水底折射采样：按波纹法线偏移 UV 取水底色（屏幕空间折射近似）---
            // 偏移量 = 法线水平分量 × 视深缩放（5m 封顶：远处波动太密偏移会闪烁）。
            // 两重钳制防误采样：
            //   ① 屏幕边缘 clamp（偏移落在画面外会取到 wrap 垃圾）；
            //   ② mask 守门（偏移落点不是水面 → 回退中心采样，防止把岸上
            //      几何/天空拽进水下画面）。
            // litTex（gtao_apply 输出）已含 AO/emissive 待遇，偏移点与中心同源。
            vec2 refrOff = N.xz * (WATER_REFRACT * min(underwaterDist, 5.0));
            vec2 refrUV = clamp(fragTexCoord + refrOff, vec2(0.002), vec2(0.998));
            if (texture(waterTex, refrUV).r < 0.5) refrUV = fragTexCoord;
            vec3 bottom = texture(litTex, refrUV).rgb * transmittance;

            vec3 L = normalize(pc.sunDir.xyz);
            vec3 sunLight = max(pc.lightColor.rgb, vec3(0.0));
            vec3 skyAmb = colors_LogLuv32ToSRGB(
                texture(skyRT, skylutuv(normalize(vec3(0.2, 1.0, 0.1)), max(cam.cameraPos.y, 0.0))));
            float NoL_water = max(vec3(0.0, 1.0, 0.0).y * L.y, 0.0);
            vec3 waterFog = WATER_BODY_TINT * (skyAmb + sunLight * NoL_water * 0.08) * 0.7;
            waterFog = max(waterFog, vec3(0.03, 0.08, 0.10));   // 阴影/夜间保底淡蓝（压低：雾色过亮会糊成奶白）
            vec3 refraction = mix(bottom, waterFog, bodyFade);

            // --- 反射项：Fresnel(0.02) × [SSR 屏幕反射, 天空回退] + GGX 太阳高光 ---
            float NoV = max(dot(N, V), 1e-4);
            float fresnel = WATER_F0 + (1.0 - WATER_F0) * pow(1.0 - NoV, 5.0);

            vec3 R = reflect(-V, N);
            vec3 skyRefl = colors_LogLuv32ToSRGB(texture(skyRT, skylutuv(R, max(cam.cameraPos.y, 0.0))));

            // --- 水面 SSR：复用 DoSSR 射线步进 ---
            // 水面不写主深度 → 深度缓冲里只有水底/岸上几何，命中即"反射该有的
            // 东西"（岸边草地/树/山体），不存在水面自命中；射线落空或落在屏幕
            // 边缘/走步过远（置信度低）→ 按权重回退天空反射。
            vec3 reflColor = skyRefl;
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
                    reflColor = mix(skyRefl, ssrColor, clamp(edgeFade * distFade, 0.0, 1.0));
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
            sunSpec = min(sunSpec, vec3(8.0));

            // Fresnel 增益 + 封顶：中景倒影可见性作弊项（见常量注释）
            float reflWeight = clamp(fresnel * WATER_FRESNEL_BOOST, 0.0, WATER_FRESNEL_MAX);
            lit = mix(refraction, reflColor, reflWeight) + sunSpec;
        }
    }

    outColor = vec4(lit, 1.0);
}
