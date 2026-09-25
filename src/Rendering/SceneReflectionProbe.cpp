#include "Rendering/SceneReflectionProbe.h"

#include "Core/EngineGlobal.h"
#include "Core/EngineConfig.h"
#include "Core/VulkanContext.h"
#include "Core/Log.h"
#include "Rendering/TexturePool.h"   // SamplerType::NearestClamp（解析 pass 的深度采样）

#include <glm/gtc/matrix_transform.hpp>

#include <cstdlib>   // std::getenv / std::strtol（降频面数 env）

namespace {

uint32_t FindDeviceLocalMemoryType(uint32_t typeFilter)
{
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(g_PhysicalDevice, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((typeFilter & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            return i;
        }
    }
    return VK_MAX_MEMORY_TYPES;
}

// 标准 cubemap 面朝向：forward / up。与 PointShadowRenderer 同一组，
// 保证与 atmo_*.comp 里 FaceDir() 的硬件 cube 约定一致（相邻面共享边方向相同）。
const glm::vec3 kFaceForward[SceneReflectionProbe::kFaceCount] = {
    glm::vec3(1, 0, 0), glm::vec3(-1, 0, 0),
    glm::vec3(0, 1, 0), glm::vec3(0, -1, 0),
    glm::vec3(0, 0, 1), glm::vec3(0, 0, -1),
};
const glm::vec3 kFaceUp[SceneReflectionProbe::kFaceCount] = {
    glm::vec3(0, -1, 0), glm::vec3(0, -1, 0),
    glm::vec3(0, 0, 1), glm::vec3(0, 0, -1),
    glm::vec3(0, -1, 0), glm::vec3(0, -1, 0),
};

}  // namespace

SceneReflectionProbe::~SceneReflectionProbe()
{
    Cleanup();
}

void SceneReflectionProbe::Cleanup()
{
    m_CompositeQuad.Cleanup();
    m_Target.Cleanup();

    if (g_Device == VK_NULL_HANDLE) {
        m_Initialized = false;
        return;
    }
    m_ResolvePipeline.Cleanup();
    if (m_ResolvePool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(g_Device, m_ResolvePool, g_Allocator);
        m_ResolvePool = VK_NULL_HANDLE;
    }
    m_ResolveSet = VK_NULL_HANDLE;
    if (m_ResolveSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(g_Device, m_ResolveSetLayout, g_Allocator);
        m_ResolveSetLayout = VK_NULL_HANDLE;
    }
    m_CachedCompositeView = VK_NULL_HANDLE;
    m_CachedCompositeSampler = VK_NULL_HANDLE;
    m_CachedResolveDepthView = VK_NULL_HANDLE;

    for (auto& fb : m_FaceFramebuffers) {
        if (fb != VK_NULL_HANDLE) vkDestroyFramebuffer(g_Device, fb, g_Allocator);
        fb = VK_NULL_HANDLE;
    }
    for (auto& v : m_FaceColorViews) {
        if (v != VK_NULL_HANDLE) vkDestroyImageView(g_Device, v, g_Allocator);
        v = VK_NULL_HANDLE;
    }
    if (m_CubeView != VK_NULL_HANDLE) vkDestroyImageView(g_Device, m_CubeView, g_Allocator);
    m_CubeView = VK_NULL_HANDLE;
    if (m_RenderPass != VK_NULL_HANDLE) vkDestroyRenderPass(g_Device, m_RenderPass, g_Allocator);
    m_RenderPass = VK_NULL_HANDLE;
    if (m_Sampler != VK_NULL_HANDLE) vkDestroySampler(g_Device, m_Sampler, g_Allocator);
    m_Sampler = VK_NULL_HANDLE;
    if (m_ColorImage != VK_NULL_HANDLE) vkDestroyImage(g_Device, m_ColorImage, g_Allocator);
    m_ColorImage = VK_NULL_HANDLE;
    if (m_ColorMemory != VK_NULL_HANDLE) vkFreeMemory(g_Device, m_ColorMemory, g_Allocator);
    m_ColorMemory = VK_NULL_HANDLE;
    m_PrimedLayers = 0;
    m_FaceWarmupPending = true;
    m_NextFace = 0;
    m_HasCapturePosition = false;
    m_LastCapturePosition = glm::vec3(0.0f);
    m_Initialized = false;
}

bool SceneReflectionProbe::CreateImages()
{
    VkImageCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.extent = { m_FaceSize, m_FaceSize, 1 };
    ci.mipLevels = 1;
    ci.arrayLayers = kFaceCount;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // CUBE_COMPATIBLE：6 层必须能被一张 samplerCube 覆盖。
    ci.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

    ci.format = kColorFormat;
    // TRANSFER_DST：PrimeColorLayout() 用它把 6 层清成 (0,0,0,0)（覆盖掩码全 0）。
    ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (vkCreateImage(g_Device, &ci, g_Allocator, &m_ColorImage) != VK_SUCCESS) {
        LOGE("[SceneProbe] color cube image create failed (%ux%u x6)", m_FaceSize, m_FaceSize);
        return false;
    }
    VkMemoryRequirements colorReq;
    vkGetImageMemoryRequirements(g_Device, m_ColorImage, &colorReq);
    const uint32_t colorType = FindDeviceLocalMemoryType(colorReq.memoryTypeBits);
    if (colorType == VK_MAX_MEMORY_TYPES) {
        LOGE("[SceneProbe] no device-local memory type for color cube");
        return false;
    }
    VkMemoryAllocateInfo colorAlloc = {};
    colorAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    colorAlloc.allocationSize = colorReq.size;
    colorAlloc.memoryTypeIndex = colorType;
    if (vkAllocateMemory(g_Device, &colorAlloc, g_Allocator, &m_ColorMemory) != VK_SUCCESS) {
        LOGE("[SceneProbe] color cube memory alloc failed");
        return false;
    }
    vkBindImageMemory(g_Device, m_ColorImage, m_ColorMemory, 0);
    return true;
}

bool SceneReflectionProbe::CreateRenderPass()
{
    // 只剩颜色附件：几何不再写进 cube 面（几何走探针自己的 RenderTarget），
    // 面只被 probe_resolve.frag 的全屏三角整体覆盖，因此不需要深度/模板。
    // loadOp=CLEAR 且清屏 alpha=0，即便解析失败也能保证 alpha=0（采样端回退天空）。
    VkAttachmentDescription colorAttachment = {};
    colorAttachment.format = kColorFormat;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef = {};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    VkRenderPassCreateInfo rpInfo = {};
    rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = 1;
    rpInfo.pAttachments = &colorAttachment;
    rpInfo.subpassCount = 1;
    rpInfo.pSubpasses = &subpass;

    if (vkCreateRenderPass(g_Device, &rpInfo, g_Allocator, &m_RenderPass) != VK_SUCCESS) {
        LOGE("[SceneProbe] render pass create failed");
        return false;
    }
    return true;
}

bool SceneReflectionProbe::CreateSampler()
{
    VkSamplerCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = VK_FILTER_LINEAR;
    info.minFilter = VK_FILTER_LINEAR;
    // 探针没有 mip 链（粗糙度过滤由 skyCube 的 GGX 预滤波承接），
    // 因此 MIPMAP_MODE_NEAREST + maxLod=0 就够，避免采样端误取未定义的 mip。
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.maxLod = 0.0f;
    if (vkCreateSampler(g_Device, &info, g_Allocator, &m_Sampler) != VK_SUCCESS) {
        LOGE("[SceneProbe] sampler create failed");
        return false;
    }
    return true;
}

bool SceneReflectionProbe::CreateFaceViewsAndFramebuffers()
{
    // 采样用 CUBE 视图（单 mip、6 层）。
    VkImageViewCreateInfo cubeView = {};
    cubeView.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    cubeView.image = m_ColorImage;
    cubeView.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    cubeView.format = kColorFormat;
    cubeView.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    cubeView.subresourceRange.baseMipLevel = 0;
    cubeView.subresourceRange.levelCount = 1;
    cubeView.subresourceRange.baseArrayLayer = 0;
    cubeView.subresourceRange.layerCount = kFaceCount;
    if (vkCreateImageView(g_Device, &cubeView, g_Allocator, &m_CubeView) != VK_SUCCESS) {
        LOGE("[SceneProbe] cube view create failed");
        return false;
    }

    for (uint32_t face = 0; face < kFaceCount; ++face) {
        VkImageViewCreateInfo colorView = {};
        colorView.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        colorView.image = m_ColorImage;
        colorView.viewType = VK_IMAGE_VIEW_TYPE_2D;
        colorView.format = kColorFormat;
        colorView.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        colorView.subresourceRange.levelCount = 1;
        colorView.subresourceRange.baseArrayLayer = face;
        colorView.subresourceRange.layerCount = 1;
        if (vkCreateImageView(g_Device, &colorView, g_Allocator, &m_FaceColorViews[face]) != VK_SUCCESS) {
            LOGE("[SceneProbe] face %u color view create failed", face);
            return false;
        }

        VkFramebufferCreateInfo fbInfo = {};
        fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass = m_RenderPass;
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments = &m_FaceColorViews[face];
        fbInfo.width = m_FaceSize;
        fbInfo.height = m_FaceSize;
        fbInfo.layers = 1;
        if (vkCreateFramebuffer(g_Device, &fbInfo, g_Allocator, &m_FaceFramebuffers[face]) != VK_SUCCESS) {
            LOGE("[SceneProbe] face %u framebuffer create failed", face);
            return false;
        }
    }
    return true;
}

bool SceneReflectionProbe::CreateResolvePipeline()
{
    // binding 0 = 探针视图合成附件（颜色）；binding 1 = 探针视图深度附件。
    // 深度是「该像素是不是天空」的唯一判据 —— 探针合成 shader 的天空分支只采
    // 纯大气 skyRT（无云纹理绑定），必须靠深度把天空方向标出来让水面回退 skyCube。
    VkDescriptorSetLayoutBinding bindings[2] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(g_Device, &layoutInfo, g_Allocator, &m_ResolveSetLayout) != VK_SUCCESS) {
        LOGE("[SceneProbe] resolve descriptor layout create failed");
        return false;
    }

