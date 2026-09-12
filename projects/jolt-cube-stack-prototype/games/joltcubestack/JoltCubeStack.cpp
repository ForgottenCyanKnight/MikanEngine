#include "Game/IGameModule.h"
#include "Game/GameManager.h"
#include "ECS/Components.h"
#include "ECS/Coordinator.h"
#include "ECS/PhysicsSystem.h"
#include "ECS/SceneECS.h"
#include "Rendering/Renderer2D.h"
#include "Rendering/TextRenderer.h"

#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr int kColumns = 12;
constexpr int kDepth = 12;
constexpr int kLayers = 6;
constexpr std::size_t kCubeCount = static_cast<std::size_t>(
    kColumns * kDepth * kLayers);

// cube.glb 的局部顶点范围是 [-1, 1]。Transform.scale=0.39 后，
// 渲染模型和 Jolt BoxShape 的实际世界尺寸都为 0.78。
constexpr float kCubeSize = 0.78f;
// 初始中心距大于盒体尺寸，给离散物理步进留出明确的安全间隙。
constexpr float kSpacing = 0.94f;
constexpr float kStartHeight = 4.5f;
static_assert(kSpacing > kCubeSize, "Initial cube spacing must prevent overlap");

struct CubePose {
    glm::vec3 position = glm::vec3(0.0f);
    glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
};

CubePose InitialPose(int x, int layer, int z) {
    const float centerX = static_cast<float>(kColumns - 1) * 0.5f;
    const float centerZ = static_cast<float>(kDepth - 1) * 0.5f;

    CubePose pose;
    pose.position = glm::vec3(
        (static_cast<float>(x) - centerX) * kSpacing,
        kStartHeight + static_cast<float>(layer) * kSpacing,
        (static_cast<float>(z) - centerZ) * kSpacing);
    // 首帧严格对齐且互不相交；旋转只由 Jolt 的接触冲量产生，避免启动时
    // 旋转后的 OBB 超出网格间距而造成视觉穿透。
    pose.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    return pose;
}

ECS::RigidBodyComponent MakeDynamicCubeBody() {
    ECS::RigidBodyComponent body;
    body.type = ECS::RigidBodyComponent::Type::Dynamic;
    body.shapeType = ECS::RigidBodyComponent::ShapeType::OBB;
    body.size = glm::vec3(2.0f);
    body.offset = glm::vec3(0.0f);
    body.mass = 1.0f;
    body.restitution = 0.0f;
    body.useGravity = true;
    body.isTrigger = false;
    body.useOBB = true;
    body.syncWithModel = false;
    body.autoFitToModel = false;
    return body;
}

ECS::MaterialComponent MakeCubeMaterial() {
    ECS::MaterialComponent material;
    material.albedoColor = glm::vec3(0.10f, 0.46f, 0.82f);
    material.metallic = 0.05f;
    material.roughness = 0.32f;
    material.ao = 1.0f;
    return material;
}

class JoltCubeStackPrototype final : public Game::IGameModule {
public:
    static JoltCubeStackPrototype& GetInstance() {
        static JoltCubeStackPrototype instance;
        return instance;
    }

    const char* GetName() const override { return "joltcubestack"; }

    void OnSceneLoaded() override {
        // 场景重载前引擎已经销毁旧 ECS 实体；只清空本插件缓存，
        // 避免使用可能已被新场景复用的旧 Entity ID。
        m_cubes.clear();
        m_spawnPoses.clear();
        m_cubes.reserve(kCubeCount);
        m_spawnPoses.reserve(kCubeCount);
        m_playing = false;

        // 编辑器停止播放时会恢复“播放前”的 ECS 快照。该快照已经包含
        // 首帧生成的 JoltBox_* 实体；如果这里无条件 SpawnStack，会把同一
        // 组方块再创建一次，第二次播放就会累积 2 倍刚体并耗尽 Jolt 的
        // BroadPhase 节点。优先接管快照中的实体，只有首次场景加载才创建。
        const bool reusedSnapshot = AdoptExistingStack();
        if (!reusedSnapshot) {
            SpawnStack();
        }
        std::printf("[JoltCubeStack] loaded: dynamicBoxes=%zu (%s), boxSize=%.2f, "
                    "spacing=%.2f, ground=20x1x20\n",
                    m_cubes.size(), reusedSnapshot ? "reused snapshot" : "spawned",
                    kCubeSize, kSpacing);
    }

    // 物理步进由引擎 PhysicsSystem 统一执行；插件只负责构造实体。
    void OnUpdate(float deltaSeconds) override { (void)deltaSeconds; }

    // 不注册键盘快捷键，避免和编辑器/相机输入冲突。
    void OnKey(SDL_Keycode key) override { (void)key; }

