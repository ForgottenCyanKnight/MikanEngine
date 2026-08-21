// Mikan Engine Vulkan Shaders - 着色器函数库
// 参考 Mikan Engine 的 shaderfunction.h，针对 Vulkan 优化

#ifndef MIKAN_SHADER_FUNCTIONS_H
#define MIKAN_SHADER_FUNCTIONS_H

// ============================================================================
// 光照计算函数（参考 Mikan Engine）
// ============================================================================

// Blinn-Phong 光照模型
vec3 CalculateBlinnPhong(vec3 albedo, vec3 normal, vec3 viewDir, vec3 lightDir, 
                         vec3 lightColor, float metallic, float roughness, float ao) {
    // 环境光
    vec3 ambient = 0.1 * albedo;
    
    // 漫反射
    float diff = max(dot(normal, lightDir), 0.0);
    vec3 diffuse = diff * lightColor;
    
    // 高光反射（Blinn-Phong）
    vec3 halfwayDir = normalize(lightDir + viewDir);
    float spec = pow(max(dot(normal, halfwayDir), 0.0), 32.0);
    vec3 specular = spec * lightColor;
    
    // 金属度混合
    vec3 diffuseColor = albedo * (1.0 - metallic);
    vec3 specularColor = mix(vec3(0.04), albedo, metallic);
    
    return ambient + (diffuseColor * diffuse + specularColor * specular) * ao;
}

// PBR 光照模型（简化版）
vec3 CalculatePBR(vec3 albedo, vec3 normal, vec3 viewDir, vec3 lightDir,
                  vec3 lightColor, float metallic, float roughness, float ao) {
    // 使用 Schlick-Fresnel 近似
    vec3 F0 = vec3(0.04);
    F0 = mix(F0, albedo, metallic);
    
    // 计算半程向量
    vec3 halfwayDir = normalize(lightDir + viewDir);
    
    // NDF - 法线分布函数（GGX）
    float NdotH = max(dot(normal, halfwayDir), 0.0);
    float NdotH2 = NdotH * NdotH;
    
    float roughness2 = roughness * roughness;
    float denom = (NdotH2 * (roughness2 - 1.0) + 1.0);
    float NDF = roughness2 / (3.14159 * denom * denom);
    
    // 几何遮挡函数（Schlick-GGX）
    float NdotV = max(dot(normal, viewDir), 0.0);
    float NdotL = max(dot(normal, lightDir), 0.0);
    
    float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
    float G_V = NdotV / (NdotV * (1.0 - k) + k);
    float G_L = NdotL / (NdotL * (1.0 - k) + k);
    float G = G_V * G_L;
    
    // Fresnel 项
    float VdotH = max(dot(viewDir, halfwayDir), 0.0);
    float Fc = pow(1.0 - VdotH, 5.0);
    vec3 F = F0 + (1.0 - F0) * Fc;
    
    // 组合 BRDF
    vec3 numerator = NDF * G * F;
    float denominator = 4.0 * NdotV * NdotL + 0.0001;
    vec3 brdf = numerator / denominator;
    
    // 能量守恒
    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
    
    // 最终光照
    vec3 Lo = kD * albedo / 3.14159 + brdf;
    return Lo * lightColor * NdotL * ao;
}

// ============================================================================
// 阴影采样函数（参考 Mikan Engine 的 CSM 阴影）
// ============================================================================

float SampleShadow(sampler2DShadow shadowMap, vec3 shadowCoord, float bias) {
    if (shadowCoord.z > 1.0 || shadowCoord.x < 0.0 || shadowCoord.x > 1.0 ||
        shadowCoord.y < 0.0 || shadowCoord.y > 1.0) {
        return 1.0;
    }
    
    return texture(shadowMap, shadowCoord).r;
}

// PCF 软阴影
float SampleShadowPCF(sampler2DShadow shadowMap, vec3 shadowCoord, float bias) {
    if (shadowCoord.z > 1.0) return 1.0;
    
    float texelSize = 1.0 / 1024.0; // 假设阴影贴图 1024x1024
    
    float visibility = 0.0;
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            vec3 offset = shadowCoord;
            offset.x += float(x) * texelSize;
            offset.y += float(y) * texelSize;
            offset.z -= bias;
            visibility += texture(shadowMap, offset).r;
        }
    }
    
    return visibility / 9.0;
}

// ============================================================================
// 环境光遮蔽函数
// ============================================================================

