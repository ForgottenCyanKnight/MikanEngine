#version 450

layout(location = 0) in vec3 instanceColor;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(instanceColor, 1.0);
}
