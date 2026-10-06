// RTXDI application bridge for MikanEngine's hardware-RT viewport.
// Implements the RAB_* interface required by the RTXDI DI includes over this
// engine's resources: the emissive triangle light list (the same float4 layout
// the GLSL passes use), primary-surface G-buffer images, and Lambert BRDF.
// Receivers are diffuse (albedo from the material guide, already weighted by
// throughput and (1-metallic)); environment and BRDF sampling are disabled,
// the boiling filter is disabled, and checkerboard is off.

#pragma pack_matrix(row_major)

#include <Rtxdi/RtxdiParameters.h>
#include <Rtxdi/Utils/RandomSamplerState.hlsli>

struct RtxdiConsts
{
    uint4 runtimeParams;      // x neighborOffsetMask, y checkerboard (0), z frameIndex
    uint4 reservoirParams;    // x blockRowPitch, y arrayPitch
    uint4 bufferIndices;      // x initialOutput, y temporalInput, z spatiotemporalOutput, w shadingInput
    uint4 diParams;           // x lightCount, y initial candidates, z spatial samples, w specular enabled
    float4 thresholds;        // x normalThreshold, y depthThreshold, z jitterX, w jitterY
    float4x4 previousViewProj;
    float4 cameraPosition;
    float4 extent;            // width, height, tileCull, 0
    float4x4 inverseRayViewProj;
    uint4 risParams;          // x local tileSize, y local tileCount, z env bufferOffset, w spare
};

[[vk::binding(1, 0)]] Texture2D<float> GViewZ;
[[vk::binding(2, 0)]] Texture2D<float4> GNormal;
[[vk::binding(3, 0)]] Texture2D<float2> GWorldPos;
[[vk::binding(4, 0)]] Texture2D<float> GViewZPrev;
[[vk::binding(5, 0)]] Texture2D<float4> GNormalPrev;
[[vk::binding(6, 0)]] Texture2D<float4> GMaterial;
[[vk::binding(7, 0)]] StructuredBuffer<float4> Lights;
[[vk::binding(8, 0)]] RWStructuredBuffer<RTXDI_PackedDIReservoir> Reservoirs;
[[vk::binding(9, 0)]] StructuredBuffer<float2> NeighborOffsets;
#define RTXDI_NEIGHBOR_OFFSETS_BUFFER NeighborOffsets
[[vk::binding(10, 0)]] ConstantBuffer<RtxdiConsts> Const;

[[vk::binding(11, 0)]] Texture2D<float4> GSpecular;

// Presampled local-light tiles (RTXDI_POWER_RIS initial sampling). Entries are
// uint2(lightIndex, invSourcePdf) per RTXDI_RIS_BUFFER contract.
[[vk::binding(13, 0)]] RWStructuredBuffer<uint2> RISBuffer;
#define RTXDI_RIS_BUFFER RISBuffer

// The RAB surface: primary-hit receiver for direct lighting.
struct RAB_Surface
{
    float3 worldPos;
    float3 normal;
    float viewDepth;
    float3 albedo;   // demodulated diffuse (material guide)
    float3 f0;
    float3 viewDir;
    float roughness;
    bool specularEnabled;
    bool valid;
};

struct RAB_LightInfo
{
    float3 p0, p1, p2;
    float3 radiance;
    float selectionPdf;
    float area;
};

struct RAB_LightSample
{
    float3 position;
    float3 normal;
    float3 radiance;
    float neePdf;
    float solidAnglePdf;
};

RAB_Surface RAB_EmptySurface()
{
    RAB_Surface s = (RAB_Surface)0;
    s.valid = false;
    return s;
}

bool RAB_IsSurfaceValid(RAB_Surface surface)
{
    return surface.valid;
}

float3 RAB_GetSurfaceWorldPos(RAB_Surface surface) { return surface.worldPos; }
float3 RAB_GetSurfaceNormal(RAB_Surface surface) { return surface.normal; }
float RAB_GetSurfaceLinearDepth(RAB_Surface surface) { return surface.viewDepth; }

// Current frame reads the world-position image; previous frame only carries
// view depth and normal (sufficient for the DI compatibility tests).
RAB_Surface RAB_GetGBufferSurface(uint2 pixelPosition, bool previousFrame)
{
    RAB_Surface surface = RAB_EmptySurface();
    if (any(pixelPosition >= uint2(Const.extent.xy))) return surface;
    float viewDepth = previousFrame ? GViewZPrev[pixelPosition] : GViewZ[pixelPosition];
    float3 normal = previousFrame ? GNormalPrev[pixelPosition].xyz : GNormal[pixelPosition].xyz;
    float2 distanceValid = previousFrame ? float2(0, 0) : GWorldPos[pixelPosition];
    if (viewDepth >= 1e5) return surface;
    if (!previousFrame && distanceValid.y < 0.5) return surface;
    // Match GLSL primary rays exactly, including near-plane origin and jitter.
    float2 ndc=(float2(pixelPosition)+0.5)/Const.extent.xy*2.0-1.0;
    float4 nearPoint=mul(float4(ndc,0,1),Const.inverseRayViewProj);
    float4 farPoint=mul(float4(ndc,1,1),Const.inverseRayViewProj);
    nearPoint/=nearPoint.w;farPoint/=farPoint.w;
    surface.worldPos=previousFrame?float3(0,0,0):nearPoint.xyz+distanceValid.x*normalize(farPoint.xyz-nearPoint.xyz);
    surface.normal = normalize(normal);
    surface.viewDepth = viewDepth;
    surface.albedo = GMaterial[pixelPosition].rgb;
    float4 spec = GSpecular[pixelPosition];
    surface.f0 = spec.rgb;
    surface.specularEnabled = Const.runtimeParams.w != 0 && spec.a > 0.5;
    surface.roughness = GNormal[pixelPosition].w;
    surface.viewDir = normalize(Const.cameraPosition.xyz - surface.worldPos);
    surface.valid = dot(surface.normal, surface.normal) > 0.5;
    return surface;
}

