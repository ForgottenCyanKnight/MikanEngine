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

    // Lighting is read from renderWorld->lights.  Only view-dependent light
    // values remain in this context.
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

    // 视图身份槽位（尾插）：决定地形 MDI / 叶片级草剔除 / CSM 等按视图分段的
    // GPU 资源用哪一套。0 = 编辑器场景视图，1 = 游戏视图（编辑器 GameView 面板或
    // 游戏模式主相机），2 = 反射探针视图。由 RenderECS 在 PrepareFrame 之前写入
    // （见 SceneRenderer::RenderECS 的 viewSlotOverride）。
    int viewSlot = 0;

    // 反射探针面序号（0..5，尾插）。仅 viewSlot = 2 时有意义：探针 6 个面在同一
    // 帧、同一个命令缓冲里逐面顺序录制，而相机 UBO 是 host memcpy 写的（无命令流
    // 排序），因此每个面必须有自己的 UBO + 描述符集，否则 6 面全部用最后一个面的
    // 矩阵出图。非探针视图恒为 0，不受影响。
    int probeFace = 0;
};
