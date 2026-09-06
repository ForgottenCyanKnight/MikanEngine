#version 450
#extension GL_GOOGLE_include_directive : require
#include "atmo_common.glsl"

// PostProcessQuad places the CameraUBO immediately after the texture inputs.
// cloud_high_map is input slot/binding 9, so this pass's UBO is binding 10.
layout(set = 0, binding = 10, std140) uniform CameraUBO
{
    vec4 cameraPos;
    mat4 proj;
    mat4 view;
    mat4 prevViewProj;
    mat4 invProj;
    mat4 invView;
    mat4 csmMatrices[4];
    vec4 csmSplitFars;
    vec4 csmParams;
    vec4 cloudParams0;
    vec4 cloudParams1;
    vec4 cloudNoiseOffsetKm;
    vec4 cloudLightingParams;
    mat4 cloudPrevViewProj;
    vec4 cloudShapeParams;
    vec4 cloudWindOffsetKm;
    vec4 cloudPrevWindOffsetKm;
    vec4 cloudHighParams0;
    vec4 cloudHighParams1;
    vec4 cloudHighWindOffsetKm;
    vec4 cloudHighPrevWindOffsetKm;
    vec4 cloudHighWindDirectionXZ;
} cam;

layout(push_constant, std430) uniform PC
{
    vec4 cameraPos;
    vec4 sunDir;
    vec4 lightColor;
    vec4 frameInfo;
} pc;

layout(set = 0, binding = 1) uniform sampler3D cloudBaseShapeTex;
layout(set = 0, binding = 6) uniform sampler2D cloudMotionTex;
layout(set = 0, binding = 5) uniform sampler3D cloudDetailsTex;
layout(set = 0, binding = 3) uniform sampler2D cloudHistoryTex;
layout(set = 0, binding = 4) uniform sampler2D transmittanceLUT;
layout(set = 0, binding = 0) uniform sampler2D sceneDepth;
layout(set = 0, binding = 2) uniform sampler2D bluenoiseTex;
layout(set = 0, binding = 7) uniform sampler3D scatteringLUT;
layout(set = 0, binding = 8) uniform sampler2D cloudHighTex;
layout(set = 0, binding = 9) uniform sampler2D cloudHighMapTex;
layout(location = 0) out vec4 outCloud;
layout(location = 0) in vec2 fragTexCoord;

const float CLOUD_ATMO_TOP_ALTITUDE_KM = 60.0;

vec3 ReconstructViewRay(vec2 uv)
{
    vec4 farPos = cam.invProj * vec4((uv * 2.0) - vec2(1.0), 1.0, 1.0);
    return normalize(farPos.xyz / vec3(max(farPos.w, 9.9999999747524270787835121154785e-07)));
}

vec3 ToHspePosition(vec3 worldPosition)
{
    return (worldPosition * 0.001000000047497451305389404296875) + vec3(0.0, 6371.0, 0.0);
}

float CloudBaseAltitudeKm()
{
    return max(cam.cloudParams0.w, 0.0);
}

float CloudBaseRadiusKm()
{
    return 6371.0 + CloudBaseAltitudeKm();
}

float CloudThicknessKm()
{
    return max(cam.cloudParams1.x, 0.00999999977648258209228515625);
}

float CloudTopAltitudeKm()
{
    return CloudBaseAltitudeKm() + CloudThicknessKm();
}

float CloudTopRadiusKm()
{
    return 6371.0 + CloudTopAltitudeKm();
}

bool RaySphereInterval(vec3 origin, vec3 direction, float radius, inout float nearDistance, inout float farDistance)
{
    float projected = dot(origin, direction);
    float radiusSquared = radius * radius;
    float discriminant = (projected * projected) - (dot(origin, origin) - radiusSquared);
    if (discriminant < 0.0)
    {
        return false;
    }
    float root = sqrt(max(discriminant, 0.0));
    nearDistance = (-projected) - root;
    farDistance = (-projected) + root;
    return farDistance >= 0.0;
}

bool IntersectCloudLayer(vec3 worldOrigin, vec3 direction, inout float enterDistance, inout float exitDistance)
{
    vec3 param = worldOrigin;
    vec3 origin = ToHspePosition(param);
    float cameraRadius = length(origin);
    float baseRadius = CloudBaseRadiusKm();
    float topRadius = CloudTopRadiusKm();
    if (cameraRadius < baseRadius)
    {
        vec3 param_1 = origin;
        vec3 param_2 = direction;
        float param_3 = 6371.0;
        float param_4;
        float param_5;
        bool _1040 = RaySphereInterval(param_1, param_2, param_3, param_4, param_5);
        float groundNear = param_4;
        float groundFar = param_5;
        if (_1040 && (groundFar > 9.9999997473787516355514526367188e-05))
        {
            return false;
        }
    }
    vec3 param_6 = origin;
    vec3 param_7 = direction;
    float param_8 = topRadius;
    float param_9;
    float param_10;
    bool _1059 = RaySphereInterval(param_6, param_7, param_8, param_9, param_10);
    float outerNear = param_9;
    float outerFar = param_10;
    if (!_1059)
    {
        return false;
    }
    float rayStart = max(outerNear, 0.0);
    float rayEnd = outerFar;
    vec3 param_11 = origin;
    vec3 param_12 = direction;
    float param_13 = baseRadius;
    float param_14;
    float param_15;
    bool _1083 = RaySphereInterval(param_11, param_12, param_13, param_14, param_15);
    float innerNear = param_14;
    float innerFar = param_15;
    bool hitsInner = _1083;
    if (cameraRadius < baseRadius)
    {
        if ((!hitsInner) || (innerFar <= rayStart))
        {
            return false;
        }
        rayStart = max(rayStart, innerFar);
    }
    else
    {
        if (cameraRadius < topRadius)
        {
            rayStart = 0.0;
            if (hitsInner && (innerNear > 0.0))
            {
                rayEnd = min(rayEnd, innerNear);
            }
        }
        else
        {
            if (hitsInner && (innerNear > rayStart))
            {
                rayEnd = min(rayEnd, innerNear);
            }
        }
    }
    enterDistance = max(rayStart, 0.0);
    exitDistance = rayEnd;
    return exitDistance > (enterDistance + 9.9999997473787516355514526367188e-05);
}

// 高层 2D 云使用高层云壳的球面交点，而不是无限水平平面。
// 这样地平线处不会因为 1 / ray.y 把一张平面纹理拉到无穷远，
// 摄像机进入云层后也不会因为“平面在身后”突然截断。
bool IntersectHighCloudLayerHspe(vec3 origin, vec3 direction,
                                 inout float enterDistance, inout float exitDistance)
{
    float baseAltitudeKm = clamp(cam.cloudHighParams0.w, 0.0,
                                 CLOUD_ATMO_TOP_ALTITUDE_KM - 0.01);
    float thicknessKm = min(max(cam.cloudHighParams1.x, 0.01),
                            max(CLOUD_ATMO_TOP_ALTITUDE_KM - baseAltitudeKm,
                                0.01));
    float baseRadius = 6371.0 + baseAltitudeKm;
    float topRadius = baseRadius + thicknessKm;
    float cameraRadius = length(origin);
    float baseNear;
    float baseFar;
    float topNear;
    float topFar;
    bool hitsBase = RaySphereInterval(origin, direction, baseRadius,
                                      baseNear, baseFar);
    bool hitsTop = RaySphereInterval(origin, direction, topRadius,
                                     topNear, topFar);
    if (!hitsTop)
    {
        return false;
    }

    float hitDistance = -1.0;
    if (cameraRadius < baseRadius)
    {
        // From below, the first cloud sample is the near side of the
        // spherical sheet: the positive intersection with the base shell.
        if (hitsBase && (baseFar > 0.0))
        {
            hitDistance = baseFar;
        }
    }
    else if (cameraRadius <= topRadius)
    {
        // While inside the high layer, choose the boundary in front of the
        // camera.  This keeps the layer continuous when crossing its height.
        if (dot(origin, direction) >= 0.0)
        {
            hitDistance = topFar;
        }
        else if (hitsBase && (baseNear >= 0.0))
        {
            // Looking toward the planet exits through the near side of the
            // inner shell.  Using baseFar here would place the sample behind
            // the planet and causes a hard cut when the camera enters the
            // cloud altitude.
            hitDistance = baseNear;
        }
        else
        {
            hitDistance = topFar;
        }
    }
    else if (topNear > 0.0)
    {
        // From above, use the first intersection with the outer shell.
        hitDistance = topNear;
    }

    if (hitDistance < 0.0)
    {
        return false;
    }
    enterDistance = hitDistance;
    // The high layer is a 2D optical sheet.  Keep a tiny geometric interval
    // only for front/back ordering; its optical depth is thickness-based.
    exitDistance = hitDistance + 0.001;
    return true;
}

bool IntersectHighCloudLayer(vec3 worldOrigin, vec3 direction,
                             inout float enterDistance, inout float exitDistance)
{
    return IntersectHighCloudLayerHspe(ToHspePosition(worldOrigin), direction,
                                       enterDistance, exitDistance);
}

float CloudLutSafeSqrt(float a)
{
    return sqrt(max(a, 0.0));
}

float CloudLutClampDistance(float d)
{
    return max(d, 0.0);
}

// cloud_view and the atmosphere LUT must use the same spherical atmosphere.
// The previous mobile source used a 6360/6420 km mapping while the LUT is
// generated for a 6371/6431 km Earth.  That mismatch was most visible at
// sunset, where the LUT lookup kept too much direct sunlight.
const float CLOUD_ATMO_BOTTOM_RADIUS_M = 6371000.0;
const float CLOUD_ATMO_TOP_RADIUS_M = 6431000.0;
// HSPE passes the solar irradiance directly into the normalized volume phase
// function.  The phase already contains 1/(4*pi); dividing this input by pi
// as if it were a Lambert BRDF makes the cloud direct light 3.14x too dark.
const vec3 CLOUD_SOLAR_IRRADIANCE =
    vec3(1.474000000000000,
         1.850399999999999,
         2.361200000000000);
