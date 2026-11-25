#version 450

layout(location = 0) in vec3 aPos;       // 顶点位置（单位立方体）
layout(location = 1) in vec3 position;   // 实例位置
layout(location = 2) in vec3 size;       // 实例尺寸
layout(location = 3) in vec4 rotation;   // 实例旋转（四元数）
layout(location = 4) in vec3 color;      // 实例颜色

layout(push_constant) uniform UniformBufferObject {
    mat4 view;
    mat4 proj;
} ubo;

vec3 rotateByQuaternion(vec3 v, vec4 q) {
    vec3 qvec = q.xyz;
    vec3 uv = cross(qvec, v);
    vec3 uuv = cross(qvec, uv);
    return v + ((uv * q.w) + uuv) * 2.0;
}

layout(location = 0) out vec3 instanceColor;

void main() {
    vec3 scaledPos = aPos * size;        // 应用尺寸缩放
    vec3 rotatedPos = rotateByQuaternion(scaledPos, rotation);  // 应用旋转
    vec3 worldPos = rotatedPos + position;  // 应用位置
    gl_Position = ubo.proj * ubo.view * vec4(worldPos, 1.0);
    instanceColor = color;
}
