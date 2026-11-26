#pragma once

#include "World/Chunk.h"

#include <algorithm>
#include <cmath>

namespace ChunkTerrain {

struct Column {
    int surfaceY;  // Highest solid block.
    int waterY;    // Highest water block, or -1 for dry land.
    Chunk::BiomeType biome;
};

namespace Detail {

inline FastNoiseLite Fractal(int seed, float frequency, int octaves) {
    FastNoiseLite noise(seed);
    noise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    noise.SetFrequency(frequency);
    noise.SetFractalType(FastNoiseLite::FractalType_FBm);
    noise.SetFractalOctaves(octaves);
    return noise;
}

struct NoiseFields {
    FastNoiseLite continent = Fractal(1337, 0.00075f, 3);
    FastNoiseLite relief = Fractal(1337, 0.0042f, 4);
    FastNoiseLite detail = Fractal(9739, 0.024f, 2);
    FastNoiseLite warpX = Fractal(2939, 0.0032f, 2);
    FastNoiseLite warpZ = Fractal(5479, 0.0032f, 2);
    FastNoiseLite river{6727};
    FastNoiseLite lake{7193};

    NoiseFields() {
        river.SetFrequency(0.0026f);
        lake.SetNoiseType(FastNoiseLite::NoiseType_Cellular);
        lake.SetFrequency(0.0041f);
        lake.SetCellularDistanceFunction(FastNoiseLite::CellularDistanceFunction_EuclideanSq);
        lake.SetCellularReturnType(FastNoiseLite::CellularReturnType_Distance);
    }
};

inline float CarveValley(float land, float distance, float waterEdge, float depth, float shoulder, float bankSlope) {
    constexpr float waterLevel = Chunk::SEA_LEVEL + 3.0f;
    const float ratio = distance / waterEdge;
    const float profile = ratio < 1.0f
        ? waterLevel - 1.0f - depth * (1.0f - ratio * ratio)
        : waterLevel - 1.0f + bankSlope * (ratio - 1.0f);
    // Let high ground rise over a wider bank while preserving the same channel.
    const float extraShoulder = std::clamp((land - waterLevel - 12.0f) * 0.0023f, 0.0f, 0.24f);
    const float blend = glm::smoothstep(waterEdge, shoulder + extraShoulder, distance);
    return std::min(land, std::lerp(profile, land, blend));
}

} // namespace Detail

inline Column Sample(int worldX, int worldZ) {
    static const Detail::NoiseFields noise;
    constexpr int waterLevel = Chunk::SEA_LEVEL + 3;
    const float x = static_cast<float>(worldX);
    const float z = static_cast<float>(worldZ);

    const float continent = noise.continent.GetNoise(x, z);
    const float hills = noise.relief.GetNoise(x, z);
    const float detail = noise.detail.GetNoise(x, z);
    const float inland = glm::smoothstep(-0.50f, 0.50f, continent);
    const float highland = glm::smoothstep(0.03f, 0.62f, continent);
    const float uplift = 0.5f + 0.5f * hills;
    const float land = 103.0f + 40.0f * inland + hills * 29.0f
        + highland * (11.0f + uplift * uplift * 116.0f) + (3.0f + 4.0f * highland) * detail;

    // Use a continuous warped contour for the river axis, then carve a wider
    // valley around it. River, lake and coastal water meet at the same level.
    const float warpX = noise.warpX.GetNoise(x, z);
    const float warpZ = noise.warpZ.GetNoise(x, z);
    const float riverDist = std::abs(noise.river.GetNoise(x + 87.0f * warpX, z + 87.0f * warpZ));
    const float riverWidth = 0.074f + 0.034f * warpZ;
    const float riverBed = Detail::CarveValley(land, riverDist, riverWidth, 6.0f + 2.0f * warpX,
                                             0.44f, 4.5f);

    const float lakeDistanceSq = noise.lake.GetNoise(x + 67.0f * warpZ, z + 67.0f * warpX) + 1.0f;
    const float lakeDistance = std::sqrt(std::max(0.0f, lakeDistanceSq));
    const float lakeRadius = 0.180f + 0.068f * warpX;
    const float lakeBed = Detail::CarveValley(land, lakeDistance, lakeRadius, 11.0f + 5.0f * warpZ,
                                            lakeRadius + 0.30f, 11.0f);

    const int surface = std::clamp(static_cast<int>(std::floor(std::min(riverBed, lakeBed))),
                                   1, Chunk::HEIGHT - 2);
    if (surface < waterLevel) {
        const Chunk::BiomeType biome = land < static_cast<float>(waterLevel)
            ? Chunk::OCEAN : (lakeBed < riverBed ? Chunk::LAKE : Chunk::RIVER);
        return {surface, waterLevel, biome};
    }
    if (surface <= waterLevel + 2) return {surface, -1, Chunk::BEACH};
    if (surface >= 199 + static_cast<int>(10.0f * warpZ)) return {surface, -1, Chunk::SNOWY};
    return {surface, -1, highland > 0.40f ? Chunk::MOUNTAINS : Chunk::PLAINS};
}

} // namespace ChunkTerrain
