#ifndef VK_ENABLE_BETA_EXTENSIONS
#define VK_ENABLE_BETA_EXTENSIONS
#endif

#include "Core/VulkanCompositeResources.h"
#include "Core/VulkanContext.h"
#include "Core/VulkanManager.h"

#include <vector>

// 创建游戏模式合成 render pass：
//   attachment 0: swapchain 颜色（CLEAR 装载——确定性初始化，最终 PRESENT）
//   单 subpass: 全屏四边形经普通纹理采样（descriptor）读 GameRT G-Buffer 颜色0/深度 → 写 swapchain
//   （几何已由 GameRT render pass 单独渲染；本机驱动不支持 subpass input attachment，合成走独立 pass + barrier）
void VulkanComposite_CreateRenderPass(ImGui_ImplVulkanH_Window* wd)
{
    VkAttachmentDescription swap = {};
    swap.format = wd->SurfaceFormat.format;
    swap.samples = VK_SAMPLE_COUNT_1_BIT;
    swap.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;      // 确定性初始化，避免 AMD 暴露未定义 tile 内容
    swap.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    swap.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    swap.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    swap.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    swap.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    VkSubpassDependency deps[1] = {};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rp = {};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp.attachmentCount = 1;
    rp.pAttachments = &swap;
    rp.subpassCount = 1;
    rp.pSubpasses = &subpass;
    rp.dependencyCount = 1;
    rp.pDependencies = deps;
    check_vk_result(vkCreateRenderPass(g_Device, &rp, g_Allocator, &g_CompositeRenderPass));

    // ===== swapchain UI 叠加 pass（loadOp=LOAD）：链末输出后画 UI，保留链结果并 alpha 混合 =====
    // 游戏模式（RenderGameComposite）链（g_SwapChain）输出 swapchain 后，UI 叠加于此
    VkAttachmentDescription swapUI = swap;
    swapUI.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;   // 保留链末 tonemap 结果
    swapUI.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;   // loadOp=LOAD 不能 UNDEFINED（链后布局 PRESENT_SRC，自动转换）
    swapUI.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkSubpassDependency uiDeps[1] = {};
    uiDeps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    uiDeps[0].dstSubpass = 0;
    uiDeps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    uiDeps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    uiDeps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    uiDeps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo uiRp = {};
    uiRp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    uiRp.attachmentCount = 1;
    uiRp.pAttachments = &swapUI;
    uiRp.subpassCount = 1;
    uiRp.pSubpasses = &subpass;
    uiRp.dependencyCount = 1;
    uiRp.pDependencies = uiDeps;
    check_vk_result(vkCreateRenderPass(g_Device, &uiRp, g_Allocator, &g_CompositeUIPass));
}

// 合成 render pass 的 framebuffer（per swapchain image）：只含 swapchain view（GameRT G-Buffer 经 descriptor 采样）
void VulkanComposite_CreateFramebuffers(ImGui_ImplVulkanH_Window* wd)
{
    g_CompositeFramebuffers.resize(wd->ImageCount);
    for (uint32_t i = 0; i < wd->ImageCount; i++) {
        VkFramebufferCreateInfo fb = {};
        fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb.renderPass = g_CompositeRenderPass;
        fb.attachmentCount = 1;
        fb.pAttachments = &wd->Frames[i].BackbufferView;
        fb.width = wd->Width;
        fb.height = wd->Height;
        fb.layers = 1;
        check_vk_result(vkCreateFramebuffer(g_Device, &fb, g_Allocator, &g_CompositeFramebuffers[i]));
    }
}



void DestroyCompositeResources()
{
    for (VkFramebuffer fb : g_CompositeFramebuffers) {
        if (fb != VK_NULL_HANDLE)
            vkDestroyFramebuffer(g_Device, fb, g_Allocator);
    }
    g_CompositeFramebuffers.clear();
    if (g_CompositeRenderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(g_Device, g_CompositeRenderPass, g_Allocator);
        g_CompositeRenderPass = VK_NULL_HANDLE;
    }
    if (g_CompositeUIPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(g_Device, g_CompositeUIPass, g_Allocator);
        g_CompositeUIPass = VK_NULL_HANDLE;
    }
}



