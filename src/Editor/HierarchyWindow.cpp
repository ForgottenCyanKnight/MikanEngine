#include "Editor/HierarchyWindow.h"
#include "Editor/EntityPresets.h"
#include "ECS/ECS.h"
#include "ECS/SceneECS.h"
#include "ECS/Components.h"
#include "ECS/ComponentRegistry.h"
#include "Camera.h"
#include <imgui/imgui.h>
#include <cstring>

extern MIKAN_API Camera g_Camera;

namespace Editor {

HierarchyWindow& HierarchyWindow::GetInstance() {
    static HierarchyWindow instance;
    return instance;
}

void HierarchyWindow::Render() {
    if (!m_visible) return;

    ImGui::Begin("层级", &m_visible);

    // 添加对象按钮
    if (ImGui::Button("+", ImVec2(30, 30))) {
        ImGui::OpenPopup("AddObjectPopup");
    }
    ImGui::SameLine();
    ImGui::Text("添加对象");

    // 添加对象弹出菜单(由预设表 EntityPresets 驱动:新增对象类别只需加一行数据)
    if (ImGui::BeginPopup("AddObjectPopup")) {
        const auto& presets = Editor::GetEntityPresets();
        const char* lastCategory = nullptr;
        for (const auto& preset : presets) {
            if (lastCategory == nullptr || strcmp(lastCategory, preset.category) != 0) {
                if (lastCategory != nullptr) ImGui::Separator();
                lastCategory = preset.category;
                ImGui::TextDisabled("%s", preset.category);
            }
            if (ImGui::MenuItem(preset.displayName)) {
                auto entity = ECS::SceneECS::GetInstance().CreateEmpty(preset.defaultName);
                for (const auto& compType : preset.components) {
                    auto* meta = ECS::ComponentRegistry::GetInstance().Find(compType);
                    if (meta) meta->addTo(entity);
                }
                if (preset.customize) preset.customize(entity);
                ECS::SceneECS::GetInstance().SetSelectedEntity(entity); // 保证所有预设创建后都被选中
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("创建父级(组)")) {
            auto parent = ECS::SceneECS::GetInstance().CreateEmpty("组");
            auto selected = ECS::SceneECS::GetInstance().GetSelectedEntity();
            if (selected != ECS::INVALID_ENTITY && ECS::SceneECS::GetInstance().GetParent(selected) == ECS::INVALID_ENTITY) {
                ECS::SceneECS::GetInstance().SetParent(selected, parent);
            }
            ECS::SceneECS::GetInstance().SetSelectedEntity(parent);
        }
        ImGui::EndPopup();
    }

    ImGui::Separator();

    // 显示场景对象列表
    auto rootEntities = ECS::SceneECS::GetInstance().GetRootEntities();
    ECS::Entity selectedEntity = ECS::SceneECS::GetInstance().GetSelectedEntity();

    for (const auto& entity : rootEntities) {
        if (entity == ECS::INVALID_ENTITY) continue;

        auto& coordinator = ECS::Coordinator::GetInstance();
        if (!coordinator.HasComponent<ECS::NameComponent>(entity)) continue;

        std::string name = coordinator.GetComponent<ECS::NameComponent>(entity).name;
        auto children = ECS::SceneECS::GetInstance().GetChildren(entity);

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (entity == selectedEntity) {
            flags |= ImGuiTreeNodeFlags_Selected;
        }
        if (children.empty()) {
            flags |= ImGuiTreeNodeFlags_Leaf;
        }

        ImGui::PushID(static_cast<int>(entity));

        // 行首固定列可见性开关（●/○，不受名字长度影响）
        RenderVisibilityToggle(entity);
        ImGui::SameLine();

        bool isOpen = ImGui::TreeNodeEx(name.c_str(), flags);

        // 拖拽源
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
            ECS::Entity draggedEntity = entity;
            ImGui::SetDragDropPayload("HIERARCHY_ENTITY", &draggedEntity, sizeof(ECS::Entity));
            ImGui::Text("移动: %s", name.c_str());
            ImGui::EndDragDropSource();
        }

        // 拖拽目标
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY")) {
                ECS::Entity draggedEntity = *(ECS::Entity*)payload->Data;
                if (draggedEntity != ECS::INVALID_ENTITY && draggedEntity != entity) {
                    ECS::SceneECS::GetInstance().SetParent(draggedEntity, entity);
                }
            }
            ImGui::EndDragDropTarget();
        }

        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
            ECS::SceneECS::GetInstance().SetSelectedEntity(entity);
        }

        // 右键菜单
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("删除")) {
                ECS::SceneECS::GetInstance().DestroyEntity(entity);
            }
            if (ImGui::MenuItem("复制")) {
                auto newObj = ECS::SceneECS::GetInstance().CreateEmpty(name + " (复制)");
                ECS::SceneECS::GetInstance().SetPosition(newObj, ECS::SceneECS::GetInstance().GetPosition(entity));
                ECS::SceneECS::GetInstance().SetRotation(newObj, coordinator.GetComponent<ECS::TransformComponent>(entity).rotation);
                ECS::SceneECS::GetInstance().SetScale(newObj, ECS::SceneECS::GetInstance().GetScale(entity));

                // 复制其他组件
                if (coordinator.HasComponent<ECS::MeshComponent>(entity)) {
                    auto mesh = coordinator.GetComponent<ECS::MeshComponent>(entity);
                    coordinator.AddComponent<ECS::MeshComponent>(newObj, mesh);
                }

                if (coordinator.HasComponent<ECS::VoxModelComponent>(entity)) {
                    auto vox = coordinator.GetComponent<ECS::VoxModelComponent>(entity);
                    coordinator.AddComponent<ECS::VoxModelComponent>(newObj, vox);
                }

                if (coordinator.HasComponent<ECS::RenderComponent>(entity)) {
                    auto render = coordinator.GetComponent<ECS::RenderComponent>(entity);
                    coordinator.AddComponent<ECS::RenderComponent>(newObj, render);
                }

                if (coordinator.HasComponent<ECS::MaterialComponent>(entity)) {
                    auto material = coordinator.GetComponent<ECS::MaterialComponent>(entity);
                    coordinator.AddComponent<ECS::MaterialComponent>(newObj, material);
                }

                ECS::SceneECS::GetInstance().SetSelectedEntity(newObj);
            }
            ImGui::EndPopup();
        }

        if (isOpen) {
            RenderHierarchyChildren(entity, selectedEntity);
            ImGui::TreePop();
        }

        ImGui::PopID();
    }

    // 空场景提示
    if (rootEntities.empty()) {
        ImGui::TextDisabled("场景中没有对象");
        ImGui::TextDisabled("点击 + 添加对象");
    }

    ImGui::End();
}

