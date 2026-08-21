#pragma once
// SpriteAnimatorSystem.h - 2D 精灵帧动画系统(序2)
// 每帧推进带 SpriteAnimationComponent 实体的帧号, 写回同实体 Sprite2DComponent.uv0/uv1。
// 渲染入口 Canvas2D::RenderSpriteEntityAt 读 uv0/uv1 画精灵, 渲染器无需改动。
// 支持规则网格(cols×rows)与显式帧矩形表(frames, 纹理像素坐标)。
#include "Platform/Export.h"
#include "ECS/Types.h"

namespace ECS { struct SpriteAnimationComponent; struct Sprite2DComponent; }

class MIKAN_API SpriteAnimatorSystem {
public:
    static SpriteAnimatorSystem& GetInstance();

    // 每帧调用(EngineMain 主循环, TweenSystem 附近)
    void Update(float dt);

private:
    SpriteAnimatorSystem() = default;
    ~SpriteAnimatorSystem() = default;
    SpriteAnimatorSystem(const SpriteAnimatorSystem&) = delete;
    SpriteAnimatorSystem& operator=(const SpriteAnimatorSystem&) = delete;

    void VisitEntity(ECS::Entity e, float dt);
    void UpdateAnimation(ECS::SpriteAnimationComponent& anim, ECS::Sprite2DComponent& spr, float dt);
};