// Keep the full cool moon spectrum. Daylight must not lose its blue component;
// night balance is controlled by source visibility, not by lowering blue.
const vec3 CLOUD_MOON_IRRADIANCE =
    vec3(0.058960000000000,
         0.074016000000000,
         0.188896000000000);
// Keep the moon dimmer than the sun, matching the reference cloud relighting
// path's single moon-darken factor rather than adding several light boosts.
const float CLOUD_MOON_VISUAL_SCALE = 0.350000000000000;
// CloudAtmosphereAmbient integrates the visible upper hemisphere only.  Use
// the corresponding hemispherical phase normalization for the MC-style sky
// environment term; using 1/(4*pi) here would discard half of that source.
const float CLOUD_INV_TWO_PI = 0.15915494309189533577;
const vec3 CLOUD_LUMINANCE =
    vec3(0.2125999927520752, 0.7152000069618225, 0.0722000002861023);

// Keep the PhiFwd transport constants separate from the user-facing strength
// controls.  The previous path reused cloudLightingParams.x as both the
// higher-order contribution and the PhiFwd intensity, so x=1 raised the Phi
// source to 10x the Vibroscat reference before any compression was applied.
const float CLOUD_PHI_OMEGA0 = 0.94;
const float CLOUD_PHI_INTENSITY = 0.10;
const float CLOUD_PHI_COMPRESSION_MAX = 2.0;
const float CLOUD_LIGHT_MAX_DISTANCE_KM = 2.0;
const int CLOUD_MS_OCTAVES = 3;
const float CLOUD_MS_ATTENUATION = 0.5;
const float CLOUD_MS_ECCENTRICITY = 0.5;
const float CLOUD_MS_DEPTH_POWER = 2.0;
const float CLOUD_MS_DEPTH_BIAS = -0.04;

// 只调整白天大气环境光进入云体时的蓝色通道；不改变云的 albedo、太阳
// 直射光或夜间月光。0.6 是当前云影可见度的蓝光保留比例。
const float CLOUD_DAY_SKY_BLUE_SCALE = 0.5;

// Use one solar visibility curve for direct sun, solar ambient, and the
// camera-to-cloud solar air-light term. This keeps daylight unchanged while
// removing residual solar light after the sun has actually set.
float CloudSunVisibility(float sunHeight)
{
    return smoothstep(-0.100000001490116,
                      0.0799999982118607,
                      sunHeight);
}

float CloudLutDistanceToTopAtmosphereBoundary(float r, float mu)
{
    float discriminant = ((r * r) * ((mu * mu) - 1.0))
                       + (CLOUD_ATMO_TOP_RADIUS_M * CLOUD_ATMO_TOP_RADIUS_M);
    float param = discriminant;
    float param_1 = ((-r) * mu) + CloudLutSafeSqrt(param);
    return CloudLutClampDistance(param_1);
}

float CloudLutTextureCoordFromUnitRange(float x, int _textureSize)
{
    return (0.5 / float(_textureSize)) + (x * (1.0 - (1.0 / float(_textureSize))));
}

vec2 CloudLutTransmittanceUv(float r, float mu)
{
    float H = sqrt((CLOUD_ATMO_TOP_RADIUS_M * CLOUD_ATMO_TOP_RADIUS_M)
                 - (CLOUD_ATMO_BOTTOM_RADIUS_M * CLOUD_ATMO_BOTTOM_RADIUS_M));
    r = clamp(r, CLOUD_ATMO_BOTTOM_RADIUS_M, CLOUD_ATMO_TOP_RADIUS_M);
    float param = (r * r)
                - (CLOUD_ATMO_BOTTOM_RADIUS_M * CLOUD_ATMO_BOTTOM_RADIUS_M);
    float rho = CloudLutSafeSqrt(param);
    float param_1 = r;
    float param_2 = mu;
    float d = CloudLutDistanceToTopAtmosphereBoundary(param_1, param_2);
    float d_min = CLOUD_ATMO_TOP_RADIUS_M - r;
    float d_max = rho + H;
    float x_mu = (d - d_min) / (d_max - d_min);
    float x_r = rho / H;
    float param_3 = x_mu;
    int param_4 = 256;
    float param_5 = x_r;
    int param_6 = 64;
    return vec2(CloudLutTextureCoordFromUnitRange(param_3, param_4),
                CloudLutTextureCoordFromUnitRange(param_5, param_6));
}

// The transmittance LUT stores the path from a point to the top of the
// atmosphere. For a camera ray that intersects the planet, the segment
// camera -> sample must be recovered through reciprocity (reverse both
// endpoints) instead of dividing the upward-ray entries directly.
// This is the same geometric distinction used by the MC reference cloud
// compositor, adapted to this engine's kilometre-space cloud coordinates.
bool CloudViewRayIntersectsGround(vec3 origin, vec3 direction)
{
    float originRadius = length(origin);
    float projected = dot(origin, direction);
    float planetRadiusSquared = 6371.0 * 6371.0;
    float discriminant = (projected * projected) + planetRadiusSquared -
                         (originRadius * originRadius);
    return (originRadius > 6371.0) && (projected < 0.0) &&
           (discriminant >= 0.0);
}

vec3 SampleSunDiskColor(vec3 sunDirection, vec3 hspePosition)
{
    // Use the cloud-layer position, not the camera height: sunlight reaches
    // a cloud from the top of the atmosphere at that local radius.
    float atmosphereRadius = max(length(hspePosition) * 1000.0,
                                 CLOUD_ATMO_BOTTOM_RADIUS_M);
    vec3 localUp = normalize(hspePosition);
    float param = atmosphereRadius;
    float param_1 = clamp(dot(localUp, sunDirection), -1.0, 1.0);
    vec3 sunTransmittance = texture(transmittanceLUT, CloudLutTransmittanceUv(param, param_1)).xyz;
    // This is incident solar irradiance for the cloud volume, not a surface
    // radiance term. Preserve the LUT's full RGB transmission so the cloud
    // receives the same spectral atmospheric filtering as the MC reference.
    return CLOUD_SOLAR_IRRADIANCE * max(sunTransmittance, vec3(0.0));
}

vec3 SampleMoonLightColor(vec3 moonDirection, vec3 hspePosition)
{
    float atmosphereRadius = max(length(hspePosition) * 1000.0,
                                 CLOUD_ATMO_BOTTOM_RADIUS_M);
    float param = atmosphereRadius;
    vec3 localUp = normalize(hspePosition);
    float param_1 = clamp(dot(localUp, moonDirection), -1.0, 1.0);
    vec3 moonTransmittance = texture(transmittanceLUT, CloudLutTransmittanceUv(param, param_1)).xyz;
    return CLOUD_MOON_IRRADIANCE * max(moonTransmittance, vec3(0.0));
}

vec3 CloudViewAtmosphereTransmittance(vec3 cameraPosition, vec3 samplePosition, vec3 rayDirection)
{
    // The transmittance LUT stores T(point -> top of atmosphere). For a ray
    // that misses the ground, T(camera -> sample) is the usual ratio of the
    // two upward paths. When the ray intersects the planet, the LUT entries
    // must be queried along the reciprocal upward directions; otherwise the
    // horizon and below-cloud path use the wrong atmospheric distance.
    float cameraPositionLength = length(cameraPosition);
    float samplePositionLength = length(samplePosition);
    float cameraRadius = clamp(cameraPositionLength * 1000.0,
                               CLOUD_ATMO_BOTTOM_RADIUS_M,
                               CLOUD_ATMO_TOP_RADIUS_M);
    float sampleRadius = clamp(samplePositionLength * 1000.0,
                                CLOUD_ATMO_BOTTOM_RADIUS_M,
                                CLOUD_ATMO_TOP_RADIUS_M);
    float cameraMu = clamp(dot(cameraPosition / max(cameraPositionLength,
                                                     0.000001),
                               rayDirection), -1.0, 1.0);
    float sampleMu = clamp(dot(samplePosition / max(samplePositionLength,
                                                    0.000001),
                              rayDirection), -1.0, 1.0);

    vec3 numerator;
    vec3 denominator;
    if (CloudViewRayIntersectsGround(cameraPosition, rayDirection))
    {
        // Reciprocity: T(camera -> sample) =
        // T(sample -> top, -mu_sample) / T(camera -> top, -mu_camera).
        numerator = texture(transmittanceLUT,
                            CloudLutTransmittanceUv(sampleRadius,
                                                    -sampleMu)).xyz;
        denominator = texture(transmittanceLUT,
                              CloudLutTransmittanceUv(cameraRadius,
                                                      -cameraMu)).xyz;
    }
    else
    {
        numerator = texture(transmittanceLUT,
                            CloudLutTransmittanceUv(cameraRadius,
                                                    cameraMu)).xyz;
        denominator = texture(transmittanceLUT,
                              CloudLutTransmittanceUv(sampleRadius,
                                                      sampleMu)).xyz;
    }
    return clamp(numerator / max(denominator,
                                 vec3(0.0010000000474974513)),
                 vec3(0.0), vec3(1.0));
}

vec3 CloudViewAtmosphereAttenuation(vec3 cameraPosition, vec3 samplePosition,
                                    vec3 rayDirection)
{
    vec3 transmittance = CloudViewAtmosphereTransmittance(
        cameraPosition, samplePosition, rayDirection);

    // Keep the complete RGB atmospheric transmittance.  The MC reference
    // applies the sky/air transmittance to cloud radiance spectrally; reducing
    // it to luminance here removes the blue-red separation from cloud shadows
    // and makes the whole layer look neutral gray.
    return max(transmittance, vec3(0.0));
}

