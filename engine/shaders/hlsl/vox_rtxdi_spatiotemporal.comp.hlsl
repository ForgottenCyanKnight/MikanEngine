// Current-frame RTXDI spatial reuse. Temporal reuse is deliberately disabled
// until complete per-view previous-frame geometry and reservoir ownership exist.
#pragma pack_matrix(row_major)
#define RTXDI_ENABLE_PRESAMPLING 0
#define RTXDI_LIGHT_RESERVOIR_BUFFER Reservoirs
#include "vox_rtxdi_shared.hlsli"
#include <Rtxdi/DI/ReSTIRDIParameters.h>
#include <Rtxdi/DI/SpatialResampling.hlsli>
#include <Rtxdi/DI/ReservoirStorage.hlsli>
[numthreads(8,8,1)]
void main(uint3 groupID : SV_GroupID,uint3 localID : SV_GroupThreadID)
{
    uint2 pixel=ActiveTilePixel(groupID,localID);
    if(any(pixel >= uint2(Const.extent.xy))) return;
    RTXDI_RuntimeParameters runtime=(RTXDI_RuntimeParameters)0;
    runtime.neighborOffsetMask=Const.runtimeParams.x;
    runtime.frameIndex=Const.runtimeParams.z;
    RTXDI_ReservoirBufferParameters storage=(RTXDI_ReservoirBufferParameters)0;
    storage.reservoirBlockRowPitch=Const.reservoirParams.x;
    storage.reservoirArrayPitch=Const.reservoirParams.y;
    RTXDI_RandomSamplerState rng=RTXDI_InitRandomSampler(pixel,Const.runtimeParams.z,0x2F123BBBu);
    RAB_Surface surface=RAB_GetGBufferSurface(pixel,false);
    RTXDI_DIReservoir result=RTXDI_EmptyDIReservoir();
    if(RAB_IsSurfaceValid(surface)){
        RTXDI_DIReservoir center=RTXDI_LoadDIReservoir(storage,pixel,Const.bufferIndices.x);
        RTXDI_DISpatialResamplingParameters spatial=(RTXDI_DISpatialResamplingParameters)0;
        spatial.numSamples=Const.diParams.z;
        spatial.numDisocclusionBoostSamples=Const.diParams.z;
        spatial.samplingRadius=16.0;
        spatial.normalThreshold=Const.thresholds.x;
        spatial.depthThreshold=Const.thresholds.y;
        spatial.biasCorrectionMode=RTXDI_BIAS_CORRECTION_BASIC;
        spatial.enableMaterialSimilarityTest=1;
        RAB_LightSample selected=RAB_EmptyLightSample();
        result=RTXDI_DISpatialResampling(pixel,surface,center,rng,runtime,storage,Const.bufferIndices.x,spatial,selected);
    }
    RTXDI_StoreDIReservoir(result,storage,pixel,Const.bufferIndices.z);
}