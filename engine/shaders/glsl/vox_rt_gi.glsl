// One cosine-weighted diffuse sample per pixel. Visibility is evaluated using
// the full TLAS; a sky miss contributes sky radiance, never unoccluded sky SH.
layout(rgba16f,binding=10) uniform writeonly image2D diffuseImage;
layout(r32f,binding=11) uniform writeonly image2D viewZImage;
layout(rgba16f,binding=12) uniform writeonly image2D normalImage;
layout(rg16f,binding=13) uniform writeonly image2D motionImage;
layout(rgba16f,binding=14) uniform writeonly image2D materialImage;
layout(rgba16f,binding=22) uniform writeonly image2D specularImage;
layout(rgba16f,binding=23) uniform writeonly image2D specularMaterialImage;
layout(std140,binding=15) uniform Temporal {
    mat4 view;
    mat4 previousViewProjection;
    mat4 previousView;
    vec4 jitter; // current and previous jitter in UV
    vec4 options; // x temporal sample index; y history reset; z STBN available
    vec4 giOptions; // x GI reuse; y reset; z firefly scale; w integer flags: bit3 diagnostics, bit4 skip mirrors, bit5 RR
    vec4 rtxdiParams; // x RTXDI mode (0 NEE, 1 self ReSTIR, 2 RTXDI SDK); y shading buffer index; z blockRowPitch; w arrayPitch
} temporal;

// Linear HDR contribution limits, before tone mapping and material remodulation.
// The knee leaves normal samples unchanged and asymptotically limits Y to 2*T.
// Scale all channels together to retain chromaticity. Never modify material Le.
const float FIREFLY_DI=16.0, FIREFLY_GI=8.0, FIREFLY_SPECULAR=32.0;
float rtFireflyThreshold(float base){return base*max(pc.extent.w,0.0001)*temporal.giOptions.z;}
float rtFireflyScale(vec3 value,float base){
    if(any(isnan(value))||any(isinf(value)))return 0.0;
    // Clipping contribution weights is biased. Experimental corrected sampling
    // retains full energy; display/denoiser behavior is evaluated separately.
    if((uint(temporal.giOptions.w)&24576u)!=0u)return 1.0;
    float threshold=rtFireflyThreshold(base);
    if(threshold<=0.0)return 1.0;
    float y=dot(max(value,vec3(0)),vec3(.2126,.7152,.0722));
    if(y<=threshold)return 1.0;
    float capped=threshold+threshold*((y-threshold)/y);
    return capped/y;
}
vec3 rtClampFirefly(vec3 value,float base){
    if(any(isnan(value))||any(isinf(value)))return vec3(0);
    return max(value,vec3(0))*rtFireflyScale(value,base);
}

