#version 450

layout(location = 0) in vec3 inPosition;

layout(push_constant) uniform PushConstants {
    mat4 view;
    mat4 proj;
    vec4 tintAndIntensity;
} pc;

layout(location = 0) out vec3 outTexCoords;

void main() {
    outTexCoords = inPosition;
    vec4 pos = pc.proj * pc.view * vec4(inPosition, 1.0);
    gl_Position = pos.xyww;
}
