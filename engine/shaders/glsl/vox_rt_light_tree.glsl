// Binding 20 retains its original header/light prefix. All tail offsets are
// uint bit patterns in header.yzw; no float conversion of addresses or keys.
const uint LIGHT_TREE_LEAF=0x80000000u,LIGHT_TREE_SELECTORS=16777216u;
struct EmitterTreeNode {vec3 center;float mass;uint child;uint leaves;uint parent;float radius2;};
EmitterTreeNode emitterTreeNode(uint at){
    uint base=floatBitsToUint(emissiveLights[0].y)+at*2u;
    vec4 a=emissiveLights[base],b=emissiveLights[base+1u];uvec3 words=floatBitsToUint(b.xyz);
    return EmitterTreeNode(a.xyz,a.w,words.x,words.y,words.z,b.w);
}
bool useEmitterTree(){return neeTreeProposal&&temporal.rtxdiParams.x<0.0&&floatBitsToUint(emissiveLights[0].y)!=0u;}
float emitterTreeDistance(EmitterTreeNode node,vec3 point){
    if(neeTreeQuality&&(node.child&LIGHT_TREE_LEAF)!=0u){
        uint base=1u+(node.child&~LIGHT_TREE_LEAF)*4u;
        vec3 a=emissiveLights[base+1u].xyz,b=emissiveLights[base+2u].xyz,c=emissiveLights[base+3u].xyz;
        vec3 lo=min(a,min(b,c)),hi=max(a,max(b,c));
        vec3 delta=max(max(lo-point,point-hi),vec3(0));
        // The finite area floor avoids singular proposals on an emitter plane.
        return max(dot(delta,delta),max(emissiveLights[base].w*0.25,1e-12));
    }
    vec3 delta=node.center-point;
    return max(neeTreePrime?dot(delta,delta):max(dot(delta,delta),node.radius2),1e-12);
}
uint emitterTreeLeft(EmitterTreeNode a,EmitterTreeNode b,vec3 point,uint count){
    float dA=emitterTreeDistance(a,point),dB=emitterTreeDistance(b,point);
    float ds=max(dA,dB),ps=max(a.mass,b.mass);
    float wa=(a.mass/ps)*(dB/ds),wb=(b.mass/ps)*(dA/ds);
    float p=clamp(wa/(wa+wb),1e-6,1.0-1e-6);
    return clamp(uint(floor(float(count)*p+.5)),a.leaves,count-b.leaves);
}
uint selectEmitterTree(vec3 point,float u,out float probability){
    uint target=min(uint(u*float(LIGHT_TREE_SELECTORS)),LIGHT_TREE_SELECTORS-1u);
    uint at=0u,first=0u,count=LIGHT_TREE_SELECTORS;
    for(uint depth=0u;depth<=24u;++depth){
        EmitterTreeNode node=emitterTreeNode(at);
        if((node.child&LIGHT_TREE_LEAF)!=0u){probability=float(count)/float(LIGHT_TREE_SELECTORS);return node.child&~LIGHT_TREE_LEAF;}
        uint left=emitterTreeLeft(emitterTreeNode(node.child),emitterTreeNode(node.child+1u),point,count);
        if(target<first+left){at=node.child;count=left;}else{at=node.child+1u;first+=left;count-=left;}
    }
    probability=0.0;return 0u;
}
float emitterTreeProbability(vec3 point,uint emitter){
    uint leafOffset=floatBitsToUint(emissiveLights[0].z);
    uint at=floatBitsToUint(emissiveLights[leafOffset+emitter].x),trail=0u,depth=0u;
    // Build leaf-to-root trail, then replay exactly the forward integer splits.
    for(uint i=0u;i<24u&&at!=0u;++i){EmitterTreeNode node=emitterTreeNode(at),parent=emitterTreeNode(node.parent);
        trail=(trail<<1u)|uint(at==parent.child+1u);at=node.parent;++depth;}
    uint count=LIGHT_TREE_SELECTORS;at=0u;
    for(uint i=0u;i<depth;++i){EmitterTreeNode node=emitterTreeNode(at);
        uint left=emitterTreeLeft(emitterTreeNode(node.child),emitterTreeNode(node.child+1u),point,count);
        bool right=((trail>>i)&1u)!=0u;count=right?count-left:left;at=node.child+uint(right);}
    return float(count)/float(LIGHT_TREE_SELECTORS);
}
float emitterPrimeProbability(vec3 point,uint emitter){
    return 0.5*(emitterTreeProbability(point,emitter)+emissiveLights[3u+emitter*4u].w);
}
uint selectEmitterPrimeMixture(vec3 point,float branch,float selector,out float probability){
    uint chosen;float treeProbability;
    if(branch<0.5){
        uint count=uint(emissiveLights[0].x),lo=0u,hi=count;
        while(lo<hi){uint mid=lo+(hi-lo)/2u;if(selector<emissiveLights[2u+mid*4u].w)hi=mid;else lo=mid+1u;}
        chosen=min(lo,count-1u);treeProbability=emitterTreeProbability(point,chosen);
    }else chosen=selectEmitterTree(point,selector,treeProbability);
    probability=0.5*(treeProbability+emissiveLights[3u+chosen*4u].w);return chosen;
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
