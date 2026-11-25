#version 450

layout(location = 0) in vec3 aPos;       // 顶点位置
layout(location = 1) in vec3 aColor;     // 顶点颜色

layout(push_constant) uniform UniformBufferObject {
    mat4 view;
    mat4 proj;
} ubo;

layout(location = 0) out vec3 vertexColor;

void main() {
    gl_Position = ubo.proj * ubo.view * vec4(aPos, 1.0);
    vertexColor = aColor;
}
