#include "Editor/HierarchyWindow.h"
#include "Core/Log.h"
#include "Editor/EntityPresets.h"
#include "Core/I18n.h"
#include "ECS/ECS.h"
#include "ECS/SceneECS.h"
#include "ECS/Components.h"
#include "ECS/ComponentRegistry.h"
#include "Camera.h"
#include <imgui/imgui.h>
#include <cstring>
#include <chrono>
#include <cstdlib>
#include <cctype>

extern MIKAN_API Camera g_Camera;

namespace Editor {

namespace {

bool IsEditorCpuProfileEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("MIKAN_CPU_PROFILE");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

std::string FoldAscii(const std::string& value)
{
    std::string folded = value;
    for (char& character : folded) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return folded;
}

uint64_t g_hierarchyProfileFrames = 0;
double g_hierarchyProfileMs = 0.0;
uint64_t g_hierarchyProfileRoots = 0;

} // namespace

HierarchyWindow& HierarchyWindow::GetInstance() {
    static HierarchyWindow instance;
    return instance;
}

void HierarchyWindow::Render() {
    if (!m_visible) return;

    const bool cpuProfileEnabled = IsEditorCpuProfileEnabled();
    const auto profileStart = cpuProfileEnabled
        ? std::chrono::steady_clock::now()
        : std::chrono::steady_clock::time_point{};

    ImGui::Begin(I18n::WindowTitle("层级", "editor.hierarchy").c_str(), &m_visible);

    // 添加对象按钮
    if (ImGui::Button("+", ImVec2(30, 30))) {
        ImGui::OpenPopup("AddObjectPopup");
    }
    ImGui::SameLine();
    ImGui::Text(Tr("添加对象"));

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
                // 所有 3D 预设统一放到编辑器相机前方；2D 预设由 customize 挂到 Canvas，位置保持为 Canvas 局部坐标。
                if (strcmp(preset.category, "3D") == 0) {
                    Editor::PlaceInFrontOfCamera(entity);
                }
                ECS::SceneECS::GetInstance().SetSelectedEntity(entity); // 保证所有预设创建后都被选中
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem(Tr("创建父级(组)"))) {
            auto parent = ECS::SceneECS::GetInstance().CreateEmpty("组");
            auto selected = ECS::SceneECS::GetInstance().GetSelectedEntity();
            // 先定位父级，再挂接已有对象；SetParent 会保持子对象的世界位置不变。
            Editor::PlaceInFrontOfCamera(parent);
            if (selected != ECS::INVALID_ENTITY && ECS::SceneECS::GetInstance().GetParent(selected) == ECS::INVALID_ENTITY) {
                ECS::SceneECS::GetInstance().SetParent(selected, parent);
            }
            ECS::SceneECS::GetInstance().SetSelectedEntity(parent);
        }
        ImGui::EndPopup();
    }

    ImGui::Separator();

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint(
        "##HierarchySearch", Tr("搜索对象..."), m_searchBuffer, sizeof(m_searchBuffer),
        ImGuiInputTextFlags_EscapeClearsAll);
    m_searchText = FoldAscii(m_searchBuffer);
    const bool searchActive = !m_searchText.empty();
    m_searchSubtreeCache.clear();

    // 显示场景对象列表
    auto rootEntities = ECS::SceneECS::GetInstance().GetRootEntities();
    ECS::Entity selectedEntity = ECS::SceneECS::GetInstance().GetSelectedEntity();
    bool hasMatchingRoot = false;

    for (const auto& entity : rootEntities) {
        if (entity == ECS::INVALID_ENTITY) continue;

        auto& coordinator = ECS::Coordinator::GetInstance();
        if (!coordinator.HasComponent<ECS::NameComponent>(entity)) continue;
        if (searchActive && !SubtreeMatchesSearch(entity)) continue;
        hasMatchingRoot = true;

        std::string name = coordinator.GetComponent<ECS::NameComponent>(entity).name;
        auto children = ECS::SceneECS::GetInstance().GetChildren(entity);

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (entity == selectedEntity) {
            flags |= ImGuiTreeNodeFlags_Selected;
        }
        if (children.empty()) {
            flags |= ImGuiTreeNodeFlags_Leaf;
        }
        if (searchActive && !children.empty()) {
            ImGui::SetNextItemOpen(true, ImGuiCond_Always);
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
            ImGui::Text(Tr("移动: %s"), name.c_str());
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
            if (ImGui::MenuItem(Tr("删除"))) {
                ECS::SceneECS::GetInstance().DestroyEntity(entity);
            }
            if (ImGui::MenuItem(Tr("复制"))) {
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
            RenderHierarchyChildren(entity, selectedEntity, searchActive);
            ImGui::TreePop();
        }

        ImGui::PopID();
    }

    // 空场景提示
    if (rootEntities.empty()) {
        ImGui::TextDisabled(Tr("场景中没有对象"));
        ImGui::TextDisabled(Tr("点击 + 添加对象"));
    } else if (searchActive && !hasMatchingRoot) {
        ImGui::TextDisabled(Tr("没有匹配的对象"));
    }

    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
        !ImGui::IsAnyItemHovered() && !ImGui::IsAnyItemActive() &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        ECS::SceneECS::GetInstance().ClearSelection();
    }

    ImGui::End();

    if (cpuProfileEnabled) {
        const auto profileEnd = std::chrono::steady_clock::now();
        ++g_hierarchyProfileFrames;
        g_hierarchyProfileMs +=
            std::chrono::duration<double, std::milli>(profileEnd - profileStart).count();
        g_hierarchyProfileRoots += static_cast<uint64_t>(rootEntities.size());
        if ((g_hierarchyProfileFrames % 60u) == 0u) {
            const double invFrames = 1.0 / static_cast<double>(g_hierarchyProfileFrames);
            LOGI("[HierarchyWindow][CPU] frames=%llu avg_ms=%.3f avg_roots=%.1f",
                   static_cast<unsigned long long>(g_hierarchyProfileFrames),
                   g_hierarchyProfileMs * invFrames,
                   static_cast<double>(g_hierarchyProfileRoots) * invFrames);
        }
    }
}

