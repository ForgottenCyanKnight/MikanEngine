#version 450

layout(location = 0) in vec3 inTexCoords;

layout(binding = 0) uniform samplerCube skybox;

layout(push_constant) uniform PushConstants {
    mat4 view;
    mat4 proj;
    vec4 tintAndIntensity;
} pc;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 tex = texture(skybox, inTexCoords);
    // tint.rgb 颜色调制 + tintAndIntensity.a 亮度
    outColor = vec4(tex.rgb * pc.tintAndIntensity.rgb, 1.0) * pc.tintAndIntensity.a;
}
