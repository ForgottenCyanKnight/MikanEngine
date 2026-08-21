#include "PreviewModelRenderer.h"
#include "EngineGlobal.h"
#include "EngineConfig.h"
#include "EditorManager.h"
#include "TexturePool.h"

void PreviewModelRenderer::Init(VkRenderPass renderPass) {
    m_TexturePool = EditorManager::GetInstance().GetTexturePool();
    if (m_TexturePool == nullptr) {
        return;
    }
    
    CreatePipeline(renderPass);
    CreateUniformBuffer();
    CreateInstanceBuffer(100);
}

void PreviewModelRenderer::CreatePipeline(VkRenderPass renderPass) {
    PipelineConfig config;
    config.vertShader = "preview.vert.spv";
    config.fragShader = "preview.frag.spv";
    config.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    config.cullMode = VK_CULL_MODE_BACK_BIT;
    config.colorAttachmentCount = 1;
    config.usePushConstants = true;
    config.pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    config.pushConstantRange.offset = 0;
    config.pushConstantRange.size = sizeof(ModelUniformData);
    
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
    
    attrDescs.push_back({0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, Position)});
    attrDescs.push_back({1, 0, VK_FORMAT_R8G8B8A8_SNORM, offsetof(Vertex, Normal)});    // 压缩：SNORM int8
    attrDescs.push_back({2, 0, VK_FORMAT_R16G16_SFLOAT, offsetof(Vertex, TexCoords)});  // 压缩：HALF
    attrDescs.push_back({3, 0, VK_FORMAT_R8G8B8A8_SNORM, offsetof(Vertex, Tangent)});   // 压缩：xyz+w 手性
    // location 4: Bitangent 已移除（压缩顶点不再存储；shader 推导）
    
    attrDescs.push_back({5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(ModelInstanceData, model)});
    attrDescs.push_back({6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(ModelInstanceData, model) + 16});
    attrDescs.push_back({7, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(ModelInstanceData, model) + 32});
    attrDescs.push_back({8, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(ModelInstanceData, model) + 48});
    attrDescs.push_back({9, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(ModelInstanceData, albedoColor)});
    attrDescs.push_back({10, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(ModelInstanceData, materialData)});
    attrDescs.push_back({11, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(ModelInstanceData, textureFlags)});
    
    config.vertexAttributes = attrDescs;

    if (!m_ModelData.pipeline.Create(renderPass, DescriptorSetCache::GetInstance().GetLayout(), config)) {
        return;
    }
}
