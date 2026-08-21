#version 450

// 为 Adreno GPU 强制使用高精度
precision highp float;
precision highp int;

layout(location = 0) in vec3 fragPosition;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in highp vec2 fragTexCoord;
layout(location = 3) in vec3 fragTangent;
layout(location = 4) in vec3 fragBitangent;
// 2026-08-17：offset 修正——ModelUniformData 实为 160B（TAA 加了 cameraPosition 后 144→160），
    // 旧硬编码 144/160 使 C++ push 写 [160,176) 而 shader 读 [144,160)=cameraPosition+padding → w 恒 0 → MR 采样条件永不成立
    layout(push_constant) uniform PC_Material {
    layout(offset = 160) vec4 subMeshMaterial;   // x=metallic y=roughness z=ao（-1=未设）w=mrValid（MR 纹理有效性）
    layout(offset = 176) vec4 subMeshAlpha;      // 2026-08-16 x=alphaCutoff y=alphaMode（-1=未知 0=OPAQUE 1=MASK 2=BLEND）；2026-08-17 z=doubleSided（背面翻转法线）
};
layout(location = 5) in vec4 fragAlbedoColor;
layout(location = 6) in vec4 fragMaterialData;
layout(location = 7) in vec4 fragTextureFlags;
layout(location = 8) in vec2 fragMotionVector;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outMaterial;   // 2026-08 附件重排：材质=附件2（location 2）
layout(location = 3) out vec2 outMotionVector; // 运动=附件3（location 3，TAA 预留）

layout(binding = 0) uniform sampler2D albedoTexture;
layout(binding = 1) uniform sampler2D normalTexture;
layout(binding = 2) uniform sampler2D roughnessTexture;   // 2026-08-09 粗糙度贴图（MRT 材质附件）
layout(binding = 3) uniform sampler2D metallicTexture;    // 2026-08-09 金属度贴图
layout(binding = 5) uniform sampler2D emissiveTexture;   // 2026-08-09 自发光贴图（Bistro 发光体 BaseColor 黑 + Emissive 亮）

// 2026-08-11 八面体编码（Cigolle 2014 对称版）——世界法线 → [-1,1]²（R16G16_SNORM 直接存，含朝向）
vec2 OctahedronEncode(vec3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    if (n.z < 0.0) {
        n.xy = (1.0 - abs(n.yx)) * sign(n.xy);
    }
    return n.xy;
}

