// CesiumWalk.cpp - 键盘控制角色行走 3D 原型插件（独立 DLL，引擎零修改）
// 场景 assets/cesiumwalk.json：Main Camera + Directional Light + Skybox +
//   Ground（立方体缩放 + 静态刚体）+ CesiumMan（glTF 蒙皮角色 + 动态胶囊刚体）。
// 玩法逻辑全部在 PlayerWalkScript（场景挂载的 Unity 式脚本）：
//   - WASD 移动（方向基于主相机前向，按 W 朝镜头前方走）
//   - 空格跳跃（Jolt 动态刚体，落地后再次可跳）
//   - 移动时模型转向移动方向（SetRigidBodyOrientation 驱动物理朝向）
//   - 第三人称镜头跟随（脚本每帧驱动 Main Camera 实体 Transform）
//   - 角色动画倍速随移动状态切换
#include "Game/IGameModule.h"
#include "ECS/Types.h"
#include "ECS/SceneECS.h"
#include "ECS/Coordinator.h"
#include "ECS/Components.h"
#include "ECS/ScriptSystem.h"
#include "ECS/PhysicsSystem.h"
#include "Core/PhysicsGlobals.h"   // g_PhysicsManager / g_PhysicsSystemPtr（Game.dll 导出）
#include "Core/InputSystem.h"      // Input::InputSystem 动作映射（WASD/空格默认绑定）
#include "Core/InputController.h"  // sSceneCameraControlLocked：玩法接管场景相机
#include "Rendering/Renderer2D.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdio>
#include <string>

// 模块级状态：PlayerWalkScript 是否成功启动（HUD 诊断用，跨类共享）
// 置于文件顶部：CesiumWalk::OnAlwaysUpdate 定义在其后，需先声明
static bool g_scriptAlive = false;

namespace Game {

class CesiumWalk : public IGameModule {
public:
    static CesiumWalk& GetInstance();

