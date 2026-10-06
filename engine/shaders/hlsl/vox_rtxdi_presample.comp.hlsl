// RTXDI local-light RIS tile fill. The engine's emissive light table already
// carries the normalized cumulative power CDF in Lights[2 + i*4].w (the same
// distribution the GLSL NEE path binary-searches), so tiles are drawn directly
// from it and stored in the RTXDI_RIS_BUFFER uint2(lightIndex, invSourcePdf)
// contract. RTXDI_PresampleLocalLights would require a PDF mipmap texture; this
// variant reaches the identical POWER_RIS proposal distribution without it.
#pragma pack_matrix(row_major)
#define RTXDI_ENABLE_PRESAMPLING 1
#define RTXDI_LIGHT_RESERVOIR_BUFFER Reservoirs
#include "vox_rtxdi_shared.hlsli"

uint rtxdiHash(uint x)
{
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}

[numthreads(64, 1, 1)]
void main(uint3 groupID : SV_GroupID, uint3 localID : SV_GroupThreadID)
{
    uint index = groupID.x * 64u + localID.x;
    uint tileSize = Const.risParams.x;
    uint tileCount = Const.risParams.y;
    uint total = tileSize * tileCount;
    if (index >= total) return;

    uint count = Const.diParams.x;
    if (count == 0u) { RISBuffer[index] = uint2(0u, 0u); return; }

    uint seed = rtxdiHash(index ^ (Const.runtimeParams.z * 0x85ebca77u));
    float choice = float(rtxdiHash(seed) & 0x00ffffffu) / 16777216.0;

    uint low = 0u, high = count;
    while (low < high)
    {
        uint mid = low + (high - low) / 2u;
        if (choice < Lights[2u + mid * 4u].w) high = mid; else low = mid + 1u;
    }
    uint light = min(low, count - 1u);

    float cHi = Lights[2u + light * 4u].w;
    float cLo = (light == 0u) ? 0.0f : Lights[2u + (light - 1u) * 4u].w;
    float pdf = max(cHi - cLo, 1e-8f);

    RISBuffer[index] = uint2(light, asuint(1.0f / pdf));
}
