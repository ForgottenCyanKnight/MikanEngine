#version 450

// 为 Adreno GPU 强制使用高精度
precision highp float;
precision highp int;

layout(location = 0) in vec3 fragPosition;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in highp vec2 fragTexCoord;
layout(location = 3) in vec3 fragTangent;
layout(location = 4) in vec3 fragBitangent;
    // Material data starts at the offset used by ModelRenderer's push-constant layout.
    layout(push_constant) uniform PC_Material {
    layout(offset = 160) vec4 subMeshMaterial;   // x=metallic y=roughness z=ao（-1=未设）w=mrValid（MR 纹理有效性）
    layout(offset = 176) vec4 subMeshAlpha;
};
layout(location = 5) in vec4 fragAlbedoColor;
layout(location = 6) in vec4 fragMaterialData;
layout(location = 7) in vec4 fragTextureFlags;
layout(location = 8) in vec2 fragMotionVector;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outMaterial;
layout(location = 3) out vec2 outMotionVector; // 运动=附件3（location 3，TAA 预留）

layout(binding = 0) uniform sampler2D albedoTexture;
layout(binding = 1) uniform sampler2D normalTexture;
layout(binding = 2) uniform sampler2D roughnessTexture;
layout(binding = 3) uniform sampler2D metallicTexture;
layout(binding = 5) uniform sampler2D emissiveTexture;

// 折叠时必须使用 sign-not-zero：GLSL sign(0)=0 会让 -Z 极点与 +Z 极点
// 都编码成 (0,0)，使合成阶段无法恢复法线朝向。
vec2 SignNotZero(vec2 v) {
    return vec2(v.x < 0.0 ? -1.0 : 1.0,
                v.y < 0.0 ? -1.0 : 1.0);
}

vec2 OctahedronEncode(vec3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    if (n.z < 0.0) {
        n.xy = (1.0 - abs(n.yx)) * SignNotZero(n.xy);
    }
    return n.xy;
}

