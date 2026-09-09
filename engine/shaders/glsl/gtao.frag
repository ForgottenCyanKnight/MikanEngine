#version 450
// GTAO（Jimenez 2016）+ ReBLUR 式历史帧双边过滤 + 相机 UBO 重投影

precision highp float;

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outGTAO;   // R=AO, G=体积光, B=Godray, A=预留（半分辨率）

layout(push_constant) uniform PC {
    vec4 cameraPos;
    vec4 sunDir;
    vec4 lightColor;
    vec4 frameInfo;
} pc;

layout(binding = 0) uniform sampler2D sceneDepth;
layout(binding = 1) uniform sampler2D sceneNormal;
layout(binding = 2) uniform sampler2D historyTex;
layout(binding = 3) uniform sampler2D bluenoiseTex;
layout(binding = 4) uniform sampler2D motionTex;
layout(binding = 5) uniform sampler2DArrayShadow csmShadowMaps;
layout(binding = 6) uniform sampler2D cloudMaskTex;               // cloud_view alpha：天空云透射率

// Camera UBO（binding 8）
layout(binding = 8) uniform CameraUBO {
    vec4 cameraPos;
    mat4 proj;
    mat4 view;
    mat4 prevViewProj;
    mat4 invProj;
    mat4 invView;
    // CSM 级联数据（gtao 半分辨率体积光采样阴影）
    mat4 csmMatrices[4];   // 世界 → 光 NDC
    vec4 csmSplitFars;     // 每级联 far
    vec4 csmParams;        // x = 级联数, y = 启用
} cam;

// 八面体编码/解码在零分量处不能使用 sign(0)=0，否则 -Z 极点会和 +Z 冲突。
vec2 SignNotZero(vec2 v) {
    return vec2(v.x < 0.0 ? -1.0 : 1.0,
                v.y < 0.0 ? -1.0 : 1.0);
}

