// Header.w points to the sorted (instance, geometry, primitive) emitter index.
// The CDF and its represented interval are written by BuildEmissiveAreaDistribution.
uint selectEmitterCdf(float u,out float probability){
    uint count=uint(emissiveLights[0].x);
    if(count==0u){probability=0.0;return 0xffffffffu;}
    uint lo=0u,hi=count;
    while(lo<hi){uint mid=lo+(hi-lo)/2u;if(u<emissiveLights[2u+mid*4u].w)hi=mid;else lo=mid+1u;}
    uint chosen=min(lo,count-1u);probability=emissiveLights[3u+chosen*4u].w;return chosen;
}
bool emitterKeyLess(uvec3 a,uvec3 b){return a.x!=b.x?a.x<b.x:(a.y!=b.y?a.y<b.y:a.z<b.z);}
uint emitterForHit(uvec3 identity){
    uint base=floatBitsToUint(emissiveLights[0].w),count=uint(emissiveLights[0].x);
    if(base==0u)return 0xffffffffu;
    uint lo=0u,hi=count;
    while(lo<hi){uint mid=lo+(hi-lo)/2u;uvec4 key=floatBitsToUint(emissiveLights[base+mid]);
        if(emitterKeyLess(key.xyz,identity))lo=mid+1u;else hi=mid;}
    if(lo>=count)return 0xffffffffu;
    uvec4 key=floatBitsToUint(emissiveLights[base+lo]);return all(equal(key.xyz,identity))?key.w:0xffffffffu;
}
