#version 450

layout(set = 0, binding = 0) uniform WaterUniformData {
    mat4 projView;
    mat4 prevProjView;
    vec4 cameraPosition;
    vec4 taaJitter;
} ubo;

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec2 inUv;
layout(location = 2) in mat4 inModel;
layout(location = 6) in mat4 inPreviousModel;
layout(location = 10) in vec4 inColor;
layout(location = 11) in vec4 inMaterial;

layout(location = 0) out vec3 outWorldPosition;
layout(location = 1) out vec3 outWorldNormal;
layout(location = 2) out vec2 outUv;
layout(location = 3) out vec4 outColor;
layout(location = 4) out vec4 outMaterial;
layout(location = 5) out vec2 outMotionVector;

void main() {
    vec3 localPosition = vec3(inPosition.x, 0.0, inPosition.y);
    vec4 worldPosition = inModel * vec4(localPosition, 1.0);
    vec4 clipPosition = ubo.projView * worldPosition;
    clipPosition.xy += ubo.taaJitter.xy * clipPosition.w;
    gl_Position = clipPosition;

    vec4 previousWorldPosition = inPreviousModel * vec4(localPosition, 1.0);
    vec4 previousClipPosition = ubo.prevProjView * previousWorldPosition;
    vec2 currentNdc = clipPosition.xy / max(abs(clipPosition.w), 0.000001);
    currentNdc -= ubo.taaJitter.xy;
    vec2 previousNdc = previousClipPosition.xy /
                       max(abs(previousClipPosition.w), 0.000001);

    outWorldPosition = worldPosition.xyz;
    outWorldNormal = normalize(transpose(inverse(mat3(inModel))) * vec3(0.0, 1.0, 0.0));
    outUv = inUv;
    outColor = inColor;
    outMaterial = inMaterial;
    outMotionVector = (currentNdc - previousNdc) * 0.5;
}

