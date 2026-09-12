#include "Rendering/SceneGeometryPass.h"

#include "AABB.h"
#include "Rendering/SceneDebugPass.h"
#include "Rendering/SceneEnvironmentPass.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <set>
#include <utility>
#include <vector>
#include <glm/gtc/matrix_transform.hpp>

void SceneGeometryPass::Render(SceneRenderer& sceneRenderer, RenderFrameContext& ctx)
{
    VkCommandBuffer commandBuffer = ctx.commandBuffer;
    int width = ctx.width, height = ctx.height;
    const glm::mat4& view = ctx.view;
    const glm::mat4& proj = ctx.proj;
    const glm::mat4& cullView = ctx.cullView;
    const glm::mat4& cullProj = ctx.cullProj;
    bool isSceneView = ctx.isSceneView;
    auto& modelGroups = ctx.modelGroups;
    auto& voxGroups = ctx.voxGroups;
    auto& allModelEntities = ctx.allModelEntities;
    auto& allVoxEntities = ctx.allVoxEntities;
    glm::vec3& cameraPos = ctx.cameraPos;
    glm::vec3& cullingCameraPos = ctx.cullingCameraPos;
    glm::mat4& viewProj = ctx.viewProj;
    std::array<Plane, 6>& frustumPlanes = ctx.frustumPlanes;
    bool& useFrustumCulling = ctx.useFrustumCulling;
    bool& useSceneCameraCulling = ctx.useSceneCameraCulling;
    bool& useSubMeshCulling = ctx.useSubMeshCulling;
    bool& cameraMoved = ctx.cameraMoved;
    bool& modelCountChanged = ctx.modelCountChanged;
    struct VulkanBuffer* uniformBuffer = ctx.uniformBuffer;
    VkDescriptorSet descriptorSet = ctx.descriptorSet;
    glm::mat4& projView = ctx.projView;
    glm::mat4& prevProjView = ctx.prevProjView;
    glm::mat4& effectiveCullView = ctx.effectiveCullView;
    glm::mat4& effectiveCullProj = ctx.effectiveCullProj;
    bool& useMainCameraCulling = ctx.useMainCameraCulling;
    std::array<Plane, 6>& mainCameraFrustumPlanes = ctx.mainCameraFrustumPlanes;
    auto& rootEntities = ctx.rootEntities;
    const RenderWorld& world = ctx.renderWorld != nullptr ? *ctx.renderWorld : sceneRenderer.m_RenderWorld;
    // Terrain/Water 属于环境底层，保持在模型和体素之前绘制。
    SceneEnvironmentPass::RenderTerrainWater(sceneRenderer, ctx);
    // [diag] 模型绘制入口（运行中交换链重建后蒙皮模型消失排查；前几次打印对比启动 vs 重建）
    {
        static int s_drawDiag = 0;
        if (s_drawDiag < 4) {
            s_drawDiag++;
            printf("[SceneRenderer][diag] model draw: groups=%zu renderers=%zu",
                   modelGroups.size(), sceneRenderer.m_ModelRenderers.size());
            for (const auto& [p, g] : modelGroups) printf(" '%s'[%zu]", p.c_str(), g.entities.size());
            printf("\n");
        }
    }
    // 渲染模型
    for (auto& [modelPath, group] : modelGroups) {
        if (group.entities.empty()) continue;
        
        auto rendererIt = sceneRenderer.m_ModelRenderers.find(modelPath);
        if (rendererIt == sceneRenderer.m_ModelRenderers.end() || !rendererIt->second) {
            static bool s_warnedMissing = false;
            if (!s_warnedMissing) {
                s_warnedMissing = true;
                printf("[SceneRenderer][diag] model '%s' MISSING from m_ModelRenderers (%zu entries):",
                       modelPath.c_str(), sceneRenderer.m_ModelRenderers.size());
                for (const auto& [p, r] : sceneRenderer.m_ModelRenderers) printf(" '%s'", p.c_str());
                printf("\n");
            }
            continue; // 跳过未加载的模型
        }
        
        auto& renderer = rendererIt->second;
        
        // 检查模型是否有效
        if (!renderer->HasModelLoaded()) {
            static bool s_warnedNotLoaded = false;
            if (!s_warnedNotLoaded) {
                s_warnedNotLoaded = true;
                printf("[SceneRenderer][diag] model '%s' exists but HasModelLoaded()=false\n",
                       modelPath.c_str());
            }
            continue;
        }
        
        // 注: BLAS 级别视锥剔除暂未启用（实现保留在 git 历史中；viewProj 已在主流程统一计算）
        
        bool modelHasAlbedoTexture = renderer->HasAlbedoTexture();
        bool modelHasNormalTexture = renderer->HasNormalTexture();
        bool modelHasEmissiveTexture = renderer->HasEmissiveTexture();
        bool modelHasRoughnessTexture = renderer->HasRoughnessTexture();
        bool modelHasMetallicTexture = renderer->HasMetallicTexture();
        // if (modelHasRoughnessTexture || modelHasMetallicTexture) {
        //     printf("[SceneRenderer][diag] useMR=1 model='%s' rough=%d metal=%d\n", modelPath.c_str(), modelHasRoughnessTexture ? 1 : 0, modelHasMetallicTexture ? 1 : 0);
        // }
        
        // 根据剔除粒度选择渲染方式
        // The submesh BVH uses bind/raw bounds and is not animation-aware.
        // Keep model-level culling for skinned models, but never drop an
        // animated submesh based on stale local bounds.
        const bool effectiveSubMeshCulling = useSubMeshCulling && !renderer->HasSkinning();
        if (effectiveSubMeshCulling) {
            // 逐submesh剔除：先用模型级AABB快速筛选，再对子网格精确剔除
            struct VisibleEntityData {
                ECS::Entity entity;
                glm::mat4 modelMatrix;
                std::vector<size_t> visibleSubMeshIndices;
                ModelInstanceData instanceData;
            };
            std::vector<VisibleEntityData> visibleEntities;
            
            for (size_t entityIdx = 0; entityIdx < group.entities.size(); ++entityIdx) {
                const auto& entity = group.entities[entityIdx];
                
                const RenderWorldEntity* entityData = world.Find(entity);
                if (entityData == nullptr || !entityData->hasTransform) continue;
                glm::mat4 modelMatrix = entityData->transform.worldMatrix;
                
                // 第一步：主相机视锥剔除（快速筛选）
                if (useMainCameraCulling) {
                    AABB localAABB = renderer->GetAABB();
                    AABB worldAABB = localAABB.Transform(modelMatrix);
                    if (!AABBUtils::IsAABBInFrustum(worldAABB, mainCameraFrustumPlanes)) {
                        continue; // 模型不在主相机视锥体内，跳过所有子网格
                    }
                }
                
                // 第二步：场景相机视锥剔除（仅场景视图，编辑器相机视锥）
                if (useSceneCameraCulling) {
                    AABB localAABB = renderer->GetAABB();
                    AABB worldAABB = localAABB.Transform(modelMatrix);
                    if (!AABBUtils::IsAABBInFrustum(worldAABB, frustumPlanes)) {
                        continue; // 模型不在场景相机视锥体内，跳过所有子网格
                    }
                }
                
                // 第二步：对模型内的子网格进行精确剔除
                std::vector<size_t> visibleSubMeshIndices = sceneRenderer.m_OcclusionCulling.GetVisibleSubMeshIndices(
                    renderer.get(), modelMatrix, frustumPlanes, allModelEntities, cameraPos,
                    useFrustumCulling, entity);

                if (visibleSubMeshIndices.empty()) {
                    continue; // 没有可见的submesh
                }
                
                VisibleEntityData ved;
                ved.entity = entity;
                ved.modelMatrix = modelMatrix;
                ved.visibleSubMeshIndices = visibleSubMeshIndices;
                
                // 准备实例数据
                ved.instanceData.model = modelMatrix;
                // 获取上一帧的模型矩阵，如果没有则使用当前帧矩阵
                auto prevModelIt = sceneRenderer.m_PrevModelMatrices.find(entity);
                ved.instanceData.prevModel = (prevModelIt != sceneRenderer.m_PrevModelMatrices.end()) ? prevModelIt->second : modelMatrix;
                
                if (entityData->hasMaterial) {
                    const auto& material = entityData->material;
                    ved.instanceData.albedoColor = glm::vec4(material.albedoColor, 1.0f);
                    ved.instanceData.materialData = glm::vec4(
                        material.metallic,
                        material.roughness,
                        material.ao,
                        material.emissiveIntensity
                    );
                    ved.instanceData.textureFlags = glm::vec4(
                        (material.useNormalTexture || modelHasNormalTexture) ? 1.0f : 0.0f,
                        (material.useAlbedoTexture && modelHasAlbedoTexture) ? 1.0f : 0.0f,
                        (material.useEmissiveTexture || modelHasEmissiveTexture) ? 1.0f : 0.0f,
(modelHasRoughnessTexture || modelHasMetallicTexture) ? 1.0f : 0.0f
                    );
                } else {
                    ved.instanceData.albedoColor = glm::vec4(1.0f);
                    ved.instanceData.materialData = glm::vec4(-1.0f, -1.0f, -1.0f, 0.0f);
                    ved.instanceData.textureFlags = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f); // 没有MaterialComponent时不使用纹理
                }
                
                visibleEntities.push_back(std::move(ved));
            }
            
            // 收集所有出现过的可见 submesh 索引
            std::set<size_t> allVisibleSubMeshIndices;
            for (const auto& ved : visibleEntities) {
                for (size_t idx : ved.visibleSubMeshIndices) {
                    allVisibleSubMeshIndices.insert(idx);
                }
            }
            
            // 按可见 submesh 组织批次：每个可见 submesh 一批（含包含它的实体的实例）
            std::vector<SubMeshInstanceData> batches;
            batches.reserve(allVisibleSubMeshIndices.size());
            for (size_t subMeshIdx : allVisibleSubMeshIndices) {
                SubMeshInstanceData batch;
                batch.subMeshIndex = subMeshIdx;
                for (const auto& ved : visibleEntities) {
                    for (size_t idx : ved.visibleSubMeshIndices) {
                        if (idx == subMeshIdx) {
                            batch.instances.push_back(ved.instanceData);
                            break;
                        }
                    }
                }
                if (!batch.instances.empty()) {
                    batches.push_back(std::move(batch));
                }
            }
            
            if (!batches.empty()) {
                // 管线选择（保持原有语义：任一可见实体为 wireframe/双面则整体用对应管线）
                bool hasDoubleSided = false;
                bool hasWireframe = false;
                for (const auto& ved : visibleEntities) {
                    const RenderWorldEntity* entityData = world.Find(ved.entity);
                    if (entityData != nullptr && entityData->hasRenderFlags) {
                        if (entityData->render.doubleSided) hasDoubleSided = true;
                        if (entityData->render.wireframe) hasWireframe = true;
                    }
                }
                
                // 一次调用渲染全部可见 submesh：内部按材质(描述符集)分组，共享管线/常量/实例缓冲
                renderer->RenderInstancedBatches(commandBuffer, width, height, projView, prevProjView,
                                                 cameraPos, batches, nullptr, hasDoubleSided, hasWireframe);
            }
            
            // 保存当前帧的模型矩阵作为下一帧的上一帧矩阵
            for (const auto& ved : visibleEntities) {
                sceneRenderer.m_PrevModelMatrices[ved.entity] = ved.modelMatrix;
            }

        } else {
            std::vector<ModelInstanceData> instanceData;
            instanceData.reserve(group.entities.size());

            for (const auto& entity : group.entities) {
                // 计算模型的世界空间AABB
                AABB localAABB = renderer->GetAABB();
                const RenderWorldEntity* entityData = world.Find(entity);
                if (entityData == nullptr || !entityData->hasTransform) continue;
                glm::mat4 modelMatrix = entityData->transform.worldMatrix;
                AABB worldAABB = localAABB.Transform(modelMatrix);
                
                // 第一步：主相机视锥剔除
                if (useMainCameraCulling) {
                    if (!AABBUtils::IsAABBInFrustum(worldAABB, mainCameraFrustumPlanes)) {
                        continue; // 模型不在主相机视锥体内，跳过
                    }
                }
                
                if (useSceneCameraCulling) {
                    if (!AABBUtils::IsAABBInFrustum(worldAABB, frustumPlanes)) {
                        continue;
                    }
                }
                
                ModelInstanceData data;
                data.model = modelMatrix;
                // 获取上一帧的模型矩阵，如果没有则使用当前帧矩阵
                auto prevModelIt = sceneRenderer.m_PrevModelMatrices.find(entity);
                data.prevModel = (prevModelIt != sceneRenderer.m_PrevModelMatrices.end()) ? prevModelIt->second : modelMatrix;
                
                if (entityData->hasMaterial) {
                    const auto& material = entityData->material;
                    data.albedoColor = glm::vec4(material.albedoColor, 1.0f);
                    data.materialData = glm::vec4(
                        material.metallic,
                        material.roughness,
                        material.ao,
                        material.emissiveIntensity
                    );
                    data.textureFlags = glm::vec4(
                        (material.useNormalTexture || modelHasNormalTexture) ? 1.0f : 0.0f,
                        (material.useAlbedoTexture && modelHasAlbedoTexture) ? 1.0f : 0.0f,
                        (material.useEmissiveTexture || modelHasEmissiveTexture) ? 1.0f : 0.0f,
(modelHasRoughnessTexture || modelHasMetallicTexture) ? 1.0f : 0.0f
                    );
                } else {
                    data.albedoColor = glm::vec4(1.0f);
                    data.materialData = glm::vec4(-1.0f, -1.0f, -1.0f, 0.0f);
                    data.textureFlags = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f); // 没有MaterialComponent时不使用纹理
                }
                
                instanceData.push_back(data);
            }
            
            // 检查是否需要双面渲染和线框渲染
            bool hasDoubleSided = false;
            bool hasWireframe = false;
            for (const auto& entity : group.entities) {
                const RenderWorldEntity* entityData = world.Find(entity);
                if (entityData != nullptr && entityData->hasRenderFlags) {
                    if (entityData->render.doubleSided) {
                        hasDoubleSided = true;
                    }
                    if (entityData->render.wireframe) {
                        hasWireframe = true;
                    }
                }
            }
            
            // 根据渲染模式选择不同的管线
            if (hasWireframe) {
                // 线框渲染
                renderer->RenderInstancedWireframe(commandBuffer, width, height, projView, prevProjView, cameraPos, instanceData, nullptr, {});
            } else if (hasDoubleSided || renderer->HasDoubleSided()) {
                renderer->RenderInstancedDoubleSided(commandBuffer, width, height, projView, prevProjView, cameraPos, instanceData, nullptr, {});
            } else {
                // 单面渲染
                renderer->RenderInstanced(commandBuffer, width, height, projView, prevProjView, cameraPos, instanceData, nullptr, {});
            }
            
            // 保存当前帧的模型矩阵作为下一帧的上一帧矩阵
            for (size_t i = 0; i < group.entities.size(); ++i) {
                const auto& entity = group.entities[i];
                if (i < instanceData.size()) {
                    sceneRenderer.m_PrevModelMatrices[entity] = instanceData[i].model;
                } else {
                    // 对于不可见的实体，也保存其当前矩阵
                    const RenderWorldEntity* entityData = world.Find(entity);
                    if (entityData != nullptr && entityData->hasTransform) {
                        sceneRenderer.m_PrevModelMatrices[entity] = entityData->transform.worldMatrix;
                    }
                }
            }

        }
    }
    
    // 保存当前帧的 ProjView 矩阵作为下一帧的上一帧矩阵
    sceneRenderer.m_PrevProjViewMatrix = projView;
    sceneRenderer.m_HasPrevFrameMatrices = true;
    
    // 调试几何收集保持在模型绘制之后，供链末 overlay pass 使用。
    SceneDebugPass::Collect(sceneRenderer, ctx);
    
    // 渲染体素模型（在模型渲染之后）
    // 注意：不再每帧调用 Clear()，只在需要真正清空时才调用（如场景切换）
    // 现在使用 UpdateVoxelModel 每帧更新模型数据（通过脏标记优化）
    
    // 统计当前帧可见的静态体素数量
    size_t totalVisibleStaticVoxels = 0;
    for (const auto& [voxPath, voxGroup] : voxGroups) {
        for (const auto& entity : voxGroup.entities) {
            const RenderWorldEntity* entityData = world.Find(entity);
            if (entityData == nullptr) continue;
            const bool isWireframe = entityData->hasRenderFlags && entityData->render.wireframe;
            const bool isStatic = !entityData->hasVoxel || entityData->voxel.isStatic;
            if (!isWireframe && isStatic) {
                totalVisibleStaticVoxels++;
            }
        }
    }
    
    // 如果 MDI 中的模型数量与当前帧不匹配，需要清空重建
    if (sceneRenderer.m_VoxelMeshMultiDrawIndirect && sceneRenderer.m_VoxelMeshMultiDrawIndirect->GetTotalInstances() != totalVisibleStaticVoxels) {
        sceneRenderer.m_VoxelMeshMultiDrawIndirect->Clear();
    }
    
    for (auto& [voxPath, voxGroup] : voxGroups) {
        auto voxRendererIt = sceneRenderer.m_VoxRenderers.find(voxPath);
        if (voxRendererIt == sceneRenderer.m_VoxRenderers.end()) {
            // 创建新的 VoxRenderer
            auto renderer = std::make_unique<VoxRenderer>();
            renderer->Init(sceneRenderer.m_RenderPass);
            
            printf("SceneRenderer: Attempting to load Vox file: %s", voxPath.c_str());
            if (renderer->LoadVoxFile(voxPath)) {
                sceneRenderer.m_VoxRenderers[voxPath] = std::move(renderer);
                printf("SceneRenderer: Successfully loaded Vox file: %s", voxPath.c_str());
            } else {
                printf("SceneRenderer: Failed to load Vox file: %s", voxPath.c_str());
                continue; // 加载失败，跳过
            }
        }
        
        voxRendererIt = sceneRenderer.m_VoxRenderers.find(voxPath);
        if (voxRendererIt != sceneRenderer.m_VoxRenderers.end() && voxRendererIt->second && voxRendererIt->second->HasLoaded()) {
            auto& voxRenderer = voxRendererIt->second;
            
            std::vector<VoxelInstanceData> staticInstances;
            std::vector<VoxelInstanceData> dynamicInstances;
            std::vector<VoxelInstanceData> wireframeInstances;
            
            // 获取体素的局部 AABB
            glm::vec3 minBounds = voxRenderer->GetMinBounds();
            glm::vec3 maxBounds = voxRenderer->GetMaxBounds();
            AABB localAABB;
            localAABB.min = minBounds;
            localAABB.max = maxBounds;
            
            // 收集所有通过主相机（游戏相机）视锥剔除的体素
            for (const auto& entity : voxGroup.entities) {
                const RenderWorldEntity* entityData = world.Find(entity);
                if (entityData == nullptr || !entityData->hasTransform) continue;
                glm::mat4 modelMatrix = entityData->transform.worldMatrix;
                
                // 主相机视锥剔除（只使用游戏相机）
                if (useMainCameraCulling) {
                    AABB worldAABB = localAABB.Transform(modelMatrix);
                    if (!AABBUtils::IsAABBInFrustum(worldAABB, mainCameraFrustumPlanes)) {
                        continue; // 体素不在主相机视锥体内，跳过
                    }
                }
                
                VoxelInstanceData instanceData;
                
                // 获取原始模型矩阵并翻转 Z 轴（左右手坐标系转换）
                glm::mat4 flipZ = glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, 1.0f, -1.0f));
                instanceData.model = modelMatrix * flipZ;
                
                auto prevModelIt = sceneRenderer.m_PrevModelMatrices.find(entity);
                instanceData.prevModel = (prevModelIt != sceneRenderer.m_PrevModelMatrices.end()) ? prevModelIt->second : instanceData.model;
                
                // 设置体素数据
                instanceData.worldMinBounds = voxRenderer->GetMinBounds();
                instanceData.voxelSize = voxRenderer->GetVoxelSize();
                
                if (entityData->hasMaterial) {
                    const auto& material = entityData->material;
                    instanceData.albedoColor = glm::vec4(material.albedoColor, 1.0f);
                    instanceData.materialData = glm::vec4(material.metallic, material.roughness, material.ao, material.emissiveIntensity);
                } else {
                    instanceData.albedoColor = glm::vec4(1.0f);
                    instanceData.materialData = glm::vec4(0.0f, 0.75f, 1.0f, 0.0f);
                }
                const bool isWireframe = entityData->hasRenderFlags && entityData->render.wireframe;
                const bool isStatic = !entityData->hasVoxel || entityData->voxel.isStatic;
                
                if (isWireframe) {
                    wireframeInstances.push_back(instanceData);
                } else if (isStatic) {
                    staticInstances.push_back(instanceData);
                } else {
                    dynamicInstances.push_back(instanceData);
                }
                
                sceneRenderer.m_PrevModelMatrices[entity] = instanceData.model;
            }
            
            // 使用 MDI 渲染静态体素
            if (!staticInstances.empty() && sceneRenderer.m_VoxelMeshMultiDrawIndirect) {
                // 将每个静态体素模型添加到 MDI 渲染器（使用 UpdateVoxelModel 每帧更新）
                // 注意：staticInstances 中的模型都是可见的（通过视锥剔除）
                // 需要为每个 staticInstance 找到对应的 entity
                size_t staticInstIdx = 0;
                for (const auto& entity : voxGroup.entities) {
                    if (staticInstIdx >= staticInstances.size()) break;
                    const RenderWorldEntity* entityData = world.Find(entity);
                    if (entityData == nullptr || !entityData->hasTransform) continue;
                    // 检查该实体是否是静态的且在视锥体内
                    glm::mat4 modelMatrix = entityData->transform.worldMatrix;
                    
                    // 主相机视锥剔除（只使用游戏相机）
                    if (useMainCameraCulling) {
                        AABB worldAABB = localAABB.Transform(modelMatrix);
                        if (!AABBUtils::IsAABBInFrustum(worldAABB, mainCameraFrustumPlanes)) {
                            continue; // 体素不在主相机视锥体内，跳过
                        }
                    }
                    
                    const bool isWireframe = entityData->hasRenderFlags && entityData->render.wireframe;
                    const bool isStatic = !entityData->hasVoxel || entityData->voxel.isStatic;
                    
                    if (!isWireframe && isStatic) {
                        const auto& instanceData = staticInstances[staticInstIdx];
                        sceneRenderer.m_VoxelMeshMultiDrawIndirect->UpdateVoxelModel(
                            reinterpret_cast<void*>(static_cast<uintptr_t>(entity)),
                            voxPath, voxRenderer.get(), instanceData.model,
                            instanceData.albedoColor, true);
                        staticInstIdx++;
                    }
                }
            } else if (!staticInstances.empty()) {
                // 使用带背面剔除的渲染方式（只绘制可见面）
                voxRenderer->RenderMeshWithBackfaceCulling(commandBuffer, width, height, projView, prevProjView, cameraPos, staticInstances, true);
            }
            
            // 动态体素使用实例化面渲染
            if (!dynamicInstances.empty()) {
                voxRenderer->RenderInstanced(commandBuffer, width, height, projView, prevProjView, cameraPos, dynamicInstances);
            }
            
            // 线框渲染
            if (!wireframeInstances.empty()) {
                voxRenderer->RenderMeshWireframe(commandBuffer, width, height, projView, prevProjView, cameraPos, wireframeInstances);
            }
        }
    }
    
    // 执行 MDI 渲染
    if (sceneRenderer.m_VoxelMeshMultiDrawIndirect) {
        // 计算游戏相机的 ProjView 矩阵用于双重剔除
        glm::mat4 cullProjView = cullProj * cullView;
        
        // 根据是否启用双重剔除来决定传递的参数
        bool useDualCulling = true;  // 启用双重剔除
        
        // 使用主相机位置进行背面剔除
        sceneRenderer.m_VoxelMeshMultiDrawIndirect->Render(commandBuffer, width, height, projView, prevProjView, 
                                            cullProjView, cullingCameraPos, useDualCulling, true);
    }
    
    // 无限体素世界保持在模型/MDI 之后绘制，避免改变原有 pass 顺序。
    SceneEnvironmentPass::RenderVoxelWorld(sceneRenderer, ctx);
}
