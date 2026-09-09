// 流水线：cmaa_edges.comp（全屏：边缘检测 + 候选列表）→ cmaa_process.comp（候选：Simple/Z 形状 → 混合颜色 → 固定 4 槽）
//   → cmaa_apply.comp（稀疏 quad：O(1) 直读本像素槽 → 写 result 图）→ cmaa_apply.frag（链 pass：仅采样 result）
// 依赖引擎设施：EngineConfig::GetShaderPath / FindMemoryType（VulkanManager 提供）
#include "Rendering/CMAA2.h"
#include "Core/Log.h"
#include "EngineConfig.h"

#include <cstdio>
#include <vector>

extern VkDevice g_Device;
extern VkPhysicalDevice g_PhysicalDevice;
extern VkAllocationCallbacks* g_Allocator;

static uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(g_PhysicalDevice, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProps.memoryTypes[i].propertyFlags & properties) == properties)
            return i;
    }
    return 0;
}

// ---- 资源创建辅助 ----
bool CMAA2::CreateImage(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage,
                        VkImage& image, VkDeviceMemory& memory, VkImageView& view)
{
    VkImageCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent.width = width;
    info.extent.height = height;
    info.extent.depth = 1;
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(m_Device, &info, g_Allocator, &image) != VK_SUCCESS) return false;

    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(m_Device, image, &memReq);
    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = FindMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(m_Device, &allocInfo, g_Allocator, &memory) != VK_SUCCESS) return false;
    vkBindImageMemory(m_Device, image, memory, 0);

    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    return vkCreateImageView(m_Device, &viewInfo, g_Allocator, &view) == VK_SUCCESS;
}

void CMAA2::ImageBarrier(VkCommandBuffer cmd, VkImage image,
                         VkImageLayout oldLayout, VkImageLayout newLayout,
                         VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                         VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage)
{
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

// ---- Init/Cleanup ----
bool CMAA2::Init(VkDevice device, uint32_t w, uint32_t h)
{
    m_Device = device;
    m_Width = w;
    m_Height = h;
    m_CandidateCapacity = w * h;   // 上限 = 像素数（官方 requiredCandidatePixels = resX*resY）

    if (!CreateImages(w, h)) { LOGE("[CMAA2] CreateImages failed"); return false; }
    LOGD("[CMAA2] images OK");
    if (!CreateBuffers(w, h)) { LOGE("[CMAA2] CreateBuffers failed"); return false; }
    LOGD("[CMAA2] buffers OK");
    if (!CreatePipelines()) { LOGE("[CMAA2] CreatePipelines failed"); return false; }
    LOGD("[CMAA2] pipelines OK");
    LOGD("[CMAA2] Init OK (%ux%u, candidates=%u)", w, h, m_CandidateCapacity);
    return true;
}

void CMAA2::Cleanup()
{
    if (m_Device == VK_NULL_HANDLE) return;
    DestroyPipeline(m_EdgesPipe);
    DestroyPipeline(m_ProcessPipe);
    DestroyPipeline(m_ArgsPipe);
    if (m_EdgesView) vkDestroyImageView(m_Device, m_EdgesView, g_Allocator);
    if (m_EdgesImage) vkDestroyImage(m_Device, m_EdgesImage, g_Allocator);
    if (m_EdgesMemory) vkFreeMemory(m_Device, m_EdgesMemory, g_Allocator);
    if (m_ResultView) vkDestroyImageView(m_Device, m_ResultView, g_Allocator);
    if (m_ResultImage) vkDestroyImage(m_Device, m_ResultImage, g_Allocator);
    if (m_ResultMemory) vkFreeMemory(m_Device, m_ResultMemory, g_Allocator);
    if (m_WeightSampler) vkDestroySampler(m_Device, m_WeightSampler, g_Allocator);
    if (m_EdgesSampler) vkDestroySampler(m_Device, m_EdgesSampler, g_Allocator);
    if (m_CandidateBuffer) vkDestroyBuffer(m_Device, m_CandidateBuffer, g_Allocator);
    if (m_CandidateMemory) vkFreeMemory(m_Device, m_CandidateMemory, g_Allocator);
    if (m_ArgsBuffer) vkDestroyBuffer(m_Device, m_ArgsBuffer, g_Allocator);
    if (m_ArgsMemory) vkFreeMemory(m_Device, m_ArgsMemory, g_Allocator);
    m_Device = VK_NULL_HANDLE;
}

void CMAA2::DestroyPipeline(ComputePipeline& pipe)
{
    if (m_Device == VK_NULL_HANDLE) return;
    if (pipe.pipeline) vkDestroyPipeline(m_Device, pipe.pipeline, g_Allocator);
    if (pipe.layout) vkDestroyPipelineLayout(m_Device, pipe.layout, g_Allocator);
    if (pipe.setLayout) vkDestroyDescriptorSetLayout(m_Device, pipe.setLayout, g_Allocator);
    if (pipe.pool) vkDestroyDescriptorPool(m_Device, pipe.pool, g_Allocator);
    if (pipe.module) vkDestroyShaderModule(m_Device, pipe.module, g_Allocator);
    pipe = ComputePipeline();
}

// ---- 资源 ----
bool CMAA2::CreateImages(uint32_t w, uint32_t h)
{
    // edges：R8UINT（storage 写 + sampled 读）
    if (!CreateImage(w, h, VK_FORMAT_R8_UINT,
                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                     m_EdgesImage, m_EdgesMemory, m_EdgesView)) return false;
    if (!CreateImage(w, h, VK_FORMAT_R8G8B8A8_UNORM,
                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                     m_ResultImage, m_ResultMemory, m_ResultView)) return false;

    // 采样器
    VkSamplerCreateInfo sInfo = {};
    sInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sInfo.magFilter = VK_FILTER_NEAREST;
    sInfo.minFilter = VK_FILTER_NEAREST;
    sInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(m_Device, &sInfo, g_Allocator, &m_EdgesSampler) != VK_SUCCESS) return false;
    sInfo.magFilter = VK_FILTER_LINEAR;
    sInfo.minFilter = VK_FILTER_LINEAR;
    if (vkCreateSampler(m_Device, &sInfo, g_Allocator, &m_WeightSampler) != VK_SUCCESS) return false;
    return true;
}

