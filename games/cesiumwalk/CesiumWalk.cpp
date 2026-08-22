// CesiumWalk.cpp - 键盘控制角色行走 3D 原型插件（独立 DLL，引擎零修改）
// 场景 assets/third_person_prototype.json：Main Camera + Directional Light + Skybox +
//   Ground/方块 + AnimationPlayer（骨骼角色）+ PuppetEnemy（静态木偶目标）。
// 玩法逻辑全部在 PlayerWalkScript（场景挂载的 Unity 式脚本）：
//   - WASD 移动（方向基于主相机前向，按 W 朝镜头前方走）
//   - 空格跳跃（Jolt 动态刚体，落地后再次可跳）
//   - 移动时模型转向移动方向（SetRigidBodyOrientation 驱动物理朝向）
//   - 第三人称镜头跟随（由引擎 ThirdPersonCameraSystem 驱动 Main Camera）
//   - 角色动画倍速随移动状态切换
#include "Game/IGameModule.h"
#include "ECS/Types.h"
#include "ECS/SceneECS.h"
#include "ECS/Coordinator.h"
#include "ECS/Components.h"
#include "ECS/ScriptSystem.h"
#include "ECS/PhysicsSystem.h"
#include "Core/PhysicsGlobals.h"   // g_PhysicsSystemPtr（Game.dll 导出）
#include "Core/InputSystem.h"      // Input::InputSystem 动作映射（WASD/空格默认绑定）
#include "Core/InputController.h"  // sSceneCameraControlLocked：玩法接管场景相机
#include "Rendering/Renderer2D.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <queue>
#include <string>
#include <vector>

// 模块级状态：PlayerWalkScript 是否成功启动（HUD 诊断用，跨类共享）
// 置于文件顶部：CesiumWalk::OnAlwaysUpdate 定义在其后，需先声明
static bool g_scriptAlive = false;
static const char* g_playerAnimState = "Idle";
// 原型玩家生命值：由 PlayerWalkScript 驱动，CesiumWalk::OnRenderUI 只负责显示。
static float g_playerHealth = 100.0f;
static float g_playerMaxHealth = 100.0f;
// 敌人攻击通过队列修改 PlayerWalkScript 的真实 health，避免直接改 HUD 快照后
// 下一帧又被玩家脚本覆盖。脚本更新顺序不固定时，最多延迟一个逻辑帧生效。
static float g_playerDamagePending = 0.0f;
static float g_puppetHealth = 100.0f;
static float g_puppetMaxHealth = 100.0f;
static const char* g_puppetAnimState = "Idle";
static const char* g_puppetAiState = "Patrol";
static float g_puppetHitFlashTimer = 0.0f;
static int g_puppetHitCount = 0;
// 屏幕空间敌人血条的世界锚点，按角色模型在场景中的实际头顶高度配置。
// 这不是实体 ID/尺寸 hack：更换模型时只需调整脚本参数 healthBarOffsetY。
static float g_puppetHealthBarOffsetY = 2.1f;

// 第三人称移动的唯一方向来源：渲染相机的水平前向和局部右向。
// 不能用世界 -Z/+X 兜底，否则相机环绕后会出现“角色仍只朝固定轴移动”的手感。
static void BuildCameraRelativeBasis(ECS::SceneECS& scene,
                                      ECS::Coordinator& coordinator,
                                      ECS::Entity cameraEntity,
                                      ECS::Entity targetEntity,
                                      glm::vec3& outForward,
                                      glm::vec3& outRight) {
    constexpr float kBasisEpsilon = 0.000001f;
    const glm::vec3 worldUp(0.0f, 1.0f, 0.0f);
    outForward = glm::vec3(0.0f, 0.0f, 1.0f);
    bool forwardFromRotation = false;

    // CameraComponent 的渲染约定是 rotation * (0,0,-1)。
    // 直接从相机旋转提取，保证移动方向与屏幕中看到的前方一致。
    if (cameraEntity != ECS::INVALID_ENTITY &&
        coordinator.HasComponent<ECS::TransformComponent>(cameraEntity)) {
        const glm::quat rawRotation =
            coordinator.GetComponent<ECS::TransformComponent>(cameraEntity).rotation;
        const float rotationLengthSquared = glm::dot(rawRotation, rawRotation);
        if (std::isfinite(rotationLengthSquared) &&
            rotationLengthSquared > kBasisEpsilon) {
            const glm::quat rotation = rawRotation / std::sqrt(rotationLengthSquared);
            glm::vec3 forward = rotation * glm::vec3(0.0f, 0.0f, -1.0f);
            forward.y = 0.0f;
            const float forwardLengthSquared = glm::dot(forward, forward);
            if (std::isfinite(forwardLengthSquared) &&
                forwardLengthSquared > kBasisEpsilon) {
                outForward = forward / std::sqrt(forwardLengthSquared);
                forwardFromRotation = true;
            }
        }
    }

    // 场景重载/相机刚创建的极短窗口里，若旋转还不可用，使用相机位置到跟随
    // 目标的水平方向作为同一语义的安全回退，而不是跳到固定世界轴。
    if (cameraEntity != ECS::INVALID_ENTITY &&
        targetEntity != ECS::INVALID_ENTITY &&
        coordinator.HasComponent<ECS::TransformComponent>(cameraEntity) &&
        coordinator.HasComponent<ECS::TransformComponent>(targetEntity)) {
        const glm::vec3 cameraPosition = scene.GetWorldPosition(cameraEntity);
        const glm::vec3 targetPosition = scene.GetWorldPosition(targetEntity);
        glm::vec3 toTarget = targetPosition - cameraPosition;
        toTarget.y = 0.0f;
        const float toTargetLengthSquared = glm::dot(toTarget, toTarget);
        if (!std::isfinite(toTargetLengthSquared) ||
            toTargetLengthSquared <= kBasisEpsilon) {
            // 保留已从有效相机旋转提取的前向。
        } else if (!forwardFromRotation) {
            outForward = toTarget / std::sqrt(toTargetLengthSquared);
        }
    }

    // 对于相机前向 F，cross(F, Up) 正是相机局部 +X（屏幕右方）。
    // 因此 D=+right、A=-right，且两者随环绕相机同步旋转。
    outRight = glm::cross(outForward, worldUp);
    const float rightLengthSquared = glm::dot(outRight, outRight);
    if (!std::isfinite(rightLengthSquared) || rightLengthSquared <= kBasisEpsilon) {
        outRight = glm::vec3(-1.0f, 0.0f, 0.0f);
    } else {
        outRight /= std::sqrt(rightLengthSquared);
    }
}

static void QueuePlayerDamage(float amount) {
    if (!std::isfinite(amount) || amount <= 0.0f) return;
    g_playerDamagePending = std::min(1000.0f, g_playerDamagePending + amount);
}

static ECS::Entity FindPlayerEntity(ECS::SceneECS& scene) {
    // 场景原型使用过多个角色名；统一在这里解析，避免重载场景后缓存旧实体 ID。
    const char* names[] = {"AnimationPlayer", "FoxPlayer", "CesiumMan"};
    for (const char* name : names) {
        const ECS::Entity entity = scene.FindByName(name);
        if (entity != ECS::INVALID_ENTITY) return entity;
    }
    return ECS::INVALID_ENTITY;
}

static bool ProjectWorldToScreen(ECS::Entity cameraEntity, const glm::vec3& worldPosition,
                                 int viewWidth,
                                 int viewHeight, glm::vec2& outScreenPosition) {
    if (viewWidth <= 0 || viewHeight <= 0 || cameraEntity == ECS::INVALID_ENTITY) {
        return false;
    }

    auto& coordinator = ECS::Coordinator::GetInstance();
    if (!coordinator.HasComponent<ECS::CameraComponent>(cameraEntity) ||
        !coordinator.HasComponent<ECS::TransformComponent>(cameraEntity)) {
        return false;
    }

    const auto& camera = coordinator.GetComponent<ECS::CameraComponent>(cameraEntity);
    const auto& cameraTransform = coordinator.GetComponent<ECS::TransformComponent>(cameraEntity);
    const float aspectRatio = static_cast<float>(viewWidth) / static_cast<float>(viewHeight);
    const glm::mat4 view = camera.GetViewMatrix(cameraTransform.position, cameraTransform.rotation);
    glm::mat4 projection = camera.GetProjectionMatrix(aspectRatio);
    projection[1][1] *= -1.0f; // 与 SceneRenderer::GetMainCameraMatrices 保持一致。

    const glm::vec4 clipPosition = projection * view * glm::vec4(worldPosition, 1.0f);
    if (clipPosition.w <= 0.001f) {
        return false; // 目标在相机后方。
    }

    const glm::vec3 ndc = glm::vec3(clipPosition) / clipPosition.w;
    if (!std::isfinite(ndc.x) || !std::isfinite(ndc.y) || !std::isfinite(ndc.z) ||
        ndc.x < -1.0f || ndc.x > 1.0f || ndc.y < -1.0f || ndc.y > 1.0f ||
        ndc.z < -1.0f || ndc.z > 1.0f) {
        return false;
    }

    // 主相机 projection 已按 Vulkan 约定翻转 Y；与 Canvas2D 的屏幕坐标同向，
    // NDC(-1,-1) 对应左上，NDC(+1,+1) 对应右下。
    outScreenPosition = (glm::vec2(ndc.x, ndc.y) * 0.5f + glm::vec2(0.5f)) *
                        glm::vec2(static_cast<float>(viewWidth), static_cast<float>(viewHeight));
    return true;
}

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
    ECS::Entity player = scene.FindByName("AnimationPlayer");
    if (player == ECS::INVALID_ENTITY) player = scene.FindByName("CesiumMan");
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
    static uint32_t cachedEntitySetVersion = std::numeric_limits<uint32_t>::max();
    const uint32_t entitySetVersion = scene.GetEntitySetVersion();
    const bool entitySetChanged = cachedEntitySetVersion != entitySetVersion;
    if (entitySetChanged ||
        infoText == ECS::INVALID_ENTITY ||
        !coordinator.HasComponent<ECS::TextComponent>(infoText)) {
        infoText = scene.FindByName("InfoText");
    }
    if (entitySetChanged ||
        player == ECS::INVALID_ENTITY ||
        !coordinator.HasComponent<ECS::NameComponent>(player) ||
        !coordinator.HasComponent<ECS::TransformComponent>(player)) {
        player = FindPlayerEntity(scene);
    }
    cachedEntitySetVersion = entitySetVersion;
    if (infoText == ECS::INVALID_ENTITY ||
        !coordinator.HasComponent<ECS::TextComponent>(infoText)) {
        return;
    }

    const bool bodyOk = (g_PhysicsSystemPtr && player != ECS::INVALID_ENTITY) &&
                        !g_PhysicsSystemPtr->GetRigidBodyId(player).IsInvalid();
    const char* scriptState = g_scriptAlive ? "on" : "OFF";
    const char* bodyState = bodyOk ? "ok" : "MISSING";
    char buf[320];
    std::snprintf(buf, sizeof(buf),
        "状态:%s  脚本:%s  物理:%s\n"
        "WASD移动  Ctrl奔跑  C蹲伏  F攻击  空格跳跃\n"
        "AI:%s  右键环视  滚轮缩放  Shift瞄准  Q锁敌",
        g_playerAnimState, scriptState, bodyState, g_puppetAiState);
    coordinator.GetComponent<ECS::TextComponent>(infoText).text = buf;
}