vec3 CloudViewAtmosphereInscatter(vec3 cameraPosition, vec3 samplePosition,
                                  vec3 rayDirection, vec3 sourceDirection)
{
    // Restore the daytime atmospheric veil that makes cloud shadows belong to
    // the same blue sky. It follows the shared solar visibility curve so the
    // solar term cannot remain orange after sunset.
    vec3 cameraAtmospherePosition = cameraPosition * 1000.0;
    vec3 sampleAtmospherePosition = samplePosition * 1000.0;
    vec3 cameraTransmittance;
    vec3 sampleTransmittance;
    vec3 cameraRadiance = GetSkyRadiance(
        transmittanceLUT, scatteringLUT, cameraAtmospherePosition,
        normalize(rayDirection), 0.0, normalize(sourceDirection),
        cameraTransmittance);
    vec3 sampleRadiance = GetSkyRadiance(
        transmittanceLUT, scatteringLUT, sampleAtmospherePosition,
        normalize(rayDirection), 0.0, normalize(sourceDirection),
        sampleTransmittance);
    vec3 segmentTransmittance = CloudViewAtmosphereTransmittance(
        cameraPosition, samplePosition, rayDirection);
    float sunVisibility = CloudSunVisibility(normalize(sourceDirection).y);
    return max(cameraRadiance - (segmentTransmittance * sampleRadiance),
               vec3(0.0)) * sunVisibility;
}

// The cloud pass receives the same Bruneton scattering LUT as the sky pass.
// Sample a few directions in the local upper hemisphere and average them over
// the full sphere: this is the low-cost isotropic sky irradiance needed by the
// cloud multiple-scattering term.  The cloud density field is not involved.
vec3 CloudAtmosphereSkySample(vec3 atmospherePosition, vec3 viewDirection,
                              vec3 sourceDirection)
{
    vec3 transmittance;
    return max(GetSkyRadiance(transmittanceLUT, scatteringLUT,
                              atmospherePosition, normalize(viewDirection),
                              0.0, normalize(sourceDirection), transmittance),
               vec3(0.0));
}

vec3 CloudAtmosphereAmbient(vec3 hspePosition, vec3 sourceDirection)
{
    vec3 atmospherePosition = hspePosition * 1000.0;
    float radius = length(atmospherePosition);
    if (radius <= 9.9999997473787516355514526367188e-05)
    {
        return vec3(0.0);
    }

    vec3 up = atmospherePosition / radius;
    vec3 horizontalSource = sourceDirection - (up * dot(up, sourceDirection));
    float horizontalLengthSquared = dot(horizontalSource, horizontalSource);
    vec3 fallbackTangent = (abs(up.y) < 0.949999988079071044921875)
        ? normalize(cross(vec3(0.0, 1.0, 0.0), up))
        : normalize(cross(vec3(1.0, 0.0, 0.0), up));
    vec3 tangent = (horizontalLengthSquared > 9.9999997473787516355514526367188e-05)
        ? (horizontalSource / sqrt(horizontalLengthSquared))
        : fallbackTangent;

    vec3 zenith = CloudAtmosphereSkySample(atmospherePosition, up,
                                            sourceDirection);
    vec3 sourceSide = CloudAtmosphereSkySample(
        atmospherePosition, normalize(up + tangent), sourceDirection);
    vec3 antiSourceSide = CloudAtmosphereSkySample(
        atmospherePosition, normalize(up - tangent), sourceDirection);

    // Return an irradiance-like integral. CloudHspeAmbientColor converts it to
    // the hemispherical cloud environment term with albedo/(2*pi), so the
    // three samples cover the upper hemisphere with its 2*pi solid angle here.
    return (zenith + sourceSide + antiSourceSide) * 2.0943951023931954923;
}

// HSPE volumetricCloud2 keeps an ambient source in addition to direct cloud
// light.  Use atmospheric in-scattering for that source, matching the
// reference project's cloud relighting path; direct sun/moon terms continue to
// use the transmittance LUT in SampleSunDiskColor/SampleMoonLightColor.
vec3 CloudHspeAmbientColor(float sunWeight, float moonWeight,
                           vec3 cloudLightPosition, vec3 sunDirection)
{
    // Reduce only the daytime atmospheric environment's blue channel. At night,
    // add a separate neutral moon environment for the cloud body; the direct
    // moon source remains fully cool/blue and supplies the directional contour.
    vec3 sunAmbient = CloudAtmosphereAmbient(cloudLightPosition,
                                             sunDirection);
    vec3 daySkyAmbient = max(sunAmbient, vec3(0.0));
    daySkyAmbient.b *= CLOUD_DAY_SKY_BLUE_SCALE;
    vec3 moonAmbient = CloudAtmosphereAmbient(cloudLightPosition,
                                              -sunDirection) *
                       (CLOUD_MOON_IRRADIANCE /
                        max(CLOUD_SOLAR_IRRADIANCE,
                            vec3(0.0010000000474974513))) *
                       CLOUD_MOON_VISUAL_SCALE;
    vec3 neutralMoonAmbient = vec3(dot(max(moonAmbient, vec3(0.0)),
                                      CLOUD_LUMINANCE));
    // CloudAtmosphereAmbient is an upper-hemisphere irradiance estimate. Make
    // the unit conversion here once, before both the volumetric and high-sheet
    // cloud paths consume the source. This is the hemispherical environment
    // normalization used by the MC-style cloud relight path, not an exposure
    // multiplier.
    float singleScatteringAlbedo = clamp(cam.cloudParams1.w, 0.0, 1.0);
    vec3 environmentAmbient =
        (daySkyAmbient * clamp(sunWeight, 0.0, 1.0)) +
        (neutralMoonAmbient * clamp(moonWeight, 0.0, 1.0));
    vec3 atmosphereAmbient = environmentAmbient *
                             (singleScatteringAlbedo * CLOUD_INV_TWO_PI);
    return max(atmosphereAmbient, vec3(0.0));
}

// Nubis 的高层云是独立的 2D 滚动覆盖层。实际实现把“覆盖/云型场”和
// “三种云景形状”分开：覆盖图 RGB=Ci/Cs/Cc，A=shared coverage；
// CirrusLut RGB 是三种可平铺云景。这里保留同样的数据流，不再用一张
// 随机噪声图在 shader 内凭空生成规则 tile。
float CloudHighBaseAltitudeKm()
{
    // The atmosphere LUT is only defined up to 60 km.  Keep invalid editor
    // values from placing the high-cloud sheet outside that domain.
    return clamp(cam.cloudHighParams0.w, 0.0,
                 CLOUD_ATMO_TOP_ALTITUDE_KM - 0.01);
}

float CloudHighThicknessKm()
{
    float availableAltitude = max(CLOUD_ATMO_TOP_ALTITUDE_KM -
                                  CloudHighBaseAltitudeKm(), 0.01);
    return min(max(cam.cloudHighParams1.x, 0.01), availableAltitude);
}

float CloudHighScaleRatio()
{
    // The reference shader uses a fixed 1e-3-per-metre shape scale.  Older
    // scene data stored 0.0045 here, so keep that value as the 1.0 baseline;
    // values in the normal UI range (0.25..4.0) are direct relative scales.
    float scale = max(cam.cloudHighParams1.y, 0.00001);
    float ratio = (scale < 0.02) ? (scale / 0.0045) : scale;
    return clamp(ratio, 0.25, 4.0);
}

vec2 CloudHighWorldPosition(vec3 hspePosition)
{
    // hspePosition is already in planet-centred kilometres, so unlike the
    // original local-ray implementation it already contains cameraPosition.xz.
    // Subtracting the animated offset matches Nubis' rayPos -= windOffset.
    return hspePosition.xz - cam.cloudHighWindOffsetKm.xz;
}

vec4 CloudHighWeather(vec3 hspePosition)
{
    // CloudMapHigh in the reference is sampled with rcp(786e3).  hspePosition
    // is kilometres here, hence the equivalent 786 km tile.
    const float weatherTileKm = 786.0;
    float ratio = CloudHighScaleRatio();
    return texture(cloudHighMapTex,
                   CloudHighWorldPosition(hspePosition) /
                   (weatherTileKm / ratio));
}

vec3 CloudHighShapes(vec3 hspePosition, float localCoverage,
                     vec3 cloudType)
{
    // This is the Nubis/Revelation CloudHighDensity path in kilometre units:
    // curl domain offset, golden-angle rotation, two LUT frequencies, then a
    // per-channel mix controlled by the weather map's Ci/Cs/Cc fields.
    const float goldenAngle = 2.399963140000000;
    const mat2 goldenRotate = mat2(cos(goldenAngle), -sin(goldenAngle),
                                  sin(goldenAngle), cos(goldenAngle));
    vec2 rayPosition = CloudHighWorldPosition(hspePosition);
    vec2 curlNoise = texture(cloudMotionTex, rayPosition / 24.0).xy;
    vec2 position = rayPosition - (cam.cloudHighWindOffsetKm.xz * 0.5);
    position += curlNoise;
    position = goldenRotate * position;

    float ratio = CloudHighScaleRatio();
    vec2 lookupPos = (position * (0.05 * ratio)) -
                     vec2(localCoverage * 0.1);
    vec3 coarseShapes = texture(cloudHighTex, lookupPos * 2.0).xyz;
    coarseShapes *= coarseShapes;
    vec3 fineShapes = texture(cloudHighTex, lookupPos).xyz;
    return clamp(mix(coarseShapes, fineShapes,
                     clamp(cloudType, vec3(0.0), vec3(1.0))),
                 vec3(0.0), vec3(1.0));
}

