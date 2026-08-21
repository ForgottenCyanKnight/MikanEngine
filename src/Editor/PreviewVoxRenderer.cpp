#include "PreviewVoxRenderer.h"
#include "EngineGlobal.h"

void PreviewVoxRenderer::Init(VkRenderPass renderPass) {
    VoxRenderer::Init(renderPass);
}

void PreviewVoxRenderer::CreatePipeline(VkRenderPass renderPass) {
    // 调用父类的 CreatePipeline 方法创建基础管线
    VoxRenderer::CreatePipeline(renderPass);
    
    // 现在创建自定义的预览 mesh 管线，使用新的着色器
    PipelineConfig meshConfig;
    meshConfig.vertShader = "voxel_preview.vert.spv";
    meshConfig.fragShader = "voxel_preview.frag.spv";
    meshConfig.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    meshConfig.cullMode = VK_CULL_MODE_NONE;
    meshConfig.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    meshConfig.depthTest = true;
    meshConfig.depthWrite = true;
    meshConfig.colorAttachmentCount = 1;
    
    VkVertexInputBindingDescription meshBindings[2] = {};
    meshBindings[0].binding = 0;
    meshBindings[0].stride = sizeof(VoxelMeshVertex);
    meshBindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    
    meshBindings[1].binding = 1;
    meshBindings[1].stride = sizeof(VoxelInstanceData);
    meshBindings[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
    
    VkVertexInputAttributeDescription meshAttributes[15] = {};
    meshAttributes[0].binding = 0;
    meshAttributes[0].location = 0;
    meshAttributes[0].format = VK_FORMAT_R8G8B8A8_UINT;
    meshAttributes[0].offset = 0;  // x, y, z, padding
    
    meshAttributes[1].binding = 0;
    meshAttributes[1].location = 1;
    meshAttributes[1].format = VK_FORMAT_R8G8B8A8_UINT;
    meshAttributes[1].offset = 3;  // r, g, b, padding
    
    meshAttributes[2].binding = 0;
    meshAttributes[2].location = 2;
    meshAttributes[2].format = VK_FORMAT_R16_UINT;
    meshAttributes[2].offset = 6;  // faceDirAndMaterial
    
    for (int i = 0; i < 4; i++) {
        meshAttributes[3 + i].binding = 1;
        meshAttributes[3 + i].location = 3 + i;
        meshAttributes[3 + i].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        meshAttributes[3 + i].offset = offsetof(VoxelInstanceData, model) + sizeof(glm::vec4) * i;
    }
    
    for (int i = 0; i < 4; i++) {
        meshAttributes[7 + i].binding = 1;
        meshAttributes[7 + i].location = 7 + i;
        meshAttributes[7 + i].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        meshAttributes[7 + i].offset = offsetof(VoxelInstanceData, prevModel) + sizeof(glm::vec4) * i;
    }
    
    meshAttributes[11].binding = 1;
    meshAttributes[11].location = 11;
    meshAttributes[11].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    meshAttributes[11].offset = offsetof(VoxelInstanceData, albedoColor);
    
    meshAttributes[12].binding = 1;
    meshAttributes[12].location = 12;
    meshAttributes[12].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    meshAttributes[12].offset = offsetof(VoxelInstanceData, materialData);
    
    meshAttributes[13].binding = 1;
    meshAttributes[13].location = 13;
    meshAttributes[13].format = VK_FORMAT_R32G32B32_SFLOAT;
    meshAttributes[13].offset = offsetof(VoxelInstanceData, worldMinBounds);
    
    meshAttributes[14].binding = 1;
    meshAttributes[14].location = 14;
    meshAttributes[14].format = VK_FORMAT_R32_SFLOAT;
    meshAttributes[14].offset = offsetof(VoxelInstanceData, voxelSize);
    
    meshConfig.vertexBindings.assign(meshBindings, meshBindings + 2);
    meshConfig.vertexAttributes.assign(meshAttributes, meshAttributes + 15);
    
    meshConfig.pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    meshConfig.pushConstantRange.offset = 0;
    meshConfig.pushConstantRange.size = sizeof(VoxelMeshUniformData);
    meshConfig.usePushConstants = true;
    
    // 直接修改父类的 mesh 管线
    // 注意：这里假设 m_RenderData 是 protected 成员
    // 如果编译失败，我们需要修改 VoxRenderer.h 文件
    if (!m_RenderData.meshPipeline.Create(renderPass, VK_NULL_HANDLE, meshConfig)) {
        throw std::runtime_error("Failed to create voxel mesh pipeline!");
    }
}