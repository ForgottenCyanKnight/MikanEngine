#version 450
#extension GL_GOOGLE_include_directive : require
#include "pmx_uniform.glsl"
layout(location=0) out vec4 outColor;
void main() {
    if (pmx.edgeColor.a <= 0.0) discard;
    vec3 c = max(pmx.edgeColor.rgb,vec3(0.0));
    outColor = vec4(mix(c/12.92,pow((c+0.055)/1.055,vec3(2.4)),step(vec3(0.04045),c)),pmx.edgeColor.a);
}
