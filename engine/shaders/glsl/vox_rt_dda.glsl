// Header: 16 words, then 24 words per instance (worldToGrid mat4,
// uvec4(dimensions,texture), uvec4(paletteOffset,hitInstance,shadowMask,0)).
// Full source palettes follow; RGB is sRGB, w is the canonical material word.
// The tail holds the broadphase: 8-word world-space AABBs per record, then
// 8-word AABBs per 16-record chunk. Header words 13/14/15 = record bounds
// base / chunk bounds base / chunk count (word 13 == 0 when absent).
// Array size must match kVoxelDdaMaxGrids (include/Rendering/VoxelDDARegistry.h)
// and the binding-39 descriptor counts in RayTracingViewport(Resources).
layout(binding=39) uniform utexture3D ddaGridTex[512];
#include "vox_rt_dda_data.glsl"
struct DdaHit {bool hit;float t;uint record;uint index;uint voxel;uint face;};
#ifdef MIKAN_RUNTIME_DDA_OPTIMIZED
// This routine is reached by several lighting paths. Keep dynamic traversals
// as loops rather than cloning their bodies during driver pipeline compilation.
#define DDA_TRAVERSAL_LOOP [[dont_unroll]]
#else
#define DDA_TRAVERSAL_LOOP
#endif
uint ddaTexel(uint slot,ivec3 cell){return texelFetch(ddaGridTex[nonuniformEXT(slot)],cell,0).r;}
#if DDA_MORTON_MIP
uint ddaSpreadMortonBits(uint value){
    value=(value|(value<<16u))&0x030000ffu;value=(value|(value<<8u))&0x0300f00fu;
    value=(value|(value<<4u))&0x030c30c3u;return (value|(value<<2u))&0x09249249u;
}
uint ddaMortonIndex(uvec3 cell){return ddaSpreadMortonBits(cell.x)|(ddaSpreadMortonBits(cell.y)<<1u)|(ddaSpreadMortonBits(cell.z)<<2u);}
#endif
uint ddaMipOccupancy(uint slot,ivec3 cell,int level){
#if DDA_MIP_BITSET
    uint metadata=16u+24u+256u*4u;
    ivec3 mipDimensions=max(ivec3(ddaWords[1],ddaWords[2],ddaWords[3])>>level,ivec3(1));
    ivec3 mipCell=cell>>level;
#if DDA_MORTON_MIP
    uint linearIndex=ddaMortonIndex(uvec3(mipCell));
#else
    uint linearIndex=uint((mipCell.z*mipDimensions.y+mipCell.y)*mipDimensions.x+mipCell.x);
#endif
    uint bitIndex=ddaWords[metadata+uint(level-1)]+linearIndex;
    return (ddaWords[metadata+8u+(bitIndex>>5u)]>>(bitIndex&31u))&1u;
#elif DDA_MIP_SSBO
    uint metadata=16u+24u+256u*4u;
    uint data=metadata+8u+ddaWords[metadata+uint(level-1)];
    ivec3 mipDimensions=max(ivec3(ddaWords[1],ddaWords[2],ddaWords[3])>>level,ivec3(1));
    ivec3 mipCell=cell>>level;
    uint linearIndex=uint((mipCell.z*mipDimensions.y+mipCell.y)*mipDimensions.x+mipCell.x);
    return ddaWords[data+linearIndex];
#else
    return texelFetch(ddaGridTex[nonuniformEXT(slot)],cell>>level,level).r;
#endif
}
#ifdef MIKAN_RUNTIME_DDA_OPTIMIZED
// Reject only when both endpoints are outside the same screen-cone plane.
// Rays crossing the camera plane fail open; a camera pixel is never used as
// a proxy for secondary-ray visibility. Rectangles include a pixel margin.
bool ddaSecondaryOutside(vec4 rectangle,vec3 origin,vec3 direction,float limit){
    mat4 clipFromWorld=mat4(ddaScreenRects[0],ddaScreenRects[1],ddaScreenRects[2],ddaScreenRects[3]);
    vec4 a=clipFromWorld*vec4(origin,1);
    if(any(isnan(a))||any(isinf(a))||a.w<=1e-4)return false;
    vec2 uv=a.xy/a.w;
    if(all(greaterThanEqual(uv,rectangle.xy))&&all(lessThanEqual(uv,rectangle.zw)))return false;
    vec4 b=a+(clipFromWorld*vec4(direction,0))*limit;
    if(any(isnan(b))||any(isinf(b))||b.w<=1e-4)return false;
    vec4 da=vec4(a.x-rectangle.x*a.w,rectangle.z*a.w-a.x,a.y-rectangle.y*a.w,rectangle.w*a.w-a.y);
    vec4 db=vec4(b.x-rectangle.x*b.w,rectangle.z*b.w-b.x,b.y-rectangle.y*b.w,rectangle.w*b.w-b.y);
    float tolerance=1e-5*max(1.0,max(max(abs(a.x),abs(a.y)),max(max(abs(b.x),abs(b.y)),max(abs(a.w),abs(b.w)))));
    return any(lessThan(max(da,db),vec4(-tolerance)));
}
uint ddaRecordMipOccupancy(uint record,uint slot,ivec3 cell,int level){
    if(!ddaBitsetMips)return ddaMipOccupancy(slot,cell,level);
    uint metadata=ddaWords[16u+record*24u+23u];
    ivec3 dimensions=max(ivec3(ddaWords[metadata],ddaWords[metadata+1u],ddaWords[metadata+2u])>>level,ivec3(1));
    ivec3 c=cell>>level;
    uint linear=uint((c.z*dimensions.y+c.y)*dimensions.x+c.x);
    uint base=metadata+ddaWords[metadata+4u+uint(level-1)];
    return (ddaWords[base+(linear>>5u)]>>(linear&31u))&1u;
}
// Returns a palette id, or an empty cuboid [minimum,maximum) in grid space.
// The CPU constructs the entire cuboid as empty; independent axis distances
// to individual obstacles would not be conservative for diagonal rays.
uint ddaNaadfLookup(uint record,ivec3 cell,out ivec3 minimum,out ivec3 maximum){
    uint m=ddaWords[16u+record*24u+23u];
    ivec3 chunk=cell>>4;
    uint ci=uint((chunk.z*int(ddaWords[m+2u])+chunk.y)*int(ddaWords[m+1u])+chunk.x);
    uint node=ddaWords[m+ddaWords[m+4u]+ci];
    int edge=16,bits=5;ivec3 local=chunk;
    if((node>>30u)==2u){
        ivec3 block=(cell>>2)&3;
        uint bi=uint((block.z*4+block.y)*4+block.x);
        node=ddaWords[m+ddaWords[m+5u]+(node&0x3fffffffu)+bi];
        edge=4;bits=2;local=cell>>2;
        if((node>>30u)==2u){
            ivec3 voxel=cell&3;
            uint vi=(node&0x3fffffffu)+uint((voxel.z*4+voxel.y)*4+voxel.x);
            node=(ddaWords[m+ddaWords[m+6u]+(vi>>1u)]>>((vi&1u)*16u))&0xffffu;
            if((node&0x8000u)!=0u)return node&0x7fffu;
            edge=1;local=cell;
        }else if((node>>30u)==1u)return node&0x3fffffffu;
    }else if((node>>30u)==1u)return node&0x3fffffffu;
    uint mask=(1u<<bits)-1u;
    ivec3 negative=ivec3(node&mask,(node>>uint(2*bits))&mask,(node>>uint(4*bits))&mask);
    ivec3 positive=ivec3((node>>uint(bits))&mask,(node>>uint(3*bits))&mask,(node>>uint(5*bits))&mask);
    minimum=(local-negative)*edge;maximum=(local+positive+1)*edge;
    return 0u;
}
#endif
bool ddaOwnsScene(){
    // CPU emits one DDA record per distinct DDA-owned hit instance. The
    // instance buffer span is at least the live instance count (it may retain
    // spare capacity). Equality therefore proves there is no triangle fallback
    // instance; spare capacity merely disables this shortcut conservatively.
    return neeVoxelDDA&&ddaWords[0]>0u&&ddaWords[0]==uint(instances.length());
}
// World-space ray/AABB slab test over the CPU-built broadphase. direction may
// be unnormalized: t stays in the caller's ray units, matching the precise
// grid-space test (an affine worldToGrid preserves the ray parameter). The
// world AABB of a rotated grid box is conservative, so a pass here never skips
// a record the precise test would accept.
bool ddaSlabIntersects(vec3 origin,vec3 direction,vec3 low,vec3 high,float bestT){
    float enter=-1e30,leave=bestT;bool intersects=true;
    for(int a=0;a<3;++a){
        if(abs(direction[a])<1e-20){if(origin[a]<low[a]||origin[a]>high[a])intersects=false;}
        else{float lo=(low[a]-origin[a])/direction[a],hi=(high[a]-origin[a])/direction[a];
            enter=max(enter,min(lo,hi));leave=min(leave,max(lo,hi));}
    }
    return intersects&&enter<leave&&max(enter,.001)<bestT;
}
DdaHit traceVoxelDDA(vec3 origin,vec3 direction,float tMax,uint mask,bool cameraRay){
    DdaHit best=DdaHit(false,tMax,0u,0u,0u,0u);if(!neeVoxelDDA)return best;
    // Two-level broadphase in the ddaWords tail: header word 13 is the record
    // bounds base (0 = absent, linear scan), 14 the 16-record chunk bounds
    // base, 15 the chunk count. As soon as one grid produces a hit, best.t
    // tightens and whole chunks/records fall out of a single slab test.
    const uint boundsBase=ddaWords[13];
    bool chunkOpen=true;
    DDA_TRAVERSAL_LOOP for(uint record=0u;record<ddaWords[0];++record){
        if(boundsBase!=0u){
            if((record&15u)==0u){
                uint w=ddaWords[14u]+(record>>4u)*8u;
                chunkOpen=ddaSlabIntersects(origin,direction,
                    uintBitsToFloat(uvec3(ddaWords[w],ddaWords[w+1u],ddaWords[w+2u])),
                    uintBitsToFloat(uvec3(ddaWords[w+3u],ddaWords[w+4u],ddaWords[w+5u])),best.t);
            }
            if(!chunkOpen)continue;
            uint w=boundsBase+record*8u;
            if(!ddaSlabIntersects(origin,direction,
                uintBitsToFloat(uvec3(ddaWords[w],ddaWords[w+1u],ddaWords[w+2u])),
                uintBitsToFloat(uvec3(ddaWords[w+3u],ddaWords[w+4u],ddaWords[w+5u])),best.t))continue;
        }
        uint b=16u+record*24u;if(mask==2u&&(ddaWords[b+22u]&2u)==0u)continue;
#ifdef MIKAN_RUNTIME_DDA_OPTIMIZED
        if(ddaScreenBounds){
            uint header=ddaSecondaryScreenBounds?4u:0u;
            vec4 rectangle=ddaScreenRects[header+ddaWords[b+21u]];
            if(cameraRay){
                vec2 uv=(vec2(rtPixel)+.5)/pc.extent.xy*2.0-1.0;
                if(any(lessThan(uv,rectangle.xy))||any(greaterThan(uv,rectangle.zw)))continue;
            }else if(ddaSecondaryScreenBounds&&ddaSecondaryOutside(rectangle,origin,direction,tMax))continue;
        }
#endif
        ivec3 dims=ivec3(ddaWords[b+16u],ddaWords[b+17u],ddaWords[b+18u]);uint slot=ddaWords[b+19u];
        if(slot>=512u||any(lessThanEqual(dims,ivec3(0))))continue;
        mat4 transform=ddaWorldToGrid(record);vec3 o=(transform*vec4(origin,1)).xyz,d=mat3(transform)*direction;
        vec3 inverseDirection=vec3(0);
        for(int a=0;a<3;++a)if(d[a]!=0.0)inverseDirection[a]=1.0/d[a];
        float enter=-1e30,leave=best.t;int entryAxis=0;bool intersects=true;
        for(int a=0;a<3;++a){
            if(abs(d[a])<1e-20){if(o[a]<0.0||o[a]>=float(dims[a]))intersects=false;}
            else{float lo=(0.0-o[a])*inverseDirection[a],hi=(float(dims[a])-o[a])*inverseDirection[a];
                float nearT=min(lo,hi),farT=max(lo,hi);if(nearT>enter){enter=nearT;entryAxis=a;}leave=min(leave,farT);}
        }
        float t=max(enter,.001);if(!intersects||leave<t||t>=best.t)continue;
        ivec3 stepCell=ivec3(sign(d)),cell=ivec3(floor(o+t*d+vec3(stepCell)*1e-4));cell=clamp(cell,ivec3(0),dims-1);
        vec3 delta=vec3(1e30),nextT=vec3(1e30);
#if DDA_VECTOR_FAST || defined(MIKAN_RUNTIME_DDA_OPTIMIZED)
        bvec3 activeAxes=notEqual(stepCell,ivec3(0));
        delta=mix(delta,abs(inverseDirection),activeAxes);
        nextT=mix(nextT,(vec3(cell)+max(vec3(stepCell),vec3(0))-o)*inverseDirection,activeAxes);
#else
        for(int a=0;a<3;++a)if(stepCell[a]!=0){delta[a]=abs(inverseDirection[a]);nextT[a]=(float(cell[a]+max(stepCell[a],0))-o[a])*inverseDirection[a];}
#endif
#if DDA_MIPS_FROM_DIMS
        uint largestDimension=uint(max(dims.x,max(dims.y,dims.z)));
        int mipCount=largestDimension>1u?findMSB(largestDimension-1u)+2:1;
#else
        int mipCount=textureQueryLevels(ddaGridTex[nonuniformEXT(slot)]);
#endif
        // Occupancy mips form an implicit octree. Retain the current level
        // across block crossings instead of probing upward from every voxel.
        int level=ddaSkipEmpty?mipCount-1:0;
        uint previous=0u;bool exiting=enter<.001&&ddaTexel(slot,cell)!=0u;int axis=entryAxis;
        // No fixed 512-step truncation. Advance all tied axes at edges/corners.
        int limit=dims.x+dims.y+dims.z+3;
        DDA_TRAVERSAL_LOOP for(int iteration=0;iteration<limit&&t<=leave&&t<best.t;++iteration){
            bool outside=any(lessThan(cell,ivec3(0)))||any(greaterThanEqual(cell,dims));
#ifdef MIKAN_RUNTIME_DDA_OPTIMIZED
            if(!outside&&ddaNaadf&&ddaSkipEmpty&&!exiting){
                ivec3 minimum,maximum;
                if(ddaNaadfLookup(record,cell,minimum,maximum)==0u){
                    vec3 boundary=mix(vec3(1e30),
                        (mix(vec3(minimum),vec3(maximum),greaterThan(stepCell,ivec3(0)))-o)*inverseDirection,activeAxes);
                    float crossing=min(boundary.x,min(boundary.y,boundary.z));
                    axis=boundary.x<=boundary.y&&boundary.x<=boundary.z?0:(boundary.y<=boundary.z?1:2);
                    t=crossing;cell=clamp(ivec3(floor(o+t*d)),minimum,maximum-1);
                    for(int a=0;a<3;++a)if(boundary[a]<=crossing)cell[a]=stepCell[a]>0?maximum[a]:minimum[a]-1;
                    nextT=mix(vec3(1e30),(vec3(cell)+max(vec3(stepCell),vec3(0))-o)*inverseDirection,activeAxes);
                    continue;
                }
            }
#endif
            if(!outside&&ddaSkipEmpty&&!exiting){
#ifdef MIKAN_RUNTIME_DDA_OPTIMIZED
                if(!ddaNaadf){
#endif
                DDA_TRAVERSAL_LOOP while(level>0){
#ifdef MIKAN_RUNTIME_DDA_OPTIMIZED
                    if(ddaRecordMipOccupancy(record,slot,cell,level)==0u)break;
#else
                    if(ddaMipOccupancy(slot,cell,level)==0u)break;
#endif
                    --level;
                }
                if(level>0){
                    int size=1<<level;ivec3 block=(cell>>level)<<level;
                    vec3 boundary=vec3(1e30);
#if DDA_VECTOR_FAST || defined(MIKAN_RUNTIME_DDA_OPTIMIZED)
                    boundary=mix(boundary,(vec3(block)+max(vec3(stepCell),vec3(0))*float(size)-o)*inverseDirection,activeAxes);
#else
                    for(int a=0;a<3;++a)if(stepCell[a]!=0)
                        boundary[a]=(float(block[a]+(stepCell[a]>0?size:0))-o[a])*inverseDirection[a];
#endif
                    float crossing=min(boundary.x,min(boundary.y,boundary.z));
                    axis=boundary.x<=boundary.y&&boundary.x<=boundary.z?0:(boundary.y<=boundary.z?1:2);
                    t=crossing;ivec3 oldCell=cell;
                    cell=clamp(ivec3(floor(o+t*d)),block,block+ivec3(size-1));
                    for(int a=0;a<3;++a)if(boundary[a]<=crossing)
                        cell[a]=stepCell[a]>0?block[a]+size:block[a]-1;
#if DDA_VECTOR_FAST || defined(MIKAN_RUNTIME_DDA_OPTIMIZED)
                    nextT=mix(vec3(1e30),(vec3(cell)+max(vec3(stepCell),vec3(0))-o)*inverseDirection,activeAxes);
#else
                    for(int a=0;a<3;++a)if(stepCell[a]!=0)
                        nextT[a]=(float(cell[a]+max(stepCell[a],0))-o[a])*inverseDirection[a];
#endif
                    uvec3 changed=uvec3(oldCell^cell);
                    level=min(mipCount-1,findMSB(changed.x|changed.y|changed.z));
                    continue;
                }
#ifdef MIKAN_RUNTIME_DDA_OPTIMIZED
                }
#endif
            }
            uint voxel=outside?0u:ddaTexel(slot,cell);
            if((!exiting&&voxel!=0u)||(exiting&&voxel==0u&&previous!=0u)){
                // A ray starting in solid first sees an outward-facing exit.
                // Skip that back face, then continue looking for a solid entry.
                if(cameraRay&&exiting){exiting=false;previous=0u;continue;}
                uint face=uint(axis*2)+(exiting?(stepCell[axis]>0?1u:0u):(stepCell[axis]<0?1u:0u));
                best=DdaHit(true,t,record,ddaWords[b+21u],exiting?previous:voxel,face);break;
            }
            if(outside)break;
            previous=voxel;float crossing=min(nextT.x,min(nextT.y,nextT.z));
            axis=nextT.x<=nextT.y&&nextT.x<=nextT.z?0:(nextT.y<=nextT.z?1:2);
            ivec3 oldCell=cell;
#if DDA_VECTOR_FAST || defined(MIKAN_RUNTIME_DDA_OPTIMIZED)
            bvec3 crossedAxes=lessThanEqual(nextT,vec3(crossing));
            vec3 crossedMask=mix(vec3(0),vec3(1),crossedAxes);
            cell+=ivec3(crossedMask)*stepCell;nextT+=crossedMask*delta;
#else
            for(int a=0;a<3;++a)if(nextT[a]<=crossing){cell[a]+=stepCell[a];nextT[a]+=delta[a];}
#endif
            t=crossing;
            if(ddaSkipEmpty&&!exiting){
                uvec3 changed=uvec3(oldCell^cell);
                level=min(mipCount-1,findMSB(changed.x|changed.y|changed.z));
            }
        }
    }return best;
}
bool voxelOccluded(vec3 origin,vec3 direction,float tMax,uint mask){return neeVoxelDDA&&traceVoxelDDA(origin,direction,tMax,mask,false).hit;}
bool decodeDdaSurface(uint record,uint voxel,uint face,vec3 origin,vec3 direction,float t,out Surface surface){
    uint b=16u+record*24u,index=ddaWords[b+21u],p=16u+ddaWords[0]*24u+(ddaWords[b+20u]+voxel)*4u;
    uint material=ddaWords[p+3u];Instance instance=instances[index];
    surface.position=origin+t*direction;surface.albedo=rtSrgbToLinear(uintBitsToFloat(uvec3(ddaWords[p],ddaWords[p+1u],ddaWords[p+2u])))*instance.color.rgb;
    vec3 n=vec3(0);n[int(face/2u)]=(face&1u)!=0u?1.0:-1.0;
    surface.normal=normalize(transpose(mat3(ddaWorldToGrid(record)))*n);if(dot(surface.normal,direction)>0.0)surface.normal=-surface.normal;
    surface.geometricNormal=surface.normal;surface.previousPosition=(previousFromCurrent[index]*vec4(surface.position,1)).xyz;
    surface.previousNormal=normalize(transpose(inverse(mat3(previousFromCurrent[index])))*surface.normal);
    surface.mirror=(material&1u)!=0u||(instance.info.y&1u)!=0u;surface.specularEnabled=(material&4u)!=0u;
    // Voxel glass (MATT type 2, packed as bit 3 by VoxRenderer) refracts like
    // model dielectrics; tint comes from the palette color in surface.albedo.
    surface.transmission=(material&8u)!=0u?0.9:0.0;
    surface.metallic=surface.specularEnabled?clamp(unpackHalf2x16(material>>16).x,0.0,1.0):0.0;
    surface.roughness=surface.specularEnabled?float((material>>8)&255u)/255.0:1.0;
    float entityEmission=(instance.info.y&2u)!=0u?unpackHalf2x16(instance.info.y>>16).x:0.0;
    surface.emissive=surface.albedo*(entityEmission+((material&2u)!=0u?unpackHalf2x16(material>>16).x:0.0));
    vec3 rawNormal=vec3(0);rawNormal[int(face/2u)]=(face&1u)!=0u?1.0:-1.0;
    vec3 gridPoint=(ddaWorldToGrid(record)*vec4(surface.position,1)).xyz;
    ivec3 dims=ivec3(ddaWords[b+16u],ddaWords[b+17u],ddaWords[b+18u]);
    ivec3 emitterCell=clamp(ivec3(floor(gridPoint-rawNormal*1e-3)),ivec3(0),dims-1);
    uint emitterIdentity=uint((emitterCell.z*dims.y+emitterCell.y)*dims.x+emitterCell.x)*6u+face;
    surface.hitIdentity=uvec3(index,0xfffffffdu,emitterIdentity);
    // NEE/MIS uses the voxel face directly; no triangle metadata query.
    return true;
}