float CloudHighMask(vec3 hspePosition)
{
    vec4 weather = CloudHighWeather(hspePosition);
    float localCoverage = clamp(weather.a, 0.0, 1.0);
    if (localCoverage < 0.001)
    {
        return 0.0;
    }

    float globalCoverage = clamp(cam.cloudHighParams0.y, 0.0, 1.0);
    // Nubis exposes one coverage control per cloud type.  This engine has a
    // single high-cloud control, so map it to the reference coverage range:
    // 0 -> 1.0 and 1 -> 1.5.  The old 1 + coverage mapping reached 2.0 at
    // the top of the UI and over-selected the Cc channel, leaving the
    // authored Ci cirrus almost invisible.
    float coverageScale = mix(1.0, 1.5, globalCoverage);
    float coverageCi = pow(clamp((weather.x * coverageScale) - 0.5,
                                 0.0, 1.0), 3.0);
    float coverageCs = pow(clamp((weather.y * coverageScale) - 0.5,
                                 0.0, 1.0), 3.0);
    float coverageCc = pow(clamp((weather.z * coverageScale) - 0.5,
                                 0.0, 1.0) * 2.0, 3.0);
    // CirrusLutRev is authored as three different cloud families.  Its
    // wispy Ci channel has intentionally lower integrated energy than the
    // compact Cc channel (which also has Nubis' *2 contrast lift).  Balance
    // those authored channels after coverage selection so enabling the high
    // layer does not silently turn it into a cirrocumulus-only layer.
    const vec3 cloudTypeEnergyCompensation = vec3(1.55, 1.0, 0.55);
    float activeCoverage = (coverageCi * cloudTypeEnergyCompensation.x) +
                           (coverageCs * cloudTypeEnergyCompensation.y) +
                           (coverageCc * cloudTypeEnergyCompensation.z);
    if (activeCoverage < 0.001)
    {
        return 0.0;
    }

    vec3 shapes = CloudHighShapes(hspePosition, localCoverage, weather.rgb);
    float density = (shapes.x * coverageCi * cloudTypeEnergyCompensation.x) +
                    (shapes.y * coverageCs * cloudTypeEnergyCompensation.y) +
                    (shapes.z * coverageCc * cloudTypeEnergyCompensation.z);
    // Shared coverage is the large-scale weather mask.  The reference uses
    // square(localCoverage) * 8.0 after selecting the three cloud types.
    return clamp(density * localCoverage * localCoverage * 8.0,
                 0.0, 1.0);
}

float CloudHighExtinction(vec3 hspePosition)
{
    return CloudHighMask(hspePosition) * max(cam.cloudHighParams0.z, 0.0);
}

// Wind deformation is deliberately detail-only.  The low-frequency 128^3
// field keeps its original rigid advection so its silhouette does not stretch;
// the bounded curl field is applied below to the high-frequency erosion
// coordinates.  Radiance and atmospheric colour are intentionally untouched.
const float CLOUD_WIND_CURL_TILE_KM = 24.0;
const float CLOUD_WIND_CURL_AMPLITUDE_KM = 0.65;

vec2 CloudWindDirectionXZ()
{
    vec2 direction = cam.cloudShapeParams.yz;
    float directionLength = length(direction);
    return (directionLength > 0.0001)
        ? (direction / directionLength)
        : vec2(1.0, 0.0);
}

vec2 CloudWindCurlOffset(vec2 relativePositionXZ, float heightFraction)
{
    // Sample the curl field in the moving weather domain.  The map is UNORM
    // encoded, so decode it around zero.  Rotate its local axes into the
    // current wind frame so changing wind direction does not make the flow
    // appear locked to world X/Z.
    vec2 curlSamplePosition = relativePositionXZ + cam.cloudWindOffsetKm.xz;
    vec2 curlField = texture(cloudMotionTex,
                              curlSamplePosition / CLOUD_WIND_CURL_TILE_KM).xy;
    curlField = (curlField * 2.0) - 1.0;
    vec2 windDirection = CloudWindDirectionXZ();
    vec2 crossWindDirection = vec2(-windDirection.y, windDirection.x);
    vec2 windAlignedCurl = (windDirection * curlField.x) +
                           (crossWindDirection * curlField.y);
    float lowerFlowWeight = 1.0 - smoothstep(0.0, 0.95,
                                              clamp(heightFraction, 0.0, 1.0));
    return windAlignedCurl * (CLOUD_WIND_CURL_AMPLITUDE_KM *
                              lowerFlowWeight);
}

vec3 CloudNubisPoint(vec3 position)
{
    float scale = max(cam.cloudParams1.y, 9.9999997473787516355514526367188e-06);
    vec3 relativePosition = position - vec3(0.0, CloudBaseAltitudeKm(), 0.0);
    relativePosition += cam.cloudNoiseOffsetKm.xyz;
    // Preserve rigid whole-layer advection for the base silhouette.  Do not
    // apply height shear or curl here: those belong to the detail field only.
    relativePosition.xz += cam.cloudWindOffsetKm.xz;
    return relativePosition * scale;
}

// These are deliberately small, shape-only controls.  They add the HFW/Nubis
// style of runtime up-rez detail without changing the cloud radiance path.
const float CLOUD_HFW_CURL_SAMPLE_SCALE = 0.35;
const float CLOUD_HFW_CURL_WARP = 0.18;
const float CLOUD_HFW_DETAIL_FREQUENCY = 4.0;
const float CLOUD_HFW_DETAIL_NEAR_KM = 18.0;
const float CLOUD_HFW_DETAIL_FAR_KM = 120.0;
// Reuse the existing 128^3 base field at a higher frequency for the
// dimensional-profile path.  This is deliberately a restrained medium-scale
// up-rez: it gives the silhouette a finer, lobe-like breakup without making
// the user-controlled erosion/detail layer mandatory or adding a new volume.
const float CLOUD_HFW_BASE_UPREZ_SCALE = 2.5;
const float CLOUD_HFW_BASE_UPREZ_BLEND = 0.32;
// Coverage remains a direct density/occupancy control: larger values retain
// more of the base field.  The profile is blended gently so it shapes the
// upper silhouette without turning the layer into horizontal bands.
const float CLOUD_HFW_PROFILE_BLEND = 0.28;

float NubisRemapClamped(float value, float originalMin, float originalMax, float newMin, float newMax)
{
    float t = newMin + (((value - originalMin) / (originalMax - originalMin)) * (newMax - newMin));
    return clamp(t, min(newMin, newMax), max(newMin, newMax));
}

float NubisRemapClampedBeforeAndAfter(float value, float originalMin, float originalMax, float newMin, float newMax)
{
    float param = clamp(value, originalMin, originalMax);
    float param_1 = originalMin;
    float param_2 = originalMax;
    float param_3 = newMin;
    float param_4 = newMax;
    return NubisRemapClamped(param, param_1, param_2, param_3, param_4);
}

// HFW/Nubis' dimensional profile is the product of a vertical profile and a
// coverage field.  We do not have the reference project's authored NDF type
// map here, so detailType is the existing procedural low-frequency type field.
// The three profiles preserve the same stratus -> stratocumulus -> cumulus
// progression while keeping the result in the normalized density domain.
float CloudHfwVerticalProfile(float heightFraction, float cloudType)
{
    float h = clamp(heightFraction, 0.0, 1.0);
    float type = clamp(cloudType, 0.0, 1.0);

    // A short common condensation ramp makes the cloud base read as a fairly
    // level floor.  The upper ramps are type-dependent, so the mass stays
    // broad below and tapers toward a narrower, billowy top.
    float bottomRamp = smoothstep(0.0, 0.08, h);
    float stratus = bottomRamp * (1.0 - smoothstep(0.84, 1.0, h));
    float stratocumulus = bottomRamp *
                          (1.0 - smoothstep(0.74, 1.0, h));
    float cumulus = bottomRamp *
                    (1.0 - smoothstep(0.58, 0.98, h));

    float stratusWeight = 1.0 - smoothstep(0.15, 0.50, type);
    float cumulusWeight = smoothstep(0.50, 0.85, type);
    float stratocumulusWeight = max(1.0 - stratusWeight - cumulusWeight, 0.0);
    return clamp((stratus * stratusWeight) +
                 (stratocumulus * stratocumulusWeight) +
                 (cumulus * cumulusWeight), 0.0, 1.0);
}

float SampleNubisBaseShape(vec3 position, float heightFraction, out float detailType)
{
    vec3 basePoint = CloudNubisPoint(position);
    vec4 lowFrequencyNoises = texture(cloudBaseShapeTex, basePoint);
    float lowFrequencyFBM = ((lowFrequencyNoises.y * 0.625) + (lowFrequencyNoises.z * 0.25)) + (lowFrequencyNoises.w * 0.125);
    lowFrequencyFBM = clamp(lowFrequencyFBM, 0.0, 1.0);
    // Reuse a low-frequency channel as the runtime equivalent of Nubis'
    // authored detail-type field: low values favour wispy detail, high values
    // favour billowy detail.  It is a field selection, not a colour gain.
    detailType = smoothstep(0.28, 0.72, lowFrequencyNoises.z);
    float cloudNoiseComposite = NubisRemapClamped(lowFrequencyNoises.x,
                                                   lowFrequencyFBM - 0.90,
                                                   1.0, 0.0, 1.0);

    // The reference uses a higher-frequency field to break up the upper edge.
    // Re-sampling the existing base volume keeps storage unchanged and is
    // independent of detailErosion/detailScale.  Keep this relief out of the
    // lower condensation zone so the cloud floor does not become noisy.
    float upperReliefWeight = smoothstep(0.12, 0.36,
                                         clamp(heightFraction, 0.0, 1.0));
    if (upperReliefWeight > 0.0001)
    {
        vec4 upRezNoises = texture(cloudBaseShapeTex,
                                   fract(basePoint * CLOUD_HFW_BASE_UPREZ_SCALE));
        float upRezFBM = clamp(((upRezNoises.y * 0.625) +
                                (upRezNoises.z * 0.25)) +
                               (upRezNoises.w * 0.125), 0.0, 1.0);
        float upRezComposite = NubisRemapClamped(upRezNoises.x,
                                                 upRezFBM - 0.90,
                                                 1.0, 0.0, 1.0);
        float edgeWeight = 1.0 - smoothstep(0.16, 0.72, cloudNoiseComposite);
        float edgeRelief = mix(0.70, 1.30, upRezComposite);
        cloudNoiseComposite = clamp(cloudNoiseComposite *
                                     mix(1.0, edgeRelief,
                                         edgeWeight *
                                         CLOUD_HFW_BASE_UPREZ_BLEND *
                                         upperReliefWeight),
                                     0.0, 1.0);
    }
    // Return the raw composite.  Coverage and the vertical profile are
    // combined later as a dimensional profile, matching the HFW density step.
    return cloudNoiseComposite;
}

