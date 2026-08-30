#version 450

precision highp float;
precision highp int;

layout(location = 0) in vec3 fragPosition;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec4 fragAlbedoColor;
layout(location = 3) in vec4 fragMaterialData;
layout(location = 4) in vec2 fragMotionVector;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec2 outMotionVector;
layout(location = 3) out vec4 outMaterial;

// 2026-08-11 八面体编码（Cigolle 2014 对称版）——世界法线 → [-1,1]²（R16G16_SNORM 直接存，含朝向）
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

void main() {
    vec3 albedo = fragAlbedoColor.rgb;
    vec3 N = normalize(fragNormal);
    
    outColor = vec4(albedo, 1.0);
    outNormal = vec4(OctahedronEncode(N), 0.0, 0.0);   // R16G16_SNORM 八面体编码（2026-08-11）
    outMotionVector = fragMotionVector;
    outMaterial = vec4(fragMaterialData.xyz, 1.0);
}
