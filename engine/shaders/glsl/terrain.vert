#version 450

layout(set = 0, binding = 0) uniform TerrainUniformData {
    mat4 projView;
    mat4 prevProjView;
    mat4 model;
    mat4 prevModel;
    mat4 normalMatrix;
    vec4 heightParams;   // heightScale, heightOffset, materialTiling, worldSizeX
    vec4 materialParams; // worldSizeZ, useControlMap, blendSharpness, reserved
    vec4 cameraPosition;
    vec4 taaJitter;
} ubo;

layout(set = 0, binding = 1) uniform sampler2D uHeightmap;

layout(location = 0) in vec2 inPatchPosition;
layout(location = 1) in vec4 inChunkOriginSize;
layout(location = 2) in vec4 inChunkUvRect;
layout(location = 3) in vec4 inChunkParams;

layout(location = 0) out vec3 outWorldPosition;
layout(location = 1) out vec3 outWorldNormal;
layout(location = 2) out vec2 outMaterialUv;
layout(location = 3) out vec2 outHeightUv;
layout(location = 4) out vec2 outTerrainFactors; // normalized height, slope
layout(location = 5) out vec2 outMotionVector;

float SampleHeight(vec2 uv) {
    return texture(uHeightmap, clamp(uv, vec2(0.0), vec2(1.0))).r;
}

float SnapEdgeCoordinate(float coordinate, float intervals, uint lodDelta) {
    if (lodDelta == 0u) {
        return coordinate;
    }
    float coarseStep = exp2(float(lodDelta));
    float fineIndex = coordinate * intervals;
    float snappedIndex = floor((fineIndex + 0.01) / coarseStep) * coarseStep;
    return clamp(snappedIndex / intervals, 0.0, 1.0);
}

vec2 StitchPatchEdges(vec2 position) {
    uint packedDeltas = uint(inChunkParams.y + 0.5);
    float intervals = max(inChunkParams.z, 1.0);
    const float edgeEpsilon = 0.00001;

    // 2 bit/edge: -Z, +X, +Z, -X。只折叠细网格的边界顶点，
    // 所有三角形仍位于高度场上表面，不再需要向地下延伸的 skirt。
    if (position.y <= edgeEpsilon) {
        position.x = SnapEdgeCoordinate(position.x, intervals, packedDeltas & 3u);
    }
    if (position.x >= 1.0 - edgeEpsilon) {
        position.y = SnapEdgeCoordinate(position.y, intervals, (packedDeltas >> 2u) & 3u);
    }
    if (position.y >= 1.0 - edgeEpsilon) {
        position.x = SnapEdgeCoordinate(position.x, intervals, (packedDeltas >> 4u) & 3u);
    }
    if (position.x <= edgeEpsilon) {
        position.y = SnapEdgeCoordinate(position.y, intervals, (packedDeltas >> 6u) & 3u);
    }
    return position;
}

void main() {
    vec2 patchPosition = StitchPatchEdges(inPatchPosition);
    vec2 patchUv = patchPosition;

    vec2 heightUv = clamp(inChunkUvRect.xy + patchUv * inChunkUvRect.zw,
                          vec2(0.0), vec2(1.0));
    ivec2 textureDimensions = textureSize(uHeightmap, 0);
    vec2 texelUv = 1.0 / vec2(max(textureDimensions, ivec2(1)));

    float normalizedHeight = SampleHeight(heightUv);
    float localHeight = normalizedHeight * ubo.heightParams.x + ubo.heightParams.y;
    float hL = SampleHeight(heightUv - vec2(texelUv.x, 0.0));
    float hR = SampleHeight(heightUv + vec2(texelUv.x, 0.0));
    float hD = SampleHeight(heightUv - vec2(0.0, texelUv.y));
    float hU = SampleHeight(heightUv + vec2(0.0, texelUv.y));

    float texelWorldX = max(abs(ubo.heightParams.w) / max(float(textureDimensions.x - 1), 1.0), 0.0001);
    float texelWorldZ = max(abs(ubo.materialParams.x) / max(float(textureDimensions.y - 1), 1.0), 0.0001);
    float dHdX = (hR - hL) * ubo.heightParams.x / (2.0 * texelWorldX);
    float dHdZ = (hU - hD) * ubo.heightParams.x / (2.0 * texelWorldZ);
    vec3 localNormal = normalize(vec3(-dHdX, 1.0, -dHdZ));

    vec2 localXZ = inChunkOriginSize.xy + patchPosition * inChunkOriginSize.zw;
    vec3 localPosition = vec3(localXZ.x, localHeight, localXZ.y);
    vec4 worldPosition = ubo.model * vec4(localPosition, 1.0);
    vec4 clipPosition = ubo.projView * worldPosition;
    clipPosition.xy += ubo.taaJitter.xy * clipPosition.w;
    gl_Position = clipPosition;

    vec4 previousWorldPosition = ubo.prevModel * vec4(localPosition, 1.0);
    vec4 previousClipPosition = ubo.prevProjView * previousWorldPosition;
    vec2 currentNdc = clipPosition.xy / max(abs(clipPosition.w), 0.000001);
    vec2 previousNdc = previousClipPosition.xy / max(abs(previousClipPosition.w), 0.000001);

    outWorldPosition = worldPosition.xyz;
    outWorldNormal = normalize(mat3(ubo.normalMatrix) * localNormal);

    // 材质 UV 使用地形局部世界坐标计算，而不是使用每个 chunk 的
    // uvRect。这样区块边界两侧共享同一套连续坐标，LOD stitching
    // 折叠边界顶点时也不会把材质采样截断在 chunk 内。
    vec2 terrainWorldSize = max(vec2(abs(ubo.heightParams.w),
                                     abs(ubo.materialParams.x)),
                                vec2(0.0001));
    vec2 globalTerrainUv = localXZ / terrainWorldSize + vec2(0.5);
    outMaterialUv = globalTerrainUv * ubo.heightParams.z;
    outHeightUv = heightUv;
    outTerrainFactors = vec2(normalizedHeight, clamp(1.0 - localNormal.y, 0.0, 1.0));
    outMotionVector = (currentNdc - previousNdc) * 0.5;
}