RAB_LightInfo RAB_LoadLightInfo(uint lightIndex, bool previousFrame)
{
    RAB_LightInfo info = (RAB_LightInfo)0;
    uint base = 1u + lightIndex * 4u;
    info.radiance = Lights[base].rgb;
    info.area = Lights[base].w;
    info.selectionPdf = Lights[base + 2u].w;
    info.p0 = Lights[base + 1u].xyz;
    info.p1 = Lights[base + 2u].xyz;
    info.p2 = Lights[base + 3u].xyz;
    return info;
}

RAB_LightSample RAB_SamplePolymorphicLight(RAB_LightInfo lightInfo, RAB_Surface surface, float2 uv)
{
    RAB_LightSample sample = (RAB_LightSample)0;
    float root = sqrt(uv.x);
    uv = float2(root * (1.0 - uv.y), root * uv.y);
    float w0 = 1.0 - uv.x - uv.y;
    float3 position = lightInfo.p0 * w0 + lightInfo.p1 * uv.x + lightInfo.p2 * uv.y;
    sample.position = position;
    sample.normal = normalize(cross(lightInfo.p1 - lightInfo.p0, lightInfo.p2 - lightInfo.p0));
    sample.radiance = lightInfo.radiance;
    float3 offset = position - surface.worldPos;
    float dist2 = dot(offset, offset);
    if (dist2 < 1e-10) { sample.solidAnglePdf = 0; return sample; }
    float facing = abs(dot(sample.normal, -offset * rsqrt(dist2)));
    if (facing <= 0.0) { sample.solidAnglePdf = 0; return sample; }
    // Uniform area sample on the triangle: pdf_sa = dist^2 / (area * cos_light).
    sample.solidAnglePdf = dist2 / max(lightInfo.area * facing, 1e-12);
    sample.neePdf = lightInfo.selectionPdf * sample.solidAnglePdf;
    return sample;
}

// Shared diffuse/GGX target. GGX uses the same VNDF PDF and complementary
// emitter MIS as the continuation ray in vox_rt_gi.glsl.
float RAB_GgxLambda(float c, float alpha)
{
    float c2 = max(c*c, 1e-8);
    return 0.5 * (sqrt(1 + alpha*alpha*(1-c2)/c2) - 1);
}
float RAB_GetLightSampleTargetPdfForSurface(RAB_LightSample lightSample, RAB_Surface surface)
{
    if (lightSample.solidAnglePdf <= 0) return 0;
    float3 offset = lightSample.position - surface.worldPos;
    float dist2 = dot(offset, offset);
    if (dist2 < 1e-10) return 0;
    float3 L = offset * rsqrt(dist2);
    float nl = saturate(dot(surface.normal, L));
    float reflected = dot(lightSample.radiance, float3(0.2126, 0.7152, 0.0722))
        * nl / 3.14159265 * dot(surface.albedo, float3(0.2126, 0.7152, 0.0722));
    float nv = dot(surface.normal, surface.viewDir);
    float3 sum = surface.viewDir + L;
    if (surface.specularEnabled && nv > 0 && nl > 0 && dot(sum,sum) > 1e-12)
    {
        float3 H = normalize(sum);
        float nh = saturate(dot(surface.normal,H));
        float alpha = max(surface.roughness*surface.roughness,1e-4);
        float a2 = alpha*alpha;
        float d = (1-nh*nh)+nh*nh*a2;
        float lv = RAB_GgxLambda(nv,alpha), ll = RAB_GgxLambda(nl,alpha);
        float pdf = a2 / (3.14159265*d*d) / (4*nv*(1+lv));
        float3 F = surface.f0 + (1-surface.f0)*pow(1-saturate(dot(surface.viewDir,H)),5);
        float nee = lightSample.neePdf;
        float mis = nee*nee / max(nee*nee + pdf*pdf,1e-30);
        float3 spec = lightSample.radiance * F * ((1+lv)/(1+lv+ll)) * pdf * mis;
        reflected += dot(spec,float3(0.2126,0.7152,0.0722));
    }
    return reflected / lightSample.solidAnglePdf;
}