float CalculateSSAO(vec3 position, vec3 normal, sampler2D depthTexture, 
                    vec2 uv, float radius, float bias) {
    float currentDepth = texture(depthTexture, uv).r;
    float ssao = 0.0;
    
    // 简化版 SSAO - 实际应该使用噪声纹理和旋转矩阵
    for (int i = 0; i < 4; ++i) {
        float angle = float(i) * 3.14159 * 0.5;
        vec2 offset = vec2(cos(angle), sin(angle)) * radius;
        vec2 sampleUV = uv + offset;
        
        float sampleDepth = texture(depthTexture, sampleUV).r;
        float depthDiff = currentDepth - sampleDepth;
        
        if (depthDiff > bias) {
            ssao += 1.0;
        }
    }
    
    return 1.0 - (ssao / 4.0);
}

// ============================================================================
// 雾效函数（参考 Mikan Engine）
// ============================================================================

vec3 CalculateFog(vec3 color, vec3 viewPos, vec3 fogColor, float fogDensity, 
                  float fogGradient, float fogStart) {
    float distance = length(viewPos);
    float fogFactor = 1.0 / exp(pow((distance - fogStart) * fogDensity, fogGradient));
    fogFactor = clamp(fogFactor, 0.0, 1.0);
    
    return mix(fogColor, color, fogFactor);
}

// 高度雾
vec3 CalculateHeightFog(vec3 color, vec3 viewPos, vec3 fogColor, float fogDensity,
                        float fogHeight, float fogStart) {
    float distance = length(viewPos);
    float heightFactor = exp(-abs(viewPos.y) / fogHeight);
    float fogFactor = 1.0 - exp(-pow((distance - fogStart) * fogDensity * heightFactor, 2.0));
    fogFactor = clamp(fogFactor, 0.0, 1.0);
    
    return mix(fogColor, color, fogFactor);
}

// ============================================================================
// 法线贴图函数
// ============================================================================

vec3 CalculateNormalFromMap(vec3 normal, vec3 tangent, vec3 bitangent,
                            sampler2D normalMap, vec2 uv, float normalScale) {
    vec3 tangentNormal = texture(normalMap, uv).rgb * 2.0 - 1.0;
    tangentNormal.xy *= normalScale;
    
    mat3 TBN = mat3(normalize(tangent), normalize(bitangent), normalize(normal));
    return normalize(TBN * tangentNormal);
}

// ============================================================================
// 视差遮蔽贴图函数（高级效果）
// ============================================================================

vec2 CalculateParallaxOcclusion(vec2 uv, vec3 viewDir, sampler2D heightMap,
                                float minLayers, float maxLayers) {
    float numLayers = mix(maxLayers, minLayers, abs(dot(vec3(0.0, 0.0, 1.0), viewDir)));
    float layerDepth = 1.0 / numLayers;
    float currentLayerDepth = 0.0;
    vec2 P = viewDir.xy * 0.03; // 视差强度
    vec2 deltaTexCoords = P / numLayers;
    
    vec2 currentTexCoords = uv - deltaTexCoords;
    float currentDepthMapValue = texture(heightMap, currentTexCoords).r;
    
    while (currentLayerDepth < currentDepthMapValue) {
        currentTexCoords += deltaTexCoords;
        currentDepthMapValue = texture(heightMap, currentTexCoords).r;
        currentLayerDepth += layerDepth;
    }
    
    vec2 prevTexCoords = currentTexCoords - deltaTexCoords;
    float afterDepth = currentDepthMapValue - currentLayerDepth;
    float beforeDepth = texture(heightMap, prevTexCoords).r - currentLayerDepth + layerDepth;
    
    float weight = afterDepth / (afterDepth - beforeDepth);
    return prevTexCoords + weight * deltaTexCoords;
}

// ============================================================================
// 清屏函数（体积云/天空盒背景）
// ============================================================================

vec3 CalculateSkyColor(vec3 viewDir, vec3 sunDirection, vec3 skyColor, 
                       vec3 horizonColor, vec3 groundColor) {
    float y = viewDir.y;
    
    // 地平线混合
    vec3 color = mix(groundColor, horizonColor, smoothstep(-0.1, 0.1, y));
    color = mix(color, skyColor, smoothstep(0.0, 0.5, y));
    
    // 太阳辉光
    float sunIntensity = pow(max(dot(viewDir, sunDirection), 0.0), 256.0);
    color += vec3(1.0, 0.9, 0.7) * sunIntensity * 2.0;
    
    return color;
}

#endif // MIKAN_SHADER_FUNCTIONS_H
