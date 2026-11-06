#include "Core/VulkanPresentation.h"
#include "Core/VulkanGpuProfiler.h"
#include "Rendering/RayTracing/VoxLod.h"
#include "RayTracingVisibilityHzb.inl"
bool RayTracingViewport::EnsureVisibilityPipelines(){
    const char* names[]={voxelDDAEnabled||softwareQuadBvhEnabled||realtimeSpecialization[16]||mikan::rt::VoxLodEnabled()?"visibility_dda.comp.spv":"visibility_raytrace.comp.spv","visibility_raster_pack.comp.spv"};
    VkPipeline* outputs[]={&visibilityRayPipeline,&visibilityPackPipeline};
    for(int i=0;i<2;++i)if(!*outputs[i]){
        auto shader=RendererUtils::CreateShaderModule(RendererUtils::ReadFile(EngineConfig::GetShaderPath(names[i])),names[i]);if(!shader)return false;
        VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};info.layout=layout;
        info.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};info.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;info.stage.module=shader;info.stage.pName="main";
        VkSpecializationMapEntry entries[27]{};for(uint32_t k=0;k<27;++k)entries[k]={k,k*4u,4u};
        VkSpecializationInfo specialization{27,entries,sizeof(realtimeSpecialization),realtimeSpecialization.data()};
        if(i==0&&(voxelDDAEnabled||softwareQuadBvhEnabled||realtimeSpecialization[16]||mikan::rt::VoxLodEnabled()))info.stage.pSpecializationInfo=&specialization;
        auto result=vkCreateComputePipelines(g_Device,VK_NULL_HANDLE,1,&info,g_Allocator,outputs[i]);vkDestroyShaderModule(g_Device,shader,g_Allocator);if(result!=VK_SUCCESS)return false;
    }return true;
}
// Raster visibility uses the RT scene's canonical geometry tables and transforms.
RayTracingViewport::Frame::~Frame(){
    if(g_Device&&visibilityFramebuffer)vkDestroyFramebuffer(g_Device,visibilityFramebuffer,g_Allocator);
}
bool RayTracingViewport::RecordRasterVisibility(VkCommandBuffer cmd,Frame& frame,const std::vector<RayTracingHitInstance>& hits,
    const glm::mat4& clipFromWorld,uint32_t width,uint32_t height){
    for(const auto& hit:hits)if(hit.voxelLodLevel){LOGE("[Visibility] raster requires exact LOD0 geometry");return false;}
    if(!visibilityRenderPass){
        VkAttachmentDescription attachments[3]{};
        const VkFormat formats[]={VK_FORMAT_R32G32B32A32_UINT,VK_FORMAT_R32G32_SFLOAT,VK_FORMAT_D32_SFLOAT};
        for(uint32_t i=0;i<3;++i){auto& a=attachments[i];a.format=formats[i];a.samples=VK_SAMPLE_COUNT_1_BIT;
            a.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;a.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
            a.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;a.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
            a.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;a.finalLayout=i==2?VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;}
        VkAttachmentReference colors[]={{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},{1,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
        VkAttachmentReference depth{2,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.colorAttachmentCount=2;sub.pColorAttachments=colors;sub.pDepthStencilAttachment=&depth;
        VkSubpassDependency deps[2]{};deps[0].srcSubpass=VK_SUBPASS_EXTERNAL;deps[0].dstSubpass=0;
        deps[0].srcStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;deps[0].dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        deps[0].srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT;deps[0].dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[1].srcSubpass=0;deps[1].dstSubpass=VK_SUBPASS_EXTERNAL;deps[1].srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].dstStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT;deps[1].srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;deps[1].dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};info.attachmentCount=3;info.pAttachments=attachments;info.subpassCount=1;info.pSubpasses=&sub;info.dependencyCount=2;info.pDependencies=deps;
        if(vkCreateRenderPass(g_Device,&info,g_Allocator,&visibilityRenderPass)!=VK_SUCCESS)return false;
    }
    static const bool useStrip=[](){const char* v=std::getenv("MIKAN_VISIBILITY_QUAD_STRIP");return !v||v[0]!='0';}();
    for(uint32_t variant=0;variant<(useStrip?2u:1u);++variant)if(!(variant?visibilityStripPipeline:visibilityRasterPipeline)){
        VkShaderModule modules[2]{};const char* names[]={variant?"visibility_buffer.vert.strip.spv":"visibility_buffer.vert.spv",variant?"visibility_buffer.frag.strip.spv":"visibility_buffer.frag.spv"};
        for(int i=0;i<2;++i)modules[i]=RendererUtils::CreateShaderModule(RendererUtils::ReadFile(EngineConfig::GetShaderPath(names[i])),names[i]);
        if(!modules[0]||!modules[1]){for(auto m:modules)if(m)vkDestroyShaderModule(g_Device,m,g_Allocator);return false;}
        VkPipelineShaderStageCreateInfo stages[2]{};
        for(int i=0;i<2;++i){stages[i]={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};stages[i].stage=i?VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_VERTEX_BIT;stages[i].module=modules[i];stages[i].pName="main";}
        VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};assembly.topology=variant?VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;assembly.primitiveRestartEnable=variant?VK_TRUE:VK_FALSE;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};viewport.viewportCount=viewport.scissorCount=1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};raster.polygonMode=VK_POLYGON_MODE_FILL;raster.cullMode=VK_CULL_MODE_NONE;raster.frontFace=VK_FRONT_FACE_CLOCKWISE;raster.lineWidth=1;
        VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};samples.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};depth.depthTestEnable=depth.depthWriteEnable=VK_TRUE;depth.depthCompareOp=VK_COMPARE_OP_LESS;
        VkPipelineColorBlendAttachmentState attachments[2]{};for(auto& a:attachments)a.colorWriteMask=15;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};blend.attachmentCount=2;blend.pAttachments=attachments;
        VkDynamicState dynamics[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dynamic.dynamicStateCount=2;dynamic.pDynamicStates=dynamics;
        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};info.stageCount=2;info.pStages=stages;info.pVertexInputState=&vertex;info.pInputAssemblyState=&assembly;info.pViewportState=&viewport;info.pRasterizationState=&raster;info.pMultisampleState=&samples;info.pDepthStencilState=&depth;info.pColorBlendState=&blend;info.pDynamicState=&dynamic;info.layout=layout;info.renderPass=visibilityRenderPass;
        auto result=vkCreateGraphicsPipelines(g_Device,VK_NULL_HANDLE,1,&info,g_Allocator,variant?&visibilityStripPipeline:&visibilityRasterPipeline);
        for(auto m:modules)vkDestroyShaderModule(g_Device,m,g_Allocator);if(result!=VK_SUCCESS)return false;
    }
    if(frame.visibilityRasterWidth!=width||frame.visibilityRasterHeight!=height){
        if(frame.visibilityFramebuffer)vkDestroyFramebuffer(g_Device,frame.visibilityFramebuffer,g_Allocator);frame.visibilityFramebuffer=VK_NULL_HANDLE;
        frame.visibilityHzb.reset();frame.visibilityHzbValid=false;
        frame.visibilityIdentity.Cleanup();frame.visibilityBary.Cleanup();frame.visibilityDepth.Cleanup();
        const auto colorUsage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if(!frame.visibilityIdentity.Create(width,height,VK_FORMAT_R32G32B32A32_UINT,VK_IMAGE_TILING_OPTIMAL,colorUsage,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)||!frame.visibilityIdentity.CreateView(VK_FORMAT_R32G32B32A32_UINT,VK_IMAGE_ASPECT_COLOR_BIT)||
           !frame.visibilityBary.Create(width,height,VK_FORMAT_R32G32_SFLOAT,VK_IMAGE_TILING_OPTIMAL,colorUsage,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)||!frame.visibilityBary.CreateView(VK_FORMAT_R32G32_SFLOAT,VK_IMAGE_ASPECT_COLOR_BIT)||
           !frame.visibilityDepth.Create(width,height,VK_FORMAT_D32_SFLOAT,VK_IMAGE_TILING_OPTIMAL,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)||!frame.visibilityDepth.CreateView(VK_FORMAT_D32_SFLOAT,VK_IMAGE_ASPECT_DEPTH_BIT))return false;
        VkImageView views[]={frame.visibilityIdentity.GetView(),frame.visibilityBary.GetView(),frame.visibilityDepth.GetView()};
        VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};info.renderPass=visibilityRenderPass;info.attachmentCount=3;info.pAttachments=views;info.width=width;info.height=height;info.layers=1;
        if(vkCreateFramebuffer(g_Device,&info,g_Allocator,&frame.visibilityFramebuffer)!=VK_SUCCESS)return false;
        frame.visibilityRasterWidth=width;frame.visibilityRasterHeight=height;
    }
    std::vector<VkDrawIndirectCommand> commands;
    const char* cullOption=std::getenv("MIKAN_VISIBILITY_CULL");
    const bool cull=!cullOption||cullOption[0]!='0';
    const char* faceOption=std::getenv("MIKAN_VISIBILITY_BACKFACE_CULL");
    const bool faceCull=cull&&(!faceOption||faceOption[0]!='0');
    const glm::mat4 worldFromClip=glm::inverse(clipFromWorld);
    glm::vec4 camera=worldFromClip*glm::vec4(0,0,-1,0); // replaced below with perspective eye
    // For a perspective matrix, inverse-VP times (0,0,1,0) gives the eye.
    camera=worldFromClip*glm::vec4(0,0,1,0);
    const bool perspective=std::abs(camera.w)>1e-6f;
    if(perspective)camera/=camera.w;
    uint64_t inputVertices=0;uint32_t frustumRejected=0,faceRejected=0;

    const auto& tables=*frame.sceneInputs->tables;
    for(uint32_t i=0;i<hits.size();++i){const auto& hit=hits[i];const uint32_t base=i*12;
        if(!(hit.rayMask&13u))continue;
        if(base+7>=tables.cachedInstances.size())return false;
        const uint32_t type=tables.cachedInstances[base+6];
        if(type==1u){const auto model=tables.cachedInstances[base+4];if(model*12u+5u>=tables.cachedModels.size())return false;
            commands.push_back({tables.cachedModels[model*12+5],1,0,i*256u});
        }else if(hit.geometry&&type==0u){
            const auto* source=hit.geometry->Source();
            const glm::vec3 minimum=source->GetMinBounds(),maximum=source->GetMaxBounds();
            if(cull){
                uint32_t common=63u;
                for(uint32_t corner=0;corner<8;++corner){
                    const glm::vec3 v((corner&1)?maximum.x:minimum.x,(corner&2)?maximum.y:minimum.y,(corner&4)?maximum.z:minimum.z);
                    const glm::vec4 clip=clipFromWorld*hit.model*glm::vec4(v,1);
                    uint32_t outside=(clip.x< -clip.w?1u:0u)|(clip.x>clip.w?2u:0u)|(clip.y< -clip.w?4u:0u)|(clip.y>clip.w?8u:0u)|(clip.z<0?16u:0u)|(clip.z>clip.w?32u:0u);
                    common&=outside;
                }
                if(common){++frustumRejected;continue;}
            }
            const glm::vec3 eye=glm::vec3(glm::inverse(hit.model)*camera);

            if(hit.geometry->Ranges().size()>256u)return false;
            for(uint32_t g=0;g<hit.geometry->Ranges().size();++g){
                const auto& r=hit.geometry->Ranges()[g];inputVertices+=uint64_t(r.quadCount)*6u;
                // Reject only directions proven back-facing for the entire local AABB.
                // Keep both sides when the eye lies within its extent on that axis.
                const uint32_t axis=r.direction<2?2u:r.direction<4?0u:1u;
                const bool positive=r.direction==0u||r.direction==3u||r.direction==4u;
                if(faceCull&&perspective&&((positive&&eye[axis]<minimum[axis]-1e-4f)||(!positive&&eye[axis]>maximum[axis]+1e-4f))){++faceRejected;continue;}
                if(r.quadCount)commands.push_back({r.quadCount*6u,1,0,i*256u+g});
            }
        }else{LOGE("[Visibility] unsupported raster geometry");return false;}
    }
    if(cull&&perspective)std::stable_sort(commands.begin(),commands.end(),[&](const auto& a,const auto& b){
        const auto& x=hits[a.firstInstance/256u];const auto& y=hits[b.firstInstance/256u];
        const glm::vec3 dx=glm::vec3(x.model[3])-glm::vec3(camera),dy=glm::vec3(y.model[3])-glm::vec3(camera);
        return glm::dot(dx,dx)<glm::dot(dy,dy);
    });
    static bool reported=false;if(!reported){reported=true;uint64_t submitted=0;for(const auto& draw:commands)submitted+=useStrip&&hits[draw.firstInstance/256u].geometry?uint64_t(draw.vertexCount)*2u/3u:draw.vertexCount;
        LOGI("[Visibility raster cull] enabled=%u strip=%u instances=%u frustumRejected=%u faceRangesRejected=%u draws=%u consideredVertices=%llu submittedVertices=%llu",uint32_t(cull),uint32_t(useStrip),uint32_t(hits.size()),frustumRejected,faceRejected,uint32_t(commands.size()),(unsigned long long)inputVertices,(unsigned long long)submitted);
    }
    std::vector<VkDrawIndexedIndirectCommand> stripCommands;
    uint32_t maxQuads=0;
    if(useStrip)for(const auto& draw:commands)if(hits[draw.firstInstance/256u].geometry){
        uint32_t quads=draw.vertexCount/6u;maxQuads=std::max(maxQuads,quads);
        stripCommands.push_back({quads*5u,1,0,0,draw.firstInstance});
    }
    if(maxQuads>visibilityStripQuads){
        if(visibilityStripIndices.GetBuffer())Core::WaitForDeviceIdle(g_Device);
        visibilityStripIndices.Cleanup();
        std::vector<uint32_t> indices;indices.reserve(size_t(maxQuads)*5u);
        for(uint32_t q=0;q<maxQuads;++q)indices.insert(indices.end(),{q*4u+1u,q*4u,q*4u+2u,q*4u+3u,UINT32_MAX});
        if(!visibilityStripIndices.Create(indices.size()*4u,VK_BUFFER_USAGE_INDEX_BUFFER_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))return false;
        visibilityStripIndices.Write(indices.data(),indices.size()*4u);visibilityStripQuads=maxQuads;
    }
    VkPhysicalDeviceFeatures features{};vkGetPhysicalDeviceFeatures(g_PhysicalDevice,&features);
    VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(g_PhysicalDevice,&properties);
    const bool indirect=features.drawIndirectFirstInstance&&commands.size()*sizeof(VkDrawIndirectCommand)<=1024u*1024u;
    const VkDeviceSize commandOffset=VkDeviceSize(width)*height*24u;
    if(indirect&&!useStrip)for(size_t first=0;first<commands.size();first+=4096u){
        size_t count=std::min(size_t(4096),commands.size()-first);
        vkCmdUpdateBuffer(cmd,frame.visibilityRaw.GetBuffer(),commandOffset+first*sizeof(VkDrawIndirectCommand),count*sizeof(VkDrawIndirectCommand),commands.data()+first);
    }
    if(useStrip)for(size_t first=0;first<stripCommands.size();first+=3000u){
        size_t count=std::min(size_t(3000),stripCommands.size()-first);
        if((first+count)*sizeof(VkDrawIndexedIndirectCommand)>1024u*1024u)return false;
        vkCmdUpdateBuffer(cmd,frame.visibilityRaw.GetBuffer(),commandOffset+first*sizeof(VkDrawIndexedIndirectCommand),count*sizeof(VkDrawIndexedIndirectCommand),stripCommands.data()+first);
    }
    VkMemoryBarrier ready{VK_STRUCTURE_TYPE_MEMORY_BARRIER};ready.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;ready.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_INDIRECT_COMMAND_READ_BIT|VK_ACCESS_INDEX_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_HOST_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT|VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,0,1,&ready,0,nullptr,0,nullptr);
    if((useStrip?features.drawIndirectFirstInstance:indirect)&&!CullVisibilityHzb(cmd,frame,hits,commands,stripCommands,useStrip,width,height))return false;
    VkClearValue clear[3]{};clear[2].depthStencil={1,0};
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};begin.renderPass=visibilityRenderPass;begin.framebuffer=frame.visibilityFramebuffer;begin.renderArea.extent={width,height};begin.clearValueCount=3;begin.pClearValues=clear;
    { Core::VulkanGpuScope drawTiming(cmd,"visibility.raster_draw");
    vkCmdBeginRenderPass(cmd,&begin,VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,visibilityRasterPipeline);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&frame.descriptor,0,nullptr);
    VkViewport viewport{0,0,float(width),float(height),0,1};VkRect2D scissor{{0,0},{width,height}};vkCmdSetViewport(cmd,0,1,&viewport);vkCmdSetScissor(cmd,0,1,&scissor);
    struct Push {glm::mat4 clip;glm::vec4 eye;glm::uvec4 draw;glm::vec4 light,extent;} push{clipFromWorld,{}, {}, {},glm::vec4(width,height,0,0)};
    if(useStrip){
        if(!stripCommands.empty()){
            vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,visibilityStripPipeline);
            vkCmdBindIndexBuffer(cmd,visibilityStripIndices.GetBuffer(),0,VK_INDEX_TYPE_UINT32);
            push.draw.z=1u;vkCmdPushConstants(cmd,layout,VK_SHADER_STAGE_ALL_GRAPHICS|VK_SHADER_STAGE_COMPUTE_BIT,0,128,&push);
            uint32_t batch=features.multiDrawIndirect?std::max(1u,properties.limits.maxDrawIndirectCount):1u;
            if(features.drawIndirectFirstInstance)for(uint32_t first=0;first<stripCommands.size();){uint32_t count=std::min(batch,uint32_t(stripCommands.size())-first);
                vkCmdDrawIndexedIndirect(cmd,frame.visibilityRaw.GetBuffer(),commandOffset+VkDeviceSize(first)*sizeof(VkDrawIndexedIndirectCommand),count,sizeof(VkDrawIndexedIndirectCommand));first+=count;}
            else for(const auto& draw:stripCommands){push.draw={draw.firstInstance/256u,draw.firstInstance%256u,0u,0u};vkCmdPushConstants(cmd,layout,VK_SHADER_STAGE_ALL_GRAPHICS|VK_SHADER_STAGE_COMPUTE_BIT,0,128,&push);vkCmdDrawIndexed(cmd,draw.indexCount,1,0,0,0);}
        }
        vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,visibilityRasterPipeline);
        for(const auto& draw:commands)if(!hits[draw.firstInstance/256u].geometry){push.draw={draw.firstInstance/256u,draw.firstInstance%256u,0u,0u};vkCmdPushConstants(cmd,layout,VK_SHADER_STAGE_ALL_GRAPHICS|VK_SHADER_STAGE_COMPUTE_BIT,0,128,&push);vkCmdDraw(cmd,draw.vertexCount,1,0,0);}
    }else if(indirect){
        push.draw.z=1u;vkCmdPushConstants(cmd,layout,VK_SHADER_STAGE_ALL_GRAPHICS|VK_SHADER_STAGE_COMPUTE_BIT,0,128,&push);
        uint32_t batch=features.multiDrawIndirect?std::max(1u,properties.limits.maxDrawIndirectCount):1u;
        for(uint32_t first=0;first<commands.size();){uint32_t count=std::min(batch,uint32_t(commands.size())-first);
            vkCmdDrawIndirect(cmd,frame.visibilityRaw.GetBuffer(),commandOffset+VkDeviceSize(first)*sizeof(VkDrawIndirectCommand),count,sizeof(VkDrawIndirectCommand));first+=count;}
    }else for(const auto& draw:commands){
        push.draw={draw.firstInstance/256u,draw.firstInstance%256u,0u,0u};
        vkCmdPushConstants(cmd,layout,VK_SHADER_STAGE_ALL_GRAPHICS|VK_SHADER_STAGE_COMPUTE_BIT,0,128,&push);vkCmdDraw(cmd,draw.vertexCount,1,0,0);
    }
    vkCmdEndRenderPass(cmd); }
    const char* hzbOption=std::getenv("MIKAN_VISIBILITY_HZB");
    if(hzbOption&&hzbOption[0]=='1'){
        Core::VulkanGpuScope hzbTiming(cmd,"visibility.hzb_build");
        if(!frame.visibilityHzb){frame.visibilityHzb=std::make_unique<HiZComputeShader>();if(!frame.visibilityHzb->Init(g_Device,g_PhysicalDevice,width,height))return false;}
        frame.visibilityHzb->GenerateMipLevelsFromDepth(cmd,frame.visibilityDepth.GetImage(),frame.visibilityDepth.GetView(),frame.visibilityHzb->GetSampler());
        frame.visibilityHzb->SwapBuffers();
        frame.visibilityHzbStable=frame.visibilityViewProjection;frame.visibilityHzbClip=clipFromWorld;
        frame.visibilityHzbRevision=HiZHistory::OccluderRevision(g_SceneRenderer.GetRenderWorld())^frame.sceneInputs->version;
        frame.visibilityHzbValid=true;
    }else frame.visibilityHzbValid=false;
    Core::VulkanGpuScope copyTiming(cmd,"visibility.raster_copy");
    // Pack the two raster attachments into a planar staging buffer; the common
    // compute kernel converts it to the same 24-byte PrimaryHit ABI as RT.
    const VkDeviceSize pixels=VkDeviceSize(width)*height;
    VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={width,height,1};
    vkCmdCopyImageToBuffer(cmd,frame.visibilityIdentity.GetImage(),VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,frame.visibilityRaw.GetBuffer(),1,&copy);
    copy.bufferOffset=pixels*16;vkCmdCopyImageToBuffer(cmd,frame.visibilityBary.GetImage(),VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,frame.visibilityRaw.GetBuffer(),1,&copy);
    ready.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;ready.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&ready,0,nullptr,0,nullptr);
    return true;
}
