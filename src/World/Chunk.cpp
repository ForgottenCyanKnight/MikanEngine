// Chunk.cpp
#include "World/Chunk.h"
#include "World/BlockManager.h"
#include <glm/gtc/noise.hpp>
#include <algorithm>
#include <cmath>

inline float sign(float x) {
    return (x > 0.0f) ? 1.0f : ((x < 0.0f) ? -1.0f : 0.0f);
}

static float HaltonSequence(int index, int base) {
    float f = 1.0f;
    float r = 0.0f;
    while (index > 0) {
        f /= base;
        r += f * (index % base);
        index = index / base;
    }
    return r;
}

Chunk::Chunk() {
    Clear();
}

void Chunk::Generate(int x, int z) {
    chunkX = x;
    chunkZ = z;
    Clear();

    TerrainType type = GetWorldConfig().terrainType;

    switch (type) {
    case FLAT: GenerateFlatTerrain(); break;
    case PERLIN: GeneratePerlinTerrain(); break;
    case ADVANCED: GenerateAdvancedTerrain(); break;
    }
}

// Chunk.cpp
void Chunk::InitializeNoiseGenerators() {
    // ��½���� - ��������Ⱥϵ�ֲ�
    continentNoise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    continentNoise.SetFrequency(0.001f);
    continentNoise.SetFractalType(FastNoiseLite::FractalType_FBm);
    continentNoise.SetFractalOctaves(3);

    // �������� - ����������״
    terrainNoise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    terrainNoise.SetFrequency(0.01f);
    terrainNoise.SetFractalType(FastNoiseLite::FractalType_FBm);
    terrainNoise.SetFractalOctaves(4);

    // ɽ������ - ����ɽ������
    ridgeNoise.SetNoiseType(FastNoiseLite::NoiseType_Cellular);
    ridgeNoise.SetFrequency(0.02f);
    ridgeNoise.SetCellularDistanceFunction(FastNoiseLite::CellularDistanceFunction_Hybrid);
    ridgeNoise.SetCellularReturnType(FastNoiseLite::CellularReturnType_Distance2Div);

    // ϸ������ - С�߶�ϸ��
    detailNoise.SetNoiseType(FastNoiseLite::NoiseType_Perlin);
    detailNoise.SetFrequency(0.1f);
    detailNoise.SetFractalType(FastNoiseLite::FractalType_FBm);
    detailNoise.SetFractalOctaves(2);

    treeNoise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    treeNoise.SetFrequency(0.15f);
    treeNoise.SetFractalType(FastNoiseLite::FractalType_FBm);
    treeNoise.SetFractalOctaves(3);

    // ������������
    waterNoise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    waterNoise.SetFrequency(0.015f); // ���ߵ�Ƶ�ʴ����ϸ�ĺ���
    waterNoise.SetFractalType(FastNoiseLite::FractalType_FBm);
    waterNoise.SetFractalOctaves(3);

    // ������������
    lakeNoise.SetNoiseType(FastNoiseLite::NoiseType_Cellular);
    lakeNoise.SetFrequency(0.01f);
    lakeNoise.SetCellularDistanceFunction(FastNoiseLite::CellularDistanceFunction_EuclideanSq);
    lakeNoise.SetCellularReturnType(FastNoiseLite::CellularReturnType_Distance);
}