void CesiumWalk::OnRenderUI(Renderer2D& r2d, int viewWidth, int viewHeight) {
    // 屏幕提示文字由场景树 textComp 渲染（PlayerWalkScript 每帧更新）。
    // 玩家血条是固定 HUD；敌人血条使用“世界锚点 -> 屏幕投影”的屏幕空间广告牌，
    // 保持像素尺寸并随相机环绕/缩放跟随，避免和玩家 HUD 堆在一起。
    const auto drawHealthBar = [&](const glm::vec2& barPos, float ratio,
                                   const glm::vec4& fillColor, const glm::vec2& barSize,
                                   int layerBase) {
        const float barWidth = std::max(1.0f, barSize.x);
        const float barHeight = std::max(1.0f, barSize.y);
        const glm::vec2 frameSize(barWidth + 8.0f, barHeight + 8.0f);
        const glm::vec2 fillPos = barPos + glm::vec2(2.0f, 2.0f);
        const glm::vec2 fillSize((barWidth - 4.0f) * ratio, barHeight - 4.0f);

        // 黑色外框 + 深色空槽 + 生命值填充；layer 高于场景 Canvas 的提示文字。
        r2d.DrawRect(barPos - glm::vec2(4.0f), frameSize,
                     glm::vec4(0.02f, 0.02f, 0.025f, 0.92f), layerBase);
        r2d.DrawRect(barPos, glm::vec2(barWidth, barHeight),
                     glm::vec4(0.12f, 0.03f, 0.035f, 0.95f), layerBase + 1);
        if (fillSize.x > 0.0f) {
            r2d.DrawRect(fillPos, fillSize, fillColor, layerBase + 2);
        }
    };

    const float playerMaxHealth = std::max(1.0f, g_playerMaxHealth);
    const float playerRatio = glm::clamp(g_playerHealth / playerMaxHealth, 0.0f, 1.0f);
    const glm::vec4 playerColor = playerRatio > 0.5f
        ? glm::vec4(0.18f, 0.82f, 0.24f, 1.0f)
        : (playerRatio > 0.25f
            ? glm::vec4(0.95f, 0.68f, 0.12f, 1.0f)
            : glm::vec4(0.90f, 0.12f, 0.08f, 1.0f));
    drawHealthBar(glm::vec2(40.0f, 60.0f), playerRatio, playerColor,
                  glm::vec2(280.0f, 24.0f), 20);

    // 敌人血条：屏幕空间广告牌。目标死亡后隐藏，避免空血条一直悬在尸体上。
    if (viewWidth > 0 && viewHeight > 0 && g_puppetHealth > 0.0f) {
        auto& scene = ECS::SceneECS::GetInstance();
        auto& coordinator = ECS::Coordinator::GetInstance();
        const ECS::Entity puppet = scene.FindByName("PuppetEnemy");
        const ECS::Entity camera = scene.FindByName("Main Camera");
        if (puppet != ECS::INVALID_ENTITY && camera != ECS::INVALID_ENTITY &&
            coordinator.HasComponent<ECS::TransformComponent>(puppet)) {
            const glm::vec3 anchorWorld = scene.GetWorldPosition(puppet) +
                                          glm::vec3(0.0f, g_puppetHealthBarOffsetY, 0.0f);
            glm::vec2 anchorScreen(0.0f);
            if (ProjectWorldToScreen(camera, anchorWorld, viewWidth, viewHeight,
                                     anchorScreen)) {
                constexpr float barWidth = 132.0f;
                constexpr float barHeight = 8.0f;
                constexpr float barGap = 4.0f;
                const glm::vec2 barPos(anchorScreen.x - barWidth * 0.5f,
                                       anchorScreen.y - barHeight - barGap);
                const bool intersectsViewport = barPos.x + barWidth + 8.0f >= 0.0f &&
                                                barPos.x - 4.0f <= static_cast<float>(viewWidth) &&
                                                barPos.y + barHeight + 8.0f >= 0.0f &&
                                                barPos.y - 4.0f <= static_cast<float>(viewHeight);
                if (intersectsViewport) {
                    const float puppetMaxHealth = std::max(1.0f, g_puppetMaxHealth);
                    const float puppetRatio = glm::clamp(g_puppetHealth / puppetMaxHealth,
                                                         0.0f, 1.0f);
                    const glm::vec4 puppetColor = g_puppetHitFlashTimer > 0.0f
                        ? glm::vec4(1.0f, 0.85f, 0.25f, 1.0f)
                        : glm::vec4(0.88f, 0.18f, 0.12f, 1.0f);
                    drawHealthBar(barPos, puppetRatio, puppetColor,
                                  glm::vec2(barWidth, barHeight), 30);
                }
            }
        }
    }
}

} // namespace Game

// ===== Unity 式脚本：PuppetEnemyScript =====
// 最小敌人 AI：巡逻 → 发现玩家 → 追击 → 近距离反击；玩家离开后回到巡逻原点。
// 敌人使用动态胶囊刚体，AI 只给水平速度和朝向，墙体/玩家碰撞仍由 Jolt 处理。
class PuppetEnemyScript : public ECS::IScriptBehaviour {
public:
    enum class AIState {
        Patrol,
        Chase,
        Attack,
        Return,
    };

    static const char* AIStateName(AIState state) {
        switch (state) {
        case AIState::Patrol: return "Patrol";
        case AIState::Chase: return "Chase";
        case AIState::Attack: return "Attack";
        case AIState::Return: return "Return";
        default: return "Unknown";
        }
    }

    const char* GetScriptName() const override { return "PuppetEnemyScript"; }

    void OnStart(ECS::Entity entity) override {
        m_Entity = entity;
        m_HitTimer = 0.0f;
        m_DeathTimer = 0.0f;
        m_AIStateTime = 0.0f;
        m_AttackCooldownTimer = 0.0f;
        m_AttackHitApplied = false;
        m_Dead = false;
        m_NavRepathTimer = 0.0f;
        m_NavPathIndex = 0;
        m_NavHasTarget = false;

        maxHealth = std::max(1.0f, maxHealth);
        health = glm::clamp(health, 0.0f, maxHealth);
        healthBarOffsetY = std::max(0.1f, healthBarOffsetY);
        detectionRange = std::max(0.5f, detectionRange);
        disengageRange = std::max(detectionRange + 0.5f, disengageRange);
        patrolDistance = std::max(0.1f, patrolDistance);
        patrolSpeed = std::max(0.0f, patrolSpeed);
        chaseSpeed = std::max(0.0f, chaseSpeed);
        attackRange = std::max(0.25f, attackRange);
        attackCooldown = std::max(0.0f, attackCooldown);
        attackHitTime = std::max(0.0f, attackHitTime);
        attackDuration = std::max(attackHitTime + 0.05f, attackDuration);
        pathCellSize = glm::clamp(pathCellSize, 0.25f, 2.0f);
        pathRepathInterval = glm::clamp(pathRepathInterval, 0.1f, 2.0f);
        pathObstaclePadding = glm::clamp(pathObstaclePadding, 0.0f, 0.5f);
        patrolAxis = glm::clamp(patrolAxis, 0, 1);

        auto& scene = ECS::SceneECS::GetInstance();
        m_PatrolOrigin = scene.GetWorldPosition(entity);
        m_PatrolDirection = 1.0f;
        m_PatrolTarget = m_PatrolOrigin + PatrolAxisVector() * patrolDistance;
        m_AIState = AIState::Patrol;

        auto& instances = Instances();
        if (std::find(instances.begin(), instances.end(), this) == instances.end()) {
            instances.push_back(this);
        }

        BuildNavigationGrid();

        g_puppetMaxHealth = maxHealth;
        g_puppetHealth = health;
        g_puppetAiState = health > 0.0f ? AIStateName(AIState::Patrol) : "Down";
        g_puppetAnimState = health > 0.0f ? "Idle" : "Down";
        g_puppetHitFlashTimer = 0.0f;
        g_puppetHitCount = 0;
        g_puppetHealthBarOffsetY = healthBarOffsetY;
        if (health > 0.0f) {
            SetAnimation(idleClipIndex, true);
        } else {
            // 支持场景初始就放置已死亡敌人：只播放倒地动画，
            // 不创建/保留尸体物理，避免死亡姿态被刚体同步覆盖。
            m_Dead = true;
            m_DeathTimer = std::max(0.1f, deathDuration);
            RemoveDeathPhysics();
            SetAnimation(deathClipIndex, false);
        }

        printf("[PuppetEnemyScript] started entity %u, AI=%s detect=%.1f disengage=%.1f patrol=%.1f\n",
               (unsigned)entity, g_puppetAiState, detectionRange, disengageRange, patrolDistance);
    }

