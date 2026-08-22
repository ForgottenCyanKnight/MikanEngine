#include "Editor/SceneViewWindow.h"
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <ImGuizmo.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include "ECS/SceneECS.h"
#include "ECS/ECS.h"
#include "EngineGlobal.h"
#include "Camera.h"
#include "ModelLoader.h"
#include "EditorManager.h"
#include "SceneRenderer.h"
#include "Core/PhysicsGlobals.h"

extern MIKAN_API SceneRenderer g_SceneRenderer;

namespace Editor {

SceneViewWindow& SceneViewWindow::GetInstance() {
    static SceneViewWindow instance;
    return instance;
}

void SceneViewWindow::Render(bool& showWindow) {
    if (!showWindow) {
        m_isVisible = false;
        return;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("场景视图", &showWindow);

    ImVec2 contentSize = ImGui::GetContentRegionAvail();
    bool isCollapsed = ImGui::IsWindowCollapsed();
    float titleBarHeight = ImGui::GetFrameHeight();
    float windowHeight = ImGui::GetWindowHeight();
    bool hasVisibleContent = (windowHeight > titleBarHeight + 10.0f) && (contentSize.x > 1.0f && contentSize.y > 1.0f);

    ImGuiWindow* window = ImGui::FindWindowByName("场景视图");
    bool isActiveTab = window ? (window->Flags & ImGuiWindowFlags_DockNodeHost) == 0 : true;
    if (window && window->DockNode) {
        isActiveTab = (window->DockNode->VisibleWindow == window);
    }

    ImVec2 windowPos = ImGui::GetWindowPos();
    ImVec2 windowSize = ImGui::GetWindowSize();
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    bool isOnScreen = (windowPos.x < viewport->WorkPos.x + viewport->WorkSize.x &&
                       windowPos.x + windowSize.x > viewport->WorkPos.x &&
                       windowPos.y < viewport->WorkPos.y + viewport->WorkSize.y &&
                       windowPos.y + windowSize.y > viewport->WorkPos.y);

    m_isVisible = !isCollapsed && hasVisibleContent && isOnScreen && isActiveTab;

    m_width = contentSize.x;
    m_height = contentSize.y;

    if (m_width != m_lastWidth || m_height != m_lastHeight) {
        m_sizeChanged = true;
        m_lastWidth = m_width;
        m_lastHeight = m_height;
    }

    if (m_descriptorSet != VK_NULL_HANDLE) {
        ImVec2 imageSize(contentSize.x, contentSize.y);
        ImVec2 uv0(0.0f, 0.0f);
        ImVec2 uv1(1.0f, 1.0f);

        ImGui::Image((ImTextureID)m_descriptorSet, imageSize, uv0, uv1);

        RenderDragDropTarget(windowPos);
    } else {
        ImVec2 contentAvail = ImGui::GetContentRegionAvail();
        ImVec2 textPos = ImVec2(
            ImGui::GetWindowPos().x + contentAvail.x * 0.5f - 50,
            ImGui::GetWindowPos().y + contentAvail.y * 0.5f
        );
        ImGui::SetCursorScreenPos(textPos);
        ImGui::Text("场景视图");

        ImGui::SetCursorScreenPos(ImVec2(
            ImGui::GetWindowPos().x + contentAvail.x * 0.5f - 80,
            textPos.y + 20
        ));
        ImGui::TextDisabled("(游戏画面将显示在这里)");
    }

    ImGui::End();
    ImGui::PopStyleVar();
}

void SceneViewWindow::RenderWithGizmo(bool& showWindow, const glm::mat4& view, const glm::mat4& proj, ECS::Entity selectedEntity) {
    if (!showWindow) {
        m_isVisible = false;
        return;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("场景视图", &showWindow);

    ImVec2 windowPos = ImGui::GetWindowPos();
    ImVec2 contentRegionMin = ImGui::GetWindowContentRegionMin();
    ImVec2 contentSize = ImGui::GetContentRegionAvail();

    bool isCollapsed = ImGui::IsWindowCollapsed();
    float titleBarHeight = ImGui::GetFrameHeight();
    float windowHeight = ImGui::GetWindowHeight();
    bool hasVisibleContent = (windowHeight > titleBarHeight + 10.0f) && (contentSize.x > 1.0f && contentSize.y > 1.0f);

    ImGuiWindow* window = ImGui::FindWindowByName("场景视图");
    bool isActiveTab = window ? (window->Flags & ImGuiWindowFlags_DockNodeHost) == 0 : true;
    if (window && window->DockNode) {
        isActiveTab = (window->DockNode->VisibleWindow == window);
    }

    ImVec2 windowPos2 = ImGui::GetWindowPos();
    ImVec2 windowSize = ImGui::GetWindowSize();
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    bool isOnScreen = (windowPos2.x < viewport->WorkPos.x + viewport->WorkSize.x &&
                       windowPos2.x + windowSize.x > viewport->WorkPos.x &&
                       windowPos2.y < viewport->WorkPos.y + viewport->WorkSize.y &&
                       windowPos2.y + windowSize.y > viewport->WorkPos.y);

    m_isVisible = !isCollapsed && hasVisibleContent && isOnScreen && isActiveTab;

    ImVec2 gizmoPos = ImVec2(windowPos.x + contentRegionMin.x, windowPos.y + contentRegionMin.y);

    m_width = contentSize.x;
    m_height = contentSize.y;

    if (m_width != m_lastWidth || m_height != m_lastHeight) {
        m_sizeChanged = true;
        m_lastWidth = m_width;
        m_lastHeight = m_height;
    }

    if (m_descriptorSet != VK_NULL_HANDLE) {

        ImVec2 imageSize(contentSize.x, contentSize.y);
        ImVec2 uv0(0.0f, 0.0f);
        ImVec2 uv1(1.0f, 1.0f);

        ImGui::Image((ImTextureID)m_descriptorSet, imageSize, uv0, uv1);

        ImVec2 viewManipulatePos = ImVec2(windowPos.x + contentRegionMin.x + contentSize.x - 120, windowPos.y + contentRegionMin.y + 20);
        ImVec2 viewManipulateSize = ImVec2(100, 100);

        if (selectedEntity != ECS::INVALID_ENTITY) {
            float viewMatrix[16];
            memcpy(viewMatrix, &view[0][0], sizeof(float) * 16);

            float originalViewMatrix[16];
            memcpy(originalViewMatrix, &view[0][0], sizeof(float) * 16);

            ImGuizmo::ViewManipulate(viewMatrix, 1.0f, viewManipulatePos, viewManipulateSize, IM_COL32(0, 0, 0, 100));

            bool viewMatrixChanged = false;
            for (int i = 0; i < 16; i++) {
                if (viewMatrix[i] != originalViewMatrix[i]) {
                    viewMatrixChanged = true;
                    break;
                }
            }

            if (viewMatrixChanged) {
                glm::mat4 newViewMatrix;
                memcpy(&newViewMatrix[0][0], viewMatrix, sizeof(float) * 16);

                glm::vec3 newCameraPos, cameraFront, cameraUp;
                newCameraPos = -glm::vec3(newViewMatrix[0][3], newViewMatrix[1][3], newViewMatrix[2][3]);
                cameraFront = -glm::vec3(newViewMatrix[0][2], newViewMatrix[1][2], newViewMatrix[2][2]);
                cameraUp = glm::vec3(newViewMatrix[0][1], newViewMatrix[1][1], newViewMatrix[2][1]);

                float yaw = atan2(cameraFront.z, cameraFront.x) * 180.0f / glm::pi<float>();
                float pitch = asin(cameraFront.y) * 180.0f / glm::pi<float>();

                auto& coordinator = ECS::Coordinator::GetInstance();
                if (coordinator.HasComponent<ECS::TransformComponent>(selectedEntity)) {
                    ECS::TransformComponent& transform = coordinator.GetComponent<ECS::TransformComponent>(selectedEntity);
                    glm::vec3 targetPos = transform.position;

                    float distance = glm::length(g_Camera.Position - targetPos);

                    glm::vec3 newCameraDirection = glm::normalize(cameraFront);
                    newCameraPos = targetPos - newCameraDirection * distance;
                }

                g_Camera.Position = newCameraPos;
                g_Camera.Front = cameraFront;
                g_Camera.Up = cameraUp;
                g_Camera.Yaw = yaw;
                g_Camera.Pitch = pitch;
                g_Camera.UpdateCameraVectors();
            }
        }

        RenderDragDropTarget(windowPos);
    } else {
        ImVec2 contentAvail = ImGui::GetContentRegionAvail();
        ImVec2 textPos = ImVec2(
            ImGui::GetWindowPos().x + contentAvail.x * 0.5f - 50,
            ImGui::GetWindowPos().y + contentAvail.y * 0.5f
        );
        ImGui::SetCursorScreenPos(textPos);
        ImGui::Text("场景视图");

        ImGui::SetCursorScreenPos(ImVec2(
            ImGui::GetWindowPos().x + contentAvail.x * 0.5f - 80,
            textPos.y + 20
        ));
        ImGui::TextDisabled("(游戏画面将显示在这里)");
    }

    if (selectedEntity != ECS::INVALID_ENTITY && ToolbarWindow::GetInstance().IsShowGizmoAxis()) {
        auto& coordinator = ECS::Coordinator::GetInstance();
        if (coordinator.HasComponent<ECS::TransformComponent>(selectedEntity)) {
            // 2D UI 实体（Canvas 子级：Sprite/Text/Button 任一组件且 isUI）跳过 3D gizmo：由 GameView 用屏幕坐标正交 gizmo 处理
            bool isUIEntity = false;
            if (coordinator.HasComponent<ECS::Sprite2DComponent>(selectedEntity)) {
                isUIEntity = coordinator.GetComponent<ECS::Sprite2DComponent>(selectedEntity).isUI;
            } else if (coordinator.HasComponent<ECS::TextComponent>(selectedEntity)) {
                isUIEntity = coordinator.GetComponent<ECS::TextComponent>(selectedEntity).isUI;
            } else if (coordinator.HasComponent<ECS::ButtonComponent>(selectedEntity)) {
                isUIEntity = coordinator.GetComponent<ECS::ButtonComponent>(selectedEntity).isUI;
            }
            isUIEntity = isUIEntity &&
                         ECS::SceneECS::GetInstance().GetParent(selectedEntity) != ECS::INVALID_ENTITY;
            if (isUIEntity) {
                ImGuizmo::Enable(false);
            } else {
            ImGuizmo::Enable(true);
            ImGuizmo::SetOrthographic(false);
            ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());

            ImGuizmo::SetRect(gizmoPos.x, gizmoPos.y, contentSize.x, contentSize.y);

            glm::mat4 modelMatrix = ECS::SceneECS::GetInstance().GetWorldMatrix(selectedEntity);

            ImGuizmo::OPERATION operation = ImGuizmo::TRANSLATE;
            GizmoMode gizmoMode = ToolbarWindow::GetInstance().GetGizmoMode();
            switch (gizmoMode) {
                case GizmoMode::Translate: operation = ImGuizmo::TRANSLATE; break;
                case GizmoMode::Rotate: operation = ImGuizmo::ROTATE; break;
                case GizmoMode::Scale: operation = ImGuizmo::SCALE; break;
            }

            ImGuizmo::MODE mode = ImGuizmo::LOCAL;

            float viewMatrix[16];
            float projectionMatrix[16];
            float modelMatrixArray[16];
            memcpy(viewMatrix, &view[0][0], sizeof(float) * 16);
            memcpy(projectionMatrix, &proj[0][0], sizeof(float) * 16);
            memcpy(modelMatrixArray, &modelMatrix[0][0], sizeof(float) * 16);

            ImGuizmo::Manipulate(viewMatrix, projectionMatrix, operation, mode, modelMatrixArray);

            // 增量写回状态(跨帧保持): 直接写 Decompose 的世界值会覆盖本地, 子实体(世界≠本地)会瞬移/飙升
            static glm::mat4 s_prevWorld = glm::mat4(1.0f);
            static bool s_prevUsing = false;
            static glm::mat4 s_dragStartMatrix = glm::mat4(1.0f);   // 2026-08-11 重做移植: drag-start input matrix (absolute base)
            static glm::quat s_dragStartRot = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);   // drag-start local rotation

            if (ImGuizmo::IsUsing()) {
                glm::mat4 newModelMatrix;
                memcpy(&newModelMatrix[0][0], modelMatrixArray, sizeof(float) * 16);

                if (!s_prevUsing) {
                    s_prevWorld = newModelMatrix;  // 拖动开始帧: 仅记录起点
                    s_dragStartMatrix = modelMatrix;   // 2026-08-11 重做移植: 记录拖动起始输入矩阵
                    auto& tr0 = coordinator.GetComponent<ECS::TransformComponent>(selectedEntity);
                    s_dragStartRot = tr0.rotation;     // 记录拖动起始局部旋转
                } else {
                    // 平移增量
                    glm::vec3 deltaPos = glm::vec3(newModelMatrix[3]) - glm::vec3(s_prevWorld[3]);
                    auto& tr = coordinator.GetComponent<ECS::TransformComponent>(selectedEntity);
                    tr.position += deltaPos;

                    // 2026-08-11 重做移植修复: 旋转改用"相对拖动起始的绝对增量"——不再每帧 delta 累积
                    // 旧实现 deltaRot = 输出×上一帧⁻¹ 再乘到实体: ImGuizmo 静止时输出与输入的
                    // 微小浮点差异每帧累积 → 不拖动时对象也持续旋转（用户反馈"一直转动"）。
                    // 新实现每帧从拖动起始重算总增量，静止时总增量=单位四元数 → 完全不动。
                    auto worldRot = [](const glm::mat4& m) -> glm::quat {
                        glm::mat3 r3(m);
                        glm::vec3 s(glm::length(r3[0]), glm::length(r3[1]), glm::length(r3[2]));
                        if (s.x > 1e-6f) r3[0] /= s.x;
                        if (s.y > 1e-6f) r3[1] /= s.y;
                        if (s.z > 1e-6f) r3[2] /= s.z;
                        return glm::normalize(glm::quat(r3));
                    };
                    glm::quat totalDelta = worldRot(newModelMatrix) * glm::inverse(worldRot(s_dragStartMatrix));
                    tr.rotation = totalDelta * s_dragStartRot;

                    // 缩放增量：仅 SCALE 操作时应用。
                    // 原因：length(矩阵列) 提取的 scale 在纯旋转下数学不变，但 ImGuizmo 矩阵是浮点数值，
                    // 非均匀缩放+旋转后列不再精确正交 → 列长度漂移；若无条件应用会把旋转误差写进 scale 并跨帧累积。
                    if (operation == ImGuizmo::SCALE) {
                        auto worldScale = [](const glm::mat4& m) -> glm::vec3 {
                            return glm::vec3(glm::length(glm::vec3(m[0])),
                                             glm::length(glm::vec3(m[1])),
                                             glm::length(glm::vec3(m[2])));
                        };
                        glm::vec3 newS = worldScale(newModelMatrix);
                        glm::vec3 prevS = worldScale(s_prevWorld);
                        if (prevS.x > 1e-6f) tr.scale.x *= newS.x / prevS.x;
                        if (prevS.y > 1e-6f) tr.scale.y *= newS.y / prevS.y;
                        if (prevS.z > 1e-6f) tr.scale.z *= newS.z / prevS.z;
                    }
                    tr.MarkDirty(); // ImGuizmo 拖拽改了 position/rotation/scale,世界矩阵缓存需失效
                    if (g_PhysicsSystemPtr) {
                        g_PhysicsSystemPtr->SyncModelTransforms();
                    }
                }
                s_prevWorld = newModelMatrix;
                s_prevUsing = true;
            } else {
                s_prevUsing = false;
            }
        }
        }
    }

    ImGui::End();
    ImGui::PopStyleVar();
}

void SceneViewWindow::RenderDragDropTarget(const ImVec2& windowPos) {
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_ITEM")) {
            std::string assetPath = (const char*)payload->Data;
            std::filesystem::path pathObj(assetPath);
            std::string fileName = pathObj.filename().string();
            std::string fileExt = pathObj.extension().string();
            if (!fileExt.empty() && fileExt[0] == '.') {
                fileExt = fileExt.substr(1);
            }

            auto entity = ECS::SceneECS::GetInstance().CreateEmpty(fileName);

            glm::vec3 position = g_Camera.Position + g_Camera.Front * 5.0f;
            ECS::SceneECS::GetInstance().SetPosition(entity, position);

            auto& coordinator = ECS::Coordinator::GetInstance();

            if (fileExt == "material") {
                ECS::MaterialComponent material;
                EditorManager::GetInstance().LoadMaterialFromFile(assetPath, material);
                coordinator.AddComponent<ECS::MaterialComponent>(entity, material);

                ECS::MeshComponent mesh;
                mesh.type = ECS::MeshType::Cube;
                coordinator.AddComponent<ECS::MeshComponent>(entity, mesh);

                ECS::RenderComponent render;
                render.visible = true;
                coordinator.AddComponent<ECS::RenderComponent>(entity, render);

                printf("Added material to scene: %s\n", fileName.c_str());
            } else if (fileExt == "png" || fileExt == "jpg" || fileExt == "jpeg" ||
                       fileExt == "tga" || fileExt == "bmp" || fileExt == "dds") {
                ECS::MaterialComponent material;
                material.albedoPath = assetPath;
                material.useAlbedoTexture = true;
                coordinator.AddComponent<ECS::MaterialComponent>(entity, material);

                ECS::MeshComponent mesh;
                mesh.type = ECS::MeshType::Cube;
                coordinator.AddComponent<ECS::MeshComponent>(entity, mesh);

                ECS::RenderComponent render;
                render.visible = true;
                coordinator.AddComponent<ECS::RenderComponent>(entity, render);

                printf("Added image to scene: %s (with material)\n", fileName.c_str());
            } else if (fileExt == "gltf" || fileExt == "glb" || fileExt == "obj" ||
                       fileExt == "fbx" || fileExt == "dae") {
                ECS::MaterialComponent material;

                ModelLoadResult result = ModelLoader::LoadModelWithTextures(assetPath);
                printf("[SceneView] Loaded model: %s, material count: %zu\n", assetPath.c_str(), result.materialTextures.size());

                if (!result.materialTextures.empty()) {
                    const MaterialTextureInfo* materialInfo = nullptr;
                    for (const auto& mat : result.materialTextures) {
                        if (!mat.diffuseTexturePath.empty()) {
                            materialInfo = &mat;
                            break;
                        }
                    }

                    if (materialInfo == nullptr) {
                        materialInfo = &result.materialTextures[0];
                    }

                    if (!materialInfo->diffuseTexturePath.empty()) {
                        material.albedoPath = materialInfo->diffuseTexturePath;
                        material.useAlbedoTexture = true;
                    } else {
                        material.useAlbedoTexture = false;
                    }

                    if (!materialInfo->normalTexturePath.empty()) {
                        material.normalPath = materialInfo->normalTexturePath;
                        material.useNormalTexture = true;
                    } else {
                        material.useNormalTexture = false;
                    }

                    if (!materialInfo->roughnessTexturePath.empty()) {
                        material.roughnessPath = materialInfo->roughnessTexturePath;
                        material.useRoughnessTexture = true;
                    } else {
                        material.useRoughnessTexture = false;
                    }

                    if (!materialInfo->metallicTexturePath.empty()) {
                        material.metallicPath = materialInfo->metallicTexturePath;
                        material.useMetallicTexture = true;
                    } else {
                        material.useMetallicTexture = false;
                    }
                } else {
                    material.useAlbedoTexture = false;
                }

                coordinator.AddComponent<ECS::MaterialComponent>(entity, material);

                ECS::MeshComponent mesh;
                mesh.type = ECS::MeshType::Model;
                mesh.modelPath = assetPath;
                coordinator.AddComponent<ECS::MeshComponent>(entity, mesh);

                ECS::RenderComponent render;
                render.visible = true;
                coordinator.AddComponent<ECS::RenderComponent>(entity, render);

                printf("Added model to scene: %s\n", fileName.c_str());
            } else if (fileExt == "vox") {
                ECS::VoxModelComponent voxComp;
                voxComp.voxPath = assetPath;
                voxComp.loaded = false;
                voxComp.isStatic = true;
                coordinator.AddComponent<ECS::VoxModelComponent>(entity, voxComp);

                ECS::MaterialComponent material;
                material.albedoColor = glm::vec3(1.0f);
                material.metallic = 0.0f;
                material.roughness = 0.5f;
                material.ao = 1.0f;
                coordinator.AddComponent<ECS::MaterialComponent>(entity, material);

                ECS::RenderComponent render;
                render.visible = true;
                coordinator.AddComponent<ECS::RenderComponent>(entity, render);

                printf("Added file to scene: %s\n", fileName.c_str());
            } else {
                printf("Added file to scene: %s\n", fileName.c_str());
            }

            ECS::SceneECS::GetInstance().SetSelectedEntity(entity);
        }
        ImGui::EndDragDropTarget();
    }
}

} // namespace Editor