Chunk::BiomeType Chunk::GetBiomeAt(int worldX, int worldZ, int terrainHeight) {
    const float continent = GetContinentNoise(worldX, worldZ);

    // 1. �����ж������ȳ���15��
    if (terrainHeight < SEA_LEVEL - 15) {
        return OCEAN;
    }

    // 2. �жϺ��� - ʹ��ר�ŵ�ˮ������
    float riverValue = waterNoise.GetNoise((float)worldX, (float)worldZ);
    riverValue = (riverValue + 1.0f) * 0.5f; // ת����[0,1]��Χ

    // ����������խ��״���ӽ���ƽ��߶�
    const float RIVER_WIDTH = 0.03f;
    if (fabs(riverValue - 0.5f) < RIVER_WIDTH &&
        abs(terrainHeight - SEA_LEVEL) <= 3) {
        return RIVER;
    }

    // 3. �жϺ��� - ʹ�ú�������
    float lakeValue = lakeNoise.GetNoise((float)worldX, (float)worldZ);
    lakeValue = (lakeValue + 1.0f) * 0.5f;

    // �����������ֲ��������򣬸߶Ƚӽ���ƽ��
    if (lakeValue < 0.35f &&
        terrainHeight >= SEA_LEVEL - 3 &&
        terrainHeight <= SEA_LEVEL + 1) {
        return LAKE;
    }

    // 4. ǳ�����򣨺�ƽ�����µ�δ�ﵽ���׼��
    if (terrainHeight < SEA_LEVEL) {
        return OCEAN;
    }

    // �޸�2������ƽԭ����Ⱥϵ��Χ
    const float plainsThreshold = 0.45f;  // �� 0.35f ���
    const float plainsBlend = 1.0f - glm::smoothstep(plainsThreshold, plainsThreshold + 0.1f, continent);

    // �޸�3������ɽ������Ƶ�ʣ������ֵ
    const float mountainThreshold = 0.75f;  // �� 0.65f ���
    const float mountainBlend = glm::smoothstep(mountainThreshold, mountainThreshold + 0.1f, continent);

    // �޸�4������̲��Χ
    if (terrainHeight >= SEA_LEVEL - 5 && terrainHeight <= SEA_LEVEL + 3) {  // ����Χ
        return BEACH;
    }

    // �����ж�ƽԭ
    if (plainsBlend > 0.7f) {
        return PLAINS;
    }

    // Ȼ���ж�ɽ��
    if (mountainBlend > 0.6f) {  // ����Ҫ��
        return MOUNTAINS;
    }
    /*
    // ����ж�ɳĮ
    const float desertBlend = glm::smoothstep(0.2f, 0.3f, continent);
    if (desertBlend > 0.8f) {
        return DESERT;
    }
    */
    // Ĭ�Ϸ���ƽԭ
    return PLAINS;
}
float Chunk::sigmoid(float x) {
    return 1.0f / (1.0f + exp(-x));
}

uint8_t Chunk::GetSurfaceBlock(BiomeType biome, int x, int y, int z) {
    switch (biome) {
    case OCEAN:
    case RIVER:
    case LAKE:
        return BlockManager::SAND;  // ˮ��ײ�Ϊɳ��
    case BEACH:
        return BlockManager::SAND;  // ��̲ɳ��
    case DESERT:
        return BlockManager::SAND;
    case PLAINS:
        return BlockManager::GRASSBLOCK;
    case SNOWY:
        return BlockManager::SNOW;
    default:
        return BlockManager::GRASSBLOCK;
    }
}

float Chunk::GetContinentNoise(int worldX, int worldZ) {
    return (continentNoise.GetNoise((float)worldX, (float)worldZ) + 1.0f) * 0.5f;
}

float Chunk::GetTerrainNoise(int worldX, int worldY, int worldZ) {
    return terrainNoise.GetNoise((float)worldX, (float)worldY, (float)worldZ);
}

float Chunk::GetRidgeNoise(int worldX, int worldY, int worldZ) {
    return (ridgeNoise.GetNoise((float)worldX, (float)worldY, (float)worldZ) + 1.0f) * 0.5f;
}

float Chunk::GetDetailNoise(int worldX, int worldY, int worldZ) {
    return detailNoise.GetNoise((float)worldX, (float)worldY, (float)worldZ) * 0.5f;
}

