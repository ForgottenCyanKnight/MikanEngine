// PropertiesWindow.cpp - 属性面板（反射表驱动版）
// 原文件（1048 行，含 4-8 月开发的大量手写组件编辑块）在一次编码转换事故中损坏
// （UTF-8 文件被按 ANSI 读写导致乱码 + 注释吞代码），历史副本（fix/）也是早期损坏版，
// 无法恢复。本重写版以「反射表驱动」为核心：
//   - 组件字段编辑 = 遍历 ComponentRegistry，凡注册了字段表+serializeKey 的组件自动渲染
//     （RenderComponentFields，见 ComponentInspector），新增组件无需改本文件
//   - 保留少量特殊块：变换（含 UI 锚点）、物理刚体创建、材质加载
// 丢失的原定制编辑（补间/碰撞体/精灵锚点 9 宫格等精细面板）由通用反射字段渲染替代（功能降级但可用）。
#include "Editor/PropertiesWindow.h"
#include "Editor/ComponentInspector.h"
#include "Editor/AssetPathPicker.h"
#include "ECS/ECS.h"
#include "ECS/SceneECS.h"
#include "ECS/Components.h"
#include "ECS/ComponentRegistry.h"
#include "ECS/PhysicsSystem.h"
#include "ECS/ScriptSystem.h"
#include "SceneSerializer.h"
#include "Core/ProjectManager.h"
#include "PhysicsManager.h"
#include "Rendering/SceneRenderer.h"
#include <imgui/imgui.h>
#include <cmath>
#include <filesystem>
#include <unordered_set>

extern MIKAN_API SceneRenderer g_SceneRenderer;
extern MIKAN_API std::shared_ptr<ECS::PhysicsSystem> g_PhysicsSystemPtr;

