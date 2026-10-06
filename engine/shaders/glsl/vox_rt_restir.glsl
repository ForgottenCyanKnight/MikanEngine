// Fresh RIS for primary diffuse emitters, with optional legacy ReSTIR reuse.
// Reservoirs use area measure; visibility is queried for the final sample.
struct DIReservoir {vec4 sampleData;vec4 positionM;vec4 normalMaterial;};
layout(std430,binding=24) readonly buffer DIPrevious {DIReservoir restirPrevious[];};
layout(std430,binding=25) buffer DICurrent {DIReservoir restirCurrent[];};
DIReservoir emptyDIReservoir(){return DIReservoir(vec4(0),vec4(0),vec4(0));}
float diLuminance(vec3 value){return dot(max(value,vec3(0)),vec3(.2126,.7152,.0722));}
// sampleData.xyz = barycentric random numbers and integer-valued light index.
vec3 diPoint(vec3 sampleData){
    uint base=1u+uint(sampleData.z)*4u;float su=sqrt(sampleData.x);
    return emissiveLights[base+1u].xyz*(1.0-su)
        +emissiveLights[base+2u].xyz*(su*(1.0-sampleData.y))
        +emissiveLights[base+3u].xyz*(su*sampleData.y);
}
vec3 diContribution(vec3 sampleData,vec3 position,vec3 normal){
    uint index=uint(sampleData.z);
    if(index>=uint(emissiveLights[0].x))return vec3(0);
    uint base=1u+index*4u;
    vec3 p0=emissiveLights[base+1u].xyz,p1=emissiveLights[base+2u].xyz,p2=emissiveLights[base+3u].xyz;
    vec3 offset=diPoint(sampleData)-position;float d2=dot(offset,offset);
    if(d2<=1e-10)return vec3(0);
    vec3 direction=offset*inversesqrt(d2);
    float cosine=max(dot(normal,direction),0.0);
    float facing=abs(dot(normalize(cross(p1-p0,p2-p0)),-direction));
        if(cosine<=0.0||facing<=1e-8)return vec3(0);
    // ReSTIR resamples the NEE half of the ORIGINAL MIS estimator. Keep the
    // complementary BRDF-hit half in sampleDiffuseGI. Both use the same
    // light proposal at the current receiver, even for reused samples.
    float lightPdf=emissiveLights[base+2u].w*d2/(emissiveLights[base].w*facing);
    float bsdfPdf=cosine/RT_PI;
    vec3 contribution=emissiveLights[base].rgb*(cosine*facing/(RT_PI*d2))*powerHeuristic(lightPdf,bsdfPdf);
    float proposal=emissiveLights[base+2u].w/emissiveLights[base].w;
    // Bound f/q, then restore area measure. Both RIS targets and evaluation
    // use this exact same function, including temporal/spatial reconnections.
    return proposal>0.0?rtClampFirefly(contribution/proposal,FIREFLY_DI)*proposal:vec3(0);
}
void diStream(inout DIReservoir reservoir,vec3 sampleData,float weight,float M,inout float weightSum,inout RaySampleState state){
    reservoir.positionM.w+=M;
    if(weight<=0.0||isnan(weight)||isinf(weight))return;
    weightSum+=weight;
    if(randomFloat(state.fallback)*weightSum<weight)reservoir.sampleData.xyz=sampleData;
}
bool diCompatible(DIReservoir source,Surface surface){
    if(source.positionM.w<=0.0||source.sampleData.w<0.0)return false;
    if(dot(source.normalMaterial.xyz,surface.previousNormal)<.98)return false;
    float distanceTolerance=max(.02,.005*abs((temporal.view*vec4(surface.position,1)).z));
    if(distance(source.positionM.xyz,surface.previousPosition)>distanceTolerance)return false;
    float material=diLuminance(surface.albedo);
    return abs(source.normalMaterial.w-material)<=.05+.2*material;
}
bool restirNeighbor(Surface a,Surface b){
    if(b.mirror||dot(a.normal,b.normal)<.98)return false;
    vec3 delta=b.position-a.position;
    float z=abs((temporal.view*vec4(a.position,1)).z);
    return length(delta)<max(.1,.025*z)&&abs(dot(delta,a.geometricNormal))<max(.01,.002*z);
}
ivec2 restirNeighborPixel(ivec2 pixel,int i,inout RaySampleState state){
    if(i==0)return pixel;
    float angle=2.0*RT_PI*randomFloat(state.fallback);
    float radius=1.0+floor(randomFloat(state.fallback)*4.0);
    return pixel+ivec2(round(radius*vec2(cos(angle),sin(angle))));
}
bool diVisible(vec3 sampleData,Surface surface){
    vec3 point=diPoint(sampleData),origin=surface.position+surface.geometricNormal*rayOffset(surface.position);
    vec3 delta=point-origin;float distanceToLight=length(delta);
    if(distanceToLight<=.002||dot(surface.geometricNormal,delta)<=0.0)return false;
    rayQueryEXT shadow;
    rayQueryInitializeEXT(shadow,scene,gl_RayFlagsOpaqueEXT|gl_RayFlagsTerminateOnFirstHitEXT,2,
        origin,.001,delta/distanceToLight,distanceToLight*(1.0-1e-3));
    while(rayQueryProceedEXT(shadow)){}
    return rayQueryGetIntersectionTypeEXT(shadow,true)==gl_RayQueryCommittedIntersectionNoneEXT;
}
float diFullSupportTarget(vec3 sampleData,Surface surface){
    // Proposals sample the entire emitter area, including below the source
    // hemisphere. A strictly positive target preserves that complete support.
    return max(diLuminance(diContribution(sampleData,surface.position,surface.normal)),1e-8);
}
bool previousRestirPixel(Surface surface,out ivec2 location){
    if((uint(temporal.giOptions.w)&8388608u)==0u)return false;
    vec4 clip=temporal.previousViewProjection*vec4(surface.previousPosition,1);
    if(clip.w<=0.0)return false;
    vec2 uv=clip.xy/clip.w*.5+.5-temporal.jitter.zw;
    if(any(lessThan(uv,vec2(0)))||any(greaterThanEqual(uv,vec2(1))))return false;
    location=ivec2(uv*pc.extent.xy);return true;
}
bool temporalReceiver(Surface current,vec3 position,vec3 normal,out Surface receiver){
    // This first experiment only reconnects flat, static primary surfaces.
    // Validation depends on geometry, never on candidate radiance/visibility.
    float z=abs((temporal.view*vec4(current.position,1)).z);
    vec3 delta=position-current.position;
    if(dot(current.normal,current.geometricNormal)<.999||dot(normal,current.geometricNormal)<.999)return false;
    if(length(delta)>max(.05,.01*z)||abs(dot(delta,current.geometricNormal))>max(.005,.001*z))return false;
    receiver=current;receiver.position=position;receiver.normal=normal;receiver.geometricNormal=normal;
    return true;
}
vec3 correctedSpatialDI(Surface surface,ivec2 pixel,inout RaySampleState state){
    // Separate streams: receiver selection must not depend on a candidate's
    // radiance, path length, or the RIS decisions that produced its reservoir.
    RaySampleState neighbors=initRaySampleState(uvec2(pixel),uint(temporal.options.x));
    neighbors.fallback=hashRandom(neighbors.fallback^0xb5297a4du);
    state.fallback=hashRandom(neighbors.fallback^0x68e31da4u);
    DIReservoir sources[6];Surface receivers[6];ivec2 locations[5];int count=0;
    for(int i=0;i<5;++i){
        ivec2 location=restirNeighborPixel(pixel,i,neighbors);Surface receiver;
        bool duplicate=false;for(int j=0;j<count;++j)duplicate=duplicate||all(equal(location,locations[j]));
        if(duplicate||!surfaceAtPixel(location,receiver)||!restirNeighbor(surface,receiver))continue;
        DIReservoir source=restirCurrent[uint(location.y)*uint(pc.extent.x)+uint(location.x)];
        if(source.positionM.w<=0.0)continue;
        locations[count]=location;receivers[count]=receiver;sources[count++]=source;
    }
    ivec2 previous;Surface previousReceiver;
    if(previousRestirPixel(surface,previous)){
        DIReservoir old=restirPrevious[uint(previous.y)*uint(pc.extent.x)+uint(previous.x)];
        if(old.positionM.w>0.0&&temporalReceiver(surface,old.positionM.xyz,old.normalMaterial.xyz,previousReceiver)){
            receivers[count]=previousReceiver;sources[count++]=old;
        }
    }
    float sum=0.0,selectedTarget=0.0;int selected=-1;
    for(int i=0;i<count;++i){
        vec3 f=diContribution(sources[i].sampleData.xyz,surface.position,surface.normal);
        float target=diVisible(sources[i].sampleData.xyz,surface)?diLuminance(f):0.0;
        float weight=target*sources[i].sampleData.w*sources[i].positionM.w;
        if(weight<=0.0||isnan(weight)||isinf(weight))continue;
        sum+=weight;if(randomFloat(state.fallback)*sum<weight){selected=i;selectedTarget=target;}
    }
    if(selected<0)return vec3(0);
    vec3 sampleData=sources[selected].sampleData.xyz;
    float denominator=0.0;
    for(int i=0;i<count;++i)denominator+=sources[i].positionM.w*diFullSupportTarget(sampleData,receivers[i]);
    float numerator=diFullSupportTarget(sampleData,receivers[selected]);
    float W=sum*numerator/(selectedTarget*denominator);
    return diContribution(sampleData,surface.position,surface.normal)*W;
}
vec3 sampleRestirDI(Surface surface,ivec2 pixel,vec2 pixelUV,inout RaySampleState state){
    bool corrected=(uint(temporal.giOptions.w)&8192u)!=0u;
    if(corrected&&pc.light.w< -2.5){
        RaySampleState spatialState=state;
        randomFloat(state.fallback);nextRaySample(state);
        return correctedSpatialDI(surface,pixel,spatialState);
    }
    uint address=uint(pixel.y)*uint(pc.extent.x)+uint(pixel.x);
    DIReservoir result=emptyDIReservoir();
    result.positionM.xyz=surface.position;
    result.normalMaterial=vec4(surface.normal,diLuminance(surface.albedo));
    uint count=uint(emissiveLights[0].x);if(count==0u){restirCurrent[address]=result;return vec3(0);}
    // Fresh RIS is the default: improve light selection without cross-frame
    // reservoir correlations. Visibility is still queried only once below.
    uint flags=uint(temporal.giOptions.w);
    bool reuse=!corrected&&(flags&128u)!=0u;
    uint candidates=max(1u,(flags>>8u)&31u);
    RaySampleState diState=state;
    // Keep the caller's subsequent GGX proposals independent of DI quality.
    randomFloat(state.fallback);nextRaySample(state);
    float weightSum=0.0;
    for(uint candidate=0u;candidate<candidates;++candidate){
        float choice=randomFloat(diState.fallback);uint low=0u,high=count;
        while(low<high){uint mid=low+(high-low)/2u;
            if(choice<emissiveLights[2u+mid*4u].w)high=mid;else low=mid+1u;}
        uint light=min(low,count-1u),base=1u+light*4u;
        vec3 fresh=vec3(nextRaySample(diState),float(light));
        float proposal=emissiveLights[base+2u].w/emissiveLights[base].w;
        float target=corrected?diFullSupportTarget(fresh,surface):diLuminance(diContribution(fresh,surface.position,surface.normal));
        diStream(result,fresh,proposal>0.0?target/proposal:0.0,1.0,weightSum,diState);
    }
    // Temporal and spatial reuse both read only the previous buffer, avoiding
    // cross-workgroup races. Never read an uninitialized history on reset.
    DIReservoir sources[3];ivec2 sourcePixels[3];int sourceCount=0;
    if(reuse && temporal.options.y<.5){
        vec4 clip=temporal.previousViewProjection*vec4(surface.previousPosition,1);
        if(clip.w>0.0){
            vec2 uv=clip.xy/clip.w*.5+.5-temporal.jitter.zw;
            if(all(greaterThanEqual(uv,vec2(0)))&&all(lessThan(uv,vec2(1)))){
                ivec2 previous=ivec2(uv*pc.extent.xy);
                for(int i=0;i<3;++i){
                    ivec2 location=previous;
                    if(i>0){float phi=2.0*RT_PI*randomFloat(diState.fallback);
                        int radius=1+int(randomFloat(diState.fallback)*4.0);
                        location+=ivec2(round(vec2(cos(phi),sin(phi))*float(radius)));}
                    if(any(lessThan(location,ivec2(0)))||any(greaterThanEqual(location,ivec2(pc.extent.xy))))continue;
                    // Reject duplicate pixels, including temporal vs neighbor.
                    bool duplicate=false;
                    for(int j=0;j<sourceCount;++j)duplicate=duplicate||all(equal(sourcePixels[j],location));
                    if(duplicate)continue;
                    DIReservoir old=restirPrevious[uint(location.y)*uint(pc.extent.x)+uint(location.x)];
                    if(!diCompatible(old,surface))continue;
                    float M=min(old.positionM.w,i==0?8.0:2.0);
                    old.positionM.w=M;
                    sourcePixels[sourceCount]=location;sources[sourceCount++]=old;
                    float currentTarget=diLuminance(diContribution(old.sampleData.xyz,surface.position,surface.normal));
                    diStream(result,old.sampleData.xyz,currentTarget*old.sampleData.w*M,M,weightSum,diState);
                }
            }
        }
    }
    float selectedTarget=corrected?diFullSupportTarget(result.sampleData.xyz,surface):diLuminance(diContribution(result.sampleData.xyz,surface.position,surface.normal));
    if(weightSum<=0.0||selectedTarget<=0.0){
        // An all-zero RIS estimate still represents M candidates. Dropping M
        // conditions future reuse on positive outcomes and loses normalization.
        result.sampleData.w=0.0;if(!corrected)result.positionM.w=min(result.positionM.w,16.0);
        restirCurrent[address]=result;return vec3(0);
    }
    // Conservative standard RIS normalization. Do not shrink the denominator
    // using a selected-sample support test while retaining the larger M in
    // history. This mode permits reuse bias near changing support/occlusion;
    // tight surface validation and bounded history limit that exposure.
    result.sampleData.w=weightSum/(result.positionM.w*selectedTarget);
    if(!corrected)result.positionM.w=min(result.positionM.w,16.0);
    if(isnan(result.sampleData.w)||isinf(result.sampleData.w)){restirCurrent[address]=emptyDIReservoir();return vec3(0);}
    // Biased tail suppression is also stored in W: reuse must not resurrect
    // an unbounded estimate that was only clamped at the NRD output.
    result.sampleData.w*=rtFireflyScale(diContribution(result.sampleData.xyz,surface.position,surface.normal)*result.sampleData.w,FIREFLY_DI);
    restirCurrent[address]=result;
    vec3 point=diPoint(result.sampleData.xyz),offset=point-surface.position;
    vec3 direction=normalize(offset);
    if(dot(direction,surface.geometricNormal)<=0.0)return vec3(0);
    vec3 origin=surface.position+surface.geometricNormal*rayOffset(surface.position);
    vec3 shadowOffset=point-origin;float shadowDistance=length(shadowOffset);
    if(shadowDistance<=.002)return vec3(0);
    rayQueryEXT shadow;
    rayQueryInitializeEXT(shadow,scene,gl_RayFlagsOpaqueEXT|gl_RayFlagsTerminateOnFirstHitEXT,2,
        origin,.001,shadowOffset/shadowDistance,shadowDistance*(1.0-1e-3));
    while(rayQueryProceedEXT(shadow)){}
    if(rayQueryGetIntersectionTypeEXT(shadow,true)!=gl_RayQueryCommittedIntersectionNoneEXT)return vec3(0);
    return diContribution(result.sampleData.xyz,surface.position,surface.normal)*result.sampleData.w;
}