    VkDescriptorPoolSize poolSize = {};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 2;
    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(g_Device, &poolInfo, g_Allocator, &m_ResolvePool) != VK_SUCCESS) {
        LOGE("[SceneProbe] resolve descriptor pool create failed");
        return false;
    }

    VkDescriptorSetAllocateInfo setInfo = {};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setInfo.descriptorPool = m_ResolvePool;
    setInfo.descriptorSetCount = 1;
    setInfo.pSetLayouts = &m_ResolveSetLayout;
    if (vkAllocateDescriptorSets(g_Device, &setInfo, &m_ResolveSet) != VK_SUCCESS) {
        LOGE("[SceneProbe] resolve descriptor set alloc failed");
        return false;
    }

    PipelineConfig config;
    config.vertShader = "fullscreen.vert.spv";       // 复用全屏三角（输出 fragTexCoord）
    config.fragShader = "probe_resolve.frag.spv";
    config.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    config.cullMode = VK_CULL_MODE_NONE;             // 全屏三角的绕序无关紧要，不做剔除
    config.depthTest = false;                        // render pass 无深度附件
    config.depthWrite = false;
    config.blending = false;
    config.colorAttachmentCount = 1;
    config.subpass = 0;
    if (!m_ResolvePipeline.Create(m_RenderPass, m_ResolveSetLayout, config)) {
        LOGE("[SceneProbe] resolve pipeline create failed");
        return false;
    }
    return true;
}