float ErodeNubisCloud(vec3 position, float baseCloud, float heightFraction, float detailType, float detailBlend)
{
    float detailErosion = clamp(cam.cloudParams1.z, 0.0, 1.0);
    if ((baseCloud <= 0.0001) || (detailErosion <= 0.0001))
    {
        return clamp(baseCloud, 0.0, 1.0);
    }
    vec3 param = position;
    vec3 point = CloudNubisPoint(param);
    // The cloud volume is sampled in XZ.  The old XY lookup made the curl
    // field vary with altitude and produced a mostly one-sided smear.  The
    // motion texture is UNORM encoded, so decode it around zero before using
    // it as a domain warp.
    vec2 curlUv = fract(point.xz * CLOUD_HFW_CURL_SAMPLE_SCALE);
    vec2 curlField = texture(cloudMotionTex, curlUv).xy * 2.0 - 1.0;
    // cloudShapeParams.x is a relative frequency control.  Keep values below
    // one meaningful (they produce broader detail), then apply a fixed
    // up-rez factor so the existing 32^3 detail texture is not sampled at the
    // same broad period as the 128^3 base volume.  This coordinate is kept
    // independent from the erosion amount: changing detailScale must move the
    // high-frequency field even when the erosion control is held constant.
    float detailScale = max(cam.cloudShapeParams.x, 0.05);
    vec3 detailPoint = point * (detailScale * CLOUD_HFW_DETAIL_FREQUENCY);
    float shapeScale = max(cam.cloudParams1.y,
                           9.9999997473787516355514526367188e-06);
    vec2 relativePositionXZ = param.xz + cam.cloudNoiseOffsetKm.xz;
    // The detail origin already inherits the same rigid wind offset through
    // `point`.  Only add the bounded curl displacement here; using the total
    // elapsed wind distance as an extra detail offset would make the detail
    // drift away from the base and eventually create elongated strips.
    vec2 detailWindOffset = CloudWindCurlOffset(relativePositionXZ,
                                                heightFraction);
    // Convert the detail-only world-space flow back into the high-frequency
    // texture domain.  The base shape above remains unaffected by this term.
    detailPoint.xz += detailWindOffset *
                      (shapeScale * detailScale *
                       CLOUD_HFW_DETAIL_FREQUENCY);
    detailPoint.xz += curlField * (CLOUD_HFW_CURL_WARP * (1.0 - heightFraction));
    vec4 highFrequencyNoise = texture(cloudDetailsTex, detailPoint);
    // The reference erosion field is the weighted Worley FBM itself.  Do not
    // fold it a second time: a second fold destroys the ordering of the
    // Worley values and turns the signal into a broad threshold bias instead
    // of a usable edge-carving field.  The only inversion is height driven:
    // lower cloud layers retain billowy mass while the upper layer is carved
    // by the complementary high-frequency field.
    float highFrequencyFbm = clamp(
        (highFrequencyNoise.x * 0.625) +
        (highFrequencyNoise.y * 0.25) +
        (highFrequencyNoise.z * 0.125), 0.0, 1.0);
    float highFrequencyModifier = mix(
        highFrequencyFbm,
        1.0 - highFrequencyFbm,
        clamp(heightFraction * 10.0, 0.0, 1.0));

    // The dimensional profile has already carved the raw composite, and its
    // result is bounded by coverage. Normalize it before the HFW remap so the
    // user coverage value does not change the detail control's meaning.
    float coverage = clamp(cam.cloudParams0.y, 0.0, 1.0);
    float normalizedBaseCloud = (coverage > 0.0001) ? (baseCloud / coverage) : 0.0;
    normalizedBaseCloud = clamp(normalizedBaseCloud, 0.0, 1.0);

    // HFW's detail step is a remap of cloud_with_coverage from a noise-driven
    // lower bound to one.  It is an edge erosion, not a global density scale:
    // solid cores stay near one while only low-density boundary voxels are
    // removed.  Keep the maximum at 0.5, matching the reference's 0.5 factor,
    // and use a sqrt response so the UI remains useful below one as well.
    float erosionControl = sqrt(detailErosion);
    float detailThreshold = clamp(highFrequencyModifier *
                                  (0.50 * erosionControl), 0.0, 0.90);
    float erodedCloud = NubisRemapClamped(normalizedBaseCloud,
                                          detailThreshold, 1.0,
                                          0.0, 1.0);
    return erodedCloud * coverage;
}

// Keep the authored cloud field normalized, then convert it to the extinction
// domain exactly once.  This is the same separation used by the reference
// shader: "density = 1" is a field/control value, not sigma_t by itself.
const float CLOUD_EXTINCTION_PER_DENSITY = 39.0;

float CloudDensityHspeWithDetail(vec3 position, float detailBlend)
{
    float altitude = position.y;
    float baseAltitude = CloudBaseAltitudeKm();
    float thickness = CloudThicknessKm();
    float height = clamp((altitude - baseAltitude) / thickness, 0.0, 1.0);
    float detailType = 0.5;
    float cloudComposite = SampleNubisBaseShape(position, height, detailType);
    float coverage = clamp(cam.cloudParams0.y, 0.0, 0.98);
    // Direct HFW coverage semantics: at the middle of the layer the profile
    // is one, so this reduces to saturate(composite - (1 - coverage)).
    float broadCoverageShape = clamp(cloudComposite - (1.0 - coverage),
                                     0.0, 1.0);
    float verticalProfile = CloudHfwVerticalProfile(height, detailType);
    float dimensionalProfile = clamp(verticalProfile * coverage, 0.0, 1.0);
    // HFW/Nubis carves the low-frequency composite by the dimensional profile:
    // density = saturate(composite - (1 - profile)).  Apply it mainly above the
    // flat base; retaining the broad footprint below prevents the 3D mass from
    // collapsing into horizontal strips.
    float hfwProfileShape = clamp(cloudComposite - (1.0 - dimensionalProfile),
                                  0.0, 1.0);
    float profileInfluence = CLOUD_HFW_PROFILE_BLEND *
                              smoothstep(0.10, 0.38, height);
    float baseCloud = clamp(mix(broadCoverageShape,
                                hfwProfileShape,
                                profileInfluence) *
                      smoothstep(0.0, 0.08, height),
                            0.0, 1.0);
    float cloudShape = ErodeNubisCloud(position, baseCloud, height,
                                       detailType, detailBlend);
    float normalizedDensity = clamp(cloudShape, 0.0, 1.0);
    return (normalizedDensity * CLOUD_EXTINCTION_PER_DENSITY) *
           max(cam.cloudParams0.z, 0.0);
}

float CloudDensityHspe(vec3 position)
{
    // Light integration keeps the full density detail so cloud-to-cloud
    // attenuation remains consistent with the close-view silhouette.
    return CloudDensityHspeWithDetail(position, 1.0);
}

bool HspeNoHitRay(float sphereRadiusSquared, float pointRadiusSquared, float projected)
{
    bool _788 = (pointRadiusSquared > sphereRadiusSquared) && (projected < 0.0);
    bool _799;
    if (!_788)
    {
        _799 = ((projected * projected) + sphereRadiusSquared) < pointRadiusSquared;
    }
    else
    {
        _799 = _788;
    }
    return _799;
}

bool HspeLightRayClear(vec3 position, vec3 lightDirection)
{
    float radius = length(position);
    float projected = -dot(position, lightDirection);
    float param = 40589640.0;
    float param_1 = radius * radius;
    float param_2 = projected;
    return HspeNoHitRay(param, param_1, param_2);
}

// 低层云受高层 2D 云遮挡的廉价光照项：高层覆盖图沿光源方向只取
// 2D 层中点一次。它不增加一条完整的光线 march，却能让高层云真实地
// 给低层云投下柔和的日/月光阴影。
float CloudHighLightTransmittance(vec3 startPosition, vec3 lightDirection)
{
    if (cam.cloudHighParams0.x < 0.5)
    {
        return 1.0;
    }
    float enterDistance;
    float exitDistance;
    if (!IntersectHighCloudLayerHspe(startPosition, lightDirection,
                                     enterDistance, exitDistance))
    {
        return 1.0;
    }
    // A 2D cirrus sheet has an authored optical thickness.  Do not use the
    // geometric chord through the spherical shell here: it tends to infinity
    // at the horizon and was the source of the bright horizon band.
    float pathLength = CloudHighThicknessKm();
    vec3 samplePosition = startPosition +
                          (lightDirection * ((enterDistance + exitDistance) * 0.5));
    float opticalDepth = CloudHighExtinction(samplePosition) * pathLength;
    return exp(-min(opticalDepth, 80.0));
}

float CloudOpticalDepthHspe(vec3 startPosition, vec3 lightDirection)
{
    float layerLength = (CloudThicknessKm() * 0.999000012874603271484375) / 4.0;
    float baseAltitude = CloudBaseAltitudeKm();
    float topAltitude = CloudTopAltitudeKm();
    float opticalDepth = 0.0;
    vec3 position = startPosition;
    for (int i = 0; i < 5; i++)
    {
        position += (lightDirection * layerLength);
        float altitudeKm = length(position) - 6371.0;
        if ((altitudeKm > topAltitude) || (altitudeKm < baseAltitude))
        {
            break;
        }
        vec3 densityPosition = position;
        densityPosition.y = altitudeKm;
        vec3 param = densityPosition;
        opticalDepth += (CloudDensityHspe(param) * layerLength);
    }
    // cloudParams1.w is the single-scattering albedo.  Extinction is already
    // represented by CloudDensityHspe and must not be scaled by albedo.
    return opticalDepth;
}

