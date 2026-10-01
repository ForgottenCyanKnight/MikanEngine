#version 450
#extension GL_GOOGLE_include_directive : require
#include "pmx_uniform.glsl"
layout(location=0) in vec3 inPosition;
layout(location=1) in vec4 inNormal;
layout(location=3) in vec3 inPmxExtra;
void main() {
    // Optional NPR outline: disabled by default in the shared PBR environment.
    // Set to 1.75 to restore the thicker outline.
    const float edgeWidthScale = 0.0;
    if (edgeWidthScale <= 0.0) {
        // Collapse the shell so depth bias cannot expose unexpanded back faces.
        gl_Position = vec4(0.0,0.0,0.0,1.0);
        return;
    }
    vec4 position = pmx.modelView * vec4(inPosition,1.0);
    vec3 normal = transpose(inverse(mat3(pmx.modelView))) * inNormal.xyz;
    vec2 screenNormal = (pmx.projection * vec4(normal,0.0)).xy;
    float magnitude = length(screenNormal);
    screenNormal = magnitude > 1e-6 ? screenNormal/magnitude : vec2(0.0);
    gl_Position = pmx.projection * position;
    // Keep authored material/vertex ratios, with a modest screen-space boost.
    float authoredWidth = pmx.viewportEdge.z * inPmxExtra.z;
    // Subpixel shells lose coverage along diagonals; keep enabled edges visible.
    float edgeWidth = authoredWidth > 0.0 ? max(authoredWidth * edgeWidthScale,1.25) : 0.0;
    gl_Position.xy += (screenNormal * edgeWidth
        / (pmx.viewportEdge.xy * 0.5) + pmx.jitter.xy) * gl_Position.w;
}
