// Brick TLAS custom indices address DDA records directly. Hardware supplies
// grid-space object rays; surface face masks remove all full-grid reads.
DdaHit intersectHardwareBrick(uint record,uint primitive,vec3 o,vec3 d,float limit,bool cameraRay){
    DdaHit miss=DdaHit(false,limit,0u,0u,0u,0u);
    const uint edge=1u<<brickShift,headerWords=brickShift==3u?18u:4u,wordsPerBrick=headerWords+(1u<<(brickShift*3u-1u));
    uint b=16u+record*24u,base=ddaWords[b+23u]+primitive*wordsPerBrick;
    uint info=ddaWords[base],tight=ddaWords[base+1u];
    ivec3 brickOrigin=ivec3(info&255u,(info>>8)&255u,(info>>16)&255u);
    ivec3 minimum=brickOrigin+ivec3(tight&7u,(tight>>3)&7u,(tight>>6)&7u);
    ivec3 maximum=brickOrigin+ivec3((tight>>9)&7u,(tight>>12)&7u,(tight>>15)&7u)+ivec3(1);
    bvec3 parallel=lessThan(abs(d),vec3(1e-20));
    if(any(bvec3(parallel.x&&(o.x<float(minimum.x)||o.x>=float(maximum.x)),parallel.y&&(o.y<float(minimum.y)||o.y>=float(maximum.y)),parallel.z&&(o.z<float(minimum.z)||o.z>=float(maximum.z)))))return miss;
    vec3 inv=1.0/mix(d,vec3(1e-20),parallel);
    vec3 slab0=(vec3(minimum)-o)*inv,slab1=(vec3(maximum)-o)*inv;
    vec3 nearT=mix(min(slab0,slab1),vec3(-1e30),parallel),farT=mix(max(slab0,slab1),vec3(1e30),parallel);
    float enter=max(nearT.x,max(nearT.y,nearT.z)),leave=min(limit,min(farT.x,min(farT.y,farT.z)));
    int axis=nearT.x>=nearT.y&&nearT.x>=nearT.z?0:(nearT.y>=nearT.z?1:2);
    leave+=max(1e-5,abs(leave)*1e-6);
    float t=max(enter,.001);if(t>leave||t>=limit)return miss;
    bool entered=enter>=.001;
    ivec3 stepCell=ivec3(sign(d)),cell=clamp(ivec3(floor(o+t*d+vec3(stepCell)*1e-4)),minimum,maximum-ivec3(1));
    vec3 nextT=mix((vec3(cell+max(stepCell,ivec3(0)))-o)*inv,vec3(1e30),parallel),delta=abs(inv);
    [[dont_unroll]] for(uint iteration=0u;iteration<3u*edge+2u&&t<=leave&&t<limit;++iteration){
        if(quadQueryStats)++brickTraceSteps;
        ivec3 local=cell-brickOrigin;if(any(lessThan(local,ivec3(0)))||any(greaterThanEqual(local,ivec3(edge))))break;
        uint coarse=brickShift<3u?0u:uint((local.x>>2)|((local.y>>2)<<1)|((local.z>>2)<<2));
        if(brickShift==3u&&(info&(1u<<(24u+coarse)))==0u){
            ivec3 block=(cell>>2)<<2;
            vec3 boundary=mix((vec3(block+ivec3(greaterThan(stepCell,ivec3(0)))*4)-o)*inv,vec3(1e30),parallel);
            float crossing=min(boundary.x,min(boundary.y,boundary.z));axis=boundary.x<=boundary.y&&boundary.x<=boundary.z?0:(boundary.y<=boundary.z?1:2);
            t=crossing;entered=true;cell=clamp(ivec3(floor(o+t*d)),block,block+ivec3(3));
            cell=mix(cell,block+mix(ivec3(-1),ivec3(4),greaterThan(stepCell,ivec3(0))),lessThanEqual(boundary,vec3(crossing)));
            nextT=mix((vec3(cell+max(stepCell,ivec3(0)))-o)*inv,vec3(1e30),parallel);
            continue;
        }
        float crossing=min(nextT.x,min(nextT.y,nextT.z));
        int exitAxis=nextT.x<=nextT.y&&nextT.x<=nextT.z?0:(nextT.y<=nextT.z?1:2);
        uint micro=brickShift==1u?uint(local.x|(local.y<<1)|(local.z<<2)):uint((local.x&3)|((local.y&3)<<2)|((local.z&3)<<4));
        if((ddaWords[base+2u+coarse*2u+(micro>>5)]&(1u<<(micro&31u)))!=0u){
            uint i=uint(local.x|(local.y<<int(brickShift))|(local.z<<int(brickShift*2u)));
            uint data=(ddaWords[base+headerWords+i/2u]>>((i&1u)*16u))&65535u;
            uint face=uint(axis*2)+uint(stepCell[axis]<0);
            if(entered&&(data&(1u<<(8u+face)))!=0u)return DdaHit(true,t,record,ddaWords[b+21u],data&255u,face);
            face=uint(exitAxis*2)+uint(stepCell[exitAxis]>0);
            if(!cameraRay&&crossing<limit&&crossing<=leave&&(data&(1u<<(8u+face)))!=0u)
                return DdaHit(true,crossing,record,ddaWords[b+21u],data&255u,face);
        }
        axis=exitAxis;entered=true;
        bvec3 advance=lessThanEqual(nextT,vec3(crossing));cell+=ivec3(advance)*stepCell;nextT+=vec3(advance)*delta;t=crossing;
    }
    return miss;
}
