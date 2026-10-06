// RTXDI reservoir shading for the GLSL realtime pass. The reservoir buffer is
// produced by the DXC-compiled RTXDI passes (vox_rtxdi_*.comp.hlsl); this file
// unpacks the winning sample and shades it with one visibility ray, using the
// same emissive triangle light list the resampling passes consumed.
// Packed reservoir: 6 words (lightData, uvData, mVisibility, distanceAge,
// targetPdf, weight) in the RTXDI block-linear layout, 3 buffers.
layout(std430,binding=32) readonly buffer RtxdiReservoirWords {uint rtxdiWords[];};

uint rtxdiShadePointer(uvec2 pixel,uint bufferIndex,uint blockRowPitch,uint arrayPitch){
    uvec2 block=pixel/16u,inBlock=pixel%16u;
    return bufferIndex*arrayPitch+block.y*blockRowPitch+block.x*256u+inBlock.y*16u+inBlock.x;
}

void sampleRtxdiLighting(Surface surface,ivec2 pixel,vec3 incoming,out vec3 diffuse,out vec3 specular){
    diffuse=vec3(0);specular=vec3(0);
    uvec2 rp=uvec2(pixel);
    uint pointer=rtxdiShadePointer(rp,uint(temporal.rtxdiParams.y),uint(temporal.rtxdiParams.z),uint(temporal.rtxdiParams.w));
    uint base=pointer*6u;
    uint lightData=rtxdiWords[base];
    float weight=uintBitsToFloat(rtxdiWords[base+5]);
    if(lightData==0u||weight<=0.0)return;
    uint lightIndex=lightData&0x7fffffffu;
    float count=emissiveLights[0].x;
    if(float(lightIndex)>=count)return;
    float M=float((rtxdiWords[base+2]>>18u)&0x3fffu);
    uint uvData=rtxdiWords[base+1];
    vec2 uv=vec2(float(uvData&0xffffu),float(uvData>>16u))/65535.0;
    uint b=1u+lightIndex*4u;
    vec3 radiance=emissiveLights[b].rgb;float area=emissiveLights[b].w;
    float root=sqrt(uv.x);uv=vec2(root*(1.0-uv.y),root*uv.y);float w0=1.0-uv.x-uv.y;
    vec3 point=emissiveLights[b+1u].xyz*w0+emissiveLights[b+2u].xyz*uv.x+emissiveLights[b+3u].xyz*uv.y;
    vec3 offset=point-surface.position;
    float dist2=dot(offset,offset);
    if(dist2<1e-10)return;
    float dist=sqrt(dist2);
    vec3 direction=offset/dist;
    float cosine=max(dot(surface.normal,direction),0.0);
    vec3 lightNormal=normalize(cross(emissiveLights[b+2u].xyz-emissiveLights[b+1u].xyz,emissiveLights[b+3u].xyz-emissiveLights[b+1u].xyz));
    float facing=abs(dot(lightNormal,-direction));
    if(cosine<=0.0||facing<=0.0)return;
    vec3 origin=surface.position+surface.geometricNormal*rayOffset(surface.position);
    rayQueryEXT shadow;
    rayQueryInitializeEXT(shadow,scene,gl_RayFlagsOpaqueEXT|gl_RayFlagsTerminateOnFirstHitEXT,2,
        origin,.001,direction,dist*(1.0-1e-3));
    while(rayQueryProceedEXT(shadow)){}
    if(rayQueryGetIntersectionTypeEXT(shadow,true)!=gl_RayQueryCommittedIntersectionNoneEXT)return;
    // Reservoir W = weightSum(finalized); estimator = Le * cos * W / pdf_sa.
    float solidAnglePdf=dist2/max(area*facing,1e-12);
    diffuse=radiance*cosine*weight/max(solidAnglePdf,1e-12)/RT_PI;
    if(temporal.rtxdiParams.x>2.5&&surface.specularEnabled&&dot(surface.geometricNormal,direction)>0.0){
        float pdf=ggxPdf(surface,-incoming,direction);
        float neePdf=emissiveLights[b+2u].w*solidAnglePdf;
        specular=radiance*ggxWeight(surface,-incoming,direction)*pdf
            *powerHeuristic(neePdf,pdf)*weight/max(solidAnglePdf,1e-12);
    }
}