#include "vox_rt_sampling.glsl"
vec3 diffuseDirection(vec3 n,inout RaySampleState state){
    vec2 sampleUV=nextRaySample(state);
    float u=sampleUV.x,phi=2.0*RT_PI*sampleUV.y;
    vec3 axis=abs(n.z)<0.999?vec3(0,0,1):vec3(1,0,0);
    vec3 t=normalize(cross(axis,n)),b=cross(n,t);
    return normalize(t*sqrt(u)*cos(phi)+b*sqrt(u)*sin(phi)+n*sqrt(1.0-u));
}
#include "vox_rt_sun.glsl"
vec3 sampleVisibleSky(Surface surface,inout RaySampleState state){
    if(pc.extent.z<.5)return vec3(0);
    vec3 direction=diffuseDirection(surface.normal,state);
    if(dot(direction,surface.geometricNormal)<=0)return vec3(0);
    rayQueryEXT visibility;
    rayQueryInitializeEXT(visibility,scene,gl_RayFlagsOpaqueEXT|gl_RayFlagsTerminateOnFirstHitEXT,1,
        surface.position+surface.geometricNormal*rayOffset(surface.position),.001,direction,100000);
    while(rayQueryProceedEXT(visibility)){}
    // A direct environment-light visibility query, not another shaded GI hit.
    return rayQueryGetIntersectionTypeEXT(visibility,true)==gl_RayQueryCommittedIntersectionNoneEXT
        ? (voxelOccluded(surface.position+surface.geometricNormal*rayOffset(surface.position),direction,100000,1u)?vec3(0):skyBackground(direction)) : vec3(0);
}
// PDFs are solid-angle densities at the unoffset shading point.
float powerHeuristic(float a,float b){
    float scale=max(a,b);if(scale<=0.0)return 1.0;
    a/=scale;b/=scale;return a*a/(a*a+b*b);
}
uint neeFreshGiSamples(){
    uint count=temporal.rtxdiParams.x<-3.5?4u:(temporal.rtxdiParams.x<-1.5?1u:2u);
    if(neeEdgeBudget && temporal.rtxdiParams.x<0.0){
        ivec2 p=ivec2(rtPixel),size=ivec2(pc.extent.xy);
        PrimaryHit center=primaryHits[uint(p.y)*uint(size.x)+uint(p.x)];
        const ivec2 offsets[4]=ivec2[4](ivec2(1,0),ivec2(-1,0),ivec2(0,1),ivec2(0,-1));
        for(int i=0;i<4;++i){ivec2 q=clamp(p+offsets[i],ivec2(0),size-1);
            PrimaryHit neighbor=primaryHits[uint(q.y)*uint(size.x)+uint(q.x)];
            if(center.instancePlusOne!=neighbor.instancePlusOne || abs(center.hitT-neighbor.hitT)>.02*max(center.hitT,.1)){count=4u;break;}}
    }
    return count;
}
float primaryNeeMisRatio(){
    if(temporal.rtxdiParams.x>=0.0)return 1.0;
    float gi=float(neeFreshGiSamples());
    uint ref=(uint(temporal.giOptions.w)&16384u)!=0u?(uint(temporal.giOptions.w)>>16u)&127u:0u;
    gi=max(gi,float(ref));
    return max(float(neePrimaryDiSamples),float(ref))/gi;
}
bool useLocalEmitterProposal(){return neeLocalProposal && temporal.rtxdiParams.x<0.0;}
#include "vox_rt_light_tree.glsl"
float localEmitterScore(vec3 origin,vec3 normal,uint i){
    uint b=1u+i*4u;vec3 p0=emissiveLights[b+1u].xyz,p1=emissiveLights[b+2u].xyz,p2=emissiveLights[b+3u].xyz;
    vec3 delta=(p0+p1+p2)/3.0-origin;float d2=dot(delta,delta);
    vec3 wi=delta*inversesqrt(max(d2,1e-10));vec3 ln=normalize(cross(p1-p0,p2-p0));
    float power=emissiveLights[b].w*dot(max(emissiveLights[b].rgb,vec3(0)),vec3(.2126,.7152,.0722));
    return power*max(dot(normal,wi),0.0)*abs(dot(ln,wi))/max(d2,emissiveLights[b].w);
}
float localEmitterTotal(vec3 origin,vec3 normal){
    float total=0.0;uint count=uint(emissiveLights[0].x);
    for(uint i=0u;i<count;++i)total+=localEmitterScore(origin,normal,i);return total;
}
float emitterProbability(vec3 origin,vec3 normal,uint i,float total){
    float global=emissiveLights[3u+i*4u].w;
    return total>1e-20?.1*global+.9*localEmitterScore(origin,normal,i)/total:global;
}
struct EmitterSample {vec3 direction;vec3 radiance;float pdf;};
EmitterSample sampleEmitter(Surface surface,inout RaySampleState state){
    EmitterSample result=EmitterSample(vec3(0),vec3(0),0.0);
    uint count=uint(emissiveLights[0].x);if(count==0u)return result;
    // STBN's 8-bit values cannot resolve a large categorical CDF. Use the
    // persistent 24-bit RNG for light choice, keeping STBN for surface points.
    float u=randomFloat(state.fallback);uint chosen=0u;float probability=0.0;
    if(useEmitterTree()){
        // A global proposal floor prevents position-only tree scores from
        // starving visible emitters behind a misleading spatial hierarchy.
        // Independent 24-bit draws keep each conditional selector unchanged.
        if(neeTreePrime)chosen=selectEmitterPrimeMixture(surface.position,u,randomFloat(state.fallback),probability);
        else chosen=selectEmitterTree(surface.position,u,probability);
    }
    else if(useLocalEmitterProposal()){
        float total=localEmitterTotal(surface.position,surface.normal),cumulative=0.0;
        for(uint i=0u;i<count;++i){float p=emitterProbability(surface.position,surface.normal,i,total);cumulative+=p;
            if(u<cumulative||i+1u==count){chosen=i;probability=p;break;}}
    }else{
        uint low=0u,high=count;
        while(low<high){uint mid=low+(high-low)/2u;
            if(u<emissiveLights[2u+mid*4u].w)high=mid;else low=mid+1u;}
        chosen=min(low,count-1u);probability=emissiveLights[3u+chosen*4u].w;
    }
    uint base=1u+chosen*4u;vec4 light=emissiveLights[base];
    if(probability<=0.0||light.w<=0.0)return result;
    vec3 p0=emissiveLights[base+1u].xyz,p1=emissiveLights[base+2u].xyz,p2=emissiveLights[base+3u].xyz;
    vec2 pick=nextRaySample(state);float su=sqrt(pick.x);
    vec3 point=p0*(1.0-su)+p1*(su*(1.0-pick.y))+p2*(su*pick.y);
    vec3 offset=point-surface.position;float dist2=dot(offset,offset),dist=sqrt(dist2);
    if(dist2<=1e-10)return result;
    vec3 direction=offset/dist;
    // traceSurface treats emissive surfaces as two-sided; NEE must agree.
    float facing=abs(dot(normalize(cross(p1-p0,p2-p0)),-direction));
    if(facing<=1e-8||dot(surface.normal,direction)<=0.0||dot(surface.geometricNormal,direction)<=0.0)return result;
    vec3 origin=surface.position+surface.geometricNormal*rayOffset(surface.position);
    vec3 shadowDirection=direction;float shadowMax=dist*(1.0-1e-3);
    if(neeStrictShadow && temporal.rtxdiParams.x<0.0){
        vec3 segment=point-origin;float lengthToLight=length(segment);
        shadowDirection=segment/max(lengthToLight,1e-8);
        shadowMax=max(.001,lengthToLight-max(.001,rayOffset(point)));
    }
    rayQueryEXT shadow;
    rayQueryInitializeEXT(shadow,scene,gl_RayFlagsOpaqueEXT|gl_RayFlagsTerminateOnFirstHitEXT,2,
        origin,.001,shadowDirection,shadowMax);
    while(rayQueryProceedEXT(shadow)){}
    if(rayQueryGetIntersectionTypeEXT(shadow,true)!=gl_RayQueryCommittedIntersectionNoneEXT||voxelOccluded(origin,shadowDirection,shadowMax,2u))return result;
    result.direction=direction;result.radiance=light.rgb;
    result.pdf=probability*dist2/(light.w*facing);return result;
}
// Exact TLAS custom-index / BLAS geometry / primitive mapping avoids both
// all-light scans and epsilon-based matches to unrelated coplanar emitters.
float emitterHitPdf(vec3 origin,vec3 normal,Surface hit){
    vec3 point=hit.position;
    vec3 offset=point-origin;float dist2=dot(offset,offset);
    if(dist2<=1e-10)return 0.0;vec3 direction=offset*inversesqrt(dist2);
    uint i=emitterForHit(hit.hitIdentity);if(i==0xffffffffu)return 0.0;
    float localTotal=useLocalEmitterProposal()?localEmitterTotal(origin,normal):0.0;
    uint base=1u+i*4u;
        vec3 p0=emissiveLights[base+1u].xyz,e1=emissiveLights[base+2u].xyz-p0,e2=emissiveLights[base+3u].xyz-p0;
        vec3 n=normalize(cross(e1,e2));
        float facing=abs(dot(n,-direction));
        float probability=useEmitterTree()?emitterTreeProbability(origin,i):
            (useLocalEmitterProposal()?emitterProbability(origin,normal,i,localTotal):emissiveLights[base+2u].w);
        if(useEmitterTree()&&neeTreePrime)probability=emitterPrimeProbability(origin,i);
        return facing>1e-8?probability*dist2/(emissiveLights[base].w*facing):0.0;
}
vec3 sampleEmissiveLights(Surface surface,inout RaySampleState state,bool useMIS){
    EmitterSample light=sampleEmitter(surface,state);if(light.pdf<=0.0)return vec3(0);
    float cosine=max(dot(surface.normal,light.direction),0.0),bsdfPdf=cosine/RT_PI;
    vec3 estimate=light.radiance*cosine/light.pdf*(useMIS?powerHeuristic(light.pdf,bsdfPdf):1.0);
    return rtClampFirefly(estimate/RT_PI,FIREFLY_DI)*RT_PI;
}
vec3 sampleEmissiveLights(Surface surface,inout RaySampleState state){
    return sampleEmissiveLights(surface,state,true);
}
vec3 samplePrimaryEmissiveLights(Surface surface,inout RaySampleState state){
    EmitterSample light=sampleEmitter(surface,state);if(light.pdf<=0.0)return vec3(0);
    float cosine=max(dot(surface.normal,light.direction),0.0);
    return light.radiance*(cosine/(RT_PI*light.pdf))*(neeDiVariance?1.0:powerHeuristic(primaryNeeMisRatio()*light.pdf,cosine/RT_PI));
}
// GGX visible-normal sampling (Heitz 2018) in the tangent frame around Ve.
vec3 sampleGGXVNDF(vec3 Ve,float alpha,float u1,float u2){
    vec3 Vh=normalize(vec3(alpha*Ve.x,alpha*Ve.y,Ve.z));
    float lensq=Vh.x*Vh.x+Vh.y*Vh.y;
    vec3 T1=lensq>0.0?vec3(-Vh.y,Vh.x,0.0)*inversesqrt(lensq):vec3(1.0,0.0,0.0);
    vec3 T2=cross(Vh,T1);
    float r=sqrt(u1),phi=2.0*RT_PI*u2;
    float t1=r*cos(phi),t2=r*sin(phi);
    float s=0.5*(1.0+Vh.z);
    t2=(1.0-s)*sqrt(max(0.0,1.0-t1*t1))+s*t2;
    float t3=sqrt(max(0.0,1.0-t1*t1-t2*t2));
    vec3 Nh=t1*T1+t2*T2+t3*Vh;
    return normalize(vec3(alpha*Nh.x,alpha*Nh.y,max(1e-5,Nh.z)));
}
// GGX VNDF proposal: pdf(L)=D(H)*G1(V)/(4*NdotV).
// BRDF*cos/pdf = Fresnel * G2(V,L)/G1(V), correlated Smith masking.
// Signal is demodulated by a deterministic view-dependent Fresnel factor.
vec3 specularMaterialFactor(Surface s,vec3 incoming){
    vec3 f0=mix(vec3(0.04),s.albedo,clamp(s.metallic,0.0,1.0));
    float nv=clamp(dot(s.normal,-incoming),0.0,1.0);
    return max(f0+(1.0-f0)*pow(1.0-nv,5.0),vec3(0.02));
}
float ggxLambda(float cosine,float alpha){
    float c2=max(cosine*cosine,1e-8);
    return 0.5*(sqrt(1.0+alpha*alpha*(1.0-c2)/c2)-1.0);
}
float ggxPdf(Surface s,vec3 V,vec3 L){
    float nv=dot(s.normal,V),nl=dot(s.normal,L);if(nv<=0.0||nl<=0.0)return 0.0;
    vec3 sum=V+L;if(dot(sum,sum)<=1e-12)return 0.0;
    vec3 H=normalize(sum);float nh=max(dot(s.normal,H),0.0);
    float alpha=max(s.roughness*s.roughness,1e-4),a2=alpha*alpha;
    float d=(1.0-nh*nh)+nh*nh*a2;
    float D=a2/(RT_PI*d*d);
    return D/(4.0*nv*(1.0+ggxLambda(nv,alpha)));
}
vec3 ggxWeight(Surface s,vec3 V,vec3 L){
    float nv=dot(s.normal,V),nl=dot(s.normal,L);
    if(nv<=0.0||nl<=0.0)return vec3(0);
    vec3 H=normalize(V+L),f0=mix(vec3(.04),s.albedo,clamp(s.metallic,0.0,1.0));
    vec3 F=f0+(1.0-f0)*pow(1.0-clamp(dot(V,H),0.0,1.0),5.0);
    float alpha=max(s.roughness*s.roughness,1e-4),lv=ggxLambda(nv,alpha),ll=ggxLambda(nl,alpha);
    return F*(1.0+lv)/(1.0+lv+ll);
}
vec4 sampleSpecularReflection(Surface surface,vec3 incoming,inout RaySampleState state,inout RaySampleState sunState,bool sdkSpecular,vec3 sdkDirect){
    if(!surface.specularEnabled)return vec4(0);
    float alpha=max(surface.roughness*surface.roughness,1e-4);
    vec3 V=-incoming;
    vec3 up=abs(surface.normal.z)<0.999?vec3(0,0,1):vec3(1,0,0);
    vec3 T=normalize(cross(up,surface.normal)),B=cross(surface.normal,T);
    vec3 Ve=vec3(dot(V,T),dot(V,B),dot(V,surface.normal));
    if(Ve.z<=0.0)return vec4(0);
    vec3 direct=vec3(0);
    if(sdkSpecular){
        if(emissiveLights[0].x>0.0){randomFloat(state.fallback);nextRaySample(state);}
        direct=sdkDirect;
    }else{
        EmitterSample light=sampleEmitter(surface,state);
        if(light.pdf>0.0){float pdf=ggxPdf(surface,V,light.direction);
            direct=light.radiance*ggxWeight(surface,V,light.direction)*(pdf/light.pdf)*powerHeuristic(light.pdf,pdf);}
    }
    vec2 u=nextRaySample(state);
    vec3 Ht=sampleGGXVNDF(Ve,alpha,u.x,u.y);
    vec3 H=normalize(Ht.x*T+Ht.y*B+Ht.z*surface.normal);
    vec3 L=reflect(-V,H);
    float nl=dot(surface.normal,L);
    if(nl<=0.0||dot(L,surface.geometricNormal)<=0.0)return vec4(rtClampFirefly(direct,FIREFLY_SPECULAR)/specularMaterialFactor(surface,incoming),65504.0);
    vec3 F0=mix(vec3(0.04),surface.albedo,clamp(surface.metallic,0.0,1.0));
    vec3 F=F0+(1.0-F0)*pow(1.0-clamp(dot(V,H),0.0,1.0),5.0);
    float lambdaV=ggxLambda(Ve.z,alpha),lambdaL=ggxLambda(nl,alpha);
    vec3 weight=F*((1.0+lambdaV)/(1.0+lambdaV+lambdaL));
    Surface rs;
    vec3 incident;float hitDistance=65504.0;
    if(!traceSurface(surface.position+surface.geometricNormal*rayOffset(surface.position),L,100000,rs)){
        incident=skyBackground(L);
    }else{
        hitDistance=min(length(rs.position-surface.position),65504.0);
        vec3 diffuseWeight=rs.albedo*(1.0-rs.metallic);
        incident=rs.emissive*(any(greaterThan(rs.emissive,vec3(0)))?powerHeuristic(ggxPdf(surface,V,L),emitterHitPdf(surface.position,surface.normal,rs)):1.0)
            +diffuseWeight*(sampleSunIrradiance(rs,sunState)+sampleEmissiveLights(rs,state,false))/RT_PI
            +diffuseWeight*sampleVisibleSky(rs,state);
    }
    vec3 reflected=rtClampFirefly(direct+weight*incident,FIREFLY_SPECULAR);
    return vec4(reflected/specularMaterialFactor(surface,incoming),hitDistance);
}
// Specular and diffuse continuation rays share this budget. Primary camera
// intersection and direct-light visibility queries do not consume a bounce.
const int MAX_PATH_BOUNCES=4;
const int MAX_MIRROR_BOUNCES=2;
const float RR_MIN_SURVIVAL=0.80;
struct GICandidate {vec3 position;vec3 normal;vec3 radiance;float pdf;};
vec4 sampleDiffuseGI(Surface receiver,inout RaySampleState state,inout RaySampleState sunState,int remainingBounces,int remainingMirrors,
    out GICandidate candidate,out vec3 unreused){
    candidate=GICandidate(vec3(0),vec3(0),vec3(0),0.0);unreused=vec3(0);
    if(remainingBounces<=0){unreused=sampleVisibleSky(receiver,state);return vec4(unreused,65504);}
    Surface vertex=receiver;
    // Separate scalar RNG: roulette does not consume STBN sampling dimensions.
    // Extra mirror GI paths use distinct state.frame values and therefore seeds.
    uint rouletteState=hashRandom(state.fallback^0xa511e9b3u);
    bool rouletteEnabled=(uint(temporal.giOptions.w)&32u)!=0u;
    vec3 direction=diffuseDirection(vertex.normal,state);
    vec3 weight=vec3(1),radiance=vec3(0);
    float firstHitDistance=65504;
    bool previousWasDelta=false;
    for(int bounce=0;bounce<MAX_PATH_BOUNCES;++bounce){
        if(bounce>=remainingBounces)break;
        if(dot(direction,vertex.geometricNormal)<=0)break;
        Surface next;
        if(!traceSurface(vertex.position+vertex.geometricNormal*rayOffset(vertex.position),direction,100000,next)){
            if(!neeDiMisVariance&&pc.extent.z>.5)radiance+=weight*skyBackground(direction);
            break;
        }
        if(bounce==0)firstHitDistance=min(length(next.position-receiver.position),65504.0);
        float emissionMIS=(previousWasDelta||all(lessThanEqual(next.emissive,vec3(0))))?1.0:powerHeuristic(
            max(dot(vertex.normal,direction),0.0)/RT_PI,(bounce==0?primaryNeeMisRatio():1.0)*emitterHitPdf(vertex.position,vertex.normal,next));
        // Measurement only: complete direct-emitter MIS, excluding sky and
        // all illumination reached after the first diffuse intersection.
        if(neeDiMisVariance){unreused=next.emissive*emissionMIS;return vec4(unreused,firstHitDistance);}
        vertex=next;
        if(bounce==0&&!vertex.mirror){
            candidate.position=vertex.position;candidate.normal=vertex.geometricNormal;
            candidate.pdf=max(dot(receiver.normal,direction),0.0)/RT_PI;
            // First-hit Le complements primary emitter NEE/MIS. Keep it fresh,
            // outside GI resampling, so its MIS weight is never cached/reused.
            unreused=vertex.emissive*emissionMIS;
        }else radiance+=weight*vertex.emissive*emissionMIS;
        if(vertex.mirror){
            if(remainingMirrors<=0)break;
            --remainingMirrors;
            previousWasDelta=true;
            // A mirror consumes the next continuation bounce just like diffuse.
            weight*=clamp(vertex.albedo,vec3(0),vec3(1));
            direction=normalize(reflect(direction,vertex.normal));
        }else{
            // Metals carry almost no diffuse transport; the specular lobe is a
            // separate GGX ray from the primary hit.
            previousWasDelta=false;
            weight*=vertex.albedo*(1.0-vertex.metallic);
            radiance+=weight*(sampleSunIrradiance(vertex,sunState)+sampleEmissiveLights(vertex,state,bounce+1<remainingBounces))/RT_PI;
            if(bounce+1>=remainingBounces){
                // Terminal vertex still samples the visible environment.
                radiance+=weight*(sampleVisibleSky(vertex,state));
            }else{
                // Keep both first GI intersections and this vertex's emission/
                // direct lighting. Only the future continuation is randomized.
                // Mirrors use the branch above and never take this roulette.
                if(rouletteEnabled&&bounce>=1){
                    float survival=clamp(max(weight.r,max(weight.g,weight.b)),RR_MIN_SURVIVAL,1.0);
                    if(survival<1.0){
                        if(randomFloat(rouletteState)>=survival)break;
                        // Compensate only future throughput, not collected light.
                        // The radiance below carries this weight into ReSTIR GI.
                        weight/=survival;
                    }
                }
                // This continuation estimates sky and indirect light together;
                // do not also add a separate sky sample at this vertex.
                direction=diffuseDirection(vertex.normal,state);
            }
        }
    }
    // Receiver albedo remains outside NRD. NRD tracks the first GI hit distance,
    // rather than the complete path length, for multi-bounce diffuse transport.
    radiance=rtClampFirefly(radiance,FIREFLY_GI);
    unreused=rtClampFirefly(unreused,FIREFLY_GI);
    if(candidate.pdf>0.0){candidate.radiance=radiance;return vec4(radiance+unreused,firstHitDistance);}
    // Sky misses and first-hit delta mirrors retain the original estimator.
    unreused=radiance;return vec4(radiance,firstHitDistance);
}
#include "vox_rt_restir.glsl"
#include "vox_rt_restir_gi.glsl"
#include "vox_rtxdi_shade.glsl"
vec3 mirrorPoint(vec3 point,vec3 planePoint,vec3 planeNormal){
    return point-2.0*dot(point-planePoint,planeNormal)*planeNormal;
}
void storeDiffuseGuides(ivec2 pixel,vec2 pixelUV,Surface surface,Surface mirrors[2],int mirrorCount,vec3 throughput,vec4 diffuse){
    vec3 position=surface.position,previousPosition=surface.previousPosition,n=surface.normal;
    // Undo planar mirror transport in reverse order for virtual surface guides.
    for(int i=mirrorCount-1;i>=0;--i){
        position=mirrorPoint(position,mirrors[i].position,mirrors[i].normal);
        previousPosition=mirrorPoint(previousPosition,mirrors[i].previousPosition,mirrors[i].previousNormal);
        n=reflect(n,mirrors[i].normal);
    }
    vec4 previousClip=temporal.previousViewProjection*vec4(previousPosition,1);
    vec2 previousUV=previousClip.w>0 ? clamp(previousClip.xy/previousClip.w*.5+.5,vec2(-2),vec2(3)) : vec2(-2);
    vec2 motion=temporal.options.y>.5 ? vec2(0) : pixelUV+temporal.jitter.xy-previousUV;
    float z=abs((temporal.view*vec4(position,1)).z);
    imageStore(diffuseImage,pixel,diffuse);
    imageStore(viewZImage,pixel,vec4(z));
    imageStore(normalImage,pixel,vec4(normalize(n),clamp(surface.roughness,0.0,1.0)));
    imageStore(motionImage,pixel,vec4(motion,0,0));
    imageStore(materialImage,pixel,vec4(throughput*surface.albedo*(1.0-surface.metallic),1));
}