    void OnUpdate(float deltaTime) override {
        const float dt = std::max(0.0f, deltaTime);
        g_puppetHitFlashTimer = std::max(0.0f, g_puppetHitFlashTimer - dt);
        if (m_Entity == ECS::INVALID_ENTITY) return;
        m_NavRepathTimer = std::max(0.0f, m_NavRepathTimer - dt);

        maxHealth = std::max(1.0f, maxHealth);
        health = glm::clamp(health, 0.0f, maxHealth);
        healthBarOffsetY = std::max(0.1f, healthBarOffsetY);
        g_puppetMaxHealth = maxHealth;
        g_puppetHealth = health;
        g_puppetHealthBarOffsetY = healthBarOffsetY;

        if (m_Dead || health <= 0.0f) {
            m_Dead = true;
            g_puppetAiState = "Down";
            g_puppetAnimState = "Down";
            return;
        }

        // 受击期间保留 Hit 动画和硬直，不允许 AI 立刻覆盖反馈动画。
        if (m_HitTimer > 0.0f) {
            m_HitTimer = std::max(0.0f, m_HitTimer - dt);
            StopMovement();
            if (m_HitTimer <= 0.0f) {
                g_puppetAiState = AIStateName(m_AIState);
                g_puppetAnimState = AIStateName(m_AIState);
                SetAnimation(idleClipIndex, true);
            }
            return;
        }

        m_AIStateTime += dt;
        m_AttackCooldownTimer = std::max(0.0f, m_AttackCooldownTimer - dt);
        if (!g_PhysicsSystemPtr) return;

        auto& scene = ECS::SceneECS::GetInstance();
        auto& coordinator = ECS::Coordinator::GetInstance();
        const ECS::Entity player = FindPlayerEntity(scene);
        if (player == ECS::INVALID_ENTITY ||
            !coordinator.HasComponent<ECS::TransformComponent>(player) ||
            !coordinator.HasComponent<ECS::TransformComponent>(m_Entity)) {
            if (m_AIState != AIState::Patrol) EnterAIState(AIState::Patrol);
            TickPatrol();
            return;
        }

        const glm::vec3 enemyPosition = scene.GetWorldPosition(m_Entity);
        const glm::vec3 playerPosition = scene.GetWorldPosition(player);
        const float playerDistance = HorizontalDistance(enemyPosition, playerPosition);

        switch (m_AIState) {
        case AIState::Patrol:
            if (playerDistance <= detectionRange) {
                EnterAIState(AIState::Chase);
            } else {
                TickPatrol();
            }
            break;

        case AIState::Chase:
            if (playerDistance > disengageRange) {
                EnterAIState(AIState::Return);
            } else if (playerDistance <= attackRange && m_AttackCooldownTimer <= 0.0f) {
                EnterAIState(AIState::Attack);
            } else if (playerDistance <= attackRange) {
                StopMovement();
                FaceTarget(playerPosition);
                g_puppetAnimState = "Chase";
                SetAnimation(idleClipIndex, true);
            } else {
                MoveToward(playerPosition, chaseSpeed);
            }
            break;

        case AIState::Attack:
            StopMovement();
            FaceTarget(playerPosition);
            if (playerDistance > disengageRange) {
                EnterAIState(AIState::Return);
                break;
            }
            // 攻击有短暂前摇/动作承诺；目标只要还在近战容错范围内就继续播完。
            if (playerDistance > attackRange + 0.45f) {
                EnterAIState(AIState::Chase);
                break;
            }
            if (!m_AttackHitApplied && m_AIStateTime >= attackHitTime) {
                m_AttackHitApplied = true;
                QueuePlayerDamage(attackDamage);
                printf("[PuppetEnemyScript] attack player damage=%.1f distance=%.2f\n",
                       attackDamage, playerDistance);
            }
            if (m_AIStateTime >= attackDuration) {
                m_AttackCooldownTimer = attackCooldown;
                EnterAIState(AIState::Chase);
            }
            break;

        case AIState::Return:
            if (playerDistance <= detectionRange) {
                EnterAIState(AIState::Chase);
            } else if (HorizontalDistance(enemyPosition, m_PatrolOrigin) <= 0.15f) {
                EnterAIState(AIState::Patrol);
            } else {
                MoveToward(m_PatrolOrigin, patrolSpeed);
            }
            break;
        }
    }

    void OnDestroy() override {
        auto& instances = Instances();
        instances.erase(std::remove(instances.begin(), instances.end(), this), instances.end());
        m_Entity = ECS::INVALID_ENTITY;
        g_puppetHealth = 0.0f;
        g_puppetMaxHealth = 100.0f;
        g_puppetAiState = "Patrol";
        g_puppetAnimState = "Idle";
        g_puppetHitFlashTimer = 0.0f;
        g_puppetHitCount = 0;
        g_puppetHealthBarOffsetY = 2.1f;
    }

    // 由玩家攻击状态机在命中帧调用。只选择攻击者前方一定范围内最近的木偶，
    // 不依赖实体名称或硬编码实体 ID，场景里可以安全放置多个木偶。
    static bool TryHit(ECS::Entity attacker, const glm::vec3& attackForward,
                       float range, float damage) {
        auto& scene = ECS::SceneECS::GetInstance();
        auto& coordinator = ECS::Coordinator::GetInstance();
        if (attacker == ECS::INVALID_ENTITY ||
            !coordinator.HasComponent<ECS::TransformComponent>(attacker)) {
            return false;
        }

        glm::vec3 forward(attackForward.x, 0.0f, attackForward.z);
        const float forwardLength = glm::length(forward);
        if (forwardLength < 1e-4f) return false;
        forward /= forwardLength;

        const glm::vec3 attackerPosition = scene.GetWorldPosition(attacker);
        PuppetEnemyScript* nearest = nullptr;
        float nearestDistance = std::max(0.05f, range);
        for (PuppetEnemyScript* target : Instances()) {
            if (!target || target->m_Entity == ECS::INVALID_ENTITY || target->m_Dead ||
                !coordinator.HasComponent<ECS::TransformComponent>(target->m_Entity)) {
                continue;
            }

            glm::vec3 toTarget = scene.GetWorldPosition(target->m_Entity) - attackerPosition;
            toTarget.y = 0.0f;
            const float distance = glm::length(toTarget);
            if (distance > nearestDistance || distance < 1e-4f) continue;

            toTarget /= distance;
            // 150° 的近战扇区，锁定/环绕时不需要像素级对准木偶。
            if (glm::dot(forward, toTarget) < std::cos(glm::radians(75.0f))) continue;
            nearest = target;
            nearestDistance = distance;
        }

        return nearest ? nearest->ApplyDamage(damage) : false;
    }

    float maxHealth = 100.0f;
    float health = 100.0f;
    int idleClipIndex = 9;       // Idle_Loop
    int hitClipIndex = 7;        // Hit_Chest
    int deathClipIndex = 4;      // Death01
    float hitDuration = 0.333f;
    float deathDuration = 2.4f;
    float pathCellSize = 0.5f;          // XZ 网格单元尺寸
    float pathRepathInterval = 0.35f;   // 追击目标路径重算间隔
    float pathObstaclePadding = 0.10f;  // 在胶囊半径外额外留出的间隙
    float animSpeed = 1.0f;
    float healthBarOffsetY = 2.1f; // 世界空间头顶锚点高度
    int moveClipIndex = 45;      // Walk_Loop
    int attackClipIndex = 24;    // Punch_Cross
    int patrolAxis = 0;          // 0=X, 1=Z
    float detectionRange = 6.0f;
    float disengageRange = 10.0f;
    float patrolDistance = 2.0f;
    float patrolSpeed = 1.2f;
    float chaseSpeed = 2.6f;
    float attackRange = 1.7f;
    float attackCooldown = 1.2f;
    float attackHitTime = 0.35f;
    float attackDuration = 0.9f;
    float attackDamage = 10.0f;

    const ECS::FieldMeta* GetParamFields(int& outCount) const override {
        outCount = 25;
        return s_Fields;
    }

private:
    struct NavigationObstacle {
        glm::vec3 center = glm::vec3(0.0f);
        glm::vec3 halfExtents = glm::vec3(0.0f);
        float bottom = 0.0f;
        float top = 0.0f;
    };

    struct NavigationGrid {
        bool valid = false;
        float cellSize = 0.5f;
        float floorY = 0.0f;
        float minX = 0.0f;
        float minZ = 0.0f;
        int width = 0;
        int height = 0;
        std::vector<unsigned char> blocked;

        bool InBounds(int x, int z) const {
            return x >= 0 && z >= 0 && x < width && z < height;
        }

        int Index(int x, int z) const {
            return z * width + x;
        }

        bool IsWalkable(int x, int z) const {
            return InBounds(x, z) && blocked[static_cast<size_t>(Index(x, z))] == 0;
        }

        int WorldToCell(const glm::vec3& position) const {
            if (!valid || cellSize <= 0.0f) return -1;
            const int x = static_cast<int>(std::floor((position.x - minX) / cellSize));
            const int z = static_cast<int>(std::floor((position.z - minZ) / cellSize));
            return InBounds(x, z) ? Index(x, z) : -1;
        }

        glm::vec3 CellCenter(int index) const {
            const int x = index % width;
            const int z = index / width;
            return glm::vec3(
                minX + (static_cast<float>(x) + 0.5f) * cellSize,
                floorY,
                minZ + (static_cast<float>(z) + 0.5f) * cellSize);
        }
    };

    static std::vector<PuppetEnemyScript*>& Instances() {
        static std::vector<PuppetEnemyScript*> instances;
        return instances;
    }

    static float HorizontalDistance(const glm::vec3& a, const glm::vec3& b) {
        const glm::vec2 delta(a.x - b.x, a.z - b.z);
        return glm::length(delta);
    }

    glm::vec3 PatrolAxisVector() const {
        return patrolAxis == 1 ? glm::vec3(0.0f, 0.0f, 1.0f)
                               : glm::vec3(1.0f, 0.0f, 0.0f);
    }

    static glm::vec3 WorldHalfExtents(const glm::mat4& world,
                                      const glm::vec3& localHalfExtents) {
        return glm::vec3(
            std::abs(world[0][0]) * localHalfExtents.x +
                std::abs(world[1][0]) * localHalfExtents.y +
                std::abs(world[2][0]) * localHalfExtents.z,
            std::abs(world[0][1]) * localHalfExtents.x +
                std::abs(world[1][1]) * localHalfExtents.y +
                std::abs(world[2][1]) * localHalfExtents.z,
            std::abs(world[0][2]) * localHalfExtents.x +
                std::abs(world[1][2]) * localHalfExtents.y +
                std::abs(world[2][2]) * localHalfExtents.z);
    }

