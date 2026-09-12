#include "ModelRenderer.h"
#include "ModelRendererInternals.h"

// -1 = 未设（无 glTF factor）→ model.frag 用实例 materialData（材质组件）或引擎默认
// forceDoubleSided 是当前绘制管线的 ECS 双面开关；不能只依赖 sm.doubleSided，
// 因为编辑器创建的普通平面也可以通过 RenderComponent 单独开启双面渲染。
void PushSubMeshMaterialParams(VkCommandBuffer cmd, VkPipelineLayout layout,
                               const SubMeshRenderData& sm, bool forceDoubleSided) {
    glm::vec4 mat(sm.metallic >= 0.0f ? sm.metallic : -1.0f,
                  sm.roughness >= 0.0f ? sm.roughness : -1.0f,
                  sm.ao, sm.mrValid);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(ModelUniformData), 16, &mat);
    glm::vec4 alpha(sm.alphaCutoff, (float)sm.alphaMode,
                    (forceDoubleSided || sm.doubleSided) ? 1.0f : 0.0f,
                    sm.diffuseTransmissionFactor);   // z=doubleSided，w=显式漫反射透射系数
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(ModelUniformData) + 16, 16, &alpha);
}

void ModelRenderer::Render(VkCommandBuffer commandBuffer, const glm::mat4& view, const glm::mat4& proj)
{
    (void)commandBuffer;
    (void)view;
    (void)proj;
}
