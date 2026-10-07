// Direct sunlight via next event estimation, sampled over the solar disk.
// The atmosphere sky panorama has scattered radiance, not an analytic disk;
// solar NEE therefore does not duplicate the existing sky-miss contribution.
layout(binding=18) uniform sampler2D sunTransmittanceLUT;
const float SUN_ANGULAR_RADIUS=0.00465047; // radians, approximately 0.266 degrees
const vec3 SUN_SOLAR_IRRADIANCE=vec3(1.474,1.8504,2.3612);
vec2 sunTransmittanceUV(float altitude,float mu){
    const float bottom=6371000.0,top=6431000.0;
    float r=clamp(bottom+altitude,bottom,top-1.0);
    float H=sqrt(top*top-bottom*bottom),rho=sqrt(max(r*r-bottom*bottom,0.0));
    float d=max(-r*mu+sqrt(max(r*r*(mu*mu-1.0)+top*top,0.0)),0.0);
    float xMu=(d-(top-r))/max(rho+H-(top-r),1.0);
    ivec2 size=textureSize(sunTransmittanceLUT,0);
    vec2 texel=1.0/vec2(size);
    return clamp(vec2(xMu,rho/H),vec2(0),vec2(1))*(1.0-texel)+.5*texel;
}
vec3 sunNormalIrradiance(vec3 direction){
    vec3 irradiance=max(pc.light.rgb,vec3(0));
    if(pc.sun.w<.5)return irradiance; // existing non-physical directional light
    // Same altitude datum as the sky (world origin is sea level minus 200m).
    // Atmosphere attenuation/exposure are lighting terms, never albedo edits.
    float altitude=max(pc.eye.w,0.0);
    float r=6371000.0+altitude;
    float horizon=-sqrt(max(1.0-(6371000.0*6371000.0)/(r*r),0.0));
    if(direction.y<=horizon)return vec3(0); // Earth blocks the solar disk
    vec3 transmittance=textureLod(sunTransmittanceLUT,sunTransmittanceUV(altitude,direction.y),0).rgb;
    return irradiance*SUN_SOLAR_IRRADIANCE*transmittance*pc.extent.w;
}
vec3 sampleSunIrradiance(Surface surface,inout RaySampleState state,bool hardShadow){
    float lengthSquared=dot(pc.sun.xyz,pc.sun.xyz);
    if(lengthSquared<1e-12 || all(lessThanEqual(pc.light.rgb,vec3(0))))return vec3(0);
    vec3 axis=pc.sun.xyz*inversesqrt(lengthSquared);
    float cosRadius=cos(SUN_ANGULAR_RADIUS);
    vec2 u=nextRaySample(state);
    float cosTheta=1.0-u.x*(1.0-cosRadius);
    float sinTheta=sqrt(max(1.0-cosTheta*cosTheta,0.0)),phi=2.0*RT_PI*u.y;
    vec3 helper=abs(axis.z)<.999?vec3(0,0,1):vec3(1,0,0);
    vec3 tangent=normalize(cross(helper,axis)),bitangent=cross(axis,tangent);
    vec3 direction=normalize(axis*cosTheta+sinTheta*(tangent*cos(phi)+bitangent*sin(phi)));
    if(hardShadow)direction=axis; // Deterministic primary visibility; keep RNG dimensions unchanged.
    float cosine=max(dot(surface.normal,direction),0.0);
    if(cosine<=0.0 || dot(surface.geometricNormal,direction)<=0.0)return vec3(0);
    vec3 irradiance=sunNormalIrradiance(direction);
    if(all(lessThanEqual(irradiance,vec3(0))))return vec3(0);
    if(traceOccluded(surface.position+surface.geometricNormal*max(rayOffset(surface.position),amdRobustSunOrigin?.01:.001),direction,100000,2u))return vec3(0);
    // Uniform-cone PDF = 1/[2*pi*(1-cosRadius)]. Disk radiance is E /
    // [pi*sin(radius)^2]. Their ratio simplifies to E*2/(1+cosRadius):
    // normalize energy without huge solar radiance or a tiny PDF division.
    // The Lambertian 1/pi and material factor are applied by the caller.
    return irradiance*cosine*(hardShadow?1.0:(2.0/(1.0+cosRadius)));
}

vec3 sampleSunIrradiance(Surface surface,inout RaySampleState state){
    return sampleSunIrradiance(surface,state,true);
}

// Voxel-style deterministic sunlight: one center-direction visibility query.
// RNG advancement remains one proposal, preserving other path dimensions.
vec3 samplePrimarySunIrradiance(Surface surface,inout RaySampleState state){
    return sampleSunIrradiance(surface,state,true);
}