    const char* GetName() const override { return "cesiumwalk"; }
    void OnSceneLoaded() override;
    void OnUpdate(float deltaTime) override;
    void OnAlwaysUpdate(float deltaTime) override;
    void OnKey(SDL_Keycode key) override { (void)key; }
    void OnRenderUI(Renderer2D& r2d, int viewWidth, int viewHeight) override;
    void OnGameStop() override;

private:
    CesiumWalk() = default;
    ~CesiumWalk() override = default;
    CesiumWalk(const CesiumWalk&) = delete;
    CesiumWalk& operator=(const CesiumWalk&) = delete;
};

CesiumWalk& CesiumWalk::GetInstance() {
    static CesiumWalk instance;
    return instance;
}

void CesiumWalk::OnSceneLoaded() {
    // 兜底：幂等补齐脚本实例（无论场景经 EngineMain 命令行还是编辑器 SceneManager 加载）
    ECS::ScriptSystem::GetInstance().InstantiateAll(true);
    auto& scene = ECS::SceneECS::GetInstance();
    ECS::Entity player = scene.FindByName("CesiumMan");
    // 模块层接管场景相机（无论脚本是否挂载成功都生效）
    SetSceneCameraControlLocked(true);
    printf("[CesiumWalk] scene loaded, player=%u, cameraControlLocked=%d\n",
           (unsigned)player, IsSceneCameraControlLocked() ? 1 : 0);
}

void CesiumWalk::OnGameStop() {
    // 停止播放后释放相机锁，恢复编辑器可操控相机
    SetSceneCameraControlLocked(false);
    printf("[CesiumWalk] game stopped, cameraControlLocked released\n");
}

void CesiumWalk::OnUpdate(float deltaTime) {
    // 玩法全部由 PlayerWalkScript 驱动（场景实体挂载），模块层保持空实现
    (void)deltaTime;
}

void CesiumWalk::OnAlwaysUpdate(float deltaTime) {
    // HUD 诊断（无条件执行）：脚本是否启动、刚体是否有效、每个按键是否被引擎读到
    (void)deltaTime;
    auto& coordinator = ECS::Coordinator::GetInstance();
    auto& scene = ECS::SceneECS::GetInstance();
    static ECS::Entity infoText = ECS::INVALID_ENTITY;
    static ECS::Entity player = ECS::INVALID_ENTITY;
    if (infoText == ECS::INVALID_ENTITY) infoText = scene.FindByName("InfoText");
    if (player == ECS::INVALID_ENTITY) player = scene.FindByName("FoxPlayer");
    if (infoText == ECS::INVALID_ENTITY ||
        !coordinator.HasComponent<ECS::TextComponent>(infoText)) {
        return;
    }

    auto& input = Input::InputSystem::GetInstance();
    const bool bodyOk = (g_PhysicsSystemPtr && player != ECS::INVALID_ENTITY) &&
                        !g_PhysicsSystemPtr->GetRigidBodyId(player).IsInvalid();
    // 显式中间变量：绕开 MSVC C4477 对 snprintf 三目参数的类型检查误报
    const char* scriptState = g_scriptAlive ? "on" : "OFF";
    const char* bodyState = bodyOk ? "ok" : "MISSING";
    const int kUp = input.IsDown("MoveUp") ? 1 : 0;
    const int kDown = input.IsDown("MoveDown") ? 1 : 0;
    const int kLeft = input.IsDown("MoveLeft") ? 1 : 0;
    const int kRight = input.IsDown("MoveRight") ? 1 : 0;
    const int kJump = input.IsDown("Jump") ? 1 : 0;

    // 角色实时物理速度与位置（诊断：速度应随按键变化，位置应随之移动）
    char velStr[64] = "-";
    glm::vec3 pos(0.0f);
    if (g_PhysicsSystemPtr && player != ECS::INVALID_ENTITY) {
        JPH::BodyID body = g_PhysicsSystemPtr->GetRigidBodyId(player);
        if (!body.IsInvalid()) {
            glm::vec3 v = g_PhysicsManager.GetLinearVelocity(body);
            std::snprintf(velStr, sizeof(velStr), "(%.1f,%.1f,%.1f)", v.x, v.y, v.z);
        }
    }
    if (coordinator.HasComponent<ECS::TransformComponent>(player)) {
        pos = coordinator.GetComponent<ECS::TransformComponent>(player).position;
    }

    char buf[320];
    std::snprintf(buf, sizeof(buf),
        "WASD move/SPACE jump | script:%s body:%s | W:%d S:%d A:%d D:%d J:%d | vel:%s pos:(%.1f,%.1f,%.1f)",
        scriptState, bodyState, kUp, kDown, kLeft, kRight, kJump, velStr, pos.x, pos.y, pos.z);
    coordinator.GetComponent<ECS::TextComponent>(infoText).text = buf;
}

void CesiumWalk::OnRenderUI(Renderer2D& r2d, int viewWidth, int viewHeight) {
    // 屏幕提示文字由场景树 textComp 渲染（PlayerWalkScript 每帧更新），此处不硬编码绘制
    (void)r2d; (void)viewWidth; (void)viewHeight;
}

} // namespace Game

// ===== Unity 式脚本：PlayerWalkScript =====
//（g_scriptAlive 已声明于文件顶部）
class PlayerWalkScript : public ECS::IScriptBehaviour {
public:
    const char* GetScriptName() const override { return "PlayerWalkScript"; }

    void OnStart(ECS::Entity entity) override {
        m_Entity = entity;
        auto& scene = ECS::SceneECS::GetInstance();
        auto& coordinator = ECS::Coordinator::GetInstance();
        m_CameraEntity = scene.FindByName("Main Camera");
        m_InfoText = scene.FindByName("InfoText");
        // 玩法接管场景相机：引擎不再用 WASD/鼠标驱动相机实体（WASD 全部归角色）
        SetSceneCameraControlLocked(true);
        g_scriptAlive = true;

        // 初始化角色朝向基准（平滑转向用）：从场景 transform 提取绕 Y 角
        if (coordinator.HasComponent<ECS::TransformComponent>(m_Entity)) {
            const auto& tf = coordinator.GetComponent<ECS::TransformComponent>(m_Entity);
            const glm::quat& q = tf.rotation;
            m_CurrentYaw = std::atan2(2.0f * (q.w * q.y + q.x * q.z),
                                      1.0f - 2.0f * (q.y * q.y + q.z * q.z));
            m_YawInitialized = true;
        }
        printf("[PlayerWalkScript] started on entity %u, camera=%u\n",
               (unsigned)entity, (unsigned)m_CameraEntity);
    }

