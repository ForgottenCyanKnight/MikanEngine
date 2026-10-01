#include "Rendering/PmxRenderer.h"
#include "Rendering/ModelRenderer.h"
#include "Rendering/SceneRenderer.h"
#include "Rendering/RenderStats.h"
#include "PipelineBlendState.h"
#include "Animation/MMD/Model/PMXFile.h"
#include "Core/VulkanContext.h"
#include "Core/VulkanManager.h"
#include "Core/RenderGlobals.h"
#include "Core/Utf8Path.h"
#include "Core/EngineConfig.h"
#include "Core/ProjectManager.h"
#include "Core/Log.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <cstdio>
#include <limits>
#include <memory>
#include <unordered_map>
#include <fstream>
#include "json.hpp"

namespace {
struct alignas(16) PmxDrawUniform {
    glm::mat4 projection{1}, modelView{1};
    glm::mat4 model{1}, previousMVP{1};
    glm::vec4 diffuse{1}, specularPower{0}, ambient{0}, edgeColor{0};
    glm::vec4 lightDirection{0}, lightColor{1}, viewportEdge{0}, jitter{0};
    glm::ivec4 modes{0};
    glm::mat4 shadowMatrices[4]{};
    glm::vec4 shadowSplitDepths{-1},shadowParams{0};
    glm::vec4 pbrMaterial{0,-1,1,0};
    glm::vec4 textureMul{1},textureAdd{0},sphereMul{1},sphereAdd{0};
};
static_assert(sizeof(PmxDrawUniform) == 768);
struct PmxMaterial {
    mmd::PMXMaterial source{};
    glm::vec2 pbrOverride{-1,-1};
    std::array<std::string,4> textures;
    bool normalLoaded=false;
    std::array<VkDescriptorSet,6> descriptors{};
    glm::ivec4 modes{0};
};
}