namespace Editor {

PropertiesWindow& PropertiesWindow::GetInstance() {
    static PropertiesWindow instance;
    return instance;
}

void PropertiesWindow::Render() {
    if (!m_visible) return;

    ImGui::Begin("属性", &m_visible);

    ECS::Entity selectedEntity = ECS::SceneECS::GetInstance().GetSelectedEntity();

    if (selectedEntity != ECS::INVALID_ENTITY) {
        auto& coordinator = ECS::Coordinator::GetInstance();

        // 对象名称
        std::string name = ECS::SceneECS::GetInstance().GetName(selectedEntity);
        char nameBuffer[256];
        strncpy(nameBuffer, name.c_str(), sizeof(nameBuffer) - 1);
        nameBuffer[sizeof(nameBuffer) - 1] = '\0';
        if (ImGui::InputText("名称", nameBuffer, sizeof(nameBuffer))) {
            ECS::SceneECS::GetInstance().SetName(selectedEntity, nameBuffer);
        }

        // 实体 ID
        ImGui::Text("实体ID: %u", selectedEntity);

        // 预制体：保存当前实体子树为可复用模板（Unity 式）
        if (ImGui::Button("保存为预制体", ImVec2(-1, 0))) {
            std::string dir = ProjectManager::GetInstance().ResolveAssetPath("prefabs/");
            std::filesystem::create_directories(dir);
            std::string path = dir + name + ".prefab.json";
            ECS::SceneSerializer serializer;
            if (serializer.SavePrefab(selectedEntity, path)) {
                printf("[Prefab] saved -> %s\n", path.c_str());
            } else {
                printf("[Prefab] FAILED to save %s\n", path.c_str());
            }
        }

        // 组件列表（注册表驱动：反查组件名；可移除的提供移除按钮；name/hierarchy 基础组件不列出——名称置顶编辑、层级由层级窗口管理）
        ImGui::Separator();
        ImGui::Text("组件:");
        {
            auto componentNames = coordinator.GetEntityComponentNames(selectedEntity);
            for (const auto& typeName : componentNames) {
                auto* meta = ECS::ComponentRegistry::GetInstance().Find(typeName);
                if (!meta) continue;
                if (meta->serializeKey &&
                    (std::strcmp(meta->serializeKey, "name") == 0 ||
                     std::strcmp(meta->serializeKey, "hierarchy") == 0)) continue;
                ImGui::Text("  - %s", meta->displayName);
                if (meta->userRemovable) {
                    ImGui::SameLine();
                    char removeLabel[64];
                    snprintf(removeLabel, sizeof(removeLabel), "移除##%s", meta->displayName);
                    if (ImGui::SmallButton(removeLabel)) {
                        meta->removeFrom(selectedEntity);
                    }
                }
            }
        }

        // 添加组件菜单（遍历注册表 userAddable 且未挂载的组件）
        if (ImGui::Button("添加组件", ImVec2(-1, 0))) {
            ImGui::OpenPopup("AddComponentPopup");
        }
        if (ImGui::BeginPopup("AddComponentPopup")) {
            bool any = false;

            // 体积云是渲染器直接消费的控制组件。放在弹窗顶部提供固定入口，
            // 即使编辑器侧注册表来自旧 DLL，也能把它挂到当前空物体上；
            // 后面的注册表遍历跳过同名项，避免出现两个“体积云”。
            const bool hasCloudVolume = coordinator.HasComponent<ECS::CloudVolumeComponent>(selectedEntity);
            if (!hasCloudVolume) {
                any = true;
                if (ImGui::MenuItem("体积云")) {
                    coordinator.AddComponent<ECS::CloudVolumeComponent>(
                        selectedEntity, ECS::CloudVolumeComponent{});
                }
                ImGui::Separator();
            }

            for (const auto& meta : ECS::ComponentRegistry::GetInstance().GetAll()) {
                if (!meta.userAddable) continue;
                if (meta.typeName &&
                    std::strcmp(meta.typeName, typeid(ECS::CloudVolumeComponent).name()) == 0) {
                    continue;
                }
                if (coordinator.HasComponentByName(selectedEntity, meta.typeName)) continue;
                any = true;
                if (ImGui::MenuItem(meta.displayName)) {
                    meta.addTo(selectedEntity);
                }
            }
            if (!any) {
                ImGui::TextDisabled("(无可添加组件)");
            }
            ImGui::EndPopup();
        }

        ImGui::Separator();

        // ===== 变换组件（含 UI 实体的屏幕坐标编辑）=====
        if (coordinator.HasComponent<ECS::TransformComponent>(selectedEntity)) {
            // UI 实体（Canvas 子级 + Sprite2D 且 isUI）：位置为相对画布的屏幕坐标，只编辑 x/y
            bool isUIEntity = coordinator.HasComponent<ECS::Sprite2DComponent>(selectedEntity) &&
                              coordinator.GetComponent<ECS::Sprite2DComponent>(selectedEntity).isUI &&
                              ECS::SceneECS::GetInstance().GetParent(selectedEntity) != ECS::INVALID_ENTITY;
            if (ImGui::CollapsingHeader(isUIEntity ? "屏幕坐标(UI)" : "变换", ImGuiTreeNodeFlags_DefaultOpen)) {
                auto& transform = coordinator.GetComponent<ECS::TransformComponent>(selectedEntity);

                if (isUIEntity) {
                    glm::vec2 pos2(transform.position.x, transform.position.y);
                    if (ImGui::DragFloat2("位置", &pos2.x, 1.0f, -16384.0f, 16384.0f)) {
                        transform.position.x = pos2.x;
                        transform.position.y = pos2.y;
                    }
                    // 锚点（0-1 相对父 Canvas）
                    if (coordinator.HasComponent<ECS::Sprite2DComponent>(selectedEntity)) {
                        auto& spr = coordinator.GetComponent<ECS::Sprite2DComponent>(selectedEntity);
                        ImGui::DragFloat2("锚点 Min", &spr.anchorMin.x, 0.01f, 0.0f, 1.0f);
                        ImGui::DragFloat2("锚点 Max", &spr.anchorMax.x, 0.01f, 0.0f, 1.0f);
                        spr.anchorMin.x = std::max(0.0f, std::min(1.0f, spr.anchorMin.x));
                        spr.anchorMin.y = std::max(0.0f, std::min(1.0f, spr.anchorMin.y));
                        spr.anchorMax.x = std::max(spr.anchorMin.x, std::min(1.0f, spr.anchorMax.x));
                        spr.anchorMax.y = std::max(spr.anchorMin.y, std::min(1.0f, spr.anchorMax.y));
                    }
                } else {
                    glm::vec3 position = transform.position;
                    if (ImGui::DragFloat3("位置", &position.x, 0.1f)) {
                        transform.position = position;
                        transform.MarkDirty();
                    }
                    glm::vec3 rotation = transform.GetEulerAngles();
                    if (ImGui::DragFloat3("旋转", &rotation.x, 1.0f)) {
                        transform.SetEulerAngles(rotation);
                    }
                    glm::vec3 scale = transform.scale;
                    if (ImGui::DragFloat3("缩放", &scale.x, 0.1f)) {
                        transform.scale = scale;
                        transform.MarkDirty();
                        // 编辑器未运行游戏时 PhysicsSystem 不会走主循环；
                        // 这里主动刷新一次，让刚体包围形状立即跟随模型缩放。
                        if (g_PhysicsSystemPtr) {
                            g_PhysicsSystemPtr->SyncModelTransforms();
                        }
                    }
                }
            }
        }

        // ===== 物理（仅当实体已有刚体组件时显示；添加刚体走"添加组件"菜单）=====
        if (coordinator.HasComponent<ECS::RigidBodyComponent>(selectedEntity)) {
            if (ImGui::CollapsingHeader("物理", ImGuiTreeNodeFlags_DefaultOpen)) {
                auto& rigidBody = coordinator.GetComponent<ECS::RigidBodyComponent>(selectedEntity);
                if (ImGui::Button("移除刚体")) {
                    coordinator.RemoveComponent<ECS::RigidBodyComponent>(selectedEntity);
                }
                // 刚体字段（反射渲染，15 项：类型/质量/重力/触发器/弹性/碰撞形状/尺寸/偏移/OBB/同步/碰撞体生成）
                if (auto* meta = ECS::ComponentRegistry::GetInstance().Find(typeid(ECS::RigidBodyComponent).name())) {
                    RenderComponentFields(selectedEntity, *meta);
                }
                if (coordinator.HasComponent<ECS::TransformComponent>(selectedEntity)) {
                    const auto& transform = coordinator.GetComponent<ECS::TransformComponent>(selectedEntity);
                    const glm::vec3 scale(
                        std::abs(transform.scale.x),
                        std::abs(transform.scale.y),
                        std::abs(transform.scale.z));
                    const glm::vec3 worldSize = rigidBody.size * scale;
                    ImGui::TextDisabled("实际世界碰撞尺寸: %.3f  %.3f  %.3f",
                                       worldSize.x, worldSize.y, worldSize.z);
                }
            }
        }

        // ===== 材质（加载/保存 + 反射字段）=====
        if (coordinator.HasComponent<ECS::MaterialComponent>(selectedEntity)) {
            if (ImGui::CollapsingHeader("材质", ImGuiTreeNodeFlags_DefaultOpen)) {
                auto& mat = coordinator.GetComponent<ECS::MaterialComponent>(selectedEntity);
                if (ImGui::Button("加载##Material")) {
                    // 从场景模型路径推断材质文件（assets/models/xxx.material）
                    std::string modelPath;
                    if (coordinator.HasComponent<ECS::MeshComponent>(selectedEntity)) {
                        auto& mesh = coordinator.GetComponent<ECS::MeshComponent>(selectedEntity);
                        if (mesh.type == ECS::MeshType::Model && !mesh.modelPath.empty()) {
                            modelPath = mesh.modelPath;
                        }
                    }
                    if (!modelPath.empty()) {
                        std::filesystem::path p(modelPath);
                        std::string mtlFile = p.replace_extension(".material").string();
                        // 简化：读取同名 .material 文件（键=值行）
                        FILE* f = nullptr;
                        if (fopen_s(&f, mtlFile.c_str(), "r") == 0 && f) {
                            char line[256];
                            while (fgets(line, sizeof(line), f)) {
                                std::string s(line);
                                size_t eq = s.find('=');
                                if (eq != std::string::npos) {
                                    std::string k = s.substr(0, eq);
                                    std::string v = s.substr(eq + 1);
                                    if (k == "albedoPath") mat.albedoPath = v;
                                    else if (k == "normalPath") mat.normalPath = v;
                                    else if (k == "roughnessPath") mat.roughnessPath = v;
                                    else if (k == "metallicPath") mat.metallicPath = v;
                                    else if (k == "aoPath") mat.aoPath = v;
                                }
                            }
                            fclose(f);
                            printf("[PropertiesWindow] Loaded material: %s\n", mtlFile.c_str());
                        } else {
                            printf("[PropertiesWindow] No material file: %s\n", mtlFile.c_str());
                        }
                    }
                }
                ImGui::Separator();
                ImGui::TextDisabled("纹理槽：拖拽/浏览路径 + 采样器");
                ModelRenderer* selRenderer = nullptr;
                {
                    std::string modelPath;
                    if (coordinator.HasComponent<ECS::MeshComponent>(selectedEntity)) {
                        const auto& mesh = coordinator.GetComponent<ECS::MeshComponent>(selectedEntity);
                        modelPath = mesh.modelPath;
                    }
                    selRenderer = modelPath.empty() ? nullptr : g_SceneRenderer.GetModelRenderer(modelPath);
                }
                static int s_selectedSubMesh = -1;
                if (selRenderer && selRenderer->GetSubMeshCount() > 0) {
                    const size_t subMeshCount = selRenderer->GetSubMeshCount();
                    // 下拉：全部 + 各 subMesh（Bistro 级 1591 项用 begin/end 仍可用；首项=全部）
                    if (ImGui::BeginCombo("应用范围", s_selectedSubMesh < 0 ? "全部 subMesh" : selRenderer->GetSubMeshName((size_t)s_selectedSubMesh).c_str())) {
                        if (ImGui::Selectable("全部 subMesh", s_selectedSubMesh < 0)) s_selectedSubMesh = -1;
                        const size_t previewCap = 300;   // 大模型（>300 subMesh）只列前 300，末尾提示
                        const size_t listCount = std::min(subMeshCount, previewCap);
                        for (size_t i = 0; i < listCount; i++) {
                            char label[256];
                            snprintf(label, sizeof(label), "%zu: %s", i, selRenderer->GetSubMeshName(i).c_str());
                            if (ImGui::Selectable(label, s_selectedSubMesh == (int)i)) s_selectedSubMesh = (int)i;
                        }
                        if (subMeshCount > previewCap) {
                            ImGui::TextDisabled("... 共 %zu 个 subMesh（列表截断）", subMeshCount);
                        }
                        ImGui::EndCombo();
                    }
                } else {
                    s_selectedSubMesh = -1;
                }
                // 应用回调：路径（相对项目根）+ 采样器 → ModelRenderer 选中 subMesh（或全部）
                auto applySlot = [&](int texType, const std::string& path, int samplerType) {
                    if (selRenderer) {
                        selRenderer->ApplyTextureToSubMesh(s_selectedSubMesh, texType, path, samplerType);
                    }
                };
                auto TexturePathInput = [&](const char* label,
                                            std::string& path, bool& useTexture,
                                            int& samplerType, int texType) {
                    if (RenderAssetPathInput(label, path, AssetPathKind::Texture)) {
                        useTexture = !path.empty();
                        applySlot(texType, path, samplerType);
                    }
                    ImGui::SameLine();
                    if (ImGui::Checkbox(("使用##" + std::string(label)).c_str(), &useTexture)) {
                        applySlot(texType, path, samplerType);
                    }
                    if (useTexture && !path.empty()) {
                        const char* samplerNames[] = { "Linear", "Nearest", "LinearClamp", "NearestClamp" };
                        if (ImGui::Combo(("采样##" + std::string(label)).c_str(), &samplerType, samplerNames, 4)) {
                            applySlot(texType, path, samplerType);
                        }
                    }
                };
                TexturePathInput("反照率路径", mat.albedoPath, mat.useAlbedoTexture, mat.albedoSamplerType, 0);
                TexturePathInput("法线路径", mat.normalPath, mat.useNormalTexture, mat.normalSamplerType, 1);
                TexturePathInput("粗糙度路径", mat.roughnessPath, mat.useRoughnessTexture, mat.roughnessSamplerType, 2);
                TexturePathInput("金属度路径", mat.metallicPath, mat.useMetallicTexture, mat.metallicSamplerType, 3);
                TexturePathInput("AO路径", mat.aoPath, mat.useAOTexture, mat.aoSamplerType, 4);
                TexturePathInput("自发光路径", mat.emissivePath, mat.useEmissiveTexture, mat.emissiveSamplerType, 5);

                ImGui::SeparatorText("材质参数");
                ImGui::ColorEdit3("反照率颜色", &mat.albedoColor.x);
                ImGui::DragFloat("金属度", &mat.metallic, 0.01f, 0.0f, 1.0f);
                ImGui::DragFloat("粗糙度", &mat.roughness, 0.01f, 0.0f, 1.0f);
                ImGui::DragFloat("AO", &mat.ao, 0.01f, 0.0f, 1.0f);
                ImGui::DragFloat("自发光强度", &mat.emissiveIntensity, 0.01f, 0.0f, 10.0f, "%.2f");
                ImGui::Separator();
                if (auto* meta = ECS::ComponentRegistry::GetInstance().Find(typeid(ECS::MaterialComponent).name())) {
                    RenderComponentFields(selectedEntity, *meta);
                }
            }
        }

        // ===== 脚本（Unity 式：脚本类下拉 + 参数反射面板）=====
        if (coordinator.HasComponent<ECS::ScriptComponent>(selectedEntity)) {
            if (ImGui::CollapsingHeader("脚本", ImGuiTreeNodeFlags_DefaultOpen)) {
                auto& sc = coordinator.GetComponent<ECS::ScriptComponent>(selectedEntity);

                // 脚本类下拉：已注册脚本列表（含"无"）
                const auto& registered = ECS::ScriptSystem::GetInstance().GetRegisteredNames();
                std::vector<const char*> options;
                options.push_back("(无脚本)");
                for (const auto& n : registered) options.push_back(n.c_str());
                int current = 0;
                for (size_t i = 0; i < registered.size(); ++i) {
                    if (registered[i] == sc.scriptName) { current = (int)i + 1; break; }
                }
                if (ImGui::Combo("脚本类", &current, options.data(), (int)options.size())) {
                    const std::string newName = (current > 0) ? options[current] : "";
                    ECS::ScriptSystem::GetInstance().RebindScript(selectedEntity, newName);
                }

                // 参数：实例存在时按脚本参数字段表反射渲染（写回实时生效）
                if (sc.runtime) {
                    ImGui::Separator();
                    ImGui::TextUnformatted("参数:");
                    Editor::RenderScriptParamFields(sc.runtime);
                } else if (!sc.scriptName.empty()) {
                    ImGui::TextDisabled("脚本未实例化（未注册，或场景加载后生效）");
                }
            }
        }

        // ===== 其他组件（反射表驱动自动渲染）=====
        // 新增组件只需在 ComponentRegistry 注册（含字段表+serializeKey），此处自动出现编辑面板。
        // 上方特殊块（变换/物理/材质）覆盖的组件在此跳过，避免重复。
        {
            static const std::unordered_set<std::string> kHandledKeys = {
                "transform", "rigidBody", "material", "script"
            };
            auto componentNames = coordinator.GetEntityComponentNames(selectedEntity);
            for (const auto& typeName : componentNames) {
                auto* meta = ECS::ComponentRegistry::GetInstance().Find(typeName);
                if (!meta) continue;
                // 无序列化键（运行时组件）但有字段表也可编辑；两者皆无则跳过
                if (!meta->serializeKey && !meta->fields) continue;
                // 基础组件（name 置顶编辑 / transform 有变换块 / hierarchy 由层级窗口管理）跳过通用渲染
                if (meta->serializeKey &&
                    (std::strcmp(meta->serializeKey, "name") == 0 ||
                     std::strcmp(meta->serializeKey, "transform") == 0 ||
                     std::strcmp(meta->serializeKey, "hierarchy") == 0)) continue;
                if (meta->serializeKey && kHandledKeys.count(meta->serializeKey)) continue;
                if (ImGui::CollapsingHeader(meta->displayName, ImGuiTreeNodeFlags_DefaultOpen)) {
                    RenderComponentFields(selectedEntity, *meta);
                }
            }
        }
    } else {
        ImGui::TextDisabled("未选择对象");
    }

    ImGui::End();
}

} // namespace Editor