// ===== 工具 =====
vec3 OctahedronDecode(vec2 oct) {
    vec3 n = vec3(oct, 1.0 - abs(oct.x) - abs(oct.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * SignNotZero(n.xy);
    return normalize(n);
}
vec3 ReconstructViewPos(vec2 uv, float depth) {
    vec2 ndc = uv * 2.0 - 1.0;
    vec4 vp = cam.invProj * vec4(ndc, depth, 1.0);
    return vp.xyz / vp.w;
}
#define PI 3.14159265359

float FastACos(float x) {
    float ax = abs(x);
    float res = -0.156583 * ax + 1.570796;
    res *= sqrt(1.0 - ax);
    return (x >= 0.0) ? res : 3.141593 - res;
}

// ===================================================================
// GTAO with adaptive view-space stepping.
// ===================================================================
#define GTAO_SLICES     4
#define GTAO_STEPS      2
#define GTAO_RADIUS     0.25
#define GTAO_POWER_EXP  1.0

float GTAO(vec2 uv, vec3 viewPos, vec3 normal) {
    float k = float(int(pc.frameInfo.x) % 16);
    vec3 blue = texture(bluenoiseTex, gl_FragCoord.xy / 128.0).rgb;
    float dist = length(viewPos);
    float scaling = GTAO_RADIUS / dist;
    vec3 viewV = normalize(-viewPos);
    float visibility = 0.0, projLenSum = 0.0;

    for (int slice = 0; slice < GTAO_SLICES; slice++) {
        float phi = PI / float(GTAO_SLICES) * (float(slice) + blue.r) + k * PI / float(GTAO_SLICES) / 16.0;
        vec2 omega = normalize(vec2(cos(phi), sin(phi)));
        vec3 dirV = vec3(omega, 0.0);
        vec3 orthoD = dirV - dot(dirV, viewV) * viewV;
        vec3 axisV = cross(dirV, viewV);
        vec3 projN = normal - axisV * dot(normal, axisV);
        float sgnN = sign(dot(orthoD, projN));
        float pLen = length(projN);
        float cosN = clamp(dot(projN, viewV) / max(pLen, 0.0001), 0.0, 1.0);
        float n = sgnN * FastACos(cosN), sinN = sin(n);
        float bSum = 0.0;
        for (int side = 0; side <= 1; side++) {
            float sideSign = 2.0 * float(side) - 1.0;
            float cHC = -1.0;
            for (int s = 0; s < GTAO_STEPS; s++) {
                float sv = (float(s) + fract(blue.g + k * 0.0625)) / float(GTAO_STEPS);
                sv = pow(sv, GTAO_POWER_EXP);
                vec2 offset = sideSign * sv * scaling * omega;
                vec2 sUV = uv + offset;
                if (clamp(sUV, 0.0, 1.0) != sUV) continue;
                float sD = texture(sceneDepth, sUV).r;
                if (sD >= 0.999999) continue;
                vec3 sPosV = ReconstructViewPos(sUV, sD);
                float hCos = dot(normalize(sPosV - viewPos), viewV);
                hCos = mix(-1.0, hCos, smoothstep(0.0, 1.0, GTAO_RADIUS * 1.41 / distance(sPosV, viewPos)));
                cHC = max(cHC, hCos);
            }
            float h = n + clamp(sideSign * FastACos(cHC) - n, -PI / 2.0, PI / 2.0);
            bSum += (cosN + 2.0 * h * sinN - cos(2.0 * h - n)) / 4.0;
        }
        float noOccl = cosN + n * sinN;
        projLenSum += pLen;
        visibility += pLen * bSum / max(noOccl, 1e-4);
    }
    return visibility / max(projLenSum, 1e-4);
}

// ===================================================================
// ReBLUR 式历史帧双边过滤
// ===================================================================
float BilateralFilterHistory(vec2 centerUV, float centerDepth, vec3 centerNormal) {
    vec2 texel = 1.0 / vec2(textureSize(historyTex, 0));
    float sum = 0.0;
    float wsum = 0.0;

    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            vec2 suv = centerUV + vec2(x, y) * texel;

            float sDepth = texture(sceneDepth, suv).r;
            if (sDepth >= 0.999999) continue;

            vec3 sViewPos = ReconstructViewPos(suv, sDepth);
            vec3 sWorldN = OctahedronDecode(texture(sceneNormal, suv).xy);
            vec3 sViewN = normalize(mat3(transpose(cam.invView)) * sWorldN);
            sViewN.y = -sViewN.y;

            float depthDiff = abs(centerDepth - sViewPos.z) / max(abs(centerDepth), 1e-3);
            float depthW = exp(-depthDiff * 40.0);
            float normalW = pow(max(dot(centerNormal, sViewN), 0.0), 32.0);
            float spatialW = exp(-float(x * x + y * y) * 1.0);
            float w = spatialW * depthW * normalW;

            float histAO = texture(historyTex, suv).r;
            sum += histAO * w;
            wsum += w;
        }
    }
    return wsum > 0.001 ? sum / wsum : texture(historyTex, centerUV).r;
}

// ===== 基于 CSM 的太阳阴影采样（gtao 半分辨率体积光用）=====
// worldPos → 选级联（view 空间深度）→ 投影到 shadow map 查硬件比较阴影
float LisShadeAt(vec3 worldPos, float viewDepth) {
    if (cam.csmParams.y < 0.5 || cam.csmParams.x < 0.5) return 0.0;
    int count = int(cam.csmParams.x + 0.5);
    if (count <= 0) return 0.0;
    // 选级联：找第一个 splitFar >= viewDepth 的
    int sel = -1;
    for (int i = 0; i < count; i++) {
        if (viewDepth <= cam.csmSplitFars[i]) { sel = i; break; }
    }
    if (sel < 0) sel = count - 1;
    // 投影到光 NDC
    vec4 clip = cam.csmMatrices[sel] * vec4(worldPos, 1.0);
    if (abs(clip.w) < 1e-6) return 0.5;
    vec3 ndc = clip.xyz / clip.w;
    vec2 uv = vec2(ndc.x * 0.5 + 0.5, 0.5 + ndc.y * 0.5);
    // ⚠️ 越界（CSM 覆盖范围外）：返回 0.5 中性——不产光柱也不压暗（不再全当透光 → 洗白）
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return 0.5;
    if (ndc.z < 0.0 || ndc.z > 1.0) return 0.5;   // 深度越界保守
    // 硬件 shadow compare（sampler2DArrayShadow + ref）：附件 = ndc.z ∈ [0,1] 直存
    float refDepth = ndc.z;
    float sh = texture(csmShadowMaps, vec4(uv, float(sel), refDepth));
    return sh;
}

