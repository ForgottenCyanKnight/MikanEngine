#version 450

layout(set = 0, binding = 0) uniform ParticleUniformData {
    mat4 viewProj;
    vec4 cameraRight;
    vec4 cameraUp;
    vec4 taaJitter;
} ubo;

layout(location = 0) in vec4 inPositionSize;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec4 inRotationBlend;

layout(location = 0) out vec2 outUv;
layout(location = 1) out vec4 outColor;

void main() {
    // Use an explicit triangle list so each particle is a complete quad even
    // on drivers/passes where a strip's shared diagonal is not interpolated
    // consistently with the procedural circular mask.
    const vec2 corners[6] = vec2[](
        vec2(-0.5, -0.5),
        vec2( 0.5, -0.5),
        vec2( 0.5,  0.5),
        vec2( 0.5,  0.5),
        vec2(-0.5,  0.5),
        vec2(-0.5, -0.5));
    const vec2 uvs[6] = vec2[](
        vec2(0.0, 0.0),
        vec2(1.0, 0.0),
        vec2(1.0, 1.0),
        vec2(1.0, 1.0),
        vec2(0.0, 1.0),
        vec2(0.0, 0.0));

    const uint cornerIndex = uint(gl_VertexIndex) % 6u;
    const vec2 corner = corners[cornerIndex];
    const float angle = inRotationBlend.x;
    const float c = cos(angle);
    const float s = sin(angle);
    const vec2 rotated = vec2(c * corner.x - s * corner.y,
                              s * corner.x + c * corner.y);

    const vec3 worldPosition = inPositionSize.xyz +
        (ubo.cameraRight.xyz * rotated.x + ubo.cameraUp.xyz * rotated.y) *
        inPositionSize.w;
    gl_Position = ubo.viewProj * vec4(worldPosition, 1.0);
    gl_Position.xy += ubo.taaJitter.xy * gl_Position.w;
    outUv = uvs[cornerIndex];
    outColor = inColor;
}