bool SceneReflectionProbe::PrimeColorLayout()
{
    if (g_Device == VK_NULL_HANDLE || g_CommandPool == VK_NULL_HANDLE ||
        g_Queue == VK_NULL_HANDLE || m_ColorImage == VK_NULL_HANDLE) {
        return false;
    }

    VkCommandBufferAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = g_CommandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(g_Device, &allocInfo, &cmd) != VK_SUCCESS) {
        return false;
    }

    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    VkImageSubresourceRange range = {};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = 1;
    range.layerCount = kFaceCount;

    // UNDEFINED → TRANSFER_DST：内容丢弃（本就要清），只需目标布局与写访问。
    VkImageMemoryBarrier toDst = {};
    toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.image = m_ColorImage;
    toDst.subresourceRange = range;
    toDst.srcAccessMask = 0;
    toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);

    // 清成 (0,0,0,0)：alpha=0 ⇒ 采样端 mix(天空, 探针, alpha) 全部回退天空。
    VkClearColorValue clearColor = {};
    clearColor.float32[0] = 0.0f;
    clearColor.float32[1] = 0.0f;
    clearColor.float32[2] = 0.0f;
    clearColor.float32[3] = 0.0f;
    vkCmdClearColorImage(cmd, m_ColorImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &clearColor, 1, &range);

    // TRANSFER_DST → SHADER_READ_ONLY：与 render pass 声明的 initialLayout 对齐，
    // 也让首帧的水面采样合法（后续帧由 ResolveFace/RecordShaderReadBarrier 往返）。
    VkImageMemoryBarrier toRead = {};
    toRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toRead.image = m_ColorImage;
    toRead.subresourceRange = range;
    toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toRead);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    const bool submitted = vkQueueSubmit(g_Queue, 1, &submitInfo, VK_NULL_HANDLE) == VK_SUCCESS;
    if (submitted) {
        vkQueueWaitIdle(g_Queue);
    }
    vkFreeCommandBuffers(g_Device, g_CommandPool, 1, &cmd);

    m_PrimedLayers = submitted ? ((1u << kFaceCount) - 1u) : 0u;
    return submitted;
}

