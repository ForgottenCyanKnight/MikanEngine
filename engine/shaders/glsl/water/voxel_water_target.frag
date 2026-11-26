#version 450

precision highp float;

layout(push_constant) uniform WaterPushConstants {
    mat4 projView;
    vec4 cameraPosition;
} pc;

layout(location = 0) in vec3 inWorldPosition;
layout(location = 0) out vec4 outTarget;

void main() {
    float distanceToCamera = distance(inWorldPosition, pc.cameraPosition.xyz);
    // WaterTargetRT uses an octahedral normal in BA. World up (0, 1, 0)
    // encodes as (0, 1) and gives the compositor a flat reflection plane.
    outTarget = vec4(1.0, distanceToCamera, 0.0, 1.0);
}
