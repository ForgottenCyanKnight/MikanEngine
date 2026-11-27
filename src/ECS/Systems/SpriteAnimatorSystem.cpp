// SpriteAnimatorSystem.cpp - 2D 精灵帧动画系统(序2)
#include "ECS/Systems/SpriteAnimatorSystem.h"
#include "ECS/SceneECS.h"
#include "ECS/Coordinator.h"
#include "ECS/Components.h"
#include <algorithm>
#include <cmath>

namespace {
// 推进帧号并返回当前帧索引; 非循环播完停在末帧
int AdvanceFrame(int total, float dt, float fps, bool loop, float& elapsed) {
    if (total <= 1) return 0;
    elapsed += dt;
    const float frames = elapsed * std::max(fps, 0.0f);
    const int f = static_cast<int>(frames);
    if (loop) {
        return f % total;
    }
    return std::min(f, total - 1); // 播完停末帧(elapsed 持续增长, 帧号钳制)
}
} // namespace

SpriteAnimatorSystem& SpriteAnimatorSystem::GetInstance() {
    static SpriteAnimatorSystem instance;
    return instance;
}

void SpriteAnimatorSystem::Update(float dt) {
    auto& sceneECS = ECS::SceneECS::GetInstance();
    for (const auto& root : sceneECS.GetRootEntities()) {
        VisitEntity(root, dt);
    }
}

void SpriteAnimatorSystem::VisitEntity(ECS::Entity e, float dt) {
    auto& coordinator = ECS::Coordinator::GetInstance();
    if (coordinator.HasComponent<ECS::SpriteAnimationComponent>(e) &&
        coordinator.HasComponent<ECS::Sprite2DComponent>(e)) {
        auto& anim = coordinator.GetComponent<ECS::SpriteAnimationComponent>(e);
        auto& spr = coordinator.GetComponent<ECS::Sprite2DComponent>(e);
        UpdateAnimation(anim, spr, dt);
    }
    auto& sceneECS = ECS::SceneECS::GetInstance();
    for (const auto& child : sceneECS.GetChildren(e)) {
        VisitEntity(child, dt);
    }
}

void SpriteAnimatorSystem::UpdateAnimation(ECS::SpriteAnimationComponent& anim,
                                           ECS::Sprite2DComponent& spr, float dt) {
    if (anim.playing) {
        int total = static_cast<int>(anim.frames.size());
        if (anim.frames.empty()) {
            total = std::max(anim.frameCols, 1) * std::max(anim.frameRows, 1);
            if (anim.frameCount > 0) total = std::min(anim.frameCount, total);
        }
        anim.currentFrame = AdvanceFrame(total, dt, anim.fps, anim.loop, anim.elapsed);
    }
    ApplyFrame(anim, spr, anim.currentFrame);
}

void SpriteAnimatorSystem::ApplyFrame(const ECS::SpriteAnimationComponent& anim,
                                      ECS::Sprite2DComponent& spr, int frame) {
    // 帧表模式(显式矩形)优先于规则网格
    if (!anim.frames.empty()) {
        const int total = static_cast<int>(anim.frames.size());
        const int idx = std::clamp(frame, 0, total - 1);
        // frames 每项 = {x, y, w, h} 存于 glm::ivec4 的 {x, y, z, w}
        const glm::ivec4& r = anim.frames[idx];
        const float tw = anim.texWidth > 0 ? static_cast<float>(anim.texWidth) : 1.0f;
        const float th = anim.texHeight > 0 ? static_cast<float>(anim.texHeight) : 1.0f;
        // TexturePool uploads the image with its Y axis flipped. Renderer2D
        // samples uv1.y at the top of the quad and uv0.y at the bottom.
        spr.uv0 = { r.x / tw, 1.0f - static_cast<float>(r.y + r.w) / th };
        spr.uv1 = { (r.x + r.z) / tw, 1.0f - static_cast<float>(r.y) / th };
        return;
    }

    // 规则网格模式
    const int cols = std::max(anim.frameCols, 1);
    const int rows = std::max(anim.frameRows, 1);
    int total = cols * rows;
    if (anim.frameCount > 0) total = std::min(anim.frameCount, total);
    const int idx = std::clamp(frame, 0, total - 1);
    const int col = idx % cols;
    const int row = idx / cols;
    const float u0 = static_cast<float>(col) / cols;
    const float u1 = static_cast<float>(col + 1) / cols;
    // 纹理 y 翻转(见上): 原图第 row 行(顶部) → v = 1 - row/rows
    spr.uv0 = { u0, 1.0f - static_cast<float>(row + 1) / rows };
    spr.uv1 = { u1, 1.0f - static_cast<float>(row) / rows };
}
