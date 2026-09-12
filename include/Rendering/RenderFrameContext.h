#pragma once
// RenderFrameContext.h - per-frame rendering state shared across passes
// Aggregates the state that RenderECS used to keep in one giant function so
// the geometry / voxel / Hi-Z / present passes can share it without passing
// 20+ parameters around.
#include "Platform/Export.h"
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <array>
#include <vector>
#include <unordered_map>
#include <string>
#include "ECS/Types.h"
#include "SceneTypes.h"
#include "RenderWorld.h"
#include "AABB.h"   // Plane

struct RenderFrameContext {
    // Immutable scene data extracted once at the render boundary.  The
    // command-buffer state below remains per-view, while this pointer is
    // shared by all passes for the frame.
    const RenderWorld* renderWorld = nullptr;

    // Frame inputs
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    int width = 0, height = 0;
    glm::mat4 view, proj, cullView, cullProj;
    bool isSceneView = false;
    // Scene uniform buffer + descriptor set (model rendering)
    struct VulkanBuffer* uniformBuffer = nullptr;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;

    // Lighting (collected from ECS)
    std::vector<ECS::Entity> lightEntities;
    glm::vec3 lightDir = glm::vec3(0.0f, 0.0f, -1.0f);
    float lightIntensity = 1.0f;
    glm::vec3 lightColor = glm::vec3(1.0f);

    // Culling state
    glm::mat4 viewProj = glm::mat4(1.0f);
    std::array<Plane, 6> frustumPlanes{};
    bool useFrustumCulling = false;
    bool useSceneCameraCulling = false;  // 模型级场景相机二次剔除（仅场景视图，编辑器相机视锥）
    bool useSubMeshCulling = false;
    glm::vec3 cameraPos = glm::vec3(0.0f);
    glm::vec3 cullingCameraPos = glm::vec3(0.0f);

    // Scene collection (from SceneCollector)
    std::vector<ECS::Entity> rootEntities;
    std::vector<ECS::Entity> allModelEntities;
    std::vector<ECS::Entity> allVoxEntities;
    std::vector<ECS::Entity> allEntitiesForQuadTree;
    std::unordered_map<std::string, ModelInstanceGroup> modelGroups;
    std::unordered_map<std::string, VoxInstanceGroup> voxGroups;

    // Frame-to-frame state
    bool cameraMoved = false;
    bool modelCountChanged = false;

    // Prepared culling matrices / frustums (computed in prepare, used by geometry)
    glm::mat4 projView = glm::mat4(1.0f);
    glm::mat4 prevProjView = glm::mat4(1.0f);
    glm::mat4 effectiveCullView = glm::mat4(1.0f);
    glm::mat4 effectiveCullProj = glm::mat4(1.0f);
    bool useMainCameraCulling = false;
    std::array<Plane, 6> mainCameraFrustumPlanes{};
};
