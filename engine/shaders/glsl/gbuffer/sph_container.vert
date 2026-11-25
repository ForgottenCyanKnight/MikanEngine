#version 450

layout(push_constant) uniform ContainerPushConstants {
    mat4 viewProj;
    mat4 model;
} params;

layout(location = 0) in vec3 inPosition;

void main() {
    gl_Position = params.viewProj * params.model * vec4(inPosition, 1.0);
}
