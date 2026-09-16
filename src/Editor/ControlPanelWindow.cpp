#include "Editor/ControlPanelWindow.h"
#include "imgui/imgui.h"
#include "EngineGlobal.h"
#include "VulkanManager.h"
#include "Camera.h"
#include "Rendering/RenderStats.h"

#include <cstdint>
#include <string>

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
    
    ImGui::Begin("控制面板", &m_visible, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::Separator();
    
    // 模式切换
    ImGui::Text("运行模式:");
    int runMode = (g_RunMode == RunMode::Editor) ? 0 : 1;
    if (ImGui::RadioButton("编辑器", &runMode, 0)) { g_RunMode = RunMode::Editor; }
    ImGui::SameLine();
    if (ImGui::RadioButton("游戏", &runMode, 1)) { g_RunMode = RunMode::Game; }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("编辑器模式: 完整UI，离屏渲染\n游戏模式: 直接渲染到屏幕，更高性能");
    }
    
    ImGui::Separator();
    
    // 垂直同步控制
    bool vsync = g_VSyncEnabled;
    if (ImGui::Checkbox("垂直同步", &vsync))
    {
        SetVSync(vsync);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("启用垂直同步以限制FPS为显示器刷新率。\n禁用以解除限制FPS渲染");
    }
    
    // 三重缓冲控制
    bool tripleBuffering = g_TripleBufferingEnabled;
    if (ImGui::Checkbox("三重缓冲", &tripleBuffering))
    {
        SetTripleBuffering(tripleBuffering);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("启用三重缓冲以减少输入延迟。\n需要足够的GPU内存。");
    }
    
    static const char* fsModes[] = { "窗口", "桌面全屏（无边框）", "独占全屏" };
    int fs = g_FullscreenMode;
    if (ImGui::Combo("全屏模式", &fs, fsModes, 3))
    {
        SetFullscreenMode(fs);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("独占全屏绕开 Windows DWM 合成（窗口模式 present 平台税约 0.6-0.9ms）。\n切换后系统自动重建交换链。");
    }
    
    ImGui::Separator();
    // 帧率显示: 用引擎 g_FPS(主循环实测,与游戏内右上角一致);
    // ImGui::GetIO().Framerate 是 ImGui 内部平滑统计,高帧率下明显偏低(观测偏差,非真实性能)
    {
        float fps = (g_FPS > 1.0f) ? g_FPS : ImGui::GetIO().Framerate;
        ImGui::Text("应用平均 %.3f ms/帧 (%.1f FPS)", 1000.0f / fps, fps);
    }
    
    ImGui::Separator();
    // 相机信息（直接显示）
    ImGui::Text("相机信息:");
    glm::vec3 pos = g_Camera.Position;
    ImGui::Text("位置: (%.2f, %.2f, %.2f)", pos.x, pos.y, pos.z);
    glm::vec3 front = g_Camera.Front;
    ImGui::Text("朝向: (%.2f, %.2f, %.2f)", front.x, front.y, front.z);

    ImGui::Separator();
    // 性能信息（可折叠；stats 已在函数开头每帧读走，折叠与否不影响新鲜度）
    if (ImGui::CollapsingHeader("性能信息")) {
        ImGui::Text("绘制调用: %s", FormatThousands(stats.drawCalls).c_str());
        ImGui::Text("三角面: %s", FormatThousands(stats.triangles).c_str());
        ImGui::Text("模型实例: %s", FormatThousands(stats.modelInstances).c_str());
        ImGui::Text("实例数: %s", FormatThousands(stats.gpuInstances).c_str());
        ImGui::Text("模型种类: %s", FormatThousands(stats.modelKinds).c_str());
        if (stats.indirectDraws > 0) {
            ImGui::Text("间接绘制(体素): %s", FormatThousands(stats.indirectDraws).c_str());
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("上一次 UI 刷新以来的渲染统计（读时清零）。\n"
                              "绘制调用: 实际 vkCmdDraw 次数——实例化合并在\n"
                              "  一次调用内的所有实例只算 1 次（不含 ImGui UI）。\n"
                              "三角面: 索引/非索引绘制按 index|vertex*instance/3 估算；\n"
                              "  体素 multi-draw 间接绘制的三角形在 GPU 端，仅计调用数。\n"
                              "模型实例: 视锥剔除后实际提交的模型实体数（同模型\n"
                              "  多份各算一个，不去重）。\n"
                              "实例数: 各绘制调用 instanceCount 的总和（GPU 实际\n"
                              "  处理的实例单元；逐子网格批次会把同一实体重复计入）。\n"
                              "模型种类: 实际提交的模型组数 = 去重后的网格模型种数，\n"
                              "  用于核对实例化是否把同类模型合批。");
        }
    }


    ImGui::End();
}

} // namespace Editor