bool CMAA2::CreateBuffers(uint32_t w, uint32_t h)
{
    auto createBuf = [&](VkDeviceSize size, VkBuffer& buf, VkDeviceMemory& mem) -> bool {
        VkBufferCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = size;
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;   // TRANSFER_DST：每帧 FillBuffer 清零
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(m_Device, &info, g_Allocator, &buf) != VK_SUCCESS) return false;
        VkMemoryRequirements memReq;
        vkGetBufferMemoryRequirements(m_Device, buf, &memReq);
        VkMemoryAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReq.size;
        allocInfo.memoryTypeIndex = FindMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(m_Device, &allocInfo, g_Allocator, &mem) != VK_SUCCESS) return false;
        vkBindBufferMemory(m_Device, buf, mem, 0);
        return true;
    };

    // 候选：uint count + uint data[]（官方 requiredCandidatePixels = resX*resY）
    {
        VkBufferCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = (VkDeviceSize)(m_CandidateCapacity + 1) * sizeof(uint32_t);
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(m_Device, &info, g_Allocator, &m_CandidateBuffer) != VK_SUCCESS) return false;
        VkMemoryRequirements memReq;
        vkGetBufferMemoryRequirements(m_Device, m_CandidateBuffer, &memReq);
        VkMemoryAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReq.size;
        allocInfo.memoryTypeIndex = FindMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(m_Device, &allocInfo, g_Allocator, &m_CandidateMemory) != VK_SUCCESS) return false;
        vkBindBufferMemory(m_Device, m_CandidateBuffer, m_CandidateMemory, 0);
    }
    // 独立 DispatchIndirect 参数（12B {x,y,z}）——专用 buffer，绝不复用 candidate（candidate[1]/[2] 是候选数据，当 y/z 组数会爆线程）
    if (!createBuf(3 * sizeof(uint32_t), m_ArgsBuffer, m_ArgsMemory)) return false;
    return true;
}

