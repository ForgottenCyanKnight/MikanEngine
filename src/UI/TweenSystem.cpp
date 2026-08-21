// TweenSystem.cpp - UI 补间动画系统实现
#include "UI/TweenSystem.h"
#include "ECS/SceneECS.h"
#include <cmath>
#include <algorithm>

namespace UI {

TweenSystem& TweenSystem::GetInstance() {
    static TweenSystem instance;
    return instance;
}

float TweenSystem::ApplyEasing(float t, ECS::TweenComponent::Easing easing) {
    // t 钳制到 [0,1]
    t = std::max(0.0f, std::min(1.0f, t));
    switch (easing) {
    case ECS::TweenComponent::Easing::Linear:
        return t;
    case ECS::TweenComponent::Easing::EaseInQuad:
        return t * t;
    case ECS::TweenComponent::Easing::EaseOutQuad:
        return t * (2.0f - t);
    case ECS::TweenComponent::Easing::EaseInOutQuad:
        return t < 0.5f ? 2.0f * t * t : -1.0f + (4.0f - 2.0f * t) * t;
    case ECS::TweenComponent::Easing::EaseInCubic:
        return t * t * t;
    case ECS::TweenComponent::Easing::EaseOutCubic: {
        float t1 = t - 1.0f;
        return t1 * t1 * t1 + 1.0f;
    }
    case ECS::TweenComponent::Easing::EaseInOutCubic:
        return t < 0.5f ? 4.0f * t * t * t : (t - 1.0f) * (2.0f * t - 2.0f) * (2.0f * t - 2.0f) + 1.0f;
    case ECS::TweenComponent::Easing::EaseOutBounce: {
        const float n1 = 7.5625f;
        const float d1 = 2.75f;
        if (t < 1.0f / d1)       return n1 * t * t;
        else if (t < 2.0f / d1) { t -= 1.5f / d1; return n1 * t * t + 0.75f; }
        else if (t < 2.5f / d1) { t -= 2.25f / d1; return n1 * t * t + 0.9375f; }
        else                    { t -= 2.625f / d1; return n1 * t * t + 0.984375f; }
    }
    case ECS::TweenComponent::Easing::EaseOutElastic: {
        if (t == 0.0f || t == 1.0f) return t;
        const float c4 = (2.0f * 3.1415926535f) / 3.0f;
        return std::pow(2.0f, -10.0f * t) * std::sin((t * 10.0f - 0.75f) * c4) + 1.0f;
    }
    }
    return t;
}

void TweenSystem::ApplyProperty(ECS::Entity entity, ECS::TweenComponent::Property property, float value) {
    auto& coordinator = ECS::Coordinator::GetInstance();
    switch (property) {
    case ECS::TweenComponent::Property::PositionX:
        if (coordinator.HasComponent<ECS::TransformComponent>(entity)) {
            auto& t = coordinator.GetComponent<ECS::TransformComponent>(entity);
            t.position.x = value;
            t.MarkDirty(); // Tween 写 position,世界矩阵缓存需失效
        }
        break;
    case ECS::TweenComponent::Property::PositionY:
        if (coordinator.HasComponent<ECS::TransformComponent>(entity)) {
            auto& t = coordinator.GetComponent<ECS::TransformComponent>(entity);
            t.position.y = value;
            t.MarkDirty();
        }
        break;
    case ECS::TweenComponent::Property::ScaleX:
        if (coordinator.HasComponent<ECS::TransformComponent>(entity)) {
            auto& t = coordinator.GetComponent<ECS::TransformComponent>(entity);
            t.scale.x = value;
            t.MarkDirty(); // Tween 写 scale,世界矩阵缓存需失效
        }
        break;
    case ECS::TweenComponent::Property::ScaleY:
        if (coordinator.HasComponent<ECS::TransformComponent>(entity)) {
            auto& t = coordinator.GetComponent<ECS::TransformComponent>(entity);
            t.scale.y = value;
            t.MarkDirty();
        }
        break;
    case ECS::TweenComponent::Property::RotationZ: {
        if (coordinator.HasComponent<ECS::TransformComponent>(entity)) {
            auto& t = coordinator.GetComponent<ECS::TransformComponent>(entity);
            glm::vec3 euler = t.GetEulerAngles();
            euler.z = value;
            t.SetEulerAngles(euler);
        }
        break;
    }
    case ECS::TweenComponent::Property::ColorAlpha:
        if (coordinator.HasComponent<ECS::Sprite2DComponent>(entity)) {
            auto& s2d = coordinator.GetComponent<ECS::Sprite2DComponent>(entity);
            s2d.color.a = std::max(0.0f, std::min(1.0f, value));
        }
        break;
    case ECS::TweenComponent::Property::Width:
        if (coordinator.HasComponent<ECS::Sprite2DComponent>(entity)) {
            auto& s2d = coordinator.GetComponent<ECS::Sprite2DComponent>(entity);
            s2d.width = std::max(0.0f, value);
        }
        break;
    case ECS::TweenComponent::Property::Height:
        if (coordinator.HasComponent<ECS::Sprite2DComponent>(entity)) {
            auto& s2d = coordinator.GetComponent<ECS::Sprite2DComponent>(entity);
            s2d.height = std::max(0.0f, value);
        }
        break;
    }
}

void TweenSystem::Update(float deltaTime) {
    auto& coordinator = ECS::Coordinator::GetInstance();
    const float origDt = deltaTime;  // 保存原始值，避免延迟处理时互相污染
    // 收集所有带 TweenComponent 的实体
    std::vector<ECS::Entity> entities;
    for (ECS::Entity e = 0; e < ECS::MAX_ENTITIES; ++e) {
        if (coordinator.HasComponent<ECS::TweenComponent>(e)) {
            entities.push_back(e);
        }
    }

    for (auto entity : entities) {
        auto& tw = coordinator.GetComponent<ECS::TweenComponent>(entity);
        if (!tw.running) continue;

        // 延迟
        if (tw.delay > 0.0f) {
            tw.delay -= origDt;
            if (tw.delay > 0.0f) continue;
        }

        tw.elapsed += origDt;

        float t = tw.duration > 0.0f ? tw.elapsed / tw.duration : 1.0f;
        bool finished = (t >= 1.0f);
        t = std::min(t, 1.0f);

        // ping-pong 反向阶段：翻转 t
        float easedT = ApplyEasing(tw.pingPongReverse ? (1.0f - t) : t, tw.easing);
        float value = tw.from + (tw.to - tw.from) * easedT;
        ApplyProperty(entity, tw.property, value);

        if (finished) {
            if (tw.pingPong) {
                // 反向
                tw.pingPongReverse = !tw.pingPongReverse;
                tw.elapsed = 0.0f;
                // ping-pong 完成后不停止
            } else if (tw.loop) {
                tw.elapsed = 0.0f;
                // 循环：从 from 重新开始
            } else {
                tw.running = false;
                // 确保最终值精确到达目标
                ApplyProperty(entity, tw.property, tw.to);
            }
        }
    }
}

void TweenSystem::AddTween(ECS::Entity entity,
                           ECS::TweenComponent::Property property,
                           float from, float to, float duration,
                           ECS::TweenComponent::Easing easing,
                           float delay, bool loop, bool pingPong) {
    auto& coordinator = ECS::Coordinator::GetInstance();
    ECS::TweenComponent tw;
    tw.property = property;
    tw.from = from;
    tw.to = to;
    tw.duration = duration;
    tw.easing = easing;
    tw.delay = delay;
    tw.loop = loop;
    tw.pingPong = pingPong;
    tw.elapsed = 0.0f;
    tw.running = true;
    tw.pingPongReverse = false;
    coordinator.AddComponent<ECS::TweenComponent>(entity, std::move(tw));
}

} // namespace UI