// Chunk.cpp �޸ĺ��GenerateAdvancedTerrain����
void Chunk::GenerateAdvancedTerrain() {
    InitializeNoiseGenerators();
    const int baseHeight = HEIGHT / 2;

    // ������������ʹ���θ�ƽ��
    continentNoise.SetFrequency(0.0003f);
    terrainNoise.SetFrequency(0.005f);
    ridgeNoise.SetFrequency(0.01f);
    ridgeNoise.SetFractalOctaves(5);
    // Ԥ���ɸ߶�ͼ������Ⱥϵ��ͼ
    int heightMap[SIZE][SIZE];
    BiomeType biomeMap[SIZE][SIZE]; // ��������Ⱥϵ��ͼ

    for (int lx = 0; lx < SIZE; lx++) {
        for (int lz = 0; lz < SIZE; lz++) {
            int worldX = chunkX + lx;
            int worldZ = chunkZ + lz;

            float continent = GetContinentNoise(worldX, worldZ);
            float baseTerrain = GetTerrainNoise(worldX, 0, worldZ);
            float ridges = GetRidgeNoise(worldX, 0, worldZ);

            // ��ƽ���ĵ��λ��
            float terrain = baseTerrain * 2.0f;
            terrain += pow(abs(ridges), 2.0f) * 0.3f * glm::smoothstep(0.3f, 0.7f, continent);

            // ����͵�sigmoid����
            terrain = sigmoid(terrain * 1.5f - 1.0f) * 2.0f - 1.0f;
            float height = (terrain + 1.0f) * 0.25f * 128.0f;

            // ��������ɽ������
            float mountain = glm::smoothstep(0.5f, 0.8f, continent);
            height += mountain * (baseTerrain * 0.5f + 0.5f) * 100.0f;
            biomeMap[lx][lz] = GetBiomeAt(worldX, worldZ, heightMap[lx][lz]);
            heightMap[lx][lz] = static_cast<int>(height);
        }
    }

    // �޸�ˮ�����ѭ����ȷ������ȷ�ĸ߶ȿ�ʼ
    for (int lx = 0; lx < SIZE; lx++) {
        for (int lz = 0; lz < SIZE; lz++) {
            int terrainHeight = 96 + heightMap[lx][lz];
            BiomeType biome = biomeMap[lx][lz];

            // ֻ����ˮ������Ⱥϵ
            if (biome == OCEAN || biome == RIVER || biome == LAKE) {
                int waterSurface = SEA_LEVEL + 3;
                if (biome == RIVER) waterSurface = SEA_LEVEL - 4;
                if (biome == LAKE) waterSurface = SEA_LEVEL - 5;

                // �ӵ��θ߶�+1��ʼ���ˮ��ȷ���Ӵ�����
                for (int y = terrainHeight + 1; y <= waterSurface; y++) {
                    if (y >= HEIGHT) continue;
                    int idx = index(lx, y, lz);
                    // ֻ���������򣬱��⸲�����з���
                    if (blocks[idx] == BlockManager::AIR) {
                        blocks[idx] = BlockManager::WATER;
                    }
                }

                // ���⴦����ȷ���Ӵ��ײ�û�п���
                for (int y = terrainHeight - 3; y <= terrainHeight; y++) {
                    if (y < 0 || y >= HEIGHT) continue;
                    int idx = index(lx, y, lz);
                    // ������ֿ�������ɳ����䣨�޸��Ӵ�©����
                    if (blocks[idx] == BlockManager::AIR) {
                        blocks[idx] = BlockManager::SAND;
                    }
                }
            }
        }
    }



    // ���ɷ��鲢�����¶�
    const float SLOPE_THRESHOLD = 2.0f;
    for (int lx = 0; lx < SIZE; lx++) {
        for (int lz = 0; lz < SIZE; lz++) {
            int worldX = chunkX + lx;
            int worldZ = chunkZ + lz;
            int terrainHeight = 96 + heightMap[lx][lz];
            BiomeType biome = GetBiomeAt(worldX, worldZ, terrainHeight);

            // �����¶ȣ�ʹ�����Ĳ�ַ���
            float dx = 0, dz = 0;
            if (lx > 0 && lx < SIZE - 1) dx = heightMap[lx + 1][lz] - heightMap[lx - 1][lz];
            if (lz > 0 && lz < SIZE - 1) dz = heightMap[lx][lz + 1] - heightMap[lx][lz - 1];
            float slope = sqrtf(dx * dx + dz * dz) / 2.0f;

            for (int y = 0; y < HEIGHT; y++) {
                int idx = index(lx, y, lz);

                // ���Ҳ㱣�ֲ���
                if (y < terrainHeight - 8) {
                    blocks[idx] = BlockManager::STONE;
                }
                // �м������¶Ⱥ�����Ⱥϵ����
                else if (y < terrainHeight - 1) {
                    bool isSteep = slope > SLOPE_THRESHOLD;

                    if (isSteep) {
                        // ����������¶��ʯ
                        blocks[idx] = BlockManager::STONE;
                    }
                    else {
                        switch (biome) {
                        case DESERT:
                            blocks[idx] = BlockManager::SAND;
                            break;
                        case MOUNTAINS:
                            // ����ԭ�иߺ�����ʯ���
                            blocks[idx] = (y > baseHeight + 20 && GetDetailNoise(worldX, y, worldZ) > 0.7f)
                                ? BlockManager::STONE
                                : BlockManager::DIRT;
                            break;
                        default:
                            blocks[idx] = BlockManager::DIRT;
                            break;
                        }
                    }
                }
                // �ر��㱣�ֲ���
                else if (y == terrainHeight - 1) {
                    if (biome == MOUNTAINS && terrainHeight > 240) {
                        blocks[idx] = BlockManager::SNOW;
                    }
                    else {
                        blocks[idx] = GetSurfaceBlock(biome, worldX, y, worldZ);
                    }
                }
                // �����㱣�ֲ���
                else {
                    blocks[idx] = BlockManager::AIR;
                }
            }
        }
    }

    for (int lx = 0; lx < SIZE; lx++) {
        for (int lz = 0; lz < SIZE; lz++) {
            int worldX = chunkX + lx;
            int worldZ = chunkZ + lz;
            int terrainHeight = 96 + heightMap[lx][lz];
            BiomeType biome = GetBiomeAt(worldX, worldZ, terrainHeight);

            // ȷ��ˮ��߶�
            int waterSurface = SEA_LEVEL + 3;

            // ֻ��������ˮ��߶ȵ�����
            if (terrainHeight < waterSurface) {
                for (int y = terrainHeight + 1; y <= waterSurface; y++) {
                    if (y >= HEIGHT) continue;

                    int idx = index(lx, y, lz);
                    // ֻ����������
                    if (blocks[idx] == BlockManager::AIR) {
                        blocks[idx] = BlockManager::WATER;
                    }
                }
            }
        }
    }


    for (int lx = 0; lx < SIZE; lx++) {
        for (int lz = 0; lz < SIZE; lz++) {
            int worldX = chunkX + lx;
            int worldZ = chunkZ + lz;

            // ��ȡ�ر��߶Ⱥ�����Ⱥϵ
            int terrainHeight = 96 + heightMap[lx][lz];
            BiomeType biome = GetBiomeAt(worldX, worldZ, terrainHeight);

            // ����ƽԭ����������ʱ������ľ
            if (biome == PLAINS && terrainHeight < HEIGHT - 10) {
                float treeValue = (treeNoise.GetNoise((float)worldX, (float)worldZ) + 1.0f) * 0.5f;

                // ʹ��Halton���б���ѵ�
                int sequenceIndex = abs(worldX * 7919 + worldZ * 65537);
                float haltonValue = HaltonSequence(sequenceIndex % 1024, 3);

                // ������ֵ����ԭ����ľ�ܶ�
                const float HALTON_THRESHOLD = 0.8f;
                if (treeValue > 0.82f && haltonValue > HALTON_THRESHOLD) {
                    GenerateTree(lx, lz, terrainHeight);
                }
            }
        }
    }


    const float PLANT_CHANCE = 0.2f;
    const float GRASS_RATIO = 0.1f;

    for (int lx = 0; lx < SIZE; lx++) {
        for (int lz = 0; lz < SIZE; lz++) {
            int terrainHeight = 96 + heightMap[lx][lz];
            int surfaceY = terrainHeight - 1;

            if (surfaceY < 0 || surfaceY >= HEIGHT) continue;

            // ֻ��鵱ǰ���������Ƿ�����Ч��Χ��
            if (lx < 0 || lx >= SIZE || lz < 0 || lz >= SIZE) continue;

            int idx = index(lx, surfaceY, lz);

            // ���ڲݷ���������
            if (blocks[idx] == BlockManager::GRASSBLOCK) {
                int plantY = surfaceY + 1;
                if (plantY >= HEIGHT) continue;

                int plantIdx = index(lx, plantY, lz);

                // �����Ϸ��ǿ���ʱ����
                if (blocks[plantIdx] != BlockManager::AIR) continue;

                // ʹ�ü���������������ж�
                float randomValue = static_cast<float>(rand()) / RAND_MAX; // [0,1]

                if (randomValue < PLANT_CHANCE) {
                    // �����������ֲ������
                    float typeRand = static_cast<float>(rand()) / RAND_MAX;
                    blocks[plantIdx] = (typeRand < GRASS_RATIO)
                        ? BlockManager::ROSE
                        : BlockManager::GRASS;
                }
            }
        }
    }

}

