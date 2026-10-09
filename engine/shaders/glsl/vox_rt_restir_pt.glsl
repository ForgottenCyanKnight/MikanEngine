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


// ReSTIR over complete path-sample primary sample space (PSS).
// A seed is replayed through current geometry/materials/visibility, never a cached
// outgoing-radiance suffix. PSS identity shifts have Jacobian 1. This is not
// Falcor/Prime Enhanced's Hybrid reconnection or Compact retrace implementation.
// Pairwise MIS equations follow Prime e1917423 / Falcor 759aad03.
// See docs/rendering/ReSTIR_PT.md for attribution, contracts and limitations.
struct PTReservoir {
    vec4 diffuseWeight;  // demodulated diffuse integrand; finalized UCW
    vec4 specularDistance;
    vec4 receiverM;
    uvec4 seedPixelNormal; // seed, receiving pixel xy, geometric normal
};
layout(std430,binding=42) readonly buffer PTPrevious {PTReservoir ptPrevious[];};
layout(std430,binding=43) buffer PTFresh {PTReservoir ptFresh[];};
layout(std430,binding=44) buffer PTTemporal {PTReservoir ptTemporal[];};
layout(std430,binding=45) buffer PTFinal {PTReservoir ptFinal[];};
PTReservoir ptEmpty(){return PTReservoir(vec4(0),vec4(0),vec4(0),uvec4(0));}
float ptTarget(PTReservoir r){return diLuminance(r.diffuseWeight.rgb+r.specularDistance.rgb);}
bool ptFinite(vec3 v){return !any(isnan(v))&&!any(isinf(v))&&all(greaterThanEqual(v,vec3(0)));}
float ptSpecularProbability(Surface s){
    if(!s.specularEnabled)return 0.0;
    float diffuse=diLuminance(s.albedo*(1.0-s.metallic));
    float specular=diLuminance(mix(vec3(.04),s.albedo,s.metallic));
    return diffuse<=0.0?1.0:clamp(specular/(diffuse+specular),.1,.9);
}
float ptPdf(Surface s,vec3 V,vec3 L){
    if(dot(s.geometricNormal,L)<=0.0)return 0.0;
    float ps=ptSpecularProbability(s);
    return (1.0-ps)*max(dot(s.normal,L),0.0)/RT_PI+ps*ggxPdf(s,V,L);
}
vec3 ptBsdfCos(Surface s,vec3 V,vec3 L){
    if(dot(s.geometricNormal,L)<=0.0)return vec3(0);
    return s.albedo*(1.0-s.metallic)*max(dot(s.normal,L),0.0)/RT_PI+
        (s.specularEnabled?ggxWeight(s,V,L)*ggxPdf(s,V,L):vec3(0));
}
vec3 ptDirection(Surface s,vec3 V,inout RaySampleState rng){
    float choice=randomFloat(rng.fallback);
    if(choice>=ptSpecularProbability(s))return diffuseDirection(s.normal,rng);
    vec3 T=normalize(cross(abs(s.normal.z)<.999?vec3(0,0,1):vec3(1,0,0),s.normal));
    vec3 B=cross(s.normal,T);
    vec3 Ve=vec3(dot(V,T),dot(V,B),max(dot(V,s.normal),1e-6));
    vec2 u=nextRaySample(rng);
    vec3 h=sampleGGXVNDF(Ve,max(s.roughness*s.roughness,1e-4),u.x,u.y);
    return normalize(reflect(-V,normalize(T*h.x+B*h.y+s.normal*h.z)));
}
// Sample one solar direction for both BSDF lobes and the visibility query.
vec3 ptSun(Surface s,inout RaySampleState rng,out vec3 direction){
    direction=vec3(0,1,0);
    if(dot(pc.sun.xyz,pc.sun.xyz)<1e-12)return vec3(0);
    vec3 axis=normalize(pc.sun.xyz);
    vec2 u=nextRaySample(rng);float c=cos(SUN_ANGULAR_RADIUS);
    float ct=1.0-u.x*(1.0-c),st=sqrt(max(1.0-ct*ct,0.0)),phi=2.0*RT_PI*u.y;
    vec3 t=normalize(cross(abs(axis.z)<.999?vec3(0,0,1):vec3(1,0,0),axis));
    direction=normalize(axis*ct+st*(t*cos(phi)+cross(axis,t)*sin(phi)));
    float cosine=max(dot(s.normal,direction),0.0);
    if(cosine<=0.0||dot(s.geometricNormal,direction)<=0.0)return vec3(0);
    vec3 e=sunNormalIrradiance(direction)*(2.0/(1.0+c));
    if(all(lessThanEqual(e,vec3(0))))return vec3(0);
    if(traceOccluded(s.position+s.geometricNormal*max(rayOffset(s.position),amdRobustSunOrigin?.01:.001),direction,100000,2u))return vec3(0);
    return e*cosine;
}
PTReservoir ptReplay(Surface receiver,vec3 incoming,uint seed,ivec2 pixel,int budget){
    PTReservoir r=ptEmpty();
    r.receiverM=vec4(receiver.position,1);
    r.seedPixelNormal=uvec4(seed,uvec2(pixel),giPackNormal(receiver.geometricNormal));
    // PT mode forces white PRNG in nextRaySample; pixel/frame never alter replay.
    RaySampleState rng=RaySampleState(uvec2(0),0u,0u,seed|1u);
    Surface s=receiver;
    vec3 V=-incoming,wd=vec3(0),ws=vec3(0),D=vec3(0),S=vec3(0);
    vec3 specFactor=specularMaterialFactor(receiver,incoming);
    float firstDistance=65504.0;
    for(int bounce=0;bounce<=MAX_PATH_BOUNCES;++bounce){
        if(bounce>budget)break;
        bool continuePath=bounce<budget;
        if(!s.mirror){
            // Solar disk is a separate domain from the scattered sky panorama.
            vec3 axis;vec3 sun=ptSun(s,rng,axis);
            float cosine=max(dot(s.normal,axis),0.0);
            vec3 sunDiffuse=sun/RT_PI;
            vec3 sunSpec=s.specularEnabled&&cosine>0.0?
                sun*ggxWeight(s,V,axis)*ggxPdf(s,V,axis)/cosine:vec3(0);
            EmitterSample light=sampleEmitter(s,rng);
            vec3 ld=vec3(0),ls=vec3(0);
            if(light.pdf>0.0){
                float mis=continuePath?powerHeuristic(light.pdf,ptPdf(s,V,light.direction)):1.0;
                ld=light.radiance*(max(dot(s.normal,light.direction),0.0)/(RT_PI*light.pdf))*mis;
                if(s.specularEnabled)ls=light.radiance*ggxWeight(s,V,light.direction)*ggxPdf(s,V,light.direction)/light.pdf*mis;
            }
            if(bounce==0){D+=sunDiffuse+ld;S+=(sunSpec+ls)/specFactor;}
            else{
                vec3 direct=s.albedo*(1.0-s.metallic)*(sunDiffuse+ld)+sunSpec+ls;
                D+=wd*direct;S+=ws*direct;
            }
        }
        if(!continuePath)break;
        vec3 L;
        float pdf=1.0;
        bool delta=s.mirror;
        if(delta){
            L=normalize(reflect(-V,s.normal));
            wd*=s.albedo;ws*=s.albedo;
        }else{
            L=ptDirection(s,V,rng);pdf=ptPdf(s,V,L);
            if(pdf<=0.0||dot(s.geometricNormal,L)<=0.0)break;
            if(bounce==0){
                wd=vec3(max(dot(s.normal,L),0.0)/(RT_PI*pdf));
                ws=s.specularEnabled?ggxWeight(s,V,L)*ggxPdf(s,V,L)/(pdf*specFactor):vec3(0);
            }else{vec3 f=ptBsdfCos(s,V,L)/pdf;wd*=f;ws*=f;}
        }
        Surface next;
        if(!traceSurface(s.position+s.geometricNormal*rayOffset(s.position),L,100000,next)){
            vec3 sky=skyBackground(L);D+=wd*sky;S+=ws*sky;break;
        }
        if(bounce==0)firstDistance=min(length(next.position-receiver.position),65504.0);
        float mis=delta?1.0:powerHeuristic(pdf,emitterHitPdf(s.position,s.normal,next));
        D+=wd*next.emissive*mis;S+=ws*next.emissive*mis;
        s=next;V=-L;
    }
    // Do not clamp reservoir weights or path contributions: that changes the estimator.
    if(!ptFinite(D)||!ptFinite(S)){D=vec3(0);S=vec3(0);}
    r.diffuseWeight=vec4(D,1);
    r.specularDistance=vec4(S,firstDistance);
    return r;
}
void ptMerge(inout PTReservoir result,PTReservoir source,PTReservoir shifted,float mis,inout float sum,inout uint rng){
    result.receiverM.w+=source.receiverM.w;
    float w=ptTarget(shifted)*source.diffuseWeight.w*mis;
    if(w<=0.0||isnan(w)||isinf(w))return;
    sum+=w;
    if(randomFloat(rng)*sum<w){
        result.diffuseWeight.rgb=shifted.diffuseWeight.rgb;
        result.specularDistance=shifted.specularDistance;
        result.seedPixelNormal.x=source.seedPixelNormal.x;
    }
}
void ptFinalize(inout PTReservoir r,float sum){
    float t=ptTarget(r);
    r.diffuseWeight.w=t>0.0?sum/t:0.0;
    if(isnan(r.diffuseWeight.w)||isinf(r.diffuseWeight.w))r.diffuseWeight.w=0.0;
    // Effective history mass is capped after normalization, like Prime M20.
    r.receiverM.w=min(r.receiverM.w,20.0);
}
bool ptPreviousReceiver(PTReservoir source,out Surface receiver,out vec3 incoming){
    vec2 uv=(vec2(source.seedPixelNormal.yz)+.5)/pc.extent.xy+temporal.jitter.zw;
    mat4 inv=inverse(temporal.previousViewProjection);
    vec4 a=inv*vec4(uv*2.0-1.0,0,1),b=inv*vec4(uv*2.0-1.0,1,1);a/=a.w;b/=b.w;
    incoming=normalize(b.xyz-a.xyz);
    if(!traceSurface(a.xyz,incoming,length(b.xyz-a.xyz),receiver)||receiver.mirror)return false;
    // Reject moved/removed primary geometry, not based on sample brightness.
    float z=abs((temporal.previousView*vec4(receiver.position,1)).z);
    return distance(receiver.position,source.receiverM.xyz)<max(.002,.0001*z)&&
        dot(receiver.geometricNormal,giUnpackNormal(source.seedPixelNormal.w))>.999;
}
bool ptTemporalPixel(Surface receiver,out ivec2 previous){
    if(temporal.rtxdiParams.y<.5)return false;
    vec4 clip=temporal.previousViewProjection*vec4(receiver.previousPosition,1);
    if(clip.w<=0.0)return false;
    // Unjittered motion convention: remove current jitter, not previous jitter.
    vec2 uv=clip.xy/clip.w*.5+.5-temporal.jitter.xy;
    if(any(lessThan(uv,vec2(0)))||any(greaterThanEqual(uv,vec2(1))))return false;
    previous=ivec2(uv*pc.extent.xy);return true;
}
PTReservoir ptTemporalMerge(Surface receiver,vec3 incoming,ivec2 pixel,PTReservoir canonical){
    ivec2 previous;
    if(!ptTemporalPixel(receiver,previous))return canonical;
    PTReservoir donor=ptPrevious[uint(previous.y)*uint(pc.extent.x)+uint(previous.x)];
    if(donor.receiverM.w<=0.0)return canonical;
    Surface old;vec3 oldIncoming;
    if(!ptPreviousReceiver(donor,old,oldIncoming))return canonical;
    PTReservoir forward=ptReplay(receiver,incoming,donor.seedPixelNormal.x,pixel,MAX_PATH_BOUNCES);
    PTReservoir reverse=ptReplay(old,oldIncoming,canonical.seedPixelNormal.x,previous,MAX_PATH_BOUNCES);
    // Bidirectional pairwise balance heuristic, PSS J=1.
    float c=ptTarget(canonical)*canonical.receiverM.w;
    float rev=ptTarget(reverse)*donor.receiverM.w;
    float canonicalMis=rev>0.0?c/max(c+rev,1e-30):1.0;
    float d=ptTarget(donor)*donor.receiverM.w;
    float donorMis=d/max(d+ptTarget(forward)*canonical.receiverM.w,1e-30);
    PTReservoir result=ptEmpty();result.receiverM.xyz=receiver.position;
    result.seedPixelNormal=canonical.seedPixelNormal;
    float sum=0.0;uint rng=hashRandom(canonical.seedPixelNormal.x^0x43b0d7e5u)|1u;
    ptMerge(result,donor,forward,donorMis,sum,rng);
    ptMerge(result,canonical,canonical,canonicalMis,sum,rng);
    ptFinalize(result,sum);return result;
}
PTReservoir ptSpatialMerge(Surface receiver,vec3 incoming,ivec2 pixel,PTReservoir canonical){
    PTReservoir donors[3];Surface receivers[3];vec3 directions[3];ivec2 pixels[3];int count=0;
    uint rng=hashRandom(uint(pixel.x)+uint(pixel.y)*uint(pc.extent.x)^uint(temporal.options.x)^0x7f4a7c15u)|1u;
    // Symmetric +/- neighborhood with fixed candidate count, independent of radiance.
    for(int i=0;i<3;++i){
        float angle=2.0*RT_PI*randomFloat(rng),radius=1.0+15.0*sqrt(randomFloat(rng));
        ivec2 p=pixel+ivec2(round(radius*vec2(cos(angle),sin(angle))));
        bool duplicate=all(equal(p,pixel));for(int j=0;j<count;++j)duplicate=duplicate||all(equal(p,pixels[j]));
        Surface s;
        if(duplicate||!surfaceAtPixel(p,s)||s.mirror||!restirNeighbor(receiver,s))continue;
        PTReservoir r=ptTemporal[uint(p.y)*uint(pc.extent.x)+uint(p.x)];
        if(r.receiverM.w<=0.0)continue;
        donors[count]=r;receivers[count]=s;pixels[count]=p;
        directions[count]=normalize(s.position-pc.eye.xyz);++count;
    }
    if(count==0)return canonical;
    float total=canonical.receiverM.w;for(int i=0;i<count;++i)total+=donors[i].receiverM.w;
    float canonicalMis=canonical.receiverM.w/total;
    float otherMass=total-canonical.receiverM.w;
    PTReservoir result=ptEmpty();result.receiverM.xyz=receiver.position;result.seedPixelNormal=canonical.seedPixelNormal;
    float sum=0.0;
    for(int i=0;i<count;++i){
        PTReservoir forward=ptReplay(receiver,incoming,donors[i].seedPixelNormal.x,pixel,MAX_PATH_BOUNCES);
        PTReservoir reverse=ptReplay(receivers[i],directions[i],canonical.seedPixelNormal.x,pixels[i],MAX_PATH_BOUNCES);
        float rev=ptTarget(reverse)*otherMass,c=ptTarget(canonical)*canonical.receiverM.w;
        canonicalMis+=donors[i].receiverM.w/total*(rev>0.0?c/max(rev+c,1e-30):1.0);
        float src=ptTarget(donors[i])*otherMass;
        float mis=donors[i].receiverM.w/total*src/max(src+ptTarget(forward)*canonical.receiverM.w,1e-30);
        ptMerge(result,donors[i],forward,mis,sum,rng);
    }
    ptMerge(result,canonical,canonical,canonicalMis,sum,rng);
    ptFinalize(result,sum);return result;
}
PTReservoir ptStage(Surface receiver,vec3 incoming,ivec2 pixel,int budget,bool reusable){
    uint address=uint(pixel.y)*uint(pc.extent.x)+uint(pixel.x);
    if(pc.light.w> -4.5){
        uint seed=hashRandom(address^hashRandom(uint(temporal.options.x)+1u)^hashRandom(uint(temporal.rtxdiParams.w)+1u))|1u;
        PTReservoir fresh=ptReplay(receiver,incoming,seed,pixel,budget);ptFresh[address]=fresh;return fresh;
    }
    if(pc.light.w> -5.5){
        PTReservoir result=ptFresh[address];
        if(reusable)result=ptTemporalMerge(receiver,incoming,pixel,result);
        ptTemporal[address]=result;return result;
    }
    if(pc.light.w> -6.5){
        PTReservoir result=ptTemporal[address];
        if(reusable)result=ptSpatialMerge(receiver,incoming,pixel,result);
        ptFinal[address]=result;return result;
    }
    return ptFinal[address];
}
