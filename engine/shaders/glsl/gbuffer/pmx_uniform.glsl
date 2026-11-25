// PMX shading equations adapted from Saba (MIT; see textures/mmd/SABA-LICENSE.txt).
layout(set=0,binding=0,std140) uniform PmxDraw {
    mat4 projection;
    mat4 modelView;
    mat4 model;
    mat4 previousMVP;
    vec4 diffuse;
    vec4 specularPower;
    vec4 ambient;
    vec4 edgeColor;
    vec4 lightDirection;
    vec4 lightColor;
    vec4 viewportEdge; // viewport size, edge size, opaque material fallback
    vec4 jitter;
    ivec4 modes; // diffuse texture, toon texture, sphere mode, receive shadow
    mat4 shadowMatrices[4];
    vec4 shadowSplitDepths;
    vec4 shadowParams; // enabled, PCF radius, probe-view selection, unused
    vec4 pbrMaterial; // metallic, roughness, AO, emissive; negative roughness uses PMX exponent
    vec4 textureMul;
    vec4 textureAdd;
    vec4 sphereMul;
    vec4 sphereAdd;
} pmx;
// The factor's fourth component is Saba's RGB effect strength, not texture opacity.
vec4 pmxTextureFactors(vec4 sampleColor,vec4 mulFactor,vec4 addFactor) {
    vec3 color=mix(vec3(1.0),sampleColor.rgb*mulFactor.rgb,mulFactor.a);
    color=clamp(color+(color-vec3(1.0))*addFactor.a,vec3(0.0),vec3(1.0))+addFactor.rgb;
    return vec4(color,sampleColor.a);
}
