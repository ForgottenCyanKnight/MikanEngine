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
struct EWork {EReplay replay;EShift shift;uvec4 metadata;}; // 128 B std430
layout(std430,binding=49) buffer EnhancedQueue {uint eDispatchX;uint eDispatchY;uint eDispatchZ;uint eQueueCount;uint eItems[];};
layout(std430,binding=50) buffer EnhancedWork {EWork eWork[];};
layout(std430,binding=51) readonly buffer EnhancedPairs {uint ePairs[];};
layout(std430,binding=52) buffer EnhancedDuplicates {uint eDuplicates[];};
shared uvec2 eSeedTile[576];
shared uint eLaneCounts[64];
shared uint eGroupBase;
const uint E_PAIR_SIZE[3]=uint[3](254u,232u,196u);
ivec2 ePixel(uint index){return ivec2(index%uint(pc.extent.x),index/uint(pc.extent.x));}
uint eAddress(ivec2 pixel){return uint(pixel.y)*uint(pc.extent.x)+uint(pixel.x);}
bool eInBounds(ivec2 p){return all(greaterThanEqual(p,ivec2(0)))&&all(lessThan(p,ivec2(pc.extent.xy)));}
uint ePrimaryAddress(uint pixel,bool previous){
    uint pixels=uint(pc.extent.x)*uint(pc.extent.y),readBank=(uint(temporal.samplingParams.z)>>1u)&1u;
    return pixels+((previous?readBank:1u-readBank)*pixels+pixel)*(primarySunMode>=3u?12u:5u);
}
void eStorePrimary(uint pixel,Surface s){
    uint at=ePrimaryAddress(pixel,false);
    if(primarySunMode>=3u){
        eDuplicates[at]=floatBitsToUint(s.albedo.x);eDuplicates[at+1u]=floatBitsToUint(s.albedo.y);eDuplicates[at+2u]=floatBitsToUint(s.albedo.z);
        eDuplicates[at+3u]=giPackNormal(s.normal);eDuplicates[at+4u]=giPackNormal(s.geometricNormal);
        eDuplicates[at+5u]=packHalf2x16(vec2(s.metallic,s.roughness));
        eDuplicates[at+6u]=floatBitsToUint(s.emissive.x);eDuplicates[at+7u]=floatBitsToUint(s.emissive.y);eDuplicates[at+8u]=floatBitsToUint(s.emissive.z);
        eDuplicates[at+9u]=uint(s.specularEnabled);eDuplicates[at+10u]=floatBitsToUint(s.transmission);eDuplicates[at+11u]=0u;return;
    }
    uvec4 hit=eHit(s);
    eDuplicates[at]=hit.x;eDuplicates[at+1u]=hit.y;eDuplicates[at+2u]=hit.z;
    eDuplicates[at+3u]=hit.w;eDuplicates[at+4u]=floatBitsToUint(s.hitBary.y);
}
bool ePreviousReceiver(PTReservoir source,out Surface primary,out vec3 incoming){
    uint at=ePrimaryAddress(eAddress(ivec2(source.seedPixelNormal.yz)),true);
    if(primarySunMode>=3u){
        incoming=-source.primaryViewFootprint.xyz;
        primary.position=source.receiverM.xyz;primary.previousPosition=primary.position;
        primary.albedo=uintBitsToFloat(uvec3(eDuplicates[at],eDuplicates[at+1u],eDuplicates[at+2u]));
        vec2 oct=unpackSnorm2x16(eDuplicates[at+3u]);vec3 n=vec3(oct,1.0-abs(oct.x)-abs(oct.y));
        if(n.z<0.0)n.xy=(1.0-abs(n.yx))*vec2(n.x>=0.0?1.0:-1.0,n.y>=0.0?1.0:-1.0);
        primary.normal=normalize(n);primary.previousNormal=primary.normal;
        oct=unpackSnorm2x16(eDuplicates[at+4u]);n=vec3(oct,1.0-abs(oct.x)-abs(oct.y));
        if(n.z<0.0)n.xy=(1.0-abs(n.yx))*vec2(n.x>=0.0?1.0:-1.0,n.y>=0.0?1.0:-1.0);
        primary.geometricNormal=normalize(n);vec2 mr=unpackHalf2x16(eDuplicates[at+5u]);primary.metallic=mr.x;primary.roughness=mr.y;
        primary.emissive=uintBitsToFloat(uvec3(eDuplicates[at+6u],eDuplicates[at+7u],eDuplicates[at+8u]));
        primary.specularEnabled=eDuplicates[at+9u]!=0u;primary.transmission=uintBitsToFloat(eDuplicates[at+10u]);primary.mirror=false;
        primary.hitIdentity=uvec3(E_INVALID);primary.hitPrimitive=E_INVALID;primary.hitBary=vec2(0);return true;
    }
    uvec4 hit=uvec4(eDuplicates[at],eDuplicates[at+1u],eDuplicates[at+2u],eDuplicates[at+3u]);
    incoming=-source.primaryViewFootprint.xyz;
    return eLoad(hit,uintBitsToFloat(eDuplicates[at+4u]),source.receiverM.xyz,-incoming,primary)&&!primary.mirror;
}
void eDuplicateMap(){
    ivec2 base=ivec2(gl_WorkGroupID.xy)*8-ivec2(8);
    for(uint i=gl_LocalInvocationIndex;i<576u;i+=64u){ivec2 p=base+ivec2(i%24u,i/24u);
        if(eInBounds(p)){PTReservoir r=ptPrevious[eAddress(p)];eSeedTile[i]=uvec2(r.path.y,uint(r.receiverM.w>0.0));}
        else eSeedTile[i]=uvec2(0);}
    barrier();
    ivec2 pixel=ivec2(gl_GlobalInvocationID.xy);if(!eInBounds(pixel))return;
    uint center=(gl_LocalInvocationID.y+8u)*24u+gl_LocalInvocationID.x+8u,seed=eSeedTile[center].x,count=0u;
    for(int y=-8;y<=8;++y)for(int x=-8;x<=8;++x){if(x==0&&y==0)continue;
        uvec2 neighbor=eSeedTile[int(center)+y*24+x];count+=uint(neighbor.y!=0u&&neighbor.x==seed);}
    eDuplicates[eAddress(pixel)]=count;
}
ivec2 eNeighbor(ivec2 pixel,uint neighbor){
    uint r=eSeed(uvec2(pc.extent.xy)+1u,4u*(uint(temporal.options.x)+uint(temporal.samplingParams.w))+3u);
    r=1664525u*r+1013904223u;ivec2 offset=ivec2(int(r&32767u)%int(pc.extent.x),int((r>>15u)&16383u)%int(pc.extent.y));
    bool flipX=(r&(1u<<29u))!=0u,flipY=(r&(1u<<30u))!=0u,transpose=(r&(1u<<31u))!=0u;
    int size=int(E_PAIR_SIZE[neighbor]);ivec2 p=(pixel+offset)%size;
    if(flipX)p.x=size-1-p.x;if(flipY)p.y=size-1-p.y;if(transpose)p=p.yx;
    uint packed=ePairs[neighbor*65536u+uint(p.y*size+p.x)];ivec2 delta=ivec2(int(packed&255u)-128,int((packed>>8u)&255u)-128);
    if(transpose)delta=delta.yx;if(flipX)delta.x=-delta.x;if(flipY)delta.y=-delta.y;
    return pixel+delta;
}
bool eCompatible(Surface a,Surface b){
    float da=distance(a.position,pc.eye.xyz),db=distance(b.position,pc.eye.xyz);
    return !a.mirror&&!b.mirror&&dot(a.normal,b.normal)>=.5&&abs(da-db)<.1*max(da,db);
}
void eEnqueue(uint slot,PTReservoir source){
    if(eRcLength(source)>1u)eWork[slot].metadata.z=1u;
}
// Workgroup prefix reservation needs no optional subgroup-arithmetic capability.
// Every lane, including out-of-viewport lanes, reaches every barrier.
void eCompactGroup(ivec2 pixel){
    uint lane=gl_LocalInvocationIndex,mask=0u;
    if(eInBounds(pixel))for(uint i=0u;i<3u;++i)if(eWork[eAddress(pixel)*3u+i].metadata.z!=0u)mask|=1u<<i;
    uint count=bitCount(mask);eLaneCounts[lane]=count;barrier();
    for(uint offset=1u;offset<64u;offset*=2u){
        uint add=lane>=offset?eLaneCounts[lane-offset]:0u;barrier();
        eLaneCounts[lane]+=add;barrier();
    }
    if(lane==0u){uint total=eLaneCounts[63];eGroupBase=0u;
        if(total>0u){eGroupBase=atomicAdd(eQueueCount,total);uint end=eGroupBase+total;
            atomicMax(eDispatchX,min((end+63u)/64u,65535u));atomicMax(eDispatchY,(end-1u)/(65535u*64u)+1u);}}
    barrier();
    uint at=eGroupBase+eLaneCounts[lane]-count;
    for(uint i=0u;i<3u;++i)if((mask&(1u<<i))!=0u)eItems[at++]=eAddress(pixel)*3u+i;
}
void eWorkload(ivec2 pixel,bool temporalPass){
    uint address=eAddress(pixel);
    // Only metadata is unconditional. A valid retrace writes its replay and
    // a valid spatial shift writes its result before either can be consumed.
    // Direct connections reconstruct their trivial prefix in registers.
    for(uint i=0u;i<3u;++i)eWork[address*3u+i].metadata=uvec4(E_INVALID,0,0,0);
    Surface primary;if(!surfaceAtPixel(pixel,primary)||primary.mirror)return;
    if(temporalPass){
        ivec2 previous;if(!ptTemporalPixel(primary,previous))return;
        uint donorAddress=eAddress(previous);PTReservoir donor=ptPrevious[donorAddress];
        // The merge (and reverse retrace when queued) validates the cached
        // previous receiver. Do not decode its full material twice here.
        if(donor.receiverM.w<=0.0)return;
        for(uint i=0u;i<2u;++i)eWork[address*3u+i].metadata=uvec4(donorAddress,1,0,0);
        eEnqueue(address*3u,donor);
        eEnqueue(address*3u+1u,ptFresh[address]);
    }else{
        for(uint i=0u;i<3u;++i){
            ivec2 p=eNeighbor(pixel,i);Surface other;
            if(!eInBounds(p)||!surfaceAtPixel(p,other)||!eCompatible(primary,other))continue;
            uint neighbor=eAddress(p);PTReservoir donor=ptTemporal[neighbor];if(donor.receiverM.w<=0.0)continue;
            eWork[address*3u+i].metadata=uvec4(neighbor,1,0,0);
            eEnqueue(address*3u+i,donor);
        }
    }
}
void eRetrace(uint invocation,bool temporalPass){
    if(invocation>=eQueueCount)return;
    uint slot=eItems[invocation],address=slot/3u,candidate=slot%3u;
    uint donorAddress=eWork[slot].metadata.x;Surface primary;vec3 view,eye=pc.eye.xyz;PTReservoir source;
    if(temporalPass&&candidate==1u){
        PTReservoir donor=ptPrevious[donorAddress];vec3 incoming;
        if(!ePreviousReceiver(donor,primary,incoming)){eWork[slot].replay=eEmptyReplay();return;}
        view=-incoming;eye=inverse(temporal.previousView)[3].xyz;source=ptFresh[address];
    }else{
        if(!surfaceAtPixel(ePixel(address),primary)){eWork[slot].replay=eEmptyReplay();return;}
        view=normalize(pc.eye.xyz-primary.position);source=temporalPass?ptPrevious[donorAddress]:ptTemporal[donorAddress];
    }
    eWork[slot].replay=eReplay(primary,view,eye,source);
}
void eSpatialShift(ivec2 pixel){
    uint address=eAddress(pixel);Surface primary;if(!surfaceAtPixel(pixel,primary)||primary.mirror)return;
    vec3 view=normalize(pc.eye.xyz-primary.position);
    for(uint i=0u;i<3u;++i){uint slot=address*3u+i;if(eWork[slot].metadata.y==0u)continue;
        PTReservoir source=ptTemporal[eWork[slot].metadata.x];
        EReplay replay=eRcLength(source)<=1u?eDirectReplay(primary,view):eWork[slot].replay;
        eWork[slot].shift=eShift(primary,view,pc.eye.xyz,source,replay);}
}
PTReservoir eTemporalMerge(Surface primary,vec3 incoming,ivec2 pixel){
    uint address=eAddress(pixel);PTReservoir canonical=ptFresh[address];uint slot=address*3u;
    if(temporal.samplingParams.y<.5)return canonical;
    if(eWork[slot].metadata.y==0u)return canonical;
    uint donorAddress=eWork[slot].metadata.x;PTReservoir donor=ptPrevious[donorAddress];
    float cap=enhancedDuplicateCap?mix(20.0,1.0,pow(float(eDuplicates[donorAddress])/288.0,.1)):20.0;
    donor.receiverM.w=min(donor.receiverM.w,cap);
    Surface old;vec3 oldIncoming;if(!ePreviousReceiver(donor,old,oldIncoming))return canonical;
    EReplay forwardReplay=eRcLength(donor)<=1u?eDirectReplay(primary,-incoming):eWork[slot].replay;
    EReplay reverseReplay=eRcLength(canonical)<=1u?eDirectReplay(old,-oldIncoming):eWork[slot+1u].replay;
    EShift forward=eShift(primary,-incoming,pc.eye.xyz,donor,forwardReplay);
    EShift reverse=eShift(old,-oldIncoming,inverse(temporal.previousView)[3].xyz,canonical,reverseReplay);
    float c=ptTarget(canonical)*canonical.receiverM.w,rev=eShiftTarget(reverse)*donor.receiverM.w;
    float canonicalMis=rev>0.0?c/max(c+rev,1e-30):1.0;
    float d=ptTarget(donor)*donor.receiverM.w,donorMis=d/max(d+eShiftTarget(forward)*canonical.receiverM.w,1e-30);
    PTReservoir result=ptEmpty();float sum=0.0;uint rng=eSeed(uvec2(pixel),4u*(uint(temporal.options.x)+uint(temporal.samplingParams.w))+1u);
    eMerge(result,donor,forward,donorMis,sum,rng);
    eMerge(result,canonical,EShift(vec4(canonical.diffuseWeight.rgb,1),vec4(canonical.specularDistance.rgb,canonical.incidentGeometry.w)),canonicalMis,sum,rng);
    eFinish(result,canonical,sum);return result;
}
PTReservoir eSpatialMerge(ivec2 pixel){
    uint address=eAddress(pixel);PTReservoir canonical=ptTemporal[address];float total=canonical.receiverM.w;
    if((uint(temporal.samplingParams.z)&1u)==0u)return canonical;
    for(uint i=0u;i<3u;++i)if(eWork[address*3u+i].metadata.y!=0u)total+=ptTemporal[eWork[address*3u+i].metadata.x].receiverM.w;
    if(total<=canonical.receiverM.w)return canonical;
    float canonicalMis=canonical.receiverM.w/total,otherMass=total-canonical.receiverM.w,sum=0.0;
    uint rng=eSeed(uvec2(pixel),4u*(uint(temporal.options.x)+uint(temporal.samplingParams.w))+3u);PTReservoir result=ptEmpty();
    for(uint i=0u;i<3u;++i){uint slot=address*3u+i;if(eWork[slot].metadata.y==0u)continue;
        uint neighbor=eWork[slot].metadata.x;PTReservoir donor=ptTemporal[neighbor];
        // Reciprocal LUT guarantees the reverse mapping is the same neighbor slot.
        EShift forward=eWork[slot].shift;
        EShift reverse=eWork[neighbor*3u+i].metadata.y!=0u?eWork[neighbor*3u+i].shift:eZeroShift();
        float rev=eShiftTarget(reverse)*otherMass,c=ptTarget(canonical)*canonical.receiverM.w;
        canonicalMis+=donor.receiverM.w/total*(rev>0.0?c/max(c+rev,1e-30):1.0);
        float d=ptTarget(donor)*otherMass,mis=donor.receiverM.w/total*d/max(d+eShiftTarget(forward)*canonical.receiverM.w,1e-30);
        eMerge(result,donor,forward,mis,sum,rng);
    }
    eMerge(result,canonical,EShift(vec4(canonical.diffuseWeight.rgb,1),vec4(canonical.specularDistance.rgb,canonical.incidentGeometry.w)),canonicalMis,sum,rng);
    eFinish(result,canonical,sum);return result;
}
PTReservoir eStage(Surface receiver,vec3 incoming,ivec2 pixel,int budget,bool reusable){
    uint address=eAddress(pixel);
    if(enhancedPass==0u){eStorePrimary(address,receiver);PTReservoir result=eGenerate(receiver,incoming,pixel,budget);ptFresh[address]=result;return result;}
    if(enhancedPass==3u){PTReservoir result=reusable?eTemporalMerge(receiver,incoming,pixel):ptFresh[address];ptTemporal[address]=result;return result;}
    if(enhancedPass==7u){PTReservoir result=reusable?eSpatialMerge(pixel):ptTemporal[address];ptFinal[address]=result;return result;}
    return ptFinal[address];
}
bool eDispatch(){
    if(enhancedPass==9u){eDuplicateMap();return true;}
    if(enhancedPass==2u||enhancedPass==5u){eRetrace((gl_WorkGroupID.y*65535u+gl_WorkGroupID.x)*64u+gl_LocalInvocationIndex,enhancedPass==2u);return true;}
    if(enhancedPass==1u||enhancedPass==4u){ivec2 pixel=ivec2(gl_GlobalInvocationID.xy);
        if(eInBounds(pixel))eWorkload(pixel,enhancedPass==1u);eCompactGroup(pixel);return true;}
    ivec2 pixel=ivec2(gl_GlobalInvocationID.xy);if(!eInBounds(pixel))return true;
    if(enhancedPass==6u){eSpatialShift(pixel);return true;}
    return false;
}
