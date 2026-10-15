// Engine integration of Falcor ReSTIR PT Enhanced; upstream BSD-3-Clause notice below.
// Incorporated NVIDIA Falcor algorithms retain this BSD-3-Clause notice.
/***************************************************************************
 # Copyright (c) 2015-26, NVIDIA CORPORATION. All rights reserved.
 #
 # Redistribution and use in source and binary forms, with or without
 # modification, are permitted provided that the following conditions
 # are met:
 #  * Redistributions of source code must retain the above copyright
 #    notice, this list of conditions and the following disclaimer.
 #  * Redistributions in binary form must reproduce the above copyright
 #    notice, this list of conditions and the following disclaimer in the
 #    documentation and/or other materials provided with the distribution.
 #  * Neither the name of NVIDIA CORPORATION nor the names of its
 #    contributors may be used to endorse or promote products derived
 #    from this software without specific prior written permission.
 #
 # THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS "AS IS" AND ANY
 # EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 # IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 # PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 # CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 # EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 # PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 # PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 # OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 # (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 # OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 **************************************************************************/
// Enhanced point-profile adapter for Mikan's opaque diffuse/GGX/delta transport.
// Upstream: NVIDIA Falcor 9.0, 759aad033ff610fb0d82c74f7e0a508d0096d5f2.
// Source scene changes invalidate the accepted bank before any cached hit is read.
const uint E_INVALID=0xffffffffu,E_SKY=0u,E_SUN=1u,E_EMITTER=2u;
const uint E_NEE=1u<<16u,E_DELTA_BEFORE=1u<<20u,E_DELTA_AFTER=1u<<26u;
const float E_PSEUDO_PDF=.07957747154594767;
uint eLength(PTReservoir r){return r.path.x&255u;}
uint eRcLength(PTReservoir r){return (r.path.x>>8u)&255u;}
uint eLightType(PTReservoir r){return (r.path.x>>18u)&3u;}
bool eNee(PTReservoir r){return (r.path.x&E_NEE)!=0u;}
float eRandom(inout uint rng){rng=1664525u*rng+1013904223u;return float(rng>>8u)*(1.0/16777216.0);}
vec2 eRandom2(inout uint rng){float x=eRandom(rng);return vec2(x,eRandom(rng));}
uint eMorton(uvec2 v){
    v&=uvec2(65535u);v=(v|(v<<8u))&uvec2(0x00ff00ffu);
    v=(v|(v<<4u))&uvec2(0x0f0f0f0fu);v=(v|(v<<2u))&uvec2(0x33333333u);
    v=(v|(v<<1u))&uvec2(0x55555555u);return v.x|(v.y<<1u);
}
uint eSeed(uvec2 p,uint sequence){
    uint a=eMorton(p),b=sequence,sum=0u;
    for(uint i=0u;i<16u;++i){sum+=0x9e3779b9u;
        a+=((b<<4u)+0xa341316cu)^(b+sum)^((b>>5u)+0xc8013ea4u);
        b+=((a<<4u)+0xad90777du)^(a+sum)^((a>>5u)+0x7e95761eu);}
    return a;
}
float eThreshold(inout uint rng,float footprint){
    vec2 u=eRandom2(rng);
    return footprint*.0002*(1.0+.2*sqrt(-2.0*log(max(u.x,1e-30)))*cos(2.0*RT_PI*u.y));
}
float eFootprint(Surface s,vec3 view,vec3 eye){
    vec3 d=s.position-eye;return dot(d,d)/max(E_PSEUDO_PDF*abs(dot(view,s.geometricNormal)),1e-30);
}
bool eRough(float pdf){return pdf<=25.0;}
vec3 eSpawn(Surface s,vec3 direction){
    return s.position+s.geometricNormal*(dot(direction,s.geometricNormal)>=0.0?1.0:-1.0)*rayOffset(s.position);
}
uvec4 eHit(Surface s){return uvec4(s.hitIdentity.xy,s.hitPrimitive,floatBitsToUint(s.hitBary.x));}
bool eLoad(uvec4 hit,float baryY,vec3 position,vec3 view,out Surface s){
    if(hit.x==E_INVALID||hit.x>=instances.length())return false;
    return decodeSurface(hit.x,hit.y,hit.z,vec2(uintBitsToFloat(hit.w),baryY),
        mat4x3(primaryTransforms[hit.x]),position,-view,0.0,s);
}
void eSetRc(inout PTReservoir r,Surface s,float geometry){
    r.rcHit=eHit(s);r.path.w=floatBitsToUint(s.hitBary.y);
    r.rcPosition=vec4(s.position,0);r.incidentGeometry.w=geometry;
}
vec3 eScatter(Surface s,vec3 view,inout uint rng,out float pdf,out bool delta){
    float choose=eRandom(rng);vec2 u=eRandom2(rng);delta=s.mirror;
    if(delta){pdf=1.0;return normalize(reflect(-view,s.normal));}
    vec3 t=normalize(cross(abs(s.normal.z)<.999?vec3(0,0,1):vec3(1,0,0),s.normal)),b=cross(s.normal,t),direction;
    if(choose>=ptSpecularProbability(s)){
        float phi=2.0*RT_PI*u.y;
        direction=normalize(t*sqrt(u.x)*cos(phi)+b*sqrt(u.x)*sin(phi)+s.normal*sqrt(1.0-u.x));
    }else{
        vec3 v=vec3(dot(view,t),dot(view,b),max(dot(view,s.normal),1e-6));
        vec3 h=sampleGGXVNDF(v,max(s.roughness*s.roughness,1e-4),u.x,u.y);
        direction=normalize(reflect(-view,normalize(t*h.x+b*h.y+s.normal*h.z)));
    }
    pdf=ptPdf(s,view,direction);return direction;
}
void eWeights(Surface s,vec3 view,vec3 direction,float pdf,bool first,
    inout vec3 d,inout vec3 sp){
    if(s.mirror){d*=s.albedo;sp*=s.albedo;return;}
    if(first){d=vec3(max(dot(s.normal,direction),0.0)/(RT_PI*pdf));
        sp=s.specularEnabled?ggxWeight(s,view,direction)*ggxPdf(s,view,direction)/(pdf*specularMaterialFactor(s,-view)):vec3(0);
    }else{vec3 w=ptBsdfCos(s,view,direction)/pdf;d*=w;sp*=w;}
}
void eResponse(Surface s,vec3 view,vec3 direction,bool first,inout vec3 d,inout vec3 sp){
    if(first){d=vec3(max(dot(s.normal,direction),0.0)/RT_PI);
        sp=s.specularEnabled?ggxWeight(s,view,direction)*ggxPdf(s,view,direction)/specularMaterialFactor(s,-view):vec3(0);
    }else{vec3 response=ptBsdfCos(s,view,direction);d*=response;sp*=response;}
}
struct ELight {Surface hit;vec3 direction;vec3 Le;float pdf;float geometry;uint type;bool finite;};
bool eSunEnabled(){
    return dot(pc.sun.xyz,pc.sun.xyz)>1e-12&&any(greaterThan(sunNormalIrradiance(normalize(pc.sun.xyz)),vec3(0)));
}
// Every light sample consumes five random draws, even when black or occluded.
// Prefix retrace can therefore skip NEE without performing shadow queries.
ELight eSampleLight(Surface s,inout uint rng){
    float branch=eRandom(rng),selector=eRandom(rng);eRandom(rng);vec2 u=eRandom2(rng);
    ELight l;l.direction=vec3(0,1,0);l.Le=vec3(0);l.pdf=0;l.geometry=1;l.type=E_SUN;l.finite=false;
    uint count=uint(emissiveLights[0].x);
    bool sun=eSunEnabled();
    float localProbability=sun?(count>0u?.5:0.0):1.0;
    if(branch>=localProbability){
        if(!sun)return l;
        vec3 axis=normalize(pc.sun.xyz),t=normalize(cross(abs(axis.z)<.999?vec3(0,0,1):vec3(1,0,0),axis));
        float c=cos(SUN_ANGULAR_RADIUS),ct=1.0-u.x*(1.0-c),st=sqrt(max(1.0-ct*ct,0.0)),phi=2.0*RT_PI*u.y;
        l.direction=normalize(axis*ct+st*(t*cos(phi)+cross(axis,t)*sin(phi)));
        l.pdf=(1.0-localProbability)/(2.0*RT_PI*(1.0-c));
        l.Le=sunNormalIrradiance(l.direction)/(RT_PI*(1.0-c*c));
        if(dot(s.geometricNormal,l.direction)<=0.0||traceOccluded(eSpawn(s,l.direction),l.direction,100000,2u))l.Le=vec3(0);
        return l;
    }
    l.type=E_EMITTER;if(count==0u)return l;
    float probability;uint chosen;
    chosen=selectEmitterCdf(selector,probability);
    uint at=1u+chosen*4u;vec3 p0=emissiveLights[at+1u].xyz,p1=emissiveLights[at+2u].xyz,p2=emissiveLights[at+3u].xyz;
    float su=sqrt(u.x);vec3 point=p0*(1.0-su)+p1*(su*(1.0-u.y))+p2*(su*u.y);
    vec3 displacement=point-s.position;float d2=dot(displacement,displacement);
    if(d2<=1e-10)return l;
    l.direction=displacement*inversesqrt(d2);l.geometry=abs(dot(normalize(cross(p1-p0,p2-p0)),-l.direction))/d2;
    if(l.geometry<=0.0||dot(s.geometricNormal,l.direction)<=0.0)return l;
    l.pdf=localProbability*probability/(emissiveLights[at].w*l.geometry);
    // Capture the actual immutable surface identity, including procedural-quad UVs.
    vec3 begin=eSpawn(s,l.direction),segment=point-begin;float distanceToLight=length(segment);
    if(!traceSurface(begin,segment/distanceToLight,distanceToLight+2.0*rayOffset(point),l.hit))return l;
    if(distance(l.hit.position,point)>2.0*rayOffset(point))return l;
    l.finite=true;l.Le=emissiveLights[at].rgb;return l;
}
float eLightPdf(Surface s,Surface hit){
    float localProbability=eSunEnabled()?.5:1.0;
    uint i=emitterForHit(hit.hitIdentity);if(i==E_INVALID)return 0.0;
    uint at=1u+i*4u;vec3 displacement=hit.position-s.position;float d2=dot(displacement,displacement);
    vec3 normal=normalize(cross(emissiveLights[at+2u].xyz-emissiveLights[at+1u].xyz,emissiveLights[at+3u].xyz-emissiveLights[at+1u].xyz));
    float g=abs(dot(normal,-normalize(displacement)))/max(d2,1e-30);
    float p=emissiveLights[3u+i*4u].w;
    return g>0.0?localProbability*p/(emissiveLights[at].w*g):0.0;
}
float eEnvironmentPdf(uint type,vec3 direction){
    if(type!=E_SUN||!eSunEnabled()||dot(normalize(pc.sun.xyz),direction)<cos(SUN_ANGULAR_RADIUS))return 0.0;
    float p=uint(emissiveLights[0].x)>0u?.5:1.0;
    return p/(2.0*RT_PI*(1.0-cos(SUN_ANGULAR_RADIUS)));
}
vec3 eEnvironment(uint type,vec3 direction){
    if(type==E_SKY)return skyBackground(direction);
    if(eEnvironmentPdf(type,direction)<=0.0)return vec3(0);
    float c=cos(SUN_ANGULAR_RADIUS);return sunNormalIrradiance(direction)/(RT_PI*(1.0-c*c));
}
// Select terminal contributions in PSS; store the selected integrand in its mixed chart.
bool eAdd(inout PTReservoir result,PTReservoir candidate,vec3 pssD,vec3 pssS,float u,inout float sum){
    float w=diLuminance(pssD+pssS);if(w<=0.0||isnan(w)||isinf(w))return false;
    sum+=w;if(u*sum>=w)return false;
    result=candidate;return true;
}
#include "vox_rt_enhanced_path.glsl"
#include "vox_rt_enhanced_shift.glsl"
#include "vox_rt_enhanced_dispatch.glsl"
