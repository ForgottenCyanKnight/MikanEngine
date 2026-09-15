#version 450

// GPU procedural grid based on the Hazel/Fermion infinite-grid approach.
// The CPU submits one fullscreen triangle; all grid geometry, AA, depth and
// smooth fade are evaluated per fragment.
precision highp float;
precision highp int;

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

layout(std140, set = 0, binding = 0) uniform GridData
{
    mat4 u_ViewProjection;
    mat4 u_InverseViewProjection;
    vec4 u_CameraPosition;
    // x = grid spacing, y = radial fade distance, z = green Y-axis length.
    vec4 u_GridParams;
    // xy = render target width/height in pixels.
    vec4 u_Viewport;
    vec4 u_GridColorThin;
    vec4 u_GridColorThick;
    vec4 u_AxisColorX;
    vec4 u_AxisColorZ;
    vec4 u_AxisColorY;
} grid;

const float kEpsilon = 0.000001;

bool IsFinite(float value)
{
    return !isnan(value) && !isinf(value);
}

vec3 UnprojectPoint(vec2 ndc, float depth)
{
    vec4 point = grid.u_InverseViewProjection * vec4(ndc, depth, 1.0);
    if (abs(point.w) <= kEpsilon) {
        return grid.u_CameraPosition.xyz;
    }
    return point.xyz / point.w;
}

float SmoothStepSafe(float edge0, float edge1, float value)
{
    return smoothstep(edge0, max(edge1, edge0 + kEpsilon), value);
}

float PristineGridLine(vec2 uv)
{
    vec2 derivatives = max(fwidth(uv), vec2(kEpsilon));
    vec2 uvMod = fract(uv);
    vec2 distanceToLine = min(uvMod, 1.0 - uvMod);
    vec2 distanceInPixels = distanceToLine / derivatives;
    vec2 lineAlpha = 1.0 - smoothstep(0.0, 1.0, distanceInPixels);
    float alpha = max(lineAlpha.x, lineAlpha.y);
    float density = max(derivatives.x, derivatives.y);
    float densityFade = 1.0 - smoothstep(0.5, 1.0, density);
    return alpha * densityFade;
}

float AxisLineAA(float coordinate, float derivative)
{
    float distanceInPixels = abs(coordinate) / max(abs(derivative), kEpsilon);
    return 1.0 - smoothstep(0.0, 1.5,
                             distanceInPixels);
}

float RadialFadeFactor(vec3 worldPosition, vec3 cameraPosition,
                       float fadeDistance)
{
    vec3 cameraToPoint = worldPosition - cameraPosition;
    float distanceToPoint = length(cameraToPoint);
    if (distanceToPoint <= kEpsilon) {
        return 1.0;
    }

    // Fermion/Hazel-style radial fade prevents a finite-looking hard edge.
    return 1.0 - SmoothStepSafe(
        fadeDistance * 0.30, fadeDistance, distanceToPoint);
}

float GridFadeFactor(vec3 worldPosition, vec3 cameraPosition,
                     float fadeDistance)
{
    vec3 cameraToPoint = worldPosition - cameraPosition;
    float distanceToPoint = length(cameraToPoint);
    if (distanceToPoint <= kEpsilon) {
        return 1.0;
    }

    float radialFade = RadialFadeFactor(worldPosition, cameraPosition,
                                        fadeDistance);
    vec3 viewDirection = cameraToPoint / distanceToPoint;
    float normalFade = SmoothStepSafe(0.0, 0.15, abs(viewDirection.y));
    return radialFade * normalFade;
}

bool ProjectAxisPoint(vec3 worldPosition, out vec2 ndc, out float depth)
{
    vec4 clipPosition = grid.u_ViewProjection * vec4(worldPosition, 1.0);
    if (clipPosition.w <= kEpsilon) {
        return false;
    }
    ndc = clipPosition.xy / clipPosition.w;
    depth = clipPosition.z / clipPosition.w;
    return IsFinite(ndc.x) && IsFinite(ndc.y) && IsFinite(depth) &&
           depth >= 0.0 && depth <= 1.0;
}

