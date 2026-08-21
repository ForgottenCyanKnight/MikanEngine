#include "Editor/MRTDebugWindow.h"
#include "imgui/imgui.h"
#include "imgui_impl_vulkan.h"
#include "RenderTarget.h"
#include "EngineGlobal.h"
#include "VulkanManager.h"

extern MIKAN_API RenderTarget g_SceneRenderTarget;
extern MIKAN_API RenderTarget g_GameRenderTarget;

namespace Editor {

MRTDebugWindow& MRTDebugWindow::GetInstance() {
    static MRTDebugWindow instance;
    return instance;
}

void MRTDebugWindow::Render() {
    if (!m_visible) return;

    ImGui::Begin("MRT调试", &m_visible);

    static int renderTargetIndex = 0;
    static int lastRenderTargetIndex = -1;
    const char* renderTargetNames[] = { "场景视图", "游戏视图" };
    ImGui::Combo("渲染目标", &renderTargetIndex, renderTargetNames, IM_ARRAYSIZE(renderTargetNames));

    // MRT调试窗口的静态变量
    static VkDescriptorSet s_MRTDescriptorSets[2][4] = { {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE}, {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE} };
    static VkSampler s_MRTSampler = VK_NULL_HANDLE;
    static VkImageView s_LastImageViews[2][4] = { {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE}, {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE} };

    if (renderTargetIndex != lastRenderTargetIndex && lastRenderTargetIndex != -1) {
        for (int i = 0; i < 4; i++) {
            if (s_MRTDescriptorSets[lastRenderTargetIndex][i] != VK_NULL_HANDLE) {
                ImGui_ImplVulkan_RemoveTexture(s_MRTDescriptorSets[lastRenderTargetIndex][i]);
                s_MRTDescriptorSets[lastRenderTargetIndex][i] = VK_NULL_HANDLE;
            }
            s_LastImageViews[lastRenderTargetIndex][i] = VK_NULL_HANDLE;
        }
    }
    lastRenderTargetIndex = renderTargetIndex;

    RenderTarget* currentRenderTarget = (renderTargetIndex == 0) ? &g_SceneRenderTarget : &g_GameRenderTarget;

    ImGui::Text("渲染目标信息: %d x %d", currentRenderTarget->GetWidth(), currentRenderTarget->GetHeight());
    ImGui::Separator();

    const char* channelNames[] = {
        "颜色通道",
        "法线通道",
        "PBR通道",
        "标记通道"
    };

    float cellWidth = ImGui::GetContentRegionAvail().x * 0.5f - ImGui::GetStyle().ItemSpacing.x;
    float cellHeight = cellWidth * 0.75f;

    if (s_MRTSampler == VK_NULL_HANDLE) {
        VkSamplerCreateInfo samplerInfo = {};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.anisotropyEnable = VK_FALSE;
        samplerInfo.maxAnisotropy = 1.0f;
        samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
        samplerInfo.unnormalizedCoordinates = VK_FALSE;
        samplerInfo.compareEnable = VK_FALSE;
        samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.mipLodBias = 0.0f;
        samplerInfo.minLod = 0.0f;
        samplerInfo.maxLod = 1.0f;

        VkResult err = vkCreateSampler(g_Device, &samplerInfo, g_Allocator, &s_MRTSampler);
        check_vk_result(err);
    }

    for (int i = 0; i < 4; i++) {
        if (i % 2 == 1) {
            ImGui::SameLine();
        }

        ImGui::BeginGroup();
        ImGui::Text("%s", channelNames[i]);

        VkImageView imageView = currentRenderTarget->GetColorImageView(i);
        if (imageView != VK_NULL_HANDLE) {
            if (s_MRTDescriptorSets[renderTargetIndex][i] == VK_NULL_HANDLE || s_LastImageViews[renderTargetIndex][i] != imageView) {
                if (s_MRTDescriptorSets[renderTargetIndex][i] != VK_NULL_HANDLE) {
                    ImGui_ImplVulkan_RemoveTexture(s_MRTDescriptorSets[renderTargetIndex][i]);
                    s_MRTDescriptorSets[renderTargetIndex][i] = VK_NULL_HANDLE;
                }
                s_MRTDescriptorSets[renderTargetIndex][i] = ImGui_ImplVulkan_AddTexture(s_MRTSampler, imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                s_LastImageViews[renderTargetIndex][i] = imageView;
            }

            ImGui::Image((ImTextureID)s_MRTDescriptorSets[renderTargetIndex][i], ImVec2(cellWidth, cellHeight));
        } else {
            ImGui::Button("通道不可用", ImVec2(cellWidth, cellHeight));
        }

        ImGui::EndGroup();
    }

    ImGui::End();
}

} // namespace Editor