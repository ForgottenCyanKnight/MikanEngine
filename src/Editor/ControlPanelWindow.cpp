#include "Editor/ControlPanelWindow.h"
#include "imgui/imgui.h"
#include "Core/I18n.h"
#include "Core/PipelineSettings.h"
#include "Core/DlssFrameGeneration.h"
#include "Rendering/RayTracing/RayTracingQualityOptions.h"
#include "Editor/UiId.h"
#include "Rendering/PostProcessChain.h"
#include "Core/VulkanPostProcessChains.h"
#include "EngineGlobal.h"
#include "VulkanManager.h"
#include "Camera.h"
#include "Rendering/RenderStats.h"
#include "Rendering/Denoising/DlssRayReconstruction.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Editor {

ControlPanelWindow& ControlPanelWindow::GetInstance() {
    static ControlPanelWindow instance;
    return instance;
}

namespace {
// 千位分隔格式化（性能数字更快扫读）
std::string FormatThousands(uint64_t v) {
    std::string s = std::to_string(v);
    for (int pos = static_cast<int>(s.size()) - 3; pos > 0; pos -= 3)
        s.insert(static_cast<size_t>(pos), ",");
    return s;
}
} // namespace

void ControlPanelWindow::Render() {
    // 每个 UI 帧读走渲染侧计数（读时清零），即使窗口隐藏也保持读数新鲜，
    // 重新打开时显示的就是最近一帧的统计而不是打开前的累计。
    Rendering::RenderStatsSnapshot stats = Rendering::RenderStats::Get().TakeSnapshot();
    if (!m_visible) return;
    
    ImGui::Begin(I18n::WindowTitle("控制面板", "editor.control_panel").c_str(), &m_visible, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::Separator();
    
    // 模式切换
    ImGui::Text(Tr("运行模式:"));
    int runMode = (g_RunMode == RunMode::Editor) ? 0 : 1;
    if (ImGui::RadioButton(Tr("编辑器"), &runMode, 0)) { g_RunMode = RunMode::Editor; }
    ImGui::SameLine();
    if (ImGui::RadioButton(Tr("游戏"), &runMode, 1)) { g_RunMode = RunMode::Game; }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(Tr("编辑器模式: 完整UI，离屏渲染\n游戏模式: 直接渲染到屏幕，更高性能"));
    }

    // 语言切换已移至主菜单栏"设置 → 语言 / Language"（全局唯一入口）。

    ImGui::Separator();
    
    // 垂直同步控制
    bool vsync = g_VSyncEnabled;
    if (ImGui::Checkbox(Tr("垂直同步"), &vsync))
    {
        SetVSync(vsync);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(Tr("启用垂直同步以限制FPS为显示器刷新率。\n禁用以解除限制FPS渲染"));
    }
    
    bool frameGeneration = Core::DlssFG::IsRequested();
    ImGui::BeginDisabled(!Core::DlssFG::IsSupported());
    if (ImGui::Checkbox(Tr("DLSS 2x 帧生成"), &frameGeneration))
    {
        if (frameGeneration && g_VSyncEnabled) SetVSync(false);
        Core::DlssFG::SetRequested(frameGeneration);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(Tr("可在局内切换，每个真实帧最多生成一帧。\n开启时自动关闭垂直同步；暂停或输入无效时停止生成。\n开关重建交换链，可能短暂卡顿。"));
    if (!Core::DlssFG::IsInitialized())
        ImGui::TextDisabled(Tr("帧生成运行库未加载，需重启启用支持"));
    else if (!Core::DlssFG::IsSupported())
        ImGui::TextDisabled(Tr("当前设备或系统不支持帧生成"));
    else if (frameGeneration)
        ImGui::TextDisabled(Core::DlssFG::IsEnabled() ? Tr("帧生成：已开启") : Tr("帧生成：等待有效渲染输入（需关闭垂直同步）"));

    // 三重缓冲控制
    bool tripleBuffering = g_TripleBufferingEnabled;
    if (ImGui::Checkbox(Tr("三重缓冲"), &tripleBuffering))
    {
        SetTripleBuffering(tripleBuffering);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(Tr("启用三重缓冲以减少输入延迟。\n需要足够的GPU内存。"));
    }
    
    // 不加 static：数组元素是 Tr 结果，须每帧按当前语言重算（static 会缓存启动时的中文指针）
    const char* fsModes[] = { Tr("窗口"), Tr("桌面全屏（无边框）"), Tr("独占全屏") };
    int fs = g_FullscreenMode;
    if (ImGui::Combo(Tr("全屏模式"), &fs, fsModes, 3))
    {
        SetFullscreenMode(fs);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(Tr("独占全屏绕开 Windows DWM 合成（窗口模式 present 平台税约 0.6-0.9ms）。\n切换后系统自动重建交换链。"));
    }
    
    ImGui::Separator();
    // 帧率显示: 用引擎 g_FPS(主循环实测,与游戏内右上角一致);
    // ImGui::GetIO().Framerate 是 ImGui 内部平滑统计,高帧率下明显偏低(观测偏差,非真实性能)
    {
        float fps = (g_FPS > 1.0f) ? g_FPS : ImGui::GetIO().Framerate;
        ImGui::Text(Tr("应用平均 %.3f ms/帧 (%.1f FPS)"), 1000.0f / fps, fps);
        if (Core::DlssFG::IsRequested())
            ImGui::TextDisabled(Tr("上述 FPS 为真实渲染帧，不包含生成帧"));
    }
    
    ImGui::Separator();
    const char* estimatorModes[]={Tr("NEE/MIS（默认）"),Tr("ReSTIR DI/GI（旧实验）"),Tr("ReSTIR PT（完整路径重放）")};
    const char* resolutionTiers[]={"540p (Performance)","720p (Quality)","1080p (DLAA)"};
    int resolutionTier=mikan::denoising::GetDlssResolutionTier();
    ImGui::BeginDisabled(!mikan::denoising::CanSelectDlssResolutionTier());
    if(ImGui::Combo(Tr("光追输入分辨率"),&resolutionTier,resolutionTiers,3))
        mikan::denoising::SetDlssResolutionTier(resolutionTier);
    ImGui::EndDisabled();
    if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(Tr("适用于 DLSS RR/SR。1080p 输出时输入分别为 540p、720p、1080p。\n其他输出尺寸按相同比例缩放；输出尺寸保持不变。\n切换会重建资源并重置历史，可能短暂卡顿。\n本次运行有效，重启恢复启动配置（默认 Quality）。"));
    int estimatorMode=mikan::rt::GetRestirPathTracing()?2:(mikan::rt::GetRestirEstimatorRestir()?1:0);
    if(ImGui::Combo(Tr("光照估计器"),&estimatorMode,estimatorModes,3)){
        mikan::rt::SetRestirPathTracing(estimatorMode==2);
        mikan::rt::SetRestirEstimatorRestir(estimatorMode==1);
    }
    ImGui::SameLine();ImGui::TextDisabled("(?)");
    if(ImGui::IsItemHovered())
        ImGui::SetTooltip(Tr("NEE/MIS：默认。\nReSTIR DI/GI：保留旧实验。\nReSTIR PT：完整随机路径重放、双向 pairwise MIS、独立时间/空间合并，历史 M 上限20。\nPSS 重放会增加追踪开销；尚未实现 Prime Enhanced 的 Hybrid 重连接和 Compact retrace。\n本次运行有效，也可用 MIKAN_HWRT_RESTIR_PT=1 启动。"));
    const char* neeTiers[]={Tr("性能（DI1/GI1）"),Tr("平衡（DI4/GI2）"),Tr("质量（GI4）")};
    const auto neeSamples=mikan::rt::GetNeeDiffuseSamples();
    int neeTier=neeSamples==4?2:(neeSamples==2?1:0);
    ImGui::BeginDisabled(mikan::rt::GetRestirEstimatorRestir()||mikan::rt::GetRestirPathTracing());
    if(ImGui::Combo(Tr("NEE 光追档次"),&neeTier,neeTiers,3))
        mikan::rt::SetNeeDiffuseSamples(1u<<neeTier);
    ImGui::EndDisabled();
    ImGui::SameLine();ImGui::TextDisabled("(?)");
    if(ImGui::IsItemHovered())
        ImGui::SetTooltip(Tr("普通表面与镜面末端的独立漫反射GI路径数。\n切换即时生效，并重置重建历史。\n性能DI1/GI1，平衡DI4/GI2；启动DI参数可覆盖档位。\n切换性能/平衡同时选择DI预算，反弹深度与RR设置保持不变。\n本次运行有效，重启默认性能档DI1/GI1。"));

    const bool restirPathActive = mikan::rt::GetRestirEstimatorRestir()||mikan::rt::GetRestirPathTracing();
    bool temporalReuse = mikan::rt::GetRestirTemporalReuse();
    ImGui::BeginDisabled(!restirPathActive);
    if(ImGui::Checkbox(Tr("ReSTIR 时间复用"),&temporalReuse))
        mikan::rt::SetRestirTemporalReuse(temporalReuse);
    ImGui::EndDisabled();
    ImGui::SameLine();ImGui::TextDisabled("(?)");
    if(ImGui::IsItemHovered())
        ImGui::SetTooltip(mikan::rt::GetRestirPathTracing()?Tr("PT：递归复用上一帧最终路径 reservoir，M 上限20。\n当前场景中双向重放，几何/光照变化及模式切换使历史失效。\n启动可通过 MIKAN_HWRT_RESTIR_TEMPORAL=1 开启。"):Tr("旧 DI/GI：仅复用上一帧 fresh reservoir，最多一帧。\n启动可通过 MIKAN_HWRT_RESTIR_TEMPORAL=1 开启。"));

    // 相机信息（直接显示）
    ImGui::Text(Tr("相机信息:"));
    glm::vec3 pos = g_Camera.Position;
    ImGui::Text(Tr("位置: (%.2f, %.2f, %.2f)"), pos.x, pos.y, pos.z);
    glm::vec3 front = g_Camera.Front;
    ImGui::Text(Tr("朝向: (%.2f, %.2f, %.2f)"), front.x, front.y, front.z);

    ImGui::Separator();
    // 后处理模块开关（作用于当前激活的三条链：场景/游戏/输出；
    // 改动即时应用并持久化到 engine/pipeline_settings.json）
    if (ImGui::CollapsingHeader(Tr("后处理"))) {
        auto applyAll = [](auto&& fn) -> bool {
            bool changed = false;
            changed |= fn(g_SceneChain);
            changed |= fn(g_GameChain);
            changed |= fn(g_SwapChain);
            if (changed) RequestPostProcessRebuild();
            return changed;
        };
        auto& settings = PipelineSettings::GetInstance();
        settings.EnsureLoaded();

        for (const auto& group : g_GameChain.GetGroups()) {
            // 组默认状态取自链本身；设置文件里有覆盖值则以覆盖值显示
            bool enabled = g_GameChain.IsGroupEnabled(group);
            auto it = settings.GetGroups().find(group);
            if (it != settings.GetGroups().end()) enabled = it->second;
            std::string marker = std::string("pp.") + group;
            if (ImGui::Checkbox(Editor::LabelId(group.c_str(), marker.c_str()).c_str(), &enabled)) {
                settings.SetGroup(group, enabled);
                applyAll([&](PostProcessChain& c) { return c.SetGroupEnabled(group, enabled); });
            }
        }

        const auto& profiles = g_GameChain.GetAaProfiles();
        if (!profiles.empty()) {
            int aaIndex = 0;
            std::vector<const char*> aaLabels;
            std::vector<std::string> aaNames;
            aaNames.push_back("none");
            for (const auto& profile : profiles) aaNames.push_back(profile.first);
            for (size_t i = 0; i < aaNames.size(); i++) {
                aaLabels.push_back(aaNames[i].c_str());
                if (aaNames[i] == settings.GetAaProfile()) aaIndex = (int)i;
            }
            ImGui::TextUnformatted(Tr("抗锯齿"));
            ImGui::SameLine();
            ImGui::PushItemWidth(120);
            if (ImGui::Combo("##pp_aa", &aaIndex, aaLabels.data(), (int)aaLabels.size())) {
                const std::string& chosen = aaIndex == 0 ? std::string() : aaNames[(size_t)aaIndex];
                settings.SetAaProfile(chosen);
                applyAll([&](PostProcessChain& c) {
                    return chosen.empty() ? c.SetAaProfile(std::string()) : c.SetAaProfile(chosen);
                });
            }
            ImGui::PopItemWidth();
        }

        // ===== 链拓扑读取 + 逐 pass 开关（按链顺序，group 标注；游戏链为基准）=====
        ImGui::Separator();
        ImGui::TextDisabled(Tr("pass 数: %d（启用 %d）"),
                            g_GameChain.GetPassCount(), g_GameChain.GetEnabledPassCount());
        if (ImGui::BeginChild("##pp_passes", ImVec2(0, 220.0f), true)) {
            for (const auto& def : g_GameChain.GetPasses()) {
                const bool chainEnabled = g_GameChain.IsPassEnabled(def.name);
                bool enabled = chainEnabled;
                // 组勾选框已覆盖同组开关；pass 行只显示组归属与单独开关
                std::string row = def.name;
                if (!def.group.empty()) row += "  [" + def.group + "]";
                // pass 名在链内唯一，作 ID 无冲突；开关同步三条链
                if (ImGui::Checkbox(row.c_str(), &enabled)) {
                    applyAll([&](PostProcessChain& c) {
                        return c.SetPassEnabled(def.name, enabled);
                    });
                }
            }
        }
        ImGui::EndChild();
        ImGui::TextDisabled(Tr("开关即时生效并同步场景/游戏/输出三条链；组勾选框优先于单个 pass"));
    }

    ImGui::Separator();
    // 性能信息（可折叠；stats 已在函数开头每帧读走，折叠与否不影响新鲜度）
    if (ImGui::CollapsingHeader(Tr("性能信息"))) {
        ImGui::Text(Tr("绘制调用: %s"), FormatThousands(stats.drawCalls).c_str());
        ImGui::Text(Tr("三角面: %s"), FormatThousands(stats.triangles).c_str());
        ImGui::Text(Tr("模型实例: %s"), FormatThousands(stats.modelInstances).c_str());
        ImGui::Text(Tr("实例数: %s"), FormatThousands(stats.gpuInstances).c_str());
        ImGui::Text(Tr("模型种类: %s"), FormatThousands(stats.modelKinds).c_str());
        if (stats.indirectDraws > 0) {
            ImGui::Text(Tr("间接绘制(体素): %s"), FormatThousands(stats.indirectDraws).c_str());
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(Tr("上一次 UI 刷新以来的渲染统计（读时清零）。\n"                              "绘制调用: 实际 vkCmdDraw 次数——实例化合并在\n"                              "  一次调用内的所有实例只算 1 次（不含 ImGui UI）。\n"                              "三角面: 索引/非索引绘制按 index|vertex*instance/3 估算；\n"                              "  体素 multi-draw 间接绘制的三角形在 GPU 端，仅计调用数。\n"                              "模型实例: 视锥剔除后实际提交的模型实体数（同模型\n"                              "  多份各算一个，不去重）。\n"                              "实例数: 各绘制调用 instanceCount 的总和（GPU 实际\n"                              "  处理的实例单元；逐子网格批次会把同一实体重复计入）。\n"                              "模型种类: 实际提交的模型组数 = 去重后的网格模型种数，\n"                              "  用于核对实例化是否把同类模型合批。"));
        }
    }


    ImGui::End();
}

} // namespace Editor
