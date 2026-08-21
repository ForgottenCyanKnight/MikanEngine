#version 450

precision highp float;
precision highp int;

layout(location = 0) in vec3 fragPosition;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec4 fragAlbedoColor;
layout(location = 3) in vec4 fragMaterialData;

layout(location = 0) out vec4 outColor;

const float PI = 3.14159265359;
const vec3 CAMERA_POS = vec3(2.0, 1.5, 2.0);

vec3 sRGBToLinear(vec3 srgb) {
    return pow(srgb, vec3(2.2));
}

vec3 LinearToSRGB(vec3 linear) {
    return pow(linear, vec3(1.0 / 2.2));
}

float DistributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    
    float num = a2;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;
    
    return num / denom;
}

float GeometrySchlickGGX(float NdotV, float roughness) {
    float r = (roughness + 1.0);
    float k = (r * r) / 8.0;
    
    float num = NdotV;
    float denom = NdotV * (1.0 - k) + k;
    
    return num / denom;
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx2 = GeometrySchlickGGX(NdotV, roughness);
    float ggx1 = GeometrySchlickGGX(NdotL, roughness);
    
    return ggx1 * ggx2;
}

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

void main() {
    float metallic = fragMaterialData.x;
    float roughness = fragMaterialData.y;
    
    vec3 albedo = sRGBToLinear(fragAlbedoColor.rgb);
    vec3 N = normalize(fragNormal);
    
    vec3 V = normalize(CAMERA_POS - fragPosition);
    
    vec3 F0 = vec3(0.04);
    F0 = mix(F0, albedo, metallic);
    
    vec3 Lo = vec3(0.0);
    
    // 主光源
    {
        vec3 lightDir = normalize(vec3(0.707, 0.707, 0.0));
        vec3 lightColor = vec3(3.0, 2.85, 2.7)*2.0;
        
        vec3 L = lightDir;
        vec3 H = normalize(V + L);
        
        float NdotL = max(dot(N, L), 0.0);
        
        float NDF = DistributionGGX(N, H, roughness);
        float G = GeometrySmith(N, V, L, roughness);
        vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);
        
        vec3 numerator = NDF * G * F;
        float denominator = 4.0 * max(dot(N, V), 0.0) * NdotL + 0.0001;
        vec3 specular = numerator / denominator;
        
        vec3 kS = F;
        vec3 kD = vec3(1.0) - kS;
        kD *= 1.0 - metallic;
        
        Lo += (kD * albedo / PI + specular) * lightColor * NdotL;
    }
    
    // 环境光
    vec3 ambient = vec3(0.4) * albedo;
    vec3 color = ambient + Lo;
    
    color = LinearToSRGB(color);
    
    outColor = vec4(color, 1.0);
}