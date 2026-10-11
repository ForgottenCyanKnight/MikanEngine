// Prime-derived adapter: GPL-3.0-only; additional permissions in engine/licenses/prime-enhanced/LICENSE-EXCEPTIONS.
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
// Mixed-measure reconnection: the Jacobian contains geometry, never BSDF PDF ratios.
struct EShift {vec4 d;vec4 s;}; // d.w=Jacobian, s.w=new source geometry
EShift eZeroShift(){return EShift(vec4(0),vec4(0));}
EShift eShift(Surface primary,vec3 primaryView,vec3 eye,PTReservoir source,EReplay replay){
    EShift result=eZeroShift();
    if(source.diffuseWeight.w<=0.0||replay.d.w==0.0)return result;
    uint rcLength=eRcLength(source),pathLength=eLength(source);
    if(rcLength>pathLength+1u)return EShift(vec4(replay.d.rgb,1),vec4(replay.s.rgb,source.incidentGeometry.w));
    if((source.path.x&(E_DELTA_BEFORE|E_DELTA_AFTER))!=0u)return result;
    Surface previous;
    if(!eLoad(replay.hit,replay.positionBary.w,replay.positionBary.xyz,replay.viewFactor.xyz,previous)||previous.mirror)return result;
    bool emitter=rcLength==pathLength+1u,nee=eNee(source),finite=source.rcHit.x!=E_INVALID,neeAtRc=nee&&rcLength==pathLength;
    vec3 connection=source.directionPdf.xyz;float d2=0.0;Surface target;
    if(finite){
        vec3 displacement=source.rcPosition.xyz-previous.position;
        if(!(emitter&&nee))displacement=source.rcPosition.xyz-eSpawn(previous,displacement);
        d2=dot(displacement,displacement);if(d2<=1e-20)return result;
        connection=displacement*inversesqrt(d2);
        if(!eLoad(source.rcHit,uintBitsToFloat(source.path.w),source.rcPosition.xyz,-connection,target))return result;
    }
    if(dot(previous.geometricNormal,connection)<=0.0)return result;
    float firstPdf=ptPdf(previous,replay.viewFactor.xyz,connection);
    if(firstPdf<=0.0)return result;
    float geometry=finite?abs(dot(target.geometricNormal,-connection))/d2:1.0;
    if(geometry<=0.0||source.incidentGeometry.w<=0.0)return result;
    float jacobian=geometry/source.incidentGeometry.w;
    uint thresholds=source.path.y;float threshold=eThreshold(thresholds,eFootprint(primary,primaryView,eye));
    if(rcLength>1u&&replay.viewFactor.w>0.0&&replay.viewFactor.w/firstPdf>=threshold)return result;
    if(!(emitter&&nee)){
        if(!(emitter&&!nee)&&!eRough(firstPdf))return result;
        if(finite&&1.0/(firstPdf*geometry)<threshold)return result;
    }
    vec3 d=replay.d.rgb,sp=replay.s.rgb;
    eResponse(previous,replay.viewFactor.xyz,connection,rcLength==1u,d,sp);
    float secondPdf=0.0;vec3 suffix=source.incidentGeometry.rgb;
    if(emitter){
        float lightPdf=finite?eLightPdf(previous,target):eEnvironmentPdf(eLightType(source),connection);
        bool continuation=!nee||pathLength<uint(MAX_PATH_BOUNCES);
        float mis=continuation&&lightPdf>0.0?(nee?powerHeuristic(lightPdf,firstPdf):powerHeuristic(firstPdf,lightPdf)):1.0;
        suffix=(finite?target.emissive:eEnvironment(eLightType(source),connection))*mis;
    }else{
        if(target.mirror||dot(target.geometricNormal,source.directionPdf.xyz)<=0.0)return result;
        secondPdf=ptPdf(target,-connection,source.directionPdf.xyz);
        suffix*=ptBsdfCos(target,-connection,source.directionPdf.xyz);
        if(rcLength==pathLength){
            bool continuation=!nee||pathLength<uint(MAX_PATH_BOUNCES);
            float lightPdf=source.directionPdf.w;
            suffix*=continuation&&lightPdf>0.0?(nee?powerHeuristic(lightPdf,secondPdf):powerHeuristic(secondPdf,lightPdf)):1.0;
        }
    }
    if(!(emitter&&nee)){
        float inverseGeometry=finite?d2/max(abs(dot(previous.geometricNormal,-connection)),1e-30):0.0;
        if(!(emitter&&!finite)&&inverseGeometry/(emitter&&!nee?E_PSEUDO_PDF:max(secondPdf,1e-30))<threshold)return result;
    }
    vec3 begin=eSpawn(previous,connection),visibilityDirection=connection;float limit=100000.0;
    if(finite){vec3 segment=eSpawn(target,-connection)-begin;limit=length(segment);visibilityDirection=segment/max(limit,1e-30);}
    if(limit<=.001||traceOccluded(begin,visibilityDirection,limit,2u))return result;
    d*=suffix;sp*=suffix;
    if(!ptFinite(d)||!ptFinite(sp)||isinf(jacobian)||isnan(jacobian))return result;
    return EShift(vec4(d,jacobian),vec4(sp,geometry));
}
float eShiftTarget(EShift s){return diLuminance(s.d.rgb+s.s.rgb)*s.d.w;}
void eMerge(inout PTReservoir result,PTReservoir source,EShift shifted,float mis,inout float sum,inout uint rng){
    float mass=result.receiverM.w+source.receiverM.w;
    float w=eShiftTarget(shifted)*source.diffuseWeight.w*mis;
    if(w>0.0&&!isnan(w)&&!isinf(w)){
        sum+=w;
        if(eRandom(rng)*sum<w){result=source;result.diffuseWeight.rgb=shifted.d.rgb;
            result.specularDistance.rgb=shifted.s.rgb;result.incidentGeometry.w=shifted.s.w;}
    }else eRandom(rng);
    result.receiverM.w=mass;
}
void eFinish(inout PTReservoir result,PTReservoir canonical,float sum){
    float target=ptTarget(result);result.diffuseWeight.w=target>0.0?sum/target:0.0;
    if(isnan(result.diffuseWeight.w)||isinf(result.diffuseWeight.w))result.diffuseWeight.w=0;
    ptClampFirefly(result);
    result.receiverM.xyz=canonical.receiverM.xyz;
    result.seedPixelNormal.yzw=canonical.seedPixelNormal.yzw;
    result.primaryViewFootprint=canonical.primaryViewFootprint;
    // NRD guide belongs to the current receiver's actual fresh first scatter,
    // not to a selected donor's old camera/first-segment distance.
    result.specularDistance.w=canonical.specularDistance.w;
}