    void OnRenderUI(Renderer2D& renderer, int viewWidth,
                    int viewHeight) override {
        if (viewWidth <= 0 || viewHeight <= 0) return;

        renderer.DrawRect(glm::vec2(24.0f, 24.0f), glm::vec2(500.0f, 82.0f),
                          glm::vec4(0.015f, 0.025f, 0.05f, 0.78f), 20);
        renderer.DrawRect(glm::vec2(24.0f, 24.0f), glm::vec2(6.0f, 82.0f),
                          m_playing ? glm::vec4(0.10f, 0.78f, 0.95f, 0.95f)
                                    : glm::vec4(0.95f, 0.62f, 0.16f, 0.95f),
                          21);

        TextRenderer& text = TextRenderer::GetInstance();
        text.DrawString("JOLT PHYSICS  |  BOX STACK", 44.0f, 42.0f, 22.0f,
                        glm::vec4(0.88f, 0.95f, 1.0f, 1.0f), 22);

        char details[160];
        std::snprintf(details, sizeof(details),
                      "%zu dynamic BoxShapes  |  tight OBB  |  stretched cube ground",
                      m_cubes.size());
        text.DrawString(details, 44.0f, 72.0f, 16.0f,
                        glm::vec4(0.68f, 0.76f, 0.86f, 1.0f), 22);
    }

    void OnGameStart() override { m_playing = true; }
    void OnGamePause() override { m_playing = false; }
    void OnGameResume() override { m_playing = true; }

    void OnGameStop() override {
        m_playing = false;
        // EditorPlayMode 随后会销毁运行时实体并恢复播放前快照。
        // 不在这里 Remove+Add 864 个刚体，避免停止时重复构建 BroadPhase，
        // 也避免快照恢复期间出现一批短生命周期的旧 Body。
    }

private:
    JoltCubeStackPrototype() = default;
    ~JoltCubeStackPrototype() override = default;
    JoltCubeStackPrototype(const JoltCubeStackPrototype&) = delete;
    JoltCubeStackPrototype& operator=(const JoltCubeStackPrototype&) = delete;

    bool AdoptExistingStack() {
        ECS::Coordinator& coordinator = ECS::Coordinator::GetInstance();

        for (ECS::Entity entity = 0; entity < ECS::MAX_ENTITIES; ++entity) {
            if (!coordinator.HasComponent<ECS::NameComponent>(entity) ||
                !coordinator.HasComponent<ECS::TransformComponent>(entity) ||
                !coordinator.HasComponent<ECS::MeshComponent>(entity) ||
                !coordinator.HasComponent<ECS::RigidBodyComponent>(entity)) {
                continue;
            }

            const std::string& name =
                coordinator.GetComponent<ECS::NameComponent>(entity).name;
            if (name.rfind("JoltBox_", 0) != 0) continue;

            const auto& transform =
                coordinator.GetComponent<ECS::TransformComponent>(entity);
            m_cubes.push_back(entity);
            m_spawnPoses.push_back({transform.position, transform.rotation});
        }

        // 只在完整快照匹配本原型时复用，避免误接管用户手动创建的同名实体。
        if (m_cubes.size() == kCubeCount) return true;

        m_cubes.clear();
        m_spawnPoses.clear();
        return false;
    }

    void SpawnStack() {
        ECS::SceneECS& scene = ECS::SceneECS::GetInstance();
        ECS::Coordinator& coordinator = ECS::Coordinator::GetInstance();
        const ECS::MaterialComponent material = MakeCubeMaterial();

        for (int layer = 0; layer < kLayers; ++layer) {
            for (int z = 0; z < kDepth; ++z) {
                for (int x = 0; x < kColumns; ++x) {
                    const CubePose pose = InitialPose(x, layer, z);
                    const std::size_t index = m_cubes.size();
                    const ECS::Entity entity = scene.CreateCube(
                        "JoltBox_" + std::to_string(index));

                    scene.SetPosition(entity, pose.position);
                    scene.SetRotation(entity, pose.rotation);
                    scene.SetScale(entity, glm::vec3(kCubeSize * 0.5f));

                    coordinator.AddComponent<ECS::MaterialComponent>(entity, material);
                    coordinator.AddComponent<ECS::RigidBodyComponent>(
                        entity, MakeDynamicCubeBody());

                    m_cubes.push_back(entity);
                    m_spawnPoses.push_back(pose);
                }
            }
        }
    }

    std::vector<ECS::Entity> m_cubes;
    std::vector<CubePose> m_spawnPoses;
    bool m_playing = false;
};

} // namespace

#ifndef __ANDROID__
extern "C" __declspec(dllexport) const char* GetGameModuleName() {
    return "joltcubestack";
}

extern "C" __declspec(dllexport) Game::IGameModule* CreateGameModule() {
    return &JoltCubeStackPrototype::GetInstance();
}
#else
namespace {
struct JoltCubeStackAndroidRegistration {
    JoltCubeStackAndroidRegistration() {
        Game::GameManager::GetInstance().Register(
            "joltcubestack", []() -> Game::IGameModule* {
                return &JoltCubeStackPrototype::GetInstance();
            });
    }
};
static JoltCubeStackAndroidRegistration g_joltCubeStackAndroidRegistration;
}
#endif