// ===== 基于计算太阳光线（体积光强度，半分辨率 raymarch）=====
// 沿视线从近处到表面 worldPos raymarch，每步查 CSM 阴影透过率，累积前向散射 → 返回体积光强度
float ComputeLightScattering(vec3 camPos, vec3 surfaceWorld, float dither) {
    // ⚠️ CSM 未启用（无阴影遮挡信息）→ 不产生体积光（否则越界全当透光 → 全域洗白发灰）
    if (cam.csmParams.y < 0.5 || cam.csmParams.x < 0.5) return 0.0;
    // 世界视向、光源方向
    vec3 rayDir = normalize(surfaceWorld - camPos);
    float dist = length(surfaceWorld - camPos);
    float tEnd = min(dist, 120.0);
    if (tEnd <= 0.5) return 0.0;
    // ⚠️ 减少采样数（8 步）+ dither 抖动偏移 → 时序累积降噪（不复用历史则条纹）
    const int STEPS = 12;
    const float DENSITY = 0.06;
    vec3 Ldir = normalize(pc.sunDir.xyz);

    float stepSize = (tEnd - 0.5) / float(STEPS);
    float total = 0.0;
    // 起始用 dither 相位偏移（非整数步别让采样网格固定 → 帧间错开被时序平均平滑）
    float start = dither * stepSize;
    for (int i = 0; i < STEPS; i++) {
        float t = 0.5 + start + stepSize * (float(i) + 0.5);
        if (t > tEnd) continue;
        vec3 p = camPos + rayDir * t;
        float viewD = t;
        float sh = LisShadeAt(p, viewD);
        float ct = dot(rayDir, Ldir);
        float g = 0.8;
        float denom = 1.0 + g * g - 2.0 * g * ct;
        float phase = (1.0 - g * g) / (denom * sqrt(max(denom, 1e-6)));
        total += sh * phase * DENSITY * stepSize;
    }
    return total;
}

// IG 噪声（每帧采样相位偏移——体积光 dither + 抖动，配合时序累积平滑）
float interleaved_gradientNoise() {
    vec2 coord = gl_FragCoord.xy;
    return fract(52.9829189 * fract(0.06711056 * coord.x + 0.00583715 * coord.y));
}

