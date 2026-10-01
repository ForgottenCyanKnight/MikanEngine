#version 450
#extension GL_GOOGLE_include_directive : require
#include "pmx_uniform.glsl"
layout(set=0,binding=1) uniform sampler2D diffuseTexture;
layout(set=0,binding=3) uniform sampler2D sphereTexture;
layout(set=0,binding=5) uniform sampler2D normalTexture;
layout(location=1) in vec3 viewNormal;
layout(location=2) in vec2 uv;
layout(location=3) in vec2 additionalUV;
layout(location=4) in vec3 worldNormal;
layout(location=5) in vec2 motion;
layout(location=6) in vec3 worldPosition;
layout(location=0) out vec4 outColor;
layout(location=1) out vec4 outNormal;
layout(location=2) out vec4 outMaterial;
layout(location=3) out vec2 outMotion;
// Disabled on independentBlend devices; defined for the identical-state fallback.
layout(location=4) out vec4 outHiZOccluder;
layout(location=5) out vec4 outUnusedComposite;
void main() {
    // Raw material data only: the common composite owns sun, shadows and IBL.
    vec4 color = pmx.diffuse;
    if (pmx.modes.x != 0) color *= pmxTextureFactors(texture(diffuseTexture,vec2(uv.x,1.0-uv.y)),pmx.textureMul,pmx.textureAdd);
    if (color.a <= 0.0) discard;
    if (pmx.modes.z == 1 || pmx.modes.z == 3) {
        vec3 normal = normalize(viewNormal);
        vec2 sphereUV = normal.xy*0.5+0.5;
        if (pmx.modes.z == 3) sphereUV = vec2(additionalUV.x,1.0-additionalUV.y);
        color.rgb *= pmxTextureFactors(texture(sphereTexture,sphereUV),pmx.sphereMul,pmx.sphereAdd).rgb;
    }
    outColor = color;
    if (pmx.viewportEdge.w > 0.5) outColor.a = 1.0;
    vec3 n = normalize(worldNormal);
    if (!gl_FrontFacing) n = -n;
    if (pmx.shadowParams.w > 0.5) {
        vec2 texUV=vec2(uv.x,1.0-uv.y);
        vec3 dp1=dFdx(worldPosition), dp2=dFdy(worldPosition);
        vec2 duv1=dFdx(texUV), duv2=dFdy(texUV);
        // Normalize the common derivative scales before testing degeneracy.
        // As the camera approaches, both derivatives shrink: an absolute
        // threshold on their product incorrectly disables valid normal maps.
        float positionScale=max(length(dp1),length(dp2));
        float uvScale=max(length(duv1),length(duv2));
        dp1/=max(positionScale,1e-20); dp2/=max(positionScale,1e-20);
        duv1/=max(uvScale,1e-20); duv2/=max(uvScale,1e-20);
        vec3 p2=cross(dp2,n), p1=cross(n,dp1);
        vec3 t=p2*duv1.x+p1*duv2.x;
        vec3 b=p2*duv1.y+p1*duv2.y;
        float basisLength=max(dot(t,t),dot(b,b));
        if (basisLength > 1e-12) {
            float scale=inversesqrt(basisLength);
            vec3 mapped=texture(normalTexture,texUV).xyz*2.0-1.0;
            n=normalize(mat3(t*scale,b*scale,n)*mapped);
        }
    }
    n /= abs(n.x)+abs(n.y)+abs(n.z);
    vec2 encoded = n.xy;
    if (n.z < 0.0) encoded = (1.0-abs(n.yx))*mix(vec2(-1.0),vec2(1.0),greaterThanEqual(n.xy,vec2(0.0)));
    outNormal = vec4(encoded,0.0,0.0);
    float roughness = pmx.pbrMaterial.y >= 0.0 ? pmx.pbrMaterial.y
        : sqrt(2.0/(max(pmx.specularPower.w,0.0)+2.0));
    outMaterial = vec4(clamp(pmx.pbrMaterial.x,0.0,1.0),clamp(roughness,0.04,1.0),
        clamp(pmx.pbrMaterial.z,0.0,1.0),clamp(pmx.pbrMaterial.w,0.0,1.0));
    outMotion = motion;
    outHiZOccluder = vec4(1.0);
    outUnusedComposite = vec4(0.0);
}