    void BuildNavigationGrid() {
        m_NavGrid = NavigationGrid{};
        m_NavGrid.cellSize = pathCellSize;

        auto& scene = ECS::SceneECS::GetInstance();
        auto& coordinator = ECS::Coordinator::GetInstance();
        std::vector<NavigationObstacle> obstacles;
        int floorIndex = -1;
        float floorArea = 0.0f;

        // 只读取静态盒/OBB。动态刚体会随时变化，不应污染静态寻路图。
        for (ECS::Entity entity = 0; entity < ECS::MAX_ENTITIES; ++entity) {
            if (!coordinator.HasComponent<ECS::RigidBodyComponent>(entity) ||
                !coordinator.HasComponent<ECS::TransformComponent>(entity)) {
                continue;
            }

            const auto& rigidBody = coordinator.GetComponent<ECS::RigidBodyComponent>(entity);
            if (rigidBody.type != ECS::RigidBodyComponent::Type::Static ||
                rigidBody.isTrigger ||
                (rigidBody.shapeType != ECS::RigidBodyComponent::ShapeType::Box &&
                 rigidBody.shapeType != ECS::RigidBodyComponent::ShapeType::OBB)) {
                continue;
            }

            const glm::mat4 world = scene.GetWorldMatrix(entity);
            const glm::vec3 center = glm::vec3(world * glm::vec4(rigidBody.offset, 1.0f));
            const glm::vec3 halfExtents = WorldHalfExtents(
                world, glm::abs(rigidBody.size) * 0.5f);
            if (halfExtents.x < 0.001f || halfExtents.z < 0.001f) continue;

            NavigationObstacle obstacle;
            obstacle.center = center;
            obstacle.halfExtents = halfExtents;
            obstacle.bottom = center.y - halfExtents.y;
            obstacle.top = center.y + halfExtents.y;
            const float horizontalArea = halfExtents.x * halfExtents.z;
            if (horizontalArea > floorArea) {
                floorArea = horizontalArea;
                floorIndex = static_cast<int>(obstacles.size());
            }
            obstacles.push_back(obstacle);
        }

        float minX = 0.0f;
        float maxX = 0.0f;
        float minZ = 0.0f;
        float maxZ = 0.0f;
        if (floorIndex >= 0) {
            const NavigationObstacle& floor = obstacles[static_cast<size_t>(floorIndex)];
            m_NavGrid.floorY = floor.top;
            minX = floor.center.x - floor.halfExtents.x;
            maxX = floor.center.x + floor.halfExtents.x;
            minZ = floor.center.z - floor.halfExtents.z;
            maxZ = floor.center.z + floor.halfExtents.z;
        } else if (!obstacles.empty()) {
            minX = std::numeric_limits<float>::max();
            minZ = std::numeric_limits<float>::max();
            maxX = std::numeric_limits<float>::lowest();
            maxZ = std::numeric_limits<float>::lowest();
            for (const NavigationObstacle& obstacle : obstacles) {
                minX = std::min(minX, obstacle.center.x - obstacle.halfExtents.x);
                maxX = std::max(maxX, obstacle.center.x + obstacle.halfExtents.x);
                minZ = std::min(minZ, obstacle.center.z - obstacle.halfExtents.z);
                maxZ = std::max(maxZ, obstacle.center.z + obstacle.halfExtents.z);
            }
            minX -= 4.0f;
            maxX += 4.0f;
            minZ -= 4.0f;
            maxZ += 4.0f;
        } else {
            // 没有静态地面时仍保留一个以出生点为中心的小型安全网格，
            // 让 AI 能退化为有限范围寻路，而不是访问空数组。
            minX = m_PatrolOrigin.x - 8.0f;
            maxX = m_PatrolOrigin.x + 8.0f;
            minZ = m_PatrolOrigin.z - 8.0f;
            maxZ = m_PatrolOrigin.z + 8.0f;
        }

        const float worldWidth = std::max(m_NavGrid.cellSize, maxX - minX);
        const float worldHeight = std::max(m_NavGrid.cellSize, maxZ - minZ);
        const float maxCellExtent = 256.0f;
        if (std::ceil(worldWidth / m_NavGrid.cellSize) > maxCellExtent ||
            std::ceil(worldHeight / m_NavGrid.cellSize) > maxCellExtent) {
            m_NavGrid.cellSize = std::max(
                m_NavGrid.cellSize,
                std::max(worldWidth / maxCellExtent, worldHeight / maxCellExtent));
        }

        m_NavGrid.minX = minX;
        m_NavGrid.minZ = minZ;
        m_NavGrid.width = std::max(1, static_cast<int>(
            std::ceil(worldWidth / m_NavGrid.cellSize)));
        m_NavGrid.height = std::max(1, static_cast<int>(
            std::ceil(worldHeight / m_NavGrid.cellSize)));
        m_NavGrid.blocked.assign(static_cast<size_t>(
            m_NavGrid.width * m_NavGrid.height), 0);

        float agentRadius = 0.30f;
        if (coordinator.HasComponent<ECS::RigidBodyComponent>(m_Entity) &&
            coordinator.HasComponent<ECS::TransformComponent>(m_Entity)) {
            const auto& rigidBody = coordinator.GetComponent<ECS::RigidBodyComponent>(m_Entity);
            const glm::mat4 actorWorld = scene.GetWorldMatrix(m_Entity);
            const float scaleX = glm::length(glm::vec3(actorWorld[0]));
            const float scaleZ = glm::length(glm::vec3(actorWorld[2]));
            agentRadius = 0.5f * std::max(
                std::abs(rigidBody.size.x) * scaleX,
                std::abs(rigidBody.size.z) * scaleZ);
        }
        agentRadius = std::max(0.10f, agentRadius);

        size_t blockedCount = 0;
        for (size_t obstacleIndex = 0; obstacleIndex < obstacles.size(); ++obstacleIndex) {
            const NavigationObstacle& obstacle = obstacles[obstacleIndex];
            if (static_cast<int>(obstacleIndex) == floorIndex ||
                obstacle.top <= m_NavGrid.floorY + 0.15f ||
                obstacle.bottom > m_NavGrid.floorY + 1.5f) {
                continue;
            }

            const float padding = agentRadius + pathObstaclePadding;
            const float obstacleMinX = obstacle.center.x - obstacle.halfExtents.x - padding;
            const float obstacleMaxX = obstacle.center.x + obstacle.halfExtents.x + padding;
            const float obstacleMinZ = obstacle.center.z - obstacle.halfExtents.z - padding;
            const float obstacleMaxZ = obstacle.center.z + obstacle.halfExtents.z + padding;
            const int minCellX = std::max(0, static_cast<int>(
                std::floor((obstacleMinX - m_NavGrid.minX) / m_NavGrid.cellSize)));
            const int maxCellX = std::min(m_NavGrid.width - 1, static_cast<int>(
                std::ceil((obstacleMaxX - m_NavGrid.minX) / m_NavGrid.cellSize)) - 1);
            const int minCellZ = std::max(0, static_cast<int>(
                std::floor((obstacleMinZ - m_NavGrid.minZ) / m_NavGrid.cellSize)));
            const int maxCellZ = std::min(m_NavGrid.height - 1, static_cast<int>(
                std::ceil((obstacleMaxZ - m_NavGrid.minZ) / m_NavGrid.cellSize)) - 1);

            for (int z = minCellZ; z <= maxCellZ; ++z) {
                for (int x = minCellX; x <= maxCellX; ++x) {
                    const int index = m_NavGrid.Index(x, z);
                    if (m_NavGrid.blocked[static_cast<size_t>(index)] == 0) {
                        m_NavGrid.blocked[static_cast<size_t>(index)] = 1;
                        ++blockedCount;
                    }
                }
            }
        }

        m_NavGrid.valid = true;
        printf("[PuppetEnemyScript] nav grid entity %u: %dx%d cell=%.2f floorY=%.2f obstacles=%zu blocked=%zu radius=%.2f\n",
               (unsigned)m_Entity, m_NavGrid.width, m_NavGrid.height,
               m_NavGrid.cellSize, m_NavGrid.floorY, obstacles.size(),
               blockedCount, agentRadius);
    }

    int FindNearestWalkableCell(int cell) const {
        if (!m_NavGrid.valid || cell < 0) return -1;
        const int originX = cell % m_NavGrid.width;
        const int originZ = cell / m_NavGrid.width;
        if (m_NavGrid.IsWalkable(originX, originZ)) return cell;

        // 物理碰撞可能让目标短暂处在膨胀后的障碍格内，
        // 在附近寻找最近的可行格，避免 A* 直接失败。
        constexpr int maxSearchRadius = 12;
        for (int radius = 1; radius <= maxSearchRadius; ++radius) {
            for (int z = originZ - radius; z <= originZ + radius; ++z) {
                for (int x = originX - radius; x <= originX + radius; ++x) {
                    if (std::max(std::abs(x - originX), std::abs(z - originZ)) != radius ||
                        !m_NavGrid.IsWalkable(x, z)) {
                        continue;
                    }
                    return m_NavGrid.Index(x, z);
                }
            }
        }
        return -1;
    }