// 原理：天空像素（depth≈1）是光束的亮度源。从每个像素沿"指向太阳"方向（太阳屏幕投影位置）
// 递增距离采样"天空掩码"，被实体遮挡（采样点 depth<1）的方向不贡献 → 透过缝隙形成辐射光束。
// 只使用 depth 和 cloud_view 的透射率，不需要 composite 颜色。
float ComputeGodray(vec2 uv, float centerDepth) {
    // 太阳方向投影到屏幕 NDC → sunUV（世界方向向量用 view 旋转）
    vec3 sunDirW = normalize(pc.sunDir.xyz);
    vec3 sunViewDir = normalize(mat3(cam.view) * sunDirW);
    vec4 clip = cam.proj * vec4(sunViewDir, 1.0);
    if (abs(clip.w) < 1e-6) return 0.0;
    vec2 sunNdc = clip.xy / clip.w;
    vec2 sunUV = sunNdc * 0.5 + 0.5;
    // 若太阳在屏幕外/后方，方向发散 → 无 Godray 或整屏弱
    if (sunUV.x < -1.0 || sunUV.x > 2.0 || sunUV.y < -1.0 || sunUV.y > 2.0) return 0.0;

    // 从当前像素沿"指向太阳"方向采天空掩码（朝向太阳越近越亮）
    vec2 dirToSun = sunUV - uv;
    const int STEPS = 16;
    float total = 0.0;
    // cloud_view 的 A 通道是背景透射率：1 = 没有云，0 = 云完全遮挡。
    // GTAO 与 cloud_view 都是半分辨率，因此这里可以直接用同一组 UV 采样。
    float centerCloudTransmittance = clamp(texture(cloudMaskTex, uv).a, 0.0, 1.0);
    // 只在"从光源发散出去"的方向采（uv 指向太阳的反方向延伸 = 光束从太阳射出）
    // 标准：沿 uv→sunUV 方向（减距），越靠近太阳天空越密 → 每步衰减
    vec2 stepVec = dirToSun / float(STEPS + 1);
    for (int i = 1; i <= STEPS; i++) {
        vec2 suv = uv + stepVec * float(i);   // 从当前像素往太阳方向逐点靠近
        suv = clamp(suv, 0.0, 1.0);
        float sd = texture(sceneDepth, suv).r;
        // 天空掩码：深度接近 1 = 天空（光束源）；实体（sd<1）遮挡 → 该方向不计
        float skyMask = sd >= 0.9999 ? 1.0 : 0.0;
        float cloudTransmittance = clamp(texture(cloudMaskTex, suv).a, 0.0, 1.0);
        // 越靠近太阳，采样点天空密度加权越高（光束从太阳辐射）
        float fall = pow(0.93, float(i));   // 距离衰减：近太阳处采样贡献大
        total += skyMask * cloudTransmittance * fall;
    }
    // 归一：靠近太阳的天空像素得到高 Godray
    return (total / float(STEPS)) * centerCloudTransmittance;
}

void main() {
    float depth = texture(sceneDepth, fragTexCoord).r;
    outGTAO = vec4(1.0, 0.0, 0.0, 0.0);

    // ===== Godray（B 通道）：depth 天空掩码 + 太阳径向模糊，天空/实体统一 =====
    float godrayRaw = 1.0;//ComputeGodray(fragTexCoord, depth);
    float histGod = texture(historyTex, fragTexCoord).b;
    // 云会移动，Godray 历史不能继续沿用 GTAO 的高累积比例，否则云边
    // 离开后旧光束还会滞留数十帧。
    float godray = mix(godrayRaw, histGod, 0.55);

    if (depth >= 0.999999) {
        // 天空：Godray 已由 ComputeGodray 计算（天空掩码高），直接输出
        outGTAO.b = godray;
        return;
    }

    vec3 viewPos = ReconstructViewPos(fragTexCoord, depth);
    vec3 worldPos = (cam.invView * vec4(viewPos, 1.0)).xyz;
    vec3 worldN = OctahedronDecode(texture(sceneNormal, fragTexCoord).xy);
    vec3 viewN = mat3(transpose(cam.invView)) * worldN;
    viewN.y = -viewN.y;

    // 计算当前帧 AO
    float rawAO = GTAO(fragTexCoord, viewPos, viewN);

    float filteredHist = BilateralFilterHistory(fragTexCoord, viewPos.z, viewN);

    float ao = mix(rawAO, filteredHist, 0.9);   // rawAO 在前
    // ===== 半分辨率体积光强度（G 通道），8 步 + dither + 时序累积降噪 =====
    float dither = interleaved_gradientNoise();
    float lightScatter = 1.0;//ComputeLightScattering(cam.cameraPos.xyz, worldPos, dither);
    float histVol = texture(historyTex, fragTexCoord).g;
    float volIntensity = mix(lightScatter, histVol, 0.85);

    outGTAO = vec4(ao, volIntensity, godray, 0.0);
}