void HierarchyWindow::RenderHierarchyChildren(ECS::Entity parent,
                                              ECS::Entity selectedEntity,
                                              bool searchActive) {
    auto children = ECS::SceneECS::GetInstance().GetChildren(parent);
    auto& coordinator = ECS::Coordinator::GetInstance();

    for (const auto& child : children) {
        if (child == ECS::INVALID_ENTITY) continue;
        if (!coordinator.HasComponent<ECS::NameComponent>(child)) continue;
        if (searchActive && !SubtreeMatchesSearch(child)) continue;

        std::string name = coordinator.GetComponent<ECS::NameComponent>(child).name;
        auto grandChildren = ECS::SceneECS::GetInstance().GetChildren(child);

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (child == selectedEntity) {
            flags |= ImGuiTreeNodeFlags_Selected;
        }
        if (grandChildren.empty()) {
            flags |= ImGuiTreeNodeFlags_Leaf;
        }
        if (searchActive && !grandChildren.empty()) {
            ImGui::SetNextItemOpen(true, ImGuiCond_Always);
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
            ImGui::Text(Tr("移动: %s"), name.c_str());
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
            if (ImGui::MenuItem(Tr("删除"))) {
                ECS::SceneECS::GetInstance().DestroyEntity(child);
            }
            if (ImGui::MenuItem(Tr("复制"))) {
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
            if (ImGui::MenuItem(Tr("取消父级"))) {
                ECS::SceneECS::GetInstance().RemoveParent(child);
            }

            ImGui::Separator();

            ImGui::EndPopup();
        }

        if (isOpen) {
            RenderHierarchyChildren(child, selectedEntity, searchActive);
            ImGui::TreePop();
        }

        ImGui::PopID();
    }
}

bool HierarchyWindow::MatchesSearch(unsigned int entity) const
{
    if (m_searchText.empty()) {
        return true;
    }

    auto& coordinator = ECS::Coordinator::GetInstance();
    if (!coordinator.HasComponent<ECS::NameComponent>(entity)) {
        return false;
    }

    const std::string name = FoldAscii(
        coordinator.GetComponent<ECS::NameComponent>(entity).name);
    return name.find(m_searchText) != std::string::npos;
}

bool HierarchyWindow::SubtreeMatchesSearch(unsigned int entity) const
{
    if (m_searchText.empty()) {
        return true;
    }

    const auto cached = m_searchSubtreeCache.find(entity);
    if (cached != m_searchSubtreeCache.end()) {
        return cached->second;
    }

    bool matches = MatchesSearch(entity);
    if (!matches) {
        auto& scene = ECS::SceneECS::GetInstance();
        for (const ECS::Entity child : scene.GetChildren(entity)) {
            if (SubtreeMatchesSearch(child)) {
                matches = true;
                break;
            }
        }
    }

    m_searchSubtreeCache[entity] = matches;
    return matches;
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
        ImGui::SetTooltip(vis ? Tr("隐藏对象") : Tr("显示对象"));
    }
    ImGui::PopStyleColor();
}

} // namespace Editor
