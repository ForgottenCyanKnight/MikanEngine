#version 450

// Terrain CSM caster vertex shader.
// The cascade matrix is a push constant because one command buffer records
// several cascades before the frame's host-visible UBO is consumed by the GPU.
// The terrain UBO remains the source for model/height parameters.
layout(set = 0, binding = 0) uniform TerrainUniformData {
    mat4 projView;
    mat4 prevProjView;
    mat4 model;
    mat4 prevModel;
    mat4 normalMatrix;
    vec4 heightParams;
    vec4 materialParams;
    vec4 cameraPosition;
    vec4 taaJitter;
} ubo;

layout(set = 0, binding = 1) uniform sampler2D uHeightmap;

layout(push_constant) uniform TerrainCsmPush {
    mat4 shadowProjView;
} csm;

layout(location = 0) in vec2 inPatchPosition;
layout(location = 1) in vec4 inChunkOriginSize;
layout(location = 2) in vec4 inChunkUvRect;
layout(location = 3) in vec4 inChunkParams;

float SampleHeight(vec2 uv) {
    ivec2 dimensions = max(textureSize(uHeightmap, 0), ivec2(1));
    vec2 sampleMax = vec2(max(dimensions - ivec2(1), ivec2(0)));
    vec2 texelUv = (clamp(uv, vec2(0.0), vec2(1.0)) * sampleMax + vec2(0.5)) /
                   vec2(dimensions);
    return texture(uHeightmap, texelUv).r;
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
    float chunkCount = max(inChunkUvRect.z, 1.0);
    vec2 globalTerrainUv = (inChunkUvRect.xy + patchPosition) / chunkCount;
    vec2 heightUv = clamp(globalTerrainUv, vec2(0.0), vec2(1.0));
    float localHeight = SampleHeight(heightUv) * ubo.heightParams.x + ubo.heightParams.y;
    vec2 terrainWorldSize = max(vec2(abs(ubo.heightParams.w),
                                     abs(ubo.materialParams.x)),
                                vec2(0.0001));
    vec2 localXZ = -0.5 * terrainWorldSize + globalTerrainUv * terrainWorldSize;
    vec4 worldPosition = ubo.model * vec4(localXZ.x, localHeight, localXZ.y, 1.0);
    gl_Position = csm.shadowProjView * worldPosition;
}