// ---- 管线 ----
bool CMAA2::CreatePipelines()
{
    struct PipeDef {
        const char* spv;
        ComputePipeline* out;
        std::vector<VkDescriptorSetLayoutBinding> bindings;
    };
    // edges：0=edges storage image、1=候选 SSBO、2=tonemap 输出 sampler（SHADER_READ_ONLY 布局采样）
    std::vector<VkDescriptorSetLayoutBinding> edgesBindings = {
        { 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
        { 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
    };
    // process：0=tonemap 输出 sampler（SHADER_READ_ONLY 采样）、1=候选 SSBO、2=edges storage image（r8ui readonly）、3=result storage image（写，alpha 标记）
    std::vector<VkDescriptorSetLayoutBinding> processBindings = {
        { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
        { 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
        { 3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
    };
    // args：0=候选 SSBO（readonly 读 count）、1=argsBuffer SSBO（写 DispatchIndirect 参数）
    std::vector<VkDescriptorSetLayoutBinding> argsBindings = {
        { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
    };

    PipeDef defs[] = {
        { "cmaa_edges.comp.spv",   &m_EdgesPipe,   edgesBindings },
        { "cmaa_process.comp.spv", &m_ProcessPipe, processBindings },
        { "cmaa_args.comp.spv",    &m_ArgsPipe,    argsBindings },
    };

    for (PipeDef& def : defs) {
        LOGD("[CMAA2] creating pipe %s", def.spv);
        std::string path = EngineConfig::GetShaderPath(def.spv);
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) {
            fprintf(stderr, "[CMAA2] shader not found: %s\n", path.c_str());
            return false;
        }
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        std::vector<char> code(size);
        fread(code.data(), 1, size, f);
        fclose(f);
        LOGD("[CMAA2]   spv %s size=%ld", def.spv, size);

        VkShaderModuleCreateInfo mInfo = {};
        mInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        mInfo.codeSize = code.size();
        mInfo.pCode = (const uint32_t*)code.data();
        if (vkCreateShaderModule(m_Device, &mInfo, g_Allocator, &def.out->module) != VK_SUCCESS) return false;
        LOGD("[CMAA2]   module OK");

        VkDescriptorSetLayoutCreateInfo lInfo = {};
        lInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lInfo.bindingCount = (uint32_t)def.bindings.size();
        lInfo.pBindings = def.bindings.data();
        if (vkCreateDescriptorSetLayout(m_Device, &lInfo, g_Allocator, &def.out->setLayout) != VK_SUCCESS) return false;
        LOGD("[CMAA2]   setLayout OK");

        VkPipelineLayoutCreateInfo plInfo = {};
        plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plInfo.setLayoutCount = 1;
        plInfo.pSetLayouts = &def.out->setLayout;
        if (vkCreatePipelineLayout(m_Device, &plInfo, g_Allocator, &def.out->layout) != VK_SUCCESS) return false;
        LOGD("[CMAA2]   pipelineLayout OK");

        VkPipelineShaderStageCreateInfo stage = {};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = def.out->module;
        stage.pName = "main";

        VkComputePipelineCreateInfo cInfo = {};
        cInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cInfo.stage = stage;
        cInfo.layout = def.out->layout;
        if (vkCreateComputePipelines(m_Device, VK_NULL_HANDLE, 1, &cInfo, g_Allocator, &def.out->pipeline) != VK_SUCCESS) return false;
        LOGD("[CMAA2]   computePipeline OK");

        // descriptor pool + set（3 种类型都要声明）
        VkDescriptorPoolSize poolSizes[3] = {};
        poolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        poolSizes[0].descriptorCount = 4;
        poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        poolSizes[1].descriptorCount = 8;
        poolSizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSizes[2].descriptorCount = 4;
        VkDescriptorPoolCreateInfo pInfo = {};
        pInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pInfo.maxSets = 1;
        pInfo.poolSizeCount = 3;
        pInfo.pPoolSizes = poolSizes;
        LOGD("[CMAA2]   calling vkCreateDescriptorPool...");
        if (vkCreateDescriptorPool(m_Device, &pInfo, g_Allocator, &def.out->pool) != VK_SUCCESS) return false;
        LOGD("[CMAA2]   pool OK");

        VkDescriptorSetAllocateInfo aInfo = {};
        aInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        aInfo.descriptorPool = def.out->pool;
        aInfo.descriptorSetCount = 1;
        aInfo.pSetLayouts = &def.out->setLayout;
        if (vkAllocateDescriptorSets(m_Device, &aInfo, &def.out->set) != VK_SUCCESS) return false;
        LOGD("[CMAA2]   allocSet OK");
    }

    // 更新 descriptor 内容（AtmosphereLUT 同款写法：push_back + 逐字段赋值）
    // edges set
    {
        VkDescriptorImageInfo edgesImgInfo = { VK_NULL_HANDLE, m_EdgesView, VK_IMAGE_LAYOUT_GENERAL };
        VkDescriptorBufferInfo bufInfo = { m_CandidateBuffer, 0, VK_WHOLE_SIZE };
        VkDescriptorImageInfo colorInfo = { m_EdgesSampler, m_EdgesView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };   // 占位（Dispatch 每帧换真实 color view/sampler）
        std::vector<VkWriteDescriptorSet> writes;
        auto addWrite = [&](uint32_t binding, VkDescriptorType type, const VkDescriptorImageInfo* img, const VkDescriptorBufferInfo* buf) {
            writes.push_back({});
            VkWriteDescriptorSet& w = writes.back();
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = m_EdgesPipe.set;
            w.dstBinding = binding;
            w.descriptorCount = 1;
            w.descriptorType = type;
            w.pImageInfo = img;
            w.pBufferInfo = buf;
        };
        addWrite(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &edgesImgInfo, nullptr);
        addWrite(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &bufInfo);
        addWrite(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &colorInfo, nullptr);
        vkUpdateDescriptorSets(m_Device, (uint32_t)writes.size(), writes.data(), 0, nullptr);
    }
    // process set（binding 0 = tonemap 输出 sampler——READ_ONLY 布局，Dispatch 每帧更新 view；binding 3 = result storage image 写）
    {
        VkDescriptorImageInfo colorInfo = { m_EdgesSampler, m_EdgesView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };   // 占位
        VkDescriptorBufferInfo candInfo = { m_CandidateBuffer, 0, VK_WHOLE_SIZE };
        VkDescriptorImageInfo edgesInfo = { VK_NULL_HANDLE, m_EdgesView, VK_IMAGE_LAYOUT_GENERAL };   // storage image（无 sampler）
        VkDescriptorImageInfo resultInfo = { VK_NULL_HANDLE, m_ResultView, VK_IMAGE_LAYOUT_GENERAL };
        std::vector<VkWriteDescriptorSet> writes;
        auto addWrite = [&](uint32_t binding, VkDescriptorType type, const VkDescriptorImageInfo* img, const VkDescriptorBufferInfo* buf) {
            writes.push_back({});
            VkWriteDescriptorSet& w = writes.back();
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = m_ProcessPipe.set;
            w.dstBinding = binding;
            w.descriptorCount = 1;
            w.descriptorType = type;
            w.pImageInfo = img;
            w.pBufferInfo = buf;
        };
        addWrite(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &colorInfo, nullptr);
        addWrite(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &candInfo);
        addWrite(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &edgesInfo, nullptr);
        addWrite(3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &resultInfo, nullptr);
        vkUpdateDescriptorSets(m_Device, (uint32_t)writes.size(), writes.data(), 0, nullptr);
    }
    // args set（binding 0 = 候选 SSBO readonly、binding 1 = argsBuffer SSBO 写 DispatchIndirect 参数）
    {
        VkDescriptorBufferInfo candInfo = { m_CandidateBuffer, 0, VK_WHOLE_SIZE };
        VkDescriptorBufferInfo argsInfo = { m_ArgsBuffer, 0, VK_WHOLE_SIZE };
        std::vector<VkWriteDescriptorSet> writes;
        auto addWrite = [&](uint32_t binding, VkDescriptorType type, const VkDescriptorImageInfo* img, const VkDescriptorBufferInfo* buf) {
            writes.push_back({});
            VkWriteDescriptorSet& w = writes.back();
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = m_ArgsPipe.set;
            w.dstBinding = binding;
            w.descriptorCount = 1;
            w.descriptorType = type;
            w.pImageInfo = img;
            w.pBufferInfo = buf;
        };
        addWrite(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &candInfo);
        addWrite(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &argsInfo);
        vkUpdateDescriptorSets(m_Device, (uint32_t)writes.size(), writes.data(), 0, nullptr);
    }
    return true;
}

// ---- 每帧 dispatch（官方语义：edges → process(链表) → apply 原地写 tonemap 输出）----
void CMAA2::Dispatch(VkCommandBuffer cmd, VkImageView colorView, VkSampler colorSampler, VkImage colorImage, uint32_t w, uint32_t h)
{
    if (m_Device == VK_NULL_HANDLE) return;
    if (w != m_Width || h != m_Height) {
        static bool s_sizeMismatchLogged = false;
        if (!s_sizeMismatchLogged) {
            LOGW("[CMAA2] Dispatch 尺寸不匹配 %ux%u vs Init %ux%u——跳过（AA 失效；Scene/Game/Swap 三链共用单实例）",
                 w, h, m_Width, m_Height);
            s_sizeMismatchLogged = true;
        }
        return;
    }

    // 0) 每帧清候选 count（candidate[0]=0——edges 阶段原子累加候选数）
    vkCmdFillBuffer(cmd, m_CandidateBuffer, 0, sizeof(uint32_t), 0);
    {
        VkBufferMemoryBarrier bb = {};
        bb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bb.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        bb.size = VK_WHOLE_SIZE;
        bb.buffer = m_CandidateBuffer;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                             0, nullptr, 1, &bb, 0, nullptr);
    }

    // 1) edges/result 布局 → GENERAL（compute 写）——tonemap 输出保持 SHADER_READ_ONLY（edges/process sampler 采样）
    ImageBarrier(cmd, m_EdgesImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                 VK_ACCESS_NONE, VK_ACCESS_SHADER_WRITE_BIT,
                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    // result 每帧 clear（alpha=0 标记未混合）→ GENERAL 供 process 直写
    ImageBarrier(cmd, m_ResultImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_ACCESS_NONE, VK_ACCESS_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    {
        VkClearColorValue clear = {};
        VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdClearColorImage(cmd, m_ResultImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    }
    ImageBarrier(cmd, m_ResultImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    // 2) edges：更新 color 描述符（sampler 读 READ_ONLY 布局——GENERAL 采样在 NVIDIA 不可靠）→ dispatch 全屏
    {
        VkDescriptorImageInfo colorInfo = { colorSampler, colorView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet write = {};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = m_EdgesPipe.set;
        write.dstBinding = 2;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &colorInfo;
        vkUpdateDescriptorSets(m_Device, 1, &write, 0, nullptr);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_EdgesPipe.pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_EdgesPipe.layout, 0, 1, &m_EdgesPipe.set, 0, nullptr);
        vkCmdDispatch(cmd, (w + 15) / 16, (h + 15) / 16, 1);
    }

    // 3) barrier：edges 写 → process 读（edgesImg + candidate 的 count/data）
    ImageBarrier(cmd, m_EdgesImage, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                 VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    {
        VkBufferMemoryBarrier bb = {};
        bb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        bb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        bb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        bb.size = VK_WHOLE_SIZE;
        bb.buffer = m_CandidateBuffer;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                             0, nullptr, 1, &bb, 0, nullptr);
    }

    // 4) args：候选数 → 独立 argsBuffer（组数 = ceil(count/128)）；barrier argsBuffer 写 → INDIRECT 读
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_ArgsPipe.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_ArgsPipe.layout, 0, 1, &m_ArgsPipe.set, 0, nullptr);
    vkCmdDispatch(cmd, 1, 1, 1);
    {
        VkBufferMemoryBarrier bb = {};
        bb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        bb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        bb.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
        bb.size = VK_WHOLE_SIZE;
        bb.buffer = m_ArgsBuffer;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, 0,
                             0, nullptr, 1, &bb, 0, nullptr);
    }

    // 5) process：更新 color 描述符 → DispatchIndirect(argsBuffer)——只调度真实候选组（免固定 dispatch 空转）
    {
        VkDescriptorImageInfo colorInfo = { colorSampler, colorView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet write = {};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = m_ProcessPipe.set;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &colorInfo;
        vkUpdateDescriptorSets(m_Device, 1, &write, 0, nullptr);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_ProcessPipe.pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_ProcessPipe.layout, 0, 1, &m_ProcessPipe.set, 0, nullptr);
        vkCmdDispatchIndirect(cmd, m_ArgsBuffer, 0);
    }

    // 6) barrier：result compute 写 → 后处理链采样（SHADER_READ_ONLY——cmaa_apply.frag 采样 result）
    ImageBarrier(cmd, m_ResultImage, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}
