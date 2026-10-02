// GPU contracts for the production HiZComputeShader and grass SPIR-V.
// Synthetic depth makes expected results independent of engine demo scenes.
#include "Rendering/HiZComputeShader.h"
#include "Rendering/HiZHistory.h"
#include "Core/VulkanContext.h"
#include "Core/ProjectManager.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
uint32_t validationErrors = 0;
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void VkCheck(VkResult result, const char* message) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(message) + ": " + std::to_string(result));
}
VKAPI_ATTR VkBool32 VKAPI_CALL Debug(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++validationErrors;
        std::cerr << "VALIDATION: " << data->pMessage << '\n';
    }
    return VK_FALSE;
}
uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(g_PhysicalDevice, &memory);
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags) return i;
    }
    throw std::runtime_error("Required Vulkan memory type unavailable");
}
struct Buffer {
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    void* mapped{};
    VkDeviceSize size{};
    explicit Buffer(VkDeviceSize bytes) : size(bytes) {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = bytes;
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VkCheck(vkCreateBuffer(g_Device, &info, nullptr, &buffer), "create buffer");
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(g_Device, buffer, &requirements);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = requirements.size;
        alloc.memoryTypeIndex = MemoryType(requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkCheck(vkAllocateMemory(g_Device, &alloc, nullptr, &memory), "allocate buffer");
        VkCheck(vkBindBufferMemory(g_Device, buffer, memory, 0), "bind buffer");
        VkCheck(vkMapMemory(g_Device, memory, 0, bytes, 0, &mapped), "map buffer");
        std::memset(mapped, 0, static_cast<size_t>(bytes));
    }
    Buffer(const Buffer&) = delete;
    ~Buffer() {
        if (mapped) vkUnmapMemory(g_Device, memory);
        vkDestroyBuffer(g_Device, buffer, nullptr);
        vkFreeMemory(g_Device, memory, nullptr);
    }
};
struct Image {
    VkImage image{};
    VkDeviceMemory memory{};
    VkImageView view{};
    VkSampler sampler{};
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t width, height;
    Image(uint32_t w, uint32_t h) : width(w), height(h) {
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        info.extent = {w, h, 1};
        info.mipLevels = info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        VkCheck(vkCreateImage(g_Device, &info, nullptr, &image), "create image");
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(g_Device, image, &requirements);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = requirements.size;
        alloc.memoryTypeIndex = MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkCheck(vkAllocateMemory(g_Device, &alloc, nullptr, &memory), "allocate image");
        VkCheck(vkBindImageMemory(g_Device, image, memory, 0), "bind image");
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = info.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkCheck(vkCreateImageView(g_Device, &vi, nullptr, &view), "create view");
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = si.minFilter = VK_FILTER_NEAREST;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = VK_LOD_CLAMP_NONE;
        VkCheck(vkCreateSampler(g_Device, &si, nullptr, &sampler), "create sampler");
    }
    ~Image() {
        vkDestroySampler(g_Device, sampler, nullptr);
        vkDestroyImageView(g_Device, view, nullptr);
        vkDestroyImage(g_Device, image, nullptr);
        vkFreeMemory(g_Device, memory, nullptr);
    }
};
void ImageBarrier(VkCommandBuffer command, VkImage image, uint32_t levels,
        VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst,
        VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcAccessMask = src;
    barrier.dstAccessMask = dst;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
    vkCmdPipelineBarrier(command, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}
void HostBarrier(VkCommandBuffer command, VkPipelineStageFlags stage, VkAccessFlags access) {
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = access;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(command, stage, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}
struct Commands {
    VkCommandBuffer command{};
    Commands() {
        VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool = g_CommandPool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        VkCheck(vkAllocateCommandBuffers(g_Device, &alloc, &command), "allocate commands");
    }
    ~Commands() { vkFreeCommandBuffers(g_Device, g_CommandPool, 1, &command); }
    void Begin() {
        VkCheck(vkResetCommandBuffer(command, 0), "reset commands");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VkCheck(vkBeginCommandBuffer(command, &begin), "begin commands");
    }
    void Submit() {
        VkCheck(vkEndCommandBuffer(command), "end commands");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        VkCheck(vkQueueSubmit(g_Queue, 1, &submit, VK_NULL_HANDLE), "submit commands");
        VkCheck(vkQueueWaitIdle(g_Queue), "wait commands");
    }
};
void Upload(Image& image, const std::vector<float>& depth, Commands& commands) {
    Buffer staging(static_cast<VkDeviceSize>(depth.size()) * 16);
    auto* rgba = static_cast<float*>(staging.mapped);
    for (size_t i = 0; i < depth.size(); ++i) rgba[i * 4] = depth[i];
    commands.Begin();
    ImageBarrier(commands.command, image.image, 1, image.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        image.layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_SHADER_READ_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT,
        image.layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {image.width, image.height, 1};
    vkCmdCopyBufferToImage(commands.command, staging.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    ImageBarrier(commands.command, image.image, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    commands.Submit();
    image.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
}
void Generate(HiZComputeShader& hierarchy, Image& source, Commands& commands) {
    commands.Begin();
    // Deliberately request a partial chain: the published culling view must
    // still contain every level, including the final 1x1 maximum.
    hierarchy.GenerateMipLevelsFromColor(commands.command, source.image, source.view, source.sampler, 2);
    commands.Submit();
    source.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    hierarchy.SwapBuffers();
    Check(hierarchy.HasValidCullingData(), "hierarchy not published after generation");
}
void VerifyPyramid(HiZComputeShader& hierarchy, uint32_t width, uint32_t height,
        const std::vector<float>& input, Commands& commands) {
    std::vector<VkBufferImageCopy> copies;
    VkDeviceSize bytes = 0;
    for (uint32_t mip = 0; mip < hierarchy.GetCullingMipLevels(); ++mip) {
        VkBufferImageCopy copy{};
        copy.bufferOffset = bytes;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
        copy.imageExtent = {std::max(width >> (mip + 1), 1u), std::max(height >> (mip + 1), 1u), 1};
        bytes += static_cast<VkDeviceSize>(copy.imageExtent.width) * copy.imageExtent.height * 16;
        copies.push_back(copy);
    }
    Buffer readback(bytes);
    const VkImage image = hierarchy.GetHiZTextureImageForCulling();
    commands.Begin();
    ImageBarrier(commands.command, image, hierarchy.GetCullingMipLevels(),
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    vkCmdCopyImageToBuffer(commands.command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          readback.buffer, static_cast<uint32_t>(copies.size()), copies.data());
    ImageBarrier(commands.command, image, hierarchy.GetCullingMipLevels(),
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    HostBarrier(commands.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    commands.Submit();
    auto previous = input;
    uint32_t sw = width, sh = height;
    for (const auto& copy : copies) {
        const uint32_t dw = copy.imageExtent.width, dh = copy.imageExtent.height;
        std::vector<float> expected(static_cast<size_t>(dw) * dh);
        const auto* actual = reinterpret_cast<const float*>(
            static_cast<const char*>(readback.mapped) + copy.bufferOffset);
        for (uint32_t y = 0; y < dh; ++y) for (uint32_t x = 0; x < dw; ++x) {
            float maximum = 0;
            // CPU oracle uses continuous normalized footprints, independently
            // of the shader's integer arithmetic and dispatch dimensions.
            const auto x0 = static_cast<uint32_t>(std::floor(double(x) * sw / dw));
            const auto x1 = static_cast<uint32_t>(std::ceil(double(x + 1) * sw / dw));
            const auto y0 = static_cast<uint32_t>(std::floor(double(y) * sh / dh));
            const auto y1 = static_cast<uint32_t>(std::ceil(double(y + 1) * sh / dh));
            for (uint32_t sy = y0; sy < y1; ++sy) for (uint32_t sx = x0; sx < x1; ++sx)
                maximum = std::max(maximum, previous[static_cast<size_t>(sy) * sw + sx]);
            expected[static_cast<size_t>(y) * dw + x] = maximum;
            if (std::abs(actual[(static_cast<size_t>(y) * dw + x) * 4] - maximum) > 1e-6f)
                throw std::runtime_error("GPU pyramid mismatch at mip " + std::to_string(copy.imageSubresource.mipLevel) +
                    " pixel " + std::to_string(x) + "," + std::to_string(y));
        }
        previous = std::move(expected);
        sw = dw; sh = dh;
    }
    Check(sw == 1 && sh == 1, "last mip is not 1x1");
    Check(previous[0] == *std::max_element(input.begin(), input.end()), "tail mip missed global maximum");
}
struct CullParams {
    glm::mat4 model{1};
    glm::vec4 planes[6]{};
    glm::vec4 camAndDist{0, 0, 0, 100};
    glm::vec4 heightRange{0, 0.6f, 0.22f, 0};
    glm::mat4 hizViewProj{1};
    glm::uvec4 hizParams{};
    glm::vec4 bladeTerrain{0, 0, 1, 1};
    glm::vec4 bladeShape{1, 1, 0.005f, 0};
};
static_assert(sizeof(CullParams) == 304);
struct GrassDispatch {
    Buffer source{32}, compact{64}, indirect{32}, params{608}, buckets{32};
    VkDescriptorSetLayout setLayout{};
    VkDescriptorPool pool{};
    VkDescriptorSet set{};
    VkPipelineLayout layout{};
    VkPipeline pipeline{};
    GrassDispatch() {
        std::array<VkDescriptorSetLayoutBinding, 7> bindings{};
        for (uint32_t i = 0; i < 7; ++i) {
            bindings[i] = {i, i >= 5 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                           1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        }
        VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        sl.bindingCount = 7; sl.pBindings = bindings.data();
        VkCheck(vkCreateDescriptorSetLayout(g_Device, &sl, nullptr, &setLayout), "grass set layout");
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1; pl.pSetLayouts = &setLayout;
        pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &push;
        VkCheck(vkCreatePipelineLayout(g_Device, &pl, nullptr, &layout), "grass pipeline layout");
        const std::string path = ProjectManager::GetInstance().GetEngineAssetPath("shaders/spv/grass_blade_cull.comp.spv");
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        Check(file.good(), "grass SPIR-V unavailable");
        const auto bytes = static_cast<size_t>(file.tellg());
        std::vector<uint32_t> code(bytes / 4);
        file.seekg(0); file.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(bytes));
        VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        sm.codeSize = bytes; sm.pCode = code.data();
        VkShaderModule module{};
        VkCheck(vkCreateShaderModule(g_Device, &sm, nullptr, &module), "grass shader");
        VkComputePipelineCreateInfo pi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                    VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
        pi.layout = layout;
        const auto result = vkCreateComputePipelines(g_Device, VK_NULL_HANDLE, 1, &pi, nullptr, &pipeline);
        vkDestroyShaderModule(g_Device, module, nullptr);
        VkCheck(result, "grass pipeline");
        VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 5}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2}};
        VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dp.maxSets = 1; dp.poolSizeCount = 2; dp.pPoolSizes = sizes;
        VkCheck(vkCreateDescriptorPool(g_Device, &dp, nullptr, &pool), "grass descriptor pool");
        VkDescriptorSetAllocateInfo sa{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        sa.descriptorPool = pool; sa.descriptorSetCount = 1; sa.pSetLayouts = &setLayout;
        VkCheck(vkAllocateDescriptorSets(g_Device, &sa, &set), "grass descriptor set");
        const Buffer* buffers[] = {&source, &compact, &indirect, &params, &buckets};
        std::array<VkDescriptorBufferInfo, 5> infos{};
        std::array<VkWriteDescriptorSet, 5> writes{};
        for (uint32_t i = 0; i < 5; ++i) {
            infos[i] = {buffers[i]->buffer, 0, buffers[i]->size};
            writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, i, 0, 1,
                         VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &infos[i], nullptr};
        }
        vkUpdateDescriptorSets(g_Device, 5, writes.data(), 0, nullptr);
    }
    ~GrassDispatch() {
        vkDestroyDescriptorPool(g_Device, pool, nullptr);
        vkDestroyPipeline(g_Device, pipeline, nullptr);
        vkDestroyPipelineLayout(g_Device, layout, nullptr);
        vkDestroyDescriptorSetLayout(g_Device, setLayout, nullptr);
    }
    void Run(HiZComputeShader& hierarchy, Image& depth, Commands& commands,
             CullParams data, uint32_t slot, uint32_t expected, const char* label) {
        std::array<CullParams, 2> views{data, data};
        std::memcpy(params.mapped, views.data(), sizeof(views));
        const std::array<glm::vec4, 2> blade{glm::vec4(0, 0, 0.8f, 0.6f), glm::vec4(0.12f, 0.2f, 0, 0)};
        std::memcpy(source.mapped, blade.data(), sizeof(blade));
        VkDescriptorImageInfo image{depth.sampler, hierarchy.GetHiZTextureViewForCulling(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = set; write.dstBinding = 5; write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; write.pImageInfo = &image;
        vkUpdateDescriptorSets(g_Device, 1, &write, 0, nullptr);
        write.dstBinding = 6;
        vkUpdateDescriptorSets(g_Device, 1, &write, 0, nullptr);
        commands.Begin();
        vkCmdFillBuffer(commands.command, indirect.buffer, 0, indirect.size, 0);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(commands.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &barrier, 0, nullptr, 0, nullptr);
        vkCmdBindPipeline(commands.command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(commands.command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
        const glm::uvec4 push{slot, 1, 1, 0};
        vkCmdPushConstants(commands.command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, &push);
        vkCmdDispatch(commands.command, 1, 1, 1);
        HostBarrier(commands.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
        commands.Submit();
        const auto* counts = static_cast<const uint32_t*>(indirect.mapped);
        Check(counts[slot * 4] == 10 && counts[slot * 4 + 1] == expected, label);
        Check(counts[(1 - slot) * 4 + 1] == 0, "view slot overwritten");
        if (expected) Check(std::memcmp(static_cast<char*>(compact.mapped) + slot * 32, blade.data(), 32) == 0,
                            "grass compacted into wrong view slot");
        std::cout << "PASS " << label << " visible=" << counts[slot * 4 + 1] << '\n';
    }
};
void Contracts() {
    glm::mat4 vp{1};
    Check(!HiZHistory::CanReuse(vp, vp, 1, 1, false), "first-frame history accepted");
    Check(HiZHistory::CanReuse(vp, vp, 1, 1, true), "static history rejected");
    Check(!HiZHistory::CanReuse(vp, vp, 2, 1, true), "occluder edits accepted");
    auto moved = vp; moved[3][0] = 0.00001f;
    Check(!HiZHistory::CanReuse(moved, vp, 1, 1, true), "camera movement accepted");
    vp[2][3] = -1;
    const glm::vec2 jitter{0.001f, -0.002f};
    const glm::vec4 point{0.3f, 0.4f, -3, 1};
    auto clip = vp * point;
    clip.x += jitter.x * clip.w; clip.y += jitter.y * clip.w;
    Check(glm::length(HiZHistory::WithJitter(vp, jitter) * point - clip) < 1e-6f, "jitter projection mismatch");
    std::cout << "PASS history invalidation and source jitter\n";
}
void RunGpuContracts() {
    Commands commands;
    for (const auto dimensions : {glm::uvec2(64, 32), glm::uvec2(63, 37), glm::uvec2(131, 75),
                                  glm::uvec2(1, 17), glm::uvec2(17, 1), glm::uvec2(3, 3)}) {
        Image source(dimensions.x, dimensions.y);
        HiZComputeShader hierarchy;
        Check(hierarchy.Init(g_Device, g_PhysicalDevice, dimensions.x, dimensions.y), "hierarchy init failed");
        Check(!hierarchy.HasValidCullingData(), "uninitialized hierarchy accepted");
        std::vector<float> depth(static_cast<size_t>(dimensions.x) * dimensions.y);
        for (uint32_t iteration = 0; iteration < 3; ++iteration) {
            for (size_t i = 0; i < depth.size(); ++i) depth[i] = float((i * 17 + iteration * 31) % 97) / 100.0f;
            depth.back() = 1; // right/bottom odd edge must survive every mip
            Upload(source, depth, commands);
            Generate(hierarchy, source, commands);
            VerifyPyramid(hierarchy, dimensions.x, dimensions.y, depth, commands);
        }
        std::cout << "PASS pyramid " << dimensions.x << 'x' << dimensions.y << " all mips, 3 frames\n";
    }
    Image source(64, 64);
    HiZComputeShader hierarchy;
    Check(hierarchy.Init(g_Device, g_PhysicalDevice, 64, 64), "grass hierarchy init failed");
    GrassDispatch grass;
    std::vector<float> depth(64 * 64, 0.25f);
    CullParams data;
    data.model[3] = glm::vec4(0.2f, 0, 0.7f, 1);
    data.hizParams = glm::uvec4(64, 64, hierarchy.GetCullingMipLevels(), 1);
    Upload(source, depth, commands); Generate(hierarchy, source, commands);
    grass.Run(hierarchy, source, commands, data, 0, 0, "fully occluded grass");
    auto broadBucket = data; broadBucket.heightRange = glm::vec4(-100, 100, 1.5f, 0);
    grass.Run(hierarchy, source, commands, broadBucket, 0, 0,
              "blade bounds independent of broad bucket height");
    auto disabled = data; disabled.hizParams.w = 0;
    grass.Run(hierarchy, source, commands, disabled, 1, 1, "Hi-Z off / view slot isolation");
    auto near = data; near.model[3].z = 0.1f;
    grass.Run(hierarchy, source, commands, near, 0, 1, "near plane crossing remains visible");
    auto behind = data; behind.hizViewProj[3][3] = -1;
    grass.Run(hierarchy, source, commands, behind, 0, 1, "behind-eye projection remains visible");
    auto outside = data; outside.planes[0] = glm::vec4(1, 0, 0, -10);
    grass.Run(hierarchy, source, commands, outside, 0, 0, "frustum fallback still culls");
    // A hole lies strictly inside the projected rectangle, not at any corner
    // selected by the former floor-log2 lookup. It must prevent occlusion.
    depth[44 * 64 + 36] = 1;
    Upload(source, depth, commands); Generate(hierarchy, source, commands);
    grass.Run(hierarchy, source, commands, data, 0, 1, "interior background hole remains visible");
    std::fill(depth.begin(), depth.end(), 1.0f);
    Upload(source, depth, commands); Generate(hierarchy, source, commands);
    grass.Run(hierarchy, source, commands, data, 1, 1, "clear depth remains visible");
    auto distant = data;
    distant.hizViewProj[2][2] = 0.001f;
    distant.hizViewProj[3][2] = 0.5f;
    std::fill(depth.begin(), depth.end(), 0.50025f);
    Upload(source, depth, commands); Generate(hierarchy, source, commands);
    grass.Run(hierarchy, source, commands, distant, 0, 0,
              "small distant depth separation still occludes");
}
} // namespace

int main(int argc, char** argv) {
    VkDebugUtilsMessengerEXT messenger{};
    try {
        Contracts();
        ProjectManager::GetInstance().Initialize(argc, argv);
        uint32_t layerCount = 0;
        VkCheck(vkEnumerateInstanceLayerProperties(&layerCount, nullptr), "enumerate layers");
        std::vector<VkLayerProperties> layers(layerCount);
        VkCheck(vkEnumerateInstanceLayerProperties(&layerCount, layers.data()), "enumerate layers");
        const bool validation = std::any_of(layers.begin(), layers.end(), [](const auto& layer) {
            return std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0;
        });
        const char* layer = "VK_LAYER_KHRONOS_validation";
        const char* extension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "Mikan Hi-Z GPU contracts"; app.apiVersion = VK_API_VERSION_1_1;
        VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debug.pfnUserCallback = Debug;
        VkInstanceCreateInfo instance{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        instance.pApplicationInfo = &app;
        if (validation) {
            instance.enabledLayerCount = instance.enabledExtensionCount = 1;
            instance.ppEnabledLayerNames = &layer; instance.ppEnabledExtensionNames = &extension;
            instance.pNext = &debug;
        }
        VkCheck(vkCreateInstance(&instance, nullptr, &g_Instance), "create instance");
        if (validation) {
            auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(g_Instance, "vkCreateDebugUtilsMessengerEXT"));
            VkCheck(create(g_Instance, &debug, nullptr, &messenger), "create debug messenger");
        }
        std::cout << "Validation layer: " << (validation ? "enabled" : "UNAVAILABLE") << '\n';
        uint32_t count = 0;
        VkCheck(vkEnumeratePhysicalDevices(g_Instance, &count, nullptr), "enumerate GPUs");
        if (!count) { vkDestroyInstance(g_Instance, nullptr); g_Instance = VK_NULL_HANDLE; return 77; }
        std::vector<VkPhysicalDevice> devices(count);
        VkCheck(vkEnumeratePhysicalDevices(g_Instance, &count, devices.data()), "enumerate GPUs");
        for (const auto physical : devices) {
            g_PhysicalDevice = physical;
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(physical, &properties);
            std::cout << "GPU: " << properties.deviceName << '\n';
            uint32_t familyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, nullptr);
            std::vector<VkQueueFamilyProperties> families(familyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, families.data());
            g_QueueFamily = 0;
            while (g_QueueFamily < familyCount &&
                   (families[g_QueueFamily].queueFlags & (VK_QUEUE_COMPUTE_BIT | VK_QUEUE_GRAPHICS_BIT)) !=
                   (VK_QUEUE_COMPUTE_BIT | VK_QUEUE_GRAPHICS_BIT)) ++g_QueueFamily;
            Check(g_QueueFamily < familyCount, "graphics/compute queue unavailable");
            float priority = 1;
            VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
            queue.queueFamilyIndex = g_QueueFamily; queue.queueCount = 1; queue.pQueuePriorities = &priority;
            VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
            di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &queue;
            VkCheck(vkCreateDevice(physical, &di, nullptr, &g_Device), "create device");
            vkGetDeviceQueue(g_Device, g_QueueFamily, 0, &g_Queue);
            VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pool.queueFamilyIndex = g_QueueFamily; pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            VkCheck(vkCreateCommandPool(g_Device, &pool, nullptr, &g_CommandPool), "create command pool");
            RunGpuContracts();
            vkDestroyCommandPool(g_Device, g_CommandPool, nullptr); g_CommandPool = VK_NULL_HANDLE;
            vkDestroyDevice(g_Device, nullptr); g_Device = VK_NULL_HANDLE;
        }
        if (messenger) {
            auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(g_Instance, "vkDestroyDebugUtilsMessengerEXT"));
            destroy(g_Instance, messenger, nullptr);
        }
        vkDestroyInstance(g_Instance, nullptr); g_Instance = VK_NULL_HANDLE;
        Check(validationErrors == 0, "Vulkan validation errors");
        std::cout << "PASS all Hi-Z GPU contracts\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        // Resources inside contract scopes unwind before device destruction.
        if (g_Device) { vkDeviceWaitIdle(g_Device); if (g_CommandPool) vkDestroyCommandPool(g_Device, g_CommandPool, nullptr); vkDestroyDevice(g_Device, nullptr); g_Device = VK_NULL_HANDLE; }
        if (g_Instance) vkDestroyInstance(g_Instance, nullptr);
        g_Instance = VK_NULL_HANDLE;
        return 1;
    }
}
