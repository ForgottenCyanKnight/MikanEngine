layout(constant_id=18) const bool softwareBvhCompact=true;
// Software-only intersections. BVH stack depth is bounded to 48 by the CPU builder.
layout(std430,binding=41) readonly buffer SoftwareQuadBVH {uint softWords[];};
struct SoftwareHit {bool hit;float t;uint instance;uint geometry;uint primitive;vec2 bary;};
vec3 softVec(uint p){return vec3(uintBitsToFloat(softWords[p]),uintBitsToFloat(softWords[p+1u]),uintBitsToFloat(softWords[p+2u]));}
bool softBounds(uint node,vec3 origin,vec3 inverseDirection,float limit,out float nearT){
    uint p=(softwareBvhCompact?12u:4u)+node*(softwareBvhCompact?4u:8u);vec3 minimum,maximum;
    if(softwareBvhCompact){
        uvec3 q=uvec3(softWords[p],softWords[p+1u],softWords[p+2u]);
        minimum=vec3(q.x&65535u,q.x>>16u,q.y&65535u);
        maximum=vec3(q.y>>16u,q.z&65535u,q.z>>16u);
    }else{minimum=softVec(p);maximum=softVec(p+4u);}
    vec3 a=(minimum-origin)*inverseDirection,b=(maximum-origin)*inverseDirection;
    vec3 lo=min(a,b),hi=max(a,b);
    nearT=max(.001,max(lo.x,max(lo.y,lo.z)));float farT=min(limit,min(hi.x,min(hi.y,hi.z)));
    return nearT<=farT;
}
SoftwareHit traceSoftwareQuads(vec3 origin,vec3 direction,float limit,uint mask,bool anyHit,bool cameraRay){
    SoftwareHit result;result.hit=false;result.t=limit;result.instance=0u;result.geometry=0u;result.primitive=0u;result.bary=vec2(0);
    if(softWords[0]==0u)return result;
    vec3 safeDirection=mix(direction,vec3(1e-30),lessThan(abs(direction),vec3(1e-30)));
    vec3 inverseDirection=1.0/safeDirection;
    // Test quantized bounds directly: transform the ray once, not every node.
    // step / direction preserves the world-space ray parameter t.
    vec3 boundsOrigin=origin,boundsInverse=inverseDirection;
    if(softwareBvhCompact){vec3 step=softVec(8u);boundsOrigin=(origin-softVec(4u))/step;boundsInverse=step*inverseDirection;}
    uint cachedInstance=0xffffffffu;vec3 o=vec3(0),d=vec3(0);
    uint stack[64];uint count=1u;stack[0]=0u;
    while(count>0u){
        uint node=stack[--count],p=(softwareBvhCompact?12u:4u)+node*(softwareBvhCompact?4u:8u);float nearT;
        if(!softBounds(node,boundsOrigin,boundsInverse,result.t,nearT))continue;
        uint a,b;
        if(softwareBvhCompact){uint link=softWords[p+3u];bool leafNode=(link&0x80000000u)!=0u;
            a=leafNode?link&0xffffffu:node+1u;b=leafNode?0x80000000u|((link>>24u)&127u):link;
        }else{a=softWords[p+3u];b=softWords[p+7u];}
        if((b&0x80000000u)==0u){
            float leftT,rightT;bool left=softBounds(a,boundsOrigin,boundsInverse,result.t,leftT),right=softBounds(b,boundsOrigin,boundsInverse,result.t,rightT);
            if(left&&right){stack[count++]=leftT<rightT?b:a;stack[count++]=leftT<rightT?a:b;}
            else if(left)stack[count++]=a;else if(right)stack[count++]=b;
            continue;
        }
        for(uint leaf=0u;leaf<(b&0x7fffffffu);++leaf){
            uint ref=softWords[1]+(a+leaf)*(softwareBvhCompact?2u:4u);
            uint planeDirection=softWords[ref+(softwareBvhCompact?0u:1u)];
            uint index=softwareBvhCompact?planeDirection>>8u:softWords[ref+2u];
            uint identity=softWords[ref+(softwareBvhCompact?1u:3u)];
            uint geometry=identity&7u,localQuad=identity>>3u;
            uvec4 range=ranges[instances[index].info.x+geometry];
            uint packed=softwareBvhCompact?quadWords[range.x+localQuad]:softWords[ref];
            uint transform=softWords[2]+index*(softwareBvhCompact?16u:24u);
            if((softWords[transform+(softwareBvhCompact?12u:20u)]&mask)==0u)continue;
            if(index!=cachedInstance){
                if(softwareBvhCompact){
                    vec4 r0=uintBitsToFloat(uvec4(softWords[transform],softWords[transform+1u],softWords[transform+2u],softWords[transform+3u]));
                    vec4 r1=uintBitsToFloat(uvec4(softWords[transform+4u],softWords[transform+5u],softWords[transform+6u],softWords[transform+7u]));
                    vec4 r2=uintBitsToFloat(uvec4(softWords[transform+8u],softWords[transform+9u],softWords[transform+10u],softWords[transform+11u]));
                    o=vec3(dot(r0,vec4(origin,1)),dot(r1,vec4(origin,1)),dot(r2,vec4(origin,1)));
                    d=vec3(dot(r0.xyz,direction),dot(r1.xyz,direction),dot(r2.xyz,direction));
                }else{
                mat4 inverseModel=mat4(uintBitsToFloat(uvec4(softWords[transform],softWords[transform+1u],softWords[transform+2u],softWords[transform+3u])),
                    uintBitsToFloat(uvec4(softWords[transform+4u],softWords[transform+5u],softWords[transform+6u],softWords[transform+7u])),
                    uintBitsToFloat(uvec4(softWords[transform+8u],softWords[transform+9u],softWords[transform+10u],softWords[transform+11u])),
                    uintBitsToFloat(uvec4(softWords[transform+12u],softWords[transform+13u],softWords[transform+14u],softWords[transform+15u])));
                float size=uintBitsToFloat(softWords[transform+19u]);
                o=((inverseModel*vec4(origin,1)).xyz-softVec(transform+16u))/size;
                // Do not normalize: t remains in world-ray units for nonuniform scaling.
                d=(inverseModel*vec4(direction,0)).xyz/size;
                }cachedInstance=index;
            }
            uint face=softwareBvhCompact?range.z:planeDirection>>16u;
            int axis=face<2u?2:(face<4u?0:1),u=face<2u?0:(face<4u?2:0),v=face<4u?1:2;
            vec3 start=vec3(0);start[axis]=float(planeDirection&(softwareBvhCompact?255u:65535u));start[u]=float(packed&255u);start[v]=float((packed>>8)&255u);
            float plane=start[axis]+((face==0u||face==3u||face==4u)?1.0:0.0);
            if(abs(d[axis])<1e-20)continue;
            float outward=(face==0u||face==3u||face==4u)?1.0:-1.0;
            if(cameraRay&&d[axis]*outward>=0.0)continue;
            float t=(plane-o[axis])/d[axis];if(t<.001||t>=result.t)continue;
            vec2 dimensions=vec2(((packed>>16)&255u)+1u,(packed>>24)+1u);
            vec3 point=o+d*t;vec2 uv=(vec2(point[u],point[v])-vec2(start[u],start[v]))/dimensions;
            if(any(lessThan(uv,vec2(0)))||any(greaterThan(uv,vec2(1))))continue;
            result.hit=true;result.t=t;result.instance=index;result.geometry=geometry;
            if(anyHit)return result;
            // Existing palette/MIS code uses the two original half IDs. This is
            // attribute encoding only; the intersection above tests one rectangle.
            uint halfId=face==0u?uint(uv.y>uv.x):(face==2u?uint(uv.x+uv.y<1.0):(face==5u?uint(uv.x+uv.y>1.0):uint(uv.x>uv.y)));
            result.primitive=(localQuad*2u+halfId)|0x40000000u;result.bary=uv;
        }
    }return result;
}
