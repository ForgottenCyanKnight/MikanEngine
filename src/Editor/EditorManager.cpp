// Editor Manager
// Manages the ImGui docking layout and asset/resource windows
#define IMGUI_DEFINE_MATH_OPERATORS
#define GLM_ENABLE_EXPERIMENTAL
#include "EditorManager.h"

#include "Editor/MaterialEditorWindow.h"
#include "Editor/MRTDebugWindow.h"
#include "Editor/ControlPanelWindow.h"
#include "Editor/PropertiesWindow.h"
#include "Editor/HierarchyWindow.h"
#include "Editor/ToolbarWindow.h"
#include "Editor/MainMenuBar.h"
#include "Editor/GameViewWindow.h"
#include "Editor/SceneViewWindow.h"
#include "Editor/DockingLayout.h"
#include "Editor/AssetsWindow.h"
#include "Editor/TextureCacheManager.h"
#include "Editor/MaterialEditor.h"
#include "Editor/PreviewGeneratorHelper.h"
#include "Editor/GizmoMode.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"
#include "ImGuizmo.h"
#include "EngineGlobal.h"
#include "EngineConfig.h"
#include "Core/ProjectManager.h"
#include "ECS/ECS.h"
#include "ECS/SceneECS.h"
#include "SceneSerializer.h"
#include "InputController.h"
#include "Camera.h"
#include "RenderTarget.h"
#include "SceneRenderer.h"
#include "PreviewGenerator.h"
#include "RendererBase.h"
#include "ModelLoader.h"
#include "ECS/PhysicsSystem.h"
#include "AABB.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_filesystem.h>
#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#endif

extern MIKAN_API SceneRenderer g_SceneRenderer;
extern MIKAN_API std::shared_ptr<ECS::PhysicsSystem> g_PhysicsSystemPtr;

#include <SDL3_image/SDL_image.h>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <cstdarg>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>

EditorManager& EditorManager::GetInstance() {
    static EditorManager instance;
    return instance;
}

EditorManager::EditorManager() 
{
}

void EditorManager::Init() {
    std::string assetsRootPath;
    
#ifdef __ANDROID__
    assetsRootPath = "assets";
#else
    // 用 ProjectManager 探测引擎根（向上查找 engine/shaders/spv），避免硬编码 exe 相对层级。
    // （exe 在 out/build/x64-Release/x64-Release 时 "../../../assets" 会多跳一层，解析到不存在的 out/assets）
    assetsRootPath = ProjectManager::GetInstance().GetAssetsDir();
    if (!assetsRootPath.empty() && (assetsRootPath.back() == '/' || assetsRootPath.back() == '\\')) {
        assetsRootPath.pop_back(); // 去掉尾斜杠，与旧的 "…/assets" 形态一致
    }
    if (assetsRootPath.empty()) {
        assetsRootPath = "assets";
    }
#endif
    
    Editor::AssetsWindow::GetInstance().SetAssetsRootPath(assetsRootPath);
    
    m_TexturePool = std::make_unique<TexturePool>(g_Device, g_PhysicalDevice, g_CommandPool, g_Queue, g_Allocator);
    
    Editor::AssetsWindow::GetInstance().SetTexturePool(m_TexturePool.get());
    
    // 资产窗口图标 = 引擎系统资产（engine/textures/，见 EngineAssets 校验清单）
    std::string folder1Path = EngineConfig::GetEngineTexturePath("folder1.png");
    std::string folder2Path = EngineConfig::GetEngineTexturePath("folder2.png");
    std::string filePath = EngineConfig::GetEngineTexturePath("file.png");
    std::string materialPath = EngineConfig::GetEngineTexturePath("material.png");
    
    m_TexturePool->LoadTexture2D("folder_icon", folder1Path);
    m_TexturePool->LoadTexture2D("folder_back_icon", folder2Path);
    m_TexturePool->LoadTexture2D("file_icon", filePath);
    m_TexturePool->LoadTexture2D("material_icon", materialPath);
    
    Editor::AssetsWindow::GetInstance().SetIconNames("folder_icon", "folder_back_icon", "file_icon", "material_icon");
    Editor::TextureCacheManager::GetInstance().SetIconNames("folder_icon", "folder_back_icon", "file_icon", "material_icon");
    
    LoadJoystickConfig();
}

