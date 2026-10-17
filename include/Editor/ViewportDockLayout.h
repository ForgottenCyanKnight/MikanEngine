#pragma once
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <algorithm>
#include <cmath>

namespace Editor {
// Only adjust the standard left | (center | right) row. Custom docking trees
// and the bottom asset/log panel are preserved.
inline bool FitDefaultViewportDockRow(ImGuiDockNode* center, float uiScale) {
    if (!center || center->IsSplitNode()) return false;
    auto* inner = center->ParentNode;
    auto* row = inner ? inner->ParentNode : nullptr;
    if (!row || inner->SplitAxis != ImGuiAxis_X || row->SplitAxis != ImGuiAxis_X ||
        inner->ChildNodes[0] != center || row->ChildNodes[1] != inner ||
        !row->ChildNodes[0] || row->ChildNodes[0]->IsSplitNode() ||
        !inner->ChildNodes[1] || inner->ChildNodes[1]->IsSplitNode()) return false;
    auto* left = row->ChildNodes[0];
    auto* right = inner->ChildNodes[1];
    const float separator = ImGui::GetStyle().DockingSeparatorSize;
    const float available = row->Size.x - 2.0f * separator;
    if (available <= 1.0f || row->Size.y <= ImGui::GetFrameHeight()) return false;
    const float minLeft = std::min(160.0f * uiScale, available * 0.25f);
    const float minRight = std::min(200.0f * uiScale, available * 0.25f);
    // Docked view windows have zero padding; the dock tab bar occupies one frame.
    const float imageHeight = row->Size.y - ImGui::GetFrameHeight();
    const float centerWidth = std::floor(std::clamp(imageHeight * (16.0f / 9.0f),
        1.0f, available - minLeft - minRight));
    const float sides = available - centerWidth;
    const float leftWidth = std::floor(std::clamp(
        left->Size.x + (sides - left->Size.x - right->Size.x) * 0.5f,
        minLeft, sides - minRight));
    const float rightWidth = sides - leftWidth;
    const auto position = row->Pos;
    auto place = [&](ImGuiDockNode* node, float x, float width) {
        ImGui::DockBuilderSetNodePos(node->ID, ImVec2(x, position.y));
        ImGui::DockBuilderSetNodeSize(node->ID, ImVec2(width, row->Size.y));
    };
    place(left, position.x, leftWidth);
    place(inner, position.x + leftWidth + separator, centerWidth + separator + rightWidth);
    place(center, inner->Pos.x, centerWidth);
    place(right, inner->Pos.x + centerWidth + separator, rightWidth);
    ImGui::MarkIniSettingsDirty();
    return true;
}
} // namespace Editor
