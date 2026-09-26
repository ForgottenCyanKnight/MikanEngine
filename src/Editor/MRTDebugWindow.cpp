#include "Editor/MRTDebugWindow.h"
#include "imgui/imgui.h"
#include "Core/I18n.h"
#include "imgui_impl_vulkan.h"
#include "RenderTarget.h"
#include "Rendering/PostProcessChain.h"
#include "EngineGlobal.h"
#include "VulkanManager.h"
#include <algorithm>
#include <vector>

extern MIKAN_API RenderTarget g_SceneRenderTarget;
extern MIKAN_API RenderTarget g_GameRenderTarget;

namespace Editor {

MRTDebugWindow& MRTDebugWindow::GetInstance() {
    static MRTDebugWindow instance;
    return instance;
}

// ===== 共享预览资源 =====

// 不透明视图缓存：G-buffer 附件的 alpha 常被挪作数据（材质附件 A=自发光强度、
// 探针解析 A=覆盖掩码），而 ImGui 以 SrcAlpha 混合绘制图片——alpha=0 的调试图
// 会整个变透明露出窗口背景，看起来"显示黑色"（RenderDoc 不混合所以正常）。
// 用组件重映射 {R,G,B,ONE} 的独立视图强制采样 alpha=1。视图随设备销毁，不单独清理。
struct OpaqueViewCache { VkImageView view; VkImage image; };
static VkImageView GetOpaqueView(VkImage image, VkFormat format) {
    static std::vector<OpaqueViewCache> s_Cache;
    if (image == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    for (const auto& c : s_Cache) if (c.image == image) return c.view;
    VkImageViewCreateInfo vi = {};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.components = { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G,
                      VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_ONE };
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageView v = VK_NULL_HANDLE;
    if (vkCreateImageView(g_Device, &vi, g_Allocator, &v) != VK_SUCCESS) return VK_NULL_HANDLE;
    s_Cache.push_back({ v, image });
    return v;
}

// 线性 clamp 采样器（MRT 通道与后处理预览共用；颜色类纹理足够）
static VkSampler EnsurePreviewSampler() {
    static VkSampler s_Sampler = VK_NULL_HANDLE;
    if (s_Sampler == VK_NULL_HANDLE) {
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
        VkResult err = vkCreateSampler(g_Device, &samplerInfo, g_Allocator, &s_Sampler);
        check_vk_result(err);
    }
    return s_Sampler;
}

// 为 imageView 建立 ImGui 描述符集；view 变化（链重建/resize）时自动重建
static VkDescriptorSet PreviewDescriptorFor(VkImageView imageView, VkImageView& lastView, VkDescriptorSet& lastSet) {
    if (imageView == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    if (lastSet != VK_NULL_HANDLE && lastView == imageView) return lastSet;
    if (lastSet != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(lastSet);
        lastSet = VK_NULL_HANDLE;
    }
    lastSet = ImGui_ImplVulkan_AddTexture(EnsurePreviewSampler(), imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    lastView = imageView;
    return lastSet;
}

// ===== MRT 通道模式（原有功能） =====

static void RenderMrtMode() {
    static int renderTargetIndex = 0;
    static int lastRenderTargetIndex = -1;
    const char* renderTargetNames[] = { "场景视图", "游戏视图" };
    ImGui::Combo(Tr("渲染目标"), &renderTargetIndex, renderTargetNames, IM_ARRAYSIZE(renderTargetNames));

    static VkDescriptorSet s_MRTDescriptorSets[2][4] = {};
    static VkImageView s_LastImageViews[2][4] = {};

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

    ImGui::Text(Tr("渲染目标信息: %d x %d"), currentRenderTarget->GetWidth(), currentRenderTarget->GetHeight());
    ImGui::Separator();

    const char* channelNames[] = { Tr("颜色通道 (albedo)"), Tr("法线通道 (八面体编码)"), Tr("材质PBR (metallic/roughness/ao)"), Tr("运动矢量 (TAA)") };

    float cellWidth = ImGui::GetContentRegionAvail().x * 0.5f - ImGui::GetStyle().ItemSpacing.x;
    float cellHeight = cellWidth * 0.75f;

    for (int i = 0; i < 4; i++) {
        if (i % 2 == 1) ImGui::SameLine();

        ImGui::BeginGroup();
        ImGui::Text("%s", channelNames[i]);

        // 用不透明视图采样：材质附件 alpha=自发光（常为 0），直接采样会被
        // ImGui 的 SrcAlpha 混合整个变透明。
        VkImageView imageView = currentRenderTarget->GetColorImageView(i);
        VkImageView opaqueView = VK_NULL_HANDLE;
        if (imageView != VK_NULL_HANDLE) {
            VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
            switch (i) {
                case 1: fmt = currentRenderTarget->GetNormalFormat(); break;
                case 2: fmt = currentRenderTarget->GetMaterialFormat(); break;
                case 3: fmt = currentRenderTarget->GetMotionVectorFormat(); break;
                default: fmt = currentRenderTarget->GetMainColorFormat(); break;
            }
            opaqueView = GetOpaqueView(currentRenderTarget->GetColorImage(i), fmt);
        }
        VkDescriptorSet ds = PreviewDescriptorFor(opaqueView, s_LastImageViews[renderTargetIndex][i], s_MRTDescriptorSets[renderTargetIndex][i]);
        if (ds != VK_NULL_HANDLE) {
            ImGui::Image((ImTextureID)ds, ImVec2(cellWidth, cellHeight));
        } else {
            ImGui::Button(Tr("通道不可用"), ImVec2(cellWidth, cellHeight));
        }
        ImGui::EndGroup();
    }
}

// ===== 后处理链模式 =====

static constexpr int kMaxChainPreviewSlots = 10; // 槽位0=输出, 1..8=输入, 余量
struct ChainPreviewCache {
    VkImageView lastView = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
};

// 把 pass 输入源解析为可采样的 VkImageView（统一走不透明视图，见 GetOpaqueView）。
// pass:<name> → 该 pass 中间附件；链入口/G-Buffer 源 → 对应视图的附件；
// 其余外部源（skyrt/ibl/cloudrt/history 等）归 RenderTarget 之外，返回空显示占位。
static VkImageView ResolveChainSource(const std::string& src, PostProcessChain& chain, RenderTarget* target) {
    if (src.rfind("pass:", 0) == 0) {
        const std::string passName = src.substr(5);
        for (const auto& p : chain.GetPasses()) {
            if (p.name == passName) {
                return GetOpaqueView(chain.GetPassOutputImage(passName), p.outputFormat);
            }
        }
        return VK_NULL_HANDLE;
    }
    if (target == nullptr) return VK_NULL_HANDLE;
    if (src == "composite") return target->GetCompositeImageView();
    if (src == "gbuffer0") return GetOpaqueView(target->GetColorImage(0), target->GetMainColorFormat());
    if (src == "gbuffer1") return GetOpaqueView(target->GetColorImage(1), target->GetNormalFormat());
    if (src == "gbuffer2") return GetOpaqueView(target->GetColorImage(2), target->GetMaterialFormat());
    if (src == "depth") return target->GetDepthImageView();
    return VK_NULL_HANDLE;
}

static const char* FormatName(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R16G16B16A16_SFLOAT:    return "RGBA16F";
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return "R11G11B10";
        case VK_FORMAT_R16G16_SFLOAT:          return "RG16F";
        case VK_FORMAT_R8G8B8A8_UNORM:         return "RGBA8";
        case VK_FORMAT_R8_UNORM:               return "R8";
        case VK_FORMAT_R16_UNORM:              return "R16";
        default:                               return "其他格式";
    }
}

static void RenderChainMode() {
    static int chainIndex = 0;
    static int lastChainIndex = -1;
    static int selectedPass = -1;

    // 紧凑头部：链选择与统计信息同一行（模式 combo 由 Render() 以 SameLine 衔接）
    ImGui::TextUnformatted(Tr("链"));
    ImGui::SameLine();
    ImGui::PushItemWidth(150);
    const char* chainNames[] = { Tr("场景视图链"), Tr("游戏视图链"), Tr("游戏输出链(swap)") };
    ImGui::Combo("##chain", &chainIndex, chainNames, IM_ARRAYSIZE(chainNames));
    ImGui::PopItemWidth();

    PostProcessChain* chain = (chainIndex == 0) ? &g_SceneChain : (chainIndex == 1) ? &g_GameChain : &g_SwapChain;
    // 链的 G-Buffer/合成源挂在其对应视图上；swap 链消费游戏视图的输出
    RenderTarget* target = (chainIndex == 0) ? &g_SceneRenderTarget : &g_GameRenderTarget;

    // 预览描述符缓存：链变化时整体重建（pass 中间附件随链重建销毁）
    static ChainPreviewCache s_Cache[kMaxChainPreviewSlots];
    if (chainIndex != lastChainIndex) {
        for (auto& c : s_Cache) {
            if (c.set != VK_NULL_HANDLE) ImGui_ImplVulkan_RemoveTexture(c.set);
            c = ChainPreviewCache{};
        }
        lastChainIndex = chainIndex;
        selectedPass = -1;
    }

    if (!chain->IsBuilt() || chain->GetPassCount() == 0) {
        ImGui::TextDisabled(Tr("链未构建（对应视图未渲染，或该模式未启用后处理链）"));
        return;
    }

    const auto& passes = chain->GetPasses();
    const int passCount = chain->GetPassCount();

    ImGui::SameLine();
    ImGui::Text(Tr("pass 数: %d（启用 %d）"), passCount, chain->GetEnabledPassCount());
    ImGui::Separator();

    // 左：pass 列表；右：选中 pass 的输出/输入两列网格
    const float listWidth = 180.0f;
    ImGui::BeginChild("##pass_list", ImVec2(listWidth, 0), true);
    for (int i = 0; i < passCount; i++) {
        const auto& def = passes[i];
        const bool enabled = chain->IsPassEnabled(def.name);
        std::string label = def.name + (enabled ? "" : Tr(" (已禁用)"));
        if (!enabled) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.47f, 0.52f, 1.0f));
        if (ImGui::Selectable(label.c_str(), selectedPass == i)) {
            if (selectedPass != i) {
                selectedPass = i;
                for (auto& c : s_Cache) {
                    if (c.set != VK_NULL_HANDLE) ImGui_ImplVulkan_RemoveTexture(c.set);
                    c = ChainPreviewCache{};
                }
            }
        }
        if (!enabled) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s\nshader: %s", def.name.c_str(), def.shader.c_str());
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("##pass_detail", ImVec2(0, 0), true);
    if (selectedPass < 0 || selectedPass >= passCount) {
        ImGui::TextDisabled(Tr("选择左侧 pass 查看输入/输出"));
        ImGui::EndChild();
        return;
    }
    const auto& def = passes[selectedPass];

    ImGui::Text("%s  [%s]%s", def.name.c_str(), def.shader.c_str(), chain->IsPassEnabled(def.name) ? "" : Tr("  (已禁用)"));
    ImGui::Text(Tr("格式: %s  scale: %.2f"), FormatName(def.outputFormat), def.scale);
    ImGui::Separator();

    // 两列网格：第 1 格 = 输出，其后每个输入槽一格。缩略图悬停显示大图。
    const float gridW = ImGui::GetContentRegionAvail().x;
    const float cellW = (gridW - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    const float cellH = cellW * 0.5625f;
    const ImGuiIO& io = ImGui::GetIO();
    const float zoomW = std::min(io.DisplaySize.x * 0.42f, 640.0f);
    const float zoomH = zoomW * 0.5625f;

    int column = 0;
    auto DrawCell = [&](const std::string& label, VkImageView view,
                        ChainPreviewCache& cache, const char* placeholder) {
        if (column % 2 == 1) ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextUnformatted(label.c_str());
        VkDescriptorSet ds = PreviewDescriptorFor(view, cache.lastView, cache.set);
        if (ds != VK_NULL_HANDLE) {
            ImGui::Image((ImTextureID)ds, ImVec2(cellW, cellH));
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Image((ImTextureID)ds, ImVec2(zoomW, zoomH));
                ImGui::EndTooltip();
            }
        } else {
            ImGui::Button(placeholder, ImVec2(cellW, cellH));
        }
        ImGui::EndGroup();
        column++;
    };

    // 输出（第 1 格）：末 pass 无中间附件。同样用不透明视图（链中间附件的
    // alpha 常被用作数据，如探针解析的覆盖掩码）。
    {
        VkImageView outView = chain->GetPassOutputView(def.name);
        if (outView != VK_NULL_HANDLE) {
            outView = GetOpaqueView(chain->GetPassOutputImage(def.name), def.outputFormat);
        }
        const char* placeholder = outView == VK_NULL_HANDLE
            ? (def.enabled ? Tr("输出到显示附件") : Tr("（已禁用）")) : "";
        DrawCell(Tr("输出"), outView, s_Cache[0], placeholder);
    }

    // 输入槽位（第 2 格起）
    int cacheSlot = 1;
    for (const auto& in : def.inputs) {
        if (cacheSlot >= kMaxChainPreviewSlots) break;
        VkImageView view = ResolveChainSource(in.source, *chain, target);
        std::string label = Tr("槽") + std::to_string(in.slot) + " " + in.source;
        const char* placeholder = (view == VK_NULL_HANDLE) ? Tr("外部源，无预览") : "";
        DrawCell(label, view, s_Cache[cacheSlot], placeholder);
        cacheSlot++;
    }

    ImGui::EndChild();
}

void MRTDebugWindow::Render() {
    if (!m_visible) return;

    ImGui::Begin(I18n::WindowTitle("渲染管线预览", "editor.pipeline_preview").c_str(), &m_visible);

    // 紧凑头部：模式选择（后处理链模式下链选择/统计与本行衔接）
    ImGui::TextUnformatted(Tr("模式"));
    ImGui::SameLine();
    ImGui::PushItemWidth(110);
    static int previewMode = 0;
    const char* modeNames[] = { Tr("MRT 通道"), Tr("后处理链") };
    ImGui::Combo("##preview_mode", &previewMode, modeNames, IM_ARRAYSIZE(modeNames));
    ImGui::PopItemWidth();
    ImGui::Separator();

    if (previewMode == 0) {
        RenderMrtMode();
    } else {
        RenderChainMode();
    }

    ImGui::End();
}

} // namespace Editor