float HspeSphereDistance(vec3 origin, vec3 direction, float radiusSquared)
{
    float originSquared = dot(origin, origin);
    float projected = -dot(direction, origin);
    float discriminant = ((projected * projected) + radiusSquared) - originSquared;
    if (((originSquared > radiusSquared) && (projected < 0.0)) || (discriminant < 0.0))
    {
        return -1.0;
    }
    float root = sqrt(max(discriminant, 0.0));
    float _731;
    if (radiusSquared > originSquared)
    {
        _731 = root;
    }
    else
    {
        _731 = -root;
    }
    return _731 + projected;
}

float CloudPhiFwd(vec3 startPosition, vec3 lightDirection, float localHeight)
{
    // Keep the engine's density/shape field untouched.  This pass only changes
    // how the existing density is transported along the light ray.
    if (!HspeLightRayClear(startPosition, lightDirection))
    {
        return 0.0;
    }

    float shellRadiusSquared = CloudTopRadiusKm() * CloudTopRadiusKm();
    float lightDistance = HspeSphereDistance(startPosition, lightDirection,
                                              shellRadiusSquared);
    // Match the reference light transport cap.  The old minimum of 8 km was
    // an engine-specific safety hack: with a 2 km cloud layer it integrated
    // distant source samples that did not belong to the local cloud column.
    float coverDistance = min(max(lightDistance, 0.0),
                              CLOUD_LIGHT_MAX_DISTANCE_KM);
    if (coverDistance <= 9.9999997473787516355514526367188e-05)
    {
        return 0.0;
    }

    // The light intervals are quadratically distributed.  The interval weight
    // is the corresponding Jacobian, so a constant-density ray still carries
    // the correct integrated optical depth while spending samples near the
    // receiver where source propagation changes fastest.
    const int lightStepCount = 4;
    float inverseStepCount = 1.0 / float(lightStepCount);
    float intervalScale = 2.0 * coverDistance * inverseStepCount;

    // PhiFwd has its own low-absorption transport albedo.  Reusing the
    // material/single-scattering albedo here made albedo=1 remove all
    // propagation loss (kappa=0), which is not the reference model.
    float oneMinusOmega0 = 1.0 - CLOUD_PHI_OMEGA0;
    float kappaPerOpticalDepth = sqrt(3.0 * oneMinusOmega0);
    float totalOpticalDepth = 0.0;
    float weightedSourceSum = 0.0;

    // This is the existing receiver boundary control, kept independent from
    // density and noise so it only gates the transported radiance source.
    float topNdotL = dot(vec3(0.0, 1.0, 0.0), lightDirection);
    float topBoundary = clamp((topNdotL + 0.5) / 1.5, 0.0, 1.0);
    float bottomHeight = max(localHeight + CLOUD_MS_DEPTH_BIAS, 0.0);
    float bottomBoundary = 1.0 - exp(-bottomHeight * CLOUD_MS_DEPTH_POWER);
    float boundaryConfidence = mix(1.0, topBoundary * bottomBoundary,
                                   clamp(cam.cloudLightingParams.z, 0.0, 1.0));

    for (int i = 0; i < lightStepCount; i++)
    {
        float midpointFraction = (float(i) + 0.5) * inverseStepCount;
        float distanceToSource = coverDistance * midpointFraction * midpointFraction;
        float intervalWeight = midpointFraction * intervalScale;
        vec3 sourcePosition = startPosition + (lightDirection * distanceToSource);
        float sourceAltitude = length(sourcePosition) - 6371.0;
        if (sourceAltitude < CloudBaseAltitudeKm() ||
            sourceAltitude > CloudTopAltitudeKm())
        {
            continue;
        }

        vec3 densityPosition = sourcePosition;
        densityPosition.y = sourceAltitude;

        // CloudDensityHspe returns the effective sigma_t used by both the view
        // and light marchers.  Keep Phi in that same extinction domain; dividing
        // by the conversion constant here would make light transport 39x thinner
        // than the camera ray and is not the MC-style density separation.
        float sourceDensity = CloudDensityHspe(densityPosition);
        float sigmaT = max(sourceDensity, 0.0);

        float segmentOpticalDepth = sigmaT * intervalWeight;
        float opticalDepthFromReceiver = totalOpticalDepth +
                                         (0.5 * segmentOpticalDepth);
        // cloudLightingParams.y is the raw PhiFwd build coefficient.  Keep the
        // reference equation direct: entering 0.15 means a 0.15 coefficient,
        // rather than applying another hidden 0.15 scale in the shader.
        float buildRate = clamp(cam.cloudLightingParams.y, 0.0, 1.0);
        float isotropicBuild = 1.0 - exp(-opticalDepthFromReceiver * buildRate);
        float sourceAbsorption = exp(-oneMinusOmega0 * totalOpticalDepth);
        float sourcePropagation = exp(-kappaPerOpticalDepth *
                                      opticalDepthFromReceiver);
        float scatteringSource = sigmaT * CLOUD_PHI_OMEGA0 * intervalWeight;
        float inverseDistance = 1.0 / max(distanceToSource,
                                          0.5 * intervalWeight);

        weightedSourceSum += sourceAbsorption * sourcePropagation *
                             scatteringSource * sigmaT * isotropicBuild *
                             inverseDistance * boundaryConfidence;
        totalOpticalDepth += segmentOpticalDepth;
    }

    // Isotropic radiance is normalized over the sphere before CloudHspeScattering
    // applies the user-controlled intensity/compression mapping.
    return max(weightedSourceSum * 0.0795774715, 0.0);
}

float CloudRemap(float value, float fromMin, float fromMax, float toMin, float toMax)
{
    return (((value - fromMin) / (fromMax - fromMin)) * (toMax - toMin)) + toMin;
}

float CloudPenumbra(float depth, inout float height, float viewLightCosine)
{
    float r = (viewLightCosine * 0.5) + 0.5;
    r *= r;
    height = (height * (1.0 - r)) + r;
    return depth * height;
}

vec4 CloudHspePowder(float density, float height, float VoL, float edgeDepth)
{
    float denlig = clamp(density * 0.2700000107288360595703125, 0.0, 1.39999997615814208984375);
    float param = height;
    float param_1 = 0.300000011920928955078125;
    float param_2 = 0.85000002384185791015625;
    float param_3 = 0.5;
    float param_4 = 2.0;
    float powderDepth = pow(denlig, max(CloudRemap(param, param_1, param_2, param_3, param_4), 0.0)) + 0.0500000007450580596923828125;
    float param_5 = height;
    float param_6 = 0.070000000298023223876953125;
    float param_7 = 0.2199999988079071044921875;
    float param_8 = (edgeDepth * 0.89999997615814208984375) + 0.100000001490116119384765625;
    float param_9 = 1.0;
    float powderView = pow(clamp(CloudRemap(param_5, param_6, param_7, param_8, param_9), 0.0, 1.0), 0.800000011920928955078125);
    float param_10 = powderDepth;
    float param_11 = powderView;
    float param_12 = VoL;
    float _1460 = CloudPenumbra(param_10, param_11, param_12);
    vec4 powder;
    powder.x = _1460;
    float param_13 = powderDepth;
    float param_14 = powderView;
    float param_15 = -VoL;
    float _1469 = CloudPenumbra(param_13, param_14, param_15);
    powder.y = _1469;
    float param_16 = height;
    float param_17 = 0.0;
    float param_18 = 0.07999999821186065673828125;
    float param_19 = 0.579999983310699462890625;
    float param_20 = 1.0;
    powder.z = mix(denlig, 1.0, 0.800000011920928955078125) * clamp(CloudRemap(param_16, param_17, param_18, param_19, param_20), 0.0, 1.0);
    powder.w = mix(denlig, 1.0, 0.20000000298023223876953125) * 0.25;
    return max(powder, vec4(0.0));
}

float CloudPhase3(float cosine, vec3 g3)
{
    vec3 numerator = vec3(1.0) - (g3 * g3);
    vec3 denominator = pow(max((vec3(1.0) + (g3 * g3)) - ((g3 * 2.0) * cosine), vec3(9.9999997473787516355514526367188e-05)), vec3(1.5)) * 12.56637096405029296875;
    // Preserve the HSPE forward/backward shape while keeping the phase mixture
    // energy-normalized (the old weights summed to 1.2).
    return dot(numerator / denominator, vec3(0.250000000000000,
                                             0.500000000000000,
                                             0.250000000000000));
}