bool SceneReflectionProbe::Init(uint32_t faceSize)
{
    if (m_Initialized) return true;
    if (g_Device == VK_NULL_HANDLE) return false;
    m_FaceSize = faceSize > 0 ? faceSize : kDefaultFaceSize;
    // env 覆盖（与降频面数 MIKAN_SCENE_PROBE_FACES_PER_FRAME 同族门控）：
    // MIKAN_SCENE_PROBE_FACE_SIZE=128/256/512 … 直接 A/B 对照分辨率对清晰度与
    // 捕获成本的影响，不必重编。面尺寸是**唯一**的自由参数 —— cubemap image、
    // 探针离屏目标、resolve 的 renderArea/viewport/scissor、cluster 剔除的
    // 视口尺寸全部由 m_FaceSize 派生，改这一处即全链跟随。
    if (const char* envFaceSize = std::getenv("MIKAN_SCENE_PROBE_FACE_SIZE")) {
        const long envValue = std::strtol(envFaceSize, nullptr, 10);
        if (envValue > 0 && envValue <= 4096) {
            m_FaceSize = static_cast<uint32_t>(envValue);
        }
    }

    if (!CreateImages() || !CreateRenderPass() || !CreateSampler() ||
        !CreateFaceViewsAndFramebuffers() || !CreateResolvePipeline()) {
        Cleanup();
        return false;
    }

    // 探针视图的离屏目标：与 g_SceneRenderTarget / g_GameRenderTarget 完全同规格
    // （MRT=true ⇒ 独立合成 pass），只是尺寸取探针面尺寸。共享的几何管线按其
    // render pass 兼容（附件格式一致）即可直接使用。
    m_Target.Init(m_FaceSize, m_FaceSize, true);
    if (m_Target.GetCompositeRenderPass() == VK_NULL_HANDLE) {
        LOGE("[SceneProbe] probe view render target init failed");
        Cleanup();
        return false;
    }
    // 合成 quad 的管线绑定在 target 自己的 composite render pass 上，
    // 因此必须每个 target 一个实例，不能与 g_SceneCompositeQuad 共用。
    m_CompositeQuad.Init(m_Target.GetCompositeRenderPass(), 0, MIKAN_COMPOSITE_SHADER);

    // 清屏 + 首帧布局。失败不当作致命错误：ResolveFace 会用 UNDEFINED 兜底
    // （只是首帧水面会读到垃圾 alpha，功能仍可用）。
    if (!PrimeColorLayout()) {
        LOGW("[SceneProbe] color layout priming skipped (no command pool/queue)");
    }
    m_Initialized = true;
    LOGI("[SceneProbe] probe view ready: %u² cube (RGBA16F) + %u² MRT view target",
         m_FaceSize, m_FaceSize);
    return true;
}

void SceneReflectionProbe::ComputeFaceViewProjs(const glm::vec3& capturePosition, float farPlane,
                                                std::array<glm::mat4, kFaceCount>& outViews,
                                                std::array<glm::mat4, kFaceCount>& outProjs)
{
    // Keep every probe face within the fixed capture budget even if a future
    // caller supplies a larger far plane than the probe default.
    const float requestedFar = farPlane > kNearPlane ? farPlane : kFarPlane;
    const float farP = requestedFar < kFarPlane ? requestedFar : kFarPlane;
    glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, kNearPlane, farP);
    // 与主场景 Vulkan 相机保持同一 clip-space 约定。探针地形管线启用了
    // back-face culling；不翻 Y 会把投影后的绕序反转，导致不同 cubemap
    // 面按面方向出现前后不一致的剔除结果。
    proj[1][1] *= -1.0f;
    for (uint32_t f = 0; f < kFaceCount; ++f) {
        outViews[f] = glm::lookAt(capturePosition,
                                  capturePosition + kFaceForward[f],
                                  kFaceUp[f]);
        outProjs[f] = proj;
    }
}