    bool RebuildNavigationPath(const glm::vec3& start,
                               const glm::vec3& target) {
        m_NavPath.clear();
        m_NavPathIndex = 0;
        if (!m_NavGrid.valid) return false;

        const int startCell = FindNearestWalkableCell(m_NavGrid.WorldToCell(start));
        const int goalCell = FindNearestWalkableCell(m_NavGrid.WorldToCell(target));
        if (startCell < 0 || goalCell < 0) return false;

        if (startCell == goalCell) {
            m_NavPath.push_back(m_NavGrid.CellCenter(goalCell));
            return true;
        }

        const int totalCells = m_NavGrid.width * m_NavGrid.height;
        const float infinity = std::numeric_limits<float>::infinity();
        std::vector<float> gCost(static_cast<size_t>(totalCells), infinity);
        std::vector<int> parent(static_cast<size_t>(totalCells), -1);
        std::vector<unsigned char> closed(static_cast<size_t>(totalCells), 0);
        using QueueEntry = std::pair<float, int>;
        std::priority_queue<QueueEntry, std::vector<QueueEntry>,
                            std::greater<QueueEntry>> open;

        const int goalX = goalCell % m_NavGrid.width;
        const int goalZ = goalCell / m_NavGrid.width;
        const auto heuristic = [this, goalX, goalZ](int index) {
            const int x = index % m_NavGrid.width;
            const int z = index / m_NavGrid.width;
            return std::hypot(static_cast<float>(x - goalX),
                              static_cast<float>(z - goalZ));
        };

        static constexpr int directions[8][2] = {
            {1, 0}, {-1, 0}, {0, 1}, {0, -1},
            {1, 1}, {1, -1}, {-1, 1}, {-1, -1}
        };
        gCost[static_cast<size_t>(startCell)] = 0.0f;
        open.emplace(heuristic(startCell), startCell);
        bool reached = false;

        while (!open.empty()) {
            const int current = open.top().second;
            open.pop();
            if (closed[static_cast<size_t>(current)] != 0) continue;
            closed[static_cast<size_t>(current)] = 1;
            if (current == goalCell) {
                reached = true;
                break;
            }

            const int currentX = current % m_NavGrid.width;
            const int currentZ = current / m_NavGrid.width;
            for (const auto& direction : directions) {
                const int nextX = currentX + direction[0];
                const int nextZ = currentZ + direction[1];
                if (!m_NavGrid.IsWalkable(nextX, nextZ)) continue;

                // 对角线不能穿过两个相邻障碍格的角，避免胶囊从方块拐角切进去。
                if (direction[0] != 0 && direction[1] != 0 &&
                    (!m_NavGrid.IsWalkable(currentX + direction[0], currentZ) ||
                     !m_NavGrid.IsWalkable(currentX, currentZ + direction[1]))) {
                    continue;
                }

                const int next = m_NavGrid.Index(nextX, nextZ);
                const float stepCost = direction[0] != 0 && direction[1] != 0
                    ? 1.41421356f : 1.0f;
                const float candidate = gCost[static_cast<size_t>(current)] + stepCost;
                if (candidate >= gCost[static_cast<size_t>(next)]) continue;
                gCost[static_cast<size_t>(next)] = candidate;
                parent[static_cast<size_t>(next)] = current;
                open.emplace(candidate + heuristic(next), next);
            }
        }

        if (!reached) return false;

        std::vector<glm::vec3> reversedPath;
        bool reachedStart = false;
        for (int current = goalCell; current >= 0;
             current = parent[static_cast<size_t>(current)]) {
            reversedPath.push_back(m_NavGrid.CellCenter(current));
            if (current == startCell) {
                reachedStart = true;
                break;
            }
        }
        if (!reachedStart || reversedPath.empty()) {
            return false;
        }
        m_NavPath.assign(reversedPath.rbegin(), reversedPath.rend());
        m_NavPathIndex = m_NavPath.size() > 1 ? 1 : 0;
        return true;
    }

    void InvalidateNavigationPath() {
        m_NavPath.clear();
        m_NavPathIndex = 0;
        m_NavTargetCell = -1;
        m_NavHasTarget = false;
        m_NavRepathTimer = 0.0f;
    }

    void EnterAIState(AIState next) {
        if (m_AIState == next) return;
        m_AIState = next;
        m_AIStateTime = 0.0f;
        m_AttackHitApplied = false;
        InvalidateNavigationPath();
        g_puppetAiState = AIStateName(next);
        g_puppetAnimState = AIStateName(next);
        printf("[PuppetEnemyScript] AI state -> %s\n", g_puppetAiState);

        switch (next) {
        case AIState::Attack:
            SetAnimation(attackClipIndex, false);
            break;
        case AIState::Patrol:
        case AIState::Chase:
        case AIState::Return:
            SetAnimation(moveClipIndex, true);
            break;
        }
    }

    void SetAnimation(int clipIndex, bool loop) {
        auto& coordinator = ECS::Coordinator::GetInstance();
        if (m_Entity == ECS::INVALID_ENTITY ||
            !coordinator.HasComponent<ECS::AnimatorComponent>(m_Entity)) {
            return;
        }
        auto& animator = coordinator.GetComponent<ECS::AnimatorComponent>(m_Entity);
        animator.clipIndex = clipIndex;
        animator.speed = std::max(0.1f, animSpeed);
        animator.loop = loop;
        animator.playing = true;
    }

    void StopMovement() {
        if (!g_PhysicsSystemPtr || m_Entity == ECS::INVALID_ENTITY) return;
        glm::vec3 velocity = g_PhysicsSystemPtr->GetLinearVelocity(m_Entity);
        velocity.x = 0.0f;
        velocity.z = 0.0f;
        g_PhysicsSystemPtr->SetLinearVelocity(m_Entity, velocity);
    }

    void RemoveDeathPhysics() {
        if (!g_PhysicsSystemPtr || m_Entity == ECS::INVALID_ENTITY) return;

        const JPH::BodyID bodyID = g_PhysicsSystemPtr->GetRigidBodyId(m_Entity);
        if (bodyID.IsInvalid()) return;

        // 死亡后保留实体和动画，但移除 Jolt 刚体。这样尸体不会继续受
        // 重力、碰撞冲量或 PhysicsSystem 的刚体到 Transform 同步影响。
        g_PhysicsSystemPtr->RemoveRigidBodyForEntity(m_Entity);
        printf("[PuppetEnemyScript] death physics removed entity %u\n",
               (unsigned)m_Entity);
    }

    void FaceTarget(const glm::vec3& target) {
        if (!g_PhysicsSystemPtr || m_Entity == ECS::INVALID_ENTITY) return;
        auto& scene = ECS::SceneECS::GetInstance();
        const glm::vec3 position = scene.GetWorldPosition(m_Entity);
        glm::vec3 direction = target - position;
        direction.y = 0.0f;
        if (glm::length(direction) < 1e-4f) return;
        direction = glm::normalize(direction);
        const float yaw = std::atan2(direction.x, direction.z);
        g_PhysicsSystemPtr->SetRigidBodyOrientation(m_Entity,
            glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f)));
    }

    void MoveToward(const glm::vec3& target, float speed) {
        if (!g_PhysicsSystemPtr || m_Entity == ECS::INVALID_ENTITY) return;
        auto& scene = ECS::SceneECS::GetInstance();
        const glm::vec3 position = scene.GetWorldPosition(m_Entity);
        glm::vec3 moveTarget = target;

        if (m_NavGrid.valid) {
            const int targetCell = m_NavGrid.WorldToCell(target);
            const bool targetChanged = targetCell != m_NavTargetCell;
            if (!m_NavHasTarget || targetChanged || m_NavRepathTimer <= 0.0f) {
                m_NavTargetCell = targetCell;
                m_NavHasTarget = true;
                m_NavRepathTimer = pathRepathInterval;
                if (!RebuildNavigationPath(position, target)) {
                    // 网格有效但目标不可达时停在原地，避免退回“穿墙直冲”。
                    m_NavPath.clear();
                    m_NavPathIndex = 0;
                }
            }

            while (m_NavPathIndex + 1 < m_NavPath.size() &&
                   HorizontalDistance(position, m_NavPath[m_NavPathIndex]) <
                       std::max(0.20f, m_NavGrid.cellSize * 0.45f)) {
                ++m_NavPathIndex;
            }

            if (!m_NavPath.empty()) {
                moveTarget = m_NavPath[std::min(m_NavPathIndex, m_NavPath.size() - 1)];
                if (m_NavPathIndex + 1 >= m_NavPath.size() &&
                    HorizontalDistance(position, target) <= m_NavGrid.cellSize * 1.5f) {
                    moveTarget = target;
                }
            } else {
                StopMovement();
                FaceTarget(target);
                SetAnimation(idleClipIndex, true);
                return;
            }
        }

        glm::vec3 direction = moveTarget - position;
        direction.y = 0.0f;
        const float distance = glm::length(direction);
        if (distance < 1e-4f) {
            StopMovement();
            return;
        }
        direction /= distance;

        glm::vec3 velocity = g_PhysicsSystemPtr->GetLinearVelocity(m_Entity);
        velocity.x = direction.x * std::max(0.0f, speed);
        velocity.z = direction.z * std::max(0.0f, speed);
        g_PhysicsSystemPtr->SetLinearVelocity(m_Entity, velocity);
        FaceTarget(moveTarget);
        SetAnimation(moveClipIndex, true);
    }

    void TickPatrol() {
        auto& scene = ECS::SceneECS::GetInstance();
        const glm::vec3 position = scene.GetWorldPosition(m_Entity);
        if (HorizontalDistance(position, m_PatrolTarget) <= 0.15f) {
            m_PatrolDirection *= -1.0f;
            m_PatrolTarget = m_PatrolOrigin + PatrolAxisVector() *
                             (patrolDistance * m_PatrolDirection);
        }
        MoveToward(m_PatrolTarget, patrolSpeed);
    }

    bool ApplyDamage(float amount) {
        if (m_Dead || amount <= 0.0f) return false;

        health = glm::clamp(health - amount, 0.0f, maxHealth);
        g_puppetHealth = health;
        g_puppetHitFlashTimer = 0.18f;
        ++g_puppetHitCount;

        if (health <= 0.0f) {
            m_Dead = true;
            m_DeathTimer = std::max(0.1f, deathDuration);
            g_puppetAiState = "Down";
            g_puppetAnimState = "Down";
            RemoveDeathPhysics();
            SetAnimation(deathClipIndex, false);
        } else {
            m_HitTimer = std::max(0.05f, hitDuration);
            m_AIState = AIState::Chase;
            m_AIStateTime = 0.0f;
            m_AttackHitApplied = false;
            m_AttackCooldownTimer = std::max(m_AttackCooldownTimer, m_HitTimer);
            g_puppetAiState = "Hit";
            g_puppetAnimState = "Hit";
            StopMovement();
            SetAnimation(hitClipIndex, false);
        }

        printf("[PuppetEnemyScript] hit entity %u damage=%.1f health=%.1f/%.1f ai=%s\n",
               (unsigned)m_Entity, amount, health, maxHealth, g_puppetAiState);
        return true;
    }

    static const ECS::FieldMeta s_Fields[];
    ECS::Entity m_Entity = ECS::INVALID_ENTITY;
    AIState m_AIState = AIState::Patrol;
    glm::vec3 m_PatrolOrigin = glm::vec3(0.0f);
    glm::vec3 m_PatrolTarget = glm::vec3(0.0f);
    float m_PatrolDirection = 1.0f;
    float m_HitTimer = 0.0f;
    float m_DeathTimer = 0.0f;
    float m_AIStateTime = 0.0f;
    float m_AttackCooldownTimer = 0.0f;
    bool m_AttackHitApplied = false;
    bool m_Dead = false;
    NavigationGrid m_NavGrid;
    std::vector<glm::vec3> m_NavPath;
    size_t m_NavPathIndex = 0;
    int m_NavTargetCell = -1;
    float m_NavRepathTimer = 0.0f;
    bool m_NavHasTarget = false;
};