float RAB_LightSampleSolidAnglePdf(RAB_LightSample lightSample) { return lightSample.solidAnglePdf; }
RAB_LightInfo RAB_EmptyLightInfo() { return (RAB_LightInfo)0; }
RAB_LightSample RAB_EmptyLightSample() { return (RAB_LightSample)0; }
bool RAB_IsAnalyticLightSample(RAB_LightSample lightSample) { return false; }
float3 RAB_LightSamplePosition(RAB_LightSample lightSample) { return lightSample.position; }
float3 RAB_LightSampleRadiance(RAB_LightSample lightSample) { return lightSample.radiance; }

// Lambert BRDF pdf wrt solid angle (only used when BRDF MIS is enabled; this
// integration disables BRDF sampling).
float RAB_SurfaceEvaluateBrdfPdf(RAB_Surface surface, float3 lightDir)
{
    return saturate(dot(surface.normal, lightDir)) / 3.14159265;
}

int2 RAB_ClampSamplePositionIntoView(int2 pixelPosition, bool previousFrame)
{
    return clamp(pixelPosition, int2(0, 0), int2(Const.extent.xy) - 1);
}

void RAB_GetLightDirDistance(RAB_Surface surface, RAB_LightSample lightSample, out float3 lightDir, out float lightDistance)
{
    float3 offset = lightSample.position - surface.worldPos;
    lightDistance = length(offset);
    lightDir = lightDistance > 0.0 ? offset / lightDistance : float3(0, 0, 1);
}

// Initial visibility is disabled (enableInitialVisibility=0); the stub keeps
// the dead path compilable.
bool RAB_GetConservativeVisibility(RAB_Surface surface, RAB_LightSample lightSample)
{
    return true;
}

bool RAB_GetTemporalConservativeVisibility(RAB_Surface surface, RAB_Surface neighborSurface, RAB_LightSample lightSample)
{
    return true;
}

// BRDF sampling and environment lights are disabled in this integration; the
// stubs below only satisfy the compiler for the dead code paths.
bool RAB_SurfaceImportanceSampleBrdf(RAB_Surface surface, inout RTXDI_RandomSamplerState rng, out float3 sampleDir)
{
    sampleDir = float3(0, 1, 0);
    return false;
}

bool RAB_TraceRayForLocalLight(float3 origin, float3 direction, float tMin, float tMax,
    out uint lightIndex, out float2 randXY)
{
    lightIndex = RTXDI_InvalidLightIndex;
    randXY = float2(0, 0);
    return false;
}

float RAB_EvaluateLocalLightSourcePdf(uint lightIndex) { return 1.0; }

struct RAB_Material { float luminance; float3 f0; float roughness; bool specularEnabled; };
RAB_Material RAB_GetMaterial(RAB_Surface surface)
{
    RAB_Material m;
    m.luminance = dot(surface.albedo, float3(0.2126, 0.7152, 0.0722));
    m.f0 = surface.f0; m.roughness = surface.roughness; m.specularEnabled = surface.specularEnabled;
    return m;
}
bool RAB_AreMaterialsSimilar(RAB_Material a, RAB_Material b)
{
    if (a.specularEnabled != b.specularEnabled) return false;
    if (a.specularEnabled && (abs(a.roughness-b.roughness)>0.05 || any(abs(a.f0-b.f0)>0.05))) return false;
    return abs(a.luminance - b.luminance) <= 0.05 + 0.2 * b.luminance;
}
float RAB_EvaluateEnvironmentMapSamplingPdf(float3 direction) { return 0.0; }
float2 RAB_GetEnvironmentMapRandXYFromDir(float3 direction) { return float2(0, 0); }

// Light indices are stable across frames while the light count is unchanged;
// the DI pass drops temporal history when the count changes.
int RAB_TranslateLightIndex(uint lightIndex, bool previousFrame)
{
    return (lightIndex < Const.diParams.x) ? int(lightIndex) : -1;
}

void RAB_ApplyPermutationSampling(inout int2 prevPixelPos, uint uniformRandomNumber) {}

// Compact-light storage is unused: the RIS fill pass stores plain light
// indices and never sets RTXDI_LIGHT_COMPACT_BIT. The stubs keep the SDK's
// unpack path compilable with RTXDI_ENABLE_PRESAMPLING=1.
RAB_LightInfo RAB_LoadCompactLightInfo(uint bufferIndex)
{
    return RAB_EmptyLightInfo();
}

bool RAB_StoreCompactLightInfo(uint bufferIndex, RAB_LightInfo lightInfo)
{
    return false;
}


// Same indirect work list as the GLSL primary/shading passes. Header is uint4.
[[vk::binding(12, 0)]] StructuredBuffer<uint> ActiveTiles;
uint2 ActiveTilePixel(uint3 groupID,uint3 localID){
    if(Const.extent.z<0.5)return groupID.xy*8u+localID.xy;
    uint columns=(uint(Const.extent.x)+7u)/8u,tile=ActiveTiles[4u+groupID.x];
    return uint2(tile%columns,tile/columns)*8u+localID.xy;
}