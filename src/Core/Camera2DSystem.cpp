// Camera2DSystem.cpp - Cinemachine 风格 2D 智能相机系统(序5)
#include "Core/Camera2DSystem.h"
#include "ECS/SceneECS.h"
#include "ECS/Coordinator.h"
#include "ECS/Components.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <sstream>

namespace {
constexpr float kMinDt = 1e-4f; // 速度估计除零保护

// 伪噪声(叠加两个不同频率/相位的 sin, 比纯随机平滑)
float Noise(float t) {
    return 0.65f * std::sin(t) + 0.35f * std::sin(t * 2.37f + 1.3f);
}
} // namespace

Camera2DSystem& Camera2DSystem::GetInstance() {
    static Camera2DSystem instance;
    return instance;
}

void Camera2DSystem::SetViewport(uint32_t width, uint32_t height) {
    m_ViewportWidth = width > 0 ? width : m_ViewportWidth;
    m_ViewportHeight = height > 0 ? height : m_ViewportHeight;
}

ECS::Entity Camera2DSystem::GetActiveCamera() {
    auto& coordinator = ECS::Coordinator::GetInstance();
    if (m_ActiveCamera != ECS::INVALID_ENTITY &&
        coordinator.HasComponent<ECS::Camera2DComponent>(m_ActiveCamera) &&
        coordinator.GetComponent<ECS::Camera2DComponent>(m_ActiveCamera).enabled) {
        return m_ActiveCamera;
    }
    return FindFirstEnabledCamera();
}

void Camera2DSystem::SetSceneContext(const std::string& scenePath) {
    m_SceneContext = scenePath.empty() ? "<unknown>" : scenePath;
    m_MissingTargetDiagnostics.clear();
}

void Camera2DSystem::ResetCameraRuntime(ECS::Camera2DComponent& c) {
    c.followTarget = ECS::INVALID_ENTITY;
    c.currentCenter = c.center;
    c.prevTargetPos = glm::vec2(0.0f);
    c.initialized = false;
    c.shakeStrength = 0.0f;
    c.shakeTotalDuration = 0.0f;
    c.shakeDuration = 0.0f;
    c.shakeElapsed = 0.0f;
}

void Camera2DSystem::Reset() {
    auto& coordinator = ECS::Coordinator::GetInstance();
    auto& sceneECS = ECS::SceneECS::GetInstance();
    std::function<void(ECS::Entity)> visit = [&](ECS::Entity e) {
        if (coordinator.HasComponent<ECS::Camera2DComponent>(e)) {
            ResetCameraRuntime(coordinator.GetComponent<ECS::Camera2DComponent>(e));
        }
        for (const auto& child : sceneECS.GetChildren(e)) visit(child);
    };
    for (const auto& root : sceneECS.GetRootEntities()) visit(root);
    m_ActiveCamera = ECS::INVALID_ENTITY;
    m_MissingTargetDiagnostics.clear();
}

void Camera2DSystem::Rebind() {
    auto& coordinator = ECS::Coordinator::GetInstance();
    auto& sceneECS = ECS::SceneECS::GetInstance();
    std::function<void(ECS::Entity)> visit = [&](ECS::Entity e) {
        if (coordinator.HasComponent<ECS::Camera2DComponent>(e)) {
            auto& c = coordinator.GetComponent<ECS::Camera2DComponent>(e);
            c.followTarget = ECS::INVALID_ENTITY;
            if (c.enabled) ResolveTarget(e, c);
        }
        for (const auto& child : sceneECS.GetChildren(e)) visit(child);
    };
    for (const auto& root : sceneECS.GetRootEntities()) visit(root);
}