void EditorManager::InitImGui(SDL_Window* window, int width, int height, float main_scale) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
#ifndef __ANDROID__
    // 将 imgui.ini 锁到可执行文件目录。默认 "imgui.ini" 相对进程工作目录解析,
    // 不同启动方式(双击 / 脚本 / IDE)会写到不同位置,布局时灵时不灵。
    // Android 不设:SDL_GetBasePath 返回 APK 内部只读路径,保持默认行为。
    if (const char* basePath = SDL_GetBasePath()) {
        static std::string iniPath = std::string(basePath) + "imgui.ini";
        io.IniFilename = iniPath.c_str(); // io 不拷贝字符串,须 static 保生命周期
    }
#endif
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.Framerate = 0.0f;
    
    std::string fontPath;
    
#ifdef __ANDROID__
    fontPath = EngineConfig::DEFAULT_FONT;
#else
    std::string exeDir = "";
    const char* basePath = SDL_GetBasePath();
    if (basePath != nullptr) {
        exeDir = std::string(basePath);
    }
    // GetFullPath now returns an absolute project path via ProjectManager
        fontPath = EngineConfig::GetFullPath(EngineConfig::GetFontPath("simhei.ttf").c_str());
#endif
    
    ImFontConfig config;
    config.OversampleH = 1;
    config.OversampleV = 1;
    config.PixelSnapH = true;
    
    // Adaptive font: base size on the DISPLAY diagonal (a large monitor yields
    // larger fonts regardless of the window size), scaled by DPI content scale.
    // main_scale = contentScale * 0.7, so recover the real scale (>= 1.0).
    float dpiScale = std::max(main_scale / 0.7f, 1.0f);
    float dispW = 1920.0f, dispH = 1080.0f;
    SDL_DisplayID dispId = SDL_GetDisplayForWindow(window);
    SDL_Rect dispBounds = {};
    if (dispId != 0 && SDL_GetDisplayBounds(dispId, &dispBounds))
    {
        dispW = (float)dispBounds.w;
        dispH = (float)dispBounds.h;
    }
    float screenDiagonal = sqrt(dispW * dispW + dispH * dispH);
    float baseFontSize = screenDiagonal / 160.0f;
    float minFontSize = 14.0f * dpiScale;
    float maxFontSize = 32.0f * dpiScale;
    float fontScale = std::clamp(baseFontSize, minFontSize, maxFontSize);
    
    bool fontLoaded = false;
    SDL_IOStream* fontIo = SDL_IOFromFile(fontPath.c_str(), "rb");
    if (fontIo != nullptr) {
        Sint64 fileSize = SDL_GetIOSize(fontIo);
        if (fileSize > 0) {
            void* fontData = SDL_malloc((size_t)fileSize);
            if (fontData && SDL_ReadIO(fontIo, fontData, (size_t)fileSize) == (size_t)fileSize) {
                config.FontDataOwnedByAtlas = true;
                if (io.Fonts->AddFontFromMemoryTTF(fontData, (int)fileSize, fontScale, &config, io.Fonts->GetGlyphRangesChineseFull())) {
                    fontLoaded = true;
                }
            }
        }
        SDL_CloseIO(fontIo);
    }
    
    if (!fontLoaded) {
        fprintf(stderr, "Warning: Failed to load Chinese font. Using default font instead.\n");
    }

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();

    // Unified professional spacing (scaled by DPI below)
    style.WindowPadding     = ImVec2(8.0f, 8.0f);
    style.FramePadding      = ImVec2(6.0f, 4.0f);
    style.ItemSpacing       = ImVec2(6.0f, 5.0f);
    style.ItemInnerSpacing  = ImVec2(5.0f, 5.0f);
    style.WindowRounding    = 6.0f;
    style.FrameRounding     = 4.0f;
    style.GrabRounding      = 4.0f;
    style.TabRounding       = 4.0f;
    style.ChildRounding     = 4.0f;
    style.PopupRounding     = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.WindowBorderSize  = 1.0f;
    style.FrameBorderSize   = 0.0f;
    style.ChildBorderSize   = 1.0f;
    style.TabBorderSize     = 0.0f;
    style.WindowTitleAlign  = ImVec2(0.5f, 0.5f);

    // Professional dark palette (VS Code / Unity style)
    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg]            = ImVec4(0.09f, 0.10f, 0.12f, 1.00f);
    c[ImGuiCol_ChildBg]             = ImVec4(0.11f, 0.12f, 0.14f, 1.00f);
    c[ImGuiCol_PopupBg]             = ImVec4(0.11f, 0.12f, 0.14f, 0.98f);
    c[ImGuiCol_Border]              = ImVec4(0.20f, 0.22f, 0.25f, 1.00f);
    c[ImGuiCol_FrameBg]             = ImVec4(0.15f, 0.16f, 0.19f, 1.00f);
    c[ImGuiCol_FrameBgHovered]      = ImVec4(0.20f, 0.22f, 0.26f, 1.00f);
    c[ImGuiCol_FrameBgActive]       = ImVec4(0.26f, 0.28f, 0.32f, 1.00f);
    c[ImGuiCol_TitleBg]             = ImVec4(0.13f, 0.14f, 0.17f, 1.00f);
    c[ImGuiCol_TitleBgActive]       = ImVec4(0.18f, 0.20f, 0.24f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]    = ImVec4(0.13f, 0.14f, 0.17f, 1.00f);
    c[ImGuiCol_MenuBarBg]           = ImVec4(0.11f, 0.12f, 0.15f, 1.00f);
    c[ImGuiCol_Header]              = ImVec4(0.20f, 0.22f, 0.26f, 1.00f);
    c[ImGuiCol_HeaderHovered]       = ImVec4(0.24f, 0.26f, 0.30f, 1.00f);
    c[ImGuiCol_HeaderActive]        = ImVec4(0.28f, 0.30f, 0.35f, 1.00f);
    c[ImGuiCol_Tab]                 = ImVec4(0.14f, 0.15f, 0.18f, 1.00f);
    c[ImGuiCol_TabHovered]          = ImVec4(0.24f, 0.26f, 0.30f, 1.00f);
    c[ImGuiCol_TabActive]           = ImVec4(0.20f, 0.22f, 0.26f, 1.00f);
    c[ImGuiCol_TabUnfocused]        = ImVec4(0.12f, 0.13f, 0.16f, 1.00f);
    c[ImGuiCol_TabUnfocusedActive]  = ImVec4(0.17f, 0.18f, 0.22f, 1.00f);
    c[ImGuiCol_Button]              = ImVec4(0.17f, 0.18f, 0.22f, 1.00f);
    c[ImGuiCol_ButtonHovered]       = ImVec4(0.26f, 0.28f, 0.32f, 1.00f);
    c[ImGuiCol_ButtonActive]        = ImVec4(0.30f, 0.32f, 0.37f, 1.00f);
    c[ImGuiCol_Separator]           = ImVec4(0.24f, 0.26f, 0.29f, 1.00f);
    c[ImGuiCol_Text]                = ImVec4(0.88f, 0.89f, 0.92f, 1.00f);
    c[ImGuiCol_TextDisabled]        = ImVec4(0.45f, 0.47f, 0.52f, 1.00f);
    c[ImGuiCol_CheckMark]           = ImVec4(0.30f, 0.62f, 1.00f, 1.00f);
    c[ImGuiCol_SliderGrab]          = ImVec4(0.30f, 0.62f, 1.00f, 0.80f);
    c[ImGuiCol_SliderGrabActive]    = ImVec4(0.36f, 0.68f, 1.00f, 1.00f);
    c[ImGuiCol_ScrollbarBg]         = ImVec4(0.09f, 0.10f, 0.12f, 1.00f);
    c[ImGuiCol_ScrollbarGrab]       = ImVec4(0.28f, 0.30f, 0.35f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered]= ImVec4(0.34f, 0.36f, 0.42f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.40f, 0.42f, 0.48f, 1.00f);
    c[ImGuiCol_DockingPreview]      = ImVec4(0.30f, 0.62f, 1.00f, 0.30f);
    c[ImGuiCol_DockingEmptyBg]      = ImVec4(0.15f, 0.16f, 0.19f, 1.00f);
    c[ImGuiCol_ModalWindowDimBg]  = ImVec4(0.0f, 0.0f, 0.0f, 0.60f);

    // Scale the whole style with the font size so spacing stays proportional
    // on large screens / hi-dpi displays.
    style.ScaleAllSizes(fontScale / 13.0f);
    style.FontScaleDpi = main_scale;

    ImGui_ImplSDL3_InitForVulkan(window);
    
    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.Instance = g_Instance;
    init_info.PhysicalDevice = g_PhysicalDevice;
    init_info.Device = g_Device;
    init_info.QueueFamily = g_QueueFamily;
    init_info.Queue = g_Queue;
    init_info.DescriptorPool = VK_NULL_HANDLE;
    init_info.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_IMAGE_SAMPLER_POOL_SIZE * 4;
    init_info.MinImageCount = g_MinImageCount;
    init_info.ImageCount = g_MainWindowData.ImageCount;
    init_info.Allocator = g_Allocator;
    init_info.PipelineInfoMain.RenderPass = g_MainWindowData.RenderPass;
    init_info.PipelineInfoMain.Subpass = 0;
    init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init_info.CheckVkResultFn = check_vk_result;
    ImGui_ImplVulkan_Init(&init_info);
}

