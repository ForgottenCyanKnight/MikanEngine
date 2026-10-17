#include "Editor/GameViewWindow.h"
#include "Editor/ToolbarWindow.h"
#include "ECS/SceneECS.h"
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <iostream>
#include <vector>
#include <string>

// The CPU layout test needs toolbar state, not editor startup or a GPU backend.
namespace Editor {
ToolbarWindow& ToolbarWindow::GetInstance() { static ToolbarWindow instance; return instance; }
}
int main() {
    auto* context = ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1600, 1000);
    io.DeltaTime = 1.0f / 60.0f;
    io.ConfigErrorRecoveryEnableAssert = false;
    std::vector<std::string> errors;
    context->ErrorCallbackUserData = &errors;
    context->ErrorCallback = [](ImGuiContext*, void* data, const char* message) {
        static_cast<std::vector<std::string>*>(data)->push_back(message);
    };
    unsigned char* pixels; int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    ECS::SceneECS::GetInstance().Init();
    auto& view = Editor::GameViewWindow::GetInstance();
    for (bool texture : {false, true}) {
        // AddImage only records a draw command; this synthetic descriptor is never submitted to Vulkan.
        view.SetDescriptorSet(texture ? (VkDescriptorSet)(uintptr_t)1 : VK_NULL_HANDLE);
        for (const auto size : {ImVec2(600, 900), ImVec2(1200, 400), ImVec2(640, 360)}) {
            for (bool collapsed : {false, true}) {
                for (int frame = 0; frame < 3; ++frame) {
                    ImGui::NewFrame();
                    ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Always);
                    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
                    ImGui::SetNextWindowCollapsed(collapsed, ImGuiCond_Always);
                    bool show = true;
                    view.Render(show);
                    ImGui::Render();
                }
            }
        }
    }
    view.SetDescriptorSet(VK_NULL_HANDLE);
    ECS::SceneECS::GetInstance().Shutdown();
    for (const auto& error : errors) std::cerr << error << '\n';
    ImGui::DestroyContext(context);
    std::cout << (errors.empty() ? "PASS" : "FAIL") << ": " << errors.size()
              << " ImGui errors across textured, empty, portrait, landscape and collapsed GameView frames\n";
    return errors.empty() ? 0 : 1;
}
