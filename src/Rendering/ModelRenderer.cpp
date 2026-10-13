#include "ModelRenderer.h"
#include "ModelRendererInternals.h"
#include "Core/RenderGlobals.h"
#include <cstdlib>

// -1 = 未设（无 glTF factor）→ model.frag 用实例 materialData（材质组件）或引擎默认
// forceDoubleSided 是当前绘制管线的 ECS 双面开关；不能只依赖 sm.doubleSided，
// 因为编辑器创建的普通平面也可以通过 RenderComponent 单独开启双面渲染。
void PushSubMeshMaterialParams(VkCommandBuffer cmd, VkPipelineLayout layout,
                               const SubMeshRenderData& sm, bool forceDoubleSided) {
    glm::vec4 mat(sm.metallic >= 0.0f ? sm.metallic : -1.0f,
                  sm.roughness >= 0.0f ? sm.roughness : -1.0f,
                  sm.ao, sm.mrValid);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(ModelUniformData), 16, &mat);
    static const bool hybrid=[](){const char* v=std::getenv("MIKAN_HWRT_HYBRID");const char* p=std::getenv("MIKAN_GPU_PROFILE");return v&&v[0]=='1'&&p&&p[0]=='1';}();
    glm::vec4 base=sm.baseColorFactor;
    if(hybrid){
        auto manuallyDecode=[](const std::string& path){
            const auto* texture=g_TexturePool?g_TexturePool->GetTexture(path):nullptr;
            if(!texture)return false;
            switch(texture->format){
                case VK_FORMAT_R8G8B8A8_SRGB:case VK_FORMAT_B8G8R8A8_SRGB:
                case VK_FORMAT_BC1_RGB_SRGB_BLOCK:case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
                case VK_FORMAT_BC2_SRGB_BLOCK:case VK_FORMAT_BC3_SRGB_BLOCK:
                case VK_FORMAT_BC7_SRGB_BLOCK:case VK_FORMAT_ASTC_4x4_SRGB_BLOCK:return false;
                default:return true;
            }
        };
        base.a=-1.0f-float((manuallyDecode(sm.diffuseTexturePath)?1u:0u)|(manuallyDecode(sm.emissiveTexturePath)?2u:0u));
    }
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(ModelUniformData) + 32, 16, &base);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(ModelUniformData) + 48, 16, &sm.emissiveFactor);
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