void EditorManager::ShutdownImGui() {
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

void EditorManager::Cleanup() {
    m_TexturePool.reset();
    Editor::TextureCacheManager::GetInstance().ClearCache();
}

void EditorManager::RenderDockingLayout() {
    Editor::DockingLayout::GetInstance().RenderDockingLayout();
    
    m_showSceneView = Editor::DockingLayout::GetInstance().m_showSceneView;
    m_showGameView = Editor::DockingLayout::GetInstance().m_showGameView;
    m_showAssetsWindow = Editor::DockingLayout::GetInstance().m_showAssetsWindow;
    m_showGizmoAxis = Editor::DockingLayout::GetInstance().m_showGizmoAxis;
    m_isGameRunning = Editor::DockingLayout::GetInstance().m_isGameRunning;
    m_isGamePaused = Editor::DockingLayout::GetInstance().m_isGamePaused;
}

void EditorManager::RenderSceneView() {
    Editor::SceneViewWindow::GetInstance().Render(m_showSceneView);
}

void EditorManager::RenderSceneViewWithGizmo(const glm::mat4& view, const glm::mat4& proj, ECS::Entity selectedEntity, GizmoMode gizmoMode, bool showAxis) {
    Editor::SceneViewWindow::GetInstance().SetGizmoMode(static_cast<Editor::GizmoMode>(gizmoMode));
    Editor::SceneViewWindow::GetInstance().SetShowGizmoAxis(showAxis);
    Editor::SceneViewWindow::GetInstance().RenderWithGizmo(m_showSceneView, view, proj, selectedEntity);
}

void EditorManager::RenderGizmo(const glm::mat4& view, const glm::mat4& proj, ECS::Entity selectedEntity, GizmoMode gizmoMode, bool showAxis) {
    if (selectedEntity == ECS::INVALID_ENTITY || !showAxis) return;
    
    auto& coordinator = ECS::Coordinator::GetInstance();
    if (!coordinator.HasComponent<ECS::TransformComponent>(selectedEntity)) return;
    
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
    
    ImVec2 windowPos = ImGui::GetWindowPos();
    ImVec2 contentMin = ImGui::GetCursorScreenPos();
    ImVec2 contentMax = ImGui::GetContentRegionAvail();
    
    ImGuizmo::SetRect(contentMin.x, contentMin.y, contentMax.x, contentMax.y);
    
    glm::mat4 modelMatrix = ECS::SceneECS::GetInstance().GetWorldMatrix(selectedEntity);
    
    ImGuizmo::OPERATION operation = ImGuizmo::TRANSLATE;
    switch (gizmoMode) {
        case GizmoMode::Translate: operation = ImGuizmo::TRANSLATE; break;
        case GizmoMode::Rotate: operation = ImGuizmo::ROTATE; break;
        case GizmoMode::Scale: operation = ImGuizmo::SCALE; break;
    }
    
    ImGuizmo::MODE mode = ImGuizmo::LOCAL;
    
    float deltaMatrix[16] = {0};
    bool manipulated = ImGuizmo::Manipulate(
        glm::value_ptr(view),
        glm::value_ptr(proj),
        operation,
        mode,
        glm::value_ptr(modelMatrix),
        deltaMatrix
    );
    
    if (manipulated || ImGuizmo::IsUsing()) {
        auto& transform = coordinator.GetComponent<ECS::TransformComponent>(selectedEntity);
        glm::vec3 skew;
        glm::vec4 perspective;
        glm::decompose(modelMatrix, transform.scale, transform.rotation, transform.position, skew, perspective);
        transform.MarkDirty(); // ImGuizmo 写回 scale/rotation/position,世界矩阵缓存需失效
        if (g_PhysicsSystemPtr) {
            g_PhysicsSystemPtr->SyncModelTransforms();
        }
    }
}

void EditorManager::RenderAssetsWindow() {
    Editor::AssetsWindow::GetInstance().Render(m_showAssetsWindow);
}

void EditorManager::RenderGameView() {
    Editor::DockingLayout::GetInstance().RenderGameView();
}

void EditorManager::RenderMainToolbar() {
    Editor::DockingLayout::GetInstance().RenderMainToolbar();
}

void EditorManager::SetAssetsRootPath(const std::string& path) {
    Editor::AssetsWindow::GetInstance().SetAssetsRootPath(path);
}

void EditorManager::SaveJoystickConfig() {
    const char* basePath = SDL_GetBasePath();
    if (basePath == nullptr) {
        fprintf(stderr, "Failed to get base path for saving joystick config\n");
        return;
    }
    
    std::string configPath = std::string(basePath) + "joystick_config.json";
    
    std::ofstream file(configPath);
    if (!file.is_open()) {
        fprintf(stderr, "Failed to open joystick config file for writing: %s\n", configPath.c_str());
        return;
    }
    
    file << "{\n";
    file << "  \"moveBaseRadius\": " << m_joystickConfig.moveBaseRadius << ",\n";
    file << "  \"moveStickRadius\": " << m_joystickConfig.moveStickRadius << ",\n";
    file << "  \"moveMaxDistance\": " << m_joystickConfig.moveMaxDistance << ",\n";
    file << "  \"moveOffsetX\": " << m_joystickConfig.moveOffsetX << ",\n";
    file << "  \"moveOffsetY\": " << m_joystickConfig.moveOffsetY << ",\n";
    file << "  \"lookBaseRadius\": " << m_joystickConfig.lookBaseRadius << ",\n";
    file << "  \"lookStickRadius\": " << m_joystickConfig.lookStickRadius << ",\n";
    file << "  \"lookMaxDistance\": " << m_joystickConfig.lookMaxDistance << ",\n";
    file << "  \"lookOffsetX\": " << m_joystickConfig.lookOffsetX << ",\n";
    file << "  \"lookOffsetY\": " << m_joystickConfig.lookOffsetY << ",\n";
    file << "  \"sensitivity\": " << m_joystickConfig.sensitivity << ",\n";
    file << "  \"autoSave\": " << (m_joystickConfig.autoSave ? "true" : "false") << "\n";
    file << "}\n";
    
    file.close();
    printf("Joystick config saved to: %s\n", configPath.c_str());
}

void EditorManager::LoadJoystickConfig() {
    const char* basePath = SDL_GetBasePath();
    if (basePath == nullptr) {
        fprintf(stderr, "Failed to get base path for loading joystick config\n");
        return;
    }
    
    std::string configPath = std::string(basePath) + "joystick_config.json";
    
    std::ifstream file(configPath);
    if (!file.is_open()) {
        return;
    }
    
    std::string line;
    while (std::getline(file, line)) {
        if (line.find("\"moveBaseRadius\"") != std::string::npos) {
            sscanf(line.c_str(), "  \"moveBaseRadius\": %f,", &m_joystickConfig.moveBaseRadius);
        } else if (line.find("\"moveStickRadius\"") != std::string::npos) {
            sscanf(line.c_str(), "  \"moveStickRadius\": %f,", &m_joystickConfig.moveStickRadius);
        } else if (line.find("\"moveMaxDistance\"") != std::string::npos) {
            sscanf(line.c_str(), "  \"moveMaxDistance\": %f,", &m_joystickConfig.moveMaxDistance);
        } else if (line.find("\"moveOffsetX\"") != std::string::npos) {
            sscanf(line.c_str(), "  \"moveOffsetX\": %f,", &m_joystickConfig.moveOffsetX);
        } else if (line.find("\"moveOffsetY\"") != std::string::npos) {
            sscanf(line.c_str(), "  \"moveOffsetY\": %f,", &m_joystickConfig.moveOffsetY);
        } else if (line.find("\"lookBaseRadius\"") != std::string::npos) {
            sscanf(line.c_str(), "  \"lookBaseRadius\": %f,", &m_joystickConfig.lookBaseRadius);
        } else if (line.find("\"lookStickRadius\"") != std::string::npos) {
            sscanf(line.c_str(), "  \"lookStickRadius\": %f,", &m_joystickConfig.lookStickRadius);
        } else if (line.find("\"lookMaxDistance\"") != std::string::npos) {
            sscanf(line.c_str(), "  \"lookMaxDistance\": %f,", &m_joystickConfig.lookMaxDistance);
        } else if (line.find("\"lookOffsetX\"") != std::string::npos) {
            sscanf(line.c_str(), "  \"lookOffsetX\": %f,", &m_joystickConfig.lookOffsetX);
        } else if (line.find("\"lookOffsetY\"") != std::string::npos) {
            sscanf(line.c_str(), "  \"lookOffsetY\": %f,", &m_joystickConfig.lookOffsetY);
        } else if (line.find("\"sensitivity\"") != std::string::npos) {
            sscanf(line.c_str(), "  \"sensitivity\": %f,", &m_joystickConfig.sensitivity);
        } else if (line.find("\"autoSave\"") != std::string::npos) {
            std::string value = line.substr(line.find(":") + 1);
            m_joystickConfig.autoSave = (value.find("true") != std::string::npos);
        }
    }
    
    file.close();
    printf("Joystick config loaded from: %s\n", configPath.c_str());
    ApplyJoystickConfig();
}

void EditorManager::ApplyJoystickConfig() {
    InputController::JoystickConfig config;
    config.moveBaseRadius = m_joystickConfig.moveBaseRadius;
    config.moveStickRadius = m_joystickConfig.moveStickRadius;
    config.moveMaxDistance = m_joystickConfig.moveMaxDistance;
    config.moveOffsetX = m_joystickConfig.moveOffsetX;
    config.moveOffsetY = m_joystickConfig.moveOffsetY;
    config.lookBaseRadius = m_joystickConfig.lookBaseRadius;
    config.lookStickRadius = m_joystickConfig.lookStickRadius;
    config.lookMaxDistance = m_joystickConfig.lookMaxDistance;
    config.lookOffsetX = m_joystickConfig.lookOffsetX;
    config.lookOffsetY = m_joystickConfig.lookOffsetY;
    config.sensitivity = m_joystickConfig.sensitivity;
    config.autoSave = m_joystickConfig.autoSave;
    g_InputController.SetJoystickConfig(config);
}

bool EditorManager::SaveMaterialToFile(const std::string& filePath, const ECS::MaterialComponent& material) {
    return Editor::MaterialEditor::GetInstance().SaveMaterialToFile(filePath, material);
}

bool EditorManager::LoadMaterialFromFile(const std::string& filePath, ECS::MaterialComponent& material) {
    return Editor::MaterialEditor::GetInstance().LoadMaterialFromFile(filePath, material);
}

void EditorManager::EditMaterial(const std::string& filePath) {
    Editor::MaterialEditor::GetInstance().EditMaterial(filePath);
}

void EditorManager::UpdateAssetCache() {
    Editor::AssetsWindow::GetInstance().UpdateAssetCache();
}