void HierarchyWindow::RenderHierarchyChildren(ECS::Entity parent, ECS::Entity selectedEntity) {
    auto children = ECS::SceneECS::GetInstance().GetChildren(parent);
    auto& coordinator = ECS::Coordinator::GetInstance();

    for (const auto& child : children) {
        if (child == ECS::INVALID_ENTITY) continue;
        if (!coordinator.HasComponent<ECS::NameComponent>(child)) continue;

        std::string name = coordinator.GetComponent<ECS::NameComponent>(child).name;
        auto grandChildren = ECS::SceneECS::GetInstance().GetChildren(child);

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (child == selectedEntity) {
            flags |= ImGuiTreeNodeFlags_Selected;
        }
        if (grandChildren.empty()) {
            flags |= ImGuiTreeNodeFlags_Leaf;
        }

        ImGui::PushID(static_cast<int>(child));

        // 行首固定列可见性开关（●/○，不受名字长度影响）
        RenderVisibilityToggle(child);
        ImGui::SameLine();

        bool isOpen = ImGui::TreeNodeEx(name.c_str(), flags);

        // 拖拽源
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
            ECS::Entity draggedEntity = child;
            ImGui::SetDragDropPayload("HIERARCHY_ENTITY", &draggedEntity, sizeof(ECS::Entity));
            ImGui::Text("移动: %s", name.c_str());
            ImGui::EndDragDropSource();
        }

        // 拖拽目标
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY")) {
                ECS::Entity draggedEntity = *(ECS::Entity*)payload->Data;
                if (draggedEntity != ECS::INVALID_ENTITY && draggedEntity != child) {
                    ECS::SceneECS::GetInstance().SetParent(draggedEntity, child);
                }
            }
            ImGui::EndDragDropTarget();
        }

        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
            ECS::SceneECS::GetInstance().SetSelectedEntity(child);
        }

        // 右键菜单
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("删除")) {
                ECS::SceneECS::GetInstance().DestroyEntity(child);
            }
            if (ImGui::MenuItem("复制")) {
                auto newObj = ECS::SceneECS::GetInstance().CreateEmpty(name + " (复制)");
                ECS::SceneECS::GetInstance().SetPosition(newObj, ECS::SceneECS::GetInstance().GetPosition(child));
                ECS::SceneECS::GetInstance().SetRotation(newObj, coordinator.GetComponent<ECS::TransformComponent>(child).rotation);
                ECS::SceneECS::GetInstance().SetScale(newObj, ECS::SceneECS::GetInstance().GetScale(child));

                // 复制其他组件
                if (coordinator.HasComponent<ECS::MeshComponent>(child)) {
                    auto mesh = coordinator.GetComponent<ECS::MeshComponent>(child);
                    coordinator.AddComponent<ECS::MeshComponent>(newObj, mesh);
                }

                if (coordinator.HasComponent<ECS::VoxModelComponent>(child)) {
                    auto vox = coordinator.GetComponent<ECS::VoxModelComponent>(child);
                    coordinator.AddComponent<ECS::VoxModelComponent>(newObj, vox);
                }

                if (coordinator.HasComponent<ECS::RenderComponent>(child)) {
                    auto render = coordinator.GetComponent<ECS::RenderComponent>(child);
                    coordinator.AddComponent<ECS::RenderComponent>(newObj, render);
                }

                if (coordinator.HasComponent<ECS::MaterialComponent>(child)) {
                    auto material = coordinator.GetComponent<ECS::MaterialComponent>(child);
                    coordinator.AddComponent<ECS::MaterialComponent>(newObj, material);
                }

                ECS::SceneECS::GetInstance().SetSelectedEntity(newObj);
            }
            if (ImGui::MenuItem("取消父级")) {
                ECS::SceneECS::GetInstance().RemoveParent(child);
            }

            ImGui::Separator();

            ImGui::EndPopup();
        }

        if (isOpen) {
            RenderHierarchyChildren(child, selectedEntity);
            ImGui::TreePop();
        }

        ImGui::PopID();
    }
}

void HierarchyWindow::RenderVisibilityToggle(unsigned int entity) {
    auto& scene = ECS::SceneECS::GetInstance();
    const bool vis = scene.IsVisible(entity);
    const float s = ImGui::GetFrameHeight() * 0.72f;  // 紧凑小按钮
    // 主题蓝（同 CheckMark/SliderGrab）；隐藏态灰
    const ImVec4 themeBlue(0.30f, 0.62f, 1.00f, 1.0f);
    const ImVec4 dimGray(0.38f, 0.40f, 0.44f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, vis ? themeBlue : dimGray);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));  // 让 ●/○ 完整显示
    if (ImGui::Button(vis ? "●" : "○", ImVec2(s, s))) {
        scene.SetVisible(entity, !vis);
    }
    ImGui::PopStyleVar();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(vis ? "隐藏对象" : "显示对象");
    }
    ImGui::PopStyleColor();
}

} // namespace Editor