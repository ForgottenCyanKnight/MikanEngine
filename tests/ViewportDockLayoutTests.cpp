#include "Editor/ViewportDockLayout.h"
#include <cassert>
#include <iostream>
#include <cstdlib>

int main() {
#ifdef _MSC_VER
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    auto* context = ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.DisplaySize = ImVec2(3840, 2160);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char* pixels; int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    ImGui::NewFrame();
    for (const auto size : {ImVec2(1920, 985), ImVec2(2560, 1300), ImVec2(1280, 665)}) {
        const auto root = ImGui::DockBuilderAddNode(0x123456, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodePos(root, ImVec2(0, 0));
        ImGui::DockBuilderSetNodeSize(root, size);
        auto middle = root;
        const auto bottom = ImGui::DockBuilderSplitNode(middle, ImGuiDir_Down, 0.29f, nullptr, &middle);
        const auto left = ImGui::DockBuilderSplitNode(middle, ImGuiDir_Left, 0.14f, nullptr, &middle);
        const auto right = ImGui::DockBuilderSplitNode(middle, ImGuiDir_Right, 0.18f, nullptr, &middle);
        auto* center = ImGui::DockBuilderGetNode(middle);
        const auto oldBottom = ImGui::DockBuilderGetNode(bottom)->Size;
        const float oldLeft = ImGui::DockBuilderGetNode(left)->Size.x;
        const float oldRight = ImGui::DockBuilderGetNode(right)->Size.x;
        assert(Editor::FitDefaultViewportDockRow(center, 1.0f));
        const float imageHeight = center->Size.y - ImGui::GetFrameHeight();
        const float expectedWidth = imageHeight * 16.0f / 9.0f;
        assert(std::abs(center->Size.x - expectedWidth) <= 1.0f);
        assert(ImGui::DockBuilderGetNode(bottom)->Size.x == oldBottom.x);
        assert(ImGui::DockBuilderGetNode(bottom)->Size.y == oldBottom.y);
        if (size.x >= 1920) {
            assert(ImGui::DockBuilderGetNode(left)->Size.x > oldLeft);
            assert(ImGui::DockBuilderGetNode(right)->Size.x > oldRight);
        }
        const auto firstSize = center->Size;
        assert(Editor::FitDefaultViewportDockRow(center, 1.0f));
        assert(center->Size.x == firstSize.x && center->Size.y == firstSize.y);
        // An unrelated/custom subtree must not be rearranged by the startup adjustment.
        assert(!Editor::FitDefaultViewportDockRow(ImGui::DockBuilderGetNode(bottom), 1.0f));
        ImGui::DockBuilderRemoveNode(root);
    }
    ImGui::Render();
    ImGui::DestroyContext(context);
    std::cout << "PASS: 16:9 dock content, extra width to both sides, bottom preserved, saved layout idempotence\n";
}
