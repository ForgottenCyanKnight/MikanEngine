#include "Editor/ControlPanelWindow.h"
#include "imgui/imgui.h"
#include "EngineGlobal.h"
#include "VulkanManager.h"
#include "Camera.h"

namespace Editor {

ControlPanelWindow& ControlPanelWindow::GetInstance() {
    static ControlPanelWindow instance;
    return instance;
}

void ControlPanelWindow::Render() {
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
    
    // 全屏模式（2026-08-17）：0=窗口 1=桌面全屏（无边框） 2=独占全屏（绕开 DWM，present 税 0.6-0.9ms → ~0.05ms）
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
    
    
    ImGui::End();
}

} // namespace Editor