void SceneReflectionProbe::ResolveFace(VkCommandBuffer commandBuffer, uint32_t face,
                                       RenderTarget& source)
{
    if (!m_Initialized || commandBuffer == VK_NULL_HANDLE || face >= kFaceCount ||
        m_FaceFramebuffers[face] == VK_NULL_HANDLE ||
        m_ResolveSet == VK_NULL_HANDLE || m_ResolvePipeline.GetPipeline() == VK_NULL_HANDLE) {
        return;
    }
    const VkImageView compositeView = source.GetCompositeImageView();
    VkImage compositeImage = source.GetCompositeImage();
    if (compositeView == VK_NULL_HANDLE || compositeImage == VK_NULL_HANDLE) {
        return;
    }
    const VkSampler compositeSampler = source.GetSampler();

    // 深度附件：探针 6 面共用这张深度图（每面 render pass 内 CLEAR），而 ResolveFace
    // 紧跟本面 EndRender 调用 ⇒ 读到的正是本面的深度。布局已是
    // DEPTH_STENCIL_READ_ONLY_OPTIMAL（见 RenderTargetRenderPasses.inl 的
    // depthAttachment.finalLayout），但 render pass 声明的是 initialLayout=UNDEFINED，
    // 不产生跨 pass 的隐式依赖，所以这里补一次显式的「深度写 → shader 读」同步。
    const VkImageView depthView = source.GetDepthImageView();
    const VkImage depthImage = source.GetDepthImage();
    if (depthView == VK_NULL_HANDLE || depthImage == VK_NULL_HANDLE) {
        return;
    }
    // 深度采样必须 NEAREST：线性过滤深度值没有物理意义，部分移动 GPU 上对深度图
    // 做 LINEAR 采样还会返回垃圾。与 FullscreenQuad 的深度路径同一选择。
    const VkSampler depthSampler = g_TexturePool != nullptr
        ? g_TexturePool->GetSamplerByType(SamplerType::NearestClamp)
        : m_Sampler;

    VkImageMemoryBarrier depthRwBarrier = {};
    depthRwBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    depthRwBarrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    depthRwBarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    depthRwBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    depthRwBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    depthRwBarrier.image = depthImage;
    depthRwBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    depthRwBarrier.subresourceRange.levelCount = 1;
    depthRwBarrier.subresourceRange.layerCount = 1;
    depthRwBarrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    depthRwBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer,
                         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                             VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &depthRwBarrier);

    // 本层：从「该层上一次解析结尾的 SHADER_READ_ONLY」（从未写过时是 UNDEFINED）
    // 转进附件布局，同时建立「上一次水面采样 → 本次解析写」的 WAR 依赖。
    // **必须按层**：降频轮转下每帧只解析 1 个面，其余 5 层的布局由它们各自的上一次
    // 解析决定，不能被本层带着一起转（原来的 face==0 整图转换只适配「每帧全量」）。
    const bool layerPrimed = (m_PrimedLayers & (1u << face)) != 0;
    {
        VkImageMemoryBarrier barrier = {};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = layerPrimed ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                        : VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = m_ColorImage;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseArrayLayer = face;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = layerPrimed ? VK_ACCESS_SHADER_READ_BIT : 0;
        barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(commandBuffer,
                             layerPrimed
                                 ? (VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)
                                 : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
        m_PrimedLayers |= (1u << face);
    }

    // 合成附件：合成 pass 的 finalLayout 就是 COLOR_ATTACHMENT_OPTIMAL
    // （见 RenderTargetRenderPasses.inl），转 SHADER_READ_ONLY 供本次采样。
    VkImageMemoryBarrier compositeBarrier = {};
    compositeBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    compositeBarrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    compositeBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    compositeBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    compositeBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    compositeBarrier.image = compositeImage;
    compositeBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    compositeBarrier.subresourceRange.levelCount = 1;
    compositeBarrier.subresourceRange.layerCount = 1;
    compositeBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    compositeBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &compositeBarrier);

    const bool compositeChanged =
        m_CachedCompositeView != compositeView || m_CachedCompositeSampler != compositeSampler;
    const bool depthChanged = m_CachedResolveDepthView != depthView;
    if (compositeChanged || depthChanged) {
        std::array<VkDescriptorImageInfo, 2> imageInfos{};
        imageInfos[0].imageView = compositeView;
        imageInfos[0].sampler = compositeSampler;
        imageInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfos[1].imageView = depthView;
        imageInfos[1].sampler = depthSampler;
        imageInfos[1].imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        std::array<VkWriteDescriptorSet, 2> writes{};
        for (uint32_t i = 0; i < writes.size(); ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = m_ResolveSet;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo = &imageInfos[i];
        }
        vkUpdateDescriptorSets(g_Device, static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
        m_CachedCompositeView = compositeView;
        m_CachedCompositeSampler = compositeSampler;
        m_CachedResolveDepthView = depthView;
    }

    VkClearValue clear = {};
    clear.color = { {0.0f, 0.0f, 0.0f, 0.0f} };
    VkRenderPassBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    beginInfo.renderPass = m_RenderPass;
    beginInfo.framebuffer = m_FaceFramebuffers[face];
    beginInfo.renderArea = { {0, 0}, {m_FaceSize, m_FaceSize} };
    beginInfo.clearValueCount = 1;
    beginInfo.pClearValues = &clear;
    vkCmdBeginRenderPass(commandBuffer, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport = {};
    viewport.width = static_cast<float>(m_FaceSize);
    viewport.height = static_cast<float>(m_FaceSize);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    VkRect2D scissor = {};
    scissor.extent = { m_FaceSize, m_FaceSize };
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ResolvePipeline.GetPipeline());
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_ResolvePipeline.GetLayout(), 0, 1, &m_ResolveSet, 0, nullptr);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);

    vkCmdEndRenderPass(commandBuffer);

    // WAR：本面的 resolve 已经读过深度，下一个面（或下一帧）的 render pass 会
    // CLEAR 并重写它。收口这次 shader 读，避免与后续深度写竞争。
    VkImageMemoryBarrier depthWarBarrier = depthRwBarrier;
    depthWarBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    depthWarBarrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(commandBuffer,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                             VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &depthWarBarrier);
}

