#version 450
#extension GL_GOOGLE_include_directive : require
#include "../gbuffer/voxel/voxel_world_face.glsl"

layout(push_constant) uniform WaterPushConstants {
    mat4 projView;
    vec4 cameraPosition;
} pc;

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aFacePos;
layout(location = 2) in uint aPackedData;

layout(location = 0) out vec3 outWorldPosition;

void main() {
    int face = int(aPackedData & 0xFFu);
    vec3 position = aPos * vec3(unpackSizeFromPacked(aPackedData), 1.0);
    outWorldPosition = aFacePos + rotatePosition(face, position);
    gl_Position = pc.projView * vec4(outWorldPosition, 1.0);
}
