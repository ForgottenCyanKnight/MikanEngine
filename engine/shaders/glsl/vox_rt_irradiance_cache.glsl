// Experimental world-space diffuse irradiance cache. Stored RGB is E/pi at
// a secondary surface, BEFORE its albedo, with its own emission excluded.
// No reads from the current write bank; atomic ownership selects one writer.
struct IrradianceEntry {vec4 positionCount;vec4 normalFrame;uvec4 identityBudget;vec4 irradiance;};
layout(std430,binding=46) readonly buffer PreviousIrradiance {IrradianceEntry previousIrradiance[];};
layout(std430,binding=47) writeonly buffer CurrentIrradiance {IrradianceEntry currentIrradiance[];};
layout(std430,binding=48) buffer IrradianceOwnership {uint irradianceOwnership[];};
const float IC_CELL=0.5;
const uint IC_SLOTS=65536u;
bool useDiffuseIrradianceCache(){
    uint flags=uint(temporal.giOptions.w);
    return diffuseIrradianceCache&&!restirPathTracing&&temporal.rtxdiParams.x<0.0&&
        (flags&31u)==0u&&(flags&16384u)==0u;
}
uint irradianceSlot(Surface s,int budget){
    ivec3 cell=ivec3(floor(s.position/IC_CELL));
    uint h=hashRandom(uint(cell.x)^hashRandom(uint(cell.y))^hashRandom(uint(cell.z)));
    h=hashRandom(h^s.hitIdentity.x^hashRandom(s.hitIdentity.y)^hashRandom(s.hitIdentity.z)^uint(budget));
    return h&(IC_SLOTS-1u);
}
bool irradianceMatches(IrradianceEntry e,Surface s,int budget){
    uint age=(uint(temporal.options.x)-uint(e.normalFrame.w))&65535u;
    vec3 delta=s.position-e.positionCount.xyz;
    return e.positionCount.w>0.0&&age<=64u&&
        all(equal(e.identityBudget,uvec4(s.hitIdentity,uint(budget))))&&
        all(equal(ivec3(floor(e.positionCount.xyz/IC_CELL)),ivec3(floor(s.position/IC_CELL))))&&
        dot(e.normalFrame.xyz,s.normal)>.999&&
        dot(delta,delta)<IC_CELL*IC_CELL*.25&&
        abs(dot(delta,s.geometricNormal))<.005;
}
vec4 sampleDiffuseGI(Surface receiver,inout RaySampleState state,inout RaySampleState sunState,int remainingBounces,int remainingMirrors,
    out GICandidate candidate,out vec3 unreused){
    if(!useDiffuseIrradianceCache()||remainingBounces<=0)
        return sampleDiffuseGIUncached(receiver,state,sunState,remainingBounces,remainingMirrors,candidate,unreused,primaryNeeMisRatio());
    // Preserve the first GI visibility sample and its actual NRD hit distance.
    RaySampleState original=state;
    vec3 direction=diffuseDirection(receiver.normal,state);
    Surface vertex;
    candidate=GICandidate(vec3(0),vec3(0),vec3(0),0.0);unreused=vec3(0);
    if(dot(direction,receiver.geometricNormal)<=0.0)return vec4(0,0,0,65504);
    if(!traceSurface(receiver.position+receiver.geometricNormal*rayOffset(receiver.position),direction,100000,vertex)){
        // A miss already has its complete answer. Never trace the same ray twice.
        unreused=rtClampFirefly(pc.extent.z>.5?skyBackground(direction):vec3(0),FIREFLY_GI);
        return vec4(unreused,65504);
    }
    if(vertex.mirror){
        state=original;
        return sampleDiffuseGIUncached(receiver,state,sunState,remainingBounces,remainingMirrors,candidate,unreused,primaryNeeMisRatio());
    }
    float hitDistance=min(length(vertex.position-receiver.position),65504.0);
    float pdf=max(dot(receiver.normal,direction),0.0)/RT_PI;
    float emissionMIS=all(lessThanEqual(vertex.emissive,vec3(0)))?1.0:
        powerHeuristic(pdf,primaryNeeMisRatio()*emitterHitPdf(receiver.position,receiver.normal,vertex));
    unreused=rtClampFirefly(vertex.emissive*emissionMIS,FIREFLY_GI);
    uint slot=irradianceSlot(vertex,remainingBounces);
    IrradianceEntry prior=previousIrradiance[slot];
    bool hit=irradianceMatches(prior,vertex,remainingBounces);
    if(diffuseIrradianceStats){atomicAdd(irradianceOwnership[IC_SLOTS],1u);if(hit)atomicAdd(irradianceOwnership[IC_SLOTS+1u],1u);}
    // Refresh selection is independent of the path RNG and its sampled value.
    uint selector=hashRandom(rtPixel.x+uint(pc.extent.x)*rtPixel.y^hashRandom(uint(temporal.options.x)));
    bool refresh=!hit||(selector&7u)==0u;
    vec3 illumination=hit?prior.irradiance.rgb:vec3(0);
    if(refresh){
        if(diffuseIrradianceStats)atomicAdd(irradianceOwnership[IC_SLOTS+2u],1u);
        vec3 fresh=(sampleSunIrradiance(vertex,sunState)+sampleEmissiveLights(vertex,state,remainingBounces>1))/RT_PI;
        if(remainingBounces>1){
            GICandidate ignored;vec3 emission;
            fresh+=sampleDiffuseGIUncached(vertex,state,sunState,remainingBounces-1,remainingMirrors,ignored,emission,1.0).rgb;
        }else fresh+=sampleVisibleSky(vertex,state);
        fresh=rtClampFirefly(fresh,FIREFLY_GI);
        // Accumulate material-independent irradiance, never primary direct light.
        float count=hit?min(prior.positionCount.w+1.0,32.0):1.0;
        vec3 filtered=hit?mix(prior.irradiance.rgb,fresh,1.0/count):fresh;
        if(atomicCompSwap(irradianceOwnership[slot],0u,1u)==0u){
            if(diffuseIrradianceStats)atomicAdd(irradianceOwnership[IC_SLOTS+3u],1u);
            currentIrradiance[slot]=IrradianceEntry(vec4(vertex.position,count),
                vec4(vertex.normal,temporal.options.x),uvec4(vertex.hitIdentity,uint(remainingBounces)),vec4(filtered,0));
        }
        // The current pixel also receives the accumulated estimate.
        illumination=filtered;
    }
    vec3 radiance=rtClampFirefly(vertex.albedo*(1.0-vertex.metallic)*illumination,FIREFLY_GI);
    candidate=GICandidate(vertex.position,vertex.geometricNormal,radiance,pdf);
    return vec4(radiance+unreused,hitDistance);
}