const ECS::FieldMeta PuppetEnemyScript::s_Fields[] = {
    SCRIPT_FIELD(PuppetEnemyScript, maxHealth, Float, "最大生命值"),
    SCRIPT_FIELD(PuppetEnemyScript, health, Float, "当前生命值"),
    SCRIPT_FIELD(PuppetEnemyScript, idleClipIndex, Int, "待机动画 clip"),
    SCRIPT_FIELD(PuppetEnemyScript, hitClipIndex, Int, "受击动画 clip"),
    SCRIPT_FIELD(PuppetEnemyScript, deathClipIndex, Int, "死亡动画 clip"),
    SCRIPT_FIELD(PuppetEnemyScript, hitDuration, Float, "受击恢复时长"),
    SCRIPT_FIELD(PuppetEnemyScript, deathDuration, Float, "死亡动画时长"),
    SCRIPT_FIELD(PuppetEnemyScript, pathCellSize, Float, "寻路网格单元尺寸"),
    SCRIPT_FIELD(PuppetEnemyScript, pathRepathInterval, Float, "寻路重算间隔"),
    SCRIPT_FIELD(PuppetEnemyScript, pathObstaclePadding, Float, "寻路障碍额外间隙"),
    SCRIPT_FIELD(PuppetEnemyScript, animSpeed, Float, "动画速度"),
    SCRIPT_FIELD(PuppetEnemyScript, healthBarOffsetY, Float, "血条头顶锚点高度"),
    SCRIPT_FIELD(PuppetEnemyScript, moveClipIndex, Int, "移动动画 clip"),
    SCRIPT_FIELD(PuppetEnemyScript, attackClipIndex, Int, "反击动画 clip"),
    SCRIPT_FIELD(PuppetEnemyScript, patrolAxis, Int, "巡逻轴 0=X 1=Z"),
    SCRIPT_FIELD(PuppetEnemyScript, detectionRange, Float, "发现范围"),
    SCRIPT_FIELD(PuppetEnemyScript, disengageRange, Float, "脱离追击范围"),
    SCRIPT_FIELD(PuppetEnemyScript, patrolDistance, Float, "巡逻半径"),
    SCRIPT_FIELD(PuppetEnemyScript, patrolSpeed, Float, "巡逻速度"),
    SCRIPT_FIELD(PuppetEnemyScript, chaseSpeed, Float, "追击速度"),
    SCRIPT_FIELD(PuppetEnemyScript, attackRange, Float, "反击范围"),
    SCRIPT_FIELD(PuppetEnemyScript, attackCooldown, Float, "反击冷却"),
    SCRIPT_FIELD(PuppetEnemyScript, attackHitTime, Float, "反击命中时刻"),
    SCRIPT_FIELD(PuppetEnemyScript, attackDuration, Float, "反击动作时长"),
    SCRIPT_FIELD(PuppetEnemyScript, attackDamage, Float, "反击伤害"),
};
REGISTER_SCRIPT(PuppetEnemyScript, "PuppetEnemyScript");

// ===== Unity 式脚本：PhysicsDynamicPusher =====
// 动态刚体由 Jolt 负责位置积分，脚本只提供一个平滑的目标速度。
// 碰撞产生的冲量会先保留，因此玩家可以把方块推离原本的运动轨迹。
class PhysicsDynamicPusher : public ECS::IScriptBehaviour {
public:
    const char* GetScriptName() const override { return "PhysicsDynamicPusher"; }

    void OnStart(ECS::Entity entity) override {
        m_Entity = entity;
        m_Elapsed = 0.0f;
        auto& scene = ECS::SceneECS::GetInstance();
        m_Origin = scene.GetPosition(entity);
        m_OriginInitialized = true;
        printf("[PhysicsDynamicPusher] started on entity %u at (%.2f, %.2f, %.2f)\n",
               (unsigned)entity, m_Origin.x, m_Origin.y, m_Origin.z);
    }

    void OnUpdate(float deltaTime) override {
        if (!m_OriginInitialized || m_Entity == ECS::INVALID_ENTITY) return;

        if (!g_PhysicsSystemPtr) return;

        m_Elapsed += std::max(0.0f, deltaTime);
        const float distance = std::max(0.0f, travelDistance);
        const float frequency = std::max(0.0f, cyclesPerSecond);
        const float phaseRadians = phase * 6.28318530718f;
        const float angularSpeed = frequency * 6.28318530718f;
        const float targetVelocity =
            std::cos(m_Elapsed * angularSpeed + phaseRadians) * distance * angularSpeed;

        // 不再直接写 Transform，也不瞬间覆盖碰撞冲量。用可调响应速度逐渐
        // 追踪目标速度，保持动态刚体的质量、重力和接触响应有效。
        glm::vec3 velocity = g_PhysicsSystemPtr->GetLinearVelocity(m_Entity);
        const int movementAxis = (axis == 1 || axis == 2) ? axis : 0;
        const float response = 1.0f - std::exp(-std::max(0.0f, velocityResponse) *
            std::max(0.0f, deltaTime));
        velocity[movementAxis] = glm::mix(velocity[movementAxis], targetVelocity, response);
        g_PhysicsSystemPtr->SetLinearVelocity(m_Entity, velocity);
    }

    void OnDestroy() override {
        m_Entity = ECS::INVALID_ENTITY;
        m_OriginInitialized = false;
    }

    float travelDistance = 5.0f;  // 往返中心到端点的距离
    float cyclesPerSecond = 0.125f;
    float phase = 0.0f;            // 周期相位，0..1
    int axis = 0;                  // 0=X, 1=Y, 2=Z
    float velocityResponse = 6.0f; // 动态运动目标的跟随速度

private:
    static const ECS::FieldMeta s_Fields[];
    ECS::Entity m_Entity = ECS::INVALID_ENTITY;
    glm::vec3 m_Origin = glm::vec3(0.0f);
    float m_Elapsed = 0.0f;
    bool m_OriginInitialized = false;
};

const ECS::FieldMeta PhysicsDynamicPusher::s_Fields[] = {
    SCRIPT_FIELD(PhysicsDynamicPusher, travelDistance, Float, "往返距离"),
    SCRIPT_FIELD(PhysicsDynamicPusher, cyclesPerSecond, Float, "往返频率 Hz"),
    SCRIPT_FIELD(PhysicsDynamicPusher, phase, Float, "周期相位"),
    SCRIPT_FIELD(PhysicsDynamicPusher, axis, Int, "运动轴 0=X 1=Y 2=Z"),
    SCRIPT_FIELD(PhysicsDynamicPusher, velocityResponse, Float, "速度跟随响应"),
};
REGISTER_SCRIPT(PhysicsDynamicPusher, "PhysicsDynamicPusher");

// ===== Unity 式脚本：PlayerWalkScript =====
//（g_scriptAlive 已声明于文件顶部）
class PlayerWalkScript : public ECS::IScriptBehaviour {
public:
    const char* GetScriptName() const override { return "PlayerWalkScript"; }

    enum class AnimationState {
        Idle,
        Walk,
        Run,
        CrouchIdle,
        CrouchMove,
        JumpStart,
        Airborne,
        Land,
        Attack,
    };

    static const char* AnimationStateName(AnimationState state) {
        switch (state) {
        case AnimationState::Idle: return "Idle";
        case AnimationState::Walk: return "Walk";
        case AnimationState::Run: return "Run";
        case AnimationState::CrouchIdle: return "CrouchIdle";
        case AnimationState::CrouchMove: return "CrouchMove";
        case AnimationState::JumpStart: return "JumpStart";
        case AnimationState::Airborne: return "Airborne";
        case AnimationState::Land: return "Land";
        case AnimationState::Attack: return "Attack";
        default: return "Unknown";
        }
    }

    void EnterAnimationState(AnimationState next) {
        if (m_AnimationState == next) return;
        m_AnimationState = next;
        m_AnimationStateTime = 0.0f;
        g_playerAnimState = AnimationStateName(next);
        printf("[PlayerWalkScript] state -> %s\n", g_playerAnimState);
    }

