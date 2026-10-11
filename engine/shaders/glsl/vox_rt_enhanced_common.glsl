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


uint giPackNormal(vec3 n){
    n/=abs(n.x)+abs(n.y)+abs(n.z);
    vec2 oct=n.xy;
    if(n.z<0.0)oct=(1.0-abs(oct.yx))*vec2(oct.x>=0.0?1.0:-1.0,oct.y>=0.0?1.0:-1.0);
    return packSnorm2x16(oct);
}

struct PTReservoir {
    vec4 diffuseWeight;  // demodulated diffuse integrand; finalized UCW
    vec4 specularDistance;
    vec4 receiverM;
    uvec4 seedPixelNormal; // seed, receiving pixel xy, geometric normal
    // Enhanced mixed PSS/solid-angle chart, 160-byte std430 ABI.
    uvec4 path; // flags, initial seed, suffix seed, rc barycentric y bits
    uvec4 rcHit; // instance, geometry, primitive, rc barycentric x bits
    vec4 incidentGeometry; // cached suffix radiance, source geometry term
    vec4 directionPdf; // fixed RC outgoing direction, terminal light PDF
    vec4 primaryViewFootprint; // primary outgoing view, source primary footprint
    vec4 rcPosition; // current scene's immutable connection point
};
layout(std430,binding=42) readonly buffer PTPrevious {PTReservoir ptPrevious[];};
layout(std430,binding=43) buffer PTFresh {PTReservoir ptFresh[];};
layout(std430,binding=44) buffer PTTemporal {PTReservoir ptTemporal[];};
layout(std430,binding=45) buffer PTFinal {PTReservoir ptFinal[];};
PTReservoir ptEmpty(){
    return PTReservoir(vec4(0),vec4(0),vec4(0),uvec4(0),uvec4(0),uvec4(0xffffffffu),vec4(0),vec4(0),vec4(0),vec4(0));
}
float ptTarget(PTReservoir r){return diLuminance(r.diffuseWeight.rgb+r.specularDistance.rgb);}
// Cap the weighted estimator, including resampling amplification. Keep the
// integrand intact for replay/MIS and reduce UCW before storing history.
void ptClampFirefly(inout PTReservoir r){
    if(isnan(r.diffuseWeight.w)||isinf(r.diffuseWeight.w)||r.diffuseWeight.w<0.0){r.diffuseWeight.w=0.0;return;}
    if(temporal.giOptions.z<=0.0)return;
    float d=diLuminance(r.diffuseWeight.rgb),s=diLuminance(r.specularDistance.rgb);
    float bound=1e30;
    if(d>0.0)bound=min(bound,FIREFLY_GI*temporal.giOptions.z/d);
    if(s>0.0)bound=min(bound,FIREFLY_SPECULAR*temporal.giOptions.z/s);
    r.diffuseWeight.w=min(r.diffuseWeight.w,bound);
}
bool ptFinite(vec3 v){return !any(isnan(v))&&!any(isinf(v))&&all(greaterThanEqual(v,vec3(0)));}
float ptSpecularProbability(Surface s){
    if(!s.specularEnabled)return 0.0;
    float diffuse=diLuminance(s.albedo*(1.0-s.metallic));
    float specular=diLuminance(mix(vec3(.04),s.albedo,s.metallic));
    return diffuse<=0.0?1.0:clamp(specular/(diffuse+specular),.1,.9);
}
float ptPdf(Surface s,vec3 V,vec3 L){
    if(dot(s.geometricNormal,L)<=0.0)return 0.0;
    float ps=ptSpecularProbability(s);
    return (1.0-ps)*max(dot(s.normal,L),0.0)/RT_PI+ps*ggxPdf(s,V,L);
}
vec3 ptBsdfCos(Surface s,vec3 V,vec3 L){
    if(dot(s.geometricNormal,L)<=0.0)return vec3(0);
    return s.albedo*(1.0-s.metallic)*max(dot(s.normal,L),0.0)/RT_PI+
        (s.specularEnabled?ggxWeight(s,V,L)*ggxPdf(s,V,L):vec3(0));
}
bool ptTemporalPixel(Surface receiver,out ivec2 previous){
    if(temporal.samplingParams.y<.5)return false;
    vec4 clip=temporal.previousViewProjection*vec4(receiver.previousPosition,1);
    if(clip.w<=0.0)return false;
    // Unjittered motion convention: remove current jitter, not previous jitter.
    vec2 uv=clip.xy/clip.w*.5+.5-temporal.jitter.xy;
    if(any(lessThan(uv,vec2(0)))||any(greaterThanEqual(uv,vec2(1))))return false;
    previous=ivec2(uv*pc.extent.xy);return true;
}
