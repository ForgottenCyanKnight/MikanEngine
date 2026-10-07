// Header: 16 words, then 24 words per instance (worldToGrid mat4,
// uvec4(dimensions,texture), uvec4(paletteOffset,hitInstance,shadowMask,0)).
// Full source palettes follow; RGB is sRGB, w is the canonical material word.
layout(binding=39) uniform utexture3D ddaGridTex[8];
#include "vox_rt_dda_data.glsl"
struct DdaHit {bool hit;float t;uint record;uint index;uint voxel;uint face;};
uint ddaTexel(uint slot,ivec3 cell){return texelFetch(ddaGridTex[nonuniformEXT(slot)],cell,0).r;}
DdaHit traceVoxelDDA(vec3 origin,vec3 direction,float tMax,uint mask){
    DdaHit best=DdaHit(false,tMax,0u,0u,0u,0u);if(!neeVoxelDDA)return best;
    for(uint record=0u;record<ddaWords[0];++record){
        uint b=16u+record*24u;if(mask==2u&&(ddaWords[b+22u]&2u)==0u)continue;
        ivec3 dims=ivec3(ddaWords[b+16u],ddaWords[b+17u],ddaWords[b+18u]);uint slot=ddaWords[b+19u];
        if(slot>=8u||any(lessThanEqual(dims,ivec3(0))))continue;
        mat4 transform=ddaWorldToGrid(record);vec3 o=(transform*vec4(origin,1)).xyz,d=mat3(transform)*direction;
        float enter=-1e30,leave=best.t;int entryAxis=0;bool intersects=true;
        for(int a=0;a<3;++a){
            if(abs(d[a])<1e-20){if(o[a]<0.0||o[a]>=float(dims[a]))intersects=false;}
            else{float lo=(0.0-o[a])/d[a],hi=(float(dims[a])-o[a])/d[a];
                float nearT=min(lo,hi),farT=max(lo,hi);if(nearT>enter){enter=nearT;entryAxis=a;}leave=min(leave,farT);}
        }
        float t=max(enter,.001);if(!intersects||leave<t||t>=best.t)continue;
        ivec3 stepCell=ivec3(sign(d)),cell=ivec3(floor(o+t*d+vec3(stepCell)*1e-4));cell=clamp(cell,ivec3(0),dims-1);
        vec3 delta=vec3(1e30),nextT=vec3(1e30);
        for(int a=0;a<3;++a)if(stepCell[a]!=0){delta[a]=abs(1.0/d[a]);nextT[a]=(float(cell[a]+max(stepCell[a],0))-o[a])/d[a];}
        int mipCount=textureQueryLevels(ddaGridTex[nonuniformEXT(slot)]);
        uint previous=0u;bool exiting=enter<.001&&ddaTexel(slot,cell)!=0u;int axis=entryAxis;
        // No fixed 512-step truncation. Advance all tied axes at edges/corners.
        int limit=dims.x+dims.y+dims.z+3;
        for(int iteration=0;iteration<limit&&t<=leave&&t<best.t;++iteration){
            bool outside=any(lessThan(cell,ivec3(0)))||any(greaterThanEqual(cell,dims));uint voxel=outside?0u:ddaTexel(slot,cell);
            if((!exiting&&voxel!=0u)||(exiting&&voxel==0u&&previous!=0u)){
                uint face=uint(axis*2)+(exiting?(stepCell[axis]>0?1u:0u):(stepCell[axis]<0?1u:0u));
                best=DdaHit(true,t,record,ddaWords[b+21u],exiting?previous:voxel,face);break;
            }
            if(outside)break;
            if(ddaSkipEmpty&&!exiting&&voxel==0u){
                int emptyLevel=0;
                for(int level=1;level<mipCount;++level){
                    if(texelFetch(ddaGridTex[nonuniformEXT(slot)],cell>>level,level).r!=0u)break;
                    emptyLevel=level;
                }
                if(emptyLevel>0){
                    int size=1<<emptyLevel;ivec3 block=(cell>>emptyLevel)<<emptyLevel;
                    vec3 boundary=vec3(1e30);
                    for(int a=0;a<3;++a)if(stepCell[a]!=0)
                        boundary[a]=(float(block[a]+(stepCell[a]>0?size:0))-o[a])/d[a];
                    float crossing=min(boundary.x,min(boundary.y,boundary.z));
                    axis=boundary.x<=boundary.y&&boundary.x<=boundary.z?0:(boundary.y<=boundary.z?1:2);
                    t=crossing;
                    // Only an OR-empty block is skipped; level zero still gives exact palette/face hits.
                    cell=clamp(ivec3(floor(o+t*d)),block,block+ivec3(size-1));
                    // Set crossed faces exactly; leave other axes inside the empty block.
                    for(int a=0;a<3;++a)if(boundary[a]<=crossing)
                        cell[a]=stepCell[a]>0?block[a]+size:block[a]-1;
                    for(int a=0;a<3;++a)if(stepCell[a]!=0)
                        nextT[a]=(float(cell[a]+max(stepCell[a],0))-o[a])/d[a];
                    continue;
                }
            }
            previous=voxel;float crossing=min(nextT.x,min(nextT.y,nextT.z));
            axis=nextT.x<=nextT.y&&nextT.x<=nextT.z?0:(nextT.y<=nextT.z?1:2);
            for(int a=0;a<3;++a)if(nextT[a]<=crossing){cell[a]+=stepCell[a];nextT[a]+=delta[a];}t=crossing;
        }
    }return best;
}
bool voxelOccluded(vec3 origin,vec3 direction,float tMax,uint mask){return neeVoxelDDA&&traceVoxelDDA(origin,direction,tMax,mask).hit;}
bool decodeDdaSurface(uint record,uint voxel,uint face,vec3 origin,vec3 direction,float t,out Surface surface){
    uint b=16u+record*24u,index=ddaWords[b+21u],p=16u+ddaWords[0]*24u+(ddaWords[b+20u]+voxel)*4u;
    uint material=ddaWords[p+3u];Instance instance=instances[index];
    surface.position=origin+t*direction;surface.albedo=rtSrgbToLinear(uintBitsToFloat(uvec3(ddaWords[p],ddaWords[p+1u],ddaWords[p+2u])))*instance.color.rgb;
    vec3 n=vec3(0);n[int(face/2u)]=(face&1u)!=0u?1.0:-1.0;
    surface.normal=normalize(transpose(mat3(ddaWorldToGrid(record)))*n);if(dot(surface.normal,direction)>0.0)surface.normal=-surface.normal;
    surface.geometricNormal=surface.normal;surface.previousPosition=(previousFromCurrent[index]*vec4(surface.position,1)).xyz;
    surface.previousNormal=normalize(transpose(inverse(mat3(previousFromCurrent[index])))*surface.normal);
    surface.mirror=(material&1u)!=0u||(instance.info.y&1u)!=0u;surface.specularEnabled=(material&4u)!=0u;
    surface.metallic=surface.specularEnabled?clamp(unpackHalf2x16(material>>16).x,0.0,1.0):0.0;
    surface.roughness=surface.specularEnabled?float((material>>8)&255u)/255.0:1.0;
    float entityEmission=(instance.info.y&2u)!=0u?unpackHalf2x16(instance.info.y>>16).x:0.0;
    surface.emissive=surface.albedo*(entityEmission+((material&2u)!=0u?unpackHalf2x16(material>>16).x:0.0));
    surface.hitIdentity=uvec3(index,0xfffffffeu,voxel);
    // Retain the exact triangle emitter identity for NEE/MIS. Mask-4 meshes
    // are queried only for emissive metadata, never for VOX ray visibility.
    if(any(greaterThan(surface.emissive,vec3(0)))){
        float tolerance=max(.002,abs(t)*2e-5);rayQueryEXT metadata;
        rayQueryInitializeEXT(metadata,scene,gl_RayFlagsOpaqueEXT,4,origin,max(.0001,t-tolerance),direction,t+tolerance);
        while(rayQueryProceedEXT(metadata)){}
        if(rayQueryGetIntersectionTypeEXT(metadata,true)==gl_RayQueryCommittedIntersectionTriangleEXT&&rayQueryGetIntersectionInstanceCustomIndexEXT(metadata,true)==index)
            surface.hitIdentity=uvec3(index,rayQueryGetIntersectionGeometryIndexEXT(metadata,true),rayQueryGetIntersectionPrimitiveIndexEXT(metadata,true));
    }return true;
}
