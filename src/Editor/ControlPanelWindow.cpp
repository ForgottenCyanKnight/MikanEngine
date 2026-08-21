#include "Editor/ControlPanelWindow.h"
#include "imgui/imgui.h"
#include "EngineGlobal.h"
#include "VulkanManager.h"
#include "Camera.h"
#include "InputController.h"

#include "EditorManager.h"

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
    
    // 相机碰撞选项
    bool cameraCollision = g_CameraCollisionEnabled;
    if (ImGui::Checkbox("相机碰撞", &cameraCollision)) {
        g_CameraCollisionEnabled = cameraCollision;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("启用相机碰撞检测，使用AABB测试方式，比物理系统更高效");
    }
    
    ImGui::TextDisabled("操作提示 : (?)");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("WASD键: 移动\n空格键: 上升\nShift键: 下降\n鼠标右键: 视角控制\nTab键: 切换鼠标捕获模式\nAlt+G: 显示/隐藏Gizmo\nAlt+F: 显示/隐藏控制面板");
    }
    
    ImGui::Separator();
    
    // 渲染控制
    ImGui::Text("渲染控制:");
    
    // 网格渲染控制（场景视图: 世界网格 + 原点 RGB 坐标轴）
    bool showGrid = EditorManager::GetInstance().ShowGrid();
    if (ImGui::Checkbox("网格渲染", &showGrid)) {
        EditorManager::GetInstance().SetShowGrid(showGrid);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("显示/隐藏场景网格和世界原点坐标轴（RGB 三色轴），帮助定位和布局场景物体");
    }
    
    // 输入控制
    ImGui::Text("输入控制:");
    bool touchEnabled = g_InputController.IsTouchEnabled();
    if (ImGui::Checkbox("虚拟摇杆", &touchEnabled))
    {
        g_InputController.SetTouchEnabled(touchEnabled);
        if (touchEnabled) {
            EditorManager::GetInstance().ApplyJoystickConfig();
        }
    }
    
    if (touchEnabled) {
        ImGui::Text("左摇杆: 移动  右屏幕滑动: 视角");
        
        // 虚拟摇杆配置
        if (ImGui::CollapsingHeader("虚拟摇杆配置")) {
            bool configChanged = false;
            EditorManager::JoystickConfig& joystickConfig = EditorManager::GetInstance().GetJoystickConfig();
            
            // 移动摇杆配置
            if (ImGui::TreeNode("移动摇杆")) {
                configChanged |= ImGui::SliderFloat("底座半径", &joystickConfig.moveBaseRadius, 30.0f, 150.0f);
                configChanged |= ImGui::SliderFloat("摇杆半径", &joystickConfig.moveStickRadius, 20.0f, 100.0f);
                configChanged |= ImGui::SliderFloat("最大距离", &joystickConfig.moveMaxDistance, 30.0f, 120.0f);
                configChanged |= ImGui::SliderFloat("X偏移", &joystickConfig.moveOffsetX, 50.0f, 300.0f);
                configChanged |= ImGui::SliderFloat("Y偏移", &joystickConfig.moveOffsetY, 50.0f, 300.0f);
                ImGui::TreePop();
            }
            
            // 视角摇杆配置
            if (ImGui::TreeNode("视角摇杆")) {
                configChanged |= ImGui::SliderFloat("底座半径", &joystickConfig.lookBaseRadius, 30.0f, 150.0f);
                configChanged |= ImGui::SliderFloat("摇杆半径", &joystickConfig.lookStickRadius, 20.0f, 100.0f);
                configChanged |= ImGui::SliderFloat("最大距离", &joystickConfig.lookMaxDistance, 30.0f, 120.0f);
                configChanged |= ImGui::SliderFloat("X偏移", &joystickConfig.lookOffsetX, 0.0f, 300.0f);
                configChanged |= ImGui::SliderFloat("Y偏移", &joystickConfig.lookOffsetY, 50.0f, 300.0f);
                ImGui::TreePop();
            }
            
            // 灵敏度配置
            if (ImGui::TreeNode("灵敏度")) {
                configChanged |= ImGui::SliderFloat("移动灵敏度", &joystickConfig.sensitivity, 0.1f, 2.0f);
                ImGui::TreePop();
            }
            
            // 保存模式
            if (ImGui::TreeNode("保存模式")) {
                bool autoSave = joystickConfig.autoSave;
                configChanged |= ImGui::Checkbox("自动保存", &autoSave);
                joystickConfig.autoSave = autoSave;
                
                if (ImGui::Button("保存配置")) {
                    EditorManager::GetInstance().SaveJoystickConfig();
                    EditorManager::GetInstance().ApplyJoystickConfig();
                }
                ImGui::SameLine();
                if (ImGui::Button("重置默认")) {
                    joystickConfig = EditorManager::JoystickConfig();
                    EditorManager::GetInstance().ApplyJoystickConfig();
                    if (joystickConfig.autoSave) {
                        EditorManager::GetInstance().SaveJoystickConfig();
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("加载配置")) {
                    EditorManager::GetInstance().LoadJoystickConfig();
                }
                ImGui::TreePop();
            }
            
            // 自动保存配置
            if (configChanged && joystickConfig.autoSave) {
                EditorManager::GetInstance().SaveJoystickConfig();
                EditorManager::GetInstance().ApplyJoystickConfig();
            }
        }
    }
    
    ImGui::End();
}

} // namespace Editor