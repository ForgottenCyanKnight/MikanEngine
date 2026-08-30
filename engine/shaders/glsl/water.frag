#version 450

layout(location = 0) in vec3 inWorldPosition;
layout(location = 1) in vec3 inWorldNormal;
layout(location = 2) in vec2 inUv;
layout(location = 3) in vec4 inColor;
layout(location = 4) in vec4 inMaterial;
layout(location = 5) in vec2 inMotionVector;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outMaterial;
layout(location = 3) out vec2 outMotionVector;

vec2 SignNotZero(vec2 v) {
    return vec2(v.x < 0.0 ? -1.0 : 1.0,
                v.y < 0.0 ? -1.0 : 1.0);
}

vec2 OctahedronEncode(vec3 n) {
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 0.0001);
    if (n.z < 0.0) {
        n.xy = (1.0 - abs(n.yx)) * SignNotZero(n.xy);
    }
    return n.xy;
}

void main() {
    vec3 normal = normalize(inWorldNormal);
    if (!gl_FrontFacing) normal = -normal;

    // 第一阶段使用不透明水面：alpha 固定为 1，后续 mask/后处理可以
    // 使用同一套 outColor 和 inUv，不需要重新生成水体网格。
    outColor = vec4(inColor.rgb, 1.0);
    outNormal = vec4(OctahedronEncode(normal), 0.0, 0.0);
    outMaterial = vec4(inMaterial.x, inMaterial.y, inMaterial.z, inMaterial.w);
    outMotionVector = inMotionVector;
}
