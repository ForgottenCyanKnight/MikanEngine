#version 450
// Screen-space global illumination at half resolution.
// 原理：对每个像素，沿法线半球余弦采样 N 方向，每个方向屏幕空间 ray march 找命中，
// 命中的点取 composite（光照后场景色）作入射光，余弦加权累积 → 输出间接光 RGB。
// 置于 gtao 之后、gtao_apply 之前；低频光靠半分辨率 + 时间累积降噪。

precision highp float;

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec3 outSSGI;

layout(push_constant) uniform PC {
    vec4 cameraPos;    // 与 PostProcessQuad::PushData 一致（4 vec4，offset 48 = frameInfo）
    vec4 sunDir;
    vec4 lightColor;
    vec4 frameInfo;
} pc;

layout(binding = 0) uniform sampler2D depthTex;    // 全分辨率深度（NDC z 直存）
layout(binding = 1) uniform sampler2D normalTex;   // gbuffer1（八面体世界法线）
layout(binding = 2) uniform sampler2D colorTex;    // composite（光照后场景色，间接光源）
layout(binding = 3) uniform sampler2D historyTex;  // 上帧 SSGI（半分辨率 HDR，时序累积降噪）
layout(binding = 4) uniform sampler2D bluenoiseTex; // STBN 128×128（R/G 时空、B 静态）——时空蓝噪声抖动

// Camera UBO（binding 8）
layout(binding = 8) uniform CameraUBO {
    vec4 cameraPos;
    mat4 proj;
    mat4 view;
    mat4 prevViewProj;
    mat4 invProj;
    mat4 invView;
} cam;

const float PI = 3.14159265359;

vec2 SignNotZero(vec2 v) {
    return vec2(v.x < 0.0 ? -1.0 : 1.0,
                v.y < 0.0 ? -1.0 : 1.0);
}

