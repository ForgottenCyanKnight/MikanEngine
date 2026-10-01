#version 450
#extension GL_GOOGLE_include_directive : require
#include "pmx_uniform.glsl"
layout(set=0,binding=1) uniform sampler2D diffuseTexture;
layout(set=0,binding=3) uniform sampler2D sphereTexture;
layout(location=1) in vec3 viewNormal;
layout(location=2) in vec2 uv;
layout(location=0) out vec4 outColor;
vec3 srgbToLinear(vec3 c) {
    return mix(c/12.92,pow((c+0.055)/1.055,vec3(2.4)),step(vec3(0.04045),c));
}
void main() {
    float alpha = pmx.diffuse.a;
    if (pmx.modes.x != 0) alpha *= texture(diffuseTexture,vec2(uv.x,1.0-uv.y)).a;
    if (alpha <= 0.0) discard;
    vec2 sphereUV = normalize(viewNormal).xy*0.5+0.5;
    // Authored additive sphere highlight stays separate from PBR albedo.
    outColor = vec4(srgbToLinear(pmxTextureFactors(texture(sphereTexture,sphereUV),pmx.sphereMul,pmx.sphereAdd).rgb),alpha);
}
