#pragma once
// TweenSystem.h - UI 补间动画系统：对 ECS 实体属性做时间插值动画
// 依赖 TweenComponent（ECS/Components.h），每帧由 Update 驱动。
#include "Platform/Export.h"
#include "ECS/ECS.h"
#include "ECS/Components.h"
#include <vector>

namespace UI {

class MIKAN_API TweenSystem {
public:
    static TweenSystem& GetInstance();

    // 每帧更新：遍历所有 TweenComponent，推进 elapsed、插值并写入目标属性
    void Update(float deltaTime);

    // 便捷：为一个实体添加补间动画
    // property: 要动画的属性
    // from/to: 起始/目标值
    // duration: 持续时间（秒）
    // easing: 缓动类型（默认 EaseOutQuad）
    // delay: 延迟（秒）
    // loop/pingPong: 循环模式
    static void AddTween(ECS::Entity entity,
                         ECS::TweenComponent::Property property,
                         float from, float to, float duration,
                         ECS::TweenComponent::Easing easing = ECS::TweenComponent::Easing::EaseOutQuad,
                         float delay = 0.0f, bool loop = false, bool pingPong = false);

private:
    TweenSystem() = default;
    TweenSystem(const TweenSystem&) = delete;
    TweenSystem& operator=(const TweenSystem&) = delete;

    // 缓动函数：t ∈ [0,1] → [0,1]
    static float ApplyEasing(float t, ECS::TweenComponent::Easing easing);

    // 将插值结果写入实体属性
    static void ApplyProperty(ECS::Entity entity, ECS::TweenComponent::Property property, float value);
};

} // namespace UI
