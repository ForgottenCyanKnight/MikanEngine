#version 450
#extension GL_GOOGLE_include_directive : require
#include "pmx_uniform.glsl"
layout(location=0) in vec3 inPosition;
layout(location=1) in vec4 inNormal;
layout(location=2) in vec2 inUV;
layout(location=3) in vec3 inPmxExtra; // additional UV1.xy, vertex edge factor
layout(location=0) out vec3 viewPosition;
layout(location=1) out vec3 viewNormal;
layout(location=2) out vec2 uv;
layout(location=3) out vec2 additionalUV;
layout(location=4) out vec3 worldNormal;
layout(location=5) out vec2 motion;
layout(location=6) out vec3 worldPosition;
void main() {
    vec4 position = pmx.modelView * vec4(inPosition,1.0);
    viewPosition = position.xyz;
    viewNormal = transpose(inverse(mat3(pmx.modelView))) * inNormal.xyz;
    uv = inUV;
    additionalUV = inPmxExtra.xy;
    worldNormal = transpose(inverse(mat3(pmx.model))) * inNormal.xyz;
    worldPosition = (pmx.model * vec4(inPosition,1.0)).xyz;
    gl_Position = pmx.projection * position;
    vec4 previous = pmx.previousMVP * vec4(inPosition,1.0);
    motion = (gl_Position.xy/gl_Position.w - previous.xy/previous.w) * 0.5;
    gl_Position.xy += pmx.jitter.xy * gl_Position.w;
}