vec4 CloudHspeScattering(float density, float stepLength, float height, float viewLightCosine, float sunOpticalDepth, float moonOpticalDepth, float viewTransmittance, float sunPhiFwd, float moonPhiFwd)
{
    float expod = exp2(((-density) * stepLength) * 1.44269502162933349609375);
    float edgeDepth = 1.0 - expod;
    float param = density;
    float param_1 = height;
    float param_2 = viewLightCosine;
    float param_3 = edgeDepth;
    vec4 powder = CloudHspePowder(param, param_1, param_2, param_3);
    vec4 scattering = vec4(0.0);
    float singleScatteringAlbedo = clamp(cam.cloudParams1.w, 0.0, 1.0);
    // x is one global multiple-scattering strength.  It scales the reference
    // three-octave higher-order contribution and the independently normalized
    // Phi intensity below; it is no longer a raw radiance multiplier.
    float multipleScatteringStrength = clamp(cam.cloudLightingParams.x,
                                             0.0, 1.0);
    vec3 g3 = vec3(-0.4000000059604644775390625, 0.5, 0.89999997615814208984375);
    float extK = 1.0;
    float scaK = 1.0;
    float orderAlbedo = singleScatteringAlbedo;
    for (int i = 0; i < CLOUD_MS_OCTAVES; i++)
    {
        // i=0 is the physical single-scattering term. The remaining phase
        // orders are internal multiple scattering and disappear when its
        // control is zero. Atmospheric sky light is integrated once at the
        // end of the view ray, using the same source as the reference shader.
        float orderWeight = (i == 0) ? 1.0 : multipleScatteringStrength;
        float t0 = (scaK * orderAlbedo * orderWeight * viewTransmittance) * edgeDepth;
        float sunLight = exp2(((-sunOpticalDepth) * extK) * 1.44269502162933349609375);
        float moonLight = exp2(((-moonOpticalDepth) * extK) * 1.44269502162933349609375);
        float param_4 = viewLightCosine;
        vec3 param_5 = g3;
        scattering.x += (((CloudPhase3(param_4, param_5) * sunLight) * t0) * powder.x);
        float param_6 = -viewLightCosine;
        vec3 param_7 = g3;
        scattering.y += (((CloudPhase3(param_6, param_7) * moonLight) * t0) * powder.y);
        g3 *= CLOUD_MS_ECCENTRICITY;
        extK *= CLOUD_MS_ATTENUATION;
        scaK *= CLOUD_MS_ATTENUATION;
        orderAlbedo *= singleScatteringAlbedo;
    }
    float phiIntensity = CLOUD_PHI_INTENSITY * multipleScatteringStrength;
    float phiCompress = clamp(cam.cloudLightingParams.w, 0.0,
                              CLOUD_PHI_COMPRESSION_MAX);
    float phiViewSource = (viewTransmittance * edgeDepth) * phiIntensity;
    float sunPhi = sunPhiFwd;
    float moonPhi = moonPhiFwd;
    if (phiCompress > 0.0)
    {
        sunPhi = (1.0 - exp((-sunPhi) * phiCompress)) / phiCompress;
        moonPhi = (1.0 - exp((-moonPhi) * phiCompress)) / phiCompress;
    }
    scattering.x += (sunPhi * phiViewSource);
    scattering.y += (moonPhi * phiViewSource);
    return max(scattering, vec4(0.0));
}

vec4 CloudHighScattering(vec3 hspeOrigin, vec3 rayDirection,
                         float enterDistance, float exitDistance,
                         vec3 sunDirection, vec3 sunLight, vec3 moonLight,
                         vec3 ambientColor)
{
    // Keep high clouds as a 2D optical sheet.  The geometric shell chord is
    // not the sheet thickness and becomes unstable toward the horizon.
    float pathLength = CloudHighThicknessKm();
    vec3 cloudPosition = hspeOrigin +
                         (rayDirection * ((enterDistance + exitDistance) * 0.5));
    float extinction = CloudHighExtinction(cloudPosition);
    float opticalDepth = min(extinction * pathLength, 80.0);
    float transmittance = exp(-opticalDepth);
    float alpha = 1.0 - transmittance;
    if (alpha <= 0.00001)
    {
        return vec4(0.0, 0.0, 0.0, 1.0);
    }

    vec3 up = normalize(cloudPosition);
    float sunAngle = max(dot(up, sunDirection), 0.08);
    float moonAngle = max(dot(up, -sunDirection), 0.08);
    float sunShadow = exp(-((extinction * CloudHighThicknessKm()) /
                            sunAngle) * 0.35);
    float moonShadow = exp(-((extinction * CloudHighThicknessKm()) /
                             moonAngle) * 0.35);
    float viewLightCosine = dot(rayDirection, sunDirection);
    float sunPhase = CloudPhase3(viewLightCosine,
                                 vec3(-0.250000, 0.350000, 0.750000));
    float moonPhase = CloudPhase3(-viewLightCosine,
                                  vec3(-0.250000, 0.350000, 0.750000));
    // sunLight/moonLight already include the atmospheric spectral
    // transmittance. Do not desaturate them before the cloud phase is applied.
    vec3 highSunLight = max(sunLight, vec3(0.0));
    vec3 highMoonLight = max(moonLight, vec3(0.0));
    // ambientColor is already the sky's atmospheric in-scattered radiance;
    // preserve its RGB chroma instead of applying the legacy 20% tint clamp.
    vec3 highAmbient = max(ambientColor, vec3(0.0));
    vec3 source = (highSunLight * sunPhase * sunShadow) +
                  (highMoonLight * moonPhase * moonShadow) +
                  highAmbient;
    source *= max(cam.cloudHighParams1.w, 0.0);
    vec3 atmosphereTransmittance = CloudViewAtmosphereAttenuation(
        hspeOrigin, cloudPosition, rayDirection);
    return vec4(max(source * alpha * atmosphereTransmittance, vec3(0.0)),
                transmittance);
}

vec3 HspeToWorldPosition(vec3 hspePosition)
{
    return (hspePosition - vec3(0.0, 6371.0, 0.0)) / vec3(0.001000000047497451305389404296875);
}

bool ReprojectCloudHistoryUV(vec3 hspeOrigin, vec3 rayDirection,
                             float enterDistance, float exitDistance, vec2 currentUV,
                             vec3 currentWindOffset, vec3 previousWindOffset,
                             inout vec2 historyUV)
{
    float historyDistance = mix(enterDistance, exitDistance, 0.5);
    vec3 historyHspePosition = hspeOrigin + (rayDirection * historyDistance);
    vec2 windOffsetDelta = currentWindOffset.xz - previousWindOffset.xz;
    historyHspePosition.xz += windOffsetDelta;
    vec3 param = historyHspePosition;
    vec3 historyWorldPosition = HspeToWorldPosition(param);
    vec4 previousClip = cam.cloudPrevViewProj * vec4(historyWorldPosition, 1.0);
    if (previousClip.w <= 9.9999997473787516355514526367188e-06)
    {
        return false;
    }
    historyUV = ((previousClip.xy / vec2(previousClip.w)) * 0.5) + vec2(0.5);
    bool _1196 = all(greaterThanEqual(historyUV, vec2(0.0)));
    bool _1203;
    if (_1196)
    {
        _1203 = all(lessThanEqual(historyUV, vec2(1.0)));
    }
    else
    {
        _1203 = _1196;
    }
    return _1203;
}

vec4 SampleCloudHistory(vec2 uv)
{
    return texture(cloudHistoryTex, uv);
}

vec4 ResolveCloudTemporal(vec4 currentCloud, vec3 hspeOrigin, vec3 rayDirection,
                          float enterDistance, float exitDistance, vec2 currentUV,
                          vec3 currentWindOffset, vec3 previousWindOffset)
{
    if (cam.cloudNoiseOffsetKm.w < 0.5)
    {
        return currentCloud;
    }
    vec3 param = hspeOrigin;
    vec3 param_1 = rayDirection;
    float param_2 = enterDistance;
    float param_3 = exitDistance;
    vec2 param_4 = currentUV;
    vec3 param_5 = currentWindOffset;
    vec3 param_6 = previousWindOffset;
    vec2 param_7;
    bool _1231 = ReprojectCloudHistoryUV(param, param_1, param_2, param_3,
                                          param_4, param_5, param_6,
                                          param_7);
    vec2 historyUV = param_7;
    if (!_1231)
    {
        return currentCloud;
    }
    vec2 param_8 = historyUV;
    vec4 historyCloud = SampleCloudHistory(param_8);
    float rawTransmittanceDifference = abs(historyCloud.w - currentCloud.w);
    float historyCloudAmount = max(max(historyCloud.x, max(historyCloud.y, historyCloud.z)), 1.0 - historyCloud.w);
    float currentCloudAmount = max(max(currentCloud.x, max(currentCloud.y, currentCloud.z)), 1.0 - currentCloud.w);
    bool historyIsEmpty = historyCloudAmount < 0.00200000009499490261077880859375;
    bool currentIsEmpty = currentCloudAmount < 0.00200000009499490261077880859375;
    vec3 historyMinRGB = max((currentCloud.xyz * 0.5) - vec3(0.014999999664723873138427734375), vec3(0.0));
    vec3 historyMaxRGB = (currentCloud.xyz * 1.5) + vec3(0.0500000007450580596923828125);
    vec4 _1298 = historyCloud;
    vec3 _1302 = clamp(_1298.xyz, historyMinRGB, historyMaxRGB);
    historyCloud.x = _1302.x;
    historyCloud.y = _1302.y;
    historyCloud.z = _1302.z;
    historyCloud.w = clamp(historyCloud.w, currentCloud.w - 0.25, currentCloud.w + 0.25);
    // The UV displacement is the result of the *valid* camera/cloud
    // reprojection above.  It is not a disocclusion signal: a fast camera pan
    // can move a perfectly valid cloud sample by many pixels.  Gating history
    // by that displacement made temporal accumulation disappear exactly while
    // the view was moving, leaving the STBN ray-step noise visible.
    //
    // Use the physical view-ray transmittance agreement instead.  Small
    // stochastic differences receive a long accumulation tail; large changes
    // at cloud edges or after a newly revealed region fall back quickly.
    float historyAgreement = 1.0 - smoothstep(
        0.039999999105930328369140625,
        0.3499999940395355224609375,
        rawTransmittanceDifference);
    float disocclusionWeight = 1.0 - smoothstep(
        0.449999988079071044921875,
        0.85000002384185791015625,
        rawTransmittanceDifference);
    float historyWeight = mix(0.75, 0.9700000286102294921875,
                              historyAgreement) * disocclusionWeight;
    if (historyIsEmpty != currentIsEmpty)
    {
        historyWeight = 0.0;
    }
    return mix(currentCloud, historyCloud, vec4(historyWeight));
}