void Camera2DSystem::Update(float dt) {
    const ECS::Entity active = FindFirstEnabledCamera();
    if (active != m_ActiveCamera) {
        m_ActiveCamera = active;
        if (m_ActiveCamera != ECS::INVALID_ENTITY) {
            auto& c = ECS::Coordinator::GetInstance().GetComponent<ECS::Camera2DComponent>(m_ActiveCamera);
            ResetCameraRuntime(c);
            ResolveTarget(m_ActiveCamera, c);
        }
    }
    if (m_ActiveCamera != ECS::INVALID_ENTITY) {
        auto& coordinator = ECS::Coordinator::GetInstance();
        if (coordinator.HasComponent<ECS::Camera2DComponent>(m_ActiveCamera)) {
            auto& c = coordinator.GetComponent<ECS::Camera2DComponent>(m_ActiveCamera);
            if (c.enabled) UpdateCamera(m_ActiveCamera, c, dt);
        }
    }
}

void Camera2DSystem::VisitEntity(ECS::Entity e, float dt) {
    auto& coordinator = ECS::Coordinator::GetInstance();
    if (coordinator.HasComponent<ECS::Camera2DComponent>(e)) {
        auto& c = coordinator.GetComponent<ECS::Camera2DComponent>(e);
        if (c.enabled) UpdateCamera(e, c, dt);
    }
    auto& sceneECS = ECS::SceneECS::GetInstance();
    for (const auto& child : sceneECS.GetChildren(e)) {
        VisitEntity(child, dt);
    }
}

void Camera2DSystem::ResolveTarget(ECS::Entity e, ECS::Camera2DComponent& c) {
    if (c.followTargetName.empty()) {
        c.followTarget = ECS::INVALID_ENTITY;
        return;
    }
    // 已解析且有效则复用; 无效(目标未加载/改名)时每帧重试解析
    if (c.followTarget != ECS::INVALID_ENTITY &&
        ECS::Coordinator::GetInstance().HasComponent<ECS::TransformComponent>(c.followTarget) &&
        ECS::SceneECS::GetInstance().GetName(c.followTarget) == c.followTargetName) {
        return;
    }
    c.followTarget = ECS::INVALID_ENTITY;
    c.followTarget = ECS::SceneECS::GetInstance().FindByName(c.followTargetName);
    if (c.followTarget == ECS::INVALID_ENTITY) {
        DiagnoseMissingTarget(e, c);
    }
}

void Camera2DSystem::DiagnoseMissingTarget(ECS::Entity e, const ECS::Camera2DComponent& c) {
    std::ostringstream key;
    key << e << "|" << c.followTargetName << "|" << m_SceneContext;
    if (!m_MissingTargetDiagnostics.insert(key.str()).second) return;

    std::cerr << "[Camera2D] missing follow target {camera_entity=" << e
              << ", target=\"" << c.followTargetName
              << "\", scene=\"" << m_SceneContext << "\"}" << std::endl;
}