void Chunk::GenerateFlatTerrain() {
    int baseHeight = HEIGHT / 2;

    // һ����������������Ϊ����
    std::fill_n(blocks, SIZE * HEIGHT * SIZE, BlockManager::AIR);

    // �����Ҳ㣨��0��baseHeight-5��
    for (int y = 0; y < baseHeight - 5; y++) {
        for (int lx = 0; lx < SIZE; lx++) {
            for (int lz = 0; lz < SIZE; lz++) {
                blocks[index(lx, y, lz)] = BlockManager::STONE;
            }
        }
    }

    // ��������㣨baseHeight-5 �� baseHeight-1��
    for (int y = baseHeight - 5; y < baseHeight; y++) {
        for (int lx = 0; lx < SIZE; lx++) {
            for (int lz = 0; lz < SIZE; lz++) {
                blocks[index(lx, y, lz)] = BlockManager::DIRT;
            }
        }
    }

    // ���ݷ���㣨baseHeight��
    for (int lx = 0; lx < SIZE; lx++) {
        for (int lz = 0; lz < SIZE; lz++) {
            blocks[index(lx, baseHeight, lz)] = BlockManager::GRASSBLOCK;
        }
    }
}


// ʹ��FastNoiseLite���ɰ�����������
void Chunk::GeneratePerlinTerrain() {
    const int baseHeight = HEIGHT / 2;  // �����߶�

    for (int lx = 0; lx < SIZE; lx++) {
        for (int lz = 0; lz < SIZE; lz++) {
            int gx = chunkX + lx;
            int gz = chunkZ + lz;

            // ��ȡ����ֵ (-1.0 �� 1.0)
            float noise = noiseGen.GetNoise((float)gx, (float)gz);

            // ������ֵӳ�䵽�߶ȷ�Χ (0.0 - 1.0) Ȼ����������ʵĸ߶�
            float normalizedNoise = (noise + 1.0f) * 0.5f;  // ת����0-1��Χ
            int terrainHeight = static_cast<int>(normalizedNoise * 30) + baseHeight - 15;

            // ȷ���߶��ں�����Χ��
            terrainHeight = std::max(0, std::min(terrainHeight, HEIGHT - 1));

            for (int y = 0; y < HEIGHT; y++) {
                int idx = index(lx, y, lz);

                if (y < terrainHeight - 5) {
                    blocks[idx] = BlockManager::STONE;
                }
                else if (y < terrainHeight) {
                    blocks[idx] = BlockManager::DIRT;
                }
                else if (y == terrainHeight) {
                    blocks[idx] = BlockManager::GRASSBLOCK;
                }
                else {
                    blocks[idx] = BlockManager::AIR;
                }
            }
        }
    }
}


