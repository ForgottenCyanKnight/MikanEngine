#version 450

layout(location = 0) in vec4 inPosition;
layout(location = 1) in vec4 inColor;

layout(location = 0) out vec4 vertexColor;

layout(push_constant) uniform ClothPushConstants {
    mat4 viewProj;
    mat4 model;
} pushConstants;

void main() {
    gl_Position = pushConstants.viewProj * pushConstants.model * inPosition;
    vertexColor = inColor;
}