void main()
{
    if (cam.cloudParams0.x < 0.5)
    {
        outCloud = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    vec2 uv = fragTexCoord;
    float sceneDepthValue = texture(sceneDepth, uv).x;
    if (sceneDepthValue < 0.99989998340606689453125)
    {
        outCloud = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    vec2 param = uv;
    vec3 viewRay = ReconstructViewRay(param);
    vec3 rayDirection = normalize(mat3(cam.invView[0].xyz, cam.invView[1].xyz, cam.invView[2].xyz) * viewRay);
    vec3 rayOrigin = cam.cameraPos.xyz;
    vec3 hspeOrigin = ToHspePosition(rayOrigin);
    float tEnter = 0.0;
    float tExit = 0.0;
    bool hasLowCloud = IntersectCloudLayer(rayOrigin, rayDirection,
                                           tEnter, tExit);
    if (hasLowCloud && ((tExit <= tEnter) || (tExit <= 0.0)))
    {
        hasLowCloud = false;
    }
    float highEnter = 0.0;
    float highExit = 0.0;
    bool hasHighCloud = (cam.cloudHighParams0.x >= 0.5) &&
                        IntersectHighCloudLayer(rayOrigin, rayDirection,
                                                highEnter, highExit);
    if (hasHighCloud && ((highExit <= highEnter) || (highExit <= 0.0)))
    {
        hasHighCloud = false;
    }
    if (!hasLowCloud && !hasHighCloud)
    {
        outCloud = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    vec3 sunDirection = pc.sunDir.xyz;
    float sunDirectionLengthSquared = dot(sunDirection, sunDirection);
    if (!(sunDirectionLengthSquared > 9.9999999392252902907785028219223e-09))
    {
        sunDirection = vec3(0.3499999940395355224609375, 0.800000011920928955078125, -0.449999988079071044921875);
    }
    else
    {
        sunDirection /= vec3(sqrt(sunDirectionLengthSquared));
    }
    float sunWeight = CloudSunVisibility(sunDirection.y);
    float moonWeight = 1.0 - smoothstep(-0.07999999821186065673828125, 0.0199999995529651641845703125, sunDirection.y);
    float lightDistance = 0.0;
    if (hasLowCloud && (!hasHighCloud || tEnter <= highEnter))
    {
        lightDistance = (tEnter + tExit) * 0.5;
    }
    else
    {
        lightDistance = (highEnter + highExit) * 0.5;
    }
    vec3 cloudLightPosition = hspeOrigin + (rayDirection * lightDistance);
    vec3 sunDiskColor = SampleSunDiskColor(sunDirection, cloudLightPosition);
    vec3 moonLightColor = SampleMoonLightColor(-sunDirection, cloudLightPosition);
    // The cloud volume receives atmospheric solar/moon irradiance directly.
    // Scene directional-light colour/intensity belongs to surface lighting and
    // must not multiply the atmospheric source a second time here.
    vec3 sunLight = sunDiskColor * sunWeight;
    vec3 moonLight = moonLightColor * (CLOUD_MOON_VISUAL_SCALE * moonWeight);
    vec3 ambientColor = CloudHspeAmbientColor(sunWeight, moonWeight,
                                               cloudLightPosition,
                                               sunDirection);
    float transmittance = 1.0;
    vec3 scattering = vec3(0.0);
    if (hasLowCloud)
    {
        int sampleCount = int(floor(mix(30.0, 15.0, abs(rayDirection.y))));
        sampleCount = clamp(sampleCount, 15, 30);
        float stepLength = (tExit - tEnter) / float(sampleCount);
        // The cloud layer is represented by one pixel, so use its midpoint as a
        // representative aerial-perspective distance.  This keeps the horizon
        // attenuation while avoiding two extra LUT fetches at every ray step.
        vec3 viewAtmosphereTransmittance = CloudViewAtmosphereAttenuation(
            hspeOrigin, hspeOrigin + (rayDirection * ((tEnter + tExit) * 0.5)),
            rayDirection);
        float highSunVisibility = 1.0;
        float highMoonVisibility = 1.0;
        vec3 lowCloudPosition = hspeOrigin +
                                (rayDirection * ((tEnter + tExit) * 0.5));
        if (hasHighCloud)
        {
            highSunVisibility = CloudHighLightTransmittance(lowCloudPosition,
                                                            sunDirection);
            highMoonVisibility = CloudHighLightTransmittance(lowCloudPosition,
                                                              -sunDirection);
        }
        vec2 stbnUv = (floor(gl_FragCoord.xy) + vec2(0.5)) / vec2(128.0);
        float stbnValue = texture(bluenoiseTex, stbnUv).z;
        float dither = clamp(fract(stbnValue + (pc.frameInfo.x * 0.61803400516510009765625)), 0.07999999821186065673828125, 0.920000016689300537109375);
        float sunDepth;
        float moonDepth;
        for (int i = 0; i < 30; i++)
        {
            if (i >= sampleCount)
            {
                break;
            }
            float distanceAlongRay = tEnter + ((float(i) + dither) * stepLength);
            vec3 samplePosition = hspeOrigin + (rayDirection * distanceAlongRay);
            float altitudeKm = length(samplePosition) - 6371.0;
            vec3 densityPosition = samplePosition;
            densityPosition.y = altitudeKm;
            // Reuse the detailed field nearby and converge to the compact
            // baseline at distance, matching the reference's continuous
            // up-rez/MIP strategy without introducing temporal popping.
            float viewDetailBlend = 1.0 - smoothstep(
                CLOUD_HFW_DETAIL_NEAR_KM, CLOUD_HFW_DETAIL_FAR_KM,
                max(distanceAlongRay, 0.0));
            float density = CloudDensityHspeWithDetail(densityPosition, viewDetailBlend);
            if (density <= 9.9999997473787516355514526367188e-05)
            {
                continue;
            }
            bool sunReachesSpace = HspeLightRayClear(samplePosition, sunDirection);
            bool moonReachesSpace = HspeLightRayClear(samplePosition, -sunDirection);
            sunDepth = sunReachesSpace
                ? CloudOpticalDepthHspe(samplePosition, sunDirection) : 114514.0;
            moonDepth = moonReachesSpace
                ? CloudOpticalDepthHspe(samplePosition, -sunDirection) : 114514.0;
            float altitude = clamp((altitudeKm - CloudBaseAltitudeKm()) /
                                   CloudThicknessKm(), 0.0, 1.0);
            float viewLightCosine = dot(rayDirection, sunDirection);
            float sunPhiFwd = 0.0;
            float moonPhiFwd = 0.0;
            if (cam.cloudLightingParams.x > 9.9999997473787516355514526367188e-05)
            {
                sunPhiFwd = CloudPhiFwd(samplePosition, sunDirection, altitude);
                moonPhiFwd = CloudPhiFwd(samplePosition, -sunDirection, altitude);
            }
            vec4 sampleScattering = CloudHspeScattering(
                density, stepLength, altitude, viewLightCosine, sunDepth,
                moonDepth, transmittance, sunPhiFwd, moonPhiFwd);
            // Direct sun/moon are carried by x/y.  Defer the atmospheric sky
            // source to one end-of-ray term below, matching the reference
            // RelightClouds path and keeping it independent of internal Phi
            // multiple-scattering orders.
            vec3 sampleLight =
                (sunLight * highSunVisibility * sampleScattering.x) +
                (moonLight * highMoonVisibility * sampleScattering.y);
            sampleLight *= viewAtmosphereTransmittance;
            scattering += sampleLight;
            transmittance *= exp2(((-density) * stepLength) *
                                  1.44269502162933349609375);
            if (transmittance < 0.00999999977648258209228515625)
            {
                break;
            }
        }
        // MC-style atmospheric sky illumination: the cloud receives the sky
        // radiance over the portion of the view ray absorbed by the cloud.  It
        // is an external atmospheric source, so it remains present when the
        // internal multiple-scattering control is zero.  Apply the camera-to-
        // cloud atmospheric transmittance to the same source before compositing.
        vec3 cloudSkyAmbient = ambientColor * max(1.0 - transmittance, 0.0) *
                                viewAtmosphereTransmittance;
        scattering += max(cloudSkyAmbient, vec3(0.0));
    }

    vec4 highCloud = vec4(0.0, 0.0, 0.0, 1.0);
    if (hasHighCloud)
    {
        highCloud = CloudHighScattering(hspeOrigin, rayDirection,
                                        highEnter, highExit, sunDirection,
                                        sunLight, moonLight, ambientColor);
    }
    vec3 combinedScattering = scattering;
    float combinedTransmittance = transmittance;
    if (hasHighCloud && hasLowCloud)
    {
        if (highEnter <= tEnter)
        {
            combinedScattering = highCloud.xyz + (highCloud.w * scattering);
        }
        else
        {
            combinedScattering = scattering + (transmittance * highCloud.xyz);
        }
        combinedTransmittance = transmittance * highCloud.w;
    }
    else if (hasHighCloud)
    {
        combinedScattering = highCloud.xyz;
        combinedTransmittance = highCloud.w;
    }
    // Keep the daytime atmospheric veil that integrates cloud shadows into
    // the blue sky, but turn this solar-only term off with the shared
    // visibility curve once night begins.
    vec3 atmosphereInscatter = CloudViewAtmosphereInscatter(
        hspeOrigin, cloudLightPosition, rayDirection, sunDirection);
    combinedScattering += atmosphereInscatter *
                          max(1.0 - combinedTransmittance, 0.0);
    // Keep alpha as the physical view-ray transmittance.  Horizon fading is
    // a presentation decision in gtao_apply; putting it into alpha makes a
    // cloud disappear from the sun-disc occlusion mask exactly at the
    // horizon.
    vec4 currentCloud = vec4(combinedScattering, combinedTransmittance);
    bool historyUsesHigh = hasHighCloud && (!hasLowCloud || highEnter < tEnter);
    float historyEnter = historyUsesHigh ? highEnter : tEnter;
    float historyExit = historyUsesHigh ? highExit : tExit;
    vec3 historyWindOffset = historyUsesHigh
        ? cam.cloudHighWindOffsetKm.xyz : cam.cloudWindOffsetKm.xyz;
    vec3 historyPreviousWindOffset = historyUsesHigh
        ? cam.cloudHighPrevWindOffsetKm.xyz : cam.cloudPrevWindOffsetKm.xyz;
    outCloud = ResolveCloudTemporal(currentCloud, hspeOrigin, rayDirection,
                                    historyEnter, historyExit, uv,
                                    historyWindOffset, historyPreviousWindOffset);
}
