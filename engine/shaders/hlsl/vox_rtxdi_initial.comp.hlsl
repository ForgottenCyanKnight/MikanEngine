// RTXDI initial sampling pass (uniform local light selection, one proposal).
#pragma pack_matrix(row_major)
#define RTXDI_ENABLE_PRESAMPLING 0
#define RTXDI_LIGHT_RESERVOIR_BUFFER Reservoirs
#include "vox_rtxdi_shared.hlsli"
#include <Rtxdi/DI/InitialSampling.hlsli>
#include <Rtxdi/DI/ReservoirStorage.hlsli>

RTXDI_ReservoirBufferParameters ReservoirParams()
{
    RTXDI_ReservoirBufferParameters p = (RTXDI_ReservoirBufferParameters)0;
    p.reservoirBlockRowPitch = Const.reservoirParams.x;
    p.reservoirArrayPitch = Const.reservoirParams.y;
    return p;
}

[numthreads(8, 8, 1)]
void main(uint3 groupID : SV_GroupID,uint3 localID : SV_GroupThreadID)
{
    uint2 globalIndex=ActiveTilePixel(groupID,localID);
    if (any(globalIndex >= uint2(Const.extent.xy))) return;
    RTXDI_RuntimeParameters params = (RTXDI_RuntimeParameters)0;
    params.neighborOffsetMask = Const.runtimeParams.x;
    params.activeCheckerboardField = Const.runtimeParams.y;
    params.frameIndex = Const.runtimeParams.z;

    RTXDI_RandomSamplerState rng = RTXDI_InitRandomSampler(globalIndex, Const.runtimeParams.z, 0x1F123BBBu);

    RAB_Surface surface = RAB_GetGBufferSurface(globalIndex, false);

    RTXDI_LightBufferRegion region = (RTXDI_LightBufferRegion)0;
    region.firstLightIndex = 0;
    region.numLights = Const.diParams.x;

    RTXDI_LightBufferParameters lightBufferParams = (RTXDI_LightBufferParameters)0;
    lightBufferParams.localLightBufferRegion = region;

    RTXDI_DIInitialSamplingParameters sampleParams = (RTXDI_DIInitialSamplingParameters)0;
    sampleParams.numLocalLightSamples = Const.diParams.y;
    sampleParams.brdfRayMinT = 0.0;
    sampleParams.localLightSamplingMode = 0; // ReSTIRDI_LocalLightSamplingMode::Uniform

    RAB_LightSample selectedSample = RAB_EmptyLightSample();
    RTXDI_DIReservoir reservoir = RTXDI_EmptyDIReservoir(); if (RAB_IsSurfaceValid(surface)) reservoir = RTXDI_SampleLightsForSurface(rng, rng, surface, sampleParams, lightBufferParams, selectedSample);

    RTXDI_StoreDIReservoir(reservoir, ReservoirParams(), globalIndex, Const.bufferIndices.x);
}