    void OnUpdate(float deltaTime) override {
        auto& scene = ECS::SceneECS::GetInstance();
        auto& coordinator = ECS::Coordinator::GetInstance();
        auto& input = Input::InputSystem::GetInstance();

        // 1) 采样输入（默认绑定：MoveUp=W/↑, MoveDown=S/↓, MoveLeft=A/←, MoveRight=D/→, Jump=空格）
        float w = input.IsDown("MoveUp") ? 1.0f : 0.0f;
        float s = input.IsDown("MoveDown") ? 1.0f : 0.0f;
        float a = input.IsDown("MoveLeft") ? 1.0f : 0.0f;
        float d = input.IsDown("MoveRight") ? 1.0f : 0.0f;
        // headless 自测钩子：autoWalk>0 模拟按住 W（无窗口环境验证运动链路）
        if (autoWalk > 0.5f) w = 1.0f;

        // 2) 第三人称镜头跟随（最先执行且不依赖刚体：脚本一启动镜头即锁定角色身后）
        if (m_CameraEntity != ECS::INVALID_ENTITY &&
            coordinator.HasComponent<ECS::TransformComponent>(m_Entity)) {
            auto& playerTf = coordinator.GetComponent<ECS::TransformComponent>(m_Entity);
            glm::vec3 playerPos = playerTf.position;
            // 镜头完全锁定：固定世界偏移（不随角色朝向旋转——角色转向不影响镜头位置）
            glm::vec3 camTarget = playerPos + glm::vec3(0.0f, camHeight, -camDistance);

            const float k = 1.0f - std::exp(-camLerpSpeed * deltaTime);
            if (!m_CamPosInitialized) {
                m_CamPos = camTarget;
                m_CamPosInitialized = true;
            } else {
                m_CamPos = glm::mix(m_CamPos, camTarget, k);
            }

            // 看向角色上半身
            glm::vec3 lookTarget = playerPos + glm::vec3(0.0f, 1.0f, 0.0f);
            glm::vec3 dir = glm::normalize(lookTarget - m_CamPos);
            glm::quat targetRot = glm::quatLookAt(dir, glm::vec3(0.0f, 1.0f, 0.0f));
            m_CamRot = (m_CamRotInitialized) ? glm::slerp(m_CamRot, targetRot, k) : targetRot;
            m_CamRotInitialized = true;

            scene.SetPosition(m_CameraEntity, m_CamPos);
            scene.SetRotation(m_CameraEntity, m_CamRot);
        }

        // 3) 移动方向基于主相机朝向的水平投影（按 W 朝镜头前方走，镜头在角色身后）
        glm::quat camRot = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        if (coordinator.HasComponent<ECS::TransformComponent>(m_CameraEntity)) {
            camRot = coordinator.GetComponent<ECS::TransformComponent>(m_CameraEntity).rotation;
        }
        glm::vec3 camFwd = glm::normalize(camRot * glm::vec3(0.0f, 0.0f, -1.0f));
        glm::vec3 camRight = glm::normalize(camRot * glm::vec3(1.0f, 0.0f, 0.0f));
        camFwd.y = 0.0f; camFwd = glm::normalize(camFwd);
        camRight.y = 0.0f; camRight = glm::normalize(camRight);
        glm::vec3 moveDir = camFwd * (w - s) + camRight * (d - a);
        const bool moving = glm::dot(moveDir, moveDir) > 1e-4f;
        if (moving) moveDir = glm::normalize(moveDir);

        // 4) 物理控制：保留垂直分量（重力），水平分量 = 输入方向 × 速度
        JPH::BodyID bodyID = g_PhysicsSystemPtr->GetRigidBodyId(m_Entity);
        if (bodyID.IsInvalid()) {
            if (!m_WarnedBodyMissing) {
                m_WarnedBodyMissing = true;
                printf("[PlayerWalkScript] WARN: no rigid body for entity %u - physics control disabled\n",
                       (unsigned)m_Entity);
            }
            return;
        }
        // 防倾倒：每帧把刚体旋转强制为"仅绕 Y"（物理积分产生的 X/Z 旋转被覆盖归零）。
        // 用导出 API SetRigidBodyOrientation，避免插件链接 Jolt 静态库。
        if (coordinator.HasComponent<ECS::TransformComponent>(m_Entity)) {
            const auto& tf = coordinator.GetComponent<ECS::TransformComponent>(m_Entity);
            const glm::quat& q = tf.rotation; // 物理→Transform 写回值
            const float yaw = std::atan2(2.0f * (q.w * q.y + q.x * q.z),
                                         1.0f - 2.0f * (q.y * q.y + q.z * q.z));
            g_PhysicsManager.SetRigidBodyOrientation(bodyID,
                glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f)));
        }
        glm::vec3 vel = g_PhysicsManager.GetLinearVelocity(bodyID);

        // 跳跃：仅在地面附近（垂直速度接近 0）时允许
        if (input.IsPressed("Jump") && std::abs(vel.y) < 0.8f) {
            vel.y = jumpSpeed;
        }
        vel.x = moveDir.x * moveSpeed;
        vel.z = moveDir.z * moveSpeed;
        g_PhysicsManager.SetLinearVelocity(bodyID, vel);

        // 5) 平滑转向：朝向以最大角速度向移动方向过渡（最短路径，不瞬转）
        if (moving) {
            const float targetYaw = std::atan2(moveDir.x, moveDir.z) + glm::radians(yawOffsetDeg);
            if (!m_YawInitialized) { m_CurrentYaw = targetYaw; m_YawInitialized = true; }
            float delta = targetYaw - m_CurrentYaw;
            delta = std::atan2(std::sin(delta), std::cos(delta)); // 归一化到 [-π,π]
            const float maxDelta = glm::radians(turnSpeedDeg) * deltaTime;
            delta = glm::clamp(delta, -maxDelta, maxDelta);
            m_CurrentYaw += delta;
            g_PhysicsManager.SetRigidBodyOrientation(bodyID,
                glm::angleAxis(m_CurrentYaw, glm::vec3(0.0f, 1.0f, 0.0f)));
        }

        // 6) 动画状态机（Fox.glb: 0=Survey待机, 1=Walk, 2=Run）
        //    静止→待机 clip；慢速→行走；快速→奔跑。clip 切换由引擎自动 PlayAnimation，
        //    同 clip 内速度用指数平滑（起步/停步不跳变）。
        //    disableAnim>0 时跳过动画驱动（调试：隔离"切换 VSync 后模型消失"是否动画驱动）
        if (!disableAnim && coordinator.HasComponent<ECS::AnimatorComponent>(m_Entity)) {
            auto& anim = coordinator.GetComponent<ECS::AnimatorComponent>(m_Entity);
            const float horiz = glm::length(glm::vec2(vel.x, vel.z));
            int targetClip = idleClipIndex;
            float targetSpeed = 1.0f;
            if (moving && horiz >= 0.3f) {
                if (horiz < runSpeedRatio * moveSpeed) {
                    targetClip = walkClipIndex;
                    targetSpeed = glm::clamp(horiz / (runSpeedRatio * moveSpeed), 0.2f, 1.0f) * walkAnimSpeed;
                } else {
                    targetClip = runClipIndex;
                    targetSpeed = glm::clamp(horiz / moveSpeed, 0.6f, 1.5f);
                }
            }
            anim.clipIndex = targetClip; // 引擎检测变化自动 PlayAnimation（新 clip 从头播放）
            anim.loop = true;
            const float k = 1.0f - std::exp(-animBlendSpeed * deltaTime);
            m_CurrentAnimSpeed += (targetSpeed - m_CurrentAnimSpeed) * k;
            anim.speed = m_CurrentAnimSpeed;
            if (m_LastAnimClip != anim.clipIndex) {
                printf("[PlayerWalkScript] anim clip %d -> %d (speed %.2f, horiz %.2f)\n",
                       m_LastAnimClip, anim.clipIndex, m_CurrentAnimSpeed, horiz);
                m_LastAnimClip = anim.clipIndex;
            }
        }
    }

    void OnDestroy() override {
        g_scriptAlive = false;
        if (m_Entity != ECS::INVALID_ENTITY) {
            SetSceneCameraControlLocked(false);
        }
    }

    // 参数字段表（Unity 式 inspector 字段）：场景 JSON params.* 注入
    const ECS::FieldMeta* GetParamFields(int& outCount) const override {
        outCount = 15;
        return s_Fields;
    }

    float moveSpeed = 5.0f;        // 行走速度 m/s
    float jumpSpeed = 7.0f;        // 跳跃初速度 m/s
    float camDistance = 6.5f;      // 镜头与角色水平距离
    float camHeight = 3.0f;        // 镜头相对角色脚底高度
    float camLerpSpeed = 8.0f;     // 镜头平滑速度（越大越跟手）
    float yawOffsetDeg = 0.0f;     // 模型正面朝向修正（Fox 正面 +Z，无需修正）
    float walkAnimSpeed = 1.5f;    // 行走动画倍速
    float animBlendSpeed = 6.0f;   // 动画速度平滑过渡系数（越大过渡越快）
    int idleClipIndex = 0;         // 待机动画 clip（Fox: Survey）
    int walkClipIndex = 1;         // 行走动画 clip（Fox: Walk）
    int runClipIndex = 2;          // 奔跑动画 clip（Fox: Run）
    float runSpeedRatio = 0.6f;    // 切奔跑的速度阈值（×moveSpeed）
    float turnSpeedDeg = 360.0f;   // 转向速率（°/s），越小转向越柔和
    int disableAnim = 0;           // 调试：>0 跳过动画驱动（隔离 VSync 切换模型消失）
    float autoWalk = 0.0f;         // headless 自测：>0 模拟按住 W