void main() {
    vec3 albedo = fragAlbedoColor.rgb; // 使用顶点着色器传递的颜色
    
    float useAlbedoTexture = fragTextureFlags.y;
    float useNormalTexture = fragTextureFlags.x;
    float useEmissiveTexture = fragTextureFlags.z;
    // 是模型级（任一 subMesh 有 MR 纹理则全体为 1），无 MR 纹理的 subMesh 会采样 1x1 黑色占位
    // → roughness=0/metallic=0 光滑镜面。mrValid=0（无 MR 纹理/黑占位/加载失败）→ 不采样，
    // 用 CPU 材质参数（materialData / per-subMesh factor / 引擎默认 0.75 粗糙非金属）。
    float useMRTexture = subMeshMaterial.w;

    //   OPAQUE(0)   → 忽略 alpha，不 discard
    //   MASK(1)     → texColor.a < alphaCutoff 丢弃（镂空；alphaCutoff 默认 0.5）
    //   BLEND(2)    → 不 discard，输出真 alpha（半透明混合管线后续批次接入）
    //   未知(-1)    → 使用 0.5 作为导入资产的默认 alpha cutoff
    float alphaMode = subMeshAlpha.y;
    float outAlpha = 1.0;
    if (useAlbedoTexture > 0.5) {
        vec4 texColor = texture(albedoTexture, fragTexCoord);
        if (alphaMode > 1.5) {
            // BLEND
            albedo = texColor.rgb * fragAlbedoColor.rgb;
            outAlpha = texColor.a * fragAlbedoColor.a;
        } else if (alphaMode > 0.5) {
            // MASK：alphaCutoff 镂空
            if (texColor.a < subMeshAlpha.x) {
                discard;
            }
            albedo = texColor.rgb * fragAlbedoColor.rgb;
        } else if (alphaMode < -0.5) {
            // Imported assets without an explicit alpha mode use a 0.5 cutoff.
            if (texColor.a < 0.5) {
                discard;
            }
            albedo = texColor.rgb * fragAlbedoColor.rgb;
        } else {
            // OPAQUE（glTF 规范）：alpha 忽略
            albedo = texColor.rgb * fragAlbedoColor.rgb;
        }
    }
    
    vec3 N = normalize(fragNormal);
    
    if (useNormalTexture > 0.5) {
        vec3 tangent = normalize(fragTangent);
        vec3 bitangent = normalize(fragBitangent);
        mat3 TBN = mat3(tangent, bitangent, N);
        
        vec3 normalSample = texture(normalTexture, fragTexCoord).rgb;
        vec3 n = normalSample * 2.0 - 1.0;
        // BC5 打包法线（RG 只有 xy，B 通道无数据）：重建 z。
        // Reconstruct the missing Z component used by two-channel normal maps.
        n.z = sqrt(max(0.0, 1.0 - n.x * n.x - n.y * n.y));
        N = normalize(TBN * n);
    }
    // 会错误受光/反照率朝向颠倒，叶片类单面几何背面发白）。翻转最终 N（含法线贴图结果）。
    if (subMeshAlpha.z > 0.5 && !gl_FrontFacing) N = -N;
    
    if (useEmissiveTexture > 0.5) {
        // 自发光：发光色直接并入 albedo（Bistro 发光体 BaseColor 黑，Emissive 贴图提供颜色）
        // 强度 = fragMaterialData.w（emissiveIntensity）；材质无自发光纹理时该分支不执行
        vec3 emissiveColor = texture(emissiveTexture, fragTexCoord).rgb;
        albedo += emissiveColor * max(fragMaterialData.w, 1.0);
    }

    // 粗糙度/金属度：有纹理用纹理值，无纹理用 CPU 材质参数（写入 MRT 材质附件）
    float metallic = fragMaterialData.x >= 0.0 ? fragMaterialData.x : (subMeshMaterial.x >= 0.0 ? subMeshMaterial.x : 0.0);
    float roughness = fragMaterialData.y >= 0.0 ? fragMaterialData.y : (subMeshMaterial.y >= 0.0 ? subMeshMaterial.y : 1.0);
    float ao = fragMaterialData.z >= 0.0 ? fragMaterialData.z : (subMeshMaterial.z >= 0.0 ? subMeshMaterial.z : 1.0);
    float emissiveStrength = fragMaterialData.w;
    if (useMRTexture > 0.5) {
        // （DamagedHelmet 面罩 = 黑色金属——F0=albedo 黑 → 反射物理弱；metallic 通道读值正常）
        // 已恢复 useMRTexture 条件（无纹理模型不采样）
        // factor = subMeshMaterial（glTF factor）> 材质组件 > 1.0（glTF 规范默认）
        float metalFactor = subMeshMaterial.x >= 0.0 ? subMeshMaterial.x : (fragMaterialData.x >= 0.0 ? fragMaterialData.x : 1.0);
        float roughFactor = subMeshMaterial.y >= 0.0 ? subMeshMaterial.y : (fragMaterialData.y >= 0.0 ? fragMaterialData.y : 1.0);
        float roughSample = texture(roughnessTexture, fragTexCoord).g;
        float metalSample = texture(metallicTexture, fragTexCoord).b;
        roughness = clamp(roughSample * roughFactor, 0.0, 1.0);
        metallic = clamp(metalSample * metalFactor, 0.0, 1.0);
    }
    // 材质附件是 R8G8B8A8_UNORM（8bit），此处 ×3 会被 clamp 到 1（曾实测无效）
    if (useEmissiveTexture > 0.5) {
        emissiveStrength = max(emissiveStrength, dot(texture(emissiveTexture, fragTexCoord).rgb, vec3(0.299, 0.587, 0.114)));
    }

    // G-buffer 主颜色的 alpha 当前不参与混合：0.5~0.75 编码显式漫反射透射系数。
    // alpha-mask + 双面只代表薄片几何，不隐式开启透射；没有 KHR 透射扩展时保持 0.0。
    // 只给 MASK 薄片保留标记，OPAQUE 双面实体仍是普通不透明 PBR。
    float thinSheetTag = 1.0;
    if (subMeshAlpha.z > 0.5 && alphaMode > 0.5 && alphaMode < 1.5) {
        thinSheetTag = 0.5 + 0.25 * clamp(subMeshAlpha.w, 0.0, 1.0);
    }
    outColor = vec4(albedo, thinSheetTag);
    outNormal = vec4(OctahedronEncode(N), 0.0, 0.0);   // R16G16_SNORM 八面体编码：完整世界法线含朝向（32bit）
    outMaterial = vec4(metallic, roughness, ao, emissiveStrength);
    outMotionVector = fragMotionVector;   // 运动=附件3（TAA 预留）
}