void Camera2DSystem::UpdateCamera(ECS::Entity e, ECS::Camera2DComponent& c, float dt) {
    auto& coordinator = ECS::Coordinator::GetInstance();
    const float safeDt = std::max(dt, 0.0f);
    c.zoom = std::max(c.zoom, 0.01f);

    ResolveTarget(e, c);
    if (c.followTarget != ECS::INVALID_ENTITY &&
        coordinator.HasComponent<ECS::TransformComponent>(c.followTarget)) {
        const auto& t = coordinator.GetComponent<ECS::TransformComponent>(c.followTarget);
        const glm::vec2 targetPos(t.position.x, t.position.y);

        // 目标速度(帧间差分)
        glm::vec2 vel(0.0f);
        if (c.initialized && safeDt > kMinDt) {
            vel = (targetPos - c.prevTargetPos) / safeDt;
        }
        c.prevTargetPos = targetPos;

        // 期望位置 = 目标位置 + 常驻偏移 + 速度前视
        const glm::vec2 desired = targetPos + c.followOffset + c.lookahead * vel;

        // 帧率无关指数阻尼
        if (!c.initialized) {
            c.currentCenter = desired; // 首帧直接定住, 避免启动漂移
            c.initialized = true;
        } else {
            const float kx = 1.0f - std::exp(-std::clamp(c.dampX, 0.0f, 1000.0f) * safeDt);
            const float ky = 1.0f - std::exp(-std::clamp(c.dampY, 0.0f, 1000.0f) * safeDt);
            c.currentCenter.x += (desired.x - c.currentCenter.x) * kx;
            c.currentCenter.y += (desired.y - c.currentCenter.y) * ky;
        }
    }

    // confiner: 相机中心限制在 [boundMin + 半视口, boundMax - 半视口], 视野不越界
    if (c.useBounds) {
        const float halfW = m_ViewportWidth * 0.5f / std::max(c.zoom, 0.01f);
        const float halfH = m_ViewportHeight * 0.5f / std::max(c.zoom, 0.01f);
        const float regionMinX = std::min(c.boundMin.x, c.boundMax.x);
        const float regionMaxX = std::max(c.boundMin.x, c.boundMax.x);
        const float regionMinY = std::min(c.boundMin.y, c.boundMax.y);
        const float regionMaxY = std::max(c.boundMin.y, c.boundMax.y);
        const float minX = regionMinX + halfW, maxX = regionMaxX - halfW;
        const float minY = regionMinY + halfH, maxY = regionMaxY - halfH;
        c.currentCenter.x = maxX >= minX ? std::clamp(c.currentCenter.x, minX, maxX)
                                         : (regionMinX + regionMaxX) * 0.5f;
        c.currentCenter.y = maxY >= minY ? std::clamp(c.currentCenter.y, minY, maxY)
                                         : (regionMinY + regionMaxY) * 0.5f;
    }

    // 屏幕震动(伪噪声 × 剩余时长比例衰减; 叠加在最终中心之上, 可短暂越出 confiner)
    if (c.shakeDuration > 0.0f && c.shakeStrength > 0.0f) {
        c.shakeElapsed += safeDt;
        c.shakeDuration -= safeDt;
        const float env = c.shakeStrength *
                          (c.shakeTotalDuration > 0.0f ? std::max(0.0f, c.shakeDuration / c.shakeTotalDuration) : 0.0f);
        c.currentCenter.x += Noise(c.shakeElapsed * c.shakeFrequency) * env;
        c.currentCenter.y += Noise(c.shakeElapsed * c.shakeFrequency * 1.31f + 2.2f) * env;
        if (c.shakeDuration <= 0.0f) {
            c.shakeStrength = 0.0f; // 结束归零
        }
    }

    // 写回: Canvas2D::SyncCameraFromScene 读取 center
    c.center = c.currentCenter;
}

ECS::Entity Camera2DSystem::FindFirstEnabledCamera() {
    auto& coordinator = ECS::Coordinator::GetInstance();
    auto& sceneECS = ECS::SceneECS::GetInstance();
    ECS::Entity found = ECS::INVALID_ENTITY;
    std::function<void(ECS::Entity)> visit = [&](ECS::Entity e) {
        if (found != ECS::INVALID_ENTITY) return;
        if (coordinator.HasComponent<ECS::Camera2DComponent>(e)) {
            if (coordinator.GetComponent<ECS::Camera2DComponent>(e).enabled) found = e;
        }
        if (found != ECS::INVALID_ENTITY) return;
        for (const auto& child : sceneECS.GetChildren(e)) visit(child);
    };
    for (const auto& root : sceneECS.GetRootEntities()) visit(root);
    return found;
}

void Camera2DSystem::ShakeCamera(ECS::Entity e, float strength, float duration) {
    if (e == ECS::INVALID_ENTITY) e = FindFirstEnabledCamera();
    if (e == ECS::INVALID_ENTITY ||
        !ECS::Coordinator::GetInstance().HasComponent<ECS::Camera2DComponent>(e)) {
        return;
    }
    auto& c = ECS::Coordinator::GetInstance().GetComponent<ECS::Camera2DComponent>(e);
    c.shakeStrength = std::max(0.0f, strength);
    c.shakeTotalDuration = std::max(duration, 0.001f);
    c.shakeDuration = c.shakeTotalDuration;
    c.shakeElapsed = 0.0f;
}

void Camera2DSystem::ShakeCamera(float strength, float duration) {
    ShakeCamera(ECS::INVALID_ENTITY, strength, duration);
}