void Chunk::Clear() {
    std::fill_n(blocks, SIZE * HEIGHT * SIZE, 0);  // �����з����ʼ��Ϊ0(AIR)
    faceInstances.clear();  // �����ʵ������
    needsMeshUpdate = true;  // �����Ҫ��������
}

#include <glm/gtc/random.hpp>  // ���������������
// Chunk.cpp
void Chunk::GenerateTree(int lx, int lz, int baseHeight) {
    // �������ɲ���
    const int TRUNK_MIN_HEIGHT = 4;
    const int TRUNK_MAX_HEIGHT = 10;
    const int LEAF_RADIUS = 2;
    const float BRANCH_PROBABILITY = 0.4f; // ֦����ɸ���



    // ����������ɸ߶ȣ����Ӹ߶ȷ�Χ��
    float trunkNoise = GetDetailNoise(lx * 10, 0, lz * 10);
    int trunkHeight = TRUNK_MIN_HEIGHT + static_cast<int>((trunkNoise + 1.0f) * 0.5f * (TRUNK_MAX_HEIGHT - TRUNK_MIN_HEIGHT));
    int trunkTop = baseHeight + trunkHeight;

    // �������ɣ����Ӹ߶ȣ�
    for (int y = baseHeight; y <= trunkTop; y++) {
        if (y >= HEIGHT) break;
        int idx = index(lx, y, lz);
        if (blocks[idx] == BlockManager::AIR || blocks[idx] == BlockManager::LEAVES) {
            blocks[idx] = BlockManager::WOOD;
        }
    }



    // ������Ҷ�����߼�
    auto GenerateSphere = [&](int centerX, int centerY, int centerZ, int radius) {
        for (int dx = -radius; dx <= radius; dx++) {
            for (int dy = -radius; dy <= radius; dy++) {
                for (int dz = -radius; dz <= radius; dz++) {
                    if (dx * dx + dy * dy + dz * dz <= radius * radius) {
                        int x = centerX + dx;
                        int y = centerY + dy;
                        int z = centerZ + dz;

                        if (x >= 0 && x < SIZE && z >= 0 && z < SIZE && y >= 0 && y < HEIGHT) {
                            int idx = index(x, y, z);
                            if (blocks[idx] == BlockManager::AIR) {
                                blocks[idx] = BlockManager::LEAVES;
                            }
                        }
                    }
                }
            }
        }
        };

    // �����ڣ����壩
    GenerateSphere(lx, trunkTop - 1, lz, LEAF_RADIUS);

    // �������֦�
    if (trunkHeight > 7 && GetDetailNoise(lx, lz, 0) > 0.7f) {
        const int branchAttempts = 2 + static_cast<int>((GetDetailNoise(lx, 0, lz) + 1.0f) * 1.5f);

        for (int i = 0; i < branchAttempts; i++) {
            // ���ѡ��֦���ʼ�߶�
            int branchY = baseHeight + 3 + (i * 2);
            if (branchY >= trunkTop - 2) continue;

            // �������ʹ����������ȷ���Ե��������
            float dirNoise = GetDetailNoise(lx * i, 0, lz * i);
            glm::vec3 direction = glm::sphericalRand(1.0f);
            direction.y = abs(direction.y); // ������������

            // ֦���������
            const int branchLength = 2 + static_cast<int>((dirNoise + 1.0f) * 1.5f);
            glm::ivec3 currentPos(lx, branchY, lz);

            // ����֦�
            for (int step = 0; step < branchLength; step++) {
                currentPos += glm::ivec3(
                    static_cast<int>(round(direction.x)),
                    static_cast<int>(round(direction.y)),
                    static_cast<int>(round(direction.z))
                );

                // �߽���
                if (currentPos.x < 0 || currentPos.x >= SIZE ||
                    currentPos.z < 0 || currentPos.z >= SIZE ||
                    currentPos.y >= HEIGHT) break;

                // ����ľͷ
                int idx = index(currentPos.x, currentPos.y, currentPos.z);
                if (blocks[idx] == BlockManager::AIR || blocks[idx] == BlockManager::LEAVES) {
                    blocks[idx] = BlockManager::WOOD;
                }

                // ��֦�ĩ������С������
                if (step == branchLength - 1) {
                    GenerateSphere(currentPos.x, currentPos.y, currentPos.z, LEAF_RADIUS - 1);
                }

                // �����������
                direction += glm::sphericalRand(0.5f);
                direction = glm::normalize(direction);
            }
        }
    }

    // �������ڲ㣨��������У�
    GenerateSphere(lx, trunkTop - LEAF_RADIUS / 2, lz, LEAF_RADIUS + 1);
    if (trunkHeight > 8) {
        GenerateSphere(lx, trunkTop - LEAF_RADIUS - 1, lz, LEAF_RADIUS - 1);
    }
}