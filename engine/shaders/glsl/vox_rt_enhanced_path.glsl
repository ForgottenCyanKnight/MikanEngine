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
// Terminal RIS, dual-footprint chart selection and prefix-only replay.
PTReservoir eGenerate(Surface primary,vec3 incoming,ivec2 pixel,int budget){
    uint seed=eSeed(uvec2(pixel),4u*(uint(temporal.options.x)+uint(temporal.samplingParams.w))),rng=seed;
    PTReservoir meta=ptEmpty();meta.receiverM=vec4(primary.position,1);
    meta.seedPixelNormal=uvec4(seed,uvec2(pixel),giPackNormal(primary.geometricNormal));
    meta.primaryViewFootprint=vec4(-incoming,eFootprint(primary,-incoming,pc.eye.xyz));
    meta.path=uvec4(255u<<8u,seed,0u,0u);
    float threshold=eThreshold(rng,meta.primaryViewFootprint.w),sourcePdf=1.0,sum=0.0;
    PTReservoir result=meta,rc=meta;
    vec3 wd=vec3(1),ws=vec3(0),suffix=vec3(1),view=-incoming,origin=pc.eye.xyz,previousNormal=view,previousPosition=pc.eye.xyz;
    float previousPdf=0,previousScatterPdf=1,lastPdf=1,firstDistance=65504.0;
    bool previousRough=false,previousDelta=false;
    Surface s=primary;
    for(uint bounce=0u;bounce<=uint(budget);++bounce){
        float d2=dot(s.position-origin,s.position-origin);
        float geometry=abs(dot(s.geometricNormal,view))/max(d2,1e-30);
        float inverseGeometry=d2/max(abs(dot(previousNormal,view)),1e-30);
        float footprint=1.0/max(geometry*previousScatterPdf,1e-30);
        float hitDraw=eRandom(rng);
        if(bounce>0u){
            // Previous receiver PDF is evaluated at the actual unoffset predecessor.
            Surface predecessor=primary;predecessor.position=previousPosition;
            float lightPdf=previousDelta?0.0:eLightPdf(predecessor,s);
            float mis=lightPdf>0.0?powerHeuristic(previousPdf,lightPdf):1.0;
            PTReservoir c=rc;c.path.x=(c.path.x&~(255u|E_NEE|(3u<<18u)))|(bounce-1u)|(E_EMITTER<<18u);
            c.diffuseWeight.rgb=wd*s.emissive*mis*sourcePdf;c.specularDistance.rgb=ws*s.emissive*mis*sourcePdf;
            c.incidentGeometry.rgb=eRcLength(rc)==bounce-1u?s.emissive:suffix*s.emissive*mis;
            c.directionPdf.w=lightPdf;
            bool force=eRcLength(rc)==255u&&min(footprint,inverseGeometry/E_PSEUDO_PDF)>=threshold;
            if(force){c.path.x=(c.path.x&~(255u<<8u))|(bounce<<8u);
                c.path.x|=previousDelta?E_DELTA_BEFORE:0u;eSetRc(c,s,geometry);
                c.incidentGeometry.rgb=s.emissive;c.directionPdf=vec4(0,0,0,lightPdf*geometry);
                c.diffuseWeight.rgb*=lastPdf;c.specularDistance.rgb*=lastPdf;}
            eAdd(result,c,wd*s.emissive*mis,ws*s.emissive*mis,hitDraw,sum);
        }
        float neeDraw=eRandom(rng);uint neeSeed=rng;ELight light=eSampleLight(s,rng);
        if(!s.mirror&&light.pdf>0.0){
            float pdf=ptPdf(s,view,light.direction),mis=bounce<uint(budget)?powerHeuristic(light.pdf,pdf):1.0;
            vec3 d=wd,sp=ws;eResponse(s,view,light.direction,bounce==0u,d,sp);
            vec3 pssD=d*light.Le*(mis/light.pdf),pssS=sp*light.Le*(mis/light.pdf);
            bool atRc=bounce>0u&&eRcLength(rc)==255u&&previousRough&&min(footprint,inverseGeometry/max(pdf,1e-30))>=threshold;
            PTReservoir c=rc;c.path.x=(c.path.x&~(255u|(3u<<18u)))|bounce|E_NEE|(light.type<<18u);
            c.diffuseWeight.rgb=pssD*sourcePdf;c.specularDistance.rgb=pssS*sourcePdf;c.directionPdf.w=light.pdf;
            if(atRc){
                c.path.x=(c.path.x&~(255u<<8u))|(bounce<<8u);eSetRc(c,s,geometry);
                c.incidentGeometry.rgb=light.Le;c.directionPdf=vec4(light.direction,light.pdf);c.path.z=neeSeed;
                c.diffuseWeight.rgb*=lastPdf*light.pdf;c.specularDistance.rgb*=lastPdf*light.pdf;
            }else if(eRcLength(rc)==255u){
                c.path.x=(c.path.x&~(255u<<8u))|((bounce+1u)<<8u);
                c.rcHit=uvec4(E_INVALID);c.incidentGeometry=vec4(light.Le,light.geometry);
                c.directionPdf=vec4(light.direction,light.finite?light.pdf*light.geometry:light.pdf);
                if(light.finite)eSetRc(c,light.hit,light.geometry);
                c.diffuseWeight.rgb*=light.pdf;c.specularDistance.rgb*=light.pdf;
            }else c.incidentGeometry.rgb=suffix*ptBsdfCos(s,view,light.direction)*light.Le*(mis/light.pdf);
            eAdd(result,c,pssD,pssS,neeDraw,sum);
        }
        if(bounce>=uint(budget))break;
        float roulette=eRandom(rng);
        vec3 physical=bounce==0u?vec3(1):wd*primary.albedo*(1.0-primary.metallic)+ws*specularMaterialFactor(primary,incoming);
        float survival=clamp(diLuminance(physical),0.0,1.0);
        if(survival<=0.0||roulette>=survival)break;
        wd/=survival;ws/=survival;sourcePdf*=survival;
        float pdf;bool delta;vec3 direction=eScatter(s,view,rng,pdf,delta);
        if(pdf<=0.0||(!delta&&dot(s.geometricNormal,direction)<=0.0))break;
        float scatterPdf=delta?1e20:pdf;bool rough=eRough(scatterPdf);
        bool connect=bounce>0u&&eRcLength(rc)==255u&&previousRough&&min(footprint,inverseGeometry/scatterPdf)>=threshold;
        eWeights(s,view,direction,pdf,bounce==0u,wd,ws);
        suffix*=delta?s.albedo:ptBsdfCos(s,view,direction)/pdf;
        if(connect){
            rc.path.x=(rc.path.x&~(255u<<8u))|(bounce<<8u)|(delta?E_DELTA_AFTER:0u);
            eSetRc(rc,s,geometry);rc.directionPdf.xyz=direction;rc.path.z=rng;
            sourcePdf*=lastPdf*(delta?1.0:pdf);suffix=vec3(1);
        }
        previousNormal=s.geometricNormal;previousDelta=delta;previousRough=rough;
        previousPdf=delta?0.0:pdf;previousScatterPdf=scatterPdf;lastPdf=delta?1.0:pdf;
        previousPosition=s.position;origin=eSpawn(s,direction);
        Surface next;
        if(!traceSurface(origin,direction,100000,next)){
            for(uint type=0u;type<2u;++type){
                float u=eRandom(rng),lightPdf=eEnvironmentPdf(type,direction),mis=previousPdf>0.0&&lightPdf>0.0?powerHeuristic(previousPdf,lightPdf):1.0;
                vec3 Le=eEnvironment(type,direction);PTReservoir c=rc;
                c.path.x=(c.path.x&~(255u|E_NEE|(3u<<18u)))|bounce|(type<<18u);
                c.diffuseWeight.rgb=wd*Le*mis*sourcePdf;c.specularDistance.rgb=ws*Le*mis*sourcePdf;
                c.incidentGeometry.rgb=eRcLength(rc)==bounce?Le:suffix*Le*mis;c.directionPdf.w=lightPdf;
                if(eRcLength(rc)==255u&&rough){c.path.x=(c.path.x&~(255u<<8u))|((bounce+1u)<<8u);
                    c.rcHit=uvec4(E_INVALID);c.incidentGeometry=vec4(Le,1);c.directionPdf=vec4(direction,lightPdf);
                    c.diffuseWeight.rgb*=lastPdf;c.specularDistance.rgb*=lastPdf;}
                eAdd(result,c,wd*Le*mis,ws*Le*mis,u,sum);
            }
            break;
        }
        if(bounce==0u)firstDistance=min(distance(next.position,primary.position),65504.0);
        s=next;view=-direction;
    }
    result.receiverM=meta.receiverM;result.seedPixelNormal=meta.seedPixelNormal;
    result.primaryViewFootprint=meta.primaryViewFootprint;result.specularDistance.w=firstDistance;
    float target=ptTarget(result);result.diffuseWeight.w=target>0.0?sum/target:0.0;
    if(!ptFinite(result.diffuseWeight.rgb)||!ptFinite(result.specularDistance.rgb)||isinf(result.diffuseWeight.w))result.diffuseWeight.w=0.0;
    ptClampFirefly(result);
    return result;
}
struct EReplay {uvec4 hit;vec4 positionBary;vec4 viewFactor;vec4 d;vec4 s;};
EReplay eEmptyReplay(){return EReplay(uvec4(E_INVALID),vec4(0),vec4(0),vec4(0),vec4(0));}
EReplay eDirectReplay(Surface primary,vec3 view){
    return EReplay(eHit(primary),vec4(primary.position,primary.hitBary.y),vec4(view,0),vec4(1,1,1,1),vec4(0));
}
EReplay eReplay(Surface primary,vec3 view,vec3 eye,PTReservoir source){
    EReplay result=eEmptyReplay();uint rcLength=eRcLength(source),pathLength=eLength(source),rng=source.path.y;
    bool connect=rcLength<=pathLength+1u;float threshold=eThreshold(rng,eFootprint(primary,view,eye));
    Surface hit=primary;vec3 origin=eye,previousNormal=view,previousPosition=eye;
    vec3 wd=vec3(1),ws=vec3(0);float scatterPdf=1,previousPdf=0;bool previousRough=false;
    for(uint bounce=0u;bounce<=uint(MAX_PATH_BOUNCES)+1u;++bounce){
        float d2=dot(hit.position-origin,hit.position-origin);
        float geometry=abs(dot(hit.geometricNormal,view))/max(d2,1e-30),inverseGeometry=d2/max(abs(dot(previousNormal,view)),1e-30);
        float footprint=1.0/max(geometry*scatterPdf,1e-30);
        eRandom(rng);
        if(connect&&bounce==rcLength-1u){
            if(hit.mirror)return result;
            float factor=!previousRough||footprint<threshold?-1.0:inverseGeometry;
            result=EReplay(eHit(hit),vec4(hit.position,hit.hitBary.y),vec4(view,factor),vec4(wd,1),vec4(ws,0));return result;
        }
        if(!eNee(source)&&bounce==pathLength+1u){
            if(connect||min(footprint,inverseGeometry/E_PSEUDO_PDF)>=threshold)return result;
            Surface predecessor=primary;predecessor.position=previousPosition;
            float lightPdf=previousPdf>0.0?eLightPdf(predecessor,hit):0.0,mis=lightPdf>0.0?powerHeuristic(previousPdf,lightPdf):1.0;
            result.d=vec4(wd*hit.emissive*mis,1);result.s=vec4(ws*hit.emissive*mis,0);return result;
        }
        if(bounce>uint(MAX_PATH_BOUNCES)||(eNee(source)&&bounce==pathLength))return result;
        eRandom(rng);for(uint i=0u;i<5u;++i)eRandom(rng);eRandom(rng);
        float pdf;bool delta;vec3 direction=eScatter(hit,view,rng,pdf,delta);
        if(pdf<=0.0)return result;float samplePdf=delta?1e20:pdf;
        if(bounce>0u&&previousRough&&min(footprint,inverseGeometry/samplePdf)>=threshold)return result;
        eWeights(hit,view,direction,pdf,bounce==0u,wd,ws);
        previousNormal=hit.geometricNormal;previousPosition=hit.position;
        previousPdf=delta?0.0:pdf;scatterPdf=samplePdf;previousRough=eRough(samplePdf);origin=eSpawn(hit,direction);
        Surface next;
        if(!traceSurface(origin,direction,100000,next)){
            if(connect||eNee(source)||bounce!=pathLength||previousRough||eLightType(source)==E_EMITTER)return result;
            float lightPdf=eEnvironmentPdf(eLightType(source),direction),mis=previousPdf>0.0&&lightPdf>0.0?powerHeuristic(previousPdf,lightPdf):1.0;
            vec3 Le=eEnvironment(eLightType(source),direction)*mis;result.d=vec4(wd*Le,1);result.s=vec4(ws*Le,0);return result;
        }
        hit=next;view=-direction;
    }
    return result;
}