private:
    static const ECS::FieldMeta s_Fields[];
    ECS::Entity m_Entity = ECS::INVALID_ENTITY;
    ECS::Entity m_CameraEntity = ECS::INVALID_ENTITY;
    ECS::Entity m_InfoText = ECS::INVALID_ENTITY;
    float m_CurrentYaw = 0.0f;
    bool m_YawInitialized = false;   // 朝向基准是否已初始化
    bool m_WarnedBodyMissing = false;
    float m_CurrentAnimSpeed = 0.0f;   // 当前动画速度（平滑过渡用）
    int m_LastAnimClip = -1;           // 上次动画 clip（诊断日志用）
    glm::vec3 m_CamPos = glm::vec3(0.0f);
    bool m_CamPosInitialized = false;
    glm::quat m_CamRot = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    bool m_CamRotInitialized = false;
};

const ECS::FieldMeta PlayerWalkScript::s_Fields[] = {
    SCRIPT_FIELD(PlayerWalkScript, moveSpeed, Float, "行走速度 m/s"),
    SCRIPT_FIELD(PlayerWalkScript, jumpSpeed, Float, "跳跃初速度 m/s"),
    SCRIPT_FIELD(PlayerWalkScript, camDistance, Float, "镜头距离"),
    SCRIPT_FIELD(PlayerWalkScript, camHeight, Float, "镜头高度"),
    SCRIPT_FIELD(PlayerWalkScript, camLerpSpeed, Float, "镜头平滑速度"),
    SCRIPT_FIELD(PlayerWalkScript, yawOffsetDeg, Float, "模型朝向修正(°)"),
    SCRIPT_FIELD(PlayerWalkScript, walkAnimSpeed, Float, "满速行走动画倍速"),
    SCRIPT_FIELD(PlayerWalkScript, animBlendSpeed, Float, "动画速度平滑过渡"),
    SCRIPT_FIELD(PlayerWalkScript, idleClipIndex, Int, "待机动画 clip"),
    SCRIPT_FIELD(PlayerWalkScript, walkClipIndex, Int, "行走动画 clip"),
    SCRIPT_FIELD(PlayerWalkScript, runClipIndex, Int, "奔跑动画 clip"),
    SCRIPT_FIELD(PlayerWalkScript, runSpeedRatio, Float, "奔跑速度阈值"),
    SCRIPT_FIELD(PlayerWalkScript, turnSpeedDeg, Float, "转向速率(°/s)"),
    SCRIPT_FIELD(PlayerWalkScript, disableAnim, Int, "禁用动画(调试)"),
    SCRIPT_FIELD(PlayerWalkScript, autoWalk, Float, "headless 自测:模拟按 W"),
};
REGISTER_SCRIPT(PlayerWalkScript, "PlayerWalkScript");

// ===== 插件导出(引擎 GameManager::LoadPlugin 约定) =====
extern "C" __declspec(dllexport) const char* GetGameModuleName() {
    return "cesiumwalk"; // 与 assets/cesiumwalk.json 顶层 "game" 键一致
}

extern "C" __declspec(dllexport) Game::IGameModule* CreateGameModule() {
    return &Game::CesiumWalk::GetInstance();
}