vec3 OctahedronDecode(vec2 oct) {
    vec3 n = vec3(oct, 1.0 - abs(oct.x) - abs(oct.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * SignNotZero(n.xy);
    return normalize(n);
}

// 深度 → view 位置（与 gtao/fullscreen 一致：深度直传 NDC）
vec3 ReconstructViewPos(vec2 uv, float depth) {
    vec2 ndc = uv * 2.0 - 1.0;
    vec4 vp = cam.invProj * vec4(ndc, depth, 1.0);
    return vp.xyz / vp.w;
}

// 视图空间位置 → 屏幕空间 [0,1] uv（用于采样）
vec2 ProjectToUV(vec3 viewPos) {
    vec4 clip = cam.proj * vec4(viewPos, 1.0);
    return (clip.xy / clip.w) * 0.5 + 0.5;
}

// 稳定正交基（避免切线基在近水平法线处退化 → 采样方向跳变环）
void BuildOrthonormalBasis(vec3 N, out vec3 T, out vec3 B) {
    // 选与 N 最不垂直的轴做主基（仿 Frisvad / Duff 稳定基）
    vec3 up = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    T = normalize(cross(up, N));
    B = cross(N, T);
}

// 余弦加权半球采样（余弦分布 + 稳定基）
vec3 CosineSampleHemisphere(vec3 N, vec2 xi) {
    vec3 T, B;
    BuildOrthonormalBasis(N, T, B);
    float u1 = xi.x, u2 = xi.y;
    float r = sqrt(max(u1, 0.0));
    float phi = 2.0 * PI * u2;
    vec3 dir = T * (r * cos(phi)) + B * (r * sin(phi)) + N * sqrt(max(1.0 - u1, 0.0));
    return normalize(dir);
}

// 亮度
float DensityLum(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

// Edge-stopping weight for geometry-aware temporal and spatial filtering.
float computeWeight(float depthCenter, float depthP, float phiDepth,
                    vec3 normalCenter, vec3 normalP, float phiNormal,
                    float lumCenter, float lumP, float phiIllum) {
    float normalDot = dot(normalize(normalCenter), normalize(normalP));
    float weightNormal = pow(clamp(normalDot, 0.0, 1.0), phiNormal * 0.1);
    float weightZ = abs(depthCenter - depthP) / max(phiDepth, 1e-4);
    weightZ = smoothstep(0.0, 1.5, weightZ);
    float weightIllum = abs(lumCenter - lumP) / max(phiIllum, 1e-4);
    weightIllum = smoothstep(0.0, 2.0, weightIllum);
    float weight = exp(-weightIllum * 2.0 - weightZ * 2.0) * weightNormal;
    return max(weight, 0.01);
}

// view 空间位置深度（线性）——用于几何权重
float LinearizeViewDepth(vec3 viewPos) { return length(viewPos); }

// 一个方向做屏幕空间 ray march，返回命中的间接光颜色（未命中返回 0）
vec3 RayMarchIndirect(vec3 viewPos, vec3 worldDir, vec3 worldPos) {
    // 方向转 view 空间
    vec3 viewDir = normalize(mat3(cam.view) * worldDir);
    // 指数步进：近处密、远处疏
    float t = 0.1;
    const int STEPS = 24;
    for (int i = 0; i < STEPS; i++) {
        vec3 sampleView = viewPos + viewDir * t;
        vec2 uv = ProjectToUV(sampleView);
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) break;
        // 采样该 uv 的深度，与该点位置深度比较
        float sceneDepth = texture(depthTex, uv).r;
        if (sceneDepth >= 0.9999) break;   // 天空：无遮挡，方向通向天空
        vec3 sceneViewPos = ReconstructViewPos(uv, sceneDepth);
        // 命中判定：采样点位置深度穿过场景表面（差 < 阈值）
        float hitDist = length(sceneViewPos - sampleView);
        if (hitDist < t * 0.6) {
            // 命中：取 composite 该点颜色作为间接光
            return texture(colorTex, uv).rgb;
        }
        t *= 1.3;
    }
    return vec3(0.0);
}

void main() {
    float depth = texture(depthTex, fragTexCoord).r;
    outSSGI = vec3(0.0);
    if (depth >= 0.9999) return;   // 天空无间接光

    vec3 viewPos = ReconstructViewPos(fragTexCoord, depth);
    vec3 worldPos = (cam.invView * vec4(viewPos, 1.0)).xyz;
    vec3 N = OctahedronDecode(texture(normalTex, fragTexCoord).xy);

    // ===== 时空蓝噪声（STBN）+ 时域相位：空间均匀 + 每帧旋转（时序累积平滑）=====
    vec2 stbnUv = (floor(gl_FragCoord.xy) + 0.5) / 128.0;
    vec3 blue = texture(bluenoiseTex, stbnUv).rgb;   // STBN .r/.g/.b 独立低相关
    float tPhase = fract(pc.frameInfo.x * 0.381966);   // 帧间黄金角相位（时空旋转）

    const int N_SAMPLES = 16;
    vec3 indirect = vec3(0.0);
    float wSum = 0.0;
    for (int k = 0; k < N_SAMPLES; k++) {
        // 每方向：蓝噪声 .r（基线）+.g（另一维）+ 帧相位；黄金角螺旋打散不聚类
        float ga = 2.399963229728653 * float(k);
        vec2 xi = fract(vec2(blue.r, blue.g) + vec2(ga, tPhase + ga * 0.5));
        vec3 dir = CosineSampleHemisphere(N, xi);
        float NoL = max(dot(N, dir), 0.0);
        vec3 hit = RayMarchIndirect(viewPos, dir, worldPos);
        indirect += hit * NoL * 2.0;
        wSum += NoL;
    }
    // 归一化（平均间接光强度）
    vec3 rawIndirect = indirect / max(wSum, 1e-4);
    vec3 raw = rawIndirect;

    // Joint bilateral disk filtering using depth and normal agreement.
    // 半分辨率纹素尺寸（历史/输出同半分辨率）
    const float SAMPLES = 16.0;
    float MAXBLUR = 3.0;   // 半分辨率圆盘半径（像素）
    float TIME_WEIGHT = 8.0;   // 时间权重：历史累积强度（增大=更平滑/更重历史，减小=更信任本帧）
    vec2 texel = 1.0 / vec2(textureSize(historyTex, 0));   // 历史=半分辨率，圆盘按半分辨率像素
    vec2 rtScale = vec2(1.0);
    // 中心几何
    float centerDepth = LinearizeViewDepth(viewPos);
    vec3 centerNormal = N;
    float centerLum = DensityLum(raw);
    // 圆盘采样：本帧空间 + 历史时间（几何一致才混）
    vec3 colorSum = raw;          // 中心（本帧采样，权重 1）
    float weightSum = 1.0;
    for (float i = 0.0; i < SAMPLES; i++) {
        // 蓝噪声圆盘偏移（空间+时间抖动）
        vec3 bn = texture(bluenoiseTex, (floor(gl_FragCoord.xy) + 0.5) / 128.0 + vec2(i * 0.5)).rgb;
        float angle = bn.x * 2.0 * PI + pc.frameInfo.x * 0.1;
        float radiusNorm = pow(bn.y, 0.4);
        vec2 offset = vec2(cos(angle), sin(angle)) * radiusNorm * MAXBLUR * rtScale * texel;
        vec2 sampleUV = fragTexCoord + offset;
        if (sampleUV.x < 0.0 || sampleUV.x > 1.0 || sampleUV.y < 0.0 || sampleUV.y > 1.0) continue;
        // 采样点几何
        float sd = texture(depthTex, sampleUV).r;
        if (sd >= 0.9999) continue;
        vec3 sv = ReconstructViewPos(sampleUV, sd);
        float sDepth = LinearizeViewDepth(sv);
        vec3 sNormal = OctahedronDecode(texture(normalTex, sampleUV).xy);
        // 历史（时间）样本
        vec3 sColor = texture(historyTex, sampleUV).rgb;
        float sLum = DensityLum(sColor);
        float w = computeWeight(centerDepth, sDepth, 0.01, centerNormal, sNormal, 0.6, centerLum, sLum, 0.5);
        w *= TIME_WEIGHT;   // 时间权重放大历史累积
        colorSum += sColor * w;
        weightSum += w;
    }
    // 历史/空间混合（几何一致加权平均）——物体边缘避免跨几何模糊
    outSSGI = colorSum / max(weightSum, 1e-4);
}