// One sidecar per entity-local renderer. Geometry remains in ModelRenderer's
// fence-protected CPU deformation ring; only PMX-specific material state lives here.
class PmxRenderer {
public:
    bool Init(ModelRenderer& model, CascadeShadowRenderer& shadows) {
        m_Shadows=&shadows;
        mmd::PMXFile file;
        const auto modelPath=ProjectManager::GetInstance().ResolveAssetPath(model.GetModelPath());
        if (!file.Load(modelPath)) return false;
        const auto& pmx = file.GetPMXModel();
        const auto directory = Utf8Path(modelPath).parent_path();
        nlohmann::json pbrRules=nlohmann::json::array();
        std::ifstream settings(Utf8Path(modelPath+".import.json"));
        if(settings) {
            try { pbrRules=nlohmann::json::parse(settings).value("pbrMaterials",nlohmann::json::array()); }
            catch(const std::exception& error) { LOGW("[PMX] invalid PBR import settings: %s",error.what()); }
        }
        auto texturePath = [&](int index) {
            if (index < 0 || size_t(index) >= pmx.m_textures.size()) return std::string{};
            std::string name = pmx.m_textures[index].m_textureName;
            std::replace(name.begin(),name.end(),'\\','/');
            std::error_code error;
            if(name.empty() || !std::filesystem::is_regular_file(directory/Utf8Path(name),error)) return std::string{};
            return Utf8String(directory/Utf8Path(name));
        };
        auto& meshes = model.GetMeshData().subMeshes;
        m_Materials.resize(meshes.size());
        for (size_t i=0;i<meshes.size();++i) {
            const auto& mesh=meshes[i];
            if (mesh.materialIndex < 0 || size_t(mesh.materialIndex)>=pmx.m_materials.size()) return false;
            auto& material=m_Materials[i];
            material.source=pmx.m_materials[mesh.materialIndex];
            const auto& source=material.source;
            material.textures[0]=texturePath(source.m_textureIndex);
            material.textures[2]=texturePath(source.m_sphereTextureIndex);
            for(const auto& rule:pbrRules) {
                const auto matches=[&](const char* key,size_t slot) {
                    return rule.contains(key) && rule.at(key).is_string() &&
                        rule.at(key).get<std::string>()==Utf8String(Utf8Path(material.textures[slot]).filename());
                };
                if(!matches("diffuseTexture",0) && !matches("sphereTexture",2) &&
                    !(rule.contains("materialName") && rule.at("materialName")==source.m_name)) continue;
                if(rule.contains("metallic") && rule.at("metallic").is_number()) material.pbrOverride.x=std::clamp(rule.at("metallic").get<float>(),0.0f,1.0f);
                if(rule.contains("roughness") && rule.at("roughness").is_number()) material.pbrOverride.y=std::clamp(rule.at("roughness").get<float>(),0.04f,1.0f);
                if(rule.contains("normalTexture") && rule.at("normalTexture").is_string())
                    material.textures[3]=Utf8String(directory/Utf8Path(rule.at("normalTexture").get<std::string>()));
                // Real environment reflections replace the authored sphere-map approximation.
                if(!rule.value("useSphereTexture",true)) material.textures[2].clear();
                LOGI("[PMX] PBR override material %zu: metallic=%.3f roughness=%.3f",i,material.pbrOverride.x,material.pbrOverride.y);
            }
            if (source.m_toonMode==mmd::PMXToonMode::Common && source.m_toonTextureIndex>=0 && source.m_toonTextureIndex<10) {
                char name[32];
                std::snprintf(name,sizeof(name),"toon%02d.bmp",source.m_toonTextureIndex+1);
                auto local=directory/Utf8Path(name);
                material.textures[1]=std::filesystem::exists(local) ? Utf8String(local)
                    : EngineConfig::GetEngineTexturePath((std::string("mmd/")+name).c_str());
            } else if (source.m_toonMode==mmd::PMXToonMode::Separate) {
                material.textures[1]=texturePath(source.m_toonTextureIndex);
            }
            std::vector<glm::vec3> extras;
            extras.reserve(mesh.mmdVertexIndices.size());
            for (uint32_t index:mesh.mmdVertexIndices) {
                const auto& vertex=pmx.m_vertices.at(index);
                const glm::vec2 uv1=pmx.m_header.m_addUVNum>0?glm::vec2(vertex.m_addUV[0]):glm::vec2(0);
                extras.emplace_back(uv1.x,uv1.y,vertex.m_edgeMag);
            }
            auto buffer=std::make_unique<VulkanBuffer>();
            if (!buffer->Create(extras.size()*sizeof(glm::vec3),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) return false;
            buffer->Map();
            buffer->Write(extras.data(),extras.size()*sizeof(glm::vec3));
            m_Extras.push_back(std::move(buffer));
        }
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(g_PhysicalDevice,&properties);
        VkPhysicalDeviceFeatures features{};
        vkGetPhysicalDeviceFeatures(g_PhysicalDevice,&features);
        m_IndependentBlend=features.independentBlend!=0;
        const auto alignment=std::max(VkDeviceSize(1),properties.limits.minUniformBufferOffsetAlignment);
        m_Stride=(sizeof(PmxDrawUniform)+alignment-1)/alignment*alignment;
        if (!m_Uniforms.Create(m_Stride*kDrawsPerFrame*3,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) return false;
        m_Uniforms.Map();
        m_Descriptor.AddBinding(0,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT);
        for (uint32_t binding=1;binding<=3;++binding)
            m_Descriptor.AddBinding(binding,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,VK_SHADER_STAGE_FRAGMENT_BIT);
        m_Descriptor.AddBinding(4,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,VK_SHADER_STAGE_FRAGMENT_BIT);
        m_Descriptor.AddBinding(5,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,VK_SHADER_STAGE_FRAGMENT_BIT);
        if (!m_Descriptor.CreateLayout() || !m_Descriptor.CreatePool(uint32_t(m_Materials.size()*6))) return false;
        for (auto& material:m_Materials) {
            for (auto& descriptor:material.descriptors)
                if (!m_Descriptor.AllocateSet(descriptor)) return false;
        }
        if (!g_TexturePool->GetTexture("white")) return false;
        for (uint32_t frame=0;frame<6;++frame) UpdateDescriptors(frame);
        LOGI("[PMX] dedicated materials + shared PBR lighting: %s (%zu materials)",model.GetModelPath().c_str(),m_Materials.size());
        return true;
    }

    void Draw(ModelRenderer& model, VkCommandBuffer commands, VkRenderPass pass,
        uint32_t width,uint32_t height,const glm::mat4& view,const glm::mat4& projection,
        const glm::mat4& transform,const glm::vec4& tint,const glm::vec3& direction,
        const glm::vec3& lightColor,const glm::vec2& jitter,uint64_t historyKey,int shadowSlot,bool receiveShadow,
        bool materialPass,const glm::vec4& pbrMaterial) {
        // The optional overlays need per-attachment masks to preserve G-buffer data.
        if (!materialPass && !m_IndependentBlend) return;
        // Scene/Game/probe targets use compatible HDR/depth formats. Do not
        // destroy pipelines while an earlier view or in-flight frame references them.
        if (!CreatePipelines(pass,materialPass)) return;
        // Texture hot reload changes image views. Rewrite this renderer's sets
        // only when bindings actually change; old sets remain alive until safe cleanup.
        const uint64_t serial=GetCurrentFrameSerial();
        const uint32_t frame=GetCurrentFrameIndex()%3;
        const uint32_t descriptorFrame=frame*2+uint32_t(shadowSlot);
        if (m_Serial[frame]!=serial) {
            m_Serial[frame]=serial;m_Cursor[frame]=0;
        }
        if (DescriptorsChanged(descriptorFrame)) UpdateDescriptors(descriptorFrame);
        model.FlushMmdVertices();
        auto& history=m_History[historyKey];
        const glm::mat4 currentMVP=projection*view*transform;
        if (!history.initialized) {
            history.current=history.previous=currentMVP;history.serial=serial;history.initialized=true;
        } else if (history.serial!=serial) {
            history.previous=history.current;history.current=currentMVP;history.serial=serial;
        }
        VkViewport viewport{0,0,float(width),float(height),0,1};
        VkRect2D scissor{{0,0},{width,height}};
        vkCmdSetViewport(commands,0,1,&viewport);
        vkCmdSetScissor(commands,0,1,&scissor);
        for (size_t i=0;i<m_Materials.size();++i) {
            const auto& material=m_Materials[i];
            const auto& source=material.source;
            const auto materialIndex=model.GetMeshData().subMeshes[i].materialIndex;
            const auto& animated=model.GetMmdMaterials();
            const auto* morph=materialIndex>=0 && size_t(materialIndex)<animated.size()?&animated[materialIndex]:nullptr;
            if ((morph?morph->m_diffuse.a:source.m_diffuse.a)<=0.0f) continue;
            if (m_Cursor[frame]>=kDrawsPerFrame) {
                LOGE("[PMX] frame uniform capacity exceeded; skipping remaining draws");
                return;
            }
            PmxDrawUniform uniform;
            uniform.projection=projection;
            uniform.modelView=view*transform;
            uniform.model=transform;
            uniform.previousMVP=history.previous;
            uniform.diffuse=source.m_diffuse*tint;
            uniform.specularPower=glm::vec4(source.m_specular,source.m_specularPower);
            uniform.pbrMaterial=pbrMaterial;
            if(material.pbrOverride.x>=0) uniform.pbrMaterial.x=material.pbrOverride.x;
            if(material.pbrOverride.y>=0) uniform.pbrMaterial.y=material.pbrOverride.y;
            uniform.ambient=glm::vec4(source.m_ambient,0);
            uniform.edgeColor=source.m_edgeColor;
            if(morph) {
                uniform.diffuse=morph->m_diffuse*tint;
                uniform.specularPower=glm::vec4(morph->m_specular,morph->m_specularPower);
                uniform.ambient=glm::vec4(morph->m_ambient,0);uniform.edgeColor=morph->m_edgeColor;
                uniform.textureMul=morph->m_textureMul;uniform.textureAdd=morph->m_textureAdd;
                uniform.sphereMul=morph->m_sphereMul;uniform.sphereAdd=morph->m_sphereAdd;
            }
            // PMX atlas morphs can author very large alpha to force a layer visible.
            // Floating-point HDR targets need the saturation that Saba's UNORM target supplied.
            uniform.diffuse.a=std::clamp(uniform.diffuse.a,0.0f,1.0f);
            // Engine sunDir points toward the light; Saba LightDir points away.
            uniform.lightDirection=glm::vec4(-glm::mat3(view)*direction,0);
            uniform.lightColor=glm::vec4(lightColor,1);
            uniform.viewportEdge=glm::vec4(width,height,morph?morph->m_edgeSize:source.m_edgeSize,m_IndependentBlend?0:1);
            uniform.jitter=glm::vec4(jitter,0,0);
            uniform.modes=material.modes;
            uniform.modes.w=receiveShadow && (uint8_t(source.m_drawMode)&8)!=0?1:0;
            uniform.shadowParams=glm::vec4(1,1,uint32_t(historyKey>>32)>=2?1:0,0);
            uniform.shadowParams.w=material.normalLoaded?1.0f:0.0f;
            for (int layer=0;layer<4;++layer) {
                uniform.shadowMatrices[layer]=m_Shadows->GetShadowMatrix(shadowSlot,layer);
                uniform.shadowSplitDepths[layer]=m_Shadows->IsCascadeValid(shadowSlot,layer)
                    ? m_Shadows->GetSplitFar(shadowSlot,layer) : -1.0f;
            }
            const uint32_t offset=uint32_t(m_Stride*(frame*kDrawsPerFrame+m_Cursor[frame]++));
            m_Uniforms.Write(&uniform,sizeof(uniform),offset);
            VkDeviceSize vertexOffset=0;
            VkBuffer vertices[2]={model.VertexBufferForDraw(i,vertexOffset),m_Extras[i]->GetBuffer()};
            VkDeviceSize offsets[2]={vertexOffset,0};
            vkCmdBindVertexBuffers(commands,0,2,vertices,offsets);
            const auto& mesh=model.GetSubMeshes()[i];
            vkCmdBindIndexBuffer(commands,mesh.indexBuffer,0,VK_INDEX_TYPE_UINT32);
            const bool doubleSided=(uint8_t(source.m_drawMode)&uint8_t(mmd::PMXDrawModeFlags::BothFace))!=0;
            auto& pipeline=materialPass?(doubleSided?m_DoubleSided:m_Surface):m_Overlay;
            if (materialPass || material.modes.z==2) {
                vkCmdBindPipeline(commands,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline.GetPipeline());
                vkCmdBindDescriptorSets(commands,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline.GetLayout(),0,1,&material.descriptors[descriptorFrame],1,&offset);
                vkCmdDrawIndexed(commands,mesh.indexCount,1,0,0,0);
            }
            if (!materialPass && (uint8_t(source.m_drawMode)&uint8_t(mmd::PMXDrawModeFlags::DrawEdge))!=0 && source.m_edgeSize>0 && source.m_edgeColor.a>0) {
                vkCmdBindPipeline(commands,VK_PIPELINE_BIND_POINT_GRAPHICS,m_Edge.GetPipeline());
                vkCmdBindDescriptorSets(commands,VK_PIPELINE_BIND_POINT_GRAPHICS,m_Edge.GetLayout(),0,1,&material.descriptors[descriptorFrame],1,&offset);
                vkCmdDrawIndexed(commands,mesh.indexCount,1,0,0,0);
            }
        }
    }
private:
    bool CreatePipelines(VkRenderPass pass,bool materialPass) {
        // Geometry and HDR passes each share formats across Scene/Game/probe views.
        if (materialPass ? (m_Surface.GetPipeline()!=VK_NULL_HANDLE && m_DoubleSided.GetPipeline()!=VK_NULL_HANDLE)
            : (m_Edge.GetPipeline()!=VK_NULL_HANDLE && m_Overlay.GetPipeline()!=VK_NULL_HANDLE)) return true;
        PipelineConfig config;
        config.vertShader="pmx.vert.spv";config.fragShader="pmx.frag.spv";
        config.vertexBindings={{0,sizeof(Vertex),VK_VERTEX_INPUT_RATE_VERTEX},{1,sizeof(glm::vec3),VK_VERTEX_INPUT_RATE_VERTEX}};
        config.vertexAttributes={{0,0,VK_FORMAT_R32G32B32_SFLOAT,uint32_t(offsetof(Vertex,Position))},
            {1,0,VK_FORMAT_R8G8B8A8_SNORM,uint32_t(offsetof(Vertex,Normal))},
            {2,0,VK_FORMAT_R16G16_SFLOAT,uint32_t(offsetof(Vertex,TexCoords))},
            {3,1,VK_FORMAT_R32G32B32_SFLOAT,0}};
        config.depthCompareOp=VK_COMPARE_OP_LESS_OR_EQUAL;
        config.colorAttachmentCount=materialPass?kMainMrtGeometryColorAttachmentCount:3;
        // Match Saba: alpha blending and depth testing/writing in PMX material order.
        config.blending=true;
        config.srcColorBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA;
        config.dstColorBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        config.srcAlphaBlendFactor=VK_BLEND_FACTOR_ONE;
        config.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        if (materialPass) {
            config.subpass=1;
            config.colorWriteMasks.assign(config.colorAttachmentCount,0);
            for (int i=0;i<4;++i) config.colorWriteMasks[i]=0xf;
            config.blending=m_IndependentBlend;
            if (m_IndependentBlend) {
                std::vector<VkBool32> blending(config.colorAttachmentCount,VK_FALSE);
                blending[0]=VK_TRUE;
                SetPipelineAttachmentBlendEnabled(&m_Surface,blending);
                SetPipelineAttachmentBlendEnabled(&m_DoubleSided,blending);
            } else {
                // Vulkan requires identical attachment states without independentBlend.
                config.colorWriteMasks.assign(config.colorAttachmentCount,0xf);
            }
            if (m_Surface.GetPipeline()==VK_NULL_HANDLE && !m_Surface.Create(pass,m_Descriptor.GetLayout(),config)) return false;
            config.cullMode=VK_CULL_MODE_NONE;
            return m_DoubleSided.Create(pass,m_Descriptor.GetLayout(),config);
        }
        config.fragShader="pmx_overlay.frag.spv";
        config.cullMode=VK_CULL_MODE_NONE;
        config.depthWrite=false;
        config.dstColorBlendFactor=VK_BLEND_FACTOR_ONE;
        config.colorWriteMasks={0xf,0,0};
        if (m_Overlay.GetPipeline()==VK_NULL_HANDLE && !m_Overlay.Create(pass,m_Descriptor.GetLayout(),config)) return false;
        config.dstColorBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        config.vertShader="pmx_edge.vert.spv";config.fragShader="pmx_edge.frag.spv";
        config.cullMode=VK_CULL_MODE_FRONT_BIT;
        config.depthWrite=false;
        // The expanded shell shares surface depth near silhouettes. Avoid
        // alternating depth rejection while retaining scene occlusion.
        config.depthCompareOp=VK_COMPARE_OP_LESS_OR_EQUAL;
        config.depthBiasEnable=true;
        config.depthBiasConstantFactor=-1.0f;
        config.depthBiasSlopeFactor=-1.0f;
        config.colorWriteMasks={VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|
            VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT,0,0};
        if (!m_Edge.Create(pass,m_Descriptor.GetLayout(),config)) return false;
        return true;
    }
    const TextureInfo* Texture(const std::string& path,size_t slot) {
        if (path.empty()) return nullptr;
        if (!g_TexturePool->GetTexture(path)) {
            std::error_code error;
            if(!std::filesystem::is_regular_file(Utf8Path(path),error)) return nullptr;
            g_TexturePool->LoadTexture2D(path,path,slot==1?SamplerType::LinearClamp:SamplerType::Linear);
        }
        return g_TexturePool->GetTexture(path);
    }
    bool DescriptorsChanged(uint32_t frame) {
        if (m_ShadowViews[frame]!=m_Shadows->GetArrayView(int(frame%2)%CascadeShadowRenderer::MAX_SLOTS)) return true;
        size_t index=0;
        for (auto& material:m_Materials) for (size_t slot=0;slot<4;++slot,++index) {
            const auto* texture=Texture(material.textures[slot],slot);
            const auto* fallback=g_TexturePool->GetTexture("white");
            const VkImageView image=texture?texture->imageView:fallback->imageView;
            if (image!=m_Views[frame][index]) return true;
        }
        return false;
    }
    void UpdateDescriptors(uint32_t frame) {
        m_Views[frame].resize(m_Materials.size()*4);
        size_t index=0;
        for (auto& material:m_Materials) {
            VkDescriptorBufferInfo buffer{m_Uniforms.GetBuffer(),0,sizeof(PmxDrawUniform)};
            std::array<VkDescriptorImageInfo,5> images{};
            std::array<VkWriteDescriptorSet,6> writes{};
            writes[0]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[0].dstSet=material.descriptors[frame];writes[0].dstBinding=0;
            writes[0].descriptorCount=1;writes[0].descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;writes[0].pBufferInfo=&buffer;
            for (size_t slot=0;slot<4;++slot,++index) {
                const auto* texture=Texture(material.textures[slot],slot);
                const auto* fallback=g_TexturePool->GetTexture("white");
                const bool loaded=texture!=nullptr;
                if (!texture) texture=fallback;
                images[slot]={g_TexturePool->GetSamplerByType(slot==1?SamplerType::LinearClamp:SamplerType::Linear),texture->imageView,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                m_Views[frame][index]=texture->imageView;
                if(slot==3) material.normalLoaded=loaded;
                else material.modes[slot==2?2:slot]=loaded?(slot==2?int(material.source.m_sphereMode):1):0;
                writes[slot+1]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                writes[slot+1].dstSet=material.descriptors[frame];writes[slot+1].dstBinding=uint32_t(slot==3?5:slot+1);
                writes[slot+1].descriptorCount=1;writes[slot+1].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;writes[slot+1].pImageInfo=&images[slot];
            }
            m_ShadowViews[frame]=m_Shadows->GetArrayView(int(frame%2)%CascadeShadowRenderer::MAX_SLOTS);
            images[4]={g_TexturePool->GetSamplerByType(SamplerType::ShadowCompare),m_ShadowViews[frame],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            writes[5]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[5].dstSet=material.descriptors[frame];writes[5].dstBinding=4;
            writes[5].descriptorCount=1;writes[5].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;writes[5].pImageInfo=&images[4];
            vkUpdateDescriptorSets(g_Device,uint32_t(writes.size()),writes.data(),0,nullptr);
        }
    }
    static constexpr uint32_t kDrawsPerFrame=2048;
    // Destruction order: pipelines before descriptor layouts and buffers.
    VulkanBuffer m_Uniforms;
    std::vector<std::unique_ptr<VulkanBuffer>> m_Extras;
    std::vector<PmxMaterial> m_Materials;
    std::array<std::vector<VkImageView>,6> m_Views;
    std::array<VkImageView,6> m_ShadowViews{};
    CascadeShadowRenderer* m_Shadows=nullptr;
    VulkanDescriptor m_Descriptor;
    VulkanPipeline m_Surface,m_DoubleSided,m_Edge,m_Overlay;
    bool m_IndependentBlend=false;
    VkDeviceSize m_Stride=0;
    std::array<uint64_t,3> m_Serial{UINT64_MAX,UINT64_MAX,UINT64_MAX};
    std::array<uint32_t,3> m_Cursor{};
    struct ViewHistory {glm::mat4 current{1},previous{1};uint64_t serial=0;bool initialized=false;};
    std::unordered_map<uint64_t,ViewHistory> m_History;
};

namespace {
std::unordered_map<ModelRenderer*,std::unique_ptr<PmxRenderer>> s_Renderers;
std::unordered_map<ModelRenderer*,std::vector<float>> s_ShadowOpacities;
}
void ReleasePmxRenderer(ModelRenderer* renderer) {s_Renderers.erase(renderer);s_ShadowOpacities.erase(renderer);}
float PmxShadowOpacity(ModelRenderer* renderer,size_t index) {
    auto it=s_ShadowOpacities.find(renderer);
    if (it==s_ShadowOpacities.end()) {
        mmd::PMXFile file;
        const auto modelPath=ProjectManager::GetInstance().ResolveAssetPath(renderer->GetModelPath());
        if (!file.Load(modelPath)) return 0;
        bool forceCastShadow=false;
        std::ifstream settings(Utf8Path(modelPath+".import.json"));
        if(settings) {
            try { forceCastShadow=nlohmann::json::parse(settings).value("forceCastShadow",false); }
            catch(const std::exception& error) { LOGW("[PMX] invalid shadow import settings: %s (%s)",modelPath.c_str(),error.what()); }
        }
        const auto& source=file.GetPMXModel().m_materials;
        std::vector<float> opacities;
        for (const auto& mesh:renderer->GetMeshData().subMeshes) {
            const auto& material=source.at(size_t(mesh.materialIndex));
            opacities.push_back(forceCastShadow || (uint8_t(material.m_drawMode)&4)!=0?std::clamp(material.m_diffuse.a,0.0f,1.0f):0.0f);
        }
        if(forceCastShadow) LOGI("[PMX] forced material shadows: %s (%zu/%zu submeshes)",renderer->GetModelPath().c_str(),
            size_t(std::count_if(opacities.begin(),opacities.end(),[](float alpha){return alpha>0;})),opacities.size());
        it=s_ShadowOpacities.emplace(renderer,std::move(opacities)).first;
    }
    return index<it->second.size()?it->second[index]:0.0f;
}

static void RenderPmxPhase(bool materialPass,SceneRenderer& scene,VkCommandBuffer commands,VkRenderPass pass,
    uint32_t width,uint32_t height,const glm::mat4& view,const glm::mat4& projection,
    const glm::vec3& direction,const glm::vec3& lightColor,const glm::vec2& jitter,uint32_t viewId,int shadowSlot) {
    if (pass==VK_NULL_HANDLE || !g_TexturePool) return;
    auto* shadows=scene.EnsureCascadeShadows();
    if (!shadows || !shadows->IsInitialized()) return;
    shadowSlot=std::clamp(shadowSlot,0,CascadeShadowRenderer::MAX_SLOTS-1);
    const auto& world=scene.GetRenderWorld();
    for (const auto& group:world.modelGroups) {
        auto* model=scene.GetModelRendererForKey(group.rendererKey);
        if (!model || !model->GetMeshData().isMmd || !model->HasModelLoaded()) continue;
        auto& renderer=s_Renderers[model];
        if (!renderer) {
            auto candidate=std::make_unique<PmxRenderer>();
            if (!candidate->Init(*model,*shadows)) {LOGE("[PMX] renderer initialization failed: %s",model->GetModelPath().c_str());continue;}
            renderer=std::move(candidate);
        }
        size_t visibleCount=0;
        for (auto entity:group.entities) {
            const auto* data=world.Find(entity);
            if (!data || (data->hasRenderFlags && !data->render.visible)) continue;
            const glm::vec4 tint=data->hasMaterial?glm::vec4(data->material.albedoColor,1):glm::vec4(1);
            const glm::vec4 pbr=data->hasMaterial?glm::vec4(data->material.metallic,data->material.roughness,
                data->material.ao,data->material.emissiveIntensity):glm::vec4(0,-1,1,0);
            renderer->Draw(*model,commands,pass,width,height,view,projection,data->transform.worldMatrix,tint,direction,lightColor,jitter,
                (uint64_t(viewId)<<32)|uint64_t(entity),shadowSlot,!data->hasRenderFlags || data->render.receiveShadow,materialPass,pbr);
            ++visibleCount;
        }
        if (materialPass && visibleCount>0 && viewId<2) {
            Rendering::RenderStats::Get().AddModelInstances(visibleCount);
            Rendering::RenderStats::Get().AddModelKinds(1);
        }
    }
}

void RenderPmxMaterials(SceneRenderer& scene,VkCommandBuffer commands,VkRenderPass pass,
    uint32_t width,uint32_t height,const glm::mat4& view,const glm::mat4& projection,
    const glm::vec3& direction,const glm::vec3& lightColor,const glm::vec2& jitter,uint32_t viewId,int shadowSlot) {
    RenderPmxPhase(true,scene,commands,pass,width,height,view,projection,direction,lightColor,jitter,viewId,shadowSlot);
}
void RenderPmxScene(SceneRenderer& scene,VkCommandBuffer commands,VkRenderPass pass,
    uint32_t width,uint32_t height,const glm::mat4& view,const glm::mat4& projection,
    const glm::vec3& direction,const glm::vec3& lightColor,const glm::vec2& jitter,uint32_t viewId,int shadowSlot) {
    RenderPmxPhase(false,scene,commands,pass,width,height,view,projection,direction,lightColor,jitter,viewId,shadowSlot);
}