void main() {
    vec3 albedo = fragAlbedoColor.rgb; // 使用顶点着色器传递的颜色
    
    float useAlbedoTexture = fragTextureFlags.y; // 2026-08：从 textureFlags.y（materialData.w 改存自发光强度）
    float useNormalTexture = fragTextureFlags.x;
    float useEmissiveTexture = fragTextureFlags.z;   // 2026-08-09 自发光贴图开关
    // 2026-08-17：MR 采样开关改 per-subMesh（subMeshMaterial.w = mrValid）——原 fragTextureFlags.w
    // 是模型级（任一 subMesh 有 MR 纹理则全体为 1），无 MR 纹理的 subMesh 会采样 1x1 黑色占位
    // → roughness=0/metallic=0 光滑镜面。mrValid=0（无 MR 纹理/黑占位/加载失败）→ 不采样，
    // 用 CPU 材质参数（materialData / per-subMesh factor / 引擎默认 0.75 粗糙非金属）。
    float useMRTexture = subMeshMaterial.w;

    // 2026-08-16 alphaMode 三分支（glTF 2.0 语义；-1=未知走旧行为保护 assimp/FBX 资产）：
    //   OPAQUE(0)   → 忽略 alpha，不 discard
    //   MASK(1)     → texColor.a < alphaCutoff 丢弃（镂空；alphaCutoff 默认 0.5）
    //   BLEND(2)    → 不 discard，输出真 alpha（半透明混合管线后续批次接入）
    //   未知(-1)    → 回退旧行为：texColor.a < 0.5 丢弃（FBX 树叶/栅栏等依赖纹理 alpha 镂空的资产）
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
            // 未知（assimp/FBX 路径）：旧行为——alpha<0.5 镂空
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
        // 与 IDKEngine 的 ReconstructPackedNormal 一致；普通法线贴图归一化时重建 z 与真实 z 相同。
        n.z = sqrt(max(0.0, 1.0 - n.x * n.x - n.y * n.y));
        N = normalize(TBN * n);
    }
    // 2026-08-17 glTF doubleSided：背面翻转法线（glTF 规范语义——双面渲染的背面片元用正面法线
    // 会错误受光/反照率朝向颠倒，叶片类单面几何背面发白）。翻转最终 N（含法线贴图结果）。
    if (subMeshAlpha.z > 0.5 && !gl_FrontFacing) N = -N;
    
    if (useEmissiveTexture > 0.5) {
        // 自发光：发光色直接并入 albedo（Bistro 发光体 BaseColor 黑，Emissive 贴图提供颜色）
        // 强度 = fragMaterialData.w（emissiveIntensity）；材质无自发光纹理时该分支不执行
        vec3 emissiveColor = texture(emissiveTexture, fragTexCoord).rgb;
        albedo += emissiveColor * max(fragMaterialData.w, 1.0);
    }

    // 粗糙度/金属度：有纹理用纹理值，无纹理用 CPU 材质参数（写入 MRT 材质附件）
    // 2026-08-11 优先级：实例 materialData（材质组件）>= 0 用实例；否则 per-subMesh push（glTF factor）；再否则引擎默认
    // 2026-08-17 默认改 roughness=1.0（用户拍板：metallic 0 / roughness 1.0——非金属全粗糙，最贴近"未处理表面"；勿用 0.75）
    float metallic = fragMaterialData.x >= 0.0 ? fragMaterialData.x : (subMeshMaterial.x >= 0.0 ? subMeshMaterial.x : 0.0);
    float roughness = fragMaterialData.y >= 0.0 ? fragMaterialData.y : (subMeshMaterial.y >= 0.0 ? subMeshMaterial.y : 1.0);
    float ao = fragMaterialData.z >= 0.0 ? fragMaterialData.z : (subMeshMaterial.z >= 0.0 ? subMeshMaterial.z : 1.0);
    float emissiveStrength = fragMaterialData.w;
    if (useMRTexture > 0.5) {
        // 2026-08-11 glTF metallicRoughnessTexture 约定：G=roughness、B=metallic（灰度纹理 .g/.b==.r 兼容旧模型）
        // ⚠️ 2026-08-16 临时调试结论（用户实测）：无条件采样效果相同 → 头盔面罩反射弱是资产本身
        // （DamagedHelmet 面罩 = 黑色金属——F0=albedo 黑 → 反射物理弱；metallic 通道读值正常）
        // 已恢复 useMRTexture 条件（无纹理模型不采样）
        // 2026-08-17 对齐官方：值 = factor × 纹理（官方 perceptualRoughness *= mrSample.g）；
        // factor = subMeshMaterial（glTF factor）> 材质组件 > 1.0（glTF 规范默认）
        float metalFactor = subMeshMaterial.x >= 0.0 ? subMeshMaterial.x : (fragMaterialData.x >= 0.0 ? fragMaterialData.x : 1.0);
        float roughFactor = subMeshMaterial.y >= 0.0 ? subMeshMaterial.y : (fragMaterialData.y >= 0.0 ? fragMaterialData.y : 1.0);
        float roughSample = texture(roughnessTexture, fragTexCoord).g;
        float metalSample = texture(metallicTexture, fragTexCoord).b;
        roughness = clamp(roughSample * roughFactor, 0.0, 1.0);
        metallic = clamp(metalSample * metalFactor, 0.0, 1.0);
    }
    // 2026-08-13 回退：正常使用 Emissive 贴图亮度（0-1）——放大统一在合成端做（fullscreen.frag ×10）：
    // 材质附件是 R8G8B8A8_UNORM（8bit），此处 ×3 会被 clamp 到 1（曾实测无效）
    if (useEmissiveTexture > 0.5) {
        emissiveStrength = max(emissiveStrength, dot(texture(emissiveTexture, fragTexCoord).rgb, vec3(0.299, 0.587, 0.114)));
    }

    outColor = vec4(albedo, outAlpha);
    outNormal = vec4(OctahedronEncode(N), 0.0, 0.0);   // R16G16_SNORM 八面体编码：完整世界法线含朝向（32bit）
    outMaterial = vec4(metallic, roughness, ao, emissiveStrength); // 2026-08-09：纹理/参数混合写入 MRT 材质附件（xyz=metal/rough/ao, w=emissive强度）
    outMotionVector = fragMotionVector;   // 运动=附件3（TAA 预留）
}
