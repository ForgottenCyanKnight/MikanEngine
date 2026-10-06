// Diffuse ReSTIR GI: reconnect a cached multi-bounce radiance estimate at the
// first non-delta hit. Solid-angle RIS weights include the reconnection Jacobian.
// Four std430 vec4-sized records (64 B). Integer packing preserves normal bits.
struct GIReservoir {
    vec3 samplePosition;uint metadata; // age: low 8 bits, receiver material: high half
    vec4 normalWeight;               // secondary geometric normal, finalized W
    vec4 radianceM;                  // outgoing diffuse radiance, candidate count
    vec3 receiverPosition;uint receiverNormal;
};
layout(std430,binding=30) readonly buffer GIPrevious {GIReservoir giPrevious[];};
layout(std430,binding=31) buffer GICurrent {GIReservoir giCurrent[];};
GIReservoir emptyGIReservoir(){return GIReservoir(vec3(0),0u,vec4(0),vec4(0),vec3(0),0u);}
bool giFinite(float x){return !isnan(x)&&!isinf(x);}
uint giPackNormal(vec3 n){
    n/=abs(n.x)+abs(n.y)+abs(n.z);
    vec2 oct=n.xy;
    if(n.z<0.0)oct=(1.0-abs(oct.yx))*vec2(oct.x>=0.0?1.0:-1.0,oct.y>=0.0?1.0:-1.0);
    return packSnorm2x16(oct);
}
vec3 giUnpackNormal(uint bits){
    vec2 oct=unpackSnorm2x16(bits);
    vec3 n=vec3(oct,1.0-abs(oct.x)-abs(oct.y));
    if(n.z<0.0)n.xy=(1.0-abs(n.yx))*vec2(n.x>=0.0?1.0:-1.0,n.y>=0.0?1.0:-1.0);
    return normalize(n);
}
float giMaterial(Surface s){return diLuminance(s.albedo*(1.0-s.metallic));}
uint giMetadata(float material,uint age){return (packHalf2x16(vec2(material,0))<<16)|(age&255u);}
bool giCompatible(GIReservoir source,Surface surface,bool spatial){
    if(source.radianceM.w<=0.0||!giFinite(source.normalWeight.w)||source.normalWeight.w<0.0)return false;
    if((source.metadata&255u)>=8u)return false;
    if(dot(giUnpackNormal(source.receiverNormal),surface.previousNormal)<.98)return false;
    float z=abs((temporal.view*vec4(surface.position,1)).z);
    float tolerance=max(.02,(spatial?.02:.005)*z);
    vec3 delta=source.receiverPosition-surface.previousPosition;
    if(length(delta)>tolerance||abs(dot(delta,surface.previousNormal))>max(.01,.002*z))return false;
    float material=giMaterial(surface),oldMaterial=unpackHalf2x16(source.metadata>>16).x;
    return abs(oldMaterial-material)<=.03+.15*material;
}
vec3 giContribution(GIReservoir source,Surface receiver){
    vec3 offset=source.samplePosition-receiver.position;
    float d2=dot(offset,offset);if(d2<=1e-10)return vec3(0);
    vec3 direction=offset*inversesqrt(d2);
    if(dot(receiver.geometricNormal,direction)<=0.0||dot(source.normalWeight.xyz,-direction)<=0.0)return vec3(0);
    return max(source.radianceM.rgb,vec3(0))*(max(dot(receiver.normal,direction),0.0)/RT_PI);
}
float giJacobian(GIReservoir source,vec3 receiver){
    vec3 fromOld=source.samplePosition-source.receiverPosition,fromNew=source.samplePosition-receiver;
    float oldD2=dot(fromOld,fromOld),newD2=dot(fromNew,fromNew);
    if(min(oldD2,newD2)<=1e-10)return 0.0;
    float oldCos=dot(source.normalWeight.xyz,-fromOld*inversesqrt(oldD2));
    float newCos=dot(source.normalWeight.xyz,-fromNew*inversesqrt(newD2));
    if(min(oldCos,newCos)<=.02)return 0.0;
    // dOmega_new / dOmega_old; radiance is not multiplied by this factor.
    float J=(newCos*oldD2)/(oldCos*newD2);
    return giFinite(J)&&J>=.1&&J<=10.0?J:0.0;
}
void giStream(inout GIReservoir result,GIReservoir source,float weight,float M,
    inout float weightSum,inout RaySampleState state){
    result.radianceM.w+=M; // retain zero-valued candidates in normalization
    if(weight<=0.0||!giFinite(weight))return;
    weightSum+=weight;
    if(randomFloat(state.fallback)*weightSum<weight){
        result.samplePosition=source.samplePosition;
        result.normalWeight.xyz=source.normalWeight.xyz;
        result.radianceM.rgb=source.radianceM.rgb;
        result.metadata=(result.metadata&0xffff0000u)|(source.metadata&255u);
    }
}
bool giVisible(Surface receiver,vec3 point){
    vec3 origin=receiver.position+receiver.geometricNormal*rayOffset(receiver.position);
    vec3 offset=point-origin;float distanceToPoint=length(offset);
    float endpointOffset=max(.002,rayOffset(point)*2.0);
    if(distanceToPoint<=endpointOffset+.001)return false;
    rayQueryEXT shadow;
    rayQueryInitializeEXT(shadow,scene,gl_RayFlagsOpaqueEXT|gl_RayFlagsTerminateOnFirstHitEXT,1,
        origin,.001,offset/distanceToPoint,distanceToPoint-endpointOffset);
    while(rayQueryProceedEXT(shadow)){}
    return rayQueryGetIntersectionTypeEXT(shadow,true)==gl_RayQueryCommittedIntersectionNoneEXT;
}
float giAreaDensity(GIReservoir source,Surface receiver){
    vec3 delta=source.samplePosition-receiver.position;float d2=dot(delta,delta);
    if(d2<=1e-10)return 0.0;
    vec3 direction=delta*inversesqrt(d2);
    float receiverCos=max(dot(receiver.normal,direction),0.0);
    float secondaryCos=max(dot(source.normalWeight.xyz,-direction),0.0);
    if(receiverCos<=0.0||secondaryCos<=0.0||dot(receiver.geometricNormal,direction)<=0.0)return 0.0;
    return giVisible(receiver,source.samplePosition)?receiverCos*secondaryCos/(RT_PI*d2):0.0;
}
vec4 correctedSpatialGI(Surface surface,ivec2 pixel,vec3 unreused,float hitDistance,inout RaySampleState state){
    RaySampleState neighbors=initRaySampleState(uvec2(pixel),uint(temporal.options.x));
    neighbors.fallback=hashRandom(neighbors.fallback^0x1b56c4e9u);
    state.fallback=hashRandom(neighbors.fallback^0x9e3779b9u);
    GIReservoir samples[6];Surface receivers[6];ivec2 locations[5];int count=0;
    // Neighbor choice depends only on current primary geometry, never on the
    // sample's radiance or whether its first continuation happened to hit sky.
    for(int i=0;i<5;++i){
        ivec2 location=restirNeighborPixel(pixel,i,neighbors);Surface receiver;
        bool duplicate=false;for(int j=0;j<count;++j)duplicate=duplicate||all(equal(location,locations[j]));
        if(duplicate||!surfaceAtPixel(location,receiver)||!restirNeighbor(surface,receiver))continue;
        locations[count]=location;receivers[count]=receiver;
        samples[count++]=giCurrent[uint(location.y)*uint(pc.extent.x)+uint(location.x)];
    }
    ivec2 previous;Surface previousReceiver;
    if(previousRestirPixel(surface,previous)){
        GIReservoir old=giPrevious[uint(previous.y)*uint(pc.extent.x)+uint(previous.x)];
        if(old.radianceM.w>0.0&&temporalReceiver(surface,old.receiverPosition,giUnpackNormal(old.receiverNormal),previousReceiver)){
            // Current TLAS is valid for this source only because the CPU flag
            // rejects all transformed/light-invalidated transport history.
            receivers[count]=previousReceiver;samples[count++]=old;
        }
    }
    float sum=0.0,selectedTarget=0.0;int selected=-1;
    for(int i=0;i<count;++i){
        if(samples[i].normalWeight.w<=0.0)continue;
        float density=giAreaDensity(samples[i],surface);
        float target=diLuminance(samples[i].radianceM.rgb)*density;
        if(target<=0.0)continue;
        float weight=target*samples[i].normalWeight.w*samples[i].radianceM.w;
        sum+=weight;if(randomFloat(state.fallback)*sum<weight){selected=i;selectedTarget=target;}
    }
    if(selected<0)return vec4(unreused,hitDistance);
    float density=giAreaDensity(samples[selected],surface);
    float piSum=0.0;
    float luminance=diLuminance(samples[selected].radianceM.rgb);
    for(int j=0;j<count;++j)piSum+=samples[j].radianceM.w*luminance*giAreaDensity(samples[selected],receivers[j]);
    float pi=luminance*giAreaDensity(samples[selected],receivers[selected]);
    if(piSum<=0.0)return vec4(unreused,hitDistance);
    // Reverse target/visibility normalization, in the same area measure as
    // the initial reservoirs. Null/zero candidates retain their source count.
    float W=sum*pi/(selectedTarget*piSum);
    vec3 estimate=samples[selected].radianceM.rgb*density*W;
    return vec4(unreused+estimate,min(length(samples[selected].samplePosition-surface.position),65504.0));
}
vec4 sampleRestirGI(Surface surface,ivec2 pixel,GICandidate fresh,vec3 unreused,float firstHitDistance,inout RaySampleState state){
    if((uint(temporal.giOptions.w)&8192u)!=0u){
        uint candidates=max(1u,(uint(temporal.giOptions.w)>>16u)&127u);
        state.fallback=hashRandom(state.fallback^0xa511e9b3u);
        GICandidate selected=fresh;float sum=0.0;vec3 unshared=vec3(0);
        for(uint i=0u;i<candidates;++i){
            GICandidate proposal=fresh;vec3 contribution=unreused;
            if(i>0u){
                RaySampleState extra=initRaySampleState(uvec2(pixel),uint(temporal.options.x)+i*131u);
                RaySampleState extraSun=extra;extraSun.dimension=8u;
                sampleDiffuseGI(surface,extra,extraSun,MAX_PATH_BOUNCES,MAX_MIRROR_BOUNCES,proposal,contribution);
            }
            unshared+=contribution;
            float weight=proposal.pdf>0.0?diLuminance(proposal.radiance):0.0;
            sum+=weight;if(weight>0.0&&randomFloat(state.fallback)*sum<weight)selected=proposal;
        }
        unshared/=float(candidates);
        if(pc.light.w< -2.5)return correctedSpatialGI(surface,pixel,unshared,firstHitDistance,state);
        GIReservoir raw=emptyGIReservoir();raw.samplePosition=fresh.position;
        raw.samplePosition=selected.position;raw.normalWeight.xyz=selected.normal;
        vec3 delta=selected.position-surface.position;float d2=dot(delta,delta);
        float secondaryCos=d2>0.0?max(dot(selected.normal,-delta*inversesqrt(d2)),0.0):0.0;
        float target=diLuminance(selected.radiance)*selected.pdf*secondaryCos/max(d2,1e-10);
        raw.normalWeight.w=target>0.0?sum/(float(candidates)*target):0.0;
        raw.radianceM=vec4(selected.radiance,float(candidates));raw.receiverPosition=surface.position;raw.receiverNormal=giPackNormal(surface.normal);
        giCurrent[uint(pixel.y)*uint(pc.extent.x)+uint(pixel.x)]=raw;
        return vec4(unshared+(sum>0.0?selected.radiance*(sum/(float(candidates)*diLuminance(selected.radiance))):vec3(0)),firstHitDistance);
    }
    uint address=uint(pixel.y)*uint(pc.extent.x)+uint(pixel.x);
    GIReservoir result=emptyGIReservoir();result.receiverPosition=surface.position;
    result.receiverNormal=giPackNormal(surface.normal);result.metadata=giMetadata(giMaterial(surface),0u);
    GIReservoir initial=emptyGIReservoir();initial.samplePosition=fresh.position;
    initial.normalWeight.xyz=fresh.normal;initial.radianceM.rgb=fresh.radiance;
    float weightSum=0.0;
    float target=fresh.pdf>0.0?diLuminance(giContribution(initial,surface)):0.0;
    giStream(result,initial,fresh.pdf>0.0?target/fresh.pdf:0.0,1.0,weightSum,state);
    // Read only last frame's immutable reservoir buffer. Current-frame spatial
    // reads require a separate dispatch; never rely on workgroup-local barriers.
    if(temporal.giOptions.y<.5){
        vec4 clip=temporal.previousViewProjection*vec4(surface.previousPosition,1);
        if(clip.w>0.0){
            vec2 uv=clip.xy/clip.w*.5+.5-temporal.jitter.zw;
            if(all(greaterThanEqual(uv,vec2(0)))&&all(lessThan(uv,vec2(1)))){
                ivec2 previous=ivec2(uv*pc.extent.xy),visited[3];int visitedCount=0;
                for(int i=0;i<3;++i){
                    ivec2 location=previous;
                    if(i>0){float phi=2.0*RT_PI*randomFloat(state.fallback);
                        float radius=1.0+floor(randomFloat(state.fallback)*4.0);
                        location+=ivec2(round(vec2(cos(phi),sin(phi))*radius));}
                    if(any(lessThan(location,ivec2(0)))||any(greaterThanEqual(location,ivec2(pc.extent.xy))))continue;
                    bool duplicate=false;for(int j=0;j<visitedCount;++j)duplicate=duplicate||all(equal(visited[j],location));
                    if(duplicate)continue;visited[visitedCount++]=location;
                    GIReservoir old=giPrevious[uint(location.y)*uint(pc.extent.x)+uint(location.x)];
                    if(!giCompatible(old,surface,i>0))continue;
                    float M=min(old.radianceM.w,i==0?8.0:2.0);
                    if(old.normalWeight.w==0.0){giStream(result,old,0.0,M,weightSum,state);continue;}
                    float J=giJacobian(old,surface.position);if(J<=0.0)continue;
                    float currentTarget=diLuminance(giContribution(old,surface));
                    old.metadata=(old.metadata&0xffff0000u)|((old.metadata&255u)+1u);
                    giStream(result,old,currentTarget*old.normalWeight.w*J*M,M,weightSum,state);
                }
            }
        }
    }
    float selectedTarget=diLuminance(giContribution(result,surface));
    if(weightSum<=0.0||selectedTarget<=0.0||!giFinite(weightSum)){
        result.normalWeight.w=0.0;result.radianceM.w=min(result.radianceM.w,16.0);
        giCurrent[address]=result;return vec4(unreused,firstHitDistance);
    }
    // Standard bounded, biased reuse mode. Full candidate M is used both for
    // normalization and stored history; no selected-support denominator shortcut.
    result.normalWeight.w=weightSum/(result.radianceM.w*selectedTarget);
    result.radianceM.w=min(result.radianceM.w,16.0);
    if(!giFinite(result.normalWeight.w)){giCurrent[address]=emptyGIReservoir();return vec4(unreused,firstHitDistance);}
    // Fresh radiance was bounded before target evaluation; bound the final
    // RIS estimate too and persist its scale in W for subsequent reuse.
    result.normalWeight.w*=rtFireflyScale(giContribution(result,surface)*result.normalWeight.w,FIREFLY_GI);
    giCurrent[address]=result;
    vec3 indirect=vec3(0);
    if(giVisible(surface,result.samplePosition)){
        indirect=giContribution(result,surface)*result.normalWeight.w;
        firstHitDistance=min(length(result.samplePosition-surface.position),65504.0);
    }
    return vec4(unreused+indirect,firstHitDistance);
}
