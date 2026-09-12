#include "ModelRenderer.h"
#include "Core/RenderGlobals.h"
#include "VulkanManager.h"

#include <cstddef>

void ModelRenderer::CreatePipeline(VkRenderPass renderPass)
{
    // 使用描述符集缓存中的布局
    VkDescriptorSetLayout descriptorLayout = DescriptorSetCache::GetInstance().GetLayout();

    PipelineConfig config;
    config.vertShader = "model.vert.spv";
    config.fragShader = "model.frag.spv";
    config.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    config.cullMode = VK_CULL_MODE_BACK_BIT;
    config.colorAttachmentCount = kMainMrtGeometryColorAttachmentCount;
    // Desktop subpass 1 also declares the reserved composite slot (index 4).
    // It must have a blend-state entry, but must never receive geometry output.
    config.colorWriteMasks = {
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        0
    };
    config.subpass = 1;               // MRT 几何 subpass（0=z-prepass depth-only）
    // z-prepass 后 MRT 深度测试必须 LESS_OR_EQUAL——z-prepass 写的深度与本阶段片元深度几乎相等，LESS 严格小于会剔除内部像素只剩剪影
    config.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    // z-prepass 关闭时（g_EnableZPrepass=false）几何 pass 恢复写深度（深度附件无预填）。
    // ⚠️ 运行时切换 g_EnableZPrepass 需重启引擎（管线创建时固化）；BLEND 半透明不受影响（不写深度语义一致）。
    config.depthWrite = !g_EnableZPrepass;
    config.usePushConstants = true;
    config.pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    config.pushConstantRange.offset = 0;
    config.pushConstantRange.size = sizeof(ModelUniformData) + 32;
    
    VkVertexInputBindingDescription vertexBindingDesc = {};
    vertexBindingDesc.binding = 0;
    vertexBindingDesc.stride = sizeof(Vertex);
    vertexBindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    config.vertexBindings.push_back(vertexBindingDesc);
    
    VkVertexInputBindingDescription instanceBindingDesc = {};
    instanceBindingDesc.binding = 1;
    instanceBindingDesc.stride = sizeof(ModelInstanceData);
    instanceBindingDesc.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
    config.vertexBindings.push_back(instanceBindingDesc);
    
    std::vector<VkVertexInputAttributeDescription> attrDescs;
    
    VkVertexInputAttributeDescription posAttr = {};
    posAttr.binding = 0;
    posAttr.location = 0;
    posAttr.format = VK_FORMAT_R32G32B32_SFLOAT;
    posAttr.offset = offsetof(Vertex, Position);
    attrDescs.push_back(posAttr);
    
    VkVertexInputAttributeDescription normalAttr = {};
    normalAttr.binding = 0;
    normalAttr.location = 1;
    normalAttr.format = VK_FORMAT_R8G8B8A8_SNORM;   // 压缩：int8 驱动自动归一化 -1..1
    normalAttr.offset = offsetof(Vertex, Normal);
    attrDescs.push_back(normalAttr);
    
    VkVertexInputAttributeDescription texAttr = {};
    texAttr.binding = 0;
    texAttr.location = 2;
    texAttr.format = VK_FORMAT_R16G16_SFLOAT;   // 压缩：half float
    texAttr.offset = offsetof(Vertex, TexCoords);
    attrDescs.push_back(texAttr);
    
    VkVertexInputAttributeDescription tangentAttr = {};
    tangentAttr.binding = 0;
    tangentAttr.location = 3;
    tangentAttr.format = VK_FORMAT_R8G8B8A8_SNORM;   // 压缩：xyz 方向 + w 手性（Bitangent 不再存储，shader 推导）
    tangentAttr.offset = offsetof(Vertex, Tangent);
    attrDescs.push_back(tangentAttr);
    
    // location 4: Bitangent 已移除（压缩顶点不再存储；shader 由 cross(normal, tangent.xyz)*tangent.w 推导）
    
    // Instance attributes (location 5-8: model matrix)
    for (int i = 0; i < 4; i++) {
        VkVertexInputAttributeDescription modelAttr = {};
        modelAttr.binding = 1;
        modelAttr.location = 5 + i;
        modelAttr.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        modelAttr.offset = offsetof(ModelInstanceData, model) + sizeof(glm::vec4) * i;
        attrDescs.push_back(modelAttr);
    }
    
    // location 9-12: prevModel matrix (mat4)
    for (int i = 0; i < 4; i++) {
        VkVertexInputAttributeDescription prevModelAttr = {};
        prevModelAttr.binding = 1;
        prevModelAttr.location = 9 + i;
        prevModelAttr.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        prevModelAttr.offset = offsetof(ModelInstanceData, prevModel) + sizeof(glm::vec4) * i;
        attrDescs.push_back(prevModelAttr);
    }
    
    // location 13: albedoColor (vec4)
    VkVertexInputAttributeDescription albedoColorAttr = {};
    albedoColorAttr.binding = 1;
    albedoColorAttr.location = 13;
    albedoColorAttr.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    albedoColorAttr.offset = offsetof(ModelInstanceData, albedoColor);
    attrDescs.push_back(albedoColorAttr);
    
    // location 14: materialData (vec4) - metallic, roughness, ao, useAlbedoTexture
    VkVertexInputAttributeDescription materialDataAttr = {};
    materialDataAttr.binding = 1;
    materialDataAttr.location = 14;
    materialDataAttr.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    materialDataAttr.offset = offsetof(ModelInstanceData, materialData);
    attrDescs.push_back(materialDataAttr);
    
    // location 15: textureFlags (vec4) - useNormalTexture, padding...
    VkVertexInputAttributeDescription textureFlagsAttr = {};
    textureFlagsAttr.binding = 1;
    textureFlagsAttr.location = 15;
    textureFlagsAttr.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    textureFlagsAttr.offset = offsetof(ModelInstanceData, textureFlags);
    attrDescs.push_back(textureFlagsAttr);
    
    // location 16: BoneIDs (u8vec4) - 骨骼蒙皮（无骨骼模型全 0xFF/weight=0，shader clamp 跳过）
    VkVertexInputAttributeDescription boneIdsAttr = {};
    boneIdsAttr.binding = 0;
    boneIdsAttr.location = 16;
    boneIdsAttr.format = VK_FORMAT_R8G8B8A8_UINT;   // 压缩：uint8 ×4
    boneIdsAttr.offset = offsetof(Vertex, BoneIDs);
    attrDescs.push_back(boneIdsAttr);

    // location 17: BoneWeights (vec4) - UNORM 驱动自动归一化 0..1
    VkVertexInputAttributeDescription boneWeightsAttr = {};
    boneWeightsAttr.binding = 0;
    boneWeightsAttr.location = 17;
    boneWeightsAttr.format = VK_FORMAT_R8G8B8A8_UNORM;   // 压缩：uint8 量化 0-255
    boneWeightsAttr.offset = offsetof(Vertex, BoneWeights);
    attrDescs.push_back(boneWeightsAttr);
    
    config.vertexAttributes = attrDescs;

    m_VertexBindings = config.vertexBindings;
    m_VertexAttributes = config.vertexAttributes;

    if (!m_ModelData.pipeline.Create(renderPass, descriptorLayout, config)) {
        return;
    }
    
    // 创建双面渲染管线（禁用背面剔除）
    PipelineConfig doubleSidedConfig = config;
    doubleSidedConfig.cullMode = VK_CULL_MODE_NONE;  // 双面渲染，不剔除任何面
    
    if (!m_ModelData.doubleSidedPipeline.Create(renderPass, descriptorLayout, doubleSidedConfig)) {
        return;
    }
    
    // 创建线框渲染管线
    PipelineConfig wireframeConfig = config;
    wireframeConfig.polygonMode = VK_POLYGON_MODE_LINE;  // 线框模式
    wireframeConfig.cullMode = VK_CULL_MODE_NONE;  // 线框模式下不剔除任何面
    
    if (!m_ModelData.wireframePipeline.Create(renderPass, descriptorLayout, wireframeConfig)) {
        return;
    }

    // ===== z-prepass depth-only 管线（subpass 0）：与主 model 管线同顶点布局/蒙皮，仅输出深度 =====
    // 复用 config（顶点绑定/属性/蒙皮 UBO/push constant 一致）；蒙皮 zprepass 绑完整 set（蒙皮 UBO binding 4）
    if (!g_UseSeparateMrtRenderPass) {
        PipelineConfig depthConfig = config;
        depthConfig.vertShader = "zprepass.vert.spv";
        depthConfig.fragShader = "model_zprepass.frag.spv";
        depthConfig.colorAttachmentCount = kMainMrtZPrepassColorAttachmentCount;
        // Desktop subpass 0 has the reserved composite color attachment even for
        // the depth-only pipeline; keep its blend slot disabled.
        depthConfig.colorWriteMasks = { 0 };
        depthConfig.subpass = 0;                   // z-prepass subpass
        depthConfig.depthCompareOp = VK_COMPARE_OP_LESS;
        depthConfig.depthWrite = true;
        if (!m_ModelData.depthPipeline.Create(renderPass, descriptorLayout, depthConfig)) {
            return;
        }
    }
}