void main()
{
    // fullscreen.vert maps the interpolated coordinate to the same NDC
    // orientation used by the scene camera (including the Vulkan Y flip).
    vec2 ndc = fragTexCoord * 2.0 - 1.0;
    vec3 cameraPosition = grid.u_CameraPosition.xyz;
    vec3 nearPoint = UnprojectPoint(ndc, 0.0);
    vec3 farPoint = UnprojectPoint(ndc, 1.0);
    vec3 ray = farPoint - nearPoint;
    float rayLength = length(ray);
    if (rayLength <= kEpsilon) {
        discard;
    }
    ray /= rayLength;

    float gridScale = max(grid.u_GridParams.x, 0.001);
    float fadeDistance = max(grid.u_GridParams.y, 1.0);
    const float axisDepthBias = 0.0005;

    // Keep ground-grid and axis coverage independent. The old implementation
    // discarded the whole fragment before reaching the Y-axis code whenever
    // the screen pixel did not intersect the XZ plane, so the vertical axis
    // could never appear above the horizon. The axes are editor overlays and
    // are evaluated even when the ground intersection is invalid.
    vec3 worldPosition = vec3(0.0);
    float gridDepth = 1.0;
    float fadeFactor = 0.0;
    vec3 finalColor = grid.u_GridColorThin.rgb;
    float finalAlpha = 0.0;
    float outputDepth = 1.0;

    // XZ ground plane (Y = 0), using the complete near/far camera ray in the
    // same way as the Hazel/Fermion infinite-grid shader.
    if (abs(ray.y) > kEpsilon) {
        float rayT = -nearPoint.y / ray.y;
        if (IsFinite(rayT) && rayT >= 0.0) {
            worldPosition = nearPoint + ray * rayT;
            vec4 gridClipPosition = grid.u_ViewProjection *
                                    vec4(worldPosition, 1.0);
            if (gridClipPosition.w > kEpsilon) {
                float candidateDepth = gridClipPosition.z /
                                       gridClipPosition.w;
                // The engine's scene depth uses the same Vulkan depth value
                // as this projection. Depth testing hides the grid behind
                // scene geometry.
                if (IsFinite(candidateDepth) && candidateDepth >= 0.0 &&
                    candidateDepth <= 1.0) {
                    gridDepth = candidateDepth;
                    outputDepth = gridDepth;
                    vec2 planePosition = worldPosition.xz;
                    fadeFactor = GridFadeFactor(worldPosition,
                                                cameraPosition,
                                                fadeDistance);

                    vec2 gridCoord1 = planePosition / gridScale;
                    vec2 gridCoord10 = planePosition / (gridScale * 10.0);
                    float grid1 = PristineGridLine(gridCoord1);
                    float grid10 = PristineGridLine(gridCoord10);
                    vec2 gridDerivative = fwidth(gridCoord1);
                    float lodFactor = smoothstep(
                        0.3, 0.6,
                        max(gridDerivative.x, gridDerivative.y));
                    float gridIntensity = mix(max(grid1, grid10 * 0.7),
                                              grid10, lodFactor);
                    finalColor = mix(grid.u_GridColorThin.rgb,
                                     grid.u_GridColorThick.rgb, lodFactor);
                    finalAlpha = mix(grid.u_GridColorThin.a,
                                     grid.u_GridColorThick.a, lodFactor) *
                                 gridIntensity * fadeFactor;

                    // X/Z axes on the ground plane. Axis colors follow the
                    // conventional editor convention: X = red, Z = blue.
                    vec2 worldDerivative = fwidth(planePosition);
                    float xAxisAlpha = clamp(
                        AxisLineAA(planePosition.y, worldDerivative.y) * fadeFactor *
                        max(grid.u_AxisColorX.a, 0.75), 0.0, 1.0);
                    float zAxisAlpha = clamp(
                        AxisLineAA(planePosition.x, worldDerivative.x) * fadeFactor *
                        max(grid.u_AxisColorZ.a, 0.75), 0.0, 1.0);
                    if (zAxisAlpha > 0.001) {
                        float zAxisBlend = clamp(zAxisAlpha, 0.0, 1.0);
                        finalColor = mix(finalColor, grid.u_AxisColorZ.rgb,
                                         zAxisBlend);
                        finalAlpha = max(finalAlpha, zAxisBlend);
                        // Keep coincident axes visible on a ground mesh whose
                        // top depth differs from the analytical plane by a
                        // small rasterization/rounding error.
                        outputDepth = min(outputDepth,
                                          max(0.0, gridDepth -
                                              axisDepthBias));
                    }
                    if (xAxisAlpha > 0.001) {
                        float xAxisBlend = clamp(xAxisAlpha, 0.0, 1.0);
                        finalColor = mix(finalColor, grid.u_AxisColorX.rgb,
                                         xAxisBlend);
                        finalAlpha = max(finalAlpha, xAxisBlend);
                        outputDepth = min(outputDepth,
                                          max(0.0, gridDepth -
                                              axisDepthBias));
                    }
                }
            }
        }
    }

    // The vertical Y axis is a short projected segment, so it can be tested
    // in screen space without creating CPU line geometry. Its depth is still
    // evaluated from the corresponding point on the world-space segment.
    float axisLength = max(grid.u_GridParams.z, 0.001);
    vec2 axisStartNdc;
    vec2 axisEndNdc;
    float axisStartDepth;
    float axisEndDepth;
    bool axisStartValid = ProjectAxisPoint(vec3(0.0), axisStartNdc,
                                           axisStartDepth);
    bool axisEndValid = ProjectAxisPoint(vec3(0.0, axisLength, 0.0),
                                         axisEndNdc, axisEndDepth);
    if (axisStartValid && axisEndValid) {
        vec2 axisVector = axisEndNdc - axisStartNdc;
        float axisLengthSquared = dot(axisVector, axisVector);
        if (axisLengthSquared > kEpsilon) {
            float axisT = clamp(dot(ndc - axisStartNdc, axisVector) /
                                    axisLengthSquared, 0.0, 1.0);
            vec2 closestNdc = axisStartNdc + axisVector * axisT;
            vec2 pixelDelta = (closestNdc - ndc) * 0.5 * grid.u_Viewport.xy;
            float yAxisAlpha = 1.0 - smoothstep(0.0, 1.5,
                                                length(pixelDelta));
            vec3 yAxisWorld = vec3(0.0, axisLength * axisT, 0.0);
            float yAxisFade = RadialFadeFactor(yAxisWorld, cameraPosition,
                                               fadeDistance);
            yAxisAlpha = clamp(yAxisAlpha * yAxisFade *
                               max(grid.u_AxisColorY.a, 0.75), 0.0, 1.0);
            float yAxisDepth = mix(axisStartDepth, axisEndDepth, axisT);
            if (yAxisAlpha > 0.001 && yAxisDepth >= 0.0 &&
                yAxisDepth <= 1.0) {
                float yAxisBlend = clamp(yAxisAlpha, 0.0, 1.0);
                finalColor = mix(finalColor, grid.u_AxisColorY.rgb,
                                 yAxisBlend);
                finalAlpha = max(finalAlpha, yAxisBlend);
                // The Y axis is an editor overlay. Keep it in front of the
                // analytical ground depth so the horizon cannot cut a gap
                // through the vertical axis. Depth writes remain disabled.
                outputDepth = 0.0;
            }
        }
    }

    if (!IsFinite(finalAlpha) || finalAlpha < 0.001) {
        discard;
    }

    gl_FragDepth = clamp(outputDepth, 0.0, 1.0);
    outColor = vec4(finalColor, finalAlpha);
}
