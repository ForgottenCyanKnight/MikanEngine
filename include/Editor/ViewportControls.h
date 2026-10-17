#pragma once
#include "Core/ViewportResolution.h"
#include <imgui/imgui.h>
#include <algorithm>

namespace Editor {
inline bool DrawViewportResolutionPreset(const char* id, int& width, int& height) {
    bool changed = false;
    const auto& current = ViewportResolution::Closest(width, height);
    if (ImGui::BeginCombo(id, current.label)) {
        for (const auto& preset : ViewportResolution::Presets) {
            const bool selected = preset.width == width && preset.height == height;
            if (ImGui::Selectable(preset.label, selected) && !selected) {
                width = preset.width; height = preset.height; changed = true;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}
// Fit the image without changing its render target size; keep input/gizmos in
// the same centered rectangle as the displayed 16:9 image.
inline ImVec2 FitViewportImage(const ImVec2& available) {
    const float width = std::max(0.0f, std::min(available.x, available.y * (16.0f / 9.0f)));
    return ImVec2(width, width * (9.0f / 16.0f));
}
inline ImVec2 CenterViewportImage(const ImVec2& available) {
    const auto size = FitViewportImage(available);
    const auto cursor = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(cursor.x + (available.x - size.x) * 0.5f,
                                   cursor.y + (available.y - size.y) * 0.5f));
    return size;
}
} // namespace Editor