    void OnStart(ECS::Entity entity) override {
        m_Entity = entity;
        auto& scene = ECS::SceneECS::GetInstance();
        auto& coordinator = ECS::Coordinator::GetInstance();
        auto& input = Input::InputSystem::GetInstance();
        m_CameraEntity = scene.FindByName("Main Camera");
        m_InfoText = scene.FindByName("InfoText");
        // 玩法接管场景相机：引擎不再用 WASD/鼠标驱动相机实体（WASD 全部归角色）
        SetSceneCameraControlLocked(true);
        g_scriptAlive = true;
        g_playerAnimState = AnimationStateName(AnimationState::Idle);
        m_AnimationState = AnimationState::Idle;
        m_AnimationStateTime = 0.0f;
        m_Airborne = false;
        m_AutoStateTime = 0.0f;
        maxHealth = std::max(1.0f, maxHealth);
        health = glm::clamp(health, 0.0f, maxHealth);
        g_playerDamagePending = 0.0f;
        g_playerMaxHealth = maxHealth;
        g_playerHealth = health;

        // 原型测试动作：Ctrl=奔跑、C=蹲伏、F=攻击。
        // 使用语义动作名，后续可由输入配置系统重新绑定。
        input.BindKey("Sprint", SDLK_LCTRL);
        input.BindKey("Sprint", SDLK_RCTRL);
        input.BindKey("Crouch", SDLK_C);
        input.BindKey("Attack", SDLK_F);

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

        // 生命值是脚本状态的唯一来源；HUD 只读取同步后的快照。
        maxHealth = std::max(1.0f, maxHealth);
        if (g_playerDamagePending > 0.0f) {
            const float damage = g_playerDamagePending;
            health = std::max(0.0f, health - damage);
            g_playerDamagePending = 0.0f;
            printf("[PlayerWalkScript] took damage=%.1f health=%.1f/%.1f\n",
                   damage, health, maxHealth);
        }
        health = glm::clamp(health, 0.0f, maxHealth);
        g_playerMaxHealth = maxHealth;
        g_playerHealth = health;

        if (m_Entity == ECS::INVALID_ENTITY ||
            !coordinator.HasComponent<ECS::TransformComponent>(m_Entity)) {
            m_MovementInputActive = false;
            return;
        }
        if (!g_PhysicsSystemPtr) {
            if (!m_WarnedBodyMissing) {
                m_WarnedBodyMissing = true;
                printf("[PlayerWalkScript] WARN: physics system unavailable - update skipped\n");
            }
            m_MovementInputActive = false;
            return;
        }

        // 1) 采样输入（默认绑定：MoveUp=W/↑, MoveDown=S/↓, MoveLeft=A/←, MoveRight=D/→, Jump=空格）
        float w = input.IsDown("MoveUp") ? 1.0f : 0.0f;
        float s = input.IsDown("MoveDown") ? 1.0f : 0.0f;
        float a = input.IsDown("MoveLeft") ? 1.0f : 0.0f;
        float d = input.IsDown("MoveRight") ? 1.0f : 0.0f;
        bool sprinting = input.IsDown("Sprint");
        bool crouching = input.IsDown("Crouch");
        bool attackPressed = input.IsPressed("Attack");
        bool autoJumpPressed = false;

        // headless 自测钩子：autoWalk>0 模拟按住 W。
        if (autoWalk > 0.5f && autoStateTest <= 0.5f) w = 1.0f;

        // headless 自测钩子：原地周期性攻击附近目标；默认关闭，不影响正常输入。
        if (autoAttack > 0.5f && autoStateTest <= 0.5f) {
            m_AutoStateTime += std::max(0.0f, deltaTime);
            const float attackPhase = std::fmod(m_AutoStateTime, 1.6f);
            w = s = a = d = 0.0f;
            sprinting = false;
            crouching = false;
            attackPressed = attackPhase < std::max(0.02f, deltaTime * 1.5f);
        }

        // 自动状态测试：Idle -> Run -> CrouchIdle -> CrouchMove -> Attack -> Jump。
        // 每段只驱动输入意图，仍然经过同一套状态机和物理/动画路径。
        if (autoStateTest > 0.5f) {
            m_AutoStateTime += std::max(0.0f, deltaTime);
            const float phase = std::fmod(m_AutoStateTime, 8.0f);
            w = s = a = d = 0.0f;
            sprinting = false;
            crouching = false;
            attackPressed = false;
            autoJumpPressed = false;
            if (phase >= 1.0f && phase < 3.0f) {
                w = 1.0f;
                sprinting = true;
            } else if (phase >= 3.0f && phase < 4.0f) {
                crouching = true;
            } else if (phase >= 4.0f && phase < 5.0f) {
                w = 1.0f;
                crouching = true;
            } else if (phase >= 5.0f && phase < 6.0f) {
                attackPressed = phase < 5.0f + std::max(0.02f, deltaTime * 1.5f);
            } else if (phase >= 6.0f && phase < 7.0f) {
                autoJumpPressed = phase < 6.0f + std::max(0.02f, deltaTime * 1.5f);
            }
        }
        m_AnimationStateTime += std::max(0.0f, deltaTime);

        // 2) 非第三人称相机的兼容跟随。
        //    thirdPersonEnabled 时由引擎 ThirdPersonCameraSystem 独占相机 Transform，
        //    否则这里每帧写固定偏移会覆盖鼠标环绕和滚轮缩放。
        bool useEngineThirdPersonCamera = false;
        if (m_CameraEntity != ECS::INVALID_ENTITY &&
            coordinator.HasComponent<ECS::CameraComponent>(m_CameraEntity)) {
            // 直接读取强类型 CameraComponent。旧的 GetComponentRaw(typeid(...))
            // 依赖跨 DLL RTTI 名称一致，失败时会错误地进入遗留固定跟随分支，
            // 把 ThirdPersonCameraSystem 刚写入的侧身相机覆盖掉。
            const auto& camera = coordinator.GetComponent<ECS::CameraComponent>(m_CameraEntity);
            useEngineThirdPersonCamera = camera.thirdPersonEnabled;
        }
        if (!useEngineThirdPersonCamera &&
            m_CameraEntity != ECS::INVALID_ENTITY &&
            coordinator.HasComponent<ECS::TransformComponent>(m_CameraEntity) &&
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

        // 3) 相机相对移动：W/S=相机水平前/后，A/D=相机局部左/右。
        //    方向基于相机当前 Transform，而不是角色朝向或世界坐标轴。
        glm::vec3 camFwd(0.0f, 0.0f, 1.0f);
        glm::vec3 camRight(-1.0f, 0.0f, 0.0f);
        BuildCameraRelativeBasis(scene, coordinator, m_CameraEntity, m_Entity,
                                 camFwd, camRight);
        const float forwardInput = w - s;
        const float rightInput = d - a;
        glm::vec3 moveDir = camFwd * forwardInput + camRight * rightInput;
        bool moving = glm::dot(moveDir, moveDir) > 1e-4f;
        if (moving) moveDir = glm::normalize(moveDir);

        // 4) 物理控制：保留垂直分量（重力），水平分量 = 输入方向 × 速度
        JPH::BodyID bodyID = g_PhysicsSystemPtr->GetRigidBodyId(m_Entity);
        if (bodyID.IsInvalid()) {
            if (!m_WarnedBodyMissing) {
                m_WarnedBodyMissing = true;
                printf("[PlayerWalkScript] WARN: no rigid body for entity %u - physics control disabled\n",
                       (unsigned)m_Entity);
            }
            m_MovementInputActive = false;
            return;
        }
        // 读取上一物理帧的速度。它用于区分“真的在移动”和“输入顶着墙”。
        glm::vec3 vel = g_PhysicsSystemPtr->GetLinearVelocity(m_Entity);
        const glm::vec2 previousHorizontalVelocity(vel.x, vel.z);
        const bool wasActuallyMoving = glm::dot(
            previousHorizontalVelocity, previousHorizontalVelocity) > 0.0025f;

        // 攻击期间锁住水平移动，便于验证攻击动作和后续接入攻击判定。
        if (attackPressed && !m_Airborne && m_AnimationState != AnimationState::Attack) {
            EnterAnimationState(AnimationState::Attack);
            m_AttackHitApplied = false;
        }
        const bool attackActive = m_AnimationState == AnimationState::Attack &&
            m_AnimationStateTime < std::max(0.1f, attackDuration);
        if (attackActive) {
            moveDir = glm::vec3(0.0f);
            moving = false;

            // 在挥拳中段触发一次近战判定，让动画反馈和伤害时机一致。
            if (!m_AttackHitApplied &&
                m_AnimationStateTime >= std::max(0.0f, attackHitTime)) {
                m_AttackHitApplied = true;
                const glm::vec3 attackForward(
                    std::sin(m_CurrentYaw), 0.0f, std::cos(m_CurrentYaw));
                PuppetEnemyScript::TryHit(m_Entity, attackForward,
                                          attackRange, attackDamage);
            }
        }

        // 跳跃：仅在地面附近（垂直速度接近 0）时允许。
        const bool jumpPressed = input.IsPressed("Jump") || autoJumpPressed;
        if (jumpPressed && !attackActive && !m_Airborne && std::abs(vel.y) < 0.8f) {
            vel.y = jumpSpeed;
            m_Airborne = true;
            EnterAnimationState(AnimationState::JumpStart);
        }
        const float desiredMoveSpeed = crouching
            ? moveSpeed * std::max(0.1f, crouchSpeedMultiplier)
            : (sprinting ? moveSpeed * std::max(1.0f, sprintSpeedMultiplier) : moveSpeed);
        vel.x = moveDir.x * desiredMoveSpeed;
        vel.z = moveDir.z * desiredMoveSpeed;
        g_PhysicsSystemPtr->SetLinearVelocity(m_Entity, vel);

        // 5) 平滑转向：朝向以最大角速度向移动方向过渡（最短路径，不瞬转）。
        // 朝向由脚本的 m_CurrentYaw 单独维护，每帧只向物理刚体写一次；
        // 不再先读取物理回写角度、再写目标角度，避免两个来源互相争抢。
        // 连续顶住障碍物时，上一帧水平速度为 0，因此不继续原地转圈；
        // 从静止开始输入时允许首帧转向，避免出现明显的输入延迟。
        const bool movementInputStarted = moving && !m_MovementInputActive;
        const bool canTurn = moving && (wasActuallyMoving || movementInputStarted);
        if (canTurn) {
            const float targetYaw = std::atan2(moveDir.x, moveDir.z) + glm::radians(yawOffsetDeg);
            if (!m_YawInitialized) { m_CurrentYaw = targetYaw; m_YawInitialized = true; }
            float delta = targetYaw - m_CurrentYaw;
            delta = std::atan2(std::sin(delta), std::cos(delta)); // 归一化到 [-π,π]
            const float maxDelta = glm::radians(std::max(0.0f, turnSpeedDeg)) *
                std::max(0.0f, deltaTime);
            delta = glm::clamp(delta, -maxDelta, maxDelta);
            m_CurrentYaw += delta;
        }
        if (!m_YawInitialized) {
            m_CurrentYaw = 0.0f;
            m_YawInitialized = true;
        }
        // 把连续角度收回 [-π,π]，避免长时间运行后浮点精度累积。
        m_CurrentYaw = std::atan2(std::sin(m_CurrentYaw), std::cos(m_CurrentYaw));
        g_PhysicsSystemPtr->SetRigidBodyOrientation(m_Entity,
            glm::angleAxis(m_CurrentYaw, glm::vec3(0.0f, 1.0f, 0.0f)));
        m_MovementInputActive = moving;

        // 6) 动画状态机：状态优先级为 Attack > Jump/Airborne/Land > Grounded。
        //    clip 切换由引擎自动 PlayAnimation，同 clip 内速度用指数平滑。
        const auto groundedState = [&]() {
            if (crouching) {
                return moving ? AnimationState::CrouchMove : AnimationState::CrouchIdle;
            }
            if (!moving) return AnimationState::Idle;
            return sprinting ? AnimationState::Run : AnimationState::Walk;
        };

        if (m_AnimationState == AnimationState::Attack) {
            if (!m_Airborne && m_AnimationStateTime >= std::max(0.1f, attackDuration)) {
                EnterAnimationState(groundedState());
            }
        } else if (m_Airborne) {
            if (m_AnimationState == AnimationState::JumpStart &&
                m_AnimationStateTime >= std::max(0.1f, jumpStartDuration)) {
                EnterAnimationState(AnimationState::Airborne);
            } else if (m_AnimationState == AnimationState::Airborne &&
                       m_AnimationStateTime >= 0.2f && std::abs(vel.y) < 0.8f) {
                m_Airborne = false;
                EnterAnimationState(AnimationState::Land);
            }
        } else if (m_AnimationState == AnimationState::Land) {
            if (m_AnimationStateTime >= std::max(0.1f, landDuration)) {
                EnterAnimationState(groundedState());
            }
        } else {
            EnterAnimationState(groundedState());
        }

        // disableAnim>0 时跳过动画驱动（调试：隔离模型/渲染问题）。
        if (!disableAnim && coordinator.HasComponent<ECS::AnimatorComponent>(m_Entity)) {
            auto& anim = coordinator.GetComponent<ECS::AnimatorComponent>(m_Entity);
            const float horiz = glm::length(glm::vec2(vel.x, vel.z));
            int targetClip = idleClipIndex;
            float targetSpeed = 1.0f;
            bool targetLoop = true;
            switch (m_AnimationState) {
            case AnimationState::Idle:
                targetClip = idleClipIndex;
                break;
            case AnimationState::Walk:
                targetClip = walkClipIndex;
                targetSpeed = glm::clamp(horiz / std::max(0.1f, moveSpeed), 0.2f, 1.0f) * walkAnimSpeed;
                break;
            case AnimationState::Run:
                targetClip = runClipIndex;
                targetSpeed = 1.0f;
                break;
            case AnimationState::CrouchIdle:
                targetClip = crouchIdleClipIndex;
                targetSpeed = 0.8f;
                break;
            case AnimationState::CrouchMove:
                targetClip = crouchMoveClipIndex;
                targetSpeed = 0.8f;
                break;
            case AnimationState::JumpStart:
                targetClip = jumpStartClipIndex;
                targetLoop = false;
                break;
            case AnimationState::Airborne:
                targetClip = jumpLoopClipIndex;
                break;
            case AnimationState::Land:
                targetClip = jumpLandClipIndex;
                targetLoop = false;
                break;
            case AnimationState::Attack:
                targetClip = attackClipIndex;
                targetLoop = false;
                break;
            }
            anim.clipIndex = targetClip; // 引擎检测变化自动 PlayAnimation（新 clip 从头播放）
            anim.loop = targetLoop;
            anim.playing = true;
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
        g_playerDamagePending = 0.0f;
        if (m_Entity != ECS::INVALID_ENTITY) {
            SetSceneCameraControlLocked(false);
        }
    }

    // 参数字段表（Unity 式 inspector 字段）：场景 JSON params.* 注入
    const ECS::FieldMeta* GetParamFields(int& outCount) const override {
        outCount = 33;
        return s_Fields;
    }

    float moveSpeed = 5.0f;        // 行走速度 m/s
    float jumpSpeed = 7.0f;        // 跳跃初速度 m/s
    float camDistance = 6.5f;      // 镜头与角色水平距离
    float camHeight = 3.0f;        // 镜头相对角色脚底高度
    float camLerpSpeed = 8.0f;     // 镜头平滑速度（越大越跟手）
    float maxHealth = 100.0f;      // 最大生命值
    float health = 100.0f;         // 当前生命值（原型可由场景参数/玩法逻辑修改）
    float yawOffsetDeg = 0.0f;     // 模型正面朝向修正
    float walkAnimSpeed = 1.5f;    // 行走动画倍速
    float animBlendSpeed = 6.0f;   // 动画速度平滑过渡系数（越大过渡越快）
    int idleClipIndex = 9;         // UAL1: Idle_Loop
    int walkClipIndex = 45;        // UAL1: Walk_Loop
    int runClipIndex = 38;         // UAL1: Sprint_Loop
    float runSpeedRatio = 0.6f;    // 兼容旧场景字段（新状态机由 Ctrl 触发）
    float sprintSpeedMultiplier = 1.6f; // Ctrl 奔跑速度倍率
    float crouchSpeedMultiplier = 0.5f; // C 蹲伏移动速度倍率
    int crouchIdleClipIndex = 2;   // UAL1: Crouch_Idle_Loop
    int crouchMoveClipIndex = 1;   // UAL1: Crouch_Fwd_Loop
    int attackClipIndex = 26;      // UAL1: Punch_Jab
    int jumpStartClipIndex = 16;   // UAL1: Jump_Start
    int jumpLoopClipIndex = 15;    // UAL1: Jump_Loop
    int jumpLandClipIndex = 14;    // UAL1: Jump_Land
    float jumpStartDuration = 0.35f;
    float attackDuration = 0.87f;
    float attackHitTime = 0.28f;
    float attackRange = 3.0f;
    float attackDamage = 25.0f;
    float landDuration = 0.45f;
    float turnSpeedDeg = 360.0f;   // 转向速率（°/s），越小转向越柔和
    int disableAnim = 0;           // 调试：>0 跳过动画驱动（隔离 VSync 切换模型消失）
    float autoWalk = 0.0f;         // headless 自测：>0 模拟按住 W
    float autoStateTest = 0.0f;     // headless 自测：>0 循环测试跑/蹲/攻/跳
    float autoAttack = 0.0f;        // headless 自测：>0 原地周期性攻击

private:
    static const ECS::FieldMeta s_Fields[];
    ECS::Entity m_Entity = ECS::INVALID_ENTITY;
    ECS::Entity m_CameraEntity = ECS::INVALID_ENTITY;
    ECS::Entity m_InfoText = ECS::INVALID_ENTITY;
    float m_CurrentYaw = 0.0f;
    bool m_YawInitialized = false;   // 朝向基准是否已初始化
    bool m_MovementInputActive = false; // 上一帧是否存在移动输入
    AnimationState m_AnimationState = AnimationState::Idle;
    float m_AnimationStateTime = 0.0f;
    bool m_Airborne = false;
    bool m_AttackHitApplied = false;
    float m_AutoStateTime = 0.0f;
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
    SCRIPT_FIELD(PlayerWalkScript, maxHealth, Float, "最大生命值"),
    SCRIPT_FIELD(PlayerWalkScript, health, Float, "当前生命值"),
    SCRIPT_FIELD(PlayerWalkScript, yawOffsetDeg, Float, "模型朝向修正(°)"),
    SCRIPT_FIELD(PlayerWalkScript, walkAnimSpeed, Float, "满速行走动画倍速"),
    SCRIPT_FIELD(PlayerWalkScript, animBlendSpeed, Float, "动画速度平滑过渡"),
    SCRIPT_FIELD(PlayerWalkScript, idleClipIndex, Int, "待机动画 clip"),
    SCRIPT_FIELD(PlayerWalkScript, walkClipIndex, Int, "行走动画 clip"),
    SCRIPT_FIELD(PlayerWalkScript, runClipIndex, Int, "奔跑动画 clip"),
    SCRIPT_FIELD(PlayerWalkScript, runSpeedRatio, Float, "奔跑速度阈值"),
    SCRIPT_FIELD(PlayerWalkScript, sprintSpeedMultiplier, Float, "Ctrl奔跑速度倍率"),
    SCRIPT_FIELD(PlayerWalkScript, crouchSpeedMultiplier, Float, "蹲伏移动速度倍率"),
    SCRIPT_FIELD(PlayerWalkScript, crouchIdleClipIndex, Int, "蹲伏待机动画 clip"),
    SCRIPT_FIELD(PlayerWalkScript, crouchMoveClipIndex, Int, "蹲伏移动动画 clip"),
    SCRIPT_FIELD(PlayerWalkScript, attackClipIndex, Int, "攻击动画 clip"),
    SCRIPT_FIELD(PlayerWalkScript, jumpStartClipIndex, Int, "起跳动画 clip"),
    SCRIPT_FIELD(PlayerWalkScript, jumpLoopClipIndex, Int, "空中动画 clip"),
    SCRIPT_FIELD(PlayerWalkScript, jumpLandClipIndex, Int, "落地动画 clip"),
    SCRIPT_FIELD(PlayerWalkScript, jumpStartDuration, Float, "起跳状态时长"),
    SCRIPT_FIELD(PlayerWalkScript, attackDuration, Float, "攻击状态时长"),
    SCRIPT_FIELD(PlayerWalkScript, attackHitTime, Float, "攻击命中时刻"),
    SCRIPT_FIELD(PlayerWalkScript, attackRange, Float, "攻击距离"),
    SCRIPT_FIELD(PlayerWalkScript, attackDamage, Float, "攻击伤害"),
    SCRIPT_FIELD(PlayerWalkScript, landDuration, Float, "落地状态时长"),
    SCRIPT_FIELD(PlayerWalkScript, turnSpeedDeg, Float, "转向速率(°/s)"),
    SCRIPT_FIELD(PlayerWalkScript, disableAnim, Int, "禁用动画(调试)"),
    SCRIPT_FIELD(PlayerWalkScript, autoWalk, Float, "headless 自测:模拟按 W"),
    SCRIPT_FIELD(PlayerWalkScript, autoStateTest, Float, "headless 自测:循环动画状态"),
    SCRIPT_FIELD(PlayerWalkScript, autoAttack, Float, "headless 自测:原地攻击"),
};
REGISTER_SCRIPT(PlayerWalkScript, "PlayerWalkScript");

// ===== 插件导出(引擎 GameManager::LoadPlugin 约定) =====
extern "C" __declspec(dllexport) const char* GetGameModuleName() {
    return "cesiumwalk"; // 与 assets/cesiumwalk.json 顶层 "game" 键一致
}

extern "C" __declspec(dllexport) Game::IGameModule* CreateGameModule() {
    return &Game::CesiumWalk::GetInstance();
}