void SceneReflectionProbe::RecordShaderReadBarrier(VkCommandBuffer commandBuffer, uint32_t face)
{
    if (!m_Initialized || commandBuffer == VK_NULL_HANDLE || face >= kFaceCount) return;
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = m_ColorImage;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseArrayLayer = face;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
}

uint32_t SceneReflectionProbe::ResolveFacesPerFrame()
{
    // 只解析一次：env 在进程生命周期内不变，不必每帧 getenv。
    static const uint32_t s_facesPerFrame = [] {
        uint32_t n = 1;   // 默认每帧 1 面（6 帧一轮）
        if (const char* env = std::getenv("MIKAN_SCENE_PROBE_FACES_PER_FRAME")) {
            const long parsed = std::strtol(env, nullptr, 10);
            if (parsed > 0) {
                n = static_cast<uint32_t>(parsed);
            }
        }
        if (n < 1) n = 1;
        if (n > kFaceCount) n = kFaceCount;
        return n;
    }();
    return s_facesPerFrame;
}

uint32_t SceneReflectionProbe::AcquireFrameFaces(const glm::vec3& capturePosition,
                                                 uint32_t* outFaces)
{
    if (outFaces == nullptr) {
        return 0;
    }
    // 首帧、或捕获点发生变化时必须 6 面全渲：否则未解析
    // 的那几层会长时间停在 PrimeColorLayout 的 alpha=0 状态，水面对应方向完全没有
    // 探针内容；更重要的是不能把不同捕获中心的六面结果混在一张 cube 里。
    constexpr float kCapturePositionEpsilon = 1.0e-4f;
    const bool capturePositionChanged =
        !m_HasCapturePosition ||
        glm::dot(capturePosition - m_LastCapturePosition,
                 capturePosition - m_LastCapturePosition) >
            kCapturePositionEpsilon * kCapturePositionEpsilon;
    if (m_FaceWarmupPending || capturePositionChanged) {
        for (uint32_t f = 0; f < kFaceCount; ++f) {
            outFaces[f] = f;
        }
        m_FaceWarmupPending = false;
        m_NextFace = 0;
        m_LastCapturePosition = capturePosition;
        m_HasCapturePosition = true;
        return kFaceCount;
    }

    const uint32_t perFrame = ResolveFacesPerFrame();
    for (uint32_t i = 0; i < perFrame; ++i) {
        outFaces[i] = m_NextFace;
        m_NextFace = (m_NextFace + 1) % kFaceCount;
    }
    return perFrame;
}
