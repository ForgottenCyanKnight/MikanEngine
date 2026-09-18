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
#include "Editor/TerrainBrushTool.h"
#include "ECS/ECS.h"
#include "ECS/SceneECS.h"
#include "ECS/Components.h"
#include "ECS/ComponentRegistry.h"
#include "ECS/PhysicsSystem.h"
#include "ECS/ScriptSystem.h"
#include "SceneSerializer.h"
#include "Core/ProjectManager.h"
#include "Core/Utf8Path.h"
#include "Core/Log.h"
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
        // 落点必须是「项目 resourceRoot 下的 prefabs/」——与资源窗口显示的根一致，
        // 否则存到引擎根 assets/ 后资源窗口看不到，用户会以为保存失败。
        if (ImGui::Button("保存为预制体", ImVec2(-1, 0))) {
            std::string dir = ProjectManager::GetInstance().ResolveAssetPath("prefabs/");
            if (dir.empty()) {
                LOGE("[Prefab] 保存失败：当前没有已加载的项目（资产目录不可用）");
            } else {
                std::filesystem::create_directories(Utf8Path(dir));
                std::string path = dir + name + ".prefab.json";
                // 同名已存在时让用户确认，避免"保存成功但内容没变"的误解
                const bool exists = std::filesystem::exists(Utf8Path(path));
                m_pendingPrefabSavePath = path;
                if (exists) {
                    ImGui::OpenPopup("覆盖预制体##PrefabOverwrite");
                } else {
                    m_pendingPrefabSavePath.clear();
                    ECS::SceneSerializer serializer;
                    if (serializer.SavePrefab(selectedEntity, path)) {
                        LOGI("[Prefab] saved -> %s", path.c_str());
                    } else {
                        LOGE("[Prefab] FAILED to save %s", path.c_str());
                    }
                }
            }
        }
        if (ImGui::BeginPopup("覆盖预制体##PrefabOverwrite")) {
            ImGui::TextWrapped("已存在同名预制体，覆盖？");
            ImGui::TextDisabled("%s", m_pendingPrefabSavePath.c_str());
            ImGui::Separator();
            if (ImGui::Button("覆盖", ImVec2(90, 0))) {
                ECS::SceneSerializer serializer;
                if (serializer.SavePrefab(selectedEntity, m_pendingPrefabSavePath)) {
                    LOGI("[Prefab] overwrote -> %s", m_pendingPrefabSavePath.c_str());
                } else {
                    LOGE("[Prefab] FAILED to overwrite %s", m_pendingPrefabSavePath.c_str());
                }
                m_pendingPrefabSavePath.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("取消", ImVec2(90, 0))) {
                m_pendingPrefabSavePath.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
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
            // UI 实体（Canvas 子级 + Sprite/Button/Text 任一组件且 isUI）：
            // 位置是相对父容器的屏幕坐标，只编辑 x/y。
            const bool spriteUI =
                coordinator.HasComponent<ECS::Sprite2DComponent>(selectedEntity) &&
                coordinator.GetComponent<ECS::Sprite2DComponent>(selectedEntity).isUI;
            const bool buttonUI =
                coordinator.HasComponent<ECS::ButtonComponent>(selectedEntity) &&
                coordinator.GetComponent<ECS::ButtonComponent>(selectedEntity).isUI;
            const bool textUI =
                coordinator.HasComponent<ECS::TextComponent>(selectedEntity) &&
                coordinator.GetComponent<ECS::TextComponent>(selectedEntity).isUI;
            bool isUIEntity = (spriteUI || buttonUI || textUI) &&
                              ECS::SceneECS::GetInstance().GetParent(selectedEntity) != ECS::INVALID_ENTITY;
            if (ImGui::CollapsingHeader(isUIEntity ? "屏幕坐标(UI)" : "变换", ImGuiTreeNodeFlags_DefaultOpen)) {
                auto& transform = coordinator.GetComponent<ECS::TransformComponent>(selectedEntity);

                if (isUIEntity) {
                    glm::vec2 pos2(transform.position.x, transform.position.y);
                    if (ImGui::DragFloat2("位置", &pos2.x, 1.0f, -16384.0f, 16384.0f)) {
                        transform.position.x = pos2.x;
                        transform.position.y = pos2.y;
                        transform.MarkDirty();
                    }
                    // 锚点（0-1 相对父 Canvas）。文本也使用相同布局模型，
                    // pivot 用于右上/居中等不依赖固定字符串宽度的停靠。
                    if (coordinator.HasComponent<ECS::Sprite2DComponent>(selectedEntity)) {
                        auto& spr = coordinator.GetComponent<ECS::Sprite2DComponent>(selectedEntity);
                        ImGui::DragFloat2("锚点 Min", &spr.anchorMin.x, 0.01f, 0.0f, 1.0f);
                        ImGui::DragFloat2("锚点 Max", &spr.anchorMax.x, 0.01f, 0.0f, 1.0f);
                        spr.anchorMin.x = std::max(0.0f, std::min(1.0f, spr.anchorMin.x));
                        spr.anchorMin.y = std::max(0.0f, std::min(1.0f, spr.anchorMin.y));
                        spr.anchorMax.x = std::max(spr.anchorMin.x, std::min(1.0f, spr.anchorMax.x));
                        spr.anchorMax.y = std::max(spr.anchorMin.y, std::min(1.0f, spr.anchorMax.y));
                    } else if (coordinator.HasComponent<ECS::TextComponent>(selectedEntity)) {
                        auto& text = coordinator.GetComponent<ECS::TextComponent>(selectedEntity);
                        ImGui::DragFloat2("锚点 Min", &text.anchorMin.x, 0.01f, 0.0f, 1.0f);
                        ImGui::DragFloat2("锚点 Max", &text.anchorMax.x, 0.01f, 0.0f, 1.0f);
                        ImGui::DragFloat2("枢轴", &text.pivot.x, 0.01f, 0.0f, 1.0f);
                        text.anchorMin.x = std::max(0.0f, std::min(1.0f, text.anchorMin.x));
                        text.anchorMin.y = std::max(0.0f, std::min(1.0f, text.anchorMin.y));
                        text.anchorMax.x = std::max(text.anchorMin.x, std::min(1.0f, text.anchorMax.x));
                        text.anchorMax.y = std::max(text.anchorMin.y, std::min(1.0f, text.anchorMax.y));
                        text.pivot.x = std::max(0.0f, std::min(1.0f, text.pivot.x));
                        text.pivot.y = std::max(0.0f, std::min(1.0f, text.pivot.y));
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
                            LOGI("[PropertiesWindow] Loaded material: %s", mtlFile.c_str());
                        } else {
                            LOGI("[PropertiesWindow] No material file: %s", mtlFile.c_str());
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

        // ===== 高度图地形（编辑模式 + 笔刷参数 + 其余字段反射渲染）=====
        // 地形编辑状态刻意放在编辑器侧的 TerrainBrushTool 而非 TerrainComponent：
        // 编辑会话状态不需要序列化，加进组件会改变跨 DLL 结构体布局。
        if (coordinator.HasComponent<ECS::TerrainComponent>(selectedEntity)) {
            if (ImGui::CollapsingHeader("高度图地形", ImGuiTreeNodeFlags_DefaultOpen)) {
                auto& brush = TerrainBrushTool::GetInstance();

                bool editing = brush.IsEditing(selectedEntity);
                if (ImGui::Checkbox("地形编辑模式", &editing)) {
                    if (editing) {
                        brush.SetEditingEntity(selectedEntity);
                    } else {
                        brush.StopEditing();
                    }
                }
                ImGui::SameLine();
                ImGui::TextDisabled("(?)");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(
                        "开启后在场景视图中：\n"
                        "  按住鼠标左键涂抹\n"
                        "  滚轮调节笔刷半径，Shift+滚轮调节强度\n"
                        "  笔刷圆圈会贴合当前地表起伏\n"
                        "笔刷类型：\n"
                        "  升高/降低地形 —— 改高度图，圆的第二圈是衰减半程\n"
                        "  材质涂抹 —— 改图层权重，第二圈是过渡带的硬核边界\n"
                        "  草地散布 —— 改草密度图，涂过的区域长出实例化草叶\n"
                        "  水位涂抹 —— 改水位图并同步挖低地形（水深=凹陷量），涂过的区域显示为水面\n"
                        "编辑模式会临时关闭 Gizmo，退出后即可照常移动对象。");
                }

                if (brush.IsEditing(selectedEntity)) {
                    int mode = static_cast<int>(brush.GetMode());
                    const char* modeNames[] = { "升高地形", "降低地形", "材质涂抹", "草地散布", "水位涂抹" };
                    if (ImGui::Combo("笔刷类型", &mode, modeNames, 5)) {
                        brush.SetMode(static_cast<TerrainBrushMode>(mode));
                    }

                    if (brush.IsMaterialMode()) {
                        // 图层下拉直接显示该层用的贴图文件名，省得"图层2到底铺了什么"
                        // 还要回头翻文件路径。
                        const auto& terrainSettings =
                            coordinator.GetComponent<ECS::TerrainComponent>(selectedEntity);
                        const std::string* layerPaths[4] = {
                            &terrainSettings.layer0Path, &terrainSettings.layer1Path,
                            &terrainSettings.layer2Path, &terrainSettings.layer3Path};
                        std::string layerLabels[4];
                        const char* layerLabelPtrs[4] = {nullptr, nullptr, nullptr, nullptr};
                        for (int layer = 0; layer < 4; ++layer) {
                            const std::string& path = *layerPaths[layer];
                            const size_t slash = path.find_last_of("/\\");
                            const std::string fileName = path.empty()
                                ? std::string("未指定贴图（白模）")
                                : path.substr(slash == std::string::npos ? 0 : slash + 1);
                            layerLabels[layer] = "图层" + std::to_string(layer) + "  " + fileName;
                            layerLabelPtrs[layer] = layerLabels[layer].c_str();
                        }
                        int materialLayer = brush.GetMaterialLayer();
                        if (ImGui::Combo("材质图层", &materialLayer, layerLabelPtrs, 4)) {
                            brush.SetMaterialLayer(materialLayer);
                        }
                    }

                    float radius = brush.GetRadius();
                    if (ImGui::DragFloat("笔刷半径", &radius, 0.5f, 0.25f, 4096.0f, "%.2f")) {
                        brush.SetRadius(radius);
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("-##TerrainRadius")) brush.AddRadiusStep(-1);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("+##TerrainRadius")) brush.AddRadiusStep(1);

                    if (brush.IsMaterialMode() || brush.IsGrassMode() || brush.IsWaterMode()) {
                        // 材质/草地/水位模式下这一根就是"过渡边界有多软/多硬"，与高度
                        // 模式的雕刻强度分开存，互不干扰。
                        float hardness = brush.GetMaterialHardness();
                        if (ImGui::DragFloat("笔刷强度（边界软硬）", &hardness, 0.01f, 0.0f, 1.0f,
                                             "%.2f")) {
                            brush.SetMaterialHardness(hardness);
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton("-##TerrainHardness")) brush.AddMaterialHardnessStep(-1);
                        ImGui::SameLine();
                        if (ImGui::SmallButton("+##TerrainHardness")) brush.AddMaterialHardnessStep(1);
                        ImGui::TextDisabled("0 = 最软（圆心到边缘全程渐变）  1 = 硬边（只在外缘窄带渐变）");
                    } else {
                        float strength = brush.GetStrength();
                        if (ImGui::DragFloat("笔刷强度", &strength, 0.02f, 0.01f, 8.0f, "%.2f")) {
                            brush.SetStrength(strength);
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton("-##TerrainStrength")) brush.AddStrengthStep(-1);
                        ImGui::SameLine();
                        if (ImGui::SmallButton("+##TerrainStrength")) brush.AddStrengthStep(1);
                    }

                    if (brush.IsGrassMode()) {
                        // 草地模式的核心旋钮：目标草密度。按住左键把密度朝目标
                        // 混合，0 等效橡皮擦（除草），1 是最密的草丛。
                        float density = brush.GetGrassDensity();
                        if (ImGui::SliderFloat("草密度（目标）", &density, 0.0f, 1.0f, "%.2f")) {
                            brush.SetGrassDensity(density);
                        }
                        ImGui::TextDisabled("0 = 除草  1 = 最密草丛；密度按“按住时长”渐变到目标值");
                    }

                    if (brush.IsWaterMode()) {
                        // 水位模式的核心旋钮：目标水深（米）。0 = 抹掉水（橡皮擦）。
                        float depthMeters = brush.GetWaterDepthMeters();
                        if (ImGui::SliderFloat("水深（目标）", &depthMeters, 0.0f,
                                               Editor::kTerrainWaterMaxDepthMeters, "%.2f m")) {
                            brush.SetWaterDepthMeters(depthMeters);
                        }
                        ImGui::TextDisabled("0 = 抹掉水（不回填湖底）；涂水深时地形同步挖低同等米数");
                    }

                    uint32_t mapWidth = 0;
                    uint32_t mapHeight = 0;
                    bool procedural = false;
                    if (g_SceneRenderer.GetTerrainRenderer().GetHeightmapInfo(
                            selectedEntity, mapWidth, mapHeight, procedural)) {
                        if (procedural) {
                            ImGui::TextDisabled(
                                "高度图: 程序化平坦 %ux%u（可直接雕刻；填路径后切换为文件高度图）",
                                mapWidth, mapHeight);
                        } else {
                            ImGui::TextDisabled("高度图: %ux%u", mapWidth, mapHeight);
                        }
                    } else {
                        ImGui::TextDisabled("地形资源尚未就绪（渲染一帧后可用）");
                    }

                    // 材质笔刷写的是控制图，这里把它的来源/尺寸/是否可涂摊开，
                    // 免得"涂了没反应"时还要猜是没涂上还是这一层本来就是白模。
                    uint32_t controlWidth = 0;
                    uint32_t controlHeight = 0;
                    bool controlProcedural = false;
                    bool controlPaintable = false;
                    if (g_SceneRenderer.GetTerrainRenderer().GetControlMapInfo(
                            selectedEntity, controlWidth, controlHeight,
                            controlProcedural, controlPaintable)) {
                        if (!controlPaintable) {
                            ImGui::TextDisabled("控制图: 不可涂抹（纹理上传失败，已退回坡度自动混合）");
                        } else if (controlProcedural) {
                            ImGui::TextDisabled(
                                "控制图: 程序化坡度权重 %ux%u（材质笔刷就地改写这张图）",
                                controlWidth, controlHeight);
                        } else {
                            ImGui::TextDisabled(
                                "控制图: 来自文件 %ux%u（材质笔刷就地改写这张图）",
                                controlWidth, controlHeight);
                        }
                    }

                    // 草密度图状态：草地笔刷的写入目标。
                    uint32_t grassWidth = 0;
                    uint32_t grassHeight = 0;
                    bool grassPaintable = false;
                    if (g_SceneRenderer.GetTerrainRenderer().GetGrassMapInfo(
                            selectedEntity, grassWidth, grassHeight, grassPaintable)) {
                        if (grassPaintable) {
                            ImGui::TextDisabled(
                                "草密度图: %ux%u（草地笔刷可涂；密度在下一帧重建为草叶实例）",
                                grassWidth, grassHeight);
                        } else {
                            ImGui::TextDisabled("草密度图: 不可用（纹理创建失败，草地渲染关闭）");
                        }
                    }

                    // 水位图状态：水位笔刷的写入目标。
                    uint32_t waterWidth = 0;
                    uint32_t waterHeight = 0;
                    bool waterPaintable = false;
                    if (g_SceneRenderer.GetTerrainRenderer().GetWaterMapInfo(
                            selectedEntity, waterWidth, waterHeight, waterPaintable)) {
                        if (waterPaintable) {
                            ImGui::TextDisabled(
                                "水位图: %ux%u（水位笔刷可涂；涂过的区域显示为不透明水面）",
                                waterWidth, waterHeight);
                        } else {
                            ImGui::TextDisabled("水位图: 不可用（纹理创建失败，水面显示关闭）");
                        }
                    }
                }

                ImGui::Separator();
                if (auto* meta = ECS::ComponentRegistry::GetInstance().Find(typeid(ECS::TerrainComponent).name())) {
                    RenderComponentFields(selectedEntity, *meta);
                }
            }
        }

        // ===== 其他组件（反射表驱动自动渲染）=====
        // 新增组件只需在 ComponentRegistry 注册（含字段表+serializeKey），此处自动出现编辑面板。
        // 上方特殊块（变换/物理/材质/地形）覆盖的组件在此跳过，避免重复。
        {
            static const std::unordered_set<std::string> kHandledKeys = {
                "transform", "rigidBody", "material", "script", "terrain"
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
