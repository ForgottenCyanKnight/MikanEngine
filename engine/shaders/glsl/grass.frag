#version 450

// 草叶片元着色器：写地形所在的 G-buffer（albedo / 法线 / 材质 / 运动矢量）。
//
// 法线策略（2026-09-18 恢复原有方案）：叶面法线由屏幕空间导数取得，双面
// 渲染按视线翻正；根部 → 尖部前 1/3 快速过渡（t*3.0），向上偏置随 t 衰减
// 只在贴根处防黑根。（"全部朝上"方案已试过并回退——草叶失去体积感。）

layout(set = 0, binding = 0) uniform TerrainUniformData {
    mat4 projView;
    mat4 prevProjView;
    mat4 model;
    mat4 prevModel;
    mat4 normalMatrix;
    vec4 heightParams;   // heightScale, heightOffset, materialTiling, worldSizeX
    vec4 materialParams; // worldSizeZ, useControlMap, blendSharpness, reserved
    vec4 cameraPosition;
    vec4 taaJitter;
    vec4 timeWind;       // time(s), windStrength, grassViewDistance, heightGain
} ubo;

layout(location = 0) in vec4 inWorldPosT;  // worldPos.xyz, t
layout(location = 1) in vec4 inNormalTint; // groundNormal.xyz, tint
layout(location = 2) in vec2 inMotionVector;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outMaterial;
layout(location = 3) out vec2 outMotionVector;

vec2 SignNotZero(vec2 v) {
    return vec2(v.x < 0.0 ? -1.0 : 1.0,
                v.y < 0.0 ? -1.0 : 1.0);
}

vec2 OctahedronEncode(vec3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    if (n.z < 0.0) {
        n.xy = (1.0 - abs(n.yx)) * SignNotZero(n.xy);
    }
    return n.xy;
}

// 上色（用户拍板 2026-09-18）：单色平涂——噪声色斑/每株 tint/根部 AO/
// 尖部黄化全部去掉。观感不均匀的来源就是这些叠加变化。
void main() {
    vec3 worldPosition = inWorldPosT.xyz;
    float t = inWorldPosT.w;
    vec3 groundNormal = normalize(inNormalTint.xyz);
    float tint = inNormalTint.w;

    // 叶面法线：屏幕空间导数（每三角形平面法线），双面按视线翻正。
    vec3 faceNormal = normalize(cross(dFdx(worldPosition), dFdy(worldPosition)));
    vec3 viewDir = normalize(ubo.cameraPosition.xyz - worldPosition);
    faceNormal *= (dot(faceNormal, viewDir) < 0.0) ? 1.0 : -1.0;

    // 根部贴地、尖部立起：前 1/3 快速过渡到叶面法线（恢复原有方案）。
    float blendCurve = clamp(t * 3.0, 0.05, 1.0);
    vec3 normal = normalize(mix(groundNormal, faceNormal, blendCurve));
    // 向上偏置随 t 衰减：只在贴根处防黑根。
    normal = normalize(normal + vec3(0.0, 0.18 * (1.0 - t), 0.0));

    // 单色草绿（用户拍板：不做噪声色斑/tint 抖动/AO/黄化），亮度对齐
    // 地面草层的保亮度重着色结果（tint × 纹理亮度≈1.5×），草与地面同调。
    // alpha = 0.5 是"双面叶"哨兵：光照 pass 对该标记用 abs(N·L) 双面照明
    //（薄叶两面透光，法线翻正方向不再决定生死），向光面不会再整叶变黑。
    // 注意避开 model.frag 的透射编码区间（0.5~0.75 因子>0），0.5 = 透射 0。
    vec3 albedo = vec3(0.34, 0.56, 0.15);

    outColor = vec4(albedo, 0.5);
    outNormal = vec4(OctahedronEncode(normal), 0.0, 0.0);
    outMaterial = vec4(0.0, 0.85, 1.0, 0.0);
    outMotionVector = inMotionVector;
